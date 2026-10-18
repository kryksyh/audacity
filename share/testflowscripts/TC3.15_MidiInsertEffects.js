/*
 * Audacity: A Digital Audio Editor
 * MIDI inserts use the original effects panel, floating Muse editor, history,
 * native renderer and real project files. No direct effect/model mutations.
 */
var Project = require("Audacity.Project")
var title = ""
var baseline
var sourceEnd
var manualGain
var expectedSource
var frozenHash
var frozenEnd
var savedPath
var monitoring
var saveAttempts = 0
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function check(ok, message) {
    if (ok) return
    api.log.info("MIDI insert UI=" + JSON.stringify(api.navigation.sections().filter(function(s) { return s.enabled })))
    api.log.info("MIDI insert controls=" + JSON.stringify(api.navigation.accessibleControls().filter(function(c) { return c.enabled })))
    api.log.info("MIDI insert XML=" + (Project.trackCount() ? info().xml : "none"))
    api.log.info("MIDI insert screenshot=" + api.testflow.captureMainWindow("midi-insert-failure.png"))
    if (api.testflow.itemWindowInfo("effectParameterGain")) api.log.info("MIDI insert floating failure=" + api.testflow.captureItemWindow("effectParameterGain", "midi-insert-floating-failure.png"))
    api.testflow.error(message)
}
function waitFor(predicate, message, timeout) {
    var end = Date.now() + (timeout || 5000)
    do { var value = predicate(); if (value) return value; sleep(50) } while (Date.now() < end)
    check(false, message)
}
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control: " + panel + "/" + JSON.stringify(name))
    sleep(250)
}
function header(name) { control("TrackViewSection", "Track " + info().id + " Header Panel", name) }
function automation(name) { control("NavigationBottomPanel", "TrackAutomationControls", name) }
function undo() { api.dispatcher.dispatch("action://undo"); sleep(350) }
function redo() { api.dispatcher.dispatch("action://redo"); sleep(350) }
function effect() { return (info().xml.match(/<effect\b[^>]*>[\s\S]*?<\/effect>/) || [""])[0] }
function gain() {
    var match = effect().match(/<parameter name="Gain" value="([^"]+)"/)
    check(match, "Missing stored manual insert gain")
    return Number(match[1])
}
function lanes() { return JSON.stringify(effect().match(/<lane\b[^>]*>[\s\S]*?<\/lane>/g) || []) }
function source() {
    return JSON.stringify({notes: info().xml.match(/<midinote\s[^>]*\/>/g) || [], effect: effect(),
        instrument: (info().xml.match(/instrumentstate="([^"]*)"/) || ["", ""])[1]})
}
function menu(names) {
    if (!Array.isArray(names)) names = [names]
    return waitFor(function() {
        for (var i = 0; i < names.length; ++i) {
            if (api.navigation.triggerAccessibleControl(names[i]) || api.navigation.triggerAccessibleControl(names[i] + " Menu")) {
                sleep(300); return names[i]
            }
        }
        return false
    }, "Missing menu: " + names.join(" or "))
}
function openEditor() {
    header("Effects")
    var panel = api.testflow.itemRect("tracksPanelContent")
    check(panel && panel.width >= 480, "Effects panel squeezed out track controls: " + JSON.stringify(panel))
    check(api.testflow.mouseClickItem("realtimeEffectEditor-track-0"), "Cannot open MIDI insert editor from its row")
    var window = waitFor(function() { return api.testflow.itemWindowInfo("effectParameterGain") }, "No floating insert editor")
    // The loader creates its controls before the floating window's Column has
    // polished their final positions below the presets bar.
    sleep(300)
    window = api.testflow.itemWindowInfo("effectParameterGain")
    check(!window.mainWindow && window.parentIsMain && !window.modal, "MIDI insert editor is not a floating nonmodal main-window child: " + JSON.stringify(window))
    check(window.title.indexOf("Instrument") !== -1, "MIDI insert editor has the wrong owner title: " + window.title)
}
function closeEditor() {
    check(api.testflow.closeItemWindow("effectParameterGain"), "Cannot close MIDI insert editor")
    waitFor(function() { return !api.testflow.itemWindowInfo("effectParameterGain") }, "Insert editor stayed open")
}
function render(expectedRatio) {
    api.dispatcher.dispatch("select-all"); sleep(200)
    check(Project.trackCount() === 1, "Unexpected render source count")
    api.dispatcher.dispatch("mix-render-to-new-track")
    waitFor(function() { return Project.trackCount() === 2 }, "MIDI insert did not render", 20000)
    var result = Project.waveformStatistics(1, 0, sourceEnd)
    check(result && result.rms > 0.00001, "MIDI insert render is silent")
    if (!baseline) baseline = result
    check(Math.abs(result.rms / baseline.rms - expectedRatio) < 0.003 &&
          Math.abs(result.peak / baseline.peak - expectedRatio) < 0.003,
          "Incorrect insert PCM ratio " + expectedRatio + ": " + JSON.stringify(result) + " baseline=" + JSON.stringify(baseline))
    api.log.info("MIDI insert rendered ratio=" + expectedRatio + " audio=" + JSON.stringify(result))
    undo()
    check(Project.trackCount() === 1, "Undo did not remove rendered audio")
    return result
}
function number(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "TrackAutomationControls", name), "Missing automation number " + name)
    api.keyboard.key("A", "CTRL"); api.keyboard.text(String(value)); sleep(100); api.keyboard.key("Return"); sleep(200)
}
function saveDialog() {
    if (api.testflow.selectedFilePath()) return
    if (api.navigation.sections().some(function(s) { return s.enabled && s.name === "AskLocationTypeDialog" })) {
        check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    } else if (++saveAttempts < 100) api.testflow.after(100, saveDialog)
    else check(false, "Save chooser did not open")
}
function reopen() {
    api.dispatcher.dispatch("file-close"); sleep(700)
    api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + savedPath))
    waitFor(function() { return Project.trackCount() === 1 }, "MIDI insert project did not reopen", 10000)
}
function main() {
    api.testflow.runTestCase({name: "TC3.15: MIDI insert effects and history", steps: [
        {name: "Draw a docked MIDI note and render its original instrument", func: function() {
            api.testflow.showMainWindowOnFront()
            monitoring = Project.playbackInfo()
            if (monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
            api.dispatcher.dispatch("file-new"); sleep(1400)
            api.dispatcher.dispatch("new-instrument-track"); sleep(500)
            header("Piano roll")
            var grid = api.testflow.itemRect("pianoRollGrid")
            check(grid && api.testflow.mouseClickItem("pianoRollGrid", 48 / grid.width, 45 / grid.height, "left", true), "Cannot draw MIDI source")
            sleep(250)
            check((info().xml.match(/<midinote\s/g) || []).length === 1, "MIDI note missing")
            sourceEnd = Project.totalTime()
            render(1)
        }},
        {name: "Add a stock insert through the MIDI effects panel and undo redo it", func: function() {
            header("Effects")
            // Track and master buttons share a navigation name. Their
            // accessible names identify the actual target on every platform.
            check(api.navigation.triggerAccessibleControl("Add effect"), "Cannot open the track insert menu")
            sleep(250)
            menu("Audacity"); title = menu(["Bass and Treble", "Bass and treble"])
            waitFor(function() { return effect() !== "" }, "MIDI insert was not added")
            if (api.testflow.itemWindowInfo("effectParameterGain")) closeEditor()
            api.keyboard.key("Escape"); sleep(150)
            undo(); check(effect() === "", "Undo did not remove MIDI insert")
            redo(); check(effect() !== "", "Redo did not restore MIDI insert")
            check(gain() === 0, "Unexpected initial insert gain")
            render(1)
        }},
        {name: "Edit a manual value in the floating Muse insert editor", func: function() {
            openEditor()
            api.log.info("MIDI insert gain rectangle=" + JSON.stringify(api.testflow.itemRect("effectParameterGain")))
            check(api.testflow.mouseDragItem("effectParameterGain", 0.5, 0.2, 0.1, 0.2), "Cannot drag insert output gain")
            sleep(350)
            manualGain = gain()
            check(manualGain < -0.1, "Floating insert editor did not change manual gain")
            api.log.info("MIDI insert actual floating editor=" + api.testflow.captureItemWindow("effectParameterGain", "midi-insert-floating.png"))
            closeEditor(); openEditor(); check(gain() === manualGain, "Reopening insert editor changed manual gain"); closeEditor()
            render(Math.pow(10, manualGain / 20))
        }},
        {name: "Automate insert gain and restore its lane through undo redo", func: function() {
            header("trackAutomationButton"); automation([-1, 1]); control("Dropdown", "Dropdown", title)
            automation([-1, 2]); control("Dropdown", "Dropdown", "Gain")
            check(api.testflow.mouseClickItem("automationChart", 0.1, 0.5, "left", true), "Cannot draw insert automation")
            sleep(200); check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select insert point")
            number("automationPointTime", 0); number("automationPointValue", 40)
            var expected = lanes()
            automation("Clear lane"); check(lanes() === "[]", "Insert lane was not cleared")
            undo(); check(lanes() === expected, "Undo did not restore insert lane")
            redo(); check(lanes() === "[]", "Redo did not clear insert lane")
            undo(); check(lanes() === expected, "Second undo did not restore insert lane")
            render(Math.pow(10, -6 / 20))
            header("trackAutomationButton"); automation("Read")
            render(Math.pow(10, manualGain / 20))
            // Rendering itself was undone; Read is again the next history edit.
            undo(); check(lanes() === expected, "Undo did not restore insert Read")
            render(Math.pow(10, -6 / 20))
            check(gain() === manualGain, "Automation overwrote the manual insert value")
            api.log.info("MIDI insert actual main scene=" + api.testflow.captureMainWindow("midi-insert-automation.png"))
        }},
        {name: "Keep physical playback running while editing the floating MIDI insert", func: function() {
            var edit = Project.beginEdit("MIDI insert playback range")
            check(edit.setSelection({start: 0, end: Math.min(sourceEnd, 1.5), tracks: []}), "Cannot select MIDI loop time range")
            edit.commit(); sleep(200)
            api.dispatcher.dispatch("set-loop-region-to-selection")
            if (!Project.playbackInfo().loopActive) api.dispatcher.dispatch("toggle-loop-region")
            api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
            var start = Date.now()
            api.dispatcher.dispatch("action://playback/toggle-play-pause")
            waitFor(function() { var s = Project.playbackInfo(); return s.status === "running" && s.streamActive && s.streamOwned }, "Inserted MIDI playback did not start")
            var startMs = Date.now() - start
            check(startMs < 500, "Inserted MIDI playback start took " + startMs + " ms")
            waitFor(function() { return Project.playbackInfo().position > 0.05 }, "Inserted MIDI playhead did not advance")
            openEditor()
            var oldGain = manualGain
            check(api.testflow.mouseDragItem("effectParameterGain", 0.5, 0.2, 0.3, 0.2), "Cannot edit insert during playback")
            sleep(200); manualGain = gain()
            check(manualGain !== oldGain, "Live insert edit did not update the manual value")
            check(Project.playbackInfo().streamActive && Project.playbackInfo().status === "running", "Opening or editing insert stopped playback")
            closeEditor()
            var stop = Date.now()
            api.dispatcher.dispatch("action://playback/stop")
            waitFor(function() { var s = Project.playbackInfo(); return s.status === "stopped" && s.streamKind !== "playback" }, "Inserted MIDI playback did not stop")
            var stopMs = Date.now() - stop
            check(stopMs < 500, "Inserted MIDI playback stop took " + stopMs + " ms")
            api.log.info("MIDI insert physical playback startMs=" + startMs + " stopMs=" + stopMs)
            api.dispatcher.dispatch("clear-loop-region")
            render(Math.pow(10, -6 / 20))
            check(gain() === manualGain, "Timeline processing overwrote the edited manual insert value")
        }},
        {name: "Freeze inserted MIDI and retain its exact audio through save reopen", func: function() {
            expectedSource = source()
            var expectedAudio = render(Math.pow(10, -6 / 20))
            header("Route"); control("NavigationBottomPanel", "TrackRoutingControls", "Freeze")
            waitFor(function() { return info().type === "wave" && info().frozen }, "Inserted MIDI did not freeze", 30000)
            check(Project.waveformStatistics(0, 0, sourceEnd).sha256 === expectedAudio.sha256, "Frozen insert audio differs from native render")
            frozenEnd = Project.totalTime(); frozenHash = Project.waveformStatistics(0, 0, frozenEnd).sha256
            check(api.testflow.prepareSavingFile("MIDI insert effects.aup4"), "No fresh save destination")
            api.testflow.after(100, saveDialog); api.dispatcher.dispatch("file-save"); sleep(4500)
            savedPath = api.testflow.selectedFilePath()
            check(savedPath && api.testflow.fileSize(savedPath) > 0, "Frozen MIDI insert project was not saved")
            reopen()
            check(info().frozen && Project.waveformStatistics(0, 0, frozenEnd).sha256 === frozenHash, "Save reopen changed complete frozen insert PCM")
        }},
        {name: "Unfreeze and reopen exact MIDI insert settings automation and floating editor", func: function() {
            header("Route"); control("NavigationBottomPanel", "TrackRoutingControls", "Unfreeze")
            check(info().type === "midi" && source() === expectedSource, "Unfreeze changed MIDI insert source")
            render(Math.pow(10, -6 / 20))
            api.dispatcher.dispatch("file-save"); sleep(900); reopen()
            check(info().type === "midi" && source() === expectedSource, "Save reopen changed unfrozen MIDI insert")
            openEditor(); check(gain() === manualGain, "Unfrozen insert editor lost manual value"); closeEditor()
            render(Math.pow(10, -6 / 20))
            api.dispatcher.dispatch("file-save"); sleep(800); api.dispatcher.dispatch("file-close"); sleep(700)
            check(Project.trackCount() === 0, "MIDI insert project did not close")
            if (monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
        }}
    ]})
}
