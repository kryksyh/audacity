/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>
#include <tracktion_graph/tracktion_graph.h>

#include "tracktionsession.h"
#include "tracktionautomation.h"
#include "au3-playable-track/TrackAutomation.h"
#include "au3-playable-track/TrackRouting.h"
#include "tracktioninstrument.h"
#include "au3-midi-track/MidiTrack.h"

#include <stdexcept>
#include <map>
#include <set>

#include "au3-mixer/Envelope.h"
#include "au3-mixer/MixerOptions.h"
#include "au3-numeric-formats/ProjectTimeSignature.h"
#include "au3-track/Track.h"
#include "au3-wave-track/WaveClip.h"
#include "au3-wave-track/WaveTrack.h"
#include "au3-realtime-effects/RealtimeEffectList.h"
#include "au3-realtime-effects/RealtimeEffectState.h"

#include "audacityaudiosource.h"
#include "audacityplugins.h"
#include "audacityeffectregistry.h"
#include "audiosourceregistry.h"
#include "tracktionruntime.h"
#include "tracktiontrackmeters.h"

using namespace au::tracktionaudio;

namespace {
tracktion::TimeStretcher::Mode clipTimeStretchMode(const WaveClip& clip)
{
    if (!clip.HasPitchOrSpeed()) {
        return tracktion::TimeStretcher::disabled;
    }
    return clip.GetPitchAndSpeedPreset() == PitchAndSpeedPreset::OptimizeForVoice
           ? tracktion::TimeStretcher::rubberbandVoice : tracktion::TimeStretcher::soundtouchBetter;
}
}

struct TracktionSession::Data : private juce::Timer
{
    struct EffectSlot {
        std::shared_ptr<RealtimeEffectState> state;
        tracktion::Plugin::Ptr plugin;
        juce::String id;
    };

    struct ClipBinding {
        std::shared_ptr<const WaveClip> source;
        tracktion::WaveAudioClip::Ptr target;
        std::shared_ptr<AudacityAudioSource> audio;
        size_t envelopeVersion;
        Observer::Subscription pitchSubscription;
        Observer::Subscription presetSubscription;
    };

    struct EffectChain {
        RealtimeEffectList& source;
        const ChannelGroup* group;
        int channels;
        tracktion::PluginList& target;
        tracktion::Plugin::Ptr anchor;
        bool live;
        std::vector<EffectSlot> entries;
        Observer::Subscription subscription;
    };

    explicit Data(TracktionRuntime& runtime)
        : engine(runtime.engine()), sources(runtime.sources()), effects(runtime.effects())
    {
    }

    ~Data()
    {
        stopTimer();
        trackSubscription.Reset();
        timeSignatureSubscription.Reset();
        clips.clear();
        effectChains.clear();
        bindings.clear();
        if (trackMeters) {
            // The audio callback can retain the meter source after this edit is closed.
            const juce::ScopedLock lock(engine.getDeviceManager().deviceManager.getAudioCallbackLock());
            trackMeters->clear();
        }
        edit.reset();
        for (const auto& id : effectIds) {
            effects->remove(id);
        }
        for (const auto& file : files) {
            engine.getAudioFileManager().releaseFile(tracktion::AudioFile(engine, file));
            sources->remove(file);
        }
    }

    void setTimeSignature(double tempo, int upper, int lower)
    {
        auto& sequence = edit->tempoSequence;
        // Keep Audacity tempo values and audio positions.
        sequence.getTempo(0)->bpm = tempo;
        sequence.getTimeSig(0)->numerator = upper;
        sequence.getTimeSig(0)->denominator = lower;
        sequence.updateTempoData();
        edit->getUndoManager().clearUndoHistory();
    }

    void observeEnvelopes()
    {
        startTimer(20);
    }

    void timerCallback() override
    {
        for (auto& clip : clips) {
            const auto& envelope = clip.source->GetEnvelope();
            const auto version = envelope.GetVersion();
            if (version != clip.envelopeVersion) {
                clip.audio->setEnvelope(envelope);
                clip.envelopeVersion = version;
            }
        }
    }

    void addEffects(RealtimeEffectList& list, const ChannelGroup* group, int channels, tracktion::PluginList& plugins,
                    int index, bool observe)
    {
        auto chain = std::make_unique<EffectChain>(EffectChain { list, group, channels, plugins, plugins[index], observe, {}, {} });
        auto* binding = chain.get();
        effectChains.push_back(std::move(chain));
        updateEffects(*binding);
        if (observe) {
            binding->subscription = list.Subscribe([this, binding](const RealtimeEffectListMessage& message) {
                if (message.type != RealtimeEffectListMessage::Type::WillReplace) {
                    updateEffects(*binding);
                    edit->getUndoManager().clearUndoHistory();
                }
            });
        }
    }

