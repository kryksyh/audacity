/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QTimer>

#include <vector>

#include "framework/global/api/apiobject.h"
#include "framework/global/modularity/ioc.h"
#include "framework/actions/iactionsdispatcher.h"
#include "framework/ui/imainwindow.h"

#include "../internal/perftracecontroller.h"

class QQuickWindow;

namespace au::appshell::api {
//! Scripting hooks for reproducible UI performance runs (testflow)
class PerfApi : public muse::api::ApiObject
{
    Q_OBJECT

    muse::ContextInject<muse::actions::IActionsDispatcher> dispatcher = { this };
    muse::ContextInject<muse::ui::IMainWindow> mainWindow = { this };

public:
    explicit PerfApi(muse::api::IApiEngine* e);

    Q_INVOKABLE QString env(const QString& name) const;
    Q_INVOKABLE void openProject(const QString& path);

    Q_INVOKABLE void startTrace();
    Q_INVOKABLE bool stopTrace(const QString& path);

    //! Plays a fixed sequence of trackpad pinch-zoom and scroll gestures over the tracks view
    Q_INVOKABLE void startTimelineBenchmark();
    Q_INVOKABLE bool isBenchmarkRunning() const;

private:
    void benchmarkStep();
    void sendPinch(Qt::NativeGestureType type, double value);
    void sendScroll(int dx);
    QQuickWindow* window() const;

    PerfTraceController m_trace;
    QTimer m_timer;
    std::vector<size_t> m_phases;
    size_t m_phase = 0;
    int m_phaseStep = 0;
    int64_t m_phaseStartNs = 0;
};
}
