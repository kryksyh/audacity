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

#include "audio/driver/iaudiodrivercontroller.h"
#include "audio/iaudioenginediagnostics.h"
#include "audio/driver/ilatencymeasurement.h"
#include "importexport/export/iexporter.h"
#include "context/iglobalcontext.h"
#include "effects/effects_base/irealtimeeffectservice.h"

#include "../internal/perftracecontroller.h"

class QQuickWindow;

namespace au::appshell::api {
//! Scripting hooks for reproducible UI performance runs (testflow)
class PerfApi : public muse::api::ApiObject
{
    Q_OBJECT

    muse::ContextInject<muse::actions::IActionsDispatcher> dispatcher = { this };
    muse::ContextInject<muse::ui::IMainWindow> mainWindow = { this };
    muse::GlobalInject<au::audio::IAudioDriverController> audioDriverController;
    muse::GlobalInject<au::audio::IAudioEngineDiagnostics> audioEngineDiagnostics;
    muse::GlobalInject<au::audio::ILatencyMeasurement> latencyMeasurementService;
    muse::ContextInject<au::importexport::IExporter> exporter = { this };
    muse::ContextInject<au::context::IGlobalContext> globalContext = { this };
    muse::ContextInject<au::effects::IRealtimeEffectService> realtimeEffectService = { this };

public:
    explicit PerfApi(muse::api::IApiEngine* e);

    Q_INVOKABLE QString env(const QString& name) const;
    Q_INVOKABLE void openProject(const QString& path);
    //! Into the project of this window; file-open would put an audio file into a new window
    Q_INVOKABLE void importAudio(const QString& path);

    //! Applied like the preferences page does. bufferMs < 0, empty device names and
    //! autoLatencyCompensation < 0 keep the current values
    Q_INVOKABLE bool configureAudio(double bufferMs, const QString& outputDevice, const QString& inputDevice,
                                    int autoLatencyCompensation = -1);
    Q_INVOKABLE QString audioConfiguration() const;
    Q_INVOKABLE QVariantMap audioEngineHealth() const;
    Q_INVOKABLE void startLatencyMeasurement();
    //! Empty until the measurement started by startLatencyMeasurement() is done
    Q_INVOKABLE QVariantMap latencyMeasurement() const;
    //! Appends a realtime effect to the track at `trackIndex` of the current project
    Q_INVOKABLE bool addRealtimeEffect(int trackIndex, const QString& effectId);
    //! Each track as a 48 kHz WAV file in `directory`
    Q_INVOKABLE bool exportTracks(const QString& directory);

    Q_INVOKABLE void startTrace();
    //! Zero-length zone named `name` on the calling thread
    Q_INVOKABLE void mark(const QString& name);
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
    QVariantMap m_latencyMeasurement;
    QTimer m_timer;
    std::vector<size_t> m_phases;
    size_t m_phase = 0;
    int m_phaseStep = 0;
    int64_t m_phaseStartNs = 0;
};
}