    void updateEffects(EffectChain& chain)
    {
        auto previous = std::move(chain.entries);
        for (const auto& slot : previous) {
            slot.plugin->removeFromParent();
        }
        for (size_t index = 0; index < chain.source.GetStatesCount(); ++index) {
            const auto state = chain.source.GetStateAt(index);
            const auto existing = std::find_if(previous.begin(), previous.end(), [&state](const EffectSlot& slot) {
                return slot.state == state;
            });
            if (existing != previous.end()) {
                chain.entries.push_back(std::move(*existing));
                previous.erase(existing);
            } else {
                const auto processingState = chain.live ? state : state->Clone();
                const auto id = effects->add(processingState, chain.source, chain.group, chain.channels, chain.live);
                if (id.isEmpty()) {
                    continue;
                }
                effectIds.push_back(id);
                juce::ValueTree pluginState(tracktion::IDs::PLUGIN);
                pluginState.setProperty(tracktion::IDs::type, AUDACITY_EFFECT_PLUGIN_TYPE, nullptr);
                pluginState.setProperty(AUDACITY_EFFECT_STATE_PROPERTY, id, nullptr);
                auto plugin = edit->getPluginCache().createNewPlugin(pluginState);
                if (automation) {
                    applyTracktionAutomation(*plugin, processingState->Automation());
                }
                chain.entries.push_back({ state, plugin, id });
            }
            const int position = chain.anchor ? chain.target.indexOf(chain.anchor.get()) : -1;
            chain.target.insertPlugin(chain.entries.back().plugin, position, nullptr);
        }
        for (const auto& slot : previous) {
            effects->remove(slot.id);
        }
    }

    void setMasterRenderChannels(unsigned channels)
    {
        for (auto& chain : effectChains) {
            if (chain->group) {
                continue;
            }
            for (const auto& slot : chain->entries) {
                slot.plugin->removeFromParent();
                effects->remove(slot.id);
            }
            chain->entries.clear();
            chain->channels = channels;
            updateEffects(*chain);
        }
    }

    void updateRoutingSolo()
    {
        if (!routed) {
            return;
        }
        std::vector<const PlayableTrack*> sources;
        for (const auto& [source, binding] : bindings) {
            sources.push_back(static_cast<const PlayableTrack*>(source));
        }
        const auto audible = TrackRouting::AudibleSources(sources, false);
        for (const auto& [source, binding] : bindings) {
            const bool allowed = std::find(audible.begin(), audible.end(), source) != audible.end();
            binding.track->setSoloIsolate(allowed);
            binding.track->setMute(static_cast<const PlayableTrack*>(source)->GetMute() || !allowed);
        }
        edit->updateMuteSoloStatuses();
    }

    void updateTrackControls(const TrackListEvent& event)
    {
        if (event.mType != TrackListEvent::TRACK_DATA_CHANGE) {
            return;
        }
        const auto source = event.mpTrack.lock();
        const auto binding = bindings.find(source.get());
        if (binding == bindings.end()) {
            return;
        }
        const auto& playable = static_cast<const PlayableTrack&>(*source);
        const bool muteSoloChanged = binding->second.sourceMute != playable.GetMute()
                                     || binding->second.sourceSolo != playable.GetSolo();
        if (!routed || muteSoloChanged) {
            binding->second.sourceMute = playable.GetMute();
            binding->second.sourceSolo = playable.GetSolo();
            binding->second.track->setMute(playable.GetMute());
            binding->second.track->setSolo(playable.GetSolo());
            if (routed) {
                updateRoutingSolo();
            }
        }
        const auto* midi = track_cast<const MidiTrack*>(source.get());
        if (midi) {
            if (binding->second.controls.isValid()) {
                binding->second.controls.setProperty(AUDACITY_VOLUME_PROPERTY, midi->GetGain(), nullptr);
                binding->second.controls.setProperty(AUDACITY_PAN_PROPERTY, midi->GetPan(), nullptr);
                return;
            }
            binding->second.track->getVolumePlugin()->setVolumeDb(juce::Decibels::gainToDecibels(static_cast<float>(midi->GetGain())));
            binding->second.track->getVolumePlugin()->setPan(static_cast<float>(midi->GetPan()));
            return;
        }
        const auto& waveTrack = static_cast<const WaveTrack&>(*source);
        binding->second.controls.setProperty(AUDACITY_VOLUME_PROPERTY, waveTrack.GetVolume(), nullptr);
        binding->second.controls.setProperty(AUDACITY_PAN_PROPERTY, waveTrack.GetPan(), nullptr);
        binding->second.controls.setProperty(AUDACITY_LEFT_GAIN_PROPERTY, waveTrack.GetChannelVolume(0), nullptr);
        binding->second.controls.setProperty(AUDACITY_RIGHT_GAIN_PROPERTY, waveTrack.GetChannelVolume(1), nullptr);
    }

    struct TrackBinding {
        tracktion::AudioTrack::Ptr track;
        juce::ValueTree controls;
        bool sourceMute = false;
        bool sourceSolo = false;
    };

