/*
 * Audacity: A Digital Audio Editor
 *
 * Runs the built-in loopback latency measurement once and logs the result.
 * Run through tools/latency/run_measure.sh.
 */

var Perf = require("Audacity.Perf")

var testCase = {
    name: "Perf: loopback latency measurement",
    description: "Measure the round trip with the built-in measurement",
    steps: [
        {
            name: "Configure audio", func: function () {
                var bufferMs = Perf.env("AU_LAT_BUFFER_MS")
                var ok = Perf.configureAudio(bufferMs ? parseFloat(bufferMs) : -1,
                                             Perf.env("AU_LAT_OUTPUT"), Perf.env("AU_LAT_INPUT"))
                api.log.info("latency", "configureAudio " + (ok ? "ok" : "FAILED") + ": " + Perf.audioConfiguration())
                api.testflow.sleep(1000)
            }
        },
        {
            name: "Measure", func: function () {
                Perf.startLatencyMeasurement()
                for (var i = 0; i < 100 && Object.keys(Perf.latencyMeasurement()).length === 0; ++i) {
                    api.testflow.sleep(100)
                }
                api.log.info("latency", "latencyMeasurement " + JSON.stringify(Perf.latencyMeasurement()))
            }
        }
    ]
}

function main() {
    api.testflow.setInterval(300)
    api.testflow.runTestCase(testCase)
}
