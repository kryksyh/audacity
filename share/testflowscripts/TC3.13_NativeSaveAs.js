/*
 * Audacity: A Digital Audio Editor
 * Actual QFileDialog Save/Save As, overwrite refusal and confirmation, with
 * MIDI, frozen source data, buses and automation. Uses Qt's widget chooser;
 * this does not claim coverage of the platform-native macOS save panel.
 */
var Project = require("Audacity.Project")
var originalPath, secondPath, monitoring, originalNotes, frozenAudio, duration
var baseline, second, overwritten, savePlan
function sleep(ms) { api.testflow.sleep(ms) }
function info(i) { return Project.trackInfo(i) }
function check(ok, message) {
    if (ok) return
    api.log.info("Save As failure=" + JSON.stringify({file: Project.fileInfo(), field: api.testflow.widgetInfo("fileNameEdit"), buttons: api.testflow.widgetButtons(),
        sections: api.navigation.sections().filter(function(s) { return s.enabled })}))
    api.log.info("Save As failure screenshot=" + api.testflow.captureMainWindow("save-as-failure.png"))
    api.log.info("Save As chooser failure screenshot=" + api.testflow.captureWidgetWindow("fileNameEdit", "save-as-chooser-failure.png"))
    // Do not leave a modal picker waiting after a timer-driven assertion fails.
    api.testflow.mouseClickWidgetButton("No")
    if (api.testflow.widgetInfo("fileNameEdit")) api.testflow.mouseClickWidgetButton("Cancel")
    api.testflow.error(message)
    throw new Error(message)
}
function waitFor(predicate, message, timeout) {
    var deadline = Date.now() + (timeout || 6000)
    do { var value = predicate(); if (value) return value; sleep(50) } while (Date.now() < deadline)
    check(false, message)
}
function control(section, panel, name) {
    waitFor(function() { return api.navigation.controls(section, panel).some(function(c) {
        return c.enabled && (Array.isArray(name) ? c.index.row === name[0] && c.index.column === name[1] : c.name === name)
    }) }, "Missing control " + panel + "/" + JSON.stringify(name))
    check(api.navigation.triggerControl(section, panel, name), "Cannot activate " + panel + "/" + JSON.stringify(name))
    sleep(200)
}
function header(i, name) { control("TrackViewSection", "Track " + info(i).id + " Header Panel", name) }
function bottom(panel, name) { control("NavigationBottomPanel", panel, name) }
function number(name, value) {
    check(api.navigation.goToControl("NavigationBottomPanel", "TrackAutomationControls", name), "Missing number field " + name)
    api.keyboard.key("A", "CTRL")
    api.keyboard.text(String(value))
    sleep(100)
    api.keyboard.key("Return")
    sleep(180)
}
function volume(value) {
    header(2, "trackAutomationButton")
    if (info(2).xml.indexOf("<point ") === -1) {
        check(api.testflow.mouseClickItem("automationChart", 0.1, 0.6, "left", true), "Cannot add bus automation")
        sleep(150)
    }
    check(api.testflow.mouseClickItem("automationPoint0"), "Cannot select bus automation")
    number("automationPointTime", 0)
    number("automationPointValue", value)
    var point = info(2).xml.match(/<point time="([^"]+)" value="([^"]+)"/)
    check(point && Math.abs(Number(point[1])) < 0.001 && Math.abs(Number(point[2]) * 398.1071705534972 - value) < 0.001,
          "Bus automation did not commit " + value + "%")
}
function identity(path, modified) {
    var file = Project.fileInfo()
    check(file.path === path && file.databasePath === path && !file.newlyCreated && file.modified === modified,
          "Document and database identity diverged: " + JSON.stringify(file))
}
function source() {
    return [0, 1, 2].map(function(i) {
        var t = info(i), xml = t.xml
        return {type: t.type, frozen: t.frozen, name: t.name,
                notes: xml.match(/<midinote\s[^>]*\/>/g) || [],
                instrument: xml.match(/instrument="[^"]*"/) || [],
                state: xml.match(/instrumentstate="[^"]*"/) || [],
                lanes: xml.match(/<lane\b[^>]*>[\s\S]*?<\/lane>/g) || [],
                route: xml.match(/<routing\b[^>]*>[\s\S]*?<\/routing>/) || xml.match(/<routing\b[^>]*\/>/) || [],
                freeze: xml.match(/<freeze\b[^>]*>[\s\S]*?<\/freeze>/) || []}
    })
}
function render() {
    api.dispatcher.dispatch("select-all")
    waitFor(function() {
        var selection = Project.selection
        return selection.audioTracks.length === 2 && selection.duration >= duration - 0.001
    }, "Render selection did not reach all tracks")
    sleep(50) // Command availability is delivered after the selection notification.
    api.dispatcher.dispatch("mix-render-to-new-track")
    waitFor(function() { return Project.trackCount() === 4 }, "Save fixture did not render", 20000)
    var result = Project.waveformStatistics(3, 0, duration)
    check(result && result.sampleCount > 0 && result.peak > 0.00001 && result.sha256.length === 64, "Save fixture render is silent")
    api.dispatcher.dispatch("action://undo")
    sleep(250)
    check(Project.trackCount() === 3, "Cannot undo the temporary render")
    return result
}
function snapshot() {
    return {source: source(), audio: render()}
}
function compare(expected) {
    check(JSON.stringify(source()) === JSON.stringify(expected.source), "Save As changed notes, routing, automation or frozen source")
    check(Project.waveformStatistics(1, 0, duration).sha256 === frozenAudio.sha256, "Save As changed stored frozen PCM")
    check(render().sha256 === expected.audio.sha256, "Save As changed the routed render PCM")
}
function handleChooser() {
    var plan = savePlan
    if (plan.complete) return
    check(Date.now() < plan.deadline, "Actual save chooser did not complete: " + JSON.stringify(plan))
    if (plan.phase === "open") {
        if (api.navigation.sections().some(function(s) { return s.name === "AskLocationTypeDialog" && s.enabled })) {
            check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
        }
        if (api.testflow.widgetInfo("fileNameEdit")) {
            check(api.testflow.mouseClickWidget("fileNameEdit"), "Cannot focus the actual file name editor")
            plan.phase = "typing"
            check(api.testflow.typeWidgetText("fileNameEdit", plan.path), "Cannot type into the actual file name editor")
            api.testflow.after(100, handleChooser)
            return
        }
    } else if (plan.phase === "typing") {
        var field = api.testflow.widgetInfo("fileNameEdit")
        if (!plan.observedField) {
            api.log.info("Actual save field=" + JSON.stringify(field))
            api.log.info("Actual save field screenshot=" + api.testflow.captureWidgetWindow("fileNameEdit", "save-as-field.png"))
            plan.observedField = true
        }
        if (field && field.text === plan.path && api.testflow.widgetButtons().some(function(b) { return b.enabled && b.text === "Save" })) {
            api.log.info("Actual save chooser screenshot=" + api.testflow.captureWidgetWindow("fileNameEdit", "save-as-chooser.png"))
            plan.phase = plan.overwrite ? "overwrite" : "finish"
            // The Save click can enter a nested QMessageBox loop. Schedule its
            // confirmation handler before sending the real button click.
            api.testflow.after(100, handleChooser)
            check(api.testflow.mouseClickWidgetButton("Save"), "Actual save button is missing or ambiguous")
            return
        }
    } else if (plan.phase === "overwrite") {
        var buttons = api.testflow.widgetButtons()
        if (buttons.some(function(b) { return b.enabled && b.text === "Yes" }) &&
            buttons.some(function(b) { return b.enabled && b.text === "No" })) {
            api.log.info("Actual overwrite choice=" + plan.overwrite + ", buttons=" + JSON.stringify(buttons))
            plan.confirmed = plan.overwrite
            plan.phase = plan.overwrite === "No" ? "cancel" : "finish"
            api.testflow.after(100, handleChooser)
            check(api.testflow.mouseClickWidgetButton(plan.overwrite), "Cannot answer the actual overwrite confirmation")
            return
        }
    } else if (plan.phase === "cancel") {
        if (api.testflow.widgetInfo("fileNameEdit")) {
            plan.phase = "finish"
            api.testflow.after(100, handleChooser)
            check(api.testflow.mouseClickWidgetButton("Cancel"), "Cannot cancel the chooser after refusing overwrite")
            return
        }
    } else if (!api.testflow.widgetInfo("fileNameEdit")) {
        plan.complete = true
        return
    }
    api.testflow.after(100, handleChooser)
}
function save(action, path, overwrite) {
    savePlan = {path: path, overwrite: overwrite || "", phase: "open", complete: false, confirmed: "", deadline: Date.now() + 20000}
    api.testflow.after(100, handleChooser)
    api.dispatcher.dispatch(action)
    waitFor(function() { return savePlan.complete }, "Actual save chooser did not finish")
    check(savePlan.confirmed === (overwrite || ""), "Overwrite confirmation was skipped")
    if (overwrite === "No") check(api.testflow.selectedFilePath() === "", "Cancelled chooser returned a destination")
    else check(api.testflow.selectedFilePath() === path && api.testflow.fileSize(path) > 0, "Actual chooser did not save its selected path")
    api.log.info("Save As result=" + JSON.stringify({plan: savePlan, file: Project.fileInfo()}))
}
function open(path) {
    api.dispatcher.dispatch("file-close")
    waitFor(function() { return Project.trackCount() === 0 }, "Project did not close")
    api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + path))
    waitFor(function() { return Project.trackCount() === 3 }, "Saved project did not reopen", 10000)
    identity(path, false)
}
function main() {
    api.testflow.runTestCase({name: "TC3.13: Native Save As ownership and overwrite", steps: [
        {name: "Record MIDI and freeze its duplicate with bus automation", func: function() {
            api.testflow.showMainWindowOnFront()
            monitoring = Project.playbackInfo()
            if (monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
            api.testflow.useWidgetFileDialogs(true)
            api.dispatcher.dispatch("file-new")
            sleep(1000)
            api.dispatcher.dispatch("new-instrument-track")
            waitFor(function() { return Project.trackCount() === 1 }, "Instrument track did not finish creating")
            header(0, "Piano roll")
            bottom("PianoRollControls", "pianoRollControl9")
            bottom("PianoRollControls", "pianoRollControl4")
            api.keyboard.pressKey("Z"); sleep(180); api.keyboard.releaseKey("Z")
            sleep(100)
            api.keyboard.pressKey("X"); sleep(220); api.keyboard.releaseKey("X")
            bottom("PianoRollControls", "pianoRollControl4")
            originalNotes = info(0).xml.match(/<midinote\s[^>]*\/>/g) || []
            check(originalNotes.length === 2, "MIDI source was not recorded")
            api.dispatcher.dispatch("duplicate")
            waitFor(function() { return Project.trackCount() === 2 }, "MIDI track was not duplicated")
            check(info(1).type === "midi", "Duplicated source is not MIDI")
            header(1, "Route")
            bottom("TrackRoutingControls", "Freeze")
            waitFor(function() { return info(1).type === "wave" && info(1).frozen }, "MIDI duplicate did not freeze", 20000)
            duration = Project.totalTime()
            frozenAudio = Project.waveformStatistics(1, 0, duration)
            bottom("TrackRoutingControls", "Add bus")
            check(Project.trackCount() === 3, "Missing save fixture bus")
            for (var i = 0; i < 2; ++i) {
                header(i, "Route")
                bottom("TrackRoutingControls", "Dropdown")
                api.navigation.down(); sleep(100); api.navigation.trigger(); sleep(200)
            }
            volume(50)
            baseline = snapshot()
        }},
        {name: "Save using Audacity's actual Qt file chooser", func: function() {
            originalPath = api.testflow.prepareSavingFile("MIDI O'Brien original.aup4")
            save("file-save", originalPath)
            identity(originalPath, false)
        }},
        {name: "Save As switches to a new database and retains the original", func: function() {
            volume(25)
            second = snapshot()
            check(Math.abs(second.audio.rms / baseline.audio.rms - 0.5) < 0.003, "Bus edit did not change audio before Save As")
            secondPath = api.testflow.prepareSavingFile("MIDI O'Brien second.aup4")
            save("file-save-as", secondPath)
            identity(secondPath, false)
        }},
        {name: "Reopen both files and compare notes, frozen PCM and routed audio", func: function() {
            open(originalPath); compare(baseline)
            open(secondPath); compare(second)
        }},
        {name: "Refusing overwrite and cancelling keeps the source dirty and connected", func: function() {
            volume(75)
            overwritten = snapshot()
            save("file-save-as", originalPath, "No")
            identity(secondPath, true)
            check(JSON.stringify(source()) === JSON.stringify(overwritten.source), "Refusing overwrite changed the live document")
        }},
        {name: "Confirm overwrite and switch the live document to that destination", func: function() {
            save("file-save-as", originalPath, "Yes")
            identity(originalPath, false)
            compare(overwritten)
        }},
        {name: "Save As during looped playback retains the active transport and both file snapshots", func: function() {
            var edit = Project.beginEdit("Save during playback loop")
            check(edit.setSelection({start: 0, end: 0.8, tracks: []}), "Cannot select the playback loop")
            edit.commit()
            api.dispatcher.dispatch("set-loop-region-to-selection")
            if (!Project.playbackInfo().loopActive) api.dispatcher.dispatch("toggle-loop-region")
            api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
            api.dispatcher.dispatch("action://playback/toggle-play-pause")
            waitFor(function() {
                var s = Project.playbackInfo()
                return s.status === "running" && s.streamActive && s.streamOwned && s.position > 0.02
            }, "Playback did not start before Save As")
            var rate = Project.playbackInfo().streamRate
            var before = JSON.stringify(source())
            var livePath = api.testflow.prepareSavingFile("MIDI O'Brien saved while playing.aup4")
            save("file-save-as", livePath)
            identity(livePath, false)
            var running = Project.playbackInfo()
            check(running.status === "running" && running.streamActive && running.streamOwned && running.streamRate === rate,
                  "Save As stopped or lost the active playback stream: " + JSON.stringify(running))
            check(JSON.stringify(source()) === before, "Save As during playback changed the live MIDI or frozen source")
            sleep(150)
            check(Math.abs(Project.playbackInfo().position - running.position) > 0.04, "The playhead stalled after Save As")
            api.dispatcher.dispatch("action://playback/stop")
            waitFor(function() { return Project.playbackInfo().status === "stopped" }, "Playback did not stop after Save As")
            api.dispatcher.dispatch("clear-loop-region")
            compare(overwritten)
            if (Project.fileInfo().modified) api.dispatcher.dispatch("file-save")
            waitFor(function() { return !Project.fileInfo().modified }, "The live-save fixture remained dirty")
            open(livePath); compare(overwritten)
            open(originalPath); compare(overwritten)
        }},
        {name: "The previous source stays unchanged and the overwritten file retains frozen source data", func: function() {
            open(secondPath); compare(second)
            open(originalPath); compare(overwritten)
            header(1, "Route")
            bottom("TrackRoutingControls", "Unfreeze")
            waitFor(function() { return info(1).type === "midi" }, "Copied frozen source did not restore MIDI")
            check(JSON.stringify(info(1).xml.match(/<midinote\s[^>]*\/>/g) || []) === JSON.stringify(originalNotes), "Saved frozen source changed recorded notes")
            api.dispatcher.dispatch("action://undo")
            sleep(200)
            check(info(1).frozen, "Undo did not restore saved frozen state")
            api.dispatcher.dispatch("file-close")
            waitFor(function() { return Project.trackCount() === 0 }, "Save fixture did not close")
            api.testflow.useWidgetFileDialogs(false)
            if (monitoring.micMetering) api.dispatcher.dispatch("command://record/toggle-mic-metering")
            if (monitoring.inputMonitoring) api.dispatcher.dispatch("command://record/toggle-input-monitoring")
        }}
    ]})
}