    tracktion::Engine& engine;
    std::shared_ptr<AudioSourceRegistry> sources;
    std::shared_ptr<AudacityEffectRegistry> effects;
    std::shared_ptr<RealtimeEffectList> masterEffects;
    std::vector<std::shared_ptr<const WaveTrack> > effectTracks;
    std::vector<std::shared_ptr<const MidiTrack> > midiTracks;
    std::vector<const Track*> renderTracks;
    std::vector<juce::String> effectIds;
    std::vector<std::unique_ptr<EffectChain> > effectChains;
    std::vector<juce::File> files;
    std::vector<ClipBinding> clips;
    std::unique_ptr<tracktion::Edit> edit;
    std::shared_ptr<AudacityPlaybackControls> playbackControls;
    std::shared_ptr<TracktionTrackMeters> trackMeters;
    std::map<const Track*, TrackBinding> bindings;
    bool automation = true;
    bool routed = false;
    Observer::Subscription trackSubscription;
    Observer::Subscription timeSignatureSubscription;
};

TracktionSession::TracktionSession(TracktionRuntime& runtime, TrackList& tracks, Role role, Selection selection)
    : m_data(std::make_unique<Data>(runtime))
{
    std::vector<const WaveTrack*> sources;
    std::vector<const MidiTrack*> midiSources;
    const auto selected = TrackRouting::AudibleSources(tracks, true, TrackRouting::Audibility::Ignore);
    for (const auto* source : tracks.Any<const PlayableTrack>()) {
        if (selection == Selection::Selected && std::find(selected.begin(), selected.end(), source) == selected.end()) {
            continue;
        }
        if (const auto* wave = track_cast<const WaveTrack*>(source)) {
            sources.push_back(wave);
        } else if (const auto* midi = track_cast<const MidiTrack*>(source)) {
            midiSources.push_back(midi);
        }
    }
    initialise(runtime, tracks, role, sources, true, true, midiSources);
}

TracktionSession::TracktionSession(TracktionRuntime& runtime, TrackList& tracks, const TransportSequences& sequences, Role role)
    : m_data(std::make_unique<Data>(runtime))
{
    std::vector<const WaveTrack*> sources;
    for (const auto& sequence : sequences.playbackSequences) {
        const auto* track = dynamic_cast<const WaveTrack*>(sequence->FindChannelGroup());
        if (!track) {
            throw std::runtime_error("The playback sequence has no audio track.");
        }
        sources.push_back(track);
    }
    std::vector<const MidiTrack*> midiSources;
    for (const auto& sequence : sequences.otherPlayableSequences) {
        const auto* midi = dynamic_cast<const MidiTrack*>(sequence.get());
        if (!midi) {
            throw std::runtime_error("The playback sequence has no MIDI track.");
        }
        midiSources.push_back(midi);
    }
    initialise(runtime, tracks, role, sources, true, true, midiSources);
}

TracktionSession::TracktionSession(TracktionRuntime& runtime, TrackList& tracks, const std::vector<const WaveTrack*>& sources, Role role,
                                   bool trackEffects, bool masterEffects, const std::vector<const MidiTrack*>& midiSources, bool automation,
                                   bool routing)
    : m_data(std::make_unique<Data>(runtime))
{
    initialise(runtime, tracks, role, sources, trackEffects, masterEffects, midiSources, automation, routing);
}

