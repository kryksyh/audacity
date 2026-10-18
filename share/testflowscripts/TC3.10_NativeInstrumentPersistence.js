/*
 * Audacity: A Digital Audio Editor
 * Installed native editor, real transport, freeze, preset and file lifecycle.
 * --test-case-func-args '[{"title":"Surge XT (AU)","format":"AudioUnit"}]'
 * selects another installed instrument.
 */
var Project = require("Audacity.Project")
var config
var expected
var reference
var frozenReference
var frozenEnd
var saveAttempts = 0
var emptyClosePending = false
var emptyCloseAttempts = 0
var freezeCancelSent = false
var freezeCancelAttempts = 0
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function info() { return Project.trackInfo(0) }
function waitFor(predicate, message, timeout) {
    var deadline = Date.now() + (timeout || 5000)
    do {
        var value = predicate()
        if (value) return value
        sleep(50)
    } while (Date.now() < deadline)
    check(false, typeof message === "function" ? message() : message)
}
function control(section, panel, name) {
    for (var attempt = 0; attempt < 100; ++attempt) {
        var ready = api.navigation.controls(section, panel).some(function(item) {
            return item.enabled && (Array.isArray(name) ? item.index.row === name[0] && item.index.column === name[1] : item.name === name)
        })
        if (ready) break
        sleep(50)
    }
    if (!ready) {
        api.log.info("Missing native-instrument control: " + JSON.stringify({section: section, panel: panel, control: name,
                                                                            controls: api.navigation.controls(section, panel),
                                                                            sections: api.navigation.sections().filter(function(s) { return s.enabled })}))
        api.log.info("Native instrument control failure screenshot=" + api.testflow.captureMainWindow("native-control-failure.png"))
    }
    check(api.navigation.triggerControl(section, panel, name), "Missing control: " + panel + "/" + JSON.stringify(name))
    sleep(250)
}
function header(name) { control("TrackViewSection", "Track " + info().id + " Header Panel", name) }
function piano(name) { control("NavigationBottomPanel", "PianoRollControls", name) }
function chooseInstrument(title) {
    piano([-1, 1])
    // The long installed-plugin menu virtualizes delegates. Navigate the real
    // popup so a row outside the initial visible range is instantiated first.
    for (var i = 0; i < 250; ++i) {
        if (api.navigation.activeControl() === title) {
            api.navigation.trigger()
            sleep(400)
            return
        }
        api.navigation.down()
        sleep(40)
    }
    check(false, "Cannot select installed instrument: " + title + ", active=" + api.navigation.activeControl())
}
function source() {
    var xml = info().xml
    return JSON.stringify({instrument: xml.match(/instrument="([^"]*)"/),
                           state: xml.match(/instrumentstate="([^"]*)"/),
                           notes: xml.match(/<midinote\s[^>]*\/>/g) || []})
}
function checkSource(expected, message) {
    var actual = source()
    if (actual !== expected) {
        // Keep full diagnostics in the redirected Testflow log, without
        // changing the extension filesystem API's normal path permissions.
        api.log.info("NATIVE_INSTRUMENT_STATE_MISMATCH " + JSON.stringify({before: JSON.parse(expected), after: JSON.parse(actual)}))
    }
    check(actual === expected, message)
}
function editor() {
    var value = api.testflow.itemWindowInfo("nativeInstrumentView")
    return value && value.available && value
}
function openEditor() {
    var requested = Date.now()
    header("Piano roll")
    piano("pianoRollControl2")
    var view = waitFor(editor, function() {
        api.log.info("Native editor failure state=" + JSON.stringify({window: api.testflow.itemWindowInfo("nativeInstrumentView"),
                                                                     playback: Project.playbackInfo(),
                                                                     sections: api.navigation.sections().filter(function(s) { return s.enabled })}))
        api.log.info("Native editor failure screenshot=" + api.testflow.captureMainWindow("native-editor-failure.png"))
        return "Native instrument editor did not attach"
    }, 12000)
    check(!view.mainWindow && !view.modal && view.parentIsMain && view.width > 100 && view.height > 100,
          "Editor is not a floating nonmodal child window: " + JSON.stringify(view))
    check(api.testflow.itemRect("pianoRollGrid"), "Floating editor removed the docked piano roll")
    api.log.info(config.title + " editor ready in " + (Date.now() - requested) + "ms: " + JSON.stringify(view))
    if (config.capture) {
        api.log.info("Native editor screenshot=" + api.testflow.captureItemWindow("nativeInstrumentView", "native-editor-" + config.format + ".png"))
        api.log.info("Native piano roll screenshot=" + api.testflow.captureMainWindow("piano-roll-" + config.format + ".png"))
        if (config.title === "Audacity delayed LV2 instrument fixture") {
            api.log.info("Native widget screenshot=" + api.testflow.captureWidgetWindow("nativeLv2Editor", "native-lv2-widget.png"))
        }
    }
}
function closeEditor() {
    check(api.testflow.closeItemWindow("nativeInstrumentView"), "Cannot close native editor window")
    waitFor(function() { return !api.testflow.itemWindowInfo("nativeInstrumentView") }, "Native editor did not close")
    sleep(200)
}
function discardEmptyReplacement() {
    if (!emptyClosePending) return
    var button = api.navigation.accessibleControls().find(function(item) {
        return item.enabled && (item.accessibleName === "Don’t save" || item.accessibleName === "Don't save")
    })
    if (button && api.testflow.mouseClickControl(button.section, button.panel, button.name)) {
        api.log.info("Discarded the disposable empty replacement project")
        emptyClosePending = false
        return
    }
    if (++emptyCloseAttempts < 50) api.testflow.after(100, discardEmptyReplacement)
    else check(false, "Cannot dismiss the disposable empty project's save confirmation")
}
function cancelVisibleFreeze() {
    check(info().type === "midi", "Freeze completed before the cancellation gesture")
    var ready = api.navigation.controls("NavigationBottomPanel", "TrackRoutingControls").some(function(item) {
        return item.name === "Cancel" && item.enabled
    })
    if (ready) {
        api.log.info("Freeze cancellation scene=" + api.testflow.captureMainWindow("native-freeze-cancel-progress.png"))
        check(api.testflow.mouseClickControl("NavigationBottomPanel", "TrackRoutingControls", "Cancel"), "Cannot click freeze Cancel")
        freezeCancelSent = true
        return
    }
    if (++freezeCancelAttempts < 100) api.testflow.after(20, cancelVisibleFreeze)
    else check(false, "Freeze did not expose its Cancel control")
}
function playStop() {
    api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=false")
    api.dispatcher.dispatch("action://playback/toggle-play-pause")
    waitFor(function() {
        var s = Project.playbackInfo()
        return s.status === "running" && s.streamActive && s.position > 0.1
    }, "Native instrument playback clock did not advance")
    check(editor(), "Native editor detached during playback")
    api.dispatcher.dispatch("action://playback/stop")
    waitFor(function() { return Project.playbackInfo().status === "stopped" }, "Playback did not stop")
    sleep(200)
    check(editor(), "Native editor detached after Stop")
}
function render() {
    api.dispatcher.dispatch("select-all")
    sleep(150)
    api.dispatcher.dispatch("mix-render-to-new-track")
    waitFor(function() { return Project.trackCount() === 2 }, "Native instrument mix was not rendered", 20000)
    var result = Project.waveformStatistics(1, 0, 2)
    check(result && result.peak > 0.00001 && result.rms > 0.000001, "Native instrument render is silent")
    api.log.info(config.title + " rendered samples=" + JSON.stringify(result))
    api.dispatcher.dispatch("action://undo")
    sleep(300)
    check(Project.trackCount() === 1, "Undo did not remove rendered audio")
    return result
}
function compareAudio(actual, expected, exact) {
    check(actual && actual.sampleCount === expected.sampleCount, "Audio sample count changed")
    if (exact) check(actual.sha256 && actual.sha256.length === 64 && actual.sha256 === expected.sha256, "Saved PCM sample bytes changed")
    // Installed synths may randomize oscillator phase between instances. The
    // serialized manual preset is checked exactly; render levels allow that
    // variation. Already-frozen PCM must survive save/reopen exactly.
    var tolerance = exact ? 1e-12 : 0.25
    check(Math.abs(actual.peak / expected.peak - 1) < tolerance && Math.abs(actual.rms / expected.rms - 1) < tolerance,
          "Native instrument audio changed: " + JSON.stringify(actual) + " vs " + JSON.stringify(expected))
}
function handleSaveDialog() {
    if (api.testflow.selectedFilePath()) return
    var section = api.navigation.sections().find(function(item) { return item.name === "AskLocationTypeDialog" && item.enabled })
    if (section) check(api.testflow.mouseClickControl("AskLocationTypeDialog", "LocationOptionsButtons", "Save to computer"), "Cannot choose local save")
    else if (++saveAttempts < 100) api.testflow.after(100, handleSaveDialog)
    else api.testflow.error("Save dialog did not open")
}
function saveReopen(fresh) {
    if (fresh) {
        var destination = api.testflow.prepareSavingFile(config.title + " freeze.aup4")
        check(destination, "No fresh save destination")
        api.testflow.after(100, handleSaveDialog)
    }
    api.dispatcher.dispatch("file-save")
    sleep(4500)
    var path = api.testflow.selectedFilePath()
    check(path && api.testflow.fileSize(path) > 0, "Native instrument project was not saved")
    api.dispatcher.dispatch("file-close")
    sleep(700)
    api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + path))
    sleep(1400)
    check(Project.trackCount() === 1, "Saved instrument project did not reopen")
}
function main(options) {
    config = options || {title: "Surge XT (VST3)", format: "VST3"}
    if (config.cancelFreeze) {
        var clipsBeforeCancel
        api.testflow.runTestCase({name: "Native instrument freeze cancellation", steps: [
            {name: "Record a longer polyphonic source through the real piano-roll controls", func: function() {
                api.testflow.showMainWindowOnFront()
                api.dispatcher.dispatch("file-new")
                sleep(1500)
                api.dispatcher.dispatch("new-instrument-track")
                sleep(600)
                header("Piano roll")
                chooseInstrument(config.title)
                piano("pianoRollControl9")
                piano("pianoRollControl4")
                var keys = ["Z", "S", "X", "D", "C", "V", "G", "B", "H", "N", "J", "M"]
                keys.forEach(function(key) { api.keyboard.pressKey(key) })
                sleep(15000)
                keys.forEach(function(key) { api.keyboard.releaseKey(key) })
                sleep(200)
                piano("pianoRollControl4")
                check((info().xml.match(/<midinote\s/g) || []).length === keys.length, "Long take lost chord notes")
                check(Project.totalTime() >= 15, "Recorded clip did not retain the long source duration")
                expected = source()
                clipsBeforeCancel = JSON.stringify(info().xml.match(/<midiclip\b[\s\S]*?<\/midiclip>/g) || [])
            }},
            {name: "Cancel the running native freeze through its visible button", func: function() {
                header("Route")
                freezeCancelSent = false
                freezeCancelAttempts = 0
                api.testflow.after(200, cancelVisibleFreeze)
                control("NavigationBottomPanel", "TrackRoutingControls", "Freeze")
                waitFor(function() { return freezeCancelSent }, "Native freeze cancellation was not requested")
                waitFor(function() {
                    return api.navigation.controls("NavigationBottomPanel", "TrackRoutingControls").some(function(item) {
                        return item.name === "Freeze" && item.enabled
                    })
                }, "Cancelled freeze did not return to its ready state")
                check(info().type === "midi" && !info().frozen && Project.trackCount() === 1, "Cancelled freeze replaced the original track")
                checkSource(expected, "Cancelled freeze changed the native preset or notes")
                check(JSON.stringify(info().xml.match(/<midiclip\b[\s\S]*?<\/midiclip>/g) || []) === clipsBeforeCancel,
                      "Cancelled freeze changed MIDI clip bounds or events")
            }},
            {name: "Cancellation adds no undo entry and the recorded take remains redoable", func: function() {
                api.dispatcher.dispatch("action://undo")
                sleep(300)
                check(info().type === "midi" && (info().xml.match(/<midinote\s/g) || []).length === 0,
                      "Undo after cancellation did not undo the recorded take")
                api.dispatcher.dispatch("action://redo")
                sleep(300)
                checkSource(expected, "Redo after cancellation did not restore the exact take and preset")
                check(JSON.stringify(info().xml.match(/<midiclip\b[\s\S]*?<\/midiclip>/g) || []) === clipsBeforeCancel,
                      "Redo after cancellation changed MIDI clip bounds or events")
            }},
            {name: "Save and reopen the cancelled source, render it, and close cleanly", func: function() {
                saveReopen(true)
                check(info().type === "midi" && !info().frozen, "Cancelled source reopened as frozen audio")
                checkSource(expected, "Cancelled source or preset changed during save and reopen")
                render()
                api.dispatcher.dispatch("file-save")
                sleep(800)
                api.dispatcher.dispatch("file-close")
                waitFor(function() { return !Project.playbackInfo().projectOpen }, "Cancelled source did not close")
            }}
        ]})
        return
    }
    if (config.project) {
        api.testflow.runTestCase({name: "TC3.10: " + config.title + " isolated editor reopen", steps: [
            {name: "Reopen the owned saved instrument project in a fresh process", func: function() {
                api.testflow.showMainWindowOnFront()
                api.dispatcher.dispatch("command://project/open?url=" + encodeURI("file://" + config.project))
                sleep(1600)
                check(Project.trackCount() === 1 && info().type === "midi", "Saved instrument project did not reopen")
                openEditor()
                playStop()
                closeEditor()
                api.dispatcher.dispatch("file-save")
                sleep(800)
                if (config.closeWhilePlaying) {
                    openEditor()
                    api.dispatcher.dispatch("action://playback/seek?seekTime=0&triggerPlay=true")
                    waitFor(function() {
                        var value = Project.playbackInfo()
                        return value.status === "running" && value.streamActive && value.position > 0.05
                    }, "Native playback did not start before project close")
                }
                api.dispatcher.dispatch("file-close")
                waitFor(function() { return !Project.playbackInfo().projectOpen }, "Isolated instrument project did not close", 20000)
                if (config.closeWhilePlaying) {
                    waitFor(function() { return !Project.playbackInfo().streamPresent && !editor() },
                            "Closing the playing project retained its stream or native editor")
                    api.dispatcher.dispatch("file-new")
                    sleep(1000)
                    check(Project.playbackInfo().projectOpen && Project.trackCount() === 0, "New project did not open empty")
                    check(!Project.playbackInfo().streamPresent && !editor(), "Old instrument followed the new project")
                    emptyClosePending = true
                    emptyCloseAttempts = 0
                    api.testflow.after(100, discardEmptyReplacement)
                    api.dispatcher.dispatch("file-close")
                    waitFor(function() { return !emptyClosePending }, "Empty replacement save confirmation was not dismissed")
                    waitFor(function() { return !Project.playbackInfo().projectOpen }, "Empty replacement project did not close")
                    emptyClosePending = false
                }
            }}
        ]})
        return
    }
    var steps = [
        {name: "Select the exact installed instrument format", func: function() {
            api.testflow.showMainWindowOnFront()
            api.dispatcher.dispatch("file-new")
            sleep(1500)
            api.dispatcher.dispatch("new-instrument-track")
            sleep(600)
            header("Piano roll")
            if (config.liveBeforeSelection) {
                piano("pianoRollControl9")
                api.keyboard.pressKey("Z")
                sleep(100)
            }
            var requested = Date.now()
            chooseInstrument(config.title)
            api.log.info(config.title + " selected in " + (Date.now() - requested) + "ms")
            if (config.liveBeforeSelection) {
                api.keyboard.releaseKey("Z")
                piano("pianoRollControl9")
            }
            check(info().xml.indexOf("Effect_" + config.format + "_") !== -1, "Wrong instrument format selected")
        }},
        {name: "Open close and reopen the floating native editor", func: function() {
            openEditor()
            closeEditor()
            openEditor()
            api.log.info("NATIVE_EDITOR_READY " + config.title)
            if (config.inspectMs) sleep(config.inspectMs)
            if (config.nativeQtFixture) {
                check(api.testflow.widgetInfo("nativeLv2Editor"), "Native Qt editor widget is missing")
                check(api.testflow.mouseDragWidget("nativeLv2Gain", 0.5, 0.5, 0.05, 0.5), "Cannot drag the native gain slider")
                sleep(300)
                var gain = api.testflow.widgetInfo("nativeLv2Gain")
                check(gain && gain.value > 0 && gain.value <= 5, "Native slider did not set a quiet gain: " + JSON.stringify(gain))
            }
            closeEditor()
        }},
        {name: "Record computer-keyboard notes through the installed instrument", func: function() {
            piano("pianoRollControl9")
            piano("pianoRollControl4")
            api.keyboard.pressKey("Z")
            sleep(600)
            api.keyboard.releaseKey("Z")
            sleep(150)
            piano("pianoRollControl4")
            check((info().xml.match(/<midinote\s/g) || []).length === 1, "Installed instrument recorded no MIDI note")
        }},
        {name: "Edit an instrument parameter using the Muse controls", func: function() {
            var before = source()
            piano("pianoRollControl3")
            var rect = api.testflow.itemRect("pianoRollParameter0")
            check(rect, "Instrument manual parameter slider missing")
            check(api.testflow.mouseDragItem("pianoRollParameter0", 0.5, 0.5, config.parameterPosition || 0.43, 0.5), "Cannot edit native instrument parameter")
            sleep(700)
            check(source() !== before, "Muse parameter gesture did not update the native preset")
            expected = source()
            reference = render()
        }},
        {name: "Keep the editor attached through real Play and Stop", func: function() {
            openEditor()
            playStop()
            closeEditor()
            checkSource(expected, "Editor or transport changed the stored manual preset or notes")
        }},
        {name: "Freeze the instrument with its release tail", func: function() {
            var sourceEnd = Project.totalTime()
            header("Route")
            control("NavigationBottomPanel", "TrackRoutingControls", "Freeze")
            waitFor(function() { return info().type === "wave" && info().frozen }, "Native instrument freeze failed", 30000)
            compareAudio(Project.waveformStatistics(0, 0, 2), reference, false)
            frozenEnd = Project.clipsOnTrack(0)[0].end
            check(Math.abs(frozenEnd - (sourceEnd + 2)) <= 2 / Project.defaultSampleRate,
                  "Freeze did not retain its two-second release tail: sourceEnd=" + sourceEnd + ", frozenEnd=" + frozenEnd)
            frozenReference = Project.waveformStatistics(0, 0, frozenEnd)
        }},
        {name: "Save reopen and verify frozen PCM exactly", func: function() {
            saveReopen(true)
            check(info().type === "wave" && info().frozen, "Saved frozen native instrument lost its source")
            check(Math.abs(Project.clipsOnTrack(0)[0].end - frozenEnd) <= 2 / Project.defaultSampleRate,
                  "Saved frozen clip lost its release tail")
            compareAudio(Project.waveformStatistics(0, 0, frozenEnd), frozenReference, true)
        }},
        {name: "Unfreeze and restore the exact MIDI notes and native preset", func: function() {
            header("Route")
            control("NavigationBottomPanel", "TrackRoutingControls", "Unfreeze")
            check(info().type === "midi", "Unfreeze did not restore MIDI")
            checkSource(expected, "Unfreeze did not restore the native preset and recorded notes")
            compareAudio(render(), reference, false)
        }},
        {name: "Save and reopen the unfrozen native instrument", func: function() {
            saveReopen(false)
            check(info().type === "midi", "Saved unfrozen MIDI track changed")
            checkSource(expected, "Saved unfrozen preset or MIDI notes changed")
        }},
        {name: "Reuse the restored native editor and transport, then close cleanly", func: function() {
            openEditor()
            playStop()
            closeEditor()
            checkSource(expected, "Reopened native editor changed its preset")
            api.dispatcher.dispatch("file-save")
            sleep(800)
            api.dispatcher.dispatch("file-close")
            sleep(700)
            check(Project.trackCount() === 0, "Native instrument project did not close cleanly")
        }}
    ]
    if (config.nativeQtFixture) {
        steps.splice(4, 0, {name: "Edit real native private state and preserve its referenced asset", func: function() {
            var before = source()
            var original = reference
            openEditor()
            check(api.testflow.mouseClickWidget("nativeLv2Preset"), "Cannot click the native private preset button")
            check(api.testflow.mouseClickWidget("nativeLv2Asset"), "Cannot edit the native referenced sample")
            sleep(500)
            closeEditor()
            check(source() !== before, "Native private preset change was not saved")
            expected = source()
            reference = render()
            check(Math.abs(reference.peak / original.peak - 4.5) < 0.0001 &&
                  Math.abs(reference.rms / original.rms - 4.5) < 0.0001,
                  "Native private preset and referenced sample did not change the real rendered signal")
        }})
    }
    api.testflow.runTestCase({name: "TC3.10: " + config.title + " native editor and freeze persistence", steps: steps})
}
