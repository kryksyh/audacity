/*
* Audacity: A Digital Audio Editor
*/
#include <tracktion_engine/tracktion_engine.h>

#include "tracktionrecording.h"
#include "tracktionruntime.h"

#include <cmath>
#include <limits>

#include "au3-mixer/AudioIOSequences.h"
#include "au3-exceptions/AudacityException.h"

using namespace au::tracktionaudio;

struct TracktionRecording::Data
{
    struct DestinationWriter {
        DestinationWriter(std::shared_ptr<RecordableSequence> sequence, std::unique_ptr<juce::AudioFormatReader> input,
                          int64_t offset, int firstInput, size_t totalChannels, double leadIn, double latencyCorrection,
                          const Options& options)
            : destination(std::move(sequence)), reader(input.get()), dataOffset(offset),
            source(input.release(), true), resampler(&source, false, reader->numChannels),
            audio(reader->numChannels, 4096), mono(4096), crossfaded(4096), firstChannel(firstInput), destinationChannels(totalChannels),
            crossfadeData(options.crossfadeData)
        {
            resampler.setResamplingRatio(reader->sampleRate / destination->GetRate());
            resampler.prepareToPlay(audio.getNumSamples(), destination->GetRate());
            const double offsetSeconds = leadIn - latencyCorrection;
            firstFrame = static_cast<int64_t>(std::floor(std::max(0.0, offsetSeconds) * reader->sampleRate));
            source.setNextReadPosition(firstFrame);
            const long double limit = options.duration ? std::round(static_cast<long double>(*options.duration) * destination->GetRate())
                                      : std::numeric_limits<int64_t>::max();
            frameLimit = static_cast<int64_t>(std::min(limit, static_cast<long double>(std::numeric_limits<int64_t>::max())));
            initialSilenceFrames = static_cast<int64_t>(std::floor(std::max(0.0, -offsetSeconds) * destination->GetRate()));
        }

        void append(int64_t availableFrames, bool final)
        {
            reader->lengthInSamples = availableFrames;
            const double ratio = destination->GetRate() / reader->sampleRate;
            const bool resample = destination->GetRate() != reader->sampleRate;
            const auto captureFrames = std::max<int64_t>(0, availableFrames - firstFrame);
            const auto safeFrames = !final && resample ? std::max<int64_t>(0, captureFrames - 8) : captureFrames;
            const auto outputFrames = final ? static_cast<int64_t>(std::llround(safeFrames * ratio))
                                      : static_cast<int64_t>(std::floor(safeFrames * ratio));
            const auto frames = std::min(frameLimit, initialSilenceFrames + outputFrames);
            while (position < frames) {
                const bool silence = position < initialSilenceFrames;
                const auto remaining = silence ? std::min(frames, initialSilenceFrames) - position : frames - position;
                const int count = static_cast<int>(std::min<int64_t>(audio.getNumSamples(), remaining));
                const juce::AudioSourceChannelInfo block(&audio, 0, count);
                if (silence) {
                    block.clearActiveBufferRegion();
                } else if (resample) {
                    resampler.getNextAudioBlock(block);
                } else {
                    source.getNextAudioBlock(block);
                }
                for (size_t channel = 0; channel < destination->NChannels(); ++channel) {
                    const float* samples = nullptr;
                    if (reader->numChannels == 2 && destinationChannels == 1) {
                        for (int frame = 0; frame < count; ++frame) {
                            mono[frame] = (audio.getSample(0, frame) + audio.getSample(1, frame)) * 0.5f;
                        }
                        samples = mono.data();
                    } else {
                        const int sourceChannel = reader->numChannels == 1 && destinationChannels == 2
                                                  ? 0 : static_cast<int>(firstChannel + channel);
                        samples = audio.getReadPointer(sourceChannel);
                    }
                    const size_t crossfadeChannel = firstChannel + channel;
                    if (crossfadeChannel < crossfadeData.size()
                        && position < static_cast<int64_t>(crossfadeData[crossfadeChannel].size())) {
                        const auto& previous = crossfadeData[crossfadeChannel];
                        std::copy_n(samples, count, crossfaded.data());
                        const int begin = static_cast<int>(std::clamp<int64_t>(initialSilenceFrames - position, 0, count));
                        for (int frame = begin; frame < count && position + frame < static_cast<int64_t>(previous.size()); ++frame) {
                            const auto index = position + frame;
                            const double ratio = static_cast<double>(index) / previous.size();
                            crossfaded[frame] = ratio * samples[frame] + (1.0 - ratio) * previous[index];
                        }
                        samples = crossfaded.data();
                    }
                    destination->Append(channel, reinterpret_cast<constSamplePtr>(samples), floatSample, count, 1, narrowestSampleFormat);
                }
                position += count;
            }
        }

