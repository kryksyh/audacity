/*
 * Audacity: A Digital Audio Editor
 */
#include "perftracecontroller.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QQuickWindow>
#include <QThread>

#include <utility>

#include "framework/global/log.h"

#include "shared/perf/perftrace.h"

using namespace au::appshell;
using namespace au::perf;

namespace {
template<typename BeginSignal, typename EndSignal>
void traceBetween(QList<QMetaObject::Connection>& connections, QQuickWindow* window, BeginSignal begin, EndSignal end,
                  int64_t& startNs, const char* name, Category category)
{
    connections << QObject::connect(window, begin, window, [&startNs]() {
        startNs = Tracer::nowNs();
    }, Qt::DirectConnection);

    connections << QObject::connect(window, end, window, [&startNs, name, category]() {
        if (startNs >= 0 && Tracer::instance().isEnabled()) {
            Tracer::instance().recordZone(name, category, startNs, Tracer::nowNs());
        }
        startNs = -1;
    }, Qt::DirectConnection);
}
}

bool PerfTraceController::isRunning() const
{
    return !m_connections.isEmpty();
}

void PerfTraceController::start(QQuickWindow* window)
{
    if (isRunning()) {
        return;
    }

    Tracer& tracer = Tracer::instance();
    tracer.clear();
    tracer.setThreadName("GUI");

    // Started from a menu handler, so the GUI thread is already busy
    m_guiAwakeNs = Tracer::nowNs();
    m_frameBeginNs = m_syncBeginNs = m_renderBeginNs = m_passBeginNs = m_lastSwapNs = -1;

    // Busy time between wake-ups; instrumented zones nest inside, the remainder is QML/Qt
    QAbstractEventDispatcher* eventDispatcher = QAbstractEventDispatcher::instance(qApp->thread());
    m_connections << QObject::connect(eventDispatcher, &QAbstractEventDispatcher::awake, [this]() {
        if (m_guiAwakeNs < 0) {
            m_guiAwakeNs = Tracer::nowNs();
        }
    });
    m_connections << QObject::connect(eventDispatcher, &QAbstractEventDispatcher::aboutToBlock, [this]() {
        if (m_guiAwakeNs >= 0 && Tracer::instance().isEnabled()) {
            Tracer::instance().recordZone("GUI busy", Category::Qml, m_guiAwakeNs, Tracer::nowNs());
            Tracer::instance().flushCallCounters();
        }
        m_guiAwakeNs = -1;
    });

    if (window) {
        m_connections << QObject::connect(window, &QQuickWindow::beforeFrameBegin, window, []() {
            // With the basic render loop these signals arrive on the GUI thread
            if (QThread::currentThread() != qApp->thread()) {
                Tracer::instance().setThreadName("Qt render");
            }
        }, Qt::DirectConnection);

        traceBetween(m_connections, window, &QQuickWindow::beforeFrameBegin, &QQuickWindow::afterFrameEnd, m_frameBeginNs, "SG frame",
                     Category::Frame);
        // QQuickPaintedItem::paint runs here, with the GUI thread blocked
        traceBetween(m_connections, window, &QQuickWindow::beforeSynchronizing, &QQuickWindow::afterSynchronizing, m_syncBeginNs, "SG sync",
                     Category::Paint);
        traceBetween(m_connections, window, &QQuickWindow::beforeRendering, &QQuickWindow::afterRendering, m_renderBeginNs, "SG render",
                     Category::Render);
        traceBetween(m_connections, window, &QQuickWindow::beforeRenderPassRecording, &QQuickWindow::afterRenderPassRecording,
                     m_passBeginNs, "SG pass", Category::Render);

        m_connections << QObject::connect(window, &QQuickWindow::frameSwapped, window, [this]() {
            const int64_t now = Tracer::nowNs();
            if (m_lastSwapNs >= 0) {
                AU_PERF_COUNTER("frame interval ms", static_cast<double>(now - m_lastSwapNs) / 1e6);
            }
            m_lastSwapNs = now;
        }, Qt::DirectConnection);
    } else {
        LOGW() << "No QQuickWindow, tracing GUI thread only";
    }

    tracer.setEnabled(true);
    LOGI() << "Performance trace started";
}

bool PerfTraceController::stopAndSave(const muse::io::path_t& filePath)
{
    if (!isRunning()) {
        return false;
    }

    Tracer& tracer = Tracer::instance();
    tracer.setEnabled(false);
    for (const QMetaObject::Connection& connection : std::as_const(m_connections)) {
        QObject::disconnect(connection);
    }
    m_connections.clear();

    const QString path = filePath.toQString();
    QFileInfo(path).dir().mkpath(".");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        LOGE() << "Failed to write performance trace: " << path;
        return false;
    }
    file.write(QByteArray::fromStdString(tracer.toChromeTraceJson()));

    LOGI() << "Performance trace saved: " << path;
    return true;
}
