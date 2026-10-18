/*
* Audacity: A Digital Audio Editor
*/
#include "instrumentprocessor.h"
#include "au3-midi-track/MidiController.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <stdexcept>

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QWindow>

#include "au3-vst3/VST3EffectBase.h"
#include "au3-vst3/VST3EffectsModule.h"
#include "au3-vst3/VST3Utils.h"
#include "au3-vst3/VST3Wrapper.h"
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <public.sdk/source/vst/hosting/eventlist.h>
#include <public.sdk/source/vst/hosting/parameterchanges.h>

namespace au::au3audio {
namespace {
using namespace Steinberg;

FIDString platformViewType()
{
#if defined(Q_OS_MACOS)
    return kPlatformTypeNSView;
#elif defined(Q_OS_WIN)
    return kPlatformTypeHWND;
#else
    return kPlatformTypeX11EmbedWindowID;
#endif
}

class VstInstrument final : public InstrumentProcessor, public IPlugFrame
{
public:
    VstInstrument(const std::string& path, const std::string& state)
    {
        VST3EffectsModule provider;
        const auto component = provider.LoadPlugin(wxString::FromUTF8(path));
        const auto* effect = dynamic_cast<const VST3EffectBase*>(component.get());
        if (!effect) {
            throw std::runtime_error("Cannot load the VST3 instrument.");
        }
        wxString modulePath;
        std::string uid;
        if (!VST3Utils::ParsePluginPath(wxString::FromUTF8(path), &modulePath, &uid)) {
            throw std::runtime_error("Invalid VST3 instrument path.");
        }
        m_module = effect->vstModule();
        for (const auto& info : m_module->getFactory().classInfos()) {
            if (info.ID().toString() == uid) {
                m_class = std::make_unique<VST3::Hosting::ClassInfo>(info);
                break;
            }
        }
        if (!m_class) {
            throw std::runtime_error("The VST3 instrument class is not available.");
        }
        m_wrapper = std::make_unique<VST3Wrapper>(*m_module, *m_class, VST3Wrapper::Role::Instrument);
        m_wrapper->InitializeComponents();
        m_settings = VST3Wrapper::MakeSettings();
        const auto saved = QJsonDocument::fromJson(QByteArray::fromStdString(state)).object();
        const auto nativeState = saved.contains("vst3State") ? saved.value("vst3State").toString().toStdString() : state;
        const auto savedParameters = saved.value("parameters").toObject();
        if (!nativeState.empty()) {
            VST3Wrapper::LoadSettings(CommandParameters(wxString::FromUTF8(nativeState)), m_settings);
            m_wrapper->FetchSettings(m_settings, false);
        }
        auto controller = m_wrapper->mEditController;
        const auto midiEvents = VST3Utils::MidiEventParameters(controller);
        m_parameterCount = controller->getParameterCount();
        m_values = std::make_unique<std::atomic<double>[]>(m_parameterCount);
        m_processingValues = std::make_unique<std::atomic<double>[]>(m_parameterCount);
        m_dirty = std::make_unique<std::atomic<bool>[]>(m_parameterCount);
        for (int32 index = 0; index < m_parameterCount; ++index) {
            Vst::ParameterInfo info {};
            controller->getParameterInfo(index, info);
            m_manualParameters.push_back(!(info.flags & (Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden))
                                         && !midiEvents.contains(info.id));
            m_programParameters.push_back((info.flags & Vst::ParameterInfo::kIsProgramChange)
                                          && !(info.flags & Vst::ParameterInfo::kIsReadOnly));
            m_ids.push_back(info.id);
            m_idStrings.push_back(std::to_string(info.id));
            m_values[index] = controller->getParamNormalized(info.id);
            const auto savedValue = savedParameters.value(QString::number(info.id));
            if (savedParameter(index) && savedValue.isDouble() && savedValue.toDouble() >= 0.0 && savedValue.toDouble() <= 1.0) {
                m_values[index] = savedValue.toDouble();
                controller->setParamNormalized(info.id, savedValue.toDouble());
            }
            m_processingValues[index] = m_values[index].load();
            m_dirty[index] = savedParameter(index);
        }
        m_wrapper->ParamChangedHandler = [this](Vst::ParamID id) {
            const auto index = parameterIndex(id);
            if (index >= 0 && savedParameter(index)) {
                m_values[index] = m_wrapper->mEditController->getParamNormalized(id);
                m_processingValues[index] = m_values[index].load();
                m_dirty[index] = true;
                m_changed = true;
            }
        };
        m_midiMapping = FUnknownPtr<Vst::IMidiMapping>(controller);
        for (int16 channel = 0; channel < 16; ++channel) {
            for (int control = 0; control < 130; ++control) {
                Vst::ParamID parameter = Vst::kNoParamId;
                if (m_midiMapping && m_midiMapping->getMidiControllerAssignment(0, channel, control, parameter) != kResultTrue) {
                    parameter = Vst::kNoParamId;
                }
                m_controllerMapping[channel][control] = parameter;
                m_controllerMappingIndex[channel][control] = parameterIndex(parameter);
            }
        }
        cacheProgramMapping();
        m_parameters.setMaxParameters(m_parameterCount);
        m_midiProgramOverrides.resize(m_parameterCount);
        m_lastParameters.resize(m_parameterCount);
        m_parameterReplay.reserve(m_parameterCount);
        m_noteReplay.reserve(16 * 128);
        m_events.setMaxSize(8192 + 3 * 16 * 128);
    }