        std::shared_ptr<RecordableSequence> destination;
        juce::AudioFormatReader* reader;
        int64_t dataOffset;
        juce::AudioFormatReaderSource source;
        juce::ResamplingAudioSource resampler;
        juce::AudioBuffer<float> audio;
        std::vector<float> mono;
        std::vector<float> crossfaded;
        int firstChannel;
        size_t destinationChannels;
        const std::vector<std::vector<float> >& crossfadeData;
        int64_t position = 0;
        int64_t firstFrame = 0;
        int64_t frameLimit = 0;
        int64_t initialSilenceFrames = 0;
    };

    Data(tracktion::Edit& sourceEdit, std::vector<std::shared_ptr<RecordableSequence> > sequences, int channels,
         const std::string& directory, Options recordingOptions)
        : edit(sourceEdit), destinations(std::move(sequences)), inputChannels(channels), options(std::move(recordingOptions)),
        directory(juce::File(directory).getChildFile("recording-" + juce::Uuid().toString()))
    {
    }

    ~Data()
    {
        writers.clear();
        if (started) {
            edit.getTransport().stop(!preserveFiles, true, false);
        }
        if (target) {
            edit.deleteTrack(target.get());
            target.reset();
        }
        if (input) {
            input->setFilenameMask(previousFilename);
            input->setOutputFormat(previousFormat);
            input->setBitDepth(previousBitDepth);
            input->setRecordAdjustmentOverride(previousAdjustment);
            input->setMonitorMode(previousMonitorMode);
            input->setInputGainDb(previousGainDb);
        }
        if (previousLayout) {
            edit.engine.getDeviceManager().applyWaveDeviceLayout(*previousLayout);
            edit.engine.getDeviceManager().dispatchPendingUpdates();
        }
        if (file != juce::File()) {
            edit.engine.getAudioFileManager().releaseFile(tracktion::AudioFile(edit.engine, file));
        }
        if (!preserveFiles) {
            directory.deleteRecursively();
        }
    }

    std::string transferToProject(bool final)
    {
        // Do not repeat an append after only some channels were stored.
        if (transferError.empty()) {
            transferError = GuardedCall<std::string>([this, final] {
                return appendToProject(final);
            }, MakeSimpleGuard(std::string("Cannot store the recorded audio.")));
        }
        return transferError;
    }

    void finishProject()
    {
        for (const auto& destination : destinations) {
            GuardedCall([&] {
                destination->Flush();
            }, [this](AudacityException*) {
                if (transferError.empty()) {
                    transferError = "Cannot store the recorded audio.";
                }
            });
        }
        if (!transferError.empty()) {
            for (const auto& destination : destinations) {
                GuardedCall([&] { destination->RepairChannels(); });
            }
        }
    }

