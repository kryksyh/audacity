/* Native MIDI clips use Audacity's normal timeline gestures and actions. */
var Project = require("Audacity.Project")
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms || 250) }
function info(i) { return Project.trackInfo(i || 0) }
function clips(i) { return Project.clipsOnTrack(i || 0) }
function clipId() { var match = info().xml.match(/<midiclip\s[^>]*id="(\d+)"/); check(match, "No MIDI clip XML"); return Number(match[1]) }
function name() { return "midiClip" + info().id + "_" + clipId() }
function undo() { api.dispatcher.dispatch("action://undo"); sleep(400) }
function redo() { api.dispatcher.dispatch("action://redo"); sleep(400) }
function bounds(expected) { var actual = clips(); check(JSON.stringify(actual) === JSON.stringify(expected), "Clip bounds differ: " + JSON.stringify(actual) + " vs " + JSON.stringify(expected)) }
function notes() { return info().xml.match(/<midinote\s[^>]*\/>/g) || [] }
function menu() { check(api.testflow.mouseClickItem(name(), 0.5, 0.65, "right"), "Cannot open MIDI context menu"); sleep(300) }
function item(title) {
    var code = { "Open piano roll": "midi-piano-roll", "Duplicate": "duplicate", "Delete": "action://trackedit/delete", "Copy": "action://trackedit/copy", "Cut": "action://trackedit/cut", "Split": "split", "Rename clip": "rename-item" }[title]
    var found = false
    var sections = api.navigation.sections()
    for (var i = 0; i < sections.length; ++i) {
        if (sections[i].enabled && api.navigation.triggerControl(sections[i].name, "StyledMenu", code)) {
            found = true
            break
        }
    }
    check(found, "Missing MIDI menu command " + title + ": " + JSON.stringify(api.navigation.sections()))
    sleep(350)
}
var original, moved, sourceNotes, savedPath, savedClips, savedXml, attempts = 0
function saveDialog() {
    if (api.testflow.selectedFilePath()) return
    if (api.navigation.sections().some(function(s) { return s.name === "AskLocationTypeDialog" && s.enabled })) {
        api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer")
    } else if (++attempts < 100) api.testflow.after(100, saveDialog)
}
function main() {
 api.testflow.runTestCase({name: "TC3.20: Native MIDI timeline", steps: [
 {name: "Configure leave-gap editing through Preferences", func: function() {
    api.testflow.showMainWindowOnFront()
    api.dispatcher.dispatch("command://app/editing-preferences")
    var deadline = Date.now() + 5000
    var choice
    do {
        choice = api.navigation.accessibleControls().find(function(c) {
            return c.enabled && c.section === "PreferencesDialog" && c.name === "Leave gap"
        })
        if (!choice) sleep(50)
    } while (!choice && Date.now() < deadline)
    check(choice, "Leave-gap preference is unavailable")
    check(api.navigation.triggerControl(choice.section, choice.panel, choice.name), "Cannot choose leave-gap editing")
    sleep(200)
    check(api.navigation.triggerControl("PreferencesDialog", "ButtonBox", "OK"), "Cannot apply editing preferences")
    deadline = Date.now() + 5000
    while (api.navigation.sections().some(function(s) { return s.name === "PreferencesDialog" && s.enabled }) && Date.now() < deadline) sleep(50)
    check(!api.navigation.sections().some(function(s) { return s.name === "PreferencesDialog" && s.enabled }), "Editing preferences remained open")
 }},
 {name: "Draw a note and expose the shared timeline clip", func: function() {
    api.testflow.showMainWindowOnFront(); api.dispatcher.dispatch("file-new"); sleep(1500)
    api.dispatcher.dispatch("new-instrument-track"); sleep(500)
    check(api.navigation.triggerControl("TrackViewSection", "Track " + info().id + " Header Panel", "Piano roll"), "Cannot open piano roll"); sleep(400)
    var grid = api.testflow.itemRect("pianoRollGrid"); check(grid, "No piano roll")
    check(api.testflow.mouseClickItem("pianoRollGrid", 96 / grid.width, 45 / grid.height, "left", true), "Cannot draw MIDI note"); sleep(350)
    check(notes().length === 1, "Note missing"); sourceNotes = JSON.stringify(notes()); original = clips()
    check(api.testflow.itemRect(name() + "Header"), "Shared MIDI clip header missing")
    var preview = api.testflow.itemRect(name() + "NotePreview")
    check(preview && preview.height > 5, "MIDI preview body missing: " + JSON.stringify(preview))
    api.testflow.captureMainWindow("midi-timeline-initial.png")
 }},
 {name: "Move the MIDI header with one undoable gesture", func: function() {
    var header = name() + "Header"
    check(api.testflow.mouseDragItem(header, 0.3, 0.5, 0.8, 0.5), "Cannot drag MIDI header"); sleep(400)
    moved = clips(); check(moved[0].start > original[0].start, "MIDI drag did not move clip: " + JSON.stringify(moved))
    check(Math.abs(moved[0].end - moved[0].start - original[0].end + original[0].start) < 1e-6, "Move changed duration")
    undo(); bounds(original); redo(); bounds(moved)
 }},
 {name: "Trim both MIDI edges without deleting source notes", func: function() {
    var edge = name() + "HandlesRightTrim", rect = api.testflow.itemRect(edge)
    check(rect, "Selected right trim handle missing")
    check(api.testflow.mouseDragItem(edge, 0.5, 0.5, 0.5 - 30 / rect.width, 0.5), "Cannot trim right edge"); sleep(400)
    check(clips()[0].end < moved[0].end, "Right trim did not change end")
    check(JSON.stringify(notes()) === sourceNotes, "Trim deleted hidden notes"); undo(); bounds(moved)
    edge = name() + "HandlesLeftTrim"; rect = api.testflow.itemRect(edge); check(rect, "Selected left trim handle missing")
    check(api.testflow.mouseDragItem(edge, 0.5, 0.5, 0.5 + 25 / rect.width, 0.5), "Cannot trim left edge"); sleep(400)
    check(clips()[0].start > moved[0].start, "Left trim did not change start")
    check(JSON.stringify(notes()) === sourceNotes, "Left trim deleted hidden notes"); undo(); bounds(moved)
 }},
 {name: "Open the MIDI context menu and its docked piano roll", func: function() {
    menu(); item("Open piano roll"); check(api.testflow.itemRect("pianoRollGrid"), "Piano roll not docked")
    api.testflow.captureMainWindow("midi-timeline-edited.png")
 }},
 {name: "Duplicate through the shared menu and undo", func: function() {
    menu(); item("Duplicate")
    check(Project.trackCount() === 2 && Project.clipCount(1) === 1, "Duplicate did not create MIDI track")
    check(info(1).type === "midi", "Duplicate made a wave track")
    check((info(1).xml.match(/<midinote\s/g) || []).length === 1, "Duplicate lost notes")
    undo(); check(Project.trackCount() === 1, "Undo duplicate failed"); bounds(moved)
    redo(); check(Project.trackCount() === 2, "Redo duplicate failed"); undo()
 }},
 {name: "Delete through the timeline menu and restore with undo", func: function() {
    menu(); item("Delete"); check(Project.clipCount(0) === 0, "Delete did not remove MIDI clip")
    undo(); bounds(moved); check(JSON.stringify(notes()) === sourceNotes, "Undo delete lost notes")
 }},
 {name: "Copy paste and cut through normal timeline commands", func: function() {
    menu(); item("Copy")
    var canvas = api.testflow.itemRect("ItemsView"), clip = api.testflow.itemRect(name())
    check(canvas && clip, "Timeline geometry missing")
    var x = Math.min(canvas.width - 10, clip.x + clip.width + 45 - canvas.x)
    var y = clip.y + clip.height * 0.65 - canvas.y
    check(x + canvas.x > clip.x + clip.width, "No visible gap for paste")
    check(api.testflow.mouseClickItem("ItemsView", x / canvas.width, y / canvas.height), "Cannot place paste cursor"); sleep(350)
    api.dispatcher.dispatch("action://trackedit/paste-default"); sleep(500)
    check(Project.trackCount() === 1 && Project.clipCount(0) === 2, "Clipboard paste did not create another MIDI clip")
    check(notes().length === 2, "Clipboard paste lost notes")
    undo(); bounds(moved); check(JSON.stringify(notes()) === sourceNotes, "Paste undo changed source notes")
    menu(); item("Cut"); check(Project.clipCount(0) === 0, "Cut did not remove MIDI clip")
    undo(); bounds(moved); check(JSON.stringify(notes()) === sourceNotes, "Cut undo changed source notes")
 }},
 {name: "Rename the MIDI clip using its normal title editor", func: function() {
    menu(); item("Rename clip")
    check(api.testflow.typeItemText(name() + "Title", "Edited MIDI phrase", true), "MIDI title field did not receive keyboard focus")
    sleep(400)
    check(info().xml.indexOf('name="Edited MIDI phrase"') !== -1, "Rename did not update MIDI document")
    undo(); check(info().xml.indexOf('name="Edited MIDI phrase"') === -1, "Undo rename failed")
    redo(); check(info().xml.indexOf('name="Edited MIDI phrase"') !== -1, "Redo rename failed")
 }},
 {name: "Split the MIDI clip at the cursor and undo", func: function() {
    check(api.testflow.mouseClickItem(name(), 0.5, 0.7), "Cannot place cursor inside MIDI clip"); sleep(300)
    menu(); item("Split"); check(Project.clipCount(0) === 2, "Split did not create two clips")
    undo(); bounds(moved); check(Project.clipCount(0) === 1, "Undo split failed")
 }},
 {name: "Save reopen and keep MIDI timeline bounds and notes", func: function() {
    savedClips = clips(); savedXml = JSON.stringify(notes())
    var destination = api.testflow.prepareSavingFile("MIDI timeline.aup4")
    api.testflow.after(100, saveDialog); api.dispatcher.dispatch("file-save"); sleep(4500)
    savedPath = api.testflow.selectedFilePath(); check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Save failed")
    api.dispatcher.dispatch("file-close"); sleep(600)
    api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath)); sleep(1200)
    bounds(savedClips); check(JSON.stringify(notes()) === savedXml, "Reopen lost notes")
    var deadline = Date.now() + 5000
    while (!api.testflow.itemRect(name() + "Header") && Date.now() < deadline) sleep(50)
    check(api.testflow.itemRect(name() + "Header"), "Reopened MIDI header missing")
 }}
 ]})
}