    ~VstInstrument() override
    {
        detachView();
        m_wrapper->ParamChangedHandler = nullptr;
        if (m_wrapper->IsActive()) {
            m_wrapper->Finalize(nullptr);
        }
    }

    std::vector<audio::InstrumentParameter> parameters() const override
    {
        std::vector<audio::InstrumentParameter> result;
        for (int32 index = 0; index < m_parameterCount; ++index) {
            Vst::ParameterInfo info {};
            m_wrapper->mEditController->getParameterInfo(index, info);
            if (!m_manualParameters[index]) {
                continue;
            }
            Vst::String128 display {};
            const auto value = m_values[index].load();
            m_wrapper->mEditController->getParamStringByValue(info.id, value, display);
            result.push_back({ std::to_string(info.id), VST3Utils::UTF16ToStdString(info.title),
                               VST3Utils::UTF16ToStdString(display), value });
        }
        return result;
    }

    bool setParameter(const std::string& id, double value) override
    {
        if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
            return false;
        }
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (m_manualParameters[index] && id == m_idStrings[index]) {
                m_values[index] = value;
                m_processingValues[index] = value;
                m_dirty[index] = true;
                m_changed = true;
                m_wrapper->mEditController->setParamNormalized(m_ids[index], value);
                return true;
            }
        }
        return false;
    }