    std::string appendToProject(bool final)
    {
        auto* format = edit.engine.getAudioFileFormatManager().getWavFormat();
        int64_t finalFrames = 0;
        if (final) {
            std::unique_ptr<juce::AudioFormatReader> reader(format->createReaderFor(file.createInputStream().release(), true));
            if (!reader) {
                return "Cannot read the recorded audio.";
            }
            finalFrames = reader->lengthInSamples;
        }
        if (writers.empty()) {
            size_t destinationChannels = 0;
            for (const auto& destination : destinations) {
                destinationChannels += destination->NChannels();
            }
            int firstChannel = 0;
            std::vector<std::unique_ptr<DestinationWriter> > prepared;
            for (const auto& destination : destinations) {
                auto stream = file.createInputStream();
                if (!stream || stream->getTotalLength() == 0) {
                    return final ? "Cannot read the recorded audio." : std::string();
                }
                auto* input = stream.get();
                std::unique_ptr<juce::AudioFormatReader> reader(format->createReaderFor(stream.release(), true));
                if (!reader) {
                    return final ? "Cannot read the recorded audio." : std::string();
                }
                const int bytesPerFrame = static_cast<int>(reader->numChannels) * reader->bitsPerSample / 8;
                // Tracktion writes the WAV data chunk after the recording metadata.
                const int64_t offset = input->getPosition() - reader->lengthInSamples * bytesPerFrame;
                prepared.push_back(std::make_unique<DestinationWriter>(destination, std::move(reader), offset,
                                                                       firstChannel, destinationChannels, leadInTime, latencyCorrection,
                                                                       options));
                firstChannel += destination->NChannels();
            }
            writers = std::move(prepared);
        }
        for (const auto& writer : writers) {
            const int bytesPerFrame = static_cast<int>(writer->reader->numChannels) * writer->reader->bitsPerSample / 8;
            const auto available = final ? finalFrames : std::max<int64_t>(0, (file.getSize() - writer->dataOffset) / bytesPerFrame);
            writer->append(available, final);
        }
        return {};
    }

    tracktion::Edit& edit;
    std::vector<std::shared_ptr<RecordableSequence> > destinations;
    int inputChannels;
    Options options;
    juce::File directory;
    juce::File file;
    tracktion::AudioTrack::Ptr target;
    tracktion::WaveInputDevice* input = nullptr;
    std::unique_ptr<juce::XmlElement> previousLayout;
    juce::String previousFilename, previousFormat;
    int previousBitDepth = 0;
    std::optional<tracktion::TimeDuration> previousAdjustment;
    float previousGainDb = 0.0f;
    tracktion::InputDevice::MonitorMode previousMonitorMode = tracktion::InputDevice::MonitorMode::off;
    bool started = false;
    bool preserveFiles = false;
    std::string transferError;
    double leadInTime = 0.0;
    double latencyCorrection = 0.0;
    std::vector<std::unique_ptr<DestinationWriter> > writers;
};

TracktionRecording::TracktionRecording(tracktion::Edit& edit, std::vector<std::shared_ptr<RecordableSequence> > destinations,
                                       int inputChannels, const std::string& directory, Options options)
    : m_data(std::make_unique<Data>(edit, std::move(destinations), inputChannels, directory, std::move(options)))
{
}

TracktionRecording::~TracktionRecording() = default;

std::string TracktionRecording::update()
{
    auto& data = *m_data;
    if (!data.started || data.file == juce::File()) {
        return {};
    }
    data.preserveFiles = true;
    const auto error = data.transferToProject(false);
    data.preserveFiles = !error.empty();
    return error;
}

