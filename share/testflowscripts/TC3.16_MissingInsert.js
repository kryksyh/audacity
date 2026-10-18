/*
 * Run by run_native_missing_effect_test.py in three fresh processes. Only the
 * owned native LV2 test bundle is moved aside between setup and missing phases.
 */
var Project = require("Audacity.Project")
var config
var stored
var title = "Audacity fixed-block audio effect fixture"
var saveAttempts = 0
var missingAttempts = 0
var opening = false
var noticeSeenAt = 0
var noticeDismissed = false
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function check(ok, message) {
    if (ok) return
    api.log.info("Missing insert controls=" + JSON.stringify(api.navigation.accessibleControls().filter(function(c) { return c.enabled })))
    api.log.info("Missing insert XML=" + (Project.trackCount() ? info().xml : "none"))
    api.log.info("Missing insert screenshot=" + api.testflow.captureMainWindow("missing-insert-failure.png"))
    api.testflow.error(message)
}
function waitFor(predicate, message, timeout) {
    var end = Date.now() + (timeout || 5000)
    do { var value = predicate(); if (value) return value; sleep(50) } while (Date.now() < end)
    check(false, message)
}
function control(section, panel, name) {
    check(api.navigation.triggerControl(section, panel, name), "Missing control: " + panel + "/" + JSON.stringify(name)); sleep(250)
}
function header(name) { control("TrackViewSection", "Track " + info().id + " Header Panel", name) }
function automation(name) { control("NavigationBottomPanel", "TrackAutomationControls", name) }
function undo() { api.dispatcher.dispatch("action://undo"); sleep(350) }
function redo() { api.dispatcher.dispatch("action://redo"); sleep(350) }
function effect() { return (info().xml.match(/<effect\b[^>]*>[\s\S]*?<\/effect>/) || [""])[0] }
function parameters() { return (effect().match(/<parameters>[\s\S]*?<\/parameters>/) || [""])[0] }
function lanes() { return JSON.stringify(effect().match(/<lane\b[^>]*>[\s\S]*?<\/lane>/g) || []) }
function active() { return effect().indexOf('<effect active="1"') === 0 }
function snapshot() {
    return {path: Project.fileInfo().path, parameters: parameters(), lanes: lanes(), source: Project.waveformStatistics(0, 0, 2),
            processed: stored ? stored.processed : null, monitoring: stored ? stored.monitoring : null}
}
function retain() {
    check(parameters() === stored.parameters && lanes() === stored.lanes, "Unavailable insert settings or automation changed")
    check(Project.waveformStatistics(0, 0, 2).sha256 === stored.source.sha256, "Original source samples changed")
}
function menu(name) {
    waitFor(function() { return api.navigation.triggerAccessibleControl(name) || api.navigation.triggerAccessibleControl(name + " Menu") }, "Missing menu " + name)
    sleep(300)
}
function number(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "TrackAutomationControls", name), "Missing number " + name)
    api.keyboard.key("A", "CTRL"); api.keyboard.text(String(value)); sleep(100); api.keyboard.key("Return"); sleep(200)
}
function render(ratio) {
    api.dispatcher.dispatch("select-all"); sleep(200)
    api.dispatcher.dispatch("mix-render-to-new-track")
    waitFor(function() { return Project.trackCount() === 2 }, "Insert project did not render", 20000)
    var result = Project.waveformStatistics(1, 0, 2)
    check(result && Math.abs(result.peak / stored.source.peak - ratio) < 0.002 &&
          Math.abs(result.rms / stored.source.rms - ratio) < 0.002, "Incorrect missing/restored insert audio: " + JSON.stringify(result))
    undo(); check(Project.trackCount() === 1, "Render undo did not retain the source track")
    return result
}
function bypass(enabled) {
    header("Effects")
    if (active() !== enabled) check(api.testflow.mouseClickItem("realtimeEffectBypass-track-0"), "Unavailable insert bypass control is disabled")
    sleep(200); check(active() === enabled, "Insert bypass did not update the document")
}
function saveDialog() {
    if (api.testflow.selectedFilePath()) return
    if (api.navigation.sections().some(function(s) { return s.enabled && s.name === "AskLocationTypeDialog" })) {
        check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    } else if (++saveAttempts < 100) api.testflow.after(100, saveDialog)
    else check(false, "Save chooser did not open")
}
function dismissMissing() {
    if (!opening) return
    var window = api.testflow.itemWindowInfo("missingPluginsAcceptButton")
    if (window) {
        if (!noticeSeenAt) noticeSeenAt = Date.now()
        if (Date.now() - noticeSeenAt >= 200) {
            check(api.testflow.mouseClickItem("missingPluginsAcceptButton"), "Cannot click missing-plugin notice OK")
            noticeDismissed = true
        }
    }
    // File data is loaded before the project page constructs the notice.
    // Target the actual button's window: the warning and its hidden details
    // dialog share the base NavigationSection name.
    check(++missingAttempts < 200, "Project open did not finish after handling its notice")
    api.testflow.after(100, dismissMissing)
}
function open(path) {
    missingAttempts = 0; noticeSeenAt = 0; noticeDismissed = false; opening = true
    api.testflow.after(200, dismissMissing)
    api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + path))
    waitFor(function() { return Project.trackCount() === 1 }, "Saved insert project did not reopen", 10000)
    sleep(500)
    opening = false
    check(config.phase !== "missing" || noticeDismissed, "The removed plugin did not produce its normal warning")
}
function saveAndReopen() {
    api.dispatcher.dispatch("file-save"); sleep(900)
    check(!Project.fileInfo().modified, "Project did not save")
    api.dispatcher.dispatch("file-close"); sleep(600); open(stored.path)
}
function main(options) {
    config = options || {phase: "setup"}; stored = config.state
    var steps = [{name: "Open the isolated insert test phase", func: function() {
        api.testflow.showMainWindowOnFront()
        if (config.phase === "setup") {
            var monitoring = Project.playbackInfo()
            if (monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
            api.dispatcher.dispatch("file-new"); sleep(1400)
            var edit = Project.beginEdit("Missing insert tone range")
            check(edit.setSelection({start: 0, end: 2, tracks: []}), "Cannot select Tone duration"); edit.commit(); sleep(150)
            api.dispatcher.dispatch("action://effects/apply?effectId=Tone&effectIdEncoding=percent&Frequency=440&Amplitude=0.1&Waveform=Sine")
            waitFor(function() { return Project.trackCount() === 1 && Project.totalTime() >= 1.99 }, "Original Tone source missing")
            stored = snapshot(); stored.monitoring = monitoring
        } else {
            check(stored && stored.path, "No persisted test state"); open(stored.path); retain()
        }
    }}]
    if (config.phase === "setup") steps.push(
        {name: "Configure real native LV2 manual parameters and automation through Muse controls", func: function() {
            header("Effects"); control("EffectsSection", "EffectsPanel", "Add effect")
            menu("LV2"); menu("n/a"); menu(title)
            waitFor(function() { return effect().indexOf("fixed-audio-effect@") !== -1 }, "Native fixture was not added")
            sleep(300)
            if (!api.testflow.itemWindowInfo("generatedParameterInput-gain")) check(api.testflow.mouseClickItem("realtimeEffectEditor-track-0"), "Cannot open generated native editor")
            var window = waitFor(function() { return api.testflow.itemWindowInfo("generatedParameterInput-gain") }, "No generated native parameter input")
            sleep(300)
            check(api.testflow.typeControlText(window.title, "GeneratedEffectParameters", "generatedParameterInput-gain", "0.3"), "Cannot edit manual LV2 gain")
            check(api.testflow.typeControlText(window.title, "GeneratedEffectParameters", "generatedParameterInput-lookahead", "7"), "Cannot edit manual LV2 lookahead")
            check(parameters().indexOf('name="Gain" value="0.3') !== -1 && parameters().indexOf('name="Lookahead" value="7"') !== -1, "Native manual values were not stored")
            check(api.testflow.closeItemWindow("generatedParameterInput-gain"), "Cannot close native parameter editor")
            render(0.3)
            header("trackAutomationButton"); automation([-1, 1]); control("Dropdown", "Dropdown", title)
            automation([-1, 2]); control("Dropdown", "Dropdown", "Gain")
            check(api.testflow.mouseClickItem("automationChart", 0.1, 0.5, "left", true), "Cannot create native automation")
            sleep(200); check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select native point")
            number("automationPointTime", 0); number("automationPointValue", 25)
            stored.processed = render(0.25)
            stored.parameters = parameters(); stored.lanes = lanes()
            check(api.testflow.prepareSavingFile("Unavailable native insert.aup4"), "No fresh project destination")
            api.testflow.after(100, saveDialog); api.dispatcher.dispatch("file-save"); sleep(4500)
            stored.path = Project.fileInfo().path
            check(stored.path && api.testflow.fileSize(stored.path) > 0, "Configured fixture project was not saved")
        }}
    )
    else if (config.phase === "missing") steps.push(
        {name: "Bypass the genuinely unavailable insert through its original row", func: function() {
            header("Effects")
            check(active(), "Unavailable insert lost its enabled state on load")
            check(!api.testflow.mouseClickItem("realtimeEffectEditor-track-0"), "Unavailable plugin editor remained enabled")
            bypass(false); check(Project.fileInfo().modified, "Bypass did not mark the file modified")
            retain(); render(1); retain()
            api.log.info("Actual missing insert scene=" + api.testflow.captureMainWindow("missing-insert-bypass.png"))
        }},
        {name: "Keep opaque parameters through unrelated history and repeated saves while unavailable", func: function() {
            api.dispatcher.dispatch("new-mono-track"); sleep(300); check(Project.trackCount() === 2, "No unrelated track edit")
            undo(); check(Project.trackCount() === 1, "Undo did not remove the unrelated track"); retain()
            redo(); check(Project.trackCount() === 2, "Redo did not restore unrelated track")
            undo(); retain(); check(!active(), "Undo lost unavailable effect bypass")
            bypass(true); retain(); bypass(false); retain()
            for (var i = 0; i < 2; ++i) { saveAndReopen(); retain(); check(!active(), "Saved unavailable bypass changed"); render(1) }
        }}
    )
    else if (config.phase === "restored") steps.push(
        {name: "Restore native parameters and exact DSP after the binary is available again", func: function() {
            check(!active(), "Restoring a binary silently enabled the insert")
            header("Effects"); check(api.testflow.mouseClickItem("realtimeEffectEditor-track-0"), "Restored insert editor is disabled")
            waitFor(function() { return api.testflow.itemWindowInfo("generatedParameterInput-gain") }, "No restored native editor"); sleep(300)
            check(Math.abs(api.testflow.itemWindowInfo("generatedParameterInput-gain").currentValue - 0.3) < 1e-6, "Restored gain was reset")
            check(api.testflow.itemWindowInfo("generatedParameterInput-lookahead").currentValue === 7, "Restored lookahead was reset")
            check(api.testflow.closeItemWindow("generatedParameterInput-gain"), "Cannot close restored editor")
            bypass(true); retain()
            check(render(0.25).sha256 === stored.processed.sha256, "Restored complete processed PCM changed")
            header("trackAutomationButton"); automation([-1, 1]); control("Dropdown", "Dropdown", title)
            automation([-1, 2]); control("Dropdown", "Dropdown", "Gain"); automation("Read")
            render(0.3); undo(); retain()
            saveAndReopen(); retain(); check(render(0.25).sha256 === stored.processed.sha256, "Saved restored native PCM changed")
        }}
    )
    else check(false, "Unknown test phase")
    steps.push({name: "Save and close only this test document", func: function() {
        retain(); api.dispatcher.dispatch("file-save"); sleep(800)
        var result = snapshot(); result.processed = stored.processed; result.monitoring = stored.monitoring
        api.log.info("MISSING_EFFECT_STATE=" + encodeURIComponent(JSON.stringify(result)))
        api.dispatcher.dispatch("file-close"); sleep(600); check(Project.trackCount() === 0, "Insert test document did not close")
        if (config.phase === "restored") {
            if (stored.monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (stored.monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
        }
    }})
    api.testflow.runTestCase({name: "TC3.16: Native missing insert " + config.phase, steps: steps})
}