    bool setAutomatedParameter(const std::string& id, double value) override
    {
        if (!std::isfinite(value)) {
            return false;
        }
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (m_manualParameters[index] && id == m_idStrings[index]) {
                m_processingValues[index] = std::clamp(value, 0.0, 1.0);
                m_dirty[index] = true;
                return true;
            }
        }
        return false;
    }

    std::string state() const override
    {
        m_wrapper->StoreSettings(m_settings);
        CommandParameters parameters;
        VST3Wrapper::SaveSettings(m_settings, parameters);
        wxString state;
        parameters.GetParameters(state);
        QJsonObject values;
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (savedParameter(index)) {
                values.insert(QString::number(m_ids[index]), m_values[index].load());
            }
        }
        return QJsonDocument(QJsonObject { { "vst3State", QString::fromStdString(state.utf8_string()) },
                                 { "parameters", values } }).toJson(QJsonDocument::Compact).toStdString();
    }

    bool takeChanges() override { return m_changed.exchange(false); }
    void syncLiveMidiControllers(std::span<const audio::InstrumentMidiMessage> messages) override
    {
        bool updated = false;
        for (const auto& message : messages) {
            if (message.status < 0x80 || message.status >= 0xf0 || message.data1 < 0 || message.data1 > 127
                || message.data2 < 0 || message.data2 > 127) {
                continue;
            }
            const auto type = message.status & 0xf0;
            if (type != 0xb0 && type != 0xd0 && type != 0xe0) {
                continue;
            }
            const int control = type == 0xe0 ? Vst::kPitchBend : type == 0xd0 ? Vst::kAfterTouch : message.data1;
            const auto parameter = m_controllerMapping[message.status & 0xf][control];
            if (parameter == Vst::kNoParamId) {
                continue;
            }
            const double value = type == 0xe0 ? (message.data1 + 128 * message.data2) / 16383.0
                                 : type == 0xd0 ? message.data1 / 127.0 : message.data2 / 127.0;
            // Hidden MIDI helper parameters can update several visible knobs
            // without a performEdit/endEdit callback. Preserve every message:
            // repeated values may represent relative encoder increments.
            m_wrapper->mEditController->setParamNormalized(parameter, value);
            updated = true;
        }
        if (!updated) {
            return;
        }
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (!m_manualParameters[index]) {
                continue;
            }
            const auto value = m_wrapper->mEditController->getParamNormalized(m_ids[index]);
            if (!std::isfinite(value) || value < 0.0 || value > 1.0 || value == m_values[index].load()) {
                continue;
            }
            m_values[index] = value;
            m_processingValues[index] = value;
            m_dirty[index] = true;
            m_changed = true;
        }
    }

    void restoreManualParameters() noexcept override
    {
        for (int32 i = 0; i < m_parameterCount; ++i) {
            if (!savedParameter(i)) {
                continue;
            }
            // CC restoration and transport handoffs must not reload a program
            // selected by a live keyboard. Timeline ownership restores only
            // its own program overrides through restoreMidiProgram().
            if (m_programParameters[i] && m_midiProgramOverrides[i]) {
                continue;
            }
            m_processingValues[i] = m_values[i].load();
            m_dirty[i] = true;
        }
    }

    bool restoreManualParameter(const std::string& id) noexcept override
    {
        for (int32 i = 0; i < m_parameterCount; ++i) {
            if (m_manualParameters[i] && id == m_idStrings[i]) {
                m_processingValues[i] = m_values[i].load();
                m_dirty[i] = true;
                return true;
            }
        }
        return false;
    }

    bool restoreMidiController(int channel, int control) noexcept override
    {
        if (channel < 0 || channel >= 16 || control < 0 || control >= 130) {
            return false;
        }
        const auto index = m_controllerMappingIndex[channel][control];
        if (index < 0) {
            return false;
        }
        if (m_manualParameters[index]) {
            m_processingValues[index] = m_values[index].load();
            m_dirty[index] = true;
            return true;
        }
        if (control < 120 && !MidiControllerResetRequired(control) && MidiControllerResetValue(control) >= 0) {
            // A virtual CC bridge is an event rather than a saved synth knob.
            // Restore the manual/automated baseline without replaying its
            // synthetic default (which may overwrite a different parameter).
            restoreManualParameters();
            return true;
        }
        return false;
    }

    bool restoreMidiProgram(int channel) noexcept override
    {
        if (channel < 0 || channel >= 16) {
            return false;
        }
        const auto index = m_programMapping[channel].index;
        if (index < 0) {
            return false;
        }
        m_processingValues[index] = m_values[index].load();
        m_dirty[index] = true;
        return true;
    }

    size_t latency() const override { return m_wrapper->CachedLatencySamples(); }
    size_t reportedLatency() const override { return std::max<int32>(0, m_wrapper->GetLatencySamples()); }
    bool latencyUpdatePending() const noexcept override
    {
        return m_wrapper->LatencyUpdatePending() || m_latencyMeasurePending.load(std::memory_order_acquire);
    }

    bool requiresMainThreadLatencyRefresh() const noexcept override { return true; }
    bool refreshLatency() override
    {
        m_latencyMeasurePending.exchange(false, std::memory_order_acq_rel);
        const bool restart = m_wrapper->LatencyUpdatePending();
        const bool changed = m_wrapper->RefreshLatency(true);
        // Reactivation may erase voices even when the reported delay stays the
        // same. Restore on the next DSP call, without touching input queues.
        m_restoreAfterRestart |= restart;
        return changed;
    }

    void prepare(double sampleRate, size_t maximumFrames) override
    {
        if (m_wrapper->IsActive()) {
            m_wrapper->Finalize(nullptr);
        }
        // Construction restores the opaque native preset once. Device setup
        // and transport preparation must not reload an older save snapshot
        // into this same processor (or make the plugin load its samples again).
        if (!m_wrapper->Initialize(m_settings, sampleRate, Vst::kRealtime, static_cast<int32>(maximumFrames), false)) {
            throw std::runtime_error("Cannot prepare the VST3 instrument for playback.");
        }
        m_rate = sampleRate;
        std::fill(m_lastParameters.begin(), m_lastParameters.end(), ParameterSnapshot {});
        std::fill(m_midiProgramOverrides.begin(), m_midiProgramOverrides.end(), false);
        m_restoreAfterRestart = false;
        m_latencyMeasurePending = false;
        m_nextContext = {};
        m_nextContext.sampleRate = sampleRate;
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (!savedParameter(index)) {
                continue;
            }
            m_wrapper->mEditController->setParamNormalized(m_ids[index], m_values[index].load());
            m_processingValues[index] = m_values[index].load();
            m_dirty[index] = true;
        }
        m_inputPointers.clear();
        m_outputPointers.clear();
        m_input.resize(VST3Utils::CountChannels(m_wrapper->mEffectComponent, Vst::kAudio, Vst::kInput, Vst::kMain));
        m_output.resize(VST3Utils::CountChannels(m_wrapper->mEffectComponent, Vst::kAudio, Vst::kOutput, Vst::kMain));
        if (m_output.empty()) {
            throw std::runtime_error("The VST3 instrument has no audio output.");
        }
        for (auto& buffer : m_input) {
            buffer.assign(maximumFrames, 0.0f);
        }
        for (auto& buffer : m_output) {
            buffer.assign(maximumFrames, 0.0f);
        }
        for (auto& buffer : m_input) {
            m_inputPointers.push_back(buffer.data());
        }
        for (auto& buffer : m_output) {
            m_outputPointers.push_back(buffer.data());
        }
        reset();
    }

    void reset() override { m_reset = true; }
    void resetTimeline() override
    {
        reset();
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (!m_programParameters[index]) {
                continue;
            }
            m_processingValues[index] = m_values[index].load();
            m_dirty[index] = true;
        }
    }

    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override
    {
        m_events.clear();
        m_parameters.clearQueue();
        if (m_reset) {
            for (int channel = 0; channel < 16; ++channel) {
                for (int note = 0; note < 128; ++note) {
                    if (m_held[channel][note]) {
                        receive({ 0, static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(note), 0 });
                    }
                }
                receive({ 0, static_cast<uint8_t>(0xb0 | channel), 64, 0 });
                receive({ 0, static_cast<uint8_t>(0xb0 | channel), 120, 0 });
            }
            m_held = {};
            m_sustained = {};
            m_reset = false;
        }
        if (m_restoreAfterRestart) {
            restoreRestartedProcessor();
        }
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (m_dirty[index].exchange(false)) {
                m_midiProgramOverrides[index] = false;
                addParameter(m_ids[index], 0, m_processingValues[index].load(), index);
            }
        }
        for (const auto& event : events) {
            receive(event);
        }
        Vst::ProcessContext transport {};
        transport.state = Vst::ProcessContext::kTempoValid | Vst::ProcessContext::kTimeSigValid
                          | Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kBarPositionValid
                          | Vst::ProcessContext::kCycleValid
                          | ((context.continuousSample || !context.cycling) ? Vst::ProcessContext::kContTimeValid : 0)
                          | (context.playing ? Vst::ProcessContext::kPlaying : 0)
                          | (context.cycling ? Vst::ProcessContext::kCycleActive : 0);
        transport.sampleRate = m_rate;
        transport.projectTimeSamples = context.sample;
        transport.continousTimeSamples = context.playing ? context.continuousSample.value_or(context.sample) : context.sample;
        transport.projectTimeMusic = context.sample / m_rate * context.tempo / 60.0;
        transport.tempo = context.tempo;
        transport.timeSigNumerator = context.numerator;
        transport.timeSigDenominator = context.denominator;
        const double bar = context.numerator * 4.0 / context.denominator;
        transport.barPositionMusic = std::floor(transport.projectTimeMusic / bar) * bar;
        transport.cycleStartMusic = context.cycleStart * context.tempo / 60.0;
        transport.cycleEndMusic = context.cycleEnd * context.tempo / 60.0;
        const auto count = m_wrapper->Process(m_inputPointers.empty() ? nullptr : m_inputPointers.data(),
                                              m_outputPointers.data(), frames, &m_events, &transport, &m_parameters);
        m_nextContext = transport;
        if (context.playing) {
            m_nextContext.projectTimeSamples += count;
            m_nextContext.continousTimeSamples += count;
            m_nextContext.projectTimeMusic += count / m_rate * context.tempo / 60.0;
            m_nextContext.barPositionMusic = std::floor(m_nextContext.projectTimeMusic / bar) * bar;
        }
        for (size_t channel = 0; channel < channels; ++channel) {
            const auto& source = m_output[std::min(channel, m_output.size() - 1)];
            std::copy_n(source.data(), count, output[channel]);
            std::fill(output[channel] + count, output[channel] + frames, 0.0f);
        }
    }

    bool attachView(void* parent, int x, int y, int width, int height) override
    {
        bool created = false;
        if (m_view && !m_window) {
            detachView();
        }
        if (!m_view) {
            m_view = owned(m_wrapper->mEditController->createView(Vst::ViewType::kEditor));
            if (!m_view || m_view->isPlatformTypeSupported(platformViewType()) != kResultTrue) {
                m_view = nullptr;
                return false;
            }
            m_view->getSize(&m_editorSize);
            QWindow* window = nullptr;
            for (auto* candidate : QGuiApplication::allWindows()) {
                if (reinterpret_cast<void*>(candidate->winId()) == parent) {
                    window = candidate;
                    break;
                }
            }
            if (!window) {
                m_view = nullptr;
                return false;
            }
            m_window = new QWindow(window);
            m_view->setFrame(this);
            if (m_view->attached(reinterpret_cast<void*>(m_window->winId()), platformViewType()) != kResultOk) {
                detachView();
                return false;
            }
            created = true;
        }
        const auto viewWidth = created && m_editorSize.getWidth() > 0 ? m_editorSize.getWidth() : width;
        const auto viewHeight = created && m_editorSize.getHeight() > 0 ? m_editorSize.getHeight() : height;
        m_window->setGeometry(x, y, viewWidth, viewHeight);
        if (m_view->canResize() == kResultTrue) {
            ViewRect size { 0, 0, viewWidth, viewHeight };
            m_view->onSize(&size);
        }
        m_window->show();
        return true;
    }

    void detachView() override
    {
        if (m_view) {
            m_view->removed();
            m_view->setFrame(nullptr);
            m_view = nullptr;
        }
        delete m_window;
        m_window = nullptr;
    }

    audio::InstrumentEditorSize editorSize() const override
    {
        return { m_editorSize.getWidth(), m_editorSize.getHeight() };
    }

    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override
    {
        if (view != m_view || !size) {
            return kInvalidArgument;
        }
        m_editorSize = *size;
        return view->onSize(size);
    }

    tresult PLUGIN_API queryInterface(const TUID iid, void** object) override
    {
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
            *object = static_cast<IPlugFrame*>(this);
            addRef();
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return ++m_references; }
    uint32 PLUGIN_API release() override { return --m_references; }

