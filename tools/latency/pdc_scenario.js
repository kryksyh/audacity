/*
 * Audacity: A Digital Audio Editor
 *
 * Plugin delay compensation: plays click_a.wav and click_b.wav as two tracks,
 * with a latent effect on the second one. When the tracks are aligned, the two
 * output onsets are exactly 0.5 s apart: after a start, after a seek, and after
 * the effect is added during play. Run through tools/latency/run_pdc_scenario.sh.
 */

var Perf = require("Audacity.Perf")

var ROUNDS = 3

function play(from) {
    api.dispatcher.dispatch("action://playback/seek?seekTime=" + from + "&triggerPlay=false")
    api.dispatcher.dispatch("action://playback/toggle-play-stop")
}

function stop() {
    api.dispatcher.dispatch("action://playback/toggle-play-stop")
    api.testflow.sleep(500)
}

var testCase = {
    name: "Perf: plugin delay compensation",
    description: "Two click tracks, one with a latent effect, traced",
    steps: [
        {
            name: "Open signals", func: function () {
                Perf.importAudio(Perf.env("AU_LAT_SIGNAL_A"))
                api.testflow.sleep(3000)
                Perf.importAudio(Perf.env("AU_LAT_SIGNAL_B"))
                api.testflow.sleep(3000)
            }
        },
        {
            name: "Configure audio", func: function () {
                var bufferMs = Perf.env("AU_LAT_BUFFER_MS")
                var ok = Perf.configureAudio(bufferMs ? parseFloat(bufferMs) : -1,
                                             Perf.env("AU_LAT_OUTPUT"), Perf.env("AU_LAT_INPUT"))
                api.log.info("latency", "configureAudio " + (ok ? "ok" : "FAILED") + ": " + Perf.audioConfiguration())
                api.log.info("latency", "addRealtimeEffect " + Perf.addRealtimeEffect(1, Perf.env("AU_LAT_EFFECT")))
                api.testflow.sleep(1000)
            }
        },
        {
            name: "Scenario", func: function () {
                var effectId = Perf.env("AU_LAT_EFFECT")
                Perf.startTrace()
                api.testflow.sleep(1000)
                for (var i = 0; i < ROUNDS; ++i) {
                    Perf.mark("pdc: play")
                    play(0)
                    api.testflow.sleep(2000)
                    stop()

                    Perf.mark("pdc: seek")
                    play(0)
                    api.testflow.sleep(300)
                    api.dispatcher.dispatch("action://playback/seek?seekTime=0.2&triggerPlay=true")
                    api.testflow.sleep(2000)
                    stop()

                    Perf.mark("pdc: effect added during play")
                    play(0)
                    api.testflow.sleep(300)
                    Perf.addRealtimeEffect(1, effectId)
                    api.testflow.sleep(2000)
                    stop()
                    Perf.removeRealtimeEffects(1)
                    Perf.addRealtimeEffect(1, effectId)
                }
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
