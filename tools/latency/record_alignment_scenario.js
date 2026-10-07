/*
 * Audacity: A Digital Audio Editor
 *
 * Record alignment (L4): opens bursts.wav, records it back on a new track
 * through a loopback (cable or speaker to microphone) and exports every track.
 * `pa_rtl --align` then finds how far the recorded bursts sit from the
 * originals on the timeline. Run through tools/latency/run_record_alignment.sh.
 */

var Perf = require("Audacity.Perf")

var testCase = {
    name: "Perf: record alignment",
    description: "Overdub a burst train through a loopback and export the tracks",
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
                var autoCompensation = Perf.env("AU_LAT_AUTO_COMPENSATION")
                var ok = Perf.configureAudio(bufferMs ? parseFloat(bufferMs) : -1,
                                             Perf.env("AU_LAT_OUTPUT"), Perf.env("AU_LAT_INPUT"),
                                             autoCompensation ? parseInt(autoCompensation) : -1)
                api.log.info("latency", "configureAudio " + (ok ? "ok" : "FAILED") + ": " + Perf.audioConfiguration())
                var effectId = Perf.env("AU_LAT_EFFECT")
                if (effectId) {
                    var added = Perf.addRealtimeEffect(0, effectId)
                    api.log.info("latency", "addRealtimeEffect " + effectId + ": " + (added ? "ok" : "FAILED"))
                }
                api.testflow.sleep(1000)
            }
        },
        {
            name: "Record", func: function () {
                Perf.startTrace()
                api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
                api.testflow.sleep(500)
                api.dispatcher.dispatch("record-on-new-track")
                api.testflow.sleep(8000)
                api.dispatcher.dispatch("action://record/stop")
                api.testflow.sleep(2000)
                Perf.stopTrace(Perf.env("AU_LAT_TRACE"))
            }
        },
        {
            name: "Export", func: function () {
                var ok = Perf.exportTracks(Perf.env("AU_LAT_EXPORT_DIR"))
                api.log.info("latency", "exportTracks " + (ok ? "ok" : "FAILED"))
                api.testflow.sleep(2000)
            }
        }
    ]
}

function main() {
    api.testflow.setInterval(300)
    api.testflow.runTestCase(testCase)
}