std::string TracktionRecording::start(double startTime)
{
    auto& data = *m_data;
    data.leadInTime = std::clamp(data.options.leadInTime, 0.0, std::max(0.0, startTime));
    size_t channels = 0;
    for (const auto& destination : data.destinations) {
        channels += destination->NChannels();
    }
    if (data.inputChannels < 1 || channels == 0
        || (static_cast<size_t>(data.inputChannels) != channels && (data.inputChannels > 2 || channels > 2))) {
        return "The recording channels do not match the audio input.";
    }
    const auto result = data.directory.createDirectory();
    if (result.failed()) {
        return result.getErrorMessage().toStdString();
    }
    auto& manager = data.edit.engine.getDeviceManager();
    data.previousLayout = std::make_unique<juce::XmlElement>(manager.getCurrentWaveDeviceLayout());
    manager.setAllWaveInputsToNumChannels(data.inputChannels);
    manager.dispatchPendingUpdates();
    const auto inputs = manager.getWaveInputDevices();
    const auto input = std::find_if(inputs.begin(), inputs.end(), [&data](const auto* device) {
        return device->getChannels().getNumChannels() == data.inputChannels
               && device->deviceDescription.getDeviceChannelRange().first == 0;
    });
    if (input == inputs.end()) {
        return "The audio input is not available.";
    }
    const auto deviceID = (*input)->getDeviceID();
    manager.setDeviceEnabled(**input, true);
    manager.dispatchPendingUpdates();
    const auto enabledInputs = manager.getWaveInputDevices();
    const auto enabledInput = std::find_if(enabledInputs.begin(), enabledInputs.end(), [&deviceID](const auto* device) {
        return device->getDeviceID() == deviceID;
    });
    if (enabledInput == enabledInputs.end()) {
        return "The audio input is not available.";
    }
    data.input = *enabledInput;
    data.previousFilename = data.input->getFilenameMask();
    data.previousFormat = data.input->getOutputFormat();
    data.previousBitDepth = data.input->getBitDepth();
    data.previousAdjustment = data.input->getRecordAdjustmentOverride();
    data.previousMonitorMode = data.input->getMonitorMode();
    data.previousGainDb = data.input->getInputGainDb();
    data.input->setFilenameMask(data.directory.getChildFile("capture_%take%").getFullPathName());
    data.input->setOutputFormat(data.edit.engine.getAudioFileFormatManager().getWavFormat()->getFormatName());
    data.input->setBitDepth(32);
    data.latencyCorrection = data.options.automaticLatencyCompensation ? -manager.getRecordAdjustmentMs() / 1000.0
                             : data.options.latencyCompensationMs / 1000.0;
    // Apply document latency correction outside the audio callback without Tracktion's setting limits.
    data.input->setRecordAdjustmentOverride(tracktion::TimeDuration());
    data.input->setMonitorMode(data.options.monitorInput ? tracktion::InputDevice::MonitorMode::on
                               : tracktion::InputDevice::MonitorMode::off);
    TracktionRuntime::setInputGain(*data.input, data.options.inputGain);
    data.target = data.edit.insertNewAudioTrack(tracktion::TrackInsertPoint::getEndOfTracks(data.edit), nullptr);
    if (!data.target) {
        return "Cannot create a recording track.";
    }
    auto& transport = data.edit.getTransport();
    transport.setPosition(tracktion::TimePosition::fromSeconds(startTime - data.leadInTime));
    transport.ensureContextAllocated();
    auto* context = transport.getCurrentPlaybackContext();
    auto* instance = context ? context->getInputFor(data.input) : nullptr;
    if (!instance) {
        return "Cannot connect the recording input.";
    }
    const auto assignment = instance->setTarget(data.target->itemID, true, nullptr);
    if (!assignment) {
        return assignment.error().toStdString();
    }
    (*assignment)->recordEnabled = true;
    instance->dispatchPendingRecordingUpdates();
    data.edit.dispatchPendingUpdatesSynchronously();
    transport.looping = false;
    transport.record(false);
    if (!transport.isRecording()) {
        return "Cannot start recording.";
    }
    data.started = true;
    data.file = instance->getRecordingFile(data.target->itemID);
    return {};
}

std::string TracktionRecording::stop()
{
    auto& data = *m_data;
    if (!data.started) {
        return {};
    }
    data.edit.getTransport().stop(false, true, false);
    data.started = false;
    data.preserveFiles = true;
    if (data.file.existsAsFile()) {
        data.transferToProject(true);
    }
    data.finishProject();
    if (!data.transferError.empty()) {
        return data.transferError + " " + data.file.getFullPathName().toStdString();
    }
    data.preserveFiles = false;
    return {};
}

bool TracktionRecording::hasReachedEnd() const
{
    return m_data->options.duration && !m_data->writers.empty()
           && std::all_of(m_data->writers.begin(), m_data->writers.end(), [](const auto& writer) {
        return writer->position >= writer->frameLimit;
    });
}
