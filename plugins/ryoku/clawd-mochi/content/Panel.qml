pragma ComponentBehavior: Bound

import QtQuick
import Ryoku.PluginKit.Singletons

// Panel: controls only. Mood buttons, say, backlight, and a link to the full
// controller (drawing). Every action is an explicit click.
Item {
    id: root

    property var pluginApi
    property string density: "full"
    property real s: 1
    property real widthBudget: 300
    property bool active: false

    readonly property var service: pluginApi ? pluginApi.mainInstance : null
    readonly property string pin: service ? service.pin : ""
    readonly property string page: service ? service.page : ""
    function send(args) { if (root.service) root.service.send(args); }

    implicitWidth: root.widthBudget
    implicitHeight: col.implicitHeight + 24 * root.s

    component Chip: Rectangle {
        id: chip
        property string label
        property bool on: false
        signal clicked
        implicitWidth: chipText.implicitWidth + 20 * root.s
        implicitHeight: 28 * root.s
        radius: Theme.radius
        color: chipArea.pressed ? Theme.threadBg : chipArea.containsMouse ? Theme.sheen : "transparent"
        border.color: chip.on || chipArea.containsMouse ? Theme.accent : Theme.hair
        Text {
            id: chipText
            anchors.centerIn: parent
            text: chip.label
            color: chip.on || chipArea.containsMouse ? Theme.accent : Theme.bright
            font.family: Theme.mono
            font.pixelSize: 12 * root.s
        }
        MouseArea {
            id: chipArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.clicked()
        }
    }

    Column {
        id: col
        x: 12 * root.s
        y: 12 * root.s
        width: root.width - 24 * root.s
        spacing: 10 * root.s

        // live mirror of the Mochi's screen
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 180 * root.s
            height: width
            radius: 14 * root.s
            color: "#da1100"
            clip: true
            Image {
                anchors.fill: parent
                source: root.service && root.service.snapTick > 0 ? "file://" + root.service.snapFile : ""
                cache: false
                smooth: true
                fillMode: Image.PreserveAspectFit
            }
        }

        // pages: let them take turns, or pin one
        Row {
            spacing: 6 * root.s
            Chip { label: "‹ prev"; width: (col.width - 12 * root.s) / 3; onClicked: root.send(["page", "prev"]) }
            Chip { label: "auto"; on: root.pin === "auto"; width: (col.width - 12 * root.s) / 3; onClicked: root.send(["page", "auto"]) }
            Chip { label: "next ›"; width: (col.width - 12 * root.s) / 3; onClicked: root.send(["page", "next"]) }
        }
        Grid {
            columns: 3
            spacing: 6 * root.s
            Repeater {
                model: ["clawd", "clock", "usage", "timer", "music", "stats", "history", "weather", "calendar", "events", "notifs"]
                Chip {
                    required property string modelData
                    width: (col.width - 12 * root.s) / 3
                    label: modelData
                    on: root.pin === modelData
                    onClicked: root.send(["page", modelData])
                }
            }
        }
        Text {
            visible: root.page !== ""
            width: col.width
            horizontalAlignment: Text.AlignHCenter
            text: "showing " + root.page + (root.pin === "auto" ? " · pages take turns" : " · pinned")
            color: Theme.faint
            font.family: Theme.mono
            font.pixelSize: 11 * root.s
        }
        Chip {
            width: col.width
            label: "clear notification history"
            onClicked: root.send(["notifs", "wipe"])
        }

        Grid {
            columns: 3
            spacing: 6 * root.s
            Repeater {
                model: root.service ? root.service.moods : []
                Chip {
                    required property string modelData
                    width: (col.width - 12 * root.s) / 3
                    label: modelData
                    onClicked: root.send(["mood", modelData])
                }
            }
        }

        Row {
            spacing: 6 * root.s
            Rectangle {
                width: col.width - sayChip.width - 6 * root.s
                height: 28 * root.s
                radius: Theme.radius
                color: "transparent"
                border.color: sayInput.activeFocus ? Theme.accent : Theme.border
                TextInput {
                    id: sayInput
                    anchors.fill: parent
                    anchors.leftMargin: 8 * root.s
                    anchors.rightMargin: 8 * root.s
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.bright
                    selectionColor: Theme.accent
                    font.family: Theme.mono
                    font.pixelSize: 12 * root.s
                    maximumLength: 60
                    clip: true
                    onAccepted: sayChip.clicked()
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !sayInput.text && !sayInput.activeFocus
                        text: "say something…"
                        color: Theme.faint
                        font: sayInput.font
                    }
                }
            }
            Chip {
                id: sayChip
                label: "say"
                onClicked: {
                    if (sayInput.text.trim()) root.send(["say", sayInput.text.trim()]);
                    sayInput.text = "";
                }
            }
        }

        // focus timer: counts down on the Mochi itself
        Row {
            spacing: 6 * root.s
            Chip { label: "focus 25m"; onClicked: root.send(["timer", "1500", "focus"]) }
            Chip { label: "break 5m"; onClicked: root.send(["timer", "300", "break"]) }
            Chip { label: "stop timer"; onClicked: root.send(["timer", "0"]) }
        }

        Row {
            spacing: 6 * root.s
            Chip { label: "light on"; onClicked: root.send(["bl", "1"]) }
            Chip { label: "light off"; onClicked: root.send(["bl", "0"]) }
            Chip {
                label: "controller ↗"
                onClicked: if (root.service) root.service.openController()
            }
        }
    }
}
