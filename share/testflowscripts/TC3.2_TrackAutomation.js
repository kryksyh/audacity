/*
 * Audacity: A Digital Audio Editor
 * Automation gestures use Quick window mouse/key events; assertions read the
 * actual document and audio produced by the public Mix and Render command.
 */
var Project = require("Audacity.Project")
var baseline = null
var renderEnd = 0

function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function info(index) { return Project.trackInfo(index) }
function control(section, panel, name) {
    for (var attempt = 0; attempt < 50; ++attempt) {
        var ready = api.navigation.controls(section, panel).some(function(item) { return item.name === name && item.enabled })
        if (ready) break
        sleep(100)
    }
    check(api.navigation.triggerControl(section, panel, name), "Missing UI control: " + panel + "/" + name)
    sleep(250)
}
function header(name) { control("TrackViewSection", "Track " + info(0).id + " Header Panel", name) }
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function automation(name) { control("NavigationBottomPanel", "TrackAutomationControls", name) }
function points() {
    var lane = info(0).xml.match(/<lane parameter="volume"[^>]*>([\s\S]*?)<\/lane>/)
    return lane ? lane[1].match(/<point\s[^>]*\/>/g) || [] : []
}
function number(tag, attr) {
    var match = tag.match(new RegExp(attr + '="([^"]+)"'))
    return match ? Number(match[1]) : NaN
}
function editNumber(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "TrackAutomationControls", name), "Missing numeric editor " + name)
    api.keyboard.key("A", "CTRL")
    api.keyboard.text(String(value))
    sleep(100)
    api.keyboard.key("Return")
    sleep(200)
    api.log.info(name + "=" + JSON.stringify(points()))
}
function render() {
    var before = Project.trackCount()
    api.dispatcher.dispatch("mix-render-to-new-track")
    sleep(600)
    check(Project.trackCount() === before + 1, "Mix and Render did not create a track")
    var clips = Project.clipsOnTrack(before)
    check(clips.length === 1, "Rendered audio clip missing")
    if (!renderEnd) renderEnd = clips[0].end
    var result = Project.waveformStatistics(before, 0, renderEnd)
    check(result && result.sampleCount > 0, "Cannot read rendered audio")
    return result
}

function main() {
    api.testflow.runTestCase({name: "TC3.2: Track automation UI and rendered audio", steps: [
        {name: "Create project", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1200)
        }},
        {name: "Record a MIDI phrase using the keyboard", func: function() {
            api.dispatcher.dispatch("new-instrument-track")
            sleep(450)
            header("Piano roll")
            piano("pianoRollControl9")
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            sleep(600)
            api.keyboard.releaseKey("Z")
            sleep(100)
            piano("pianoRollControl4")
            check((info(0).xml.match(/<midinote\s/g) || []).length === 1, "MIDI phrase was not recorded")
        }},
        {name: "Render a nonzero baseline", func: function() {
            baseline = render()
            check(baseline.peak > 0.001 && baseline.rms > 0.001, "Baseline MIDI render is silent")
        }},
        {name: "Add an automation point with a double-click", func: function() {
            header("trackAutomationButton")
            check(api.testflow.mouseClickItem("automationChart", 0.1, 0.6, "left", true), "Cannot double-click automation chart")
            sleep(250)
            check(points().length === 1, "Double-click did not add an automation point")
        }},
        {name: "Drag one point with one undo step", func: function() {
            var before = JSON.stringify(points())
            check(api.testflow.mouseDragItem("automationPoint0", 0.5, 0.5, 10.5, -5.5), "Cannot drag automation point")
            sleep(250)
            var after = JSON.stringify(points())
            check(before !== after, "Dragging did not move the point")
            api.dispatcher.dispatch("action://undo")
            sleep(300)
            check(JSON.stringify(points()) === before, "One undo did not restore the point before dragging")
            api.dispatcher.dispatch("action://redo")
            sleep(300)
            check(JSON.stringify(points()) === after, "Redo did not restore dragged point")
        }},
        {name: "Set time zero and fifty-percent volume numerically", func: function() {
            check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select the point")
            sleep(100)
            editNumber("automationPointTime", 0)
            editNumber("automationPointValue", 50)
            var point = points()[0]
            check(Math.abs(number(point, "time")) < 0.001, "Numeric time edit was not applied: " + point)
            var actualGain = number(point, "value") * 3.9810717055349722
            check(Math.abs(actualGain - 0.5) < 0.001, "Numeric volume edit was not applied: " + point)
        }},
        {name: "Rendered samples follow the automation", func: function() {
            var automated = render()
            check(Math.abs(automated.peak / baseline.peak - 0.5) < 0.002, "Automation did not halve the rendered peak")
            check(Math.abs(automated.rms / baseline.rms - 0.5) < 0.002, "Automation did not halve rendered RMS")
            api.log.info("Audio baseline=" + JSON.stringify(baseline) + ", automated=" + JSON.stringify(automated))
        }},
        {name: "Read bypass restores the baseline audio", func: function() {
            header("trackAutomationButton")
            automation("Read")
            check(info(0).xml.indexOf('parameter="volume" enabled="0"') !== -1, "Read bypass did not disable the lane")
            var bypassed = render()
            check(Math.abs(bypassed.peak / baseline.peak - 1.0) < 0.002, "Bypassed lane still changes rendered peak")
            check(Math.abs(bypassed.rms / baseline.rms - 1.0) < 0.002, "Bypassed lane still changes rendered RMS")
        }},
        {name: "Numeric point moves retain selection when points reorder", func: function() {
            header("trackAutomationButton")
            check(api.testflow.mouseClickItem("automationChart", 0.2, 0.6, "left", true), "Cannot add a second point")
            sleep(200)
            check(points().length === 2, "Second point missing")
            var neighbor = points()[1]
            check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select first point")
            sleep(100)
            editNumber("automationPointTime", number(neighbor, "time") + 1)
            editNumber("automationPointValue", 25)
            check(points()[0] === neighbor, "Editing the moved point changed its neighbor")
            check(Math.abs(number(points()[1], "value") * 3.9810717055349722 - 0.25) < 0.001,
                  "Selection did not follow the point after reordering")
        }},
        {name: "Clear lane supports undo and redo", func: function() {
            header("trackAutomationButton")
            var before = JSON.stringify(points())
            automation("Clear lane")
            check(points().length === 0, "Clear lane did not remove its points")
            api.dispatcher.dispatch("action://undo")
            sleep(300)
            check(JSON.stringify(points()) === before, "Undo did not restore cleared lane")
            api.dispatcher.dispatch("action://redo")
            sleep(300)
            check(points().length === 0, "Redo did not clear the lane")
        }}
    ]})
}
