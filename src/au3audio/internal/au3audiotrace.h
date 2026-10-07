/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

namespace au::au3audio {
// Routes au3 AudioIO diagnostics into au::perf::Tracer. The audio callback
// records are drained on each tracer flush into an "Audio callback" lane.
void installAudioIOTrace();
}