void TracktionSession::initialise(TracktionRuntime& runtime, TrackList& tracks, Role role,
                                  const std::vector<const WaveTrack*>& requestedSources,
                                  bool trackEffects, bool masterEffects, const std::vector<const MidiTrack*>& midiSources, bool automation,
                                  bool routing)
{
    m_data->automation = automation;
    auto sources = requestedSources;
    std::map<std::string, const WaveTrack*> buses;
    if (routing) {
        if (!TrackRouting::Validate(tracks)) {
            throw std::runtime_error("The track routing contains a duplicate bus or a feedback loop.");
        }
        for (const auto* track : tracks.Any<const WaveTrack>()) {
            const auto& route = TrackRouting::Get(*track);
            if (!route.busId.empty()) {
                buses.emplace(route.busId, track);
            }
        }
        std::set<const WaveTrack*> included(sources.begin(), sources.end());
        const auto includeBus = [&](const std::string& id) {
            const auto bus = buses.find(id);
            if (bus != buses.end() && included.insert(bus->second).second) {
                sources.push_back(bus->second);
            }
        };
        for (const auto* midi : midiSources) {
            const auto& route = TrackRouting::Get(*midi);
            includeBus(route.output);
            for (const auto& send : route.sends) {
                includeBus(send.bus);
            }
        }
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto& route = TrackRouting::Get(*sources[i]);
            includeBus(route.output);
            for (const auto& send : route.sends) {
                includeBus(send.bus);
            }
        }
    }
    auto& engine = runtime.engine();
    m_data->edit = std::make_unique<tracktion::Edit>(tracktion::Edit::Options {
        .engine = engine,
        .editState = tracktion::createEmptyEdit(engine),
        .editProjectItemID = {},
        .role = role == Role::Playback ? tracktion::Edit::forEditing : tracktion::Edit::forRendering,
        .numAudioTracks = 0,
        .defaultMasterVolumedB = 0.0f,
    });

    auto& edit = *m_data->edit;
    edit.getAutomationRecordManager().setReadingAutomation(true);
    if (role == Role::Playback) {
        m_data->trackMeters = std::make_shared<TracktionTrackMeters>();
    }
    auto* project = tracks.GetOwner();
    if (project) {
        auto& signature = ProjectTimeSignature::Get(*project);
        m_data->setTimeSignature(signature.GetTempo(), signature.GetUpperTimeSignature(), signature.GetLowerTimeSignature());
        if (role == Role::Playback) {
            m_data->timeSignatureSubscription = signature.Subscribe([data = m_data.get()](const TimeSignatureChangedMessage& message) {
                data->setTimeSignature(message.newTempo, message.newUpperTimeSignature, message.newLowerTimeSignature);
            });
        }
    }
    if (project && masterEffects) {
        m_data->masterEffects = RealtimeEffectList::Get(*project).shared_from_this();
    }
    for (const auto* waveTrack : sources) {
        auto track = edit.insertNewAudioTrack(tracktion::TrackInsertPoint::getEndOfTracks(edit), nullptr);
        if (!track) {
            throw std::runtime_error("Cannot create a Tracktion audio track.");
        }
        track->setName(juce::String(waveTrack->GetName().utf8_string()));
        track->setMute(waveTrack->GetMute());
        track->setSolo(waveTrack->GetSolo());

        auto* volume = track->getVolumePlugin();
        volume->setPanLaw(tracktion::PanLawLinear);
        volume->setVolumeDb(0.0f);
        volume->setPan(0.0f);

        juce::ValueTree controls(tracktion::IDs::PLUGIN);
        controls.setProperty(tracktion::IDs::type, AUDACITY_TRACK_CONTROLS_PLUGIN_TYPE, nullptr);
        controls.setProperty(AUDACITY_VOLUME_PROPERTY, waveTrack->GetVolume(), nullptr);
        controls.setProperty(AUDACITY_PAN_PROPERTY, waveTrack->GetPan(), nullptr);
        controls.setProperty(AUDACITY_LEFT_GAIN_PROPERTY, waveTrack->GetChannelVolume(0), nullptr);
        controls.setProperty(AUDACITY_RIGHT_GAIN_PROPERTY, waveTrack->GetChannelVolume(1), nullptr);
        auto controlsPlugin = edit.getPluginCache().createNewPlugin(controls);
        if (automation) {
            applyTracktionAutomation(*controlsPlugin, TrackAutomation::Get(*waveTrack));
        }
        track->pluginList.insertPlugin(controlsPlugin, track->pluginList.indexOf(volume) + 1, nullptr);
        if (m_data->trackMeters) {
            m_data->trackMeters->addTrack(*track, waveTrack->GetId(), waveTrack->NChannels());
        }
        m_data->bindings.emplace(waveTrack,
                                 Data::TrackBinding { track, controlsPlugin->state, waveTrack->GetMute(), waveTrack->GetSolo() });
        m_data->effectTracks.push_back(std::static_pointer_cast<const WaveTrack>(waveTrack->shared_from_this()));
        if (trackEffects) {
            auto& effectList = RealtimeEffectList::Get(const_cast<WaveTrack&>(*waveTrack));
            m_data->addEffects(effectList, waveTrack, waveTrack->NChannels(), track->pluginList, track->pluginList.indexOf(volume),
                               role == Role::Playback);
        }

        for (const auto& waveClip : waveTrack->Intervals()) {
            auto source = std::make_shared<AudacityAudioSource>(*waveClip);
            const auto file = m_data->sources->add(source);
            if (file == juce::File()) {
                throw std::runtime_error("Cannot create an audio source descriptor.");
            }
            m_data->files.push_back(file);

            const tracktion::ClipPosition position {
                .time = { tracktion::TimePosition::fromSeconds(waveClip->GetPlayStartTime()),
                          tracktion::TimePosition::fromSeconds(waveClip->GetPlayEndTime()) },
                .offset = tracktion::TimeDuration::fromSeconds(waveClip->GetTrimLeft()),
            };
            auto clip = track->insertWaveClip(juce::String(waveClip->GetName().utf8_string()), file, position, false);
            if (!clip) {
                throw std::runtime_error("Cannot create a Tracktion audio clip.");
            }
            clip->setAutoTempo(false);
            clip->setAutoPitch(false);
            clip->setUsesProxy(false);
            clip->setSpeedRatio(1.0 / waveClip->GetStretchRatio());
            clip->setPitchChange(waveClip->GetCentShift() / 100.0f);
            clip->setTimeStretchMode(clipTimeStretchMode(*waveClip));
            if (role == Role::Playback) {
                m_data->clips.push_back({ waveClip, clip, source, waveClip->GetEnvelope().GetVersion(),
                                          waveClip->SubscribeToCentShiftChange([data = m_data.get(), target = clip.get(),
                                                                                original = waveClip.get()](int cents) {
                        target->setPitchChange(cents / 100.0f);
                        target->setTimeStretchMode(clipTimeStretchMode(*original));
                        data->edit->getUndoManager().clearUndoHistory();
                    }), waveClip->SubscribeToPitchAndSpeedPresetChange([data = m_data.get(), target = clip.get(),
                                                                        original = waveClip.get()](PitchAndSpeedPreset) {
                        target->setTimeStretchMode(clipTimeStretchMode(*original));
                        data->edit->getUndoManager().clearUndoHistory();
                    }) });
            }

            if (role == Role::Playback || !source->envelope()->IsTrivial()) {
                juce::ValueTree state(tracktion::IDs::PLUGIN);
                state.setProperty(tracktion::IDs::type, AUDACITY_ENVELOPE_PLUGIN_TYPE, nullptr);
                state.setProperty(AUDACITY_SOURCE_ID_PROPERTY, file.getFileNameWithoutExtension(), nullptr);
                auto plugin = edit.getPluginCache().createNewPlugin(state);
                clip->getPluginList()->insertPlugin(plugin, -1, nullptr);
            }
        }
    }
    for (const auto* midi : midiSources) {
        auto track = edit.insertNewAudioTrack(tracktion::TrackInsertPoint::getEndOfTracks(edit), nullptr);
        if (!track) {
            throw std::runtime_error("Cannot create a Tracktion instrument track.");
        }
        track->setName(juce::String(midi->GetName().utf8_string()));
        track->setMute(midi->GetMute());
        track->setSolo(midi->GetSolo());
        auto* volume = track->getVolumePlugin();
        volume->setVolumeDb(juce::Decibels::gainToDecibels(static_cast<float>(midi->GetGain())));
        volume->setPan(static_cast<float>(midi->GetPan()));
        const auto instrument = createTracktionInstrument(edit, midi->GetInstrumentDescription().utf8_string(),
                                                          midi->GetInstrumentState().utf8_string());
        track->pluginList.insertPlugin(instrument, 0, nullptr);
        if (automation) {
            applyTracktionAutomation(*instrument, TrackAutomation::Get(*midi), "instrument/");
        }
        auto* external = dynamic_cast<tracktion::ExternalPlugin*>(instrument.get());
        if (external) {
            if (!external->getAudioPluginInstance()) {
                throw std::runtime_error("The instrument is not available.");
            }
        }
        if (trackEffects) {
            auto& effectList = RealtimeEffectList::Get(const_cast<MidiTrack&>(*midi));
            m_data->addEffects(effectList, midi, 2, track->pluginList, track->pluginList.indexOf(volume), role == Role::Playback);
        }
        juce::ValueTree midiControls;
        if (automation && hasTrackAutomation(TrackAutomation::Get(*midi))) {
            midiControls = juce::ValueTree(tracktion::IDs::PLUGIN);
            midiControls.setProperty(tracktion::IDs::type, AUDACITY_TRACK_CONTROLS_PLUGIN_TYPE, nullptr);
            midiControls.setProperty(AUDACITY_VOLUME_PROPERTY, midi->GetGain(), nullptr);
            midiControls.setProperty(AUDACITY_PAN_PROPERTY, midi->GetPan(), nullptr);
            auto controls = edit.getPluginCache().createNewPlugin(midiControls);
            applyTracktionAutomation(*controls, TrackAutomation::Get(*midi));
            track->pluginList.insertPlugin(controls, -1, nullptr);
            volume->setVolumeDb(0.0f);
            volume->setPan(0.0f);
            volume->setPanLaw(tracktion::PanLawLinear);
        }
        m_data->bindings.emplace(midi, Data::TrackBinding { track, midiControls, midi->GetMute(), midi->GetSolo() });
        m_data->midiTracks.push_back(std::static_pointer_cast<const MidiTrack>(midi->shared_from_this()));
        if (m_data->trackMeters) {
            m_data->trackMeters->addTrack(*track, midi->GetId(), 2);
        }
        for (const auto& source : midi->GetClips()) {
            const auto start = edit.tempoSequence.toTime(tracktion::BeatPosition::fromBeats(source.start));
            const auto end = edit.tempoSequence.toTime(tracktion::BeatPosition::fromBeats(source.start + source.length));
            auto clip = track->insertMIDIClip(juce::String(source.name.utf8_string()), { start, end }, nullptr);
            if (!clip) {
                throw std::runtime_error("Cannot create a Tracktion MIDI clip.");
            }
            clip->setMidiChannel(tracktion::MidiChannel(source.channel));
            auto& sequence = clip->getSequence();
            for (const auto& note : source.notes) {
                sequence.addNote(note.pitch, tracktion::BeatPosition::fromBeats(note.start),
                                 tracktion::BeatDuration::fromBeats(note.length), note.velocity, 0, nullptr);
            }
            for (const auto& event : source.controllers) {
                sequence.addControllerEvent(tracktion::BeatPosition::fromBeats(event.beat), event.number, event.value << 7, nullptr);
            }
        }
    }
    if (routing) {
        std::map<std::string, int> busNumbers;
        for (const auto& [id, source] : buses) {
            const auto binding = m_data->bindings.find(source);
            if (binding == m_data->bindings.end()) {
                continue;
            }
            const int number = static_cast<int>(busNumbers.size());
            busNumbers.emplace(id, number);
            auto plugin = edit.getPluginCache().createNewPlugin(tracktion::AuxReturnPlugin::xmlTypeName, {});
            static_cast<tracktion::AuxReturnPlugin*>(plugin.get())->busNumber = number;
            binding->second.track->pluginList.insertPlugin(plugin, 0, nullptr);
        }
        for (auto& [source, binding] : m_data->bindings) {
            const auto& route = TrackRouting::Get(*source);
            const auto output = buses.find(route.output);
            if (output != buses.end()) {
                binding.track->getOutput().setOutputToTrack(m_data->bindings.at(output->second).track.get());
                m_data->routed = true;
            }
            tracktion::Plugin::Ptr preFaderAnchor;
            const auto* wave = track_cast<const WaveTrack*>(source);
            const bool preFaderMono = wave && wave->NChannels() == 1
                                      && std::any_of(route.sends.begin(), route.sends.end(),
                                                     [&](const auto& send) { return send.preFader && busNumbers.contains(send.bus); });
            if (preFaderMono) {
                auto plugin = edit.getPluginCache().createNewPlugin(tracktion::PatchBayPlugin::xmlTypeName, {});
                auto& patch = *static_cast<tracktion::PatchBayPlugin*>(plugin.get());
                while (patch.getNumWires() > 0) {
                    const auto* wire = patch.getWire(0);
                    patch.breakConnection(wire->sourceChannelIndex, wire->destChannelIndex);
                }
                patch.makeConnection(0, 0, 0.0f, nullptr);
                patch.makeConnection(0, 1, 0.0f, nullptr);
                binding.track->pluginList.insertPlugin(plugin, binding.track->pluginList.indexOf(binding.track->getVolumePlugin()),
                                                       nullptr);
                preFaderAnchor = plugin;
            }
            for (const auto& send : route.sends) {
                const auto bus = busNumbers.find(send.bus);
                if (bus == busNumbers.end()) {
                    continue;
                }
                auto plugin = edit.getPluginCache().createNewPlugin(tracktion::AuxSendPlugin::xmlTypeName, {});
                auto& nativeSend = *static_cast<tracktion::AuxSendPlugin*>(plugin.get());
                nativeSend.busNumber = bus->second;
                nativeSend.setGainDb(juce::Decibels::gainToDecibels(static_cast<float>(send.gain)));
                const int index = send.preFader ? binding.track->pluginList.indexOf(binding.track->getVolumePlugin()) : -1;
                binding.track->pluginList.insertPlugin(plugin, index, nullptr);
                if (send.preFader && !preFaderAnchor) {
                    preFaderAnchor = plugin;
                }
                m_data->routed = true;
            }
            if (preFaderAnchor) {
                for (auto& chain : m_data->effectChains) {
                    if (chain->group == source) {
                        chain->anchor = preFaderAnchor;
                    }
                }
            }
        }
    }
    for (const auto* source : requestedSources) {
        m_data->renderTracks.push_back(source);
    }
    for (const auto* source : midiSources) {
        m_data->renderTracks.push_back(source);
    }
    if (!midiSources.empty()) {
        std::map<long, size_t> positions;
        for (const auto* source : tracks) {
            positions.emplace(source->GetId(), positions.size());
        }
        const auto position = [&](const Track* source) {
            const auto found = positions.find(source->GetId());
            return found == positions.end() ? positions.size() : found->second;
        };
        std::stable_sort(m_data->renderTracks.begin(), m_data->renderTracks.end(), [&](const Track* left, const Track* right) {
            return position(left) < position(right);
        });
    }
    if (m_data->masterEffects) {
        m_data->addEffects(*m_data->masterEffects, nullptr, 2, edit.getMasterPluginList(), 0, role == Role::Playback);
    }
    if (role == Role::Playback) {
        m_data->observeEnvelopes();
        auto controls = edit.getPluginCache().createNewPlugin(AUDACITY_PLAYBACK_CONTROLS_PLUGIN_TYPE, {});
        m_data->playbackControls = audacityPlaybackControls(*controls);
        edit.getMasterPluginList().insertPlugin(controls, -1, nullptr);
    }
    m_data->updateRoutingSolo();
    edit.dispatchPendingUpdatesSynchronously();
    edit.updateMuteSoloStatuses();
    edit.getUndoManager().clearUndoHistory();
    if (role == Role::Playback) {
        m_data->trackSubscription = tracks.Subscribe([data = m_data.get()](const TrackListEvent& event) {
            data->updateTrackControls(event);
        });
    }
}

