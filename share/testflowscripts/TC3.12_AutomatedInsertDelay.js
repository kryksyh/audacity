/*
 * Audacity: A Digital Audio Editor
 * Real native delay/gain automation through the Muse effects/routing/automation UI.
 * The routed signal must cancel an inverted dry track, including at frame zero.
 * Requires the chosen fixture registered through the real plugin validator.
 * The default is LV2; pass VST3 format/vendor/title/marker and numeric parameter
 * IDs via --test-case-func-args to exercise the native VST3 delay fixture.
 * With the macOS ASan build, preload native_lv2_fixture.so into the test process
 * using DYLD_INSERT_LIBRARIES. This keeps the fixture's instrumented globals
 * mapped even when the LV2 host releases its last instance (Apple Clang 17's
 * sanitizer leaves their redzones poisoned after unloading the image).
 */
var Project = require("Audacity.Project")
var title = "Audacity fixed-block audio effect fixture"
var config
var monitoring
var baseline
var stored
var saveAttempts = 0
function sleep(ms) { api.testflow.sleep(ms) }
function info(i) { return Project.trackInfo(i) }
function check(ok, message) {
    if (ok) return
    api.log.info("Insert delay failure UI=" + JSON.stringify(api.navigation.sections().filter(function(s) { return s.enabled })))
    api.navigation.sections().filter(function(s) { return s.enabled && s.name.indexOf("Menu") !== -1 }).forEach(function(s) {
        api.navigation.panels(s.name).forEach(function(p) { api.log.info("Insert delay menu " + s.name + "/" + p.name + "=" + JSON.stringify(api.navigation.controls(s.name, p.name))) })
    })
    api.log.info("Insert delay accessible controls=" + JSON.stringify(api.navigation.accessibleControls().filter(function(c) { return c.enabled })))
    api.log.info("Insert delay screenshot=" + api.testflow.captureMainWindow("insert-delay-failure.png"))
    api.testflow.error(message)
}
function waitFor(predicate, message, timeout) {
    var end = Date.now() + (timeout || 5000)
    do { var value = predicate(); if (value) return value; sleep(50) } while (Date.now() < end)
    check(false, message)
}
function control(section, panel, name) {
    waitFor(function() { return api.navigation.controls(section, panel).some(function(c) {
        return c.enabled && (Array.isArray(name) ? c.index.row === name[0] && c.index.column === name[1] : c.name === name)
    }) }, "Control not ready: " + panel + "/" + JSON.stringify(name))
    check(api.navigation.triggerControl(section, panel, name), "Cannot trigger " + panel + "/" + JSON.stringify(name))
    sleep(250)
}
function header(i, name) { control("TrackViewSection", "Track " + info(i).id + " Header Panel", name) }
function bottom(panel, name) { control("NavigationBottomPanel", panel, name) }
function select(start, end, tracks) {
    var edit = Project.beginEdit("Insert delay test selection")
    check(edit.setSelection({start: start, end: end, tracks: tracks.map(function(i) { return edit.audioTracks[i] })}), "Cannot select sources")
    edit.commit()
    sleep(200)
}
function apply(name, parameters) {
    var query = "action://effects/apply?effectId=" + encodeURIComponent(name) + "&effectIdEncoding=percent"
    Object.keys(parameters || {}).forEach(function(k) { query += "&" + k + "=" + encodeURIComponent(parameters[k]) })
    api.dispatcher.dispatch(query)
    sleep(450)
}
function menu(name, activePanelOnly) {
    waitFor(function() { return api.navigation.triggerAccessibleControl(name, !!activePanelOnly)
                            || api.navigation.triggerAccessibleControl(name + " Menu", !!activePanelOnly) },
            "Missing or ambiguous visible menu item: " + name)
    sleep(350)
}
function parameter(name) {
    bottom("TrackAutomationControls", [-1, 2])
    control("Dropdown", "Dropdown", name)
}
function number(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "TrackAutomationControls", name), "Missing numeric field " + name)
    api.keyboard.key("A", "CTRL")
    api.keyboard.text(String(value))
    sleep(120)
    api.keyboard.key("Return")
    sleep(200)
}
function point(value) {
    check(api.testflow.mouseClickItem("automationChart", 0.1, 0.5, "left", true), "Cannot create effect automation point")
    sleep(200)
    check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select effect automation point")
    number("automationPointTime", 0)
    number("automationPointValue", value)
}
function render(cancelled) {
    api.dispatcher.dispatch("select-all")
    sleep(180)
    api.dispatcher.dispatch("mix-render-to-new-track")
    waitFor(function() { return Project.trackCount() === 4 }, "Routed insert was not rendered", 20000)
    var audio = Project.waveformStatistics(3, 0, 2)
    check(audio && audio.sampleCount === 96000, "Rendered insert has the wrong length")
    api.log.info("Automated insert cancellation=" + cancelled + ", audio=" + JSON.stringify(audio))
    if (cancelled) check(audio.peak < 0.000001 && audio.rms < 0.0000001, "Automated insert does not cancel the inverted dry source")
    else check(audio.peak > 0.02 && audio.peak < 0.03 && audio.rms > 0.01, "Manual insert control signal is incorrect")
    api.dispatcher.dispatch("action://undo")
    sleep(350)
    check(Project.trackCount() === 3, "Undo did not remove the render")
    return audio
}
function saveDialog() {
    if (api.testflow.selectedFilePath()) return
    if (api.navigation.sections().some(function(s) { return s.name === "AskLocationTypeDialog" && s.enabled })) {
        check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    } else if (++saveAttempts < 100) api.testflow.after(100, saveDialog)
    else check(false, "Save chooser did not open")
}
function main(options) {
    config = options || {format: "LV2", vendor: "n/a", title: title, marker: "fixed-audio-effect@",
                         gain: "gain", lookahead: "lookahead"}
    title = config.title
    api.testflow.runTestCase({name: "TC3.12: Automated native insert delay", steps: [
        {name: "Generate two opposite-phase original-engine sources", func: function() {
            api.testflow.showMainWindowOnFront()
            monitoring = Project.playbackInfo()
            if (monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
            sleep(150)
            api.dispatcher.dispatch("file-new")
            sleep(1400)
            select(0, 2, [])
            apply("Tone", {Frequency: 440, Amplitude: 0.1, Waveform: "Sine"})
            waitFor(function() { return Project.trackCount() === 1 }, "No first source")
            api.dispatcher.dispatch("new-mono-track")
            waitFor(function() { return Project.trackCount() === 2 }, "No dry source track")
            select(0, 2, [1])
            apply("Tone", {Frequency: 440, Amplitude: 0.025, Waveform: "Sine"})
            apply("Invert")
            baseline = [Project.waveformStatistics(0, 0, 2), Project.waveformStatistics(1, 0, 2)]
            check(baseline[0].peak > 0.09 && baseline[1].peak > 0.02, "Source generation failed")
        }},
        {name: "Create and route a bus, then add the real native effect through its menu", func: function() {
            header(0, "Route")
            bottom("TrackRoutingControls", "Add bus")
            check(Project.trackCount() === 3, "Bus was not created")
            header(0, "Route")
            bottom("TrackRoutingControls", "Dropdown")
            api.navigation.down()
            sleep(100)
            api.navigation.trigger()
            sleep(250)
            var id = info(2).xml.match(/<routing bus="([^"]+)"/)
            check(id && info(0).xml.indexOf('output="' + id[1] + '"') !== -1, "Source did not route to bus")
            header(2, "Effects")
            control("EffectsSection", "EffectsPanel", "Add effect")
            menu(config.format)
            menu(config.vendor, true)
            menu(title, true)
            sleep(500)
            api.keyboard.key("Escape")
            sleep(300)
            check(info(2).xml.indexOf(config.marker) !== -1, "Selected bus has no native insert")
            render(false)
        }},
        {name: "Automate native insert gain and lookahead through Muse controls", func: function() {
            header(2, "trackAutomationButton")
            bottom("TrackAutomationControls", [-1, 1])
            control("Dropdown", "Dropdown", title)
            parameter("Gain")
            point(25)
            parameter("Lookahead")
            point(100 * 7 / 31)
            check(info(2).xml.indexOf('parameter="' + config.gain + '" enabled="1"') !== -1 &&
                  info(2).xml.indexOf('parameter="' + config.lookahead + '" enabled="1"') !== -1, "Native parameter lanes were not serialized")
            render(true)
            api.log.info("Automated insert scene=" + api.testflow.captureMainWindow("automated-insert-" + config.format + ".png"))
        }},
        {name: "Read bypass restores the manual gain and enabling Read cancels again", func: function() {
            header(2, "trackAutomationButton")
            parameter("Gain")
            bottom("TrackAutomationControls", "Read")
            render(false)
            header(2, "trackAutomationButton")
            bottom("TrackAutomationControls", "Read")
            render(true)
        }},
        {name: "Play and seek the compensated native bus through the device, including a very short range", func: function() {
            api.dispatcher.dispatch("clear-loop-region")
            select(0.125, 0.875, [])
            api.dispatcher.dispatch("action://playback/play-selection")
            waitFor(function() {
                var s = Project.playbackInfo()
                return s.status === "running" && s.streamActive && s.streamOwned && s.streamTime > 0.175 && s.streamTime < 0.5
            }, "Delayed native insert did not advance the actual device clock")
            waitFor(function() { return Project.playbackInfo().status === "stopped" }, "Delayed native insert range did not finish")
            check(Math.abs(Project.playbackInfo().position - 0.875) < 0.025,
                  "Delayed native insert range ended at the wrong position: " + JSON.stringify(Project.playbackInfo()))
            // An explicit seek leaves selection-play mode in Audacity and
            // establishes a new stop anchor. Exercise that separately from
            // the natural end of the exact selection above.
            api.dispatcher.dispatch("action://playback/seek?seekTime=0.125&triggerPlay=false")
            api.dispatcher.dispatch("action://playback/toggle-play-pause")
            waitFor(function() {
                var s = Project.playbackInfo()
                return s.status === "running" && s.streamActive && s.streamTime > 0.175 && s.streamTime < 0.5
            }, "Delayed native insert seek source did not start")
            api.dispatcher.dispatch("action://playback/seek?seekTime=0.4&triggerPlay=true")
            waitFor(function() {
                var s = Project.playbackInfo()
                return s.status === "running" && s.streamActive && s.streamTime > 0.425 && s.streamTime < 0.7
            }, "Delayed native insert did not seek on the device")
            api.dispatcher.dispatch("action://playback/stop")
            waitFor(function() { return Project.playbackInfo().status === "stopped" }, "Delayed native insert did not stop after seeking")
            check(Math.abs(Project.playbackInfo().position - 0.4) < 0.025,
                  "Delayed native insert did not preserve the seek anchor: " + JSON.stringify(Project.playbackInfo()))
            // This range is shorter than the fixture's fixed-block adapter.
            // Compensated timestamps must still let the transport reach its end.
            select(0.125, 0.127, [])
            api.dispatcher.dispatch("action://playback/play-selection")
            sleep(200)
            waitFor(function() { return Project.playbackInfo().status === "stopped" }, "Shorter-than-delay native insert range did not finish")
            check(Math.abs(Project.playbackInfo().position - 0.127) < 0.0001, "Shorter-than-delay range ended at the wrong position")
            api.log.info("Delayed native insert device transport=" + JSON.stringify(Project.playbackInfo()))
        }},
        {name: "Save reopen and retain native effect automation, routing and source samples", func: function() {
            stored = info(2).xml.match(/<lane\b[^>]*>[\s\S]*?<\/lane>/g)
            check(stored && stored.length === 2, "Missing saved insert lanes")
            check(api.testflow.prepareSavingFile("Automated insert delay.aup4"), "No fresh save destination")
            api.testflow.after(100, saveDialog)
            api.dispatcher.dispatch("file-save")
            sleep(4500)
            var path = api.testflow.selectedFilePath()
            check(path && api.testflow.fileSize(path) > 0, "Insert automation project was not saved")
            api.dispatcher.dispatch("file-close")
            sleep(700)
            api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + path))
            waitFor(function() { return Project.trackCount() === 3 }, "Insert project did not reopen")
            check(JSON.stringify(info(2).xml.match(/<lane\b[^>]*>[\s\S]*?<\/lane>/g)) === JSON.stringify(stored), "Saved native insert lanes changed")
            for (var i = 0; i < 2; ++i) check(Project.waveformStatistics(i, 0, 2).sha256 === baseline[i].sha256, "An original source changed")
            render(true)
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Insert project did not close")
            if (monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
        }}
    ]})
}
