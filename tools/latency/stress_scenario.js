/*
 * Audacity: A Digital Audio Editor
 *
 * Dropout floor (L10): imports stress.wav on several tracks, puts realtime
 * effects on each, and plays for a fixed time while tracing. Run through
 * tools/latency/run_stress.sh.
 */

var Perf = require("Audacity.Perf")

// stress.wav is 120 s long
var RESTART_MS = 110000

var testCase = {
    name: "Perf: dropout floor",
    description: "Several tracks with realtime effects, played for a fixed time",
    steps: [
        {
            name: "Open signals", func: function () {
                var tracks = parseInt(Perf.env("AU_LAT_TRACKS") || "8")
                for (var i = 0; i < tracks; ++i) {
                    Perf.importAudio(Perf.env("AU_LAT_SIGNAL"))
                    api.testflow.sleep(3000)
                }
            }
        },
        {
            name: "Configure audio", func: function () {
                var bufferMs = Perf.env("AU_LAT_BUFFER_MS")
                var ok = Perf.configureAudio(bufferMs ? parseFloat(bufferMs) : -1,
                                             Perf.env("AU_LAT_OUTPUT"), Perf.env("AU_LAT_INPUT"))
                api.log.info("latency", "configureAudio " + (ok ? "ok" : "FAILED") + ": " + Perf.audioConfiguration())
                var tracks = parseInt(Perf.env("AU_LAT_TRACKS") || "8")
                var effects = (Perf.env("AU_LAT_EFFECTS") || "").split(",").filter(function (e) { return e.length > 0 })
                var added = 0
                for (var t = 0; t < tracks; ++t) {
                    for (var k = 0; k < effects.length; ++k) {
                        added += Perf.addRealtimeEffect(t, effects[k]) ? 1 : 0
                    }
                }
                api.log.info("latency", "addRealtimeEffect " + added + "/" + tracks * effects.length)
                api.testflow.sleep(1000)
            }
        },
        {
            name: "Play", func: function () {
                var totalMs = 1000 * parseFloat(Perf.env("AU_LAT_SECONDS") || "120")
                Perf.startTrace()
                api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
                api.dispatcher.dispatch("action://playback/toggle-play-stop")
                for (var played = 0; played < totalMs; played += RESTART_MS) {
                    var slice = Math.min(RESTART_MS, totalMs - played)
                    api.testflow.sleep(slice)
                    if (played + slice < totalMs) {
                        api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=true")
                    }
                }
                api.log.info("latency", "audioEngineHealth " + JSON.stringify(Perf.audioEngineHealth()))
                api.dispatcher.dispatch("action://playback/toggle-play-stop")
                Perf.stopTrace(Perf.env("AU_LAT_TRACE"))
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
