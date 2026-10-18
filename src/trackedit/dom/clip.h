/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <vector>

#include "global/types/string.h"
#include "../trackedittypes.h"

namespace au::trackedit {
using ClipVersion = int64_t;

struct Clip {
    struct NotePreview {
        double start = 0.0;
        double length = 0.0;
        int pitch = 60;
    };
    ClipKey key;
    ClipVersion clipVersion = -1;

    muse::String title;
    ClipColorIndex colorIndex = CLIP_COLOR_INDEX_NONE;
    bool isAutoColor = true;
    int groupId = -1;
    double startTime = 0.0;
    double endTime = 0.0;
    bool stereo = false;
    bool isMidi = false;
    std::vector<NotePreview> midiNotes;

    int pitch = 0;
    double speed = 0.0;
    bool optimizeForVoice = false;

    bool stretchToMatchTempo = false;

    inline bool isValid() const { return key.isValid(); }
};

using Clips = std::vector<Clip>;
}
