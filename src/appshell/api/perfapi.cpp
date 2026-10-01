/*
 * Audacity: A Digital Audio Editor
 */
#include "perfapi.h"

#include <QCoreApplication>
#include <QFile>
#include <QQuickWindow>
#include <QUrl>
#include <QWheelEvent>

#include <vector>

#include "shared/perf/perftrace.h"

using namespace au::appshell::api;

namespace {
enum class Gesture {
    Idle,
    Pinch,
    Scroll,
};

struct Phase {
    const char* name;
    Gesture gesture;
    int steps;
    double value; // pinch: scale delta per step; scroll: pixels per step
};

// One step per timer tick, roughly the rate a trackpad delivers events
constexpr int STEP_INTERVAL_MS = 8;

// The project opens zoomed to fit, so zoom in first to have content to scroll through
const std::vector<Phase> PHASES = {
    { "bench: idle", Gesture::Idle, 30, 0.0 },
    { "bench: pinch in", Gesture::Pinch, 120, 0.02 },
    { "bench: scroll left", Gesture::Scroll, 250, 25.0 },
    { "bench: scroll right", Gesture::Scroll, 250, -25.0 },
    { "bench: pinch out", Gesture::Pinch, 120, -0.02 },
    { "bench: pinch in fast", Gesture::Pinch, 50, 0.06 },
    { "bench: scroll left fast", Gesture::Scroll, 120, 80.0 },
    { "bench: scroll right fast", Gesture::Scroll, 120, -80.0 },
    { "bench: pinch out fast", Gesture::Pinch, 50, -0.06 },
    { "bench: idle", Gesture::Idle, 30, 0.0 },
};

QPointF gesturePosition(const QWindow* window)
{
    // Inside the tracks view, right of the track panels
    return QPointF(window->width() * 0.6, window->height() * 0.55);
}
}

PerfApi::PerfApi(muse::api::IApiEngine* e)
    : ApiObject(e)
{
    m_timer.setInterval(STEP_INTERVAL_MS);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this]() { benchmarkStep(); });
}

QString PerfApi::env(const QString& name) const
{
    return qEnvironmentVariable(name.toUtf8().constData());
}

void PerfApi::openProject(const QString& path)
{
    dispatcher()->dispatch("file-open", muse::actions::ActionData::make_arg1<QUrl>(QUrl::fromLocalFile(path)));
}

void PerfApi::startTrace()
{
    m_trace.start(window());
}

bool PerfApi::stopTrace(const QString& path)
{
    return m_trace.stopAndSave(path);
}

void PerfApi::startTimelineBenchmark()
{
    // AU_BENCH_PHASES=<substring> runs only the phases whose name contains it (idle phases always run)
    const QString filter = qEnvironmentVariable("AU_BENCH_PHASES");
    m_phases.clear();
    for (size_t i = 0; i < PHASES.size(); ++i) {
        if (filter.isEmpty() || PHASES[i].gesture == Gesture::Idle || QString(PHASES[i].name).contains(filter)) {
            m_phases.push_back(i);
        }
    }

    m_phase = 0;
    m_phaseStep = 0;
    m_phaseStartNs = perf::Tracer::nowNs();
    m_timer.start();

    // Lets an external sampler (e.g. macOS `sample`) start exactly when the gestures do
    const QString marker = qEnvironmentVariable("AU_BENCH_MARKER");
    if (!marker.isEmpty()) {
        QFile(marker).open(QIODevice::WriteOnly);
    }
}

bool PerfApi::isBenchmarkRunning() const
{
    return m_timer.isActive();
}

void PerfApi::benchmarkStep()
{
    const Phase& phase = PHASES[m_phases[m_phase]];

    if (phase.gesture == Gesture::Pinch) {
        if (m_phaseStep == 0) {
            sendPinch(Qt::BeginNativeGesture, 0.0);
        }
        sendPinch(Qt::ZoomNativeGesture, phase.value);
    } else if (phase.gesture == Gesture::Scroll) {
        sendScroll(static_cast<int>(phase.value));
    }

    if (++m_phaseStep < phase.steps) {
        return;
    }

    if (phase.gesture == Gesture::Pinch) {
        sendPinch(Qt::EndNativeGesture, 0.0);
    }

    const int64_t now = perf::Tracer::nowNs();
    perf::Tracer::instance().recordZone(phase.name, perf::Category::Frame, m_phaseStartNs, now);

    m_phaseStep = 0;
    m_phaseStartNs = now;
    if (++m_phase == m_phases.size()) {
        m_timer.stop();
    }
}

QQuickWindow* PerfApi::window() const
{
    return qobject_cast<QQuickWindow*>(mainWindow()->qWindow());
}

void PerfApi::sendPinch(Qt::NativeGestureType type, double value)
{
    QQuickWindow* w = window();
    if (!w) {
        return;
    }

    const QPointF pos = gesturePosition(w);
    QNativeGestureEvent event(type, QPointingDevice::primaryPointingDevice(), 2, pos, pos, w->mapToGlobal(pos), value, QPointF());
    QCoreApplication::sendEvent(w, &event);
}

void PerfApi::sendScroll(int dx)
{
    QQuickWindow* w = window();
    if (!w) {
        return;
    }

    const QPointF pos = gesturePosition(w);
    QWheelEvent event(pos, w->mapToGlobal(pos), QPoint(dx, 0), QPoint(dx, 0), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase,
                      false);
    QCoreApplication::sendEvent(w, &event);
}
