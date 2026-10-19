/* Import a real multi-track SMF through File Open, then edit/play/save it. */
var Project = require("Audacity.Project")
var before, saved, savedPath, saving = false, closingEmpty = false, closeAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function snapshot() {
    var result = []
    for (var i = 0; i < Project.trackCount(); ++i) {
        result.push({clips: Project.clipsOnTrack(i), events: Project.trackInfo(i).xml.match(/<midi(?:note|controller)\s[^>]*\/>/g)})
    }
    return JSON.stringify(result)
}
function saveDialog() {
    if (!saving || api.testflow.selectedFilePath()) return
    api.testflow.after(100, saveDialog)
    if (api.navigation.sections().some(function(s) { return s.enabled && s.name === "AskLocationTypeDialog" })) {
        check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    }
}
function discardEmpty() {
    if (!closingEmpty) return
    var button = api.navigation.accessibleControls().find(function(item) {
        return item.enabled && (item.accessibleName === "Don’t save" || item.accessibleName === "Don't save")
    })
    if (button && api.testflow.mouseClickControl(button.section, button.panel, button.name)) {
        closingEmpty = false
        return
    }
    if (++closeAttempts < 50) api.testflow.after(100, discardEmpty)
    else check(false, "Cannot close the disposable empty project")
}
function main() {
    api.testflow.runTestCase({name: "TC3.21: Native MIDI file import", steps: [
        {name: "Open a multi-track MIDI file with tempo and expression", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1200)
            before = Project.musicalContext()
            closingEmpty = true
            api.testflow.after(100, discardEmpty)
            api.dispatcher.dispatch("file-close")
            closingEmpty = false
            sleep(600)
            check(api.testflow.openProject("native-midi-import.mid"), "MIDI fixture is missing")
            var end = Date.now() + 5000
            while (Project.trackCount() !== 2 && Date.now() < end) sleep(25)
            check(Project.trackCount() === 2, "MIDI file did not create two instrument tracks")
            check(Project.trackInfo(0).type === "midi" && Project.trackInfo(1).type === "midi", "Import created audio tracks")
            var context = Project.musicalContext()
            check(context.tempo === 100 && context.numerator === 7 && context.denominator === 8, "File tempo/meter was not imported")
            check(Project.trackInfo(0).xml.indexOf('type="192"') !== -1 && Project.trackInfo(0).xml.indexOf('type="224"') !== -1, "Program or pitch bend was discarded")
            saved = snapshot()
        }},
        {name: "Import is one undoable operation", func: function() {
            api.dispatcher.dispatch("action://undo"); sleep(300)
            check(Project.trackCount() === 0, "Undo left imported MIDI tracks")
            check(Project.musicalContext().tempo === before.tempo, "Undo did not restore the previous tempo")
            api.dispatcher.dispatch("action://redo"); sleep(300)
            check(Project.trackCount() === 2 && snapshot() === saved, "Redo lost imported notes or expression")
            check(Project.musicalContext().tempo === 100, "Redo lost imported tempo")
        }},
        {name: "Open the imported clip in the docked piano roll and play", func: function() {
            var id = Project.trackInfo(0).id
            var end = Date.now() + 5000, opened = false
            while (!opened && Date.now() < end) {
                opened = api.navigation.triggerControl("TrackViewSection", "Track " + id + " Header Panel", "Piano roll")
                if (!opened) sleep(50)
            }
            check(opened, "Cannot edit imported MIDI")
            sleep(300)
            check(api.testflow.itemRect("pianoRollGrid"), "Imported MIDI has no docked editor")
            api.dispatcher.dispatch("action://playback/toggle-play-pause")
            end = Date.now() + 5000
            while (Project.playbackInfo().position < 0.06 && Date.now() < end) sleep(10)
            check(Project.playbackInfo().position >= 0.06, "Imported MIDI did not play")
            api.dispatcher.dispatch("action://playback/stop"); sleep(100)
        }},
        {name: "Save and reopen imported notes and expression", func: function() {
            saved = snapshot()
            var destination = api.testflow.prepareSavingFile("Imported MIDI.aup4")
            saving = true
            api.testflow.after(100, saveDialog)
            api.dispatcher.dispatch("file-save"); sleep(3500)
            savedPath = api.testflow.selectedFilePath(); saving = false
            check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Imported MIDI was not saved")
            api.dispatcher.dispatch("file-close"); sleep(500)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath)); sleep(1000)
            check(Project.trackCount() === 2 && snapshot() === saved, "Saved MIDI lost timing or expressive events")
            check(Project.musicalContext().tempo === 100, "Saved MIDI lost its tempo")
            api.dispatcher.dispatch("file-close"); sleep(500)
        }}
    ]})
}
