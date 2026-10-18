/*
 * Audacity: A Digital Audio Editor
 * Real toolbar meter/tempo edits with native instrument monitoring and playback.
 */
var Project = require("Audacity.Project")
var savedPath = ""
var savedXml = ""
var savedContext = ""
var saveAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control " + panel + "/" + name)
    sleep(150)
}
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function toolbar(name) { control("PlaybackSection", "PlaybackToolBar", name) }
function triggerAccessibleWhenReady(name, message) {
    var deadline = Date.now() + 5000
    do {
        if (api.navigation.triggerAccessibleControl(name)) return
        sleep(25)
    } while (Date.now() < deadline)
    api.navigation.dump()
    check(false, message)
}
function showTimingControls() {
    var controls = api.navigation.controls("PlaybackSection", "PlaybackToolBar")
    var tempoHidden = !controls.some(function(c) { return c.name === "BPMArrowUpButton" })
    var meterHidden = !controls.some(function(c) { return c.name === "TimeSignaturePopupButton" })
    if (!tempoHidden && !meterHidden) return
    triggerAccessibleWhenReady("Customize toolbar", "Cannot customize the playback toolbar")
    sleep(250)
    if (tempoHidden) { triggerAccessibleWhenReady("Tempo", "Cannot show tempo controls"); sleep(250) }
    if (meterHidden) { triggerAccessibleWhenReady("Time signature", "Cannot show meter controls"); sleep(250) }
    api.keyboard.key("Escape")
    sleep(250)
}
function state() { return Project.playbackInfo() }
function notes() {
    return (Project.trackInfo(0).xml.match(/<midinote\s[^>]*\/>/g) || []).map(function(tag) {
        var note = {}
        tag.replace(/(\w+)="([^"]*)"/g, function(all, name, value) { note[name] = Number(value) })
        return note
    })
}
function ruler(beat, text, pixels) {
    var name = "pianoRollRulerBeat" + beat
    var item = api.testflow.itemWindowInfo(name)
    check(item && item.text === text, "Wrong ruler label at beat " + beat + ": " + JSON.stringify(item))
    var origin = api.testflow.itemRect("pianoRollRulerBeat0")
    var tick = api.testflow.itemRect(name)
    check(origin && tick && Math.abs(tick.x - origin.x - pixels) < 1, "Ruler does not use the meter's note unit")
}
function scrolledRuler() {
    var view = api.testflow.itemWindowInfo("pianoRollGrid")
    var grid = api.testflow.itemRect("pianoRollGrid")
    var beat = Math.ceil(view.contentX / 48)
    var label = api.testflow.itemWindowInfo("pianoRollRulerBeat" + beat)
    var position = api.testflow.itemRect("pianoRollRulerBeat" + beat)
    var text = String(Math.floor(beat / 7) + 1) + (beat % 7 === 0 ? "" : "." + (beat % 7 + 1))
    check(label && label.text === text, "Scrolled ruler lost its musical position")
    check(position && Math.abs(position.x - grid.x - (beat * 48 - view.contentX)) < 1, "Scrolled label drifted from the grid")
    var bar = Math.ceil(beat / 7) * 7
    var line = api.testflow.itemRect("pianoRollMeterBeat" + bar)
    check(line && line.width === 2 && Math.abs(line.x - grid.x - (bar * 48 - view.contentX)) < 1, "Scrolled bar line drifted from the ruler")
}
function waitFor(predicate, message) {
    var end = Date.now() + 5000
    var value
    do {
        value = state()
        if (predicate(value)) return value
        sleep(20)
    } while (Date.now() < end)
    check(false, message + ": " + JSON.stringify(value))
}
function meter(upper, lower) {
    toolbar("TimeSignaturePopupButton")
    check(api.testflow.typeControlText("TimeSignaturePopup", "TimeSignaturePopup", "TimeSignatureNumerator", String(upper)), "Cannot type meter numerator")
    sleep(100)
    check(Project.musicalContext().numerator === upper, "Numeric meter entry failed before choosing denominator: " + JSON.stringify(Project.musicalContext()))
    control("TimeSignaturePopup", "TimeSignaturePopup", "TimeSignatureDenominator")
    control("Dropdown", "Dropdown", String(lower))
    check(api.testflow.mouseClickControl("PlaybackSection", "PlaybackToolBar", "TimeSignaturePopupButton"), "Cannot close meter popup")
    sleep(100)
    var current = Project.musicalContext()
    check(current.numerator === upper && current.denominator === lower, "Toolbar meter edit failed: " + JSON.stringify(current))
}
function handleSaveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(item) { return item.name === "AskLocationTypeDialog" && item.enabled })
    if (section) check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    else if (++saveAttempts < 100) api.testflow.after(100, handleSaveDialog)
    else api.testflow.error("Save dialog did not open")
}
function main() {
    api.testflow.runTestCase({name: "TC3.14: Native instrument musical context", steps: [
        {name: "Record a phrase and retain stopped instrument monitoring", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1200)
            showTimingControls()
            api.dispatcher.dispatch("new-instrument-track")
            sleep(600)
            control("TrackViewSection", "Track " + Project.trackInfo(0).id + " Header Panel", "Piano roll")
            piano("pianoRollControl9")
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            sleep(450)
            api.keyboard.releaseKey("Z")
            piano("pianoRollControl4")
            check(Project.trackInfo(0).xml.indexOf("<midinote ") !== -1, "No recorded notes")
            waitFor(function(s) { return s.status === "stopped" && s.streamActive && s.streamKind === "monitoring" }, "Instrument monitoring did not remain active")
        }},
        {name: "Edit tempo and meter from the toolbar during stopped monitoring", func: function() {
            var before = Project.musicalContext()
            toolbar("BPMArrowUpButton")
            check(Project.musicalContext().tempo > before.tempo, "BPM toolbar did not update the project")
            meter(5, 8)
            check(state().streamActive && state().streamKind === "monitoring", "Meter edit lost stopped monitoring")
            // Focus the piano roll again after editing the toolbar.
            piano("pianoRollControl4")
            api.keyboard.pressKey("X")
            sleep(350)
            api.keyboard.releaseKey("X")
            piano("pianoRollControl4")
            check((Project.trackInfo(0).xml.match(/<midinote /g) || []).length >= 2, "Keyboard recording stopped working after timing edits")
        }},
        {name: "Odd, compound, half-note and dense meters preserve notes and label the grid", func: function() {
            var before = JSON.stringify(notes())
            meter(7, 8); ruler(6, "1.7", 288); ruler(7, "2", 336)
            meter(6, 8); ruler(5, "1.6", 240); ruler(6, "2", 288)
            meter(3, 2); ruler(2, "1.3", 384); ruler(3, "2", 576)
            meter(1, 64); ruler(8, "9", 48)
            check(JSON.stringify(notes()) === before, "Meter edits moved or resized the recorded notes")
            meter(5, 8)
        }},
        {name: "Change BPM during a take without changing existing notes or recorded duration", func: function() {
            var before = notes()
            piano("pianoRollControl4")
            var firstStart = Date.now()
            api.keyboard.pressKey("C")
            sleep(250)
            api.keyboard.releaseKey("C")
            var firstLength = (Date.now() - firstStart) / 1000
            sleep(100)
            toolbar("BPMArrowUpButton")
            check(JSON.stringify(notes()) === JSON.stringify(before), "A pending take entered the document before Stop")
            // Return computer-key focus after using the toolbar, retaining the take.
            piano("pianoRollControl9"); piano("pianoRollControl9")
            var secondStart = Date.now()
            api.keyboard.pressKey("V")
            sleep(300)
            api.keyboard.releaseKey("V")
            var secondLength = (Date.now() - secondStart) / 1000
            sleep(100)
            piano("pianoRollControl4")
            var after = notes()
            check(after.length === before.length + 2, "Changing BPM lost or duplicated recorded notes")
            for (var i = 0; i < before.length; ++i) {
                check(JSON.stringify(after[i]) === JSON.stringify(before[i]), "Changing BPM altered an existing document note")
            }
            var tempo = Project.musicalContext().tempo
            var first = after.find(function(n) { return n.pitch === 64 })
            var second = after.find(function(n) { return n.pitch === 65 })
            check(first && second, "Missing notes from the take with a BPM edit")
            check(Math.abs(first.length * 60 / tempo - firstLength) < 0.03, "The note before the BPM edit changed its recorded duration")
            check(Math.abs(second.length * 60 / tempo - secondLength) < 0.03, "The note after the BPM edit uses the old tempo")
        }},
        {name: "A longer take scrolls the ruler and grid without moving notes", func: function() {
            meter(7, 8)
            piano("pianoRollControl4")
            // Wait silently until the take extends beyond the visible grid.
            sleep(Math.ceil(15 * 60 / Project.musicalContext().tempo * 1000))
            api.keyboard.pressKey("M")
            sleep(100)
            api.keyboard.releaseKey("M")
            piano("pianoRollControl4")
            var view = api.testflow.itemWindowInfo("pianoRollGrid")
            check(view.contentX > 192, "The longer take did not follow the recording cursor")
            scrolledRuler()
            var image = api.testflow.captureMainWindow("native-scrolled-meter.png")
            check(image && api.testflow.fileSize(image) > 0, "Cannot capture the scrolled meter UI")
            api.log.info("Scrolled meter screenshot=" + image)
            var before = JSON.stringify(notes())
            for (var i = 0; i < 3 && api.testflow.itemWindowInfo("pianoRollGrid").contentX > 1; ++i) {
                check(api.testflow.mouseDragItem("pianoRollGrid", 0.1, 0.01, 0.9, 0.01), "Cannot scroll back through the piano roll")
                sleep(400)
                scrolledRuler()
            }
            check(api.testflow.itemWindowInfo("pianoRollGrid").contentX < 1, "The piano roll did not scroll back to the clip start")
            check(JSON.stringify(notes()) === before, "A scroll gesture changed the recorded notes")
            ruler(7, "2", 336)
            meter(5, 8)
        }},
        {name: "Edit meter during looped playback with tempo disabled", func: function() {
            var edit = Project.beginEdit("Musical context loop")
            check(edit.setSelection({start: 0, end: 1, tracks: []}), "Cannot set loop selection")
            edit.commit()
            api.dispatcher.dispatch("set-loop-region-to-selection")
            if (!state().loopActive) api.dispatcher.dispatch("toggle-loop-region")
            api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
            api.dispatcher.dispatch("action://playback/toggle-play-pause")
            waitFor(function(s) { return s.status === "running" && s.streamActive && s.position > 0.02 }, "Looped instrument playback did not start")
            var bpm = api.navigation.controls("PlaybackSection", "PlaybackToolBar").find(function(c) { return c.name === "BPMArrowUpButton" })
            check(bpm && !bpm.enabled, "BPM must stay fixed while the MIDI sequence is compiled for playback")
            meter(7, 4)
            ruler(4, "1.5", 384); ruler(7, "2", 672)
            check(state().status === "running" && state().streamActive && state().loopActive, "Meter edit interrupted looped playback")
            var before = state().position
            sleep(150)
            var after = state().position
            check(Math.abs(after - before) > 0.05, "Playhead stalled after meter edit")
            var image = api.testflow.captureMainWindow("native-live-meter.png")
            check(image && api.testflow.fileSize(image) > 0, "Cannot capture current meter UI")
            api.log.info("Live meter screenshot=" + image)
            api.dispatcher.dispatch("action://playback/stop")
            waitFor(function(s) { return s.status === "stopped" }, "Playback did not stop")
            api.dispatcher.dispatch("clear-loop-region")
        }},
        {name: "Save and reopen the edited project timing and recorded MIDI", func: function() {
            savedXml = Project.trackInfo(0).xml
            savedContext = JSON.stringify(Project.musicalContext())
            var destination = api.testflow.prepareSavingFile("Native musical context.aup4")
            check(destination, "No fresh save destination")
            api.testflow.after(100, handleSaveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            savedPath = api.testflow.selectedFilePath()
            check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Project timing was not saved")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
            sleep(1200)
            check(JSON.stringify(Project.musicalContext()) === savedContext, "Saved tempo or meter changed on reopen")
            check(Project.trackInfo(0).xml === savedXml, "Recorded MIDI changed on reopen")
            control("TrackViewSection", "Track " + Project.trackInfo(0).id + " Header Panel", "Piano roll")
            ruler(4, "1.5", 384); ruler(7, "2", 672)
            api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
            api.dispatcher.dispatch("action://playback/toggle-play-pause")
            waitFor(function(s) { return s.status === "running" && s.streamActive && s.position > 0.02 }, "Restored instrument did not play")
            api.dispatcher.dispatch("action://playback/stop")
            waitFor(function(s) { return s.status === "stopped" }, "Restored instrument did not stop")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Restored project did not close cleanly")
        }}
    ]})
}