private:
    bool savedParameter(int32 index) const { return m_manualParameters[index] || m_programParameters[index]; }

    void cacheProgramMapping()
    {
        // Query the controller only during native setup. Program changes are
        // processor parameter points, never calls into a controller from DSP.
        const auto controller = m_wrapper->mEditController;
        const FUnknownPtr<Vst::IUnitInfo> units(controller);
        std::vector<Vst::ParameterInfo> parameters(m_parameterCount);
        for (int32 index = 0; index < m_parameterCount; ++index) {
            controller->getParameterInfo(index, parameters[index]);
        }
        for (int channel = 0; channel < 16; ++channel) {
            Vst::UnitID unit = Vst::kRootUnitId;
            if (units) {
                Vst::UnitID assigned = Vst::kRootUnitId;
                if (units->getUnitByBus(Vst::kEvent, Vst::kInput, 0, channel, assigned) == kResultTrue) {
                    unit = assigned;
                }
            }
            int32 index = -1;
            for (int32 candidate = 0; candidate < m_parameterCount; ++candidate) {
                if (m_programParameters[candidate] && parameters[candidate].unitId == unit) {
                    if (index >= 0) {
                        index = -1;
                        break;
                    }                                      // Ambiguous program parameter.
                    index = candidate;
                }
            }
            if (index < 0 || parameters[index].stepCount < 0) {
                continue;
            }
            const auto count = static_cast<int64>(parameters[index].stepCount) + 1;
            if (units) {
                Vst::ProgramListID list = Vst::kNoProgramListId;
                for (int32 i = 0; i < units->getUnitCount(); ++i) {
                    Vst::UnitInfo info {};
                    if (units->getUnitInfo(i, info) == kResultTrue && info.id == unit) {
                        list = info.programListId;
                        break;
                    }
                }
                bool valid = false;
                if (list != Vst::kNoProgramListId) {
                    for (int32 i = 0; i < units->getProgramListCount(); ++i) {
                        Vst::ProgramListInfo info {};
                        if (units->getProgramListInfo(i, info) == kResultTrue && info.id == list) {
                            valid = info.programCount == count;
                            break;
                        }
                    }
                }
                if (!valid) {
                    continue;
                }
            }
            m_programMapping[channel] = { index, count };
        }
    }

    int32 parameterIndex(Vst::ParamID id) const
    {
        const auto found = std::find(m_ids.begin(), m_ids.end(), id);
        return found == m_ids.end() ? -1 : static_cast<int32>(found - m_ids.begin());
    }

    void rememberParameter(int32 index, double value)
    {
        if (index < 0) {
            return;
        }
        // A processing-only edit need not synchronously reach the controller
        // (processor/controller connection messages are main-thread-only).
        // Ask main to measure after the block, without querying native DSP here
        // or assuming that every parameter change requires reactivation.
        if (!m_lastParameters[index].order || m_lastParameters[index].value != value) {
            m_latencyMeasurePending.store(true, std::memory_order_release);
        }
        m_lastParameters[index] = { value, ++m_writeOrder };
        if (value < 0.5) {
            for (int channel = 0; channel < 16; ++channel) {
                if (m_controllerMappingIndex[channel][64] == index) {
                    m_sustained[channel].fill(0);
                }
            }
        }
    }

    void queueParameter(Vst::ParamID id, int32 sample, double value)
    {
        int32 index = 0;
        auto* queue = m_parameters.addParameterData(id, index);
        if (queue) {
            queue->addPoint(sample, value, index);
        }
    }

    void addParameter(Vst::ParamID id, int32 sample, double value, int32 index)
    {
        rememberParameter(index, value);
        queueParameter(id, sample, value);
    }

    bool pedalDown(int channel) const
    {
        const auto index = m_controllerMappingIndex[channel][64];
        return index >= 0 && m_lastParameters[index].order && m_lastParameters[index].value >= 0.5;
    }

    void restoreRestartedProcessor()
    {
        // Merge edits made while DSP was stopped before rebuilding parameter
        // order. Virtual MIDI bridges can affect the same synth knob as an
        // ordinary parameter, so the most recent write must win on replay.
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (m_dirty[index].exchange(false)) {
                m_midiProgramOverrides[index] = false;
                rememberParameter(index, m_processingValues[index].load());
            }
        }
        m_parameterReplay.clear();
        for (int32 index = 0; index < m_parameterCount; ++index) {
            if (m_lastParameters[index].order) {
                m_parameterReplay.push_back(index);
            }
        }
        std::sort(m_parameterReplay.begin(), m_parameterReplay.end(), [&](auto a, auto b) {
            return m_lastParameters[a].order < m_lastParameters[b].order;
        });
        m_parameters.clearQueue();
        for (const auto index : m_parameterReplay) {
            queueParameter(m_ids[index], 0, m_lastParameters[index].value);
        }
        if (!m_wrapper->FlushProcessingParameters(m_parameters, m_nextContext)) {
            throw std::runtime_error("The VST3 instrument could not restore its controls after reactivation.");
        }
        m_parameters.clearQueue();
        m_noteReplay.clear();
        for (int channel = 0; channel < 16; ++channel) {
            for (int note = 0; note < 128; ++note) {
                if (m_held[channel][note] || m_sustained[channel][note]) {
                    m_noteReplay.emplace_back(channel, note);
                }
            }
        }
        std::sort(m_noteReplay.begin(), m_noteReplay.end(), [&](auto a, auto b) {
            return m_noteOrder[a.first][a.second] < m_noteOrder[b.first][b.second];
        });
        for (const auto [channel, note] : m_noteReplay) {
            const auto held = m_held[channel][note];
            const auto velocity = held ? held : m_sustained[channel][note];
            const auto pressure = m_polyPressure[channel][note];
            receive({ 0, static_cast<uint8_t>(0x90 | channel), static_cast<uint8_t>(note), velocity });
            if (m_polyPressureSeen[channel][note]) {
                receive({ 0, static_cast<uint8_t>(0xa0 | channel), static_cast<uint8_t>(note), pressure });
            }
            if (!held) {
                receive({ 0, static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(note), 0 });
            }
        }
        m_restoreAfterRestart = false;
    }

    void receive(const MidiEvent& source)
    {
        const auto type = source.status & 0xf0;
        const auto channel = source.status & 0xf;
        Vst::Event event {};
        event.sampleOffset = static_cast<int32>(source.sample);
        if (type == 0x90 && source.data2 != 0) {
            event.type = Vst::Event::kNoteOnEvent;
            event.noteOn = { static_cast<int16>(channel), source.data1, 0.0f, source.data2 / 127.0f, 0, -1 };
            m_held[channel][source.data1] = source.data2;
            m_sustained[channel][source.data1] = 0;
            m_polyPressure[channel][source.data1] = 0;
            m_noteOrder[channel][source.data1] = ++m_writeOrder;
            m_events.addEvent(event);
        } else if (type == 0x80 || type == 0x90) {
            event.type = Vst::Event::kNoteOffEvent;
            event.noteOff = { static_cast<int16>(channel), source.data1, source.data2 / 127.0f, -1, 0.0f };
            if (pedalDown(channel)) {
                if (m_held[channel][source.data1]) {
                    m_sustained[channel][source.data1] = m_held[channel][source.data1];
                }
            } else {
                m_sustained[channel][source.data1] = 0;
            }
            m_held[channel][source.data1] = 0;
            m_events.addEvent(event);
        } else if (type == 0xa0) {
            m_polyPressureSeen[channel][source.data1] = true;
            m_polyPressure[channel][source.data1] = source.data2;
            event.type = Vst::Event::kPolyPressureEvent;
            event.polyPressure = { static_cast<int16>(channel), source.data1, source.data2 / 127.0f, -1 };
            m_events.addEvent(event);
        } else if (type == 0xc0) {
            const auto mapping = m_programMapping[channel];
            if (mapping.index >= 0 && source.data1 < mapping.count) {
                const auto value = mapping.count == 1 ? 0.0 : source.data1 / static_cast<double>(mapping.count - 1);
                addParameter(m_ids[mapping.index], event.sampleOffset, value, mapping.index);
                m_midiProgramOverrides[mapping.index] = true;
            }
        } else if (type == 0xb0 || type == 0xd0 || type == 0xe0) {
            if (type == 0xb0 && source.data1 < 120) {
                m_controllersSeen[channel][source.data1] = true;
            }
            if (type == 0xb0 && source.data1 == 121) {
                // There need not be a Reset All Controllers mapping. Reset
                // only the specified MIDI controllers, without changing the
                // stored manual preset or non-resettable volume/pan/banks.
                for (int cc = 0; cc < 120; ++cc) {
                    const auto value = MidiControllerResetValue(cc);
                    if (value >= 0 && (m_controllersSeen[channel][cc] || MidiControllerResetRequired(cc))) {
                        receive({ source.sample, source.status, static_cast<uint8_t>(cc), static_cast<uint8_t>(value) });
                    }
                }
                receive({ source.sample, static_cast<uint8_t>(0xe0 | channel), 0, 64 });
                receive({ source.sample, static_cast<uint8_t>(0xd0 | channel), 0, 0 });
                for (int note = 0; note < 128; ++note) {
                    if (m_polyPressureSeen[channel][note]) {
                        receive({ source.sample, static_cast<uint8_t>(0xa0 | channel), static_cast<uint8_t>(note), 0 });
                    }
                }
            }
            if (type == 0xb0 && (source.data1 == 120 || source.data1 >= 123)) {
                // VST3 MIDI controllers are parameter mappings. A plugin need
                // not expose a mapping for All Notes Off or mode changes, so
                // also release the host's actual keys at the command offset.
                for (int note = 0; note < 128; ++note) {
                    if (m_held[channel][note]) {
                        receive({ source.sample, static_cast<uint8_t>(0x80 | channel), static_cast<uint8_t>(note), 0 });
                    }
                }
                if (source.data1 == 120) {
                    m_sustained[channel].fill(0);
                }
            }
            const int controller = type == 0xe0 ? Vst::kPitchBend : type == 0xd0 ? Vst::kAfterTouch : source.data1;
            const double value = type == 0xe0 ? (source.data1 + 128 * source.data2) / 16383.0
                                 : type == 0xd0 ? source.data1 / 127.0 : source.data2 / 127.0;
            const auto parameter = m_controllerMapping[channel][controller];
            if (parameter != Vst::kNoParamId) {
                addParameter(parameter, event.sampleOffset, value, m_controllerMappingIndex[channel][controller]);
            }
        }
    }

    std::shared_ptr<VST3::Hosting::Module> m_module;
    std::unique_ptr<VST3::Hosting::ClassInfo> m_class;
    std::unique_ptr<VST3Wrapper> m_wrapper;
    mutable EffectSettings m_settings;
    IPtr<Vst::IMidiMapping> m_midiMapping;
    std::array<std::array<Vst::ParamID, 130>, 16> m_controllerMapping {};
    std::array<std::array<int32, 130>, 16> m_controllerMappingIndex {};
    std::vector<Vst::ParamID> m_ids;
    std::vector<bool> m_manualParameters;
    std::vector<bool> m_programParameters;
    std::vector<bool> m_midiProgramOverrides;
    struct ProgramMapping {
        int32 index = -1;
        int64 count = 0;
    };
    std::array<ProgramMapping, 16> m_programMapping {};
    std::unique_ptr<std::atomic<double>[]> m_values;
    std::unique_ptr<std::atomic<double>[]> m_processingValues;
    std::vector<std::string> m_idStrings;
    std::unique_ptr<std::atomic<bool>[]> m_dirty;
    int32 m_parameterCount = 0;
    Vst::EventList m_events;
    Vst::ParameterChanges m_parameters;
    struct ParameterSnapshot {
        double value = 0;
        uint64_t order = 0;
    };
    std::vector<ParameterSnapshot> m_lastParameters;
    std::vector<int32> m_parameterReplay;
    std::vector<std::pair<int, int> > m_noteReplay;
    uint64_t m_writeOrder = 0;
    std::array<std::array<uint8_t, 128>, 16> m_held {}, m_sustained {}, m_polyPressure {};
    std::array<std::array<uint64_t, 128>, 16> m_noteOrder {};
    std::array<std::array<bool, 128>, 16> m_controllersSeen {}, m_polyPressureSeen {};
    std::vector<std::vector<float> > m_input;
    std::vector<std::vector<float> > m_output;
    std::vector<const float*> m_inputPointers;
    std::vector<float*> m_outputPointers;
    IPtr<IPlugView> m_view;
    ViewRect m_editorSize {};
    QPointer<QWindow> m_window;
    std::atomic<bool> m_changed { false };
    std::atomic<bool> m_latencyMeasurePending { false };
    std::atomic<uint32> m_references { 1 };
    bool m_reset = true;
    bool m_restoreAfterRestart = false;
    Vst::ProcessContext m_nextContext {};
    double m_rate = 44100.0;
};
}

std::shared_ptr<InstrumentProcessor> createVstInstrument(const std::string& path, const std::string& state)
{
    return std::make_shared<VstInstrument>(path, state);
}
}
