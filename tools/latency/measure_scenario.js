/*
 * Audacity: A Digital Audio Editor
 *
 * Runs the built-in loopback latency measurement once and logs the result.
 * Run through tools/latency/run_measure.sh.
 */

var Perf = require("Audacity.Perf")

// Without the requested devices the app would play on the system default
// output; the steps after "Configure audio" then do nothing
var configured = false

var testCase = {
    name: "Perf: loopback latency measurement",
    description: "Measure the round trip with the built-in measurement",
    steps: [
        {
            name: "Configure audio", func: function () {
                var bufferMs = Perf.env("AU_LAT_BUFFER_MS")
                configured = Perf.configureAudio(bufferMs ? parseFloat(bufferMs) : -1,
                                             Perf.env("AU_LAT_OUTPUT"), Perf.env("AU_LAT_INPUT"))
                api.log.info("latency", "configureAudio " + (configured ? "ok" : "FAILED") + ": " + Perf.audioConfiguration())
                if (!configured) {
                    return
                }
                api.testflow.sleep(1000)
            }
        },
        {
            name: "Measure", func: function () {
                if (!configured) {
                    return
                }
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
