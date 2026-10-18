/* Original-engine capture, paused clocks, MIDI overdub and project persistence.
 * Uses only the QA profile. Pass [{"device":"Mi 10in1 Audio Device"}]. */
var Project = require("Audacity.Project")
var originalDevice, originalChannels, first, second, firstEnd, secondEnd, notes, savedPath
var saveAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function waitFor(predicate, message, timeout) {
    var deadline = Date.now() + (timeout || 6000)
    do { if (predicate()) return; sleep(25) } while (Date.now() < deadline)
    check(false, message + ": " + JSON.stringify(Project.playbackInfo()))
}
function audioPanel() {
    var panels = api.navigation.panels("PreferencesDialog")
    for (var i = 0; i < panels.length; ++i)
        if (panels[i].enabled && api.navigation.controls("PreferencesDialog", panels[i].name).some(function(c) { return c.enabled && c.name === "AudioApiBox" })) return panels[i].name
    return null
}
function click(section, panel, name) {
    check(api.testflow.mouseClickControl(section, panel, name), "Cannot click " + panel + "/" + name)
    sleep(150)
}
function preferences(device, channels) {
    api.dispatcher.dispatch("command://app/audio-settings")
    waitFor(audioPanel, "Audio settings did not open")
    var panel = audioPanel()
    var controls = api.navigation.controls("PreferencesDialog", panel)
    if (!originalDevice) {
        originalDevice = controls.find(function(c) { return c.name === "RecordingDeviceBox" }).accessibleName.replace(/^Recording device /, "")
        originalChannels = controls.find(function(c) { return c.name === "RecordingChannelsBox" }).accessibleName.replace(/^Recording channels /, "")
        api.log.info("Original QA capture settings=" + JSON.stringify({device:originalDevice, channels:originalChannels}))
    }
    check(api.navigation.triggerControl("PreferencesDialog", panel, "RecordingDeviceBox"), "Cannot open input selector")
    sleep(200)
    check(api.navigation.triggerControl("Dropdown", "Dropdown", device), "Cannot select input " + device)
    sleep(200)
    check(api.navigation.triggerControl("PreferencesDialog", panel, "RecordingChannelsBox"), "Cannot open input channels selector")
    sleep(200)
    check(api.navigation.triggerControl("Dropdown", "Dropdown", channels), "Cannot select input channels " + channels)
    sleep(200)
    click("PreferencesDialog", "ButtonBox", "OK")
    waitFor(function() { return !audioPanel() }, "Audio settings did not close")
}
function command(name) { api.dispatcher.dispatch("command://record/" + name) }
function start(name) {
    var requested = Date.now()
    command(name)
    waitFor(function() { var s = Project.playbackInfo(); return s.streamKind === "recording" && s.streamOwned && s.streamActive && s.streamTime > 0.05 }, "Capture did not start")
    api.log.info(name + " first capture clock=" + (Date.now() - requested) + "ms " + JSON.stringify(Project.playbackInfo()))
}
function stop() {
    var requested = Date.now()
    command("stop")
    waitFor(function() { return !Project.playbackInfo().streamPresent }, "Capture stream leaked after Stop")
    api.log.info("Capture Stop=" + (Date.now() - requested) + "ms")
    sleep(200)
}
function end(index) {
    var clips = Project.clipsOnTrack(index)
    api.log.info("Capture clips=" + JSON.stringify(clips))
    check(clips.length > 0, "Recording committed no clip")
    return Math.max.apply(null, clips.map(function(c) { return c.endTime !== undefined ? c.endTime : c.end }))
}
function samples(index, duration) {
    var full = Project.waveformStatistics(index, 0, duration)
    var tail = Project.waveformStatistics(index, duration - 0.4, duration)
    api.log.info("Captured PCM=" + JSON.stringify({index:index, duration:duration, full:full, tail:tail}))
    check(full && full.peak > 0 && tail && tail.peak > 0, "Capture is silent or stopped receiving audio before the end")
    return full
}
function same(a, b, message) { check(a && b && a.sha256 === b.sha256 && a.sampleCount === b.sampleCount, message) }
function piano(name) { check(api.navigation.triggerControl("NavigationBottomPanel", "PianoRollControls", name), "Missing piano control " + name); sleep(200) }
function midiSource() { return Project.trackInfo(1).xml.replace(/ is(?:Selected|Focused)="[^"]*"/g, "") }
function saveDialog() {
    if (api.testflow.selectedFilePath()) return
    if (api.navigation.sections().some(function(s) { return s.enabled && s.name === "AskLocationTypeDialog" }))
        click("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer")
    else if (++saveAttempts < 100) api.testflow.after(100, saveDialog)
    else check(false, "Save dialog did not open")
}
function main(config) {
    config = config || {device:"Mi 10in1 Audio Device"}
    api.testflow.runTestCase({name:"TC3.17: Original audio capture and MIDI overdub", steps:[
        {name:"Choose a healthy USB input in QA preferences", func:function() {
            api.testflow.showMainWindowOnFront()
            check(!Project.playbackInfo().micMetering && !Project.playbackInfo().inputMonitoring, "QA must start with monitoring disabled")
            preferences(config.device, "1 (Mono) Recording channel")
            api.dispatcher.dispatch("file-new"); sleep(1200)
        }},
        {name:"Record, pause and resume without losing later samples", func:function() {
            start("start"); sleep(1100)
            var before = Project.playbackInfo()
            command("pause"); sleep(220)
            var settled = Project.playbackInfo()
            sleep(250)
            check(Math.abs(Project.playbackInfo().streamTime - settled.streamTime) < 0.01, "Paused recording clock keeps moving")
            command("pause"); sleep(1200)
            var after = Project.playbackInfo()
            check(after.streamTime > before.streamTime + 0.9 && after.position > before.position + 0.9, "Resumed recording/UI clocks did not advance")
            stop()
            check(Project.trackCount() === 1 && Project.trackInfo(0).type === "wave", "Recording did not commit a Wave track")
            firstEnd = end(0); check(firstEnd > 2, "Capture collapsed to a short initial fragment")
            first = samples(0, firstEnd)
        }},
        {name:"Add an independently recorded computer-keyboard MIDI take", func:function() {
            api.dispatcher.dispatch("new-instrument-track"); sleep(600)
            check(api.navigation.triggerControl("TrackViewSection", "Track " + Project.trackInfo(1).id + " Header Panel", "Piano roll"), "Cannot open piano roll")
            sleep(300); piano("pianoRollControl9"); piano("pianoRollControl4")
            api.keyboard.pressKey("Z"); sleep(600); api.keyboard.releaseKey("Z"); sleep(100); piano("pianoRollControl4")
            notes = midiSource()
            check(notes.indexOf("<midinote ") !== -1, "No MIDI phrase was recorded")
        }},
        {name:"Overdub audio while original Wave and MIDI tracks play", func:function() {
            api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
            start("on-new-track"); sleep(2200); stop()
            check(Project.trackCount() === 3 && Project.trackInfo(2).type === "wave", "No overdub Wave track")
            secondEnd = end(2); check(secondEnd > 1.5, "Overdub collapsed to a short fragment")
            second = samples(2, secondEnd)
            same(first, Project.waveformStatistics(0,0,firstEnd), "Overdub changed original audio")
            check(midiSource() === notes, "Overdub changed original MIDI")
            api.log.info("Capture scene=" + api.testflow.captureMainWindow("native-audio-midi-overdub.png"))
        }},
        {name:"Undo and redo the recorded overdub", func:function() {
            api.dispatcher.dispatch("action://undo"); sleep(300)
            check(Project.trackCount() < 3 || Project.clipsOnTrack(2).length === 0, "Undo retained overdub audio")
            same(first, Project.waveformStatistics(0,0,firstEnd), "Undo changed original audio")
            api.dispatcher.dispatch("action://redo"); sleep(300)
            same(second, Project.waveformStatistics(2,0,secondEnd), "Redo did not restore exact overdub PCM")
        }},
        {name:"Save and reopen audio plus MIDI through the normal project UI", func:function() {
            api.testflow.prepareSavingFile("native-audio-midi-overdub.aup4")
            api.testflow.after(100, saveDialog); api.dispatcher.dispatch("file-save"); sleep(4500)
            savedPath = api.testflow.selectedFilePath(); check(savedPath, "Project did not save")
            api.dispatcher.dispatch("file-close"); sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
            waitFor(function() { return Project.trackCount() === 3 }, "Overdub project did not reopen")
            same(first, Project.waveformStatistics(0,0,firstEnd), "Saved original audio changed")
            same(second, Project.waveformStatistics(2,0,secondEnd), "Saved overdub audio changed")
            check(midiSource() === notes, "Saved MIDI changed")
            api.dispatcher.dispatch("file-close"); sleep(700)
            check(Project.trackCount() === 0, "Project did not close")
        }},
        {name:"Restore QA capture preferences", func:function() { preferences(config.restoreDevice || originalDevice, originalChannels) }}
    ]})
}