TracktionSession::~TracktionSession() = default;

tracktion::Edit& TracktionSession::edit() const
{
    return *m_data->edit;
}

bool TracktionSession::hasRouting() const
{
    return m_data->routed;
}

void TracktionSession::configureRender(unsigned channels, const MixerOptions::Downmix* channelMap, bool averageChannels)
{
    auto& data = *m_data;
    const bool monoMixdown = averageChannels && channelMap == nullptr;
    size_t inputChannels = 0;
    for (const auto* source : data.renderTracks) {
        if (!data.routed || !data.bindings.at(source).track->getOutput().getDestinationTrack()) {
            inputChannels += track_cast<const MidiTrack*>(source) ? 2 : source->NChannels();
        }
    }
    if (channelMap && (channelMap->GetNumChannels() != channels || channelMap->GetNumTracks() != inputChannels)) {
        channelMap = nullptr;
    }
    bool masterEffects = false;
    if (data.masterEffects && data.masterEffects->IsActive()) {
        for (size_t index = 0; index < data.masterEffects->GetStatesCount(); ++index) {
            const auto state = data.masterEffects->GetStateAt(index);
            masterEffects |= state->IsEnabled() && state->GetEffect() && state->GetSettings().has_value();
        }
    }
    if (data.routed) {
        size_t firstInput = 0;
        std::map<const Track*, size_t> rows;
        for (const auto* source : data.renderTracks) {
            if (!data.bindings.at(source).track->getOutput().getDestinationTrack()) {
                rows.emplace(source, firstInput);
                firstInput += track_cast<const MidiTrack*>(source) ? 2 : source->NChannels();
            }
            data.bindings.at(source).track->setSolo(false);
        }
        for (auto& [source, binding] : data.bindings) {
            binding.track->setSolo(false);
            if (binding.track->getOutput().getDestinationTrack()) {
                continue;
            }
            auto plugin = data.edit->getPluginCache().createNewPlugin(tracktion::PatchBayPlugin::xmlTypeName, {});
            auto& patch = *static_cast<tracktion::PatchBayPlugin*>(plugin.get());
            while (patch.getNumWires() > 0) {
                const auto* wire = patch.getWire(0);
                patch.breakConnection(wire->sourceChannelIndex, wire->destChannelIndex);
            }
            const auto row = rows.find(source);
            const size_t logicalChannels = track_cast<const MidiTrack*>(source) ? 2 : source->NChannels();
            for (unsigned input = 0; input < 2; ++input) {
                for (unsigned output = 0; output < channels; ++output) {
                    const bool mapped = channelMap && row != rows.end()
                                        ? channelMap->mMap[row->second + std::min<size_t>(input, logicalChannels - 1)][output]
                                        && (logicalChannels > 1 || channels == 1 || input == output % 2)
                                        : channels == 1 || input == output;
                    if (mapped) {
                        const float gain = channels == 1 && monoMixdown ? 0.5f : 1.0f;
                        patch.makeConnection(input, output, tracktion::gainToDb(gain), nullptr);
                    }
                }
            }
            binding.track->pluginList.insertPlugin(plugin, -1, nullptr);
        }
        data.setMasterRenderChannels(channels);
        data.edit->dispatchPendingUpdatesSynchronously();
        data.edit->updateMuteSoloStatuses();
        data.edit->getUndoManager().clearUndoHistory();
        return;
    }
    size_t firstInput = 0;
    for (const auto* source : data.renderTracks) {
        const auto* wave = track_cast<const WaveTrack*>(source);
        const size_t channelCount = wave ? wave->NChannels() : 2;
        auto& binding = data.bindings.at(source);
        auto& track = *binding.track;
        track.setMute(false);
        track.setSolo(false);
        const bool automated = data.automation && hasTrackAutomation(TrackAutomation::Get(*source));
        for (auto* plugin : track.pluginList.getPlugins()) {
            if (!automated && plugin->state == binding.controls) {
                plugin->removeFromParent();
                break;
            }
        }
        auto plugin = data.edit->getPluginCache().createNewPlugin(tracktion::PatchBayPlugin::xmlTypeName, {});
        auto& routing = *static_cast<tracktion::PatchBayPlugin*>(plugin.get());
        while (routing.getNumWires() > 0) {
            const auto* wire = routing.getWire(0);
            routing.breakConnection(wire->sourceChannelIndex, wire->destChannelIndex);
        }
        const bool automatedMono = automated && channelCount == 1;
        const size_t renderChannels = automatedMono ? 2 : channelCount;
        for (size_t input = 0; input < renderChannels; ++input) {
            for (unsigned output = 0; output < channels; ++output) {
                const bool mapped = !channelMap || channelMap->mMap[firstInput + (automatedMono ? 0 : input)][output];
                const bool connected = automatedMono ? mapped && (channels == 1 || input == output % 2)
                                       : channelMap ? mapped : channelCount == 1 || channels == 1 || input == output;
                if (!connected) {
                    continue;
                }
                float gain = wave && !automated ? wave->GetChannelVolume(channels == 1 ? input : output) : 1.0f;
                if (channels == 1 && (automatedMono || (monoMixdown && !masterEffects)) && renderChannels > 1) {
                    gain /= renderChannels;
                }
                if (gain != 0.0f) {
                    routing.makeConnection(input, output, tracktion::gainToDb(gain), nullptr);
                }
            }
        }
        track.pluginList.insertPlugin(plugin, -1, nullptr);
        firstInput += channelCount;
    }
    data.setMasterRenderChannels(channels);
    data.edit->dispatchPendingUpdatesSynchronously();
    data.edit->getUndoManager().clearUndoHistory();
}

