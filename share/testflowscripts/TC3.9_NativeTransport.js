/*
 * Audacity: A Digital Audio Editor
 * Exercise the real original-engine transport, its UI clock and device stream.
 */
var Project = require("Audacity.Project")
var timings = []
var saveAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function state() { return Project.playbackInfo() }
function action(name) { api.dispatcher.dispatch("action://playback/" + name) }
function waitFor(predicate, message, timeout) {
    var deadline = Date.now() + (timeout || 2500)
    var value
    do {
        value = state()
        if (predicate(value)) return value
        sleep(20)
    } while (Date.now() < deadline)
    check(false, message + ": " + JSON.stringify(value))
}
function timedAction(name, predicate, label) {
    var start = Date.now()
    action(name)
    var value = waitFor(predicate, label)
    var elapsed = Date.now() - start
    timings.push({action: label, ms: elapsed})
    api.log.info(label + " took " + elapsed + "ms: " + JSON.stringify(value))
    return value
}
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control: " + panel + "/" + name)
    sleep(200)
}
function header(index, name) {
    control("TrackViewSection", "Track " + Project.trackInfo(index).id + " Header Panel", name)
}
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function seek(time, playing) {
    action("seek?seekTime=" + time + "&triggerPlay=" + (playing ? "true" : "false"))
}
function selectRange(start, end) {
    var edit = Project.beginEdit("Transport test selection")
    check(edit.setSelection({start: start, end: end, tracks: []}), "Cannot set time selection")
    edit.commit()
    check(Math.abs(Project.selection.start - start) < 0.001 && Math.abs(Project.selection.end - end) < 0.001, "Time selection was not committed")
}
function checkProgress(label) {
    var before = waitFor(function(s) { return s.status === "running" && s.streamActive && s.position > 0.05 }, label + " did not start")
    check(before.streamKind === "playback" && before.streamRate > 0, label + " has no playback stream")
    // The original Wave stream can publish a 100 ms callback quantum. Wait
    // for a bounded amount of real progress instead of sampling between ticks.
    var after = waitFor(function(s) {
        return s.status === "running" && s.position > before.position + 0.07 && s.streamTime > before.streamTime + 0.07
    }, label + " playback clocks did not advance", 1000)
    check(after.status === "running" && after.position > before.position + 0.07, label + " playhead did not advance: " + JSON.stringify(after))
    check(after.streamTime > before.streamTime + 0.07, label + " device clock did not advance")
    check(Math.abs(after.position - after.streamTime) < 0.2, label + " UI and device clocks diverged: " + JSON.stringify(after))
}
function pauseResumeStop(label) {
    seek(0, false)
    var requested = Date.now()
    timedAction("toggle-play-pause", function(s) { return s.status === "running" && s.streamActive }, label + " start")
    waitFor(function(s) { return s.position > 0.025 }, label + " first device-clock advance")
    timings.push({action: label + " first device-clock advance", ms: Date.now() - requested})
    checkProgress(label)
    var paused = timedAction("pause", function(s) { return s.status === "paused" }, label + " pause")
    // The existing engine fades one final buffer and reports its actual DAC
    // position. Allow its 100 ms callback quantum plus the virtual device
    // queue to drain, then verify that the paused clock remains stationary.
    sleep(250)
    var settled = state()
    api.log.info(label + " paused clock after queued audio: " + JSON.stringify(settled))
    check(settled.status === "paused" && Math.abs(settled.position - paused.position) < 0.25, label + " excessive queued audio after pause")
    sleep(180)
    check(Math.abs(state().position - settled.position) < 0.002, label + " playhead moves after the pause fade settled: " + JSON.stringify(state()))
    timedAction("toggle-play-pause", function(s) { return s.status === "running" && s.streamActive }, label + " resume")
    waitFor(function(s) { return s.status === "running" && s.position > settled.position + 0.05 }, label + " resume did not advance", 1000)
    var stopped = timedAction("stop", function(s) { return s.status === "stopped" }, label + " stop")
    check(Math.abs(stopped.position) < 0.025, label + " stop did not restore seek anchor")
    check(stopped.streamKind !== "playback", label + " playback stream leaked after stop")
}
function seekAndRange(label) {
    seek(0, false)
    action("toggle-play-pause")
    waitFor(function(s) { return s.status === "running" && s.streamActive }, label + " seek source did not start")
    // Alternate distant forward/backward targets. Check the device clock,
    // since the UI can move its requested cursor before the callback seeks.
    var targets = [1.0, 0.15, 1.0, 0.15, 0.8]
    targets.forEach(function(target) {
        var requested = Date.now()
        seek(target, true)
        var result = waitFor(function(s) {
            return s.status === "running" && s.streamActive
                && s.streamTime >= target + 0.025 && s.streamTime < target + 0.4
        }, label + " running seek failed at " + target)
        var elapsed = Date.now() - requested
        timings.push({action: label + " seek to " + target, ms: elapsed})
        api.log.info(label + " seek device-clock advance took " + elapsed + "ms: " + JSON.stringify(result))
    })
    action("stop")
    var stopped = waitFor(function(s) { return s.status === "stopped" }, label + " seek stop failed")
    check(Math.abs(stopped.position - 0.8) < 0.025, label + " running seek anchor was not retained")
    selectRange(0.2, 0.65)
    action("play-selection")
    waitFor(function(s) { return s.status === "running" && s.position >= 0.2 && s.position < 0.65 }, label + " range did not start")
    var end = waitFor(function(s) { return s.status === "stopped" }, label + " selected range did not stop", 3000)
    check(Math.abs(end.position - 0.65) < 0.025, label + " range ended at the wrong position: " + JSON.stringify(end))
    api.log.info(label + " range end: " + JSON.stringify(end))
}
function loop(label) {
    selectRange(0.2, 0.5)
    api.dispatcher.dispatch("set-loop-region-to-selection")
    if (!state().loopActive) api.dispatcher.dispatch("toggle-loop-region")
    check(state().loopActive && Math.abs(state().loopStart - 0.2) < 0.001 && Math.abs(state().loopEnd - 0.5) < 0.001, label + " loop bounds not set")
    seek(0.2, false)
    action("toggle-play-pause")
    // A newly opened virtual/device stream may report active before its first
    // DAC clock update. Observe actual playback before counting loop wraps.
    waitFor(function(s) { return s.status === "running" && s.streamActive && s.position > 0.225 }, label + " loop clock did not start")
    var previous = state().position
    var wraps = 0
    var deadline = Date.now() + 5000
    while (wraps < 3 && Date.now() < deadline) {
        sleep(80)
        var current = state()
        check(current.status === "running" && current.position >= 0.17 && current.position <= 0.54, label + " loop escaped: " + JSON.stringify(current))
        if (current.position < previous - 0.08) ++wraps
        previous = current.position
    }
    check(wraps >= 2, label + " did not wrap repeatedly: " + wraps)
    api.log.info(label + " loop wraps=" + wraps)
    action("stop")
    api.dispatcher.dispatch("clear-loop-region")
    check(!state().loopActive, label + " loop was not cleared")
}
function handleSaveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(item) { return item.name === "AskLocationTypeDialog" && item.enabled })
    if (section) check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    else if (++saveAttempts < 100) api.testflow.after(100, handleSaveDialog)
    else api.testflow.error("Save dialog did not open")
}
function main() {
    api.testflow.runTestCase({name: "TC3.9: Native MIDI and Wave transport", steps: [
        {name: "Record a quiet instrument phrase", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1500)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(700)
            header(0, "Piano roll")
            piano("pianoRollControl9")
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            sleep(600)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            check(Project.trackInfo(0).xml.indexOf("<midinote ") !== -1 && Project.totalTime() >= 1.5, "No recorded phrase")
            var noteId = Project.trackInfo(0).xml.match(/<midinote\s[^>]*id="([^"]*)"/)[1]
            var note = api.testflow.itemRect("pianoRollNote" + noteId)
            var grid = api.testflow.itemRect("pianoRollGrid")
            check(note && grid && note.y >= grid.y && note.y + note.height <= grid.y + grid.height, "Recorded note is outside the piano-roll viewport")
            var image = api.testflow.captureMainWindow("native-piano-roll.png")
            check(image && api.testflow.fileSize(image) > 0, "Could not capture the piano roll scene")
            api.log.info("Native piano roll screenshot=" + image)
        }},
        {name: "MIDI play, UI clock, pause, resume and stop", func: function() { pauseResumeStop("MIDI") }},
        {name: "MIDI seek and selected time range", func: function() { seekAndRange("MIDI") }},
        {name: "MIDI short loop", func: function() { loop("MIDI") }},
        {name: "Render and remove the instrument through its track header", func: function() {
            api.dispatcher.dispatch("select-all")
            sleep(150) // Selection and command availability are delivered asynchronously.
            api.dispatcher.dispatch("mix-render-to-new-track")
            sleep(800)
            check(Project.trackCount() === 2 && Project.trackInfo(1).type === "wave", "No rendered Wave track")
            var samples = Project.waveformStatistics(1, 0, Project.totalTime())
            check(samples && samples.peak > 0.001 && samples.rms > 0.001, "Rendered source is silent")
            header(0, "Piano roll")
            piano("pianoRollControl9")
            header(0, [-1, 2]) // Track menu follows the editable track title.
            api.keyboard.key("Escape")
            sleep(150)
            api.dispatcher.dispatch("track-delete")
            sleep(500)
            check(Project.trackCount() === 1 && Project.trackInfo(0).type === "wave", "Deleting the MIDI track did not retain only Wave audio")
        }},
        {name: "Wave play, UI clock, pause, resume and stop", func: function() { pauseResumeStop("Wave") }},
        {name: "Wave seek, selected time range and short loop", func: function() { seekAndRange("Wave"); loop("Wave") }},
        {name: "Repeated Wave start and stop", func: function() {
            for (var i = 0; i < 4; ++i) {
                seek(0, false)
                timedAction("toggle-play-pause", function(s) { return s.status === "running" && s.streamActive }, "Wave repeated start " + i)
                waitFor(function(s) { return s.status === "running" && s.position > 0.025 }, "Repeated playback clock did not start")
                timedAction("stop", function(s) { return s.status === "stopped" && !s.streamActive }, "Wave repeated stop " + i)
            }
            api.log.info("Transport timings=" + JSON.stringify(timings))
        }},
        {name: "Save, reopen, play the original Wave source, close cleanly", func: function() {
            var destination = api.testflow.prepareSavingFile("Native transport.aup4")
            check(destination, "No fresh save destination")
            api.testflow.after(100, handleSaveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            var savedPath = api.testflow.selectedFilePath()
            check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Transport project was not saved")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
            sleep(1200)
            check(Project.trackCount() === 1 && Project.trackInfo(0).type === "wave", "Reopened transport project has wrong tracks")
            pauseResumeStop("Reopened Wave")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Transport project did not close cleanly")
        }}
    ]})
}
