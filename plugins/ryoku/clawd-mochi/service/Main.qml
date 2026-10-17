pragma ComponentBehavior: Bound

import QtQuick
import Quickshell.Io

// Sends commands to the Clawd Mochi on USB through the `clawd-mochi` command. It reads the
// device's state (which page is showing / pinned) and a mirror of its screen, only while the
// panel is open. The mirror is a PNG in the plugin's state dir, written by `clawd-mochi snap`.
Item {
    id: svc

    property var pluginApi

    readonly property var moods: ["idle", "think", "work", "wait", "happy", "sleep"]
    property string page: ""
    property string pin: ""

    // Two files, alternated, so the Image always reloads a complete picture.
    property int snapTick: 0
    readonly property string snapDir: pluginApi ? pluginApi.stateDir : ""
    readonly property string snapFile: snapDir ? snapDir + "/screen-" + (snapTick % 2) + ".png" : ""
    function snap() {
        if (!snapDir || snapProc.running) return;
        snapProc.command = ["clawd-mochi", "snap", snapDir + "/screen-" + ((snapTick + 1) % 2) + ".png"];
        snapProc.running = true;
    }

    function readState() { if (!stateProc.running) stateProc.running = true; }

    // send(["mood", "happy"]) / send(["say", text]): argv straight through, no shell (R9).
    function send(args) {
        sender.command = ["clawd-mochi"].concat(args);
        sender.running = true;
    }

    // The full controller page runs from the `clawd-mochi-ui` bridge; once it is up, just reopen the tab.
    function openController() {
        if (ui.running) browser.running = true;
        else ui.running = true;
    }

    Process { id: sender; onExited: svc.readState() }
    Process {
        id: stateProc
        command: ["clawd-mochi", "state"]
        stdout: StdioCollector {
            onStreamFinished: {
                try { const st = JSON.parse(this.text); svc.page = st.page || ""; svc.pin = st.pin || ""; }
                catch (e) { svc.page = ""; svc.pin = ""; }
            }
        }
    }
    Process { id: snapProc; onExited: code => { if (code === 0) svc.snapTick += 1; } }
    Timer {
        interval: 1000
        repeat: true
        triggeredOnStart: true
        running: !!(svc.pluginApi && svc.pluginApi.panelOpen)
        onTriggered: svc.snap()
    }
    Timer {
        interval: 2000
        repeat: true
        triggeredOnStart: true
        running: !!(svc.pluginApi && svc.pluginApi.panelOpen)
        onTriggered: svc.readState()
    }
    Process { id: ui; command: ["clawd-mochi", "ui"] }
    Process { id: browser; command: ["xdg-open", "http://127.0.0.1:8484/"] }
}
