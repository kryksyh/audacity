/* Startup discovery must be quiet; explicit rescanning retains its progress. */
var Project = require("Audacity.Project")
var scanning = false
var dismissed = false
function check(ok, message) { if (!ok) api.testflow.error(message) }
function sleep(ms) { api.testflow.sleep(ms) }
function findButton(label) {
    return api.navigation.accessibleControls().find(function(c) { return c.enabled && c.accessibleName === label })
}
function click(button) {
    check(button && api.navigation.triggerAccessibleControl(button.accessibleName, button.panel === "StandardDialog"),
          "Cannot trigger plugin manager control: " + JSON.stringify(button))
}
function dismissScanResult() {
    if (!scanning) return
    var button = api.navigation.accessibleControls().find(function(c) {
        return c.enabled && c.accessibleName === "OK" && c.panel === "StandardDialog"
    })
    if (button && api.navigation.triggerAccessibleControl("OK", true)) {
        dismissed = true
        scanning = false
        return
    }
    api.testflow.after(100, dismissScanResult)
}
function main() {
    api.testflow.runTestCase({name:"TC3.18: Quiet startup and explicit plugin discovery", steps:[
        {name:"Open plugin manager after cached startup", func:function() {
            api.testflow.showMainWindowOnFront()
            check(Project.trackCount() === 0, "Startup opened a project")
            api.dispatcher.dispatch("command://effects/plugin-manager")
            sleep(600)
            check(findButton("Rescan plugins"), "Plugin manager did not open")
        }},
        {name:"Explicit rescan completes without revalidating cached plugins", func:function() {
            scanning = true
            api.testflow.after(100, dismissScanResult)
            // This case uses an already populated registry. FlatButton queues
            // its click, and the unchanged scan opens a completion dialog.
            click(findButton("Rescan plugins"))
            var deadline = Date.now() + 20000
            while (!dismissed && Date.now() < deadline) sleep(25)
            check(dismissed, "Cached manual scan did not complete")
            sleep(200)
            check(findButton("Rescan plugins"), "Manual rescan did not return to the plugin manager")
            check(Project.trackCount() === 0, "Plugin discovery created a project")
        }}
    ]})
}
