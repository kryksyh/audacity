/*
 * Audacity: A Digital Audio Editor
 */
#include "perfapi.h"

#include <QCoreApplication>
#include <QFile>
#include <QQuickWindow>
#include <QUrl>
#include <QWheelEvent>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "framework/global/log.h"

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

bool PerfApi::configureAudio(double bufferMs, const QString& outputDevice, const QString& inputDevice,
                             int autoLatencyCompensation)
{
    // An unknown name would fall back to the system default device without an
    // error, and the run would measure the wrong device
    const auto known = [](const std::vector<std::string>& names, const QString& name) {
        return std::find(names.begin(), names.end(), name.toStdString()) != names.end();
    };
    if (!outputDevice.isEmpty() && !known(audioDriverController()->outputDevices(), outputDevice)) {
        LOGE() << "unknown output device \"" << outputDevice << "\"";
        return false;
    }
    if (!inputDevice.isEmpty() && !known(audioDriverController()->inputDevices(), inputDevice)) {
        LOGE() << "unknown input device \"" << inputDevice << "\"";
        return false;
    }

    au::audio::AudioConfigurationChange change;
    if (bufferMs >= 0) {
        change.bufferLength = bufferMs;
    }
    if (!outputDevice.isEmpty()) {
        change.outputDevice = outputDevice.toStdString();
    }
    if (!inputDevice.isEmpty()) {
        change.inputDevice = inputDevice.toStdString();
    }
    if (autoLatencyCompensation >= 0) {
        change.automaticLatencyCompensation = autoLatencyCompensation > 0;
    }
    return audioDriverController()->apply(iocContext(), change).succeeded();
}

bool PerfApi::setLatencyCompensation(double ms)
{
    au::audio::AudioConfigurationChange change;
    change.automaticLatencyCompensation = false;
    change.latencyCompensation = ms;
    return audioDriverController()->apply(iocContext(), change).succeeded();
}

QString PerfApi::audioConfiguration() const
{
    const au::audio::AudioConfiguration c = audioDriverController()->configuration();
    return QString("api=%1; output=%2; input=%3; bufferMs=%4; autoLatencyCompensation=%5; latencyCompensationMs=%6; rate=%7")
           .arg(QString::fromStdString(c.api))
           .arg(QString::fromStdString(c.outputDevice.value_or("<default>")))
           .arg(QString::fromStdString(c.inputDevice.value_or("<default>")))
           .arg(c.bufferLength)
           .arg(c.automaticLatencyCompensation ? "true" : "false")
           .arg(c.latencyCompensation)
           .arg(c.defaultSampleRate);
}

namespace {
// trackIndex < 0 means the master track
std::optional<au::trackedit::TrackId> trackAt(const au::context::IGlobalContext& context, int trackIndex)
{
    if (trackIndex < 0) {
        return au::effects::IRealtimeEffectService::masterTrackId;
    }
    const auto project = context.currentTrackeditProject();
    if (!project) {
        return std::nullopt;
    }
    const std::vector<au::trackedit::TrackId> tracks = project->trackIdList();
    if (trackIndex >= static_cast<int>(tracks.size())) {
        return std::nullopt;
    }
    return tracks[trackIndex];
}
}

QVariantMap PerfApi::audioEngineHealth() const
{
    const au::audio::AudioEngineDiagnostics d = audioEngineDiagnostics()->diagnostics();
    return {
        { "framesPerBuffer", static_cast<qulonglong>(d.framesPerBuffer) },
        { "averageLoad", d.averageLoad },
        { "peakLoad", d.peakLoad },
        { "callbacks", static_cast<qulonglong>(d.callbacks) },
        { "dropouts", static_cast<qulonglong>(d.dropouts) },
        { "overBudgetCallbacks", static_cast<qulonglong>(d.overBudgetCallbacks) },
        { "outputUnderflows", static_cast<qulonglong>(d.outputUnderflows) },
        { "inputOverflows", static_cast<qulonglong>(d.inputOverflows) },
        { "playbackStarvations", static_cast<qulonglong>(d.playbackStarvations) },
        { "lostCaptureFrames", static_cast<qulonglong>(d.lostCaptureFrames) },
        { "streamRoundTripMs", d.streamRoundTripMs },
        { "recordingCompensationMs", d.recordingCompensationMs },
        { "recordingCompensationSource", static_cast<int>(d.recordingCompensationSource) },
    };
}

