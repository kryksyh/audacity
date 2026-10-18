/*
 * Audacity: A Digital Audio Editor
 * Run with buildscripts/tools/run_native_midi_ownership_test.py. The runner
 * owns a virtual CoreMIDI source and removes it during a recorded take.
 */
var Project = require("Audacity.Project")
var savedPath = ""
var beforeUnplug = ""
var saveAttempts = 0
function check(ok, message) {
    if (!ok) {
        api.log.info("MIDI ownership failure screenshot=" + api.testflow.captureMainWindow("midi-ownership-failure.png"))
        api.log.info("MIDI ownership navigation=" + JSON.stringify({sections: api.navigation.sections().filter(function(s) { return s.enabled }),
                                                                   controls: api.navigation.controls("NavigationBottomPanel", "PianoRollControls")}))
        api.testflow.error(message)
    }
}
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function notes() {
    return (info().xml.match(/<midinote\s[^>]*\/>/g) || []).map(function(tag) {
        var result = {}
        tag.replace(/(\w+)="([^"]*)"/g, function(all, key, value) { result[key] = Number(value) })
        return result
    })
}
function controllers() { return info().xml.match(/<midicontroller\s[^>]*\/>/g) || [] }
function snapshot() { return JSON.stringify({notes: notes(), controllers: controllers()}) }
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control " + panel + "/" + JSON.stringify(name))
    sleep(200)
}
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function selectVirtualInput() {
    piano("pianoRollMidiInput")
    var start = Date.now()
    while (!api.navigation.sections().some(function(item) { return item.name === "Dropdown" && item.enabled })
           && Date.now() - start < 5000) sleep(25)
    control("Dropdown", "Dropdown", "Audacity Native MIDI QA")
}
function assertTake(data, velocities) {
    check(data.length === velocities.length, "Unexpected take: " + JSON.stringify(data))
    var beatsPerSecond = Number(info().xml.match(/tempo="([^"]+)"/)[1]) / 60
    for (var i = 0; i < velocities.length; ++i) {
        var note = data.find(function(item) { return item.velocity === velocities[i] })
        check(note, "Missing input velocity " + velocities[i])
        var seconds = note.length / beatsPerSecond
        if (note.velocity === 100) check(seconds > 0.8 && seconds < 1.5, "Device input interrupted the held computer key: " + seconds)
        else check(seconds > 0.1 && seconds < 0.7, "Device release was not recorded promptly: " + seconds)
    }
}
function handleSaveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(item) { return item.name === "AskLocationTypeDialog" && item.enabled })
    if (section) check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    else if (++saveAttempts < 100) api.testflow.after(100, handleSaveDialog)
    else api.testflow.error("Save dialog did not open")
}
function main() {
    api.testflow.runTestCase({name: "TC3.8: Computer and device MIDI ownership", steps: [
        {name: "Enable both computer and virtual MIDI keyboards", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1500)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(700)
            control("TrackViewSection", "Track " + info().id + " Header Panel", "Piano roll")
            var controls = api.navigation.controls("NavigationBottomPanel", "PianoRollControls")
            var indexes = controls.map(function(item) { return item.index.row + ":" + item.index.column })
            check(indexes.every(function(index, position) { return indexes.indexOf(index) === position }),
                  "Piano roll controls have colliding navigation indexes: " + JSON.stringify(controls))
            piano("pianoRollControl12")
            selectVirtualInput()
            api.log.info("MIDI input navigation screenshot=" + api.testflow.captureMainWindow("midi-input-navigation.png"))
            piano("pianoRollControl9")
        }},
        {name: "Reselect MIDI input while computer keyboard mode is enabled", func: selectVirtualInput},
        {name: "Record overlapping same-pitch keys with independent releases", func: function() {
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            api.log.info("MIDI OWNERSHIP OVERLAP READY")
            sleep(1000)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            assertTake(notes(), [100, 77])
            check(notes().every(function(note) { return note.pitch === 60 }), "Expected overlapping C4 notes")
        }},
        {name: "A device channel-mode command finishes only the device's recorded key", func: function() {
            var prior = notes().map(function(note) { return note.id })
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            api.log.info("MIDI OWNERSHIP CHANNEL_MODE READY")
            sleep(1000)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            var take = notes().filter(function(note) { return prior.indexOf(note.id) === -1 })
            assertTake(take, [100, 77])
            check(take.every(function(note) { return note.pitch === 60 }), "Channel mode changed a recorded pitch")
        }},
        {name: "Record a timed device controller reset without releasing the computer key", func: function() {
            var prior = notes().map(function(note) { return note.id })
            var priorControllers = controllers().length
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            api.log.info("MIDI OWNERSHIP CONTROLLER_RESET READY")
            sleep(1000)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            assertTake(notes().filter(function(note) { return prior.indexOf(note.id) === -1 }), [100, 88])
            var cc = controllers().slice(priorControllers)
            check(cc.length === 2 && cc[0].indexOf('number="64"') !== -1 && cc[1].indexOf('number="121"') !== -1,
                  "Recording dropped the controller reset or added a delayed pedal release: " + JSON.stringify(cc))
            var beat = function(tag) { return Number(tag.match(/beat="([^"]+)"/)[1]) }
            var beatsPerSecond = Number(info().xml.match(/tempo="([^"]+)"/)[1]) / 60
            check(Math.abs((beat(cc[1]) - beat(cc[0])) / beatsPerSecond - 0.2) < 0.05,
                  "The controller reset moved away from its original input time")
        }},
        {name: "Remove the sustained device while a computer key remains held", func: function() {
            beforeUnplug = snapshot()
            var priorControllers = controllers().length
            var prior = notes().map(function(note) { return note.id })
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            api.log.info("MIDI OWNERSHIP UNPLUG READY")
            sleep(1000)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            var take = notes().filter(function(note) { return prior.indexOf(note.id) === -1 })
            assertTake(take, [100, 77, 88])
            check(take.find(function(note) { return note.velocity === 88 }).pitch === 67, "Device's second pitch changed")
            var cc = controllers().slice(priorControllers)
            check(cc.length === 2 && cc[0].indexOf('value="127"') !== -1 && cc[1].indexOf('value="0"') !== -1, "Device removal did not record sustain release")
        }},
        {name: "Undo and redo the complete unplugged-device take", func: function() {
            var expected = snapshot()
            api.dispatcher.dispatch("action://undo")
            sleep(300)
            check(snapshot() === beforeUnplug, "Undo did not restore the exact document before the unplugged take")
            api.dispatcher.dispatch("action://redo")
            sleep(300)
            check(snapshot() === expected, "Redo changed the recorded input boundaries")
        }},
        {name: "Reconnect the recreated same-ID device using Connect MIDI input", func: function() {
            var prior = notes().map(function(note) { return note.id })
            api.log.info("MIDI OWNERSHIP RECREATE SOURCE")
            sleep(1000)
            piano("pianoRollControl12")
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            api.log.info("MIDI OWNERSHIP RECONNECTED READY")
            sleep(1000)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            var take = notes().filter(function(note) { return prior.indexOf(note.id) === -1 })
            assertTake(take, [100, 91])
            check(take.find(function(note) { return note.velocity === 91 }).pitch === 65, "Reconnected source's MIDI did not reach the take")
        }},
        {name: "Freeze and unfreeze the computer/device takes without changing their MIDI boundaries", func: function() {
            var expected = snapshot()
            control("TrackViewSection", "Track " + info().id + " Header Panel", "Route")
            control("NavigationBottomPanel", "TrackRoutingControls", "Freeze")
            var deadline = Date.now() + 20000
            while (info().type !== "wave" && Date.now() < deadline) sleep(50)
            check(info().type === "wave" && info().frozen && Project.clipCount(0) === 1, "Recorded MIDI could not be frozen")
            var pcm = Project.waveformStatistics(0, 0, 2)
            check(pcm && pcm.peak > 0.00001 && pcm.sha256.length === 64, "Frozen recorded MIDI has no PCM audio")
            control("NavigationBottomPanel", "TrackRoutingControls", "Unfreeze")
            check(info().type === "midi" && !info().frozen && snapshot() === expected,
                  "Unfreeze changed recorded keyboard/device note or pedal boundaries")
            control("TrackViewSection", "Track " + info().id + " Header Panel", "Piano roll")
            api.log.info("Recorded channel-mode take screenshot=" + api.testflow.captureMainWindow("midi-channel-mode-unfrozen.png"))
        }},
        {name: "Save and reopen all input takes", func: function() {
            var expected = snapshot()
            var destination = api.testflow.prepareSavingFile("MIDI input ownership.aup4")
            api.testflow.after(100, handleSaveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            savedPath = api.testflow.selectedFilePath()
            check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Input project was not saved")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
            sleep(1200)
            check(Project.trackCount() === 1 && snapshot() === expected, "Reopening changed the input takes")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Input project did not close cleanly")
        }}
    ]})
}
