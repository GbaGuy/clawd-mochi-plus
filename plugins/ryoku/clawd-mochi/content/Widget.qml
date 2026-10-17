import QtQuick
import Ryoku.PluginKit.Singletons

// Bar glyph: a static Clawd mark (two eyes). A click only toggles the control panel.
Item {
    id: root

    property var pluginApi
    property var screen
    property bool active: false
    property string density: "glyph"
    property real s: 1
    property real widthBudget: 0

    readonly property bool open: pluginApi ? pluginApi.panelOpen : false

    implicitWidth: 20 * root.s
    implicitHeight: 18 * root.s

    Rectangle {
        anchors.centerIn: parent
        width: 20 * root.s
        height: 14 * root.s
        radius: 3 * root.s
        color: root.open || area.containsMouse ? Theme.accent : Theme.iconDim
        Behavior on color { ColorAnimation { duration: Motion.fast } }
        Row {
            anchors.centerIn: parent
            spacing: 6 * root.s
            Repeater {
                model: 2
                Rectangle { width: 3 * root.s; height: 6 * root.s; color: Theme.cardBot }
            }
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: if (root.pluginApi) root.pluginApi.togglePanel()
    }
}
