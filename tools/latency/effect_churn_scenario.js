/*
 * Audacity: A Digital Audio Editor
 *
 * Stress for the realtime effect lists: adds, replaces and removes effects in a
 * loop while playing. Meant for an ASan build; run through
 * tools/latency/run_effect_churn.sh.
 */

var Perf = require("Audacity.Perf")

var testCase = {
    name: "Perf: effect churn during playback",
    description: "Add, replace and remove realtime effects while playing",
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
                Perf.configureAudio(bufferMs ? parseFloat(bufferMs) : -1, Perf.env("AU_LAT_OUTPUT"), Perf.env("AU_LAT_INPUT"))
                api.log.info("latency", "configureAudio: " + Perf.audioConfiguration())
                api.testflow.sleep(1000)
            }
        },
        {
            name: "Churn", func: function () {
                var effectId = Perf.env("AU_LAT_EFFECT")
                var rounds = parseInt(Perf.env("AU_LAT_ROUNDS") || "100")
                api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
                api.dispatcher.dispatch("action://playback/toggle-play-stop")
                api.testflow.sleep(500)
                var added = 0
                var replaced = 0
                var removed = 0
                // Track 0 and the master (-1); the audio callback processes both
                for (var i = 0; i < rounds; ++i) {
                    for (var k = 0; k < 3; ++k) {
                        added += Perf.addRealtimeEffect(0, effectId) ? 1 : 0
                        added += Perf.addRealtimeEffect(-1, effectId) ? 1 : 0
                    }
                    api.testflow.sleep(20)
                    replaced += Perf.replaceRealtimeEffect(0, 1, effectId) ? 1 : 0
                    replaced += Perf.replaceRealtimeEffect(-1, 1, effectId) ? 1 : 0
                    api.testflow.sleep(20)
                    removed += Perf.removeRealtimeEffects(0)
                    removed += Perf.removeRealtimeEffects(-1)
                    api.testflow.sleep(20)
                }
                api.dispatcher.dispatch("action://playback/toggle-play-stop")
                api.log.info("latency", "effectChurn rounds " + rounds + ", added " + added + ", replaced " + replaced
                             + ", removed " + removed + ", health " + JSON.stringify(Perf.audioEngineHealth()))
            }
        }
    ]
}

function main() {
    api.testflow.setInterval(10)
    api.testflow.runTestCase(testCase)
}
