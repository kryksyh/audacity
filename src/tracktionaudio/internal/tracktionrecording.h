/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>

struct RecordableSequence;

namespace tracktion {
inline namespace engine {
class Edit;
}
}

namespace au::tracktionaudio {
class TracktionRecording
{
public:
    struct Options {
        bool monitorInput = false;
        bool automaticLatencyCompensation = true;
        float inputGain = 1.0f;
        double latencyCompensationMs = 0.0;
        double leadInTime = 0.0;
        std::optional<double> duration;
        std::vector<std::vector<float> > crossfadeData;
    };

    TracktionRecording(tracktion::Edit& edit, std::vector<std::shared_ptr<RecordableSequence> > destinations, int inputChannels,
                       const std::string& directory, Options options);
    ~TracktionRecording();

    std::string start(double startTime);
    std::string update();
    std::string stop();
    bool hasReachedEnd() const;

private:
    struct Data;
    std::unique_ptr<Data> m_data;
};
}
