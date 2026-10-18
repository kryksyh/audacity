/*
 * Audacity: A Digital Audio Editor
 * Real controls, native keyboard events, document assertions, and undo.
 */
var Project = require("Audacity.Project")
var originalNotes = []
var busId = ""

function check(condition, message) {
    if (!condition) api.testflow.error(message)
}
function sleep(ms) { api.testflow.sleep(ms) }
function info(index) { return Project.trackInfo(index) }
function notes() { return info(0).xml.match(/<midinote\s[^>]*\/>/g) || [] }
function controlName(panel, name) {
    if (panel !== "PianoRollControls") return name
    var names = {"Record notes": "pianoRollControl4", "Stop notes": "pianoRollControl4",
        "Computer keyboard": "pianoRollControl9"}
    return names[name] || name
}
function control(section, panel, name) {
    for (var attempt = 0; attempt < 50; ++attempt) {
        var ready = api.navigation.controls(section, panel).some(function(item) { return item.name === controlName(panel, name) && item.enabled })
        if (ready) break
        sleep(100)
    }
    check(api.navigation.triggerControl(section, panel, controlName(panel, name)), "Missing UI control: " + panel + "/" + name)
    sleep(250)
}
function header(name) {
    control("TrackViewSection", "Track " + info(0).id + " Header Panel", name)
}
function bottom(panel, name) { control("NavigationBottomPanel", panel, name) }
function enabled(panel, name) {
    var controls = api.navigation.controls("NavigationBottomPanel", panel)
    for (var i = 0; i < controls.length; ++i) {
        if (controls[i].name === controlName(panel, name) && controls[i].enabled) return true
    }
    api.log.info("Controls in " + panel + ": " + JSON.stringify(controls))
    return false
}

function main() {
    api.testflow.runTestCase({name: "TC3.1: Native MIDI, freeze and routing", steps: [
        {name: "Create project", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1200)
        }},
        {name: "Create instrument and dock piano roll", func: function() {
            api.dispatcher.dispatch("new-instrument-track")
            sleep(500)
            check(Project.trackCount() === 1 && info(0).type === "midi", "Instrument track was not created")
            header("Piano roll")
            check(enabled("PianoRollControls", "Record notes"), "Docked piano roll is not active")
            check((info(0).xml.match(/<midiclip\s/g) || []).length === 1, "Opening the editor did not create its MIDI clip")
        }},
        {name: "Record two computer-keyboard notes", func: function() {
            bottom("PianoRollControls", "Computer keyboard")
            bottom("PianoRollControls", "Record notes")
            api.keyboard.pressKey("Z")
            sleep(180)
            api.keyboard.releaseKey("Z")
            sleep(100)
            api.keyboard.pressKey("X")
            sleep(220)
            api.keyboard.releaseKey("X")
            sleep(100)
            bottom("PianoRollControls", "Stop notes")
            originalNotes = notes()
            check(originalNotes.length === 2, "Expected two recorded MIDI notes, got " + originalNotes.length)
            check(originalNotes[0].indexOf('pitch="60"') !== -1, "Z did not record C4")
            check(originalNotes[1].indexOf('pitch="62"') !== -1, "X did not record D4")
        }},
        {name: "Freeze from routing dock", func: function() {
            header("Route")
            check(enabled("TrackRoutingControls", "Freeze"), "Routing dock did not become active")
            bottom("TrackRoutingControls", "Freeze")
            sleep(1000)
            check(info(0).type === "wave" && info(0).frozen, "Freeze did not retain a source on an audio track")
            check(Project.clipCount(0) === 1, "Freeze produced no audio clip")
        }},
        {name: "Unfreeze and retain both recorded notes", func: function() {
            bottom("TrackRoutingControls", "Unfreeze")
            check(info(0).type === "midi" && !info(0).frozen, "Unfreeze did not restore MIDI")
            check(JSON.stringify(notes()) === JSON.stringify(originalNotes), "Unfreeze changed recorded notes")
            header("Piano roll")
            check(enabled("PianoRollControls", "Record notes"), "Piano roll button did not activate its existing tab")
        }},
        {name: "Undo and redo the unfreeze", func: function() {
            api.dispatcher.dispatch("action://undo")
            sleep(350)
            check(info(0).type === "wave" && info(0).frozen, "Undo unfreeze did not restore frozen audio")
            api.dispatcher.dispatch("action://redo")
            sleep(350)
            check(info(0).type === "midi", "Redo unfreeze did not restore MIDI")
            check(JSON.stringify(notes()) === JSON.stringify(originalNotes), "Redo changed the MIDI source")
        }},
        {name: "Create bus with real routing control", func: function() {
            header("Route")
            bottom("TrackRoutingControls", "Add bus")
            check(Project.trackCount() === 2, "Add bus did not create a track")
            var bus = info(1)
            var match = bus.xml.match(/<routing bus="([^"]+)"/)
            check(bus.type === "wave" && match, "New bus has no routing identity")
            busId = match[1]
        }},
        {name: "Select bus as instrument output", func: function() {
            header("Route")
            bottom("TrackRoutingControls", "Dropdown")
            api.navigation.down()
            sleep(100)
            api.navigation.trigger()
            sleep(300)
            check(info(0).xml.indexOf('output="' + busId + '"') !== -1, "Output dropdown did not route instrument to bus")
        }}
    ]})
}
