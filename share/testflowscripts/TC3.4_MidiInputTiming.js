/*
 * Audacity: A Digital Audio Editor
 * Requires the test-only Audacity Native MIDI QA virtual source. Its driver
 * sends F4 for 200ms, then G4 for 150ms, with sustain down/up around the take.
 */
var Project = require("Audacity.Project")
var recorded = []
var controllers = []
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function notes() { return info().xml.match(/<midinote\s[^>]*\/>/g) || [] }
function cc() { return info().xml.match(/<midicontroller\s[^>]*\/>/g) || [] }
function value(tag, attribute) { return Number(tag.match(new RegExp(attribute + '="([^"]+)"'))[1]) }
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control: " + panel + "/" + JSON.stringify(name))
    sleep(250)
}
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function main() {
    api.testflow.runTestCase({name: "TC3.4: MIDI input timing under UI load", steps: [
        {name: "Create instrument and select the virtual MIDI input", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1500)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(700)
            control("TrackViewSection", "Track " + info().id + " Header Panel", "Piano roll")
            piano("pianoRollControl12")
            piano("pianoRollMidiInput")
            control("Dropdown", "Dropdown", "Audacity Native MIDI QA")
        }},
        {name: "Record driver events while the UI is deliberately busy", func: function() {
            piano("pianoRollControl4")
            api.log.info("VIRTUAL MIDI QA READY")
            // Simulate one long UI operation. MIDI callbacks still arrive on
            // the driver thread; recording must retain their original times.
            var start = Date.now()
            while (Date.now() - start < 700) {}
            sleep(300)
            piano("pianoRollControl4")
            recorded = notes()
            controllers = cc()
            check(recorded.length === 2, "Expected both physical-input notes: " + JSON.stringify(recorded))
            check(value(recorded[0], "pitch") === 65 && value(recorded[1], "pitch") === 67, "Virtual MIDI pitches changed")
            check(value(recorded[0], "velocity") === 100 && value(recorded[1], "velocity") === 90, "Virtual MIDI velocities changed")
            var beatsPerSecond = value(info().xml, "tempo") / 60
            var first = value(recorded[0], "length") / beatsPerSecond
            var second = value(recorded[1], "length") / beatsPerSecond
            var spacing = (value(recorded[1], "start") - value(recorded[0], "start")) / beatsPerSecond
            // The runner additionally compares against the fixture's actual
            // send timestamps; OS scheduling can make a requested sleep longer.
            check(Math.abs(first - 0.2) < 0.05, "Busy UI changed 200ms note duration: " + first)
            check(Math.abs(second - 0.15) < 0.05, "Busy UI changed 150ms note duration: " + second)
            check(Math.abs(spacing - 0.25) < 0.05, "Busy UI changed note spacing: " + spacing)
            check(controllers.length === 2, "Sustain input was not recorded: " + info().xml)
            api.log.info("Recorded MIDI timings: " + JSON.stringify({first: first, second: second, spacing: spacing, controllers: controllers}))
        }},
        {name: "The recorded MIDI take supports one undo and redo", func: function() {
            api.dispatcher.dispatch("action://undo")
            sleep(350)
            check(notes().length === 0 && cc().length === 0, "Undo did not remove the complete MIDI take")
            api.dispatcher.dispatch("action://redo")
            sleep(350)
            check(JSON.stringify(notes()) === JSON.stringify(recorded), "Redo changed the recorded note times")
            check(JSON.stringify(cc()) === JSON.stringify(controllers), "Redo changed recorded sustain")
        }}
    ]})
}
