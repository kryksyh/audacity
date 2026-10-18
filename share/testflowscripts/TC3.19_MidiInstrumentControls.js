/*
 * Audacity: A Digital Audio Editor
 * Installed Analog Lab VST3 plus the owned Audacity Native MIDI QA source.
 * Send eight CCs (74/71/76/77/82/83/85/17), sweeping each through
 * both ends before its target so Analog Lab's pickup mode can engage
 * and a short note when MIDI INSTRUMENT CONTROLS READY is logged.
 */
var Project = require("Audacity.Project")
var lastControlState = ""
var expected = {"2":25, "4":35, "6":45, "8":55, "0":95}
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control " + panel + "/" + JSON.stringify(name))
    sleep(200)
}
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function waitFor(predicate, message) {
    var deadline = Date.now() + 15000
    do { if (predicate()) return; sleep(50) } while (Date.now() < deadline)
    check(false, message)
}
function state() {
    var match = info().xml.match(/instrumentstate="([^"]*)"/)
    check(match, "No instrument preset in the document")
    return JSON.parse(match[1].replace(/&quot;/g, '"').replace(/&apos;/g, "'")
                     .replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&amp;/g, "&"))
}
function controlsSaved() {
    var values = state().parameters
    var actual = {}; Object.keys(expected).forEach(function(id) { actual[id] = values && values[id] })
    var text = JSON.stringify(actual)
    if (text !== lastControlState) { api.log.info("MIDI INSTRUMENT CONTROLS VALUES " + text); lastControlState = text }
    return values && Object.keys(expected).every(function(id) { return Math.abs(values[id] - expected[id] / 127) < 0.00001 })
}
var heartbeatActive = false
var heartbeatLast = 0
var heartbeatMax = 0
var heartbeatCount = 0
function heartbeat() {
    if (!heartbeatActive) return
    var now = Date.now()
    heartbeatMax = Math.max(heartbeatMax, now - heartbeatLast)
    heartbeatLast = now
    ++heartbeatCount
    api.testflow.after(10, heartbeat)
}
function main() {
    api.testflow.runTestCase({name: "TC3.19: Native instrument hardware controls", steps: [
        {name: "Open Analog Lab VST3 with a floating editor and virtual keyboard input", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1500)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(700)
            control("TrackViewSection", "Track " + info().id + " Header Panel", "Piano roll")
            piano([-1, 1])
            for (var i = 0; i < 250 && api.navigation.activeControl() !== "Analog Lab V (VST3)"; ++i) {
                api.navigation.down(); sleep(40)
            }
            check(api.navigation.activeControl() === "Analog Lab V (VST3)", "Installed Analog Lab is unavailable")
            api.navigation.trigger()
            sleep(600)
            piano("pianoRollControl12")
            piano("pianoRollMidiInput")
            control("Dropdown", "Dropdown", "Audacity Native MIDI QA")
            piano("pianoRollControl2")
            waitFor(function() { return api.testflow.itemWindowInfo("nativeInstrumentView") }, "Native editor did not open")
        }},
        {name: "Follow a continuous hardware knob sweep without blocking the UI", func: function() {
            heartbeatActive = true
            heartbeatLast = Date.now()
            heartbeat()
            api.log.info("MIDI INSTRUMENT CONTINUOUS READY")
            sleep(3500)
            heartbeatActive = false
            api.log.info("MIDI INSTRUMENT CONTINUOUS UI " + JSON.stringify({maxGapMs: heartbeatMax, callbacks: heartbeatCount}))
            sleep(700)
        }},
        {name: "Record CoreMIDI notes and controls while updating the native preset", func: function() {
            piano("pianoRollControl4")
            api.log.info("MIDI INSTRUMENT CONTROLS READY")
            sleep(1600)
            piano("pianoRollControl4")
            waitFor(controlsSaved, "MIDI controls did not update and save Analog Lab's actual sound parameters")
            var xml = info().xml
            check((xml.match(/<midinote\s/g) || []).length === 1, "Hardware note was not recorded")
            check((xml.match(/<midicontroller\s/g) || []).length === 24, "Hardware controls were not recorded")
            api.log.info("MIDI INSTRUMENT CONTROLS SAVED " + JSON.stringify(expected))
        }},
        {name: "Reopen the floating editor and render the controlled instrument", func: function() {
            check(api.testflow.closeItemWindow("nativeInstrumentView"), "Cannot close native editor")
            sleep(300)
            piano("pianoRollControl2")
            waitFor(function() { return api.testflow.itemWindowInfo("nativeInstrumentView") }, "Native editor did not reopen")
            check(controlsSaved(), "Reopening the editor changed the MIDI-controlled preset")
            api.dispatcher.dispatch("select-all")
            api.dispatcher.dispatch("mix-render-to-new-track")
            waitFor(function() { return Project.trackCount() === 2 }, "Controlled take did not render")
            var audio = Project.waveformStatistics(1, 0, 2)
            check(audio && audio.peak > 0.00001 && audio.rms > 0.000001, "Controlled instrument rendered silence")
            api.log.info("MIDI INSTRUMENT CONTROLS AUDIO " + JSON.stringify(audio))
        }}
    ]})
}