bool TracktionSession::startPlayback()
{
    auto& transport = m_data->edit->getTransport();
    // Context allocation must let the audio callback advance.
    transport.ensureContextAllocated();
    const juce::ScopedLock lock(m_data->engine.getDeviceManager().deviceManager.getAudioCallbackLock());
    transport.play(false);
    return transport.isPlaying();
}

void TracktionSession::setPlaybackEnd(std::optional<double> time)
{
    const juce::ScopedLock lock(m_data->engine.getDeviceManager().deviceManager.getAudioCallbackLock());
    m_data->playbackControls->endTime.store(time.value_or(std::numeric_limits<double>::max()), std::memory_order_relaxed);
    m_data->playbackControls->endSampleOffset.store(-1, std::memory_order_release);
}

bool TracktionSession::updatePlaybackLoop(bool enabled, double startTime, double endTime, PlaybackLoopUpdate update)
{
    const juce::ScopedLock lock(m_data->engine.getDeviceManager().deviceManager.getAudioCallbackLock());
    auto& transport = m_data->edit->getTransport();
    const double position = transport.getPosition().inSeconds();
    const double allowance = update == PlaybackLoopUpdate::RegionChanged ? 0.5 : 0.0;
    const bool looping = enabled && startTime < endTime && position <= endTime + allowance;
    const auto range = looping ? tracktion::TimeRange { tracktion::TimePosition::fromSeconds(startTime),
                                                        tracktion::TimePosition::fromSeconds(endTime) } : tracktion::TimeRange {};
    const double rate = m_data->engine.getDeviceManager().getSampleRate();
    auto sampleRange = tracktion::toSamples(range, rate);
    const double silentLoopStart = looping && sampleRange.isEmpty()
                                   ? tracktion::TimePosition::fromSamples(sampleRange.getStart(), rate).inSeconds()
                                   : std::numeric_limits<double>::max();
    m_data->playbackControls->silentLoopStart.store(silentLoopStart, std::memory_order_relaxed);
    if (looping && sampleRange.isEmpty()) {
        sampleRange.setEnd(sampleRange.getStart() + 1);
    }
    const bool changed = transport.looping != looping || (looping && transport.getLoopRange() != range);
    const bool wrapToStart = looping && changed && position >= endTime;
    if (looping) {
        transport.setLoopRange(range);
        if (wrapToStart) {
            transport.setPosition(range.getStart());
        }
    }
    transport.looping = looping;
    auto* context = transport.getCurrentPlaybackContext();
    auto* playHead = context ? context->getNodePlayHead() : nullptr;
    if (changed && playHead && transport.isPlaying()) {
        const auto previousPosition = wrapToStart ? tracktion::toSamples(range.getStart(), rate) : playHead->getPosition();
        playHead->setLoopRange(looping, sampleRange, false);
        if (looping && previousPosition < tracktion::toSamples(range.getStart(), rate)) {
            playHead->setRollInToLoop(previousPosition);
        } else {
            playHead->setPosition(previousPosition);
        }
    }
    return looping;
}

