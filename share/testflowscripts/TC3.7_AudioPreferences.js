/*
 * Audacity: A Digital Audio Editor
 * Audio-device preferences must not open capture I/O during discovery.
 */
var Project = require("Audacity.Project")
function check(ok, message) { if (!ok) api.testflow.error(message) }
function audioPanel() {
    var section = "PreferencesDialog"
    var sections = api.navigation.sections()
    if (!sections.some(function(item) { return item.name === section && item.enabled })) return null
    var panels = api.navigation.panels(section)
    for (var p = 0; p < panels.length; ++p) {
        if (!panels[p].enabled) continue
        var controls = api.navigation.controls(section, panels[p].name)
        if (controls.some(function(item) { return item.name === "AudioApiBox" && item.enabled })) {
            return {section: section, panel: panels[p].name}
        }
    }
    return null
}
function openAndClose(iteration) {
    var start = Date.now()
    api.dispatcher.dispatch("command://app/audio-settings")
    var target = null
    while (!(target = audioPanel()) && Date.now() - start < 15000) api.testflow.sleep(25)
    if (!target) api.navigation.dump()
    check(target, "Audio preferences did not become ready within fifteen seconds")
    var opened = Date.now() - start
    check(api.navigation.goToControl(target.section, target.panel, "AudioApiBox"), "Audio device selector cannot receive focus")
    start = Date.now()
    // Use the dialog's own navigation rather than the main window's key focus.
    check(api.navigation.triggerControl(target.section, "ButtonBox", "Cancel"), "Preferences Cancel button cannot be triggered")
    while (audioPanel() && Date.now() - start < 5000) api.testflow.sleep(25)
    check(!audioPanel(), "Audio preferences did not close")
    check(Project.trackCount() === 0, "Preferences created or opened a project")
    api.log.info("Audio preferences iteration " + iteration + ": open=" + opened + "ms, close=" + (Date.now() - start) + "ms")
    check(opened < 5000, "Audio preferences took five seconds or longer to open")
}
function visitPages() {
    api.dispatcher.dispatch("command://app/audio-settings")
    var start = Date.now()
    while (!audioPanel() && Date.now() - start < 15000) api.testflow.sleep(25)
    check(audioPanel(), "Audio preferences did not open for page navigation")
    var section = "PreferencesDialog"
    var pages = []
    start = Date.now()
    do {
        pages = api.navigation.controls(section, "PreferencesMenuPanel").filter(function(item) { return item.enabled })
        if (pages.length < 9) api.testflow.sleep(25)
    } while (pages.length < 9 && Date.now() - start < 5000)
    pages.sort(function(a, b) { return a.index.row - b.index.row })
    if (pages.length < 9) api.navigation.dump()
    check(pages.length >= 9, "Preferences page menu is incomplete")
    var count = 0
    for (var round = 0; round < 2; ++round) {
        for (var i = 0; i < pages.length; ++i) {
            var page = pages[i]
            check(api.navigation.goToControl(section, "PreferencesMenuPanel", [page.index.row, page.index.column]), "Preferences page cannot receive focus")
            api.testflow.sleep(50)
            var visible = api.navigation.panels(section).filter(function(item) {
                return item.enabled && item.name !== "PreferencesMenuPanel" && item.name !== "ButtonBox"
            })
            check(visible.length > 0, "Selected preferences page has no enabled content")
        }
        var currentCount = api.navigation.panels(section).length
        if (round === 0) count = currentCount
        else check(currentCount === count, "Revisiting preferences created duplicate page instances")
    }
    check(api.navigation.triggerControl(section, "ButtonBox", "Cancel"), "Preferences Cancel button cannot be triggered")
    api.testflow.sleep(100)
    check(!api.navigation.sections().some(function(item) { return item.name === section && item.enabled }), "Preferences remained open after Cancel")
}
function main() {
    api.testflow.runTestCase({name: "TC3.7: Audio preferences device-query performance", steps: [
        {name: "Open and close audio preferences", func: function() { api.testflow.showMainWindowOnFront(); openAndClose(1) }},
        {name: "Reopen audio preferences", func: function() { openAndClose(2) }},
        {name: "Open audio preferences a third time", func: function() { openAndClose(3) }},
        {name: "Visit and revisit every preferences page", func: visitPages}
    ]})
}