void PerfApi::startLatencyMeasurement()
{
    m_latencyMeasurement.clear();
    latencyMeasurementService()->measure(iocContext(), [this](const au::audio::LatencyMeasurement& m) {
        m_latencyMeasurement = {
            { "status", static_cast<int>(m.status) },
            { "sampleRate", m.sampleRate },
            { "roundTripFrames", static_cast<qlonglong>(m.roundTripFrames) },
            { "roundTripMs", m.roundTripMs() },
            { "reportedInputLatencyMs", m.reportedInputLatencyMs },
            { "reportedOutputLatencyMs", m.reportedOutputLatencyMs },
            { "signalsFound", static_cast<qulonglong>(m.signalsFound) },
            { "signalsSent", static_cast<qulonglong>(m.signalsSent) },
            { "spreadFrames", static_cast<qlonglong>(m.spreadFrames) },
            { "inputPeak", m.inputPeak },
            { "inverted", m.inverted },
        };
    });
}

QVariantMap PerfApi::latencyMeasurement() const
{
    return m_latencyMeasurement;
}

bool PerfApi::addRealtimeEffect(int trackIndex, const QString& effectId)
{
    const auto track = trackAt(*globalContext(), trackIndex);
    return track && realtimeEffectService()->addRealtimeEffect(*track, muse::String::fromQString(effectId)) != nullptr;
}

bool PerfApi::replaceRealtimeEffect(int trackIndex, int effectIndex, const QString& effectId)
{
    const auto track = trackAt(*globalContext(), trackIndex);
    return track && realtimeEffectService()->replaceRealtimeEffect(*track, effectIndex, muse::String::fromQString(effectId)) != nullptr;
}

int PerfApi::removeRealtimeEffects(int trackIndex)
{
    const auto track = trackAt(*globalContext(), trackIndex);
    const auto stack = track ? realtimeEffectService()->effectStack(*track) : std::nullopt;
    if (!stack) {
        return 0;
    }
    for (const auto& state : *stack) {
        realtimeEffectService()->removeRealtimeEffect(*track, state);
    }
    return static_cast<int>(stack->size());
}

bool PerfApi::exportTracks(const QString& directory)
{
    using au::importexport::IExporter;

    std::string format;
    for (const std::string& f : exporter()->formatsList()) {
        if (QString::fromStdString(f).startsWith("WAV")) {
            format = f;
            break;
        }
    }
    if (format.empty()) {
        return false;
    }

    IExporter::Options options;
    options[IExporter::OptionKey::Format] = muse::Val(format);
    options[IExporter::OptionKey::ProcessType] = muse::Val(au::importexport::ExportProcessType::TRACKS_AS_SEPARATE_AUDIO_FILES);
    options[IExporter::OptionKey::ExportSampleRate] = muse::Val(48000);
    options[IExporter::OptionKey::FileNamePrefix] = muse::Val(std::string("track"));
    options[IExporter::OptionKey::IncludeNumbers] = muse::Val(true);

    if (!exporter()->prepareSeparateFiles(options)) {
        return false;
    }
    return exporter()->exportSeparateFiles(muse::io::path_t(directory)).success();
}

void PerfApi::mark(const QString& name)
{
    // Trace events keep the name pointer, so names must live for the whole run
    static std::set<std::string> names;
    const char* interned = names.insert(name.toStdString()).first->c_str();
    const int64_t now = au::perf::Tracer::nowNs();
    au::perf::Tracer::instance().recordZone(interned, au::perf::Category::Backend, now, now);
}

void PerfApi::importAudio(const QString& path)
{
    if (!globalContext()->currentProject()) {
        dispatcher()->dispatch("file-new");
    }
    dispatcher()->dispatch("project-import", muse::actions::ActionData::make_arg1<QStringList>({ path }));
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
