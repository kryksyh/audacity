/*
 * Audacity: A Digital Audio Editor
 * Instrument automation, live monitoring, manual parameter controls and mix.
 */
var Project = require("Audacity.Project")
var baseline = null
var renderEnd = 0
var saveAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control: " + panel + "/" + JSON.stringify(name))
    sleep(250)
}
function header(name) { control("TrackViewSection", "Track " + info().id + " Header Panel", name) }
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function automation(name) { control("NavigationBottomPanel", "TrackAutomationControls", name) }
function number(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "TrackAutomationControls", name), "Missing editor " + name)
    api.keyboard.key("A", "CTRL")
    api.keyboard.text(String(value))
    sleep(100)
    api.keyboard.key("Return")
    sleep(200)
}
function manualVolume() {
    var state = info().xml.match(/instrumentstate="([^"]*)"/)
    check(state, "Instrument state missing")
    if (!state[1]) return 0.5 // The built-in instrument's unsaved default preset.
    return JSON.parse(state[1].replace(/&quot;/g, '"').replace(/&amp;/g, "&"))["0"]
}
function persistedState() {
    var xml = info().xml
    return JSON.stringify({instrument: xml.match(/instrumentstate="([^"]*)"/)[1],
                           notes: xml.match(/<midinote\s[^>]*\/>/g) || [],
                           lanes: xml.match(/<lane\b[^>]*>[\s\S]*?<\/lane>/g) || []})
}
function handleSaveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(item) { return item.name === "AskLocationTypeDialog" && item.enabled })
    if (section) check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    else if (++saveAttempts < 100) api.testflow.after(100, handleSaveDialog)
    else api.testflow.error("Save dialog did not open")
}
function renderRatio(expected) {
    var before = Project.trackCount()
    api.dispatcher.dispatch("mix-render-to-new-track")
    sleep(650)
    check(Project.trackCount() === before + 1, "No rendered track")
    var clips = Project.clipsOnTrack(before)
    check(clips.length === 1, "No rendered clip")
    if (!renderEnd) renderEnd = clips[0].end
    var result = Project.waveformStatistics(before, 0, renderEnd)
    check(result && result.rms > 0.001, "Rendered phrase is silent")
    if (!baseline) baseline = result
    check(Math.abs(result.peak / baseline.peak - expected) < 0.003, "Unexpected instrument automation peak ratio: " + result.peak / baseline.peak)
    check(Math.abs(result.rms / baseline.rms - expected) < 0.003, "Unexpected instrument automation RMS ratio: " + result.rms / baseline.rms)
    api.log.info("Instrument render ratio=" + expected + ", audio=" + JSON.stringify(result))
    api.dispatcher.dispatch("action://undo")
    sleep(350)
}
function main() {
    api.testflow.runTestCase({name: "TC3.5: Instrument automation and manual controls", steps: [
        {name: "Record source and render the default instrument", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1500)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(700)
            header("Piano roll")
            piano("pianoRollControl9")
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            sleep(600)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            renderRatio(1)
        }},
        {name: "Create an instrument volume automation lane", func: function() {
            header("trackAutomationButton")
            automation([-1, 1])
            control("Dropdown", "Dropdown", "Instrument")
            check(api.testflow.mouseClickItem("automationChart", 0.1, 0.6, "left", true), "Cannot add instrument automation point")
            sleep(200)
            check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select the automation point")
            sleep(100)
            number("automationPointTime", 0)
            number("automationPointValue", 25)
            check(info().xml.indexOf('parameter="instrument/0" enabled="1"') !== -1, "Instrument lane was not stored")
            renderRatio(0.5)
        }},
        {name: "Edit the manual volume while monitoring with automation", func: function() {
            header("Piano roll")
            // Recording left the computer-keyboard toggle on; reopen live input.
            piano("pianoRollControl9")
            piano("pianoRollControl9")
            piano("pianoRollControl3")
            var rect = api.testflow.itemRect("pianoRollParameter0")
            check(rect, "Instrument volume slider missing")
            var target = (7 + 0.75 * (rect.width - 14)) / rect.width
            check(api.testflow.mouseDragItem("pianoRollParameter0", 0.5, 0.5, target, 0.5), "Cannot edit instrument volume")
            sleep(300)
            check(Math.abs(manualVolume() - 0.75) < 0.002, "Manual volume was not changed")
            api.dispatcher.dispatch("action://undo")
            sleep(350)
            check(Math.abs(manualVolume() - 0.5) < 0.001, "One undo did not restore the volume slider gesture")
            api.dispatcher.dispatch("action://redo")
            sleep(350)
            check(Math.abs(manualVolume() - 0.75) < 0.002, "Redo did not restore manual volume")
        }},
        {name: "Muse keyboard navigation edits the parameter", func: function() {
            header("Piano roll")
            check(api.navigation.goToControl("NavigationBottomPanel", "PianoRollControls", "pianoRollParameter0"), "Cannot focus instrument parameter")
            api.navigation.up()
            sleep(300)
            check(Math.abs(manualVolume() - 0.751) < 0.00001, "Keyboard navigation did not change instrument volume: " + manualVolume())
            api.dispatcher.dispatch("action://undo")
            sleep(350)
            check(Math.abs(manualVolume() - 0.75) < 0.00001, "Undo did not restore keyboard parameter edit")
        }},
        {name: "Rendering automation preserves the edited manual baseline", func: function() {
            renderRatio(0.5)
            check(Math.abs(manualVolume() - 0.75) < 0.002, "Automation overwrote the manual preset value")
        }},
        {name: "Read bypass uses the edited manual instrument value", func: function() {
            header("trackAutomationButton")
            automation("Read")
            check(info().xml.indexOf('parameter="instrument/0" enabled="0"') !== -1, "Read bypass not stored")
            renderRatio(1.5)
            check(Math.abs(manualVolume() - 0.75) < 0.002, "Bypass changed manual instrument state")
        }},
        {name: "Save and reopen manual preset and automation, then render again", func: function() {
            var expected = persistedState()
            var destination = api.testflow.prepareSavingFile("Instrument automation.aup4")
            check(destination, "No fresh save destination")
            api.testflow.after(100, handleSaveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            var savedPath = api.testflow.selectedFilePath()
            check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Instrument automation project was not saved")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
            sleep(1200)
            check(Project.trackCount() === 1 && persistedState() === expected, "Reopening changed the manual preset, MIDI notes or automation")
            renderRatio(1.5)
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Instrument automation project did not close cleanly")
        }}
    ]})
}
