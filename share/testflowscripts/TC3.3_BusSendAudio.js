/*
 * Audacity: A Digital Audio Editor
 * Exercise routing controls and measure their actual Mix and Render output.
 */
var Project = require("Audacity.Project")
var baseline = null
var renderEnd = 0
var buses = []
var savedPath = ""
var savedTracks = []
var saveAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function info(index) { return Project.trackInfo(index) }
function control(section, panel, name) {
    // Project creation and dock restoration complete asynchronously.
    for (var attempt = 0; attempt < 50; ++attempt) {
        var ready = api.navigation.controls(section, panel).some(function(item) { return item.name === name && item.enabled })
        if (ready) break
        sleep(100)
    }
    check(api.navigation.triggerControl(section, panel, name), "Missing control: " + panel + "/" + name)
    sleep(200)
}
function header(index, name) { control("TrackViewSection", "Track " + info(index).id + " Header Panel", name) }
function bottom(panel, name) { control("NavigationBottomPanel", panel, name) }
function route(index) { header(index, "Route") }
function output(index, target) {
    route(index)
    bottom("TrackRoutingControls", "Dropdown")
    api.log.info("Output popup: " + api.navigation.activeSection() + "/" + api.navigation.activePanel() + "/" + api.navigation.activeControl())
    for (var i = 0; i < target; ++i) { api.navigation.down(); sleep(100) }
    api.log.info("Output selected: " + api.navigation.activeSection() + "/" + api.navigation.activePanel() + "/" + api.navigation.activeControl())
    api.navigation.trigger()
    sleep(250)
}
function number(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "TrackAutomationControls", name), "Missing editor: " + name)
    api.keyboard.key("A", "CTRL")
    api.keyboard.text(String(value))
    sleep(100)
    api.keyboard.key("Return")
    sleep(200)
}
function volume(index, percent) {
    header(index, "trackAutomationButton")
    var points = info(index).xml.match(/<point\s/g) || []
    if (points.length === 0) {
        check(api.testflow.mouseClickItem("automationChart", 0.1, 0.6, "left", true), "Cannot add volume point")
        sleep(200)
    }
    check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select volume point")
    sleep(100)
    number("automationPointTime", 0)
    number("automationPointValue", percent)
    var point = info(index).xml.match(/<point time="([^"]+)" value="([^"]+)"/)
    check(point && Math.abs(Number(point[1])) < 0.001 && Math.abs(Number(point[2]) * 398.1071705534972 - percent) < 0.001,
          "Volume point was not applied on track " + index)
}
function renderRatio(expected, message) {
    var before = Project.trackCount()
    api.dispatcher.dispatch("mix-render-to-new-track")
    sleep(600)
    check(Project.trackCount() === before + 1, "No rendered track: " + message)
    var clips = Project.clipsOnTrack(before)
    check(clips.length === 1, "No rendered clip: " + message)
    if (!renderEnd) renderEnd = clips[0].end
    var result = Project.waveformStatistics(before, 0, renderEnd)
    check(result && result.sampleCount > 0, "No rendered samples: " + message)
    if (!baseline) {
        check(result.peak > 0.001 && result.rms > 0.001, "Baseline is silent")
        baseline = result
    }
    check(Math.abs(result.peak / baseline.peak - expected) < 0.003, message + ": peak ratio " + result.peak / baseline.peak)
    check(Math.abs(result.rms / baseline.rms - expected) < 0.003, message + ": RMS ratio " + result.rms / baseline.rms)
    api.log.info(message + ": " + JSON.stringify(result))
    // Keep later UI controls visible and the source selection unchanged.
    api.dispatcher.dispatch("action://undo")
    sleep(300)
    check(Project.trackCount() === before, "Undo did not remove the rendered result")
}
function persistedData(index) {
    var xml = info(index).xml
    return JSON.stringify({
        name: info(index).name,
        notes: xml.match(/<midinote\s[^>]*\/>/g) || [],
        lanes: xml.match(/<lane\s[^>]*>[\s\S]*?<\/lane>/g) || [],
        route: xml.match(/<routing\s[^>]*>[\s\S]*?<\/routing>/) || xml.match(/<routing\s[^>]*\/>/) || []
    })
}
function handleSaveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(item) { return item.name === "AskLocationTypeDialog" && item.enabled })
    if (section) {
        check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    } else if (++saveAttempts < 100) {
        api.testflow.after(100, handleSaveDialog)
    } else {
        api.keyboard.key("Escape")
        api.testflow.error("Save dialog did not become available")
    }
}
function main() {
    api.testflow.runTestCase({name: "TC3.3: Bus and send rendered audio", steps: [
        {name: "Create and record source", func: function() {
            check(api.navigation.panels("Missing test section").length === 0, "Missing section should have no panels")
            check(api.navigation.controls("Missing test section", "Missing test panel").length === 0, "Missing panel should have no controls")
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1200)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(400)
            header(0, "Piano roll")
            bottom("PianoRollControls", "pianoRollControl9")
            bottom("PianoRollControls", "pianoRollControl4")
            api.keyboard.pressKey("Z")
            sleep(600)
            api.keyboard.releaseKey("Z")
            sleep(100)
            bottom("PianoRollControls", "pianoRollControl4")
            check((info(0).xml.match(/<midinote\s/g) || []).length === 1, "Source note missing")
            renderRatio(1, "Direct master baseline")
        }},
        {name: "Create two buses and route output", func: function() {
            route(0)
            bottom("TrackRoutingControls", "Add bus")
            bottom("TrackRoutingControls", "Add bus")
            check(Project.trackCount() === 3, "Missing buses")
            for (var i = 1; i <= 2; ++i) {
                var match = info(i).xml.match(/<routing bus="([^"]+)"/)
                check(match, "Bus identity missing")
                buses.push(match[1])
            }
            output(0, 1)
            check(info(0).xml.indexOf('output="' + buses[0] + '"') !== -1, "Source output was not routed: " + info(0).xml)
            renderRatio(1, "Unity bus preserves audio")
        }},
        {name: "Bus volume changes actual audio", func: function() {
            volume(1, 50)
            renderRatio(0.5, "Half bus volume")
        }},
        {name: "Source fader and post-fader send sum", func: function() {
            volume(0, 50)
            route(0)
            bottom("TrackRoutingControls", "routingSendEnabled1")
            check(info(0).xml.indexOf('bus="' + buses[1] + '" gain="1.00000000000000000" prefader="0"') !== -1,
                  "Post-fader send not enabled")
            renderRatio(0.75, "Half source through half bus plus unity post send")
        }},
        {name: "Pre-fader send ignores the source fader", func: function() {
            route(0)
            bottom("TrackRoutingControls", "routingSendPreFader1")
            check(info(0).xml.indexOf('prefader="1"') !== -1, "Pre-fader mode not stored")
            renderRatio(1.25, "Half source through half bus plus unity pre send")
        }},
        {name: "Send level changes output and supports undo", func: function() {
            route(0)
            check(api.testflow.mouseDragItem("routingSendGain1", 0.266, 0.5, 0, 0.5), "Cannot drag send slider")
            sleep(250)
            var send = info(0).xml.match(/<send bus="[^"]+" gain="([^"]+)"/)
            check(send && Number(send[1]) === 0, "Send slider did not set zero gain")
            renderRatio(0.25, "Zero send leaves the main bus")
            api.dispatcher.dispatch("action://undo")
            sleep(300)
            renderRatio(1.25, "Undo restores send gain")
        }},
        {name: "Bus mute silences only its branch", func: function() {
            header(1, "trackMuteButton")
            renderRatio(1, "Muted output bus leaves pre send")
            header(2, "trackMuteButton")
            renderRatio(0, "Both buses muted")
            header(1, "trackMuteButton")
            header(2, "trackMuteButton")
            renderRatio(1.25, "Unmuted buses restore sum")
        }},
        {name: "Bus solo includes its source and excludes other output", func: function() {
            header(1, "trackSoloButton")
            renderRatio(0.25, "Solo output bus excludes the other bus")
            header(1, "trackSoloButton")
            renderRatio(1.25, "Solo off restores both branches")
        }},
        {name: "Bus feedback is rejected without changing the graph", func: function() {
            output(1, 1) // Bus 1's first available bus is Bus 2.
            check(info(1).xml.indexOf('output="' + buses[1] + '"') !== -1, "Bus chain not applied")
            output(2, 1) // Bus 2 -> Bus 1 would close the loop.
            check(info(2).xml.indexOf('output="' + buses[0] + '"') === -1, "Feedback route was accepted")
            renderRatio(1.25, "Rejected feedback preserves summed audio")
        }},
        {name: "Save routing and automation with the project UI", func: function() {
            for (var i = 0; i < 3; ++i) savedTracks.push(persistedData(i))
            var destination = api.testflow.prepareSavingFile("Bus and send audio.aup4")
            check(destination, "No fresh save destination")
            api.testflow.after(100, handleSaveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            savedPath = api.testflow.selectedFilePath()
            check(savedPath === destination && api.testflow.fileSize(savedPath) > 0, "Project was not saved at its fresh destination")
            api.log.info("Saved routing project: " + savedPath)
        }},
        {name: "Close and reopen the saved routing project", func: function() {
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Project did not close")
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
            sleep(1200)
            check(Project.trackCount() === 3, "Saved project did not reopen")
            for (var i = 0; i < 3; ++i) {
                check(persistedData(i) === savedTracks[i], "Save/reopen changed routing, automation or MIDI on track " + i)
            }
        }},
        {name: "Reopened routes and automation reproduce the same audio", func: function() {
            renderRatio(1.25, "Saved routing and automation audio")
            api.dispatcher.dispatch("file-save")
            sleep(400)
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Reopened routing project did not close cleanly")
        }}
    ]})
}
