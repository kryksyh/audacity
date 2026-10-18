/*
 * Audacity: A Digital Audio Editor
 * Real piano-roll gestures, Muse controls, undo and project persistence.
 */
var Project = require("Audacity.Project")
var savedPath = ""
var savedXml = ""
var saveAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function noteData() {
    return (info().xml.match(/<midinote\s[^>]*\/>/g) || []).map(function(tag) {
        var result = {}
        tag.replace(/(\w+)="([^"]*)"/g, function(all, key, value) { result[key] = Number(value) })
        return result
    })
}
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control " + panel + "/" + JSON.stringify(name))
    sleep(200)
}
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function number(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "PianoRollControls", name), "Missing numeric editor " + name)
    api.keyboard.key("A", "CTRL")
    api.keyboard.text(String(value))
    sleep(100)
    api.keyboard.key("Return")
    sleep(250)
}
function dropdown(column, title) {
    piano([-1, column])
    control("Dropdown", "Dropdown", title)
}
function add(beat, pitch) {
    var rect = api.testflow.itemRect("pianoRollGrid")
    check(rect && rect.height > 70, "Piano grid missing or too small: " + JSON.stringify(rect))
    // The visible keyboard starts on C5 (72) when its dock opens.
    check(api.testflow.mouseClickItem("pianoRollGrid", beat * 96 / rect.width,
                                    ((72 - pitch) * 18 + 9) / rect.height, "left", true), "Cannot draw note")
    sleep(250)
}
function undo() { api.dispatcher.dispatch("action://undo"); sleep(300) }
function redo() { api.dispatcher.dispatch("action://redo"); sleep(300) }
function assertNote(index, start, length, pitch, velocity) {
    var note = noteData()[index]
    check(note && Math.abs(note.start - start) < 0.00001 && Math.abs(note.length - length) < 0.00001
          && note.pitch === pitch && note.velocity === velocity, "Unexpected note " + index + ": " + JSON.stringify(note))
}
function savedMidi() {
    return JSON.stringify({notes: noteData(), controllers: info().xml.match(/<midicontroller\s[^>]*\/>/g) || []})
}
function handleSaveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(item) { return item.name === "AskLocationTypeDialog" && item.enabled })
    if (section) {
        check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    } else if (++saveAttempts < 100) api.testflow.after(100, handleSaveDialog)
    else api.testflow.error("Save dialog did not open")
}
function main() {
    api.testflow.runTestCase({name: "TC3.6: Piano roll editing", steps: [
        {name: "Draw a snapped note in the docked grid", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1300)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(450)
            control("TrackViewSection", "Track " + info().id + " Header Panel", "Piano roll")
            add(0.6, 70)
            check(noteData().length === 1, "Double-click did not draw a note")
            assertNote(0, 0.5, 1, 70, 100)
            undo()
            check(noteData().length === 0, "Draw gesture did not create one undo entry")
            redo()
            assertNote(0, 0.5, 1, 70, 100)
        }},
        {name: "Move and transpose a note in one gesture", func: function() {
            var name = "pianoRollNote" + noteData()[0].id
            var rect = api.testflow.itemRect(name)
            check(rect, "Drawn note has no item")
            check(api.testflow.mouseDragItem(name, 0.3, 0.5, 0.3 + 48 / rect.width, 0.5 + 36 / rect.height), "Cannot move note")
            sleep(250)
            assertNote(0, 1, 1, 68, 100)
            undo()
            assertNote(0, 0.5, 1, 70, 100)
            redo()
            assertNote(0, 1, 1, 68, 100)
        }},
        {name: "Resize the right edge without changing pitch or position", func: function() {
            var name = "pianoRollNote" + noteData()[0].id
            var rect = api.testflow.itemRect(name)
            check(api.testflow.mouseDragItem(name, 1 - 3 / rect.width, 0.5, 1 + 45 / rect.width, 0.5), "Cannot resize note")
            sleep(250)
            assertNote(0, 1, 1.5, 68, 100)
            undo()
            assertNote(0, 1, 1, 68, 100)
            redo()
            assertNote(0, 1, 1.5, 68, 100)
        }},
        {name: "Change quantize and note length through Muse dropdowns", func: function() {
            dropdown(6, "1/8")
            dropdown(5, "1/8")
            add(2.7, 71)
            check(noteData().length === 2, "Second note was not added")
            assertNote(1, 2.5, 0.5, 71, 100)
        }},
        {name: "Edit selected note velocity numerically and by dragging its bar", func: function() {
            var id = noteData()[0].id
            check(api.testflow.mouseClickItem("pianoRollNote" + id), "Cannot select first note")
            number("pianoRollControl8", 83)
            assertNote(0, 1, 1.5, 68, 83)
            piano("pianoRollControl7")
            var name = "pianoRollVelocity" + id
            var rect = api.testflow.itemRect(name)
            check(rect, "Velocity bar missing")
            check(api.testflow.mouseDragItem(name, 0.5, 0.5, 0.5, 0.5 + 12 / rect.height), "Cannot drag velocity bar")
            sleep(250)
            assertNote(0, 1, 1.5, 68, 58)
            undo()
            assertNote(0, 1, 1.5, 68, 83)
            redo()
            assertNote(0, 1, 1.5, 68, 58)
        }},
        {name: "Write sustain events and undo a replacement at the same beat", func: function() {
            number("pianoRollSustainBeat", 0.5)
            piano("pianoRollPedalDown")
            number("pianoRollSustainBeat", 2)
            piano("pianoRollPedalUp")
            var events = info().xml.match(/<midicontroller\s[^>]*\/>/g) || []
            check(events.length === 2 && events[0].indexOf('value="127"') !== -1 && events[1].indexOf('value="0"') !== -1, "Sustain events missing")
            piano("pianoRollPedalDown")
            check((info().xml.match(/<midicontroller\s/g) || []).length === 2, "Pedal replacement duplicated the event")
            undo()
            check((info().xml.match(/<midicontroller\s[^>]*\/>/g) || [])[1] === events[1], "Undo did not restore pedal up")
        }},
        {name: "Delete with right click and with the selected-note button", func: function() {
            var second = noteData()[1].id
            check(api.testflow.mouseClickItem("pianoRollNote" + second, 0.5, 0.5, "right"), "Cannot right-click delete")
            sleep(250)
            check(noteData().length === 1, "Right-click did not delete note")
            undo()
            check(noteData().length === 2, "Undo did not restore deleted note")
            check(api.testflow.mouseClickItem("pianoRollNote" + second), "Cannot select second note")
            piano("pianoRollControl13")
            check(noteData().length === 1, "Delete button did not delete selected note")
            undo()
            check(noteData().length === 2, "Undo did not restore button-deleted note")
        }},
        {name: "Save close and reopen the edited notes and controllers", func: function() {
            savedXml = savedMidi()
            var destination = api.testflow.prepareSavingFile("Piano roll editing.aup4")
            check(destination, "No fresh save destination")
            api.testflow.after(100, handleSaveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            savedPath = api.testflow.selectedFilePath()
            check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Edited project was not saved at its fresh destination")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
            sleep(1200)
            check(Project.trackCount() === 1 && savedMidi() === savedXml, "Saved notes or controllers changed on reopen")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Reopened piano-roll project did not close cleanly")
        }}
    ]})
}
