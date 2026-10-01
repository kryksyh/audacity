/*
 * Audacity: A Digital Audio Editor
 *
 * Timeline performance benchmark: plays a fixed sequence of trackpad pinch and
 * scroll gestures over a project and saves a performance trace.
 * Run through tools/perf/run_timeline_bench.sh.
 */

var Perf = require("Audacity.Perf")

var testCase = {
    name: "Perf: timeline zoom and scroll",
    description: "Fixed pinch/scroll sequence over a project, traced",
    steps: [
        {
            name: "Open project", func: function () {
                Perf.openProject(Perf.env("AU_BENCH_PROJECT"))
                api.testflow.sleep(5000)
            }
        },
        {
            name: "Benchmark", func: function () {
                Perf.startTrace()
                Perf.startTimelineBenchmark()
                while (Perf.isBenchmarkRunning()) {
                    api.testflow.sleep(200)
                }
                Perf.stopTrace(Perf.env("AU_BENCH_TRACE"))
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
