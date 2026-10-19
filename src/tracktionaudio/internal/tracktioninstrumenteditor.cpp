/*
* Audacity: A Digital Audio Editor
*/
#include "tracktioninstrumenteditor.h"
#include "tracktioninstrument.h"

#include <cmath>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <QTemporaryDir>

#include "tracktionruntime.h"
#include "tracktioneffectinstance.h"
#include "au3-realtime-effects/RealtimeEffectState.h"
#include "global/defer.h"

namespace au::tracktionaudio {
tracktion::Plugin::Ptr createTracktionInstrument(tracktion::Edit& edit, const std::string& descriptionXml, const std::string& state)
{
    juce::PluginDescription description;
    if (!descriptionXml.empty()) {
        const auto xml = juce::parseXML(descriptionXml);
        if (!xml || !description.loadFromXml(*xml) || !description.isInstrument) {
            throw std::runtime_error("The instrument description is invalid.");
        }
        edit.engine.getPluginManager().knownPluginList.addType(description);
    }
    tracktion::Plugin::Ptr instrument;
    if (!state.empty()) {
        const auto xml = juce::parseXML(state);
        if (!xml || !xml->hasTagName(tracktion::IDs::PLUGIN.toString())) {
            throw std::runtime_error("The instrument state is invalid.");
        }
        auto pluginState = juce::ValueTree::fromXml(*xml);
        tracktion::EditItemID::remapIDs(pluginState, nullptr, edit);
        instrument = edit.getPluginCache().createNewPlugin(pluginState);
    } else if (!descriptionXml.empty()) {
        instrument = edit.getPluginCache().createNewPlugin(tracktion::ExternalPlugin::xmlTypeName, description);
    } else {
        instrument = edit.getPluginCache().createNewPlugin(tracktion::FourOscPlugin::xmlTypeName, {});
    }
    if (!instrument) {
        throw std::runtime_error("Cannot load the instrument.");
    }
    return instrument;
}

namespace {
class InstrumentEditor final : public audio::IInstrumentEditor, private juce::AudioProcessorListener, private juce::ValueTree::Listener,
    private tracktion::PhysicalMidiInputDevice::Listener
{
public:
    InstrumentEditor(const std::string& description, const std::string& state)
    {
        if (!m_directory.isValid()) {
            throw std::runtime_error("Cannot create the instrument settings directory.");
        }
        m_runtime.init(m_directory.filePath("cache").toStdString(), m_directory.filePath("settings").toStdString());
        m_ownedEdit = std::make_unique<tracktion::Edit>(m_runtime.engine(), tracktion::Edit::forRendering);
        m_edit = m_ownedEdit.get();
        m_plugin = createTracktionInstrument(*m_edit, description, state);
        auto track = m_edit->insertNewAudioTrack(tracktion::TrackInsertPoint::getEndOfTracks(*m_edit), nullptr);
        track->pluginList.insertPlugin(m_plugin, 0, nullptr);
        m_edit->dispatchPendingUpdatesSynchronously();
        auto* external = dynamic_cast<tracktion::ExternalPlugin*>(m_plugin.get());
        if (external) {
            if (!external->getAudioPluginInstance()) {
                throw std::runtime_error("The instrument is not available.");
            }
        }
        listen();
    }

    InstrumentEditor(tracktion::Edit& edit, tracktion::AudioTrack& track)
        : m_edit(&edit), m_track(&track), m_live(true)
    {
        m_plugin = track.pluginList[0];
        listen();
    }

    ~InstrumentEditor() override { close(); }

    bool active() const override { return m_plugin != nullptr; }

    void close() override
    {
        if (!active()) {
            return;
        }
        setMidiInput({});
        releaseKeyboardNotes();
        detachView();
        m_plugin->state.removeListener(this);
        if (auto* processor = audioProcessor()) {
            processor->removeListener(this);
        }
        m_track = nullptr;
        m_plugin = nullptr;
        m_edit = nullptr;
        m_ownedEdit.reset();
    }

    bool takeChanges() override { return m_gestures.load() == 0 && m_dirty.exchange(false); }

    std::vector<audio::InstrumentMidiInput> midiInputs() const override
    {
        std::vector<audio::InstrumentMidiInput> inputs;
        if (m_live && active()) {
            for (const auto& input : m_edit->engine.getDeviceManager().getMidiInDevices()) {
                if (dynamic_cast<tracktion::PhysicalMidiInputDevice*>(input.get())) {
                    inputs.push_back({ input->getDeviceID().toStdString(), input->getName().toStdString() });
                }
            }
        }
        return inputs;
    }

    bool setMidiInput(const std::string& id) override
    {
        if (m_input) {
            m_input->removeListener(this);
            auto* context = m_edit->getTransport().getCurrentPlaybackContext();
            auto* instance = context ? context->getInputFor(m_input.get()) : nullptr;
            if (instance && m_track) {
                const auto removed = instance->removeTarget(m_track->itemID, nullptr);
                juce::ignoreUnused(removed);
                instance->dispatchPendingRecordingUpdates();
            }
            m_input->setMonitorMode(m_previousMonitorMode);
            m_input->setEnabled(m_previousEnabled);
            m_input.reset();
        }
        releaseKeyboardNotes();
        m_fifo.reset();
        if (id.empty()) {
            return true;
        }
        if (!m_live || !active()) {
            return false;
        }
        for (const auto& input : m_edit->engine.getDeviceManager().getMidiInDevices()) {
            if (input->getDeviceID().toStdString() == id) {
                auto physical = std::dynamic_pointer_cast<tracktion::PhysicalMidiInputDevice>(input);
                if (physical) {
                    m_previousEnabled = physical->isEnabled();
                    m_previousMonitorMode = physical->getMonitorMode();
                    m_input = std::move(physical);
                    m_input->setEnabled(true);
                    m_input->setMonitorMode(tracktion::InputDevice::MonitorMode::on);
                    auto& transport = m_edit->getTransport();
                    m_edit->engine.getDeviceManager().dispatchPendingUpdates();
                    transport.ensureContextAllocated();
                    auto* context = transport.getCurrentPlaybackContext();
                    auto* instance = context ? context->getInputFor(m_input.get()) : nullptr;
                    if (instance) {
                        const auto target = instance->setTarget(m_track->itemID, true, nullptr);
                        if (target) {
                            (*target)->recordEnabled = false;
                            instance->dispatchPendingRecordingUpdates();
                            m_edit->dispatchPendingUpdatesSynchronously();
                            m_input->addListener(this);
                            return true;
                        }
                    }
                    setMidiInput({});
                    return false;
                }
            }
        }
        return false;
    }

    void sendMidi(int status, int data1, int data2) override
    {
        if (!m_track || status < 0x80 || status >= 0xf0 || data1 < 0 || data1 > 127 || data2 < 0 || data2 > 127) {
            return;
        }
        const int channel = status & 0xf;
        const int type = status & 0xf0;
        if (type == 0x90 || type == 0x80) {
            m_keyboardNotes[channel][data1] = type == 0x90 && data2 > 0;
        } else if (type == 0xb0 && data1 == 64) {
            m_keyboardPedal[channel] = data2 >= 64;
        }
        const juce::MidiMessage message(status, data1, data2);
        m_track->injectLiveMidiMessage(message, {});
    }

    void releaseNotes() override
    {
        if (m_track) {
            for (int channel = 1; channel <= 16; ++channel) {
                m_track->injectLiveMidiMessage(juce::MidiMessage::controllerEvent(channel, 64, 0), {});
                m_track->injectLiveMidiMessage(juce::MidiMessage::allNotesOff(channel), {});
                m_track->injectLiveMidiMessage(juce::MidiMessage::allSoundOff(channel), {});
            }
            m_keyboardNotes = {};
            m_keyboardPedal = {};
        }
    }

    std::vector<audio::InstrumentMidiMessage> takeMidiMessages() override
    {
        std::vector<audio::InstrumentMidiMessage> messages;
        int start1, size1, start2, size2;
        m_fifo.prepareToRead(m_fifo.getNumReady(), start1, size1, start2, size2);
        const auto read = [&](int start, int size) {
            for (int i = start; i < start + size; ++i) {
                const auto message = m_messages[static_cast<size_t>(i)];
                messages.push_back(message);
            }
        };
        read(start1, size1);
        read(start2, size2);
        m_fifo.finishedRead(size1 + size2);
        if (m_overflow.exchange(false)) {
            releaseNotes();
            for (int channel = 0; channel < 16; ++channel) {
                messages.push_back({ 0xb0 | channel, 123, 0, now() });
            }
        }
        return messages;
    }

    bool attachView(void* parent, int x, int y, int width, int height) override
    {
        auto* processor = audioProcessor();
        if (!processor || !processor->hasEditor() || width <= 0 || height <= 0) {
            return false;
        }
        if (!m_view || m_parent != parent) {
            detachView();
            m_editor.reset(processor->createEditorAndMakeActive());
            if (!m_editor) {
                return false;
            }
            m_view = std::make_unique<juce::Component>();
            m_view->addAndMakeVisible(*m_editor);
            m_view->addToDesktop(0, parent);
            m_parent = parent;
        }
        m_view->setBounds(x, y, width, height);
        const float scale = std::min(1.0f, std::min(static_cast<float>(width) / m_editor->getWidth(),
                                                    static_cast<float>(height) / m_editor->getHeight()));
        if (m_editor->getTransform() != juce::AffineTransform::scale(scale)) {
            m_editor->setScaleFactor(scale);
        }
        m_view->setVisible(true);
        return true;
    }

    audio::InstrumentEditorSize editorSize() const override
    {
        return m_editor ? audio::InstrumentEditorSize { m_editor->getWidth(), m_editor->getHeight() } : audio::InstrumentEditorSize {};
    }

    void detachView() override
    {
        m_view.reset();
        m_editor.reset();
        m_parent = nullptr;
    }

    std::vector<audio::InstrumentParameter> parameters() const override
    {
        std::vector<audio::InstrumentParameter> result;
        if (!active()) {
            return result;
        }
        for (auto* parameter : m_plugin->getAutomatableParameters()) {
            result.push_back({ parameter->paramID.toStdString(), parameter->getParameterName().toStdString(),
                               parameter->getCurrentValueAsStringWithLabel().toStdString(), parameter->getCurrentNormalisedValue() });
        }
        return result;
    }

    bool setParameter(const std::string& id, double value) override
    {
        if (!active() || !std::isfinite(value) || value < 0.0 || value > 1.0) {
            return false;
        }
        for (auto* parameter : m_plugin->getAutomatableParameters()) {
            if (parameter->paramID == juce::String(id)) {
                parameter->setNormalisedParameter(static_cast<float>(value), juce::sendNotificationSync);
                m_edit->dispatchPendingUpdatesSynchronously();
                return true;
            }
        }
        return false;
    }

    std::string state() const override
    {
        if (!active()) {
            return {};
        }
        // State capture must not request another capture through plug-in notifications.
        const bool capturing = m_capturingState.exchange(true);
        DEFER { m_capturingState.store(capturing);
        };
        m_plugin->flushPluginStateToValueTree();
        return m_plugin->state.toXmlString().toStdString();
    }

private:
    void releaseKeyboardNotes()
    {
        if (!m_track) {
            return;
        }
        for (int channel = 0; channel < 16; ++channel) {
            for (int note = 0; note < 128; ++note) {
                if (m_keyboardNotes[channel][note]) {
                    m_track->injectLiveMidiMessage(juce::MidiMessage::noteOff(channel + 1, note), {});
                }
            }
            if (m_keyboardPedal[channel]) {
                m_track->injectLiveMidiMessage(juce::MidiMessage::controllerEvent(channel + 1, 64, 0), {});
            }
        }
        m_keyboardNotes = {};
        m_keyboardPedal = {};
    }

    static double now()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    juce::AudioPluginInstance* audioProcessor() const
    {
        auto* external = dynamic_cast<tracktion::ExternalPlugin*>(m_plugin.get());
        return external ? external->getAudioPluginInstance() : nullptr;
    }

    void listen()
    {
        m_plugin->state.addListener(this);
        if (auto* processor = audioProcessor()) {
            processor->addListener(this);
        }
    }

    void audioProcessorParameterChanged(juce::AudioProcessor* processor, int index, float) override
    {
        const auto& parameters = processor->getParameters();
        if (juce::isPositiveAndBelow(index, parameters.size())
            && parameters[index]->getCategory() < juce::AudioProcessorParameter::inputMeter) {
            stateChanged();
        }
    }

    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++m_gestures; }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) override { --m_gestures; }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override { stateChanged(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { stateChanged(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { stateChanged(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { stateChanged(); }

    void stateChanged()
    {
        if (!m_capturingState.load()) {
            m_dirty = true;
        }
    }

    void handleIncomingMidiMessage(const juce::MidiMessage& message) override
    {
        if (message.getRawDataSize() < 2 || message.getRawDataSize() > 3 || message.getRawData()[0] >= 0xf0) {
            return;
        }
        int start1, size1, start2, size2;
        m_fifo.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 + size2 == 0) {
            m_overflow = true;
            return;
        }
        const auto* bytes = message.getRawData();
        m_messages[static_cast<size_t>(size1 ? start1 : start2)] = {
            bytes[0], bytes[1], message.getRawDataSize() == 3 ? bytes[2] : 0, now()
        };
        m_fifo.finishedWrite(1);
    }

    QTemporaryDir m_directory;
    TracktionRuntime m_runtime;
    std::unique_ptr<tracktion::Edit> m_ownedEdit;
    tracktion::Edit* m_edit = nullptr;
    tracktion::AudioTrack* m_track = nullptr;
    bool m_live = false;
    mutable std::atomic<bool> m_capturingState { false };
    std::atomic<bool> m_dirty { false };
    std::atomic<int> m_gestures { 0 };
    std::atomic<bool> m_overflow { false };
    std::shared_ptr<tracktion::PhysicalMidiInputDevice> m_input;
    bool m_previousEnabled = false;
    tracktion::InputDevice::MonitorMode m_previousMonitorMode = tracktion::InputDevice::MonitorMode::off;
    juce::AbstractFifo m_fifo { 2048 };
    std::array<audio::InstrumentMidiMessage, 2048> m_messages;
    std::array<std::array<bool, 128>, 16> m_keyboardNotes {};
    std::array<bool, 16> m_keyboardPedal {};
    std::unique_ptr<juce::AudioProcessorEditor> m_editor;
    std::unique_ptr<juce::Component> m_view;
    void* m_parent = nullptr;
    tracktion::Plugin::Ptr m_plugin;
};
}

muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> > TracktionInstrumentEditorFactory::create(const std::string& description,
                                                                                                  const std::string& state) const
{
    try {
        return muse::RetVal<std::shared_ptr<audio::IInstrumentEditor> >::make_ok(std::make_shared<InstrumentEditor>(description, state));
    } catch (const std::exception& error) {
        return muse::make_ret(muse::Ret::Code::InternalError, error.what());
    }
}
}

namespace au::tracktionaudio {
std::shared_ptr<audio::IInstrumentEditor> createLiveInstrumentEditor(tracktion::Edit& edit, tracktion::AudioTrack& track)
{
    return std::make_shared<InstrumentEditor>(edit, track);
}
}

namespace au::tracktionaudio {
std::vector<audio::InstrumentParameter> TracktionInstrumentEditorFactory::parameters(RealtimeEffectState& effect) const
{
    std::vector<audio::InstrumentParameter> result;
    if (!effect.GetEffect()) {
        return result;
    }
    const auto instance = effect.GetInstance();
    auto* hosted = dynamic_cast<TracktionHostedEffectInstance*>(instance.get());
    if (hosted && hosted->Init()) {
        for (auto* parameter : hosted->tracktionPlugin().getAutomatableParameters()) {
            result.push_back({ parameter->paramID.toStdString(), parameter->getParameterName().toStdString(),
                               parameter->getCurrentValueAsStringWithLabel().toStdString(), parameter->getCurrentNormalisedValue() });
        }
    }
    return result;
}
}
