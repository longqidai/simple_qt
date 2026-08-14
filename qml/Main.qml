import QtQuick 2.14
import QtQuick.Controls 2.14
import QtQuick.Layouts 1.14

ApplicationWindow {
    id: window
    visible: true
    width: 1400
    height: 800
    minimumWidth: 1000
    minimumHeight: 800
    title: calibrationMode === "laser" ? qsTr("激光标定工具") : qsTr("SLAM 标定工具")
    color: "#f3f6fb"

    readonly property color primaryColor: "#2563eb"
    readonly property color textColor: "#172033"
    readonly property color secondaryTextColor: "#667085"
    readonly property color borderColor: "#dfe5ee"
    property bool logExpanded: false
    property int selectionRevision: 0
    property bool closeAfterSafeStop: false
    property string calibrationMode: "laser"
    property var databaseRows: []
    // 任一模式已经连接机械臂后锁定标定模式，断开机械臂后才允许切换。
    readonly property bool modeSwitchLocked: tcpController.connected || laserController.armConnected

    // 根据窗口宽度动态调整字体和间距
    property real scaleFactor: Math.max(0.75, Math.min(1.5, width / 1400))
    
    onClosing: {
        if (laserController.running) {
            close.accepted = false
            closeAfterSafeStop = true
            laserController.stopCalibration()
            return
        }
        if (!closeAfterSafeStop && tcpController.armRunning) {
            close.accepted = false
            closeAfterSafeStop = true
            tcpController.toggleArmTask([], [])
        }
    }

    Connections {
        target: tcpController
        onArmRunningChanged: {
            if (window.closeAfterSafeStop && !tcpController.armRunning)
                window.close()
        }
    }

    Connections {
        target: laserController
        onRunningChanged: {
            if (window.closeAfterSafeStop && !laserController.running)
                window.close()
        }
    }

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
        if (state === "downloading") return "#e0f2fe"
        if (state === "waiting_download") return "#f3e8ff"
        if (state === "pending" || state === "selected") return "#dbeafe"
        return "#ffffff"
    }

    function slotBorderColor(state) {
        if (state === "failed") return "#dc2626"
        if (state === "success") return "#16a34a"
        if (state === "running") return "#f97316"
        if (state === "downloading") return "#0284c7"
        if (state === "waiting_download") return "#9333ea"
        if (state === "pending" || state === "selected") return primaryColor
        return "#cbd5e1"
    }

    function slotTextColor(state) {
        if (state === "failed") return "#b91c1c"
        if (state === "success") return "#15803d"
        if (state === "running") return "#c2410c"
        if (state === "downloading") return "#0369a1"
        if (state === "waiting_download") return "#7e22ce"
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
            width: 310 * scaleFactor
            wrapMode: Text.WordWrap
            text: qsTr("确定要中止当前 SLAM 标定任务吗？程序会先停止采集，并在需要时通知机械臂放回设备。")
            color: window.textColor
            font.pixelSize: 13 * scaleFactor
        }
        onAccepted: tcpController.toggleArmTask([], [])
    }

    Rectangle {
        id: moreButton
        z: 100
        x: 20
        y: 8
        width: 78
        height: 30
        radius: 4
        color: moreMouse.containsMouse ? "#454541" : "#363632"
        Label {
            anchors.centerIn: parent
            text: qsTr("更多  ▾")
            color: "white"
            font.pixelSize: 13
        }
        MouseArea {
            id: moreMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: moreMenu.open()
        }
        Menu {
            id: moreMenu
            y: parent.height
            MenuItem {
                text: qsTr("查找数据")
                onTriggered: {
                    searchSn.text = ""
                    window.databaseRows = laserController.queryResults("")
                    databaseDialog.open()
                }
            }
            MenuItem {
                text: qsTr("显示更多窗口")
                onTriggered: debugDialog.open()
            }
        }
    }

    Dialog {
        id: databaseDialog
        modal: true
        width: Math.min(window.width - 60, 1120)
        height: Math.min(window.height - 60, 650)
        anchors.centerIn: parent
        title: qsTr("激光标定数据库")
        standardButtons: Dialog.Close

        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: searchSn
                    Layout.fillWidth: true
                    placeholderText: qsTr("输入 SN，留空查看全部记录")
                    onAccepted: window.databaseRows = laserController.queryResults(text)
                }
                Button {
                    text: qsTr("搜索")
                    onClicked: window.databaseRows = laserController.queryResults(searchSn.text)
                }
                Label {
                    text: qsTr("%1 条").arg(window.databaseRows.length)
                    color: window.secondaryTextColor
                }
            }
            Flickable {
                id: databaseTableFlick
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: databaseTable.width
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

                Item {
                    id: databaseTable
                    width: 1730
                    height: databaseTableFlick.height - 14

                    Rectangle {
                        id: databaseHeader
                        width: parent.width
                        height: 34
                        color: "#eef2f7"
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8
                            Label { Layout.preferredWidth: 45; text: "ID"; font.bold: true }
                            Label { Layout.preferredWidth: 155; text: qsTr("时间"); font.bold: true }
                            Label { Layout.preferredWidth: 55; text: qsTr("托盘"); font.bold: true }
                            Label { Layout.preferredWidth: 45; text: qsTr("槽位"); font.bold: true }
                            Label { Layout.preferredWidth: 120; text: "SN"; font.bold: true }
                            Label { Layout.preferredWidth: 65; text: qsTr("硬件"); font.bold: true }
                            Label { Layout.preferredWidth: 100; text: qsTr("版本"); font.bold: true }
                            Label { Layout.preferredWidth: 85; text: qsTr("距离 mm"); font.bold: true }
                            Label { Layout.preferredWidth: 85; text: qsTr("高度 mm"); font.bold: true }
                            Label { Layout.preferredWidth: 90; text: "Pitch"; font.bold: true }
                            Label { Layout.preferredWidth: 90; text: "Yaw"; font.bold: true }
                            Label { Layout.preferredWidth: 75; text: qsTr("成功状态"); font.bold: true }
                            Label { Layout.preferredWidth: 300; text: qsTr("错误信息"); font.bold: true }
                            Label { Layout.preferredWidth: 330; text: "BiasBuf"; font.bold: true }
                        }
                    }

                    ListView {
                        id: databaseList
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: databaseHeader.bottom
                        anchors.bottom: parent.bottom
                        clip: true
                        model: window.databaseRows
                        spacing: 1
                        delegate: Rectangle {
                            width: databaseList.width
                            height: 40
                            color: index % 2 ? "#f8fafc" : "white"
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 8
                                Label { Layout.preferredWidth: 45; text: modelData.id }
                                Label { Layout.preferredWidth: 155; text: modelData.time; elide: Text.ElideRight; ToolTip.text: text; ToolTip.visible: hovered }
                                Label { Layout.preferredWidth: 55; text: modelData.trayText || "--" }
                                Label { Layout.preferredWidth: 45; text: modelData.slot }
                                Label { Layout.preferredWidth: 120; text: modelData.sn || "--"; elide: Text.ElideRight }
                                Label { Layout.preferredWidth: 65; text: modelData.hardware || "--"; elide: Text.ElideRight }
                                Label { Layout.preferredWidth: 100; text: modelData.version || "--"; elide: Text.ElideRight }
                                Label { Layout.preferredWidth: 85; text: Number(modelData.distance).toFixed(3) }
                                Label { Layout.preferredWidth: 85; text: Number(modelData.height).toFixed(3) }
                                Label { Layout.preferredWidth: 90; text: Number(modelData.pitch).toFixed(6) }
                                Label { Layout.preferredWidth: 90; text: Number(modelData.yaw).toFixed(6) }
                                Label {
                                    Layout.preferredWidth: 75
                                    text: modelData.success ? "1 (成功)" : "0 (失败)"
                                    color: modelData.success ? "#15803d" : "#b91c1c"
                                }
                                Label { Layout.preferredWidth: 300; text: modelData.error || "--"; elide: Text.ElideRight; ToolTip.text: text; ToolTip.visible: hovered }
                                Label { Layout.preferredWidth: 330; text: modelData.bias || "[]"; elide: Text.ElideRight; ToolTip.text: text; ToolTip.visible: hovered }
                            }
                        }
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        Label {
                            anchors.centerIn: parent
                            visible: databaseList.count === 0
                            text: qsTr("没有找到标定记录")
                            color: "#98a2b3"
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: debugDialog
        modal: true
        width: Math.min(window.width - 80, 900)
        height: Math.min(window.height - 80, 620)
        anchors.centerIn: parent
        title: qsTr("更多调试信息")
        standardButtons: Dialog.Close
        ColumnLayout {
            anchors.fill: parent
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: window.calibrationMode === "laser"
                          ? qsTr("激光标定通信与流程日志")
                          : qsTr("SLAM 标定通信与流程日志")
                    color: window.secondaryTextColor
                }
                Button {
                    text: qsTr("清空")
                    onClicked: {
                        if (window.calibrationMode === "laser") laserController.clearLog()
                        else tcpController.clearDebugLog()
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: "#111827"; radius: 6
                ScrollView {
                    anchors.fill: parent; anchors.margins: 8
                    TextArea {
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        text: window.calibrationMode === "laser"
                              ? laserController.logText
                              : tcpController.debugLog.join("\n")
                        color: "#d1fae5"
                        background: null
                        font.family: "Consolas"
                        font.pixelSize: 12
                        onTextChanged: cursorPosition = length
                    }
                }
            }
        }
    }

    ScrollView {
        id: page
        visible: window.calibrationMode === "slam"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            x: 20 * scaleFactor
            width: Math.max(0, page.availableWidth - 40 * scaleFactor)
            spacing: 14 * scaleFactor

            Item {
                Layout.fillWidth: true
                Layout.topMargin: 44 * scaleFactor
                implicitHeight: 54 * scaleFactor
                RowLayout {
                    anchors.fill: parent
                    spacing: 16 * scaleFactor

                    Column {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 2 * scaleFactor
                        Label { text: qsTr("SLAM 标定工具"); color: window.textColor; font.pixelSize: 25 * scaleFactor; font.bold: true }
                        Label { text: qsTr("机械臂与设备自动标定"); color: window.secondaryTextColor; font.pixelSize: 13 * scaleFactor }
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        Layout.preferredWidth: 264 * scaleFactor
                        Layout.preferredHeight: 40 * scaleFactor
                        radius: 10 * scaleFactor
                        color: "#e9eef6"
                        border.color: window.borderColor

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 3 * scaleFactor
                            spacing: 3 * scaleFactor

                            Button {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                text: qsTr("SLAM 标定")
                                background: Rectangle { radius: 7 * scaleFactor; color: "white"; border.color: window.primaryColor }
                                contentItem: Label { text: parent.text; color: window.primaryColor; font.bold: true; font.pixelSize: 12 * scaleFactor; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            }
                            Button {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                text: qsTr("激光标定")
                                enabled: !window.modeSwitchLocked && !tcpController.armRunning
                                onClicked: {
                                    // 除了禁用按钮外再做一次状态校验，防止其他事件绕过界面锁定。
                                    if (!window.modeSwitchLocked && !tcpController.armRunning)
                                        window.calibrationMode = "laser"
                                }
                                background: Rectangle { radius: 7 * scaleFactor; color: parent.hovered ? "#f8fafc" : "transparent" }
                                contentItem: Label { text: parent.text; color: parent.enabled ? window.textColor : "#98a2b3"; font.pixelSize: 12 * scaleFactor; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                                ToolTip.visible: hovered && !enabled
                                ToolTip.text: window.modeSwitchLocked
                                              ? qsTr("请先断开机械臂连接后再切换模式")
                                              : qsTr("请先结束当前 SLAM 标定任务")
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 146 * scaleFactor
                radius: 12 * scaleFactor
                color: "white"
                border.color: window.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 18 * scaleFactor
                    spacing: 12 * scaleFactor

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10 * scaleFactor
                        Label { text: qsTr("机械臂连接"); color: window.textColor; font.bold: true; font.pixelSize: 15 * scaleFactor }
                        TextField {
                            id: hostField
                            Layout.fillWidth: true
                            Layout.minimumWidth: 180 * scaleFactor
                            text: tcpController.host
                            enabled: !tcpController.connected
                            placeholderText: qsTr("服务器地址")
                            font.pixelSize: 12 * scaleFactor
                            onEditingFinished: tcpController.host = text.trim()
                        }
                        TextField {
                            id: portField
                            Layout.preferredWidth: 100 * scaleFactor
                            text: tcpController.port.toString()
                            enabled: !tcpController.connected
                            font.pixelSize: 12 * scaleFactor
                            validator: IntValidator { bottom: 1; top: 65535 }
                            onEditingFinished: if (acceptableInput) tcpController.port = parseInt(text)
                        }
                        Button {
                            Layout.preferredWidth: 132 * scaleFactor
                            enabled: !tcpController.armRunning
                            text: tcpController.connected ? qsTr("断开连接") : qsTr("连接机械臂")
                            font.pixelSize: 12 * scaleFactor
                            onClicked: tcpController.toggleConnection()
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10 * scaleFactor

                        Rectangle {
                            Layout.preferredWidth: 142 * scaleFactor; Layout.preferredHeight: 34 * scaleFactor; radius: 17 * scaleFactor
                            color: tcpController.armReady ? "#ecfdf3" : (tcpController.connected ? "#fff7ed" : "#f2f4f7")
                            border.color: tcpController.armReady ? "#bbf7d0" : (tcpController.connected ? "#fed7aa" : "#e4e7ec")
                            Row { anchors.centerIn: parent; spacing: 7 * scaleFactor
                                Rectangle { width: 9 * scaleFactor; height: 9 * scaleFactor; radius: 5 * scaleFactor; color: tcpController.armReady ? "#22c55e" : (tcpController.connected ? "#f97316" : "#98a2b3") }
                                Label { text: tcpController.armReady ? qsTr("机械臂已就绪") : (tcpController.connected ? qsTr("等待机械臂握手") : qsTr("机械臂未连接")); color: window.textColor; font.pixelSize: 12 * scaleFactor }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 122 * scaleFactor; Layout.preferredHeight: 34 * scaleFactor; radius: 17 * scaleFactor
                            color: tcpController.wifiConnected ? "#ecfdf3" : "#f2f4f7"; border.color: tcpController.wifiConnected ? "#bbf7d0" : "#e4e7ec"
                            Row { anchors.centerIn: parent; spacing: 7 * scaleFactor
                                Rectangle { width: 9 * scaleFactor; height: 9 * scaleFactor; radius: 5 * scaleFactor; color: tcpController.wifiConnected ? "#22c55e" : "#98a2b3" }
                                Label { text: tcpController.wifiConnected ? qsTr("Wi-Fi 已连接") : qsTr("Wi-Fi 未连接"); color: window.textColor; font.pixelSize: 12 * scaleFactor }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 122 * scaleFactor; Layout.preferredHeight: 34 * scaleFactor; radius: 17 * scaleFactor
                            color: tcpController.mqttConnected ? "#ecfdf3" : "#f2f4f7"; border.color: tcpController.mqttConnected ? "#bbf7d0" : "#e4e7ec"
                            Row { anchors.centerIn: parent; spacing: 7 * scaleFactor
                                Rectangle { width: 9 * scaleFactor; height: 9 * scaleFactor; radius: 5 * scaleFactor; color: tcpController.mqttConnected ? "#22c55e" : "#98a2b3" }
                                Label { text: tcpController.mqttConnected ? qsTr("MQTT 已连接") : qsTr("MQTT 未连接"); color: window.textColor; font.pixelSize: 12 * scaleFactor }
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: 122 * scaleFactor; Layout.preferredHeight: 34 * scaleFactor; radius: 17 * scaleFactor
                            color: tcpController.ftpDownloading ? "#eff6ff" : "#f2f4f7"; border.color: tcpController.ftpDownloading ? "#bfdbfe" : "#e4e7ec"
                            Row { anchors.centerIn: parent; spacing: 7 * scaleFactor
                                Rectangle { width: 9 * scaleFactor; height: 9 * scaleFactor; radius: 5 * scaleFactor; color: tcpController.ftpDownloading ? window.primaryColor : "#98a2b3" }
                                Label { text: tcpController.ftpDownloading ? qsTr("FTP 下载中") : qsTr("FTP 空闲"); color: window.textColor; font.pixelSize: 12 * scaleFactor }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 68 * scaleFactor
                radius: 12 * scaleFactor
                color: tcpController.lastError.length ? "#fff1f2" : "#eff6ff"
                border.color: tcpController.lastError.length ? "#fecdd3" : "#bfdbfe"
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 20 * scaleFactor
                    anchors.rightMargin: 20 * scaleFactor
                    spacing: 12 * scaleFactor
                    Rectangle { width: 11 * scaleFactor; height: 11 * scaleFactor; radius: 6 * scaleFactor; color: tcpController.lastError.length ? "#dc2626" : window.primaryColor }
                    Label { text: qsTr("当前进度："); color: window.textColor; font.bold: true; font.pixelSize: 16 * scaleFactor }
                    Label {
                        Layout.fillWidth: true
                        text: tcpController.currentProgress
                        color: tcpController.lastError.length ? "#b42318" : window.primaryColor
                        font.bold: true
                        font.pixelSize: 17 * scaleFactor
                        elide: Text.ElideRight
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 960 * scaleFactor ? 2 : 1
                columnSpacing: 14 * scaleFactor
                rowSpacing: 14 * scaleFactor

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 700 * scaleFactor
                    Layout.minimumWidth: 0
                    implicitHeight: 350 * scaleFactor
                    radius: 12 * scaleFactor
                    color: "white"
                    border.color: window.borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18 * scaleFactor
                        spacing: 12 * scaleFactor

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("托盘选择"); color: window.textColor; font.bold: true; font.pixelSize: 17 * scaleFactor }
                            Label {
                                text: qsTr("已选择 %1 个槽位").arg(window.selectedSlotCount())
                                color: window.secondaryTextColor
                                font.pixelSize: 12 * scaleFactor
                            }
                            Item { Layout.fillWidth: true }
                            Label { text: qsTr("红色表示标定失败"); color: "#dc2626"; font.pixelSize: 12 * scaleFactor }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 14 * scaleFactor

                            Repeater {
                                model: 2
                                Rectangle {
                                    id: trayDelegate
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 10 * scaleFactor
                                    color: "#f8fafc"
                                    border.color: window.borderColor
                                    property var selectionModel: index === 0 ? leftSelection : rightSelection
                                    property string sideName: index === 0 ? "left" : "right"

                                    Label {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        anchors.top: parent.top
                                        anchors.topMargin: 12 * scaleFactor
                                        text: trayDelegate.sideName === "left" ? qsTr("左托盘  2×3") : qsTr("右托盘  2×3")
                                        color: window.textColor
                                        font.bold: true
                                        font.pixelSize: 14 * scaleFactor
                                    }

                                    Grid {
                                        anchors.centerIn: parent
                                        anchors.verticalCenterOffset: 12 * scaleFactor
                                        columns: 3
                                        rows: 2
                                        spacing: 12 * scaleFactor

                                        Repeater {
                                            model: trayDelegate.selectionModel
                                            Rectangle {
                                                property string visualState: window.visualSlotState(trayDelegate.sideName, index, selected)
                                                width: Math.max(54 * scaleFactor, Math.min(74 * scaleFactor, (trayDelegate.width - 64 * scaleFactor) / 3))
                                                height: 62 * scaleFactor
                                                radius: 9 * scaleFactor
                                                color: window.slotFillColor(visualState)
                                                border.width: visualState === "idle" ? 1 : 2
                                                border.color: window.slotBorderColor(visualState)

                                                Label {
                                                    anchors.centerIn: parent
                                                    text: index + 1
                                                    color: window.slotTextColor(parent.visualState)
                                                    font.pixelSize: 18 * scaleFactor
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
                    Layout.preferredWidth: 360 * scaleFactor
                    Layout.minimumWidth: 0
                    implicitHeight: 350 * scaleFactor
                    radius: 12 * scaleFactor
                    color: "white"
                    border.color: window.borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 18 * scaleFactor
                        spacing: 11 * scaleFactor

                        Label { text: qsTr("当前设备"); color: window.textColor; font.bold: true; font.pixelSize: 17 * scaleFactor }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: 2
                            columnSpacing: 14 * scaleFactor
                            rowSpacing: 9 * scaleFactor
                            Label { text: qsTr("托盘槽位"); color: window.secondaryTextColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: tcpController.currentSlot > 0 ? tcpController.currentTray + " " + tcpController.currentSlot : "--"; color: window.textColor; font.bold: true; font.pixelSize: 12 * scaleFactor }
                            Label { text: qsTr("SN"); color: window.secondaryTextColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: tcpController.currentSn.length ? tcpController.currentSn : "--"; color: window.textColor; font.bold: tcpController.currentSn.length > 0; elide: Text.ElideRight; Layout.fillWidth: true; font.pixelSize: 12 * scaleFactor }
                            Label { text: qsTr("硬件号"); color: window.secondaryTextColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: tcpController.currentHardware.length ? tcpController.currentHardware : "--"; color: window.textColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: qsTr("版本号"); color: window.secondaryTextColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: tcpController.currentVersion.length ? tcpController.currentVersion : "--"; color: window.textColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: qsTr("采集状态"); color: window.secondaryTextColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: tcpController.collectStatus.length ? tcpController.collectStatus : "--"; color: window.textColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: qsTr("数据目录"); color: window.secondaryTextColor; font.pixelSize: 12 * scaleFactor }
                            Label { text: tcpController.collectPath.length ? tcpController.collectPath : "--"; color: window.textColor; elide: Text.ElideMiddle; Layout.fillWidth: true; font.pixelSize: 12 * scaleFactor }
                        }

                        Rectangle { Layout.fillWidth: true; height: 1 * scaleFactor; color: window.borderColor }

                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: qsTr("成功 %1").arg(tcpController.successCount); color: "#15803d"; font.bold: true; font.pixelSize: 12 * scaleFactor }
                            Label { text: qsTr("失败 %1").arg(tcpController.failureCount); color: "#b91c1c"; font.bold: true; font.pixelSize: 12 * scaleFactor }
                            Label {
                                text: qsTr("共 %1").arg(tcpController.selectedCount > 0
                                                         ? tcpController.selectedCount
                                                         : window.selectedSlotCount())
                                color: window.secondaryTextColor
                                font.pixelSize: 12 * scaleFactor
                            }
                            Item { Layout.fillWidth: true }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: failureText.implicitHeight + 18 * scaleFactor
                            visible: tcpController.lastError.length > 0
                            radius: 8 * scaleFactor
                            color: "#fff1f2"
                            border.color: "#fecdd3"
                            Label {
                                id: failureText
                                anchors.fill: parent
                                anchors.margins: 9 * scaleFactor
                                text: tcpController.lastError
                                color: "#b42318"
                                font.pixelSize: 12 * scaleFactor
                                wrapMode: Text.WordWrap
                            }
                        }

                        Item { Layout.fillHeight: true }

                        Button {
                            id: actionButton
                            Layout.fillWidth: true
                            Layout.preferredHeight: 46 * scaleFactor
                            enabled: tcpController.armRunning
                                     || (tcpController.connected
                                         && tcpController.armReady
                                         && window.selectedSlotCount() > 0)
                            text: tcpController.armRunning ? qsTr("中止任务") : qsTr("开始标定")
                            font.bold: true
                            font.pixelSize: 13 * scaleFactor
                            onClicked: {
                                if (tcpController.armRunning) {
                                    stopDialog.open()
                                } else {
                                    tcpController.toggleArmTask(window.selectedSlots(leftSelection),
                                                                window.selectedSlots(rightSelection))
                                }
                            }
                            background: Rectangle {
                                radius: 8 * scaleFactor
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
                Layout.bottomMargin: 18 * scaleFactor
                implicitHeight: window.logExpanded ? 230 * scaleFactor : 56 * scaleFactor
                radius: 12 * scaleFactor
                color: "white"
                border.color: window.borderColor
                Behavior on implicitHeight { NumberAnimation { duration: 140 } }

                RowLayout {
                    id: logHeader
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin: 18 * scaleFactor
                    anchors.rightMargin: 14 * scaleFactor
                    height: 56 * scaleFactor
                    Label { text: qsTr("运行日志"); color: window.textColor; font.bold: true; font.pixelSize: 15 * scaleFactor }
                    Label { text: qsTr("%1 条").arg(tcpController.debugLog.length); color: window.secondaryTextColor; font.pixelSize: 12 * scaleFactor }
                    Item { Layout.fillWidth: true }
                    Button { flat: true; text: window.logExpanded ? qsTr("收起") : qsTr("展开"); font.pixelSize: 12 * scaleFactor; onClicked: window.logExpanded = !window.logExpanded }
                }

                Rectangle {
                    visible: window.logExpanded
                    anchors.top: logHeader.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 18 * scaleFactor
                    anchors.rightMargin: 18 * scaleFactor
                    anchors.bottomMargin: 14 * scaleFactor
                    radius: 8 * scaleFactor
                    color: "#f8fafc"
                    border.color: window.borderColor
                    clip: true

                    ListView {
                        id: logView
                        anchors.fill: parent
                        anchors.margins: 10 * scaleFactor
                        model: tcpController.debugLog
                        clip: true
                        spacing: 3 * scaleFactor
                        onCountChanged: positionViewAtEnd()
                        delegate: Label { width: logView.width; text: modelData; color: "#465066"; font.family: "Consolas"; font.pixelSize: 12 * scaleFactor; wrapMode: Text.Wrap }
                        Label { anchors.centerIn: parent; visible: logView.count === 0; text: qsTr("暂无运行日志"); color: "#98a2b3"; font.pixelSize: 12 * scaleFactor }
                        ScrollBar.vertical: ScrollBar { }
                    }
                }
            }
        }
    }

    LaserCalibrationPage {
        anchors.fill: parent
        visible: window.calibrationMode === "laser"
        primaryColor: window.primaryColor
        textColor: window.textColor
        secondaryTextColor: window.secondaryTextColor
        borderColor: window.borderColor
        controller: laserController
        modeSwitchLocked: window.modeSwitchLocked
        onSwitchToSlamRequested: {
            // 信号入口同样校验，确保机械臂连接期间无法切换模式。
            if (!window.modeSwitchLocked && !laserController.running)
                window.calibrationMode = "slam"
        }
    }
}