void TracktionSession::setPlaybackVolume(float volume)
{
    m_data->playbackControls->volume.store(volume, std::memory_order_relaxed);
}

void TracktionSession::setPlaybackSpeed(double ratio)
{
    auto& transport = m_data->edit->getTransport();
    transport.ensureContextAllocated();
    transport.getCurrentPlaybackContext()->setPlaybackSpeed(ratio);
}

void TracktionSession::setLeadInEnd(const TransportSequences& sequences, double time)
{
    const auto end = tracktion::TimePosition::fromSeconds(time);
    for (const auto& sequence : sequences.leadInTimeSequences) {
        const auto* source = dynamic_cast<const WaveTrack*>(sequence->FindChannelGroup());
        const auto binding = m_data->bindings.find(source);
        if (binding == m_data->bindings.end()) {
            continue;
        }
        const auto clips = binding->second.track->getClips();
        for (auto* clip : clips) {
            const auto range = clip->getPosition().time;
            if (range.getStart() >= end) {
                clip->removeFromParent();
            } else if (range.getEnd() > end) {
                clip->setEnd(end, true);
            }
        }
    }
    m_data->edit->dispatchPendingUpdatesSynchronously();
    m_data->edit->getUndoManager().clearUndoHistory();
}

std::optional<int> TracktionSession::playbackEndSampleOffset() const
{
    const int offset = m_data->playbackControls->endSampleOffset.load(std::memory_order_acquire);
    return offset < 0 ? std::nullopt : std::make_optional(offset);
}

std::shared_ptr<const AudacityPlaybackControls> TracktionSession::playbackControls() const
{
    return m_data->playbackControls;
}

std::shared_ptr<TracktionTrackMeters> TracktionSession::trackMeters() const
{
    return m_data->trackMeters;
}

tracktion::AudioTrack* TracktionSession::instrumentTrack(const MidiTrack& source) const
{
    const auto binding = m_data->bindings.find(&source);
    return binding == m_data->bindings.end() ? nullptr : binding->second.track.get();
}
