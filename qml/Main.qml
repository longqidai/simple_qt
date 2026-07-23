import QtQuick 2.14
import QtQuick.Controls 2.14
import QtQuick.Layouts 1.14

ApplicationWindow {
    id: window
    visible: true
    width: 1120
    height: 780
    minimumWidth: 780
    minimumHeight: 620
    title: qsTr("SLAM 标定工具")
    color: "#f3f6fb"

    readonly property color primaryColor: "#2563eb"
    readonly property color textColor: "#172033"
    readonly property color secondaryTextColor: "#667085"
    readonly property color borderColor: "#dfe5ee"
    property bool logExpanded: false
    property int selectionRevision: 0

    ListModel {
        id: leftSelection
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
    }

    ListModel {
        id: rightSelection
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
        ListElement { selected: false }
    }

    function selectedSlots(model) {
        var result = []
        for (var i = 0; i < model.count; ++i) {
            if (model.get(i).selected)
                result.push(i + 1)
        }
        return result
    }

    function selectedSlotCount() {
        var revision = selectionRevision
        return selectedSlots(leftSelection).length
                + selectedSlots(rightSelection).length + revision * 0
    }

    function backendSlotState(side, index) {
        var states = side === "left" ? tcpController.leftSlotStates
                                      : tcpController.rightSlotStates
        return index < states.length ? states[index] : "idle"
    }

    function visualSlotState(side, index, selected) {
        var backendState = backendSlotState(side, index)
        return backendState !== "idle" ? backendState : (selected ? "selected" : "idle")
    }

    function slotFillColor(state) {
        if (state === "failed") return "#fee2e2"
        if (state === "success") return "#dcfce7"
        if (state === "running") return "#ffedd5"
        if (state === "pending" || state === "selected") return "#dbeafe"
        return "#ffffff"
    }

    function slotBorderColor(state) {
        if (state === "failed") return "#dc2626"
        if (state === "success") return "#16a34a"
        if (state === "running") return "#f97316"
        if (state === "pending" || state === "selected") return primaryColor
        return "#cbd5e1"
    }

    function slotTextColor(state) {
        if (state === "failed") return "#b91c1c"
        if (state === "success") return "#15803d"
        if (state === "running") return "#c2410c"
        if (state === "pending" || state === "selected") return primaryColor
        return textColor
    }

    Dialog {
        id: stopDialog
        modal: true
        anchors.centerIn: parent
        title: qsTr("中止当前任务")
        standardButtons: Dialog.Yes | Dialog.No
        Label {
            width: 310
            wrapMode: Text.WordWrap
            text: qsTr("确定要中止当前 SLAM 标定任务吗？程序会先停止采集，并在需要时通知机械臂放回设备。")
            color: window.textColor
        }
        onAccepted: tcpController.toggleArmTask([], [])
    }

    ScrollView {
        id: page
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            x: 20
            width: Math.max(0, page.availableWidth - 40)
            spacing: 14

            Item {
                Layout.fillWidth: true
                Layout.topMargin: 16
                implicitHeight: 54
                Column {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Label { text: qsTr("SLAM 标定工具"); color: window.textColor; font.pixelSize: 25; font.bold: true }
                    Label { text: qsTr("机械臂与设备自动标定"); color: window.secondaryTextColor; font.pixelSize: 13 }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 146
                radius: 12
                color: "white"
                border.color: window.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Label { text: qsTr("机械臂连接"); color: window.textColor; font.bold: true; font.pixelSize: 15 }
                        TextField {
                            id: hostField
                            Layout.fillWidth: true
                            Layout.minimumWidth: 180
                            text: tcpController.host
                            enabled: !tcpController.connected
                            placeholderText: qsTr("服务器地址")
                            onEditingFinished: tcpController.host = text.trim()
                        }
                        TextField {
                            id: portField
                            Layout.preferredWidth: 100
                            text: tcpController.port.toString()
                            enabled: !tcpController.connected
                            validator: IntValidator { bottom: 1; top: 65535 }
                            onEditingFinished: if (acceptableInput) tcpController.port = parseInt(text)
                        }
                        Button {
                            Layout.preferredWidth: 132
                            text: tcpController.connected ? qsTr("断开连接") : qsTr("连接机械臂")
                            onClicked: tcpController.toggleConnection()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 142; Layout.preferredHeight: 34; radius: 17
                            color: tcpController.armReady ? "#ecfdf3" : (tcpController.connected ? "#fff7ed" : "#f2f4f7")
                            border.color: tcpController.armReady ? "#bbf7d0" : (tcpController.connected ? "#fed7aa" : "#e4e7ec")
                            Row { anchors.centerIn: parent; spacing: 7
                                Rectangle { width: 9; height: 9; radius: 5; color: tcpController.armReady ? "#22c55e" : (tcpController.connected ? "#f97316" : "#98a2b3") }
                                Label { text: tcpController.armReady ? qsTr("机械臂已就绪") : (tcpController.connected ? qsTr("等待机械臂握手") : qsTr("机械臂未连接")); color: window.textColor; font.pixelSize: 12 }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 122; Layout.preferredHeight: 34; radius: 17
                            color: tcpController.wifiConnected ? "#ecfdf3" : "#f2f4f7"; border.color: tcpController.wifiConnected ? "#bbf7d0" : "#e4e7ec"
                            Row { anchors.centerIn: parent; spacing: 7
                                Rectangle { width: 9; height: 9; radius: 5; color: tcpController.wifiConnected ? "#22c55e" : "#98a2b3" }
                                Label { text: tcpController.wifiConnected ? qsTr("Wi-Fi 已连接") : qsTr("Wi-Fi 未连接"); color: window.textColor; font.pixelSize: 12 }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 122; Layout.preferredHeight: 34; radius: 17
                            color: tcpController.mqttConnected ? "#ecfdf3" : "#f2f4f7"; border.color: tcpController.mqttConnected ? "#bbf7d0" : "#e4e7ec"
                            Row { anchors.centerIn: parent; spacing: 7
                                Rectangle { width: 9; height: 9; radius: 5; color: tcpController.mqttConnected ? "#22c55e" : "#98a2b3" }
                                Label { text: tcpController.mqttConnected ? qsTr("MQTT 已连接") : qsTr("MQTT 未连接"); color: window.textColor; font.pixelSize: 12 }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 122; Layout.preferredHeight: 34; radius: 17
                            color: tcpController.ftpDownloading ? "#eff6ff" : "#f2f4f7"; border.color: tcpController.ftpDownloading ? "#bfdbfe" : "#e4e7ec"
                            Row { anchors.centerIn: parent; spacing: 7
                                Rectangle { width: 9; height: 9; radius: 5; color: tcpController.ftpDownloading ? window.primaryColor : "#98a2b3" }
                                Label { text: tcpController.ftpDownloading ? qsTr("FTP 下载中") : qsTr("FTP 空闲"); color: window.textColor; font.pixelSize: 12 }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 68
                radius: 12
                color: tcpController.lastError.length ? "#fff1f2" : "#eff6ff"
                border.color: tcpController.lastError.length ? "#fecdd3" : "#bfdbfe"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20
                    anchors.rightMargin: 20
                    spacing: 12
                    Rectangle { width: 11; height: 11; radius: 6; color: tcpController.lastError.length ? "#dc2626" : window.primaryColor }
                    Label { text: qsTr("当前进度："); color: window.textColor; font.bold: true; font.pixelSize: 16 }
                    Label {
                        Layout.fillWidth: true
                        text: tcpController.currentProgress
                        color: tcpController.lastError.length ? "#b42318" : window.primaryColor
                        font.bold: true
                        font.pixelSize: 17
                        elide: Text.ElideRight
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 960 ? 2 : 1
                columnSpacing: 14
                rowSpacing: 14

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 700
                    Layout.minimumWidth: 0
                    implicitHeight: 350
                    radius: 12
                    color: "white"
                    border.color: window.borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("托盘选择"); color: window.textColor; font.bold: true; font.pixelSize: 17 }
                            Label {
                                text: qsTr("已选择 %1 个槽位").arg(window.selectedSlotCount())
                                color: window.secondaryTextColor
                                font.pixelSize: 12
                            }
                            Item { Layout.fillWidth: true }
                            Label { text: qsTr("红色表示标定失败"); color: "#dc2626"; font.pixelSize: 12 }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 14

                            Repeater {
                                model: 2
                                Rectangle {
                                    id: trayDelegate
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 10
                                    color: "#f8fafc"
                                    border.color: window.borderColor
                                    property var selectionModel: index === 0 ? leftSelection : rightSelection
                                    property string sideName: index === 0 ? "left" : "right"

                                    Label {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        anchors.top: parent.top
                                        anchors.topMargin: 12
                                        text: trayDelegate.sideName === "left" ? qsTr("左托盘  2×3") : qsTr("右托盘  2×3")
                                        color: window.textColor
                                        font.bold: true
                                    }

                                    Grid {
                                        anchors.centerIn: parent
                                        anchors.verticalCenterOffset: 12
                                        columns: 3
                                        rows: 2
                                        spacing: 12

                                        Repeater {
                                            model: trayDelegate.selectionModel
                                            Rectangle {
                                                property string visualState: window.visualSlotState(trayDelegate.sideName, index, selected)
                                                width: Math.max(54, Math.min(74, (trayDelegate.width - 64) / 3))
                                                height: 62
                                                radius: 9
                                                color: window.slotFillColor(visualState)
                                                border.width: visualState === "idle" ? 1 : 2
                                                border.color: window.slotBorderColor(visualState)

                                                Label {
                                                    anchors.centerIn: parent
                                                    text: index + 1
                                                    color: window.slotTextColor(parent.visualState)
                                                    font.pixelSize: 18
                                                    font.bold: true
                                                }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    enabled: !tcpController.armRunning
                                                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                    onClicked: {
                                                        if (tcpController.selectedCount > 0)
                                                            tcpController.clearTaskResults()
                                                        trayDelegate.selectionModel.setProperty(index, "selected", !selected)
                                                        ++window.selectionRevision
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 360
                    Layout.minimumWidth: 0
                    implicitHeight: 350
                    radius: 12
                    color: "white"
                    border.color: window.borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18
                        spacing: 11

                        Label { text: qsTr("当前设备"); color: window.textColor; font.bold: true; font.pixelSize: 17 }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 14
                            rowSpacing: 9
                            Label { text: qsTr("托盘槽位"); color: window.secondaryTextColor }
                            Label { text: tcpController.currentSlot > 0 ? tcpController.currentTray + " " + tcpController.currentSlot : "--"; color: window.textColor; font.bold: true }
                            Label { text: qsTr("SN"); color: window.secondaryTextColor }
                            Label { text: tcpController.currentSn.length ? tcpController.currentSn : "--"; color: window.textColor; font.bold: tcpController.currentSn.length > 0; elide: Text.ElideRight; Layout.fillWidth: true }
                            Label { text: qsTr("硬件号"); color: window.secondaryTextColor }
                            Label { text: tcpController.currentHardware.length ? tcpController.currentHardware : "--"; color: window.textColor }
                            Label { text: qsTr("版本号"); color: window.secondaryTextColor }
                            Label { text: tcpController.currentVersion.length ? tcpController.currentVersion : "--"; color: window.textColor }
                            Label { text: qsTr("采集状态"); color: window.secondaryTextColor }
                            Label { text: tcpController.collectStatus.length ? tcpController.collectStatus : "--"; color: window.textColor }
                            Label { text: qsTr("数据目录"); color: window.secondaryTextColor }
                            Label { text: tcpController.collectPath.length ? tcpController.collectPath : "--"; color: window.textColor; elide: Text.ElideMiddle; Layout.fillWidth: true }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1; color: window.borderColor }

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("成功 %1").arg(tcpController.successCount); color: "#15803d"; font.bold: true }
                            Label { text: qsTr("失败 %1").arg(tcpController.failureCount); color: "#b91c1c"; font.bold: true }
                            Label {
                                text: qsTr("共 %1").arg(tcpController.selectedCount > 0
                                                         ? tcpController.selectedCount
                                                         : window.selectedSlotCount())
                                color: window.secondaryTextColor
                            }
                            Item { Layout.fillWidth: true }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: failureText.implicitHeight + 18
                            visible: tcpController.lastError.length > 0
                            radius: 8
                            color: "#fff1f2"
                            border.color: "#fecdd3"
                            Label {
                                id: failureText
                                anchors.fill: parent
                                anchors.margins: 9
                                text: tcpController.lastError
                                color: "#b42318"
                                wrapMode: Text.WordWrap
                            }
                        }

                        Item { Layout.fillHeight: true }

                        Button {
                            id: actionButton
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            enabled: tcpController.armRunning
                                     || (tcpController.connected
                                         && tcpController.armReady
                                         && window.selectedSlotCount() > 0)
                            text: tcpController.armRunning ? qsTr("中止任务") : qsTr("开始标定")
                            font.bold: true
                            onClicked: {
                                if (tcpController.armRunning) {
                                    stopDialog.open()
                                } else {
                                    tcpController.toggleArmTask(window.selectedSlots(leftSelection),
                                                                window.selectedSlots(rightSelection))
                                }
                            }
                            background: Rectangle {
                                radius: 8
                                color: !actionButton.enabled ? "#d0d5dd" : (tcpController.armRunning ? "#dc2626" : window.primaryColor)
                            }
                            contentItem: Label {
                                text: actionButton.text
                                color: "white"
                                font: actionButton.font
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.bottomMargin: 18
                implicitHeight: window.logExpanded ? 230 : 56
                radius: 12
                color: "white"
                border.color: window.borderColor
                Behavior on implicitHeight { NumberAnimation { duration: 140 } }

                RowLayout {
                    id: logHeader
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: 18
                    anchors.rightMargin: 14
                    height: 56
                    Label { text: qsTr("运行日志"); color: window.textColor; font.bold: true; font.pixelSize: 15 }
                    Label { text: qsTr("%1 条").arg(tcpController.debugLog.length); color: window.secondaryTextColor; font.pixelSize: 12 }
                    Item { Layout.fillWidth: true }
                    Button { flat: true; text: window.logExpanded ? qsTr("收起") : qsTr("展开"); onClicked: window.logExpanded = !window.logExpanded }
                }

                Rectangle {
                    visible: window.logExpanded
                    anchors.top: logHeader.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 18
                    anchors.rightMargin: 18
                    anchors.bottomMargin: 14
                    radius: 8
                    color: "#f8fafc"
                    border.color: window.borderColor
                    clip: true

                    ListView {
                        id: logView
                        anchors.fill: parent
                        anchors.margins: 10
                        model: tcpController.debugLog
                        clip: true
                        spacing: 3
                        onCountChanged: positionViewAtEnd()
                        delegate: Label { width: logView.width; text: modelData; color: "#465066"; font.family: "Consolas"; font.pixelSize: 12; wrapMode: Text.Wrap }
                        Label { anchors.centerIn: parent; visible: logView.count === 0; text: qsTr("暂无运行日志"); color: "#98a2b3" }
                        ScrollBar.vertical: ScrollBar { }
                    }
                }
            }
        }
    }
}
