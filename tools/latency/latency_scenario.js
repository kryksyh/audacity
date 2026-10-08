/*
 * Audacity: A Digital Audio Editor
 *
 * Latency scenario: opens control.wav (tone / silence / tone), then repeats
 * play, mute, unmute, seek into silence, seek into tone and stop while tracing.
 * Each action is preceded by an "expect sound: ..." or "expect silence: ..."
 * mark; analyze_trace.py finds the output peak step that follows it.
 * Run through tools/latency/run_latency_scenario.sh.
 */

var Perf = require("Audacity.Perf")

// Without the requested devices the app would play on the system default
// output; the steps after "Configure audio" then do nothing
var configured = false

var ROUNDS = 5
// Longer than the slowest change seen with 100 ms buffers, so steps never overlap
var DWELL_MS = 1500

function step(expect, action) {
    if (expect) {
        Perf.mark(expect)
    }
    api.dispatcher.dispatch(action)
    api.testflow.sleep(DWELL_MS)
}

var testCase = {
    name: "Perf: latency scenario",
    description: "Transport and control actions over a tone/silence signal, traced",
    steps: [
        {
            name: "Open signal", func: function () {
                Perf.importAudio(Perf.env("AU_LAT_SIGNAL"))
                api.testflow.sleep(5000)
            }
        },
        {
            name: "Configure audio", func: function () {
                var bufferMs = Perf.env("AU_LAT_BUFFER_MS")
                configured = Perf.configureAudio(bufferMs ? parseFloat(bufferMs) : -1,
                                             Perf.env("AU_LAT_OUTPUT"), Perf.env("AU_LAT_INPUT"))
                api.log.info("latency", "configureAudio " + (configured ? "ok" : "FAILED") + ": " + Perf.audioConfiguration())
                if (!configured) {
                    return
                }
                // Load for L10: AU_LAT_EFFECT_COUNT copies of AU_LAT_EFFECT on the track
                var effectId = Perf.env("AU_LAT_EFFECT")
                var count = effectId ? parseInt(Perf.env("AU_LAT_EFFECT_COUNT") || "1") : 0
                var added = 0
                for (var i = 0; i < count; ++i) {
                    added += Perf.addRealtimeEffect(0, effectId) ? 1 : 0
                }
                if (count) {
                    api.log.info("latency", "addRealtimeEffect " + added + "/" + count + " " + effectId)
                }
                api.testflow.sleep(1000)
            }
        },
        {
            name: "Scenario", func: function () {
                if (!configured) {
                    return
                }
                Perf.startTrace()
                api.testflow.sleep(1000)
                for (var i = 0; i < ROUNDS; ++i) {
                    step("", "action://playback/seek?seekTime=1&triggerPlay=false")
                    step("expect sound: play", "action://playback/toggle-play-stop")
                    step("expect silence: mute", "mute-all-tracks")
                    step("expect sound: unmute", "unmute-all-tracks")
                    step("expect silence: seek into silence", "action://playback/seek?seekTime=12&triggerPlay=true")
                    step("expect sound: seek into tone", "action://playback/seek?seekTime=3&triggerPlay=true")
                    step("expect silence: stop", "action://playback/toggle-play-stop")
                }
                // The tone ends at exactly 10 s: analyze_trace.py compares the playhead
                // crossing with the moment the silence is audible
                step("", "action://playback/seek?seekTime=8.5&triggerPlay=false")
                step("", "action://playback/toggle-play-stop")
                api.testflow.sleep(2000)
                step("", "action://playback/toggle-play-stop")
                Perf.stopTrace(Perf.env("AU_LAT_TRACE"))
                api.log.info("latency", "audioEngineHealth " + JSON.stringify(Perf.audioEngineHealth()))
            }
        },
        {
            name: "Quit", func: function () {
                api.dispatcher.dispatch("quit")
            }
        }
    ]
}

function main() {
    api.testflow.setInterval(300)
    api.testflow.runTestCase(testCase)
}
