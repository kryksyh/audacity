/*
 * Audacity: A Digital Audio Editor
 * Original built-in/Nyquist processing must preserve neighboring clips and
 * unselected tracks while MIDI, routing and instruments use the native engine.
 */
var Project = require("Audacity.Project")
var reference
var processed
var saveAttempts = 0
var originalMonitoring
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function waitFor(predicate, message, timeout) {
    var deadline = Date.now() + (timeout || 5000)
    do { if (predicate()) return; sleep(50) } while (Date.now() < deadline)
    check(false, message)
}
function select(start, end, indices) {
    var edit = Project.beginEdit("Original effects test selection")
    check(edit.setSelection({start: start, end: end, tracks: indices.map(function(i) { return edit.audioTracks[i] })}), "Cannot set effect selection")
    edit.commit()
    sleep(200)
}
function apply(title, params) {
    var query = "action://effects/apply?effectId=" + encodeURIComponent(title) + "&effectIdEncoding=percent"
    Object.keys(params || {}).forEach(function(key) { query += "&" + key + "=" + encodeURIComponent(params[key]) })
    api.dispatcher.dispatch(query)
    sleep(400)
}
function samples() {
    return {left: Project.waveformStatistics(0, 0, 0.5), middle: Project.waveformStatistics(0, 0.5, 1.5),
            right: Project.waveformStatistics(0, 1.5, 2), other: Project.waveformStatistics(1, 0, 2)}
}
function same(actual, expected, label) {
    check(actual && expected && actual.sha256 && actual.sha256 === expected.sha256 &&
          actual.sampleCount === expected.sampleCount && actual.channelCount === expected.channelCount &&
          Math.abs(actual.peak - expected.peak) < 1e-12 && Math.abs(actual.rms - expected.rms) < 1e-12,
          label + " samples changed: " + JSON.stringify(actual) + " vs " + JSON.stringify(expected))
}
function structure() {
    check(Project.trackCount() === 2, "An unselected track disappeared")
    var clips = Project.clipsOnTrack(0)
    check(clips.length === 3 && Math.abs(clips[0].start) < 1e-6 && Math.abs(clips[0].end - 0.5) < 1e-6 &&
          Math.abs(clips[1].start - 0.5) < 1e-6 && Math.abs(clips[1].end - 1.5) < 1e-6 &&
          Math.abs(clips[2].start - 1.5) < 1e-6 && Math.abs(clips[2].end - 2) < 1e-6,
          "Unselected clips or bounds changed: " + JSON.stringify(clips))
    check(Project.clipCount(1) === 1, "The unselected track's clip changed")
}
function preserved() {
    structure()
    var result = samples()
    same(result.left, reference.left, "Left neighboring clip")
    same(result.right, reference.right, "Right neighboring clip")
    same(result.other, reference.other, "Unselected track")
    return result
}
function saveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(s) { return s.name === "AskLocationTypeDialog" && s.enabled })
    if (section) check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    else if (++saveAttempts < 100) api.testflow.after(100, saveDialog)
    else check(false, "Save chooser did not open")
}
function main() {
    api.testflow.runTestCase({name: "TC3.11: Original built-in and Nyquist effects", steps: [
        {name: "Generate original Tone audio and retain three clips plus an unselected track", func: function() {
            api.testflow.showMainWindowOnFront()
            // Output-only regression: capture is tested separately. Do not let
            // automatic mic metering open a capture stream when Tone adds mono.
            originalMonitoring = Project.playbackInfo()
            if (originalMonitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (originalMonitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
            sleep(150)
            check(!Project.playbackInfo().micMetering && !Project.playbackInfo().inputMonitoring, "Cannot disable capture monitoring for the output-only test")
            api.dispatcher.dispatch("file-new")
            sleep(1400)
            select(0, 2, [])
            apply("Tone", {Frequency: 440, Amplitude: 0.1, Waveform: "Sine"})
            waitFor(function() { return Project.trackCount() === 1 && Project.totalTime() >= 1.99 }, "Original Tone generator produced no audio")
            var generated = Project.waveformStatistics(0, 0, 2)
            check(generated && Math.abs(generated.peak - 0.1) < 0.001 && Math.abs(generated.rms - 0.1 / Math.sqrt(2)) < 0.001,
                  "Original Tone samples are incorrect: " + JSON.stringify(generated))
            select(0, 2, [0])
            api.dispatcher.dispatch("duplicate")
            waitFor(function() { return Project.trackCount() === 2 }, "Duplicate did not create the reference track")
            select(0.5, 1.5, [0])
            api.dispatcher.dispatch("split") // Split both ends of the real time selection.
            sleep(300)
            structure()
            reference = samples()
            same(reference.middle, Project.waveformStatistics(1, 0.5, 1.5), "Duplicated original source")
        }},
        {name: "Apply built-in Fade In to only the middle clip and verify Undo Redo", func: function() {
            select(0.5, 1.5, [0])
            apply("Fade In")
            var result = preserved()
            var ratio = result.middle.rms / reference.middle.rms
            check(ratio > 0.56 && ratio < 0.59, "Built-in Fade In did not process the selected audio: " + ratio)
            processed = result
            api.dispatcher.dispatch("action://undo")
            sleep(350)
            same(preserved().middle, reference.middle, "Undo original Fade In")
            api.dispatcher.dispatch("action://redo")
            sleep(350)
            same(preserved().middle, processed.middle, "Redo original Fade In")
        }},
        {name: "Apply original Nyquist Tremolo without changing neighboring clips", func: function() {
            // Nyquist deliberately permits merging at the processing range's
            // edges. Keep those edges inside the middle clip so its existing
            // behavior cannot merge an adjacent clip at a selected boundary.
            select(0.75, 1.25, [0])
            var before = Project.waveformStatistics(0, 0.75, 1.25)
            var left = Project.waveformStatistics(0, 0.5, 0.75)
            var right = Project.waveformStatistics(0, 1.25, 1.5)
            apply("Tremolo", {WAVE: "Sine", PHASE: 0, WET: 100, LFO: 8})
            var result = preserved()
            same(Project.waveformStatistics(0, 0.5, 0.75), left, "Nyquist left unselected portion")
            same(Project.waveformStatistics(0, 1.25, 1.5), right, "Nyquist right unselected portion")
            var ratio = Project.waveformStatistics(0, 0.75, 1.25).rms / before.rms
            check(ratio > 0.1 && ratio < 0.9, "Original Nyquist Tremolo did not process audio: " + ratio)
            processed = result
        }},
        {name: "Apply a second built-in effect after Nyquist", func: function() {
            select(0.5, 1.5, [0])
            var before = samples().middle
            apply("Fade Out")
            var result = preserved()
            check(result.middle.rms > 0.00001 && result.middle.rms < before.rms * 0.9, "Fade Out after Nyquist did not process audio")
            processed = result
        }},
        {name: "Save close reopen and retain all processed and untouched samples", func: function() {
            check(api.testflow.prepareSavingFile("Original effects regression.aup4"), "No fresh save path")
            api.testflow.after(100, saveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            var path = api.testflow.selectedFilePath()
            check(path && api.testflow.fileSize(path) > 0, "Original effects project was not saved")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + path))
            waitFor(function() { return Project.trackCount() === 2 }, "Original effects project did not reopen")
            same(preserved().middle, processed.middle, "Saved processed audio")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Original effects project did not close")
            if (originalMonitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (originalMonitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
            sleep(150)
            check(Project.playbackInfo().micMetering === originalMonitoring.micMetering &&
                  Project.playbackInfo().inputMonitoring === originalMonitoring.inputMonitoring, "QA monitoring preferences were not restored")
        }}
    ]})
}
