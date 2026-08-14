import QtQuick 2.14
import QtQuick.Controls 2.14
import QtQuick.Layouts 1.14

ScrollView {
    id: page
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    property var controller
    property color primaryColor: "#2563eb"
    property color textColor: "#172033"
    property color secondaryTextColor: "#667085"
    property color borderColor: "#dfe5ee"
    property bool modeSwitchLocked: false
    property int selectionRevision: 0
    property bool taskWasRunning: false
    signal switchToSlamRequested()

    ListModel { id: leftModel }
    ListModel { id: rightModel }

    Component.onCompleted: {
        for (var i = 0; i < 10; ++i) {
            leftModel.append({"selected": false})
            rightModel.append({"selected": false})
        }
    }

    Connections {
        target: controller
        function onRunningChanged() {
            if (!controller)
                return
            if (controller.running) {
                page.taskWasRunning = true
                return
            }
            // 一轮任务结束后取消旧选择，但保留红/绿结果供操作员查看。
            // 下一轮重新点击槽位时，选中状态会覆盖旧结果颜色。
            if (page.taskWasRunning) {
                page.setAll(leftModel, false)
                page.setAll(rightModel, false)
                page.taskWasRunning = false
            }
        }
    }

    function selectedCount() {
        var revision = selectionRevision
        var count = 0
        for (var i = 0; i < leftModel.count; ++i)
            if (leftModel.get(i).selected) ++count
        for (var j = 0; j < rightModel.count; ++j)
            if (rightModel.get(j).selected) ++count
        return count + revision * 0
    }

    function selectedSlots(model) {
        var values = []
        for (var i = 0; i < model.count; ++i)
            if (model.get(i).selected) values.push(i + 1)
        return values
    }

    function setAll(model, selected) {
        if (controller && controller.running) return
        for (var i = 0; i < model.count; ++i)
            model.setProperty(i, "selected", selected)
        ++selectionRevision
    }

    function statusColor(tray, slot, selected) {
        var revision = controller ? controller.slotRevision : 0
        var status = controller ? controller.slotStatus(tray, slot) : "idle"
        if (selected && (!controller || !controller.running)) return "#dbeafe"
        if (status === "success") return "#dcfce7"
        if (status === "failed") return "#fee2e2"
        if (status === "active") return "#ffedd5"
        return selected ? "#dbeafe" : "white"
    }

    function statusBorder(tray, slot, selected) {
        var revision = controller ? controller.slotRevision : 0
        var status = controller ? controller.slotStatus(tray, slot) : "idle"
        if (selected && (!controller || !controller.running)) return primaryColor
        if (status === "success") return "#16a34a"
        if (status === "failed") return "#dc2626"
        if (status === "active") return "#f97316"
        return selected ? primaryColor : "#cbd5e1"
    }

    function startTask() {
        if (!controller) return
        controller.startCalibration(
            selectedSlots(leftModel), selectedSlots(rightModel), {
                "horizontalLineHeight": horizontalLineHeight.text
            })
    }

    ColumnLayout {
        x: 20
        width: Math.max(0, page.availableWidth - 40)
        spacing: 14

        Item {
            Layout.fillWidth: true
            Layout.topMargin: 44
            implicitHeight: 54
            RowLayout {
                anchors.fill: parent
                spacing: 16
                Column {
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 2
                    Label { text: qsTr("激光标定工具"); color: page.textColor; font.pixelSize: 25; font.bold: true }
                    Label { text: qsTr("接收机激光外参自动标定"); color: page.secondaryTextColor; font.pixelSize: 13 }
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    Layout.preferredWidth: 264
                    Layout.preferredHeight: 40
                    radius: 10
                    color: "#e9eef6"
                    border.color: page.borderColor
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 3
                        Button {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            text: qsTr("SLAM 标定")
                            enabled: (!controller || !controller.running) && !page.modeSwitchLocked
                            onClicked: {
                                // 机械臂连接期间不允许跨模式，避免两个状态机混用同一设备。
                                if ((!controller || !controller.running) && !page.modeSwitchLocked)
                                    page.switchToSlamRequested()
                            }
                            background: Rectangle { radius: 7; color: parent.hovered ? "#f8fafc" : "transparent" }
                            ToolTip.visible: hovered && !enabled
                            ToolTip.text: page.modeSwitchLocked
                                              ? qsTr("请先断开机械臂连接后再切换模式")
                                              : qsTr("请先结束当前激光标定任务")
                        }
                        Button {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            text: qsTr("激光标定")
                            background: Rectangle { radius: 7; color: "white"; border.color: page.primaryColor }
                            contentItem: Label { text: parent.text; color: page.primaryColor; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 170
            radius: 12
            color: "white"
            border.color: page.borderColor
            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 18
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Label { text: qsTr("TCP 连接信息"); color: page.textColor; font.pixelSize: 15; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("机械臂"); color: page.textColor; font.pixelSize: 13 }
                        TextField { id: armHost; Layout.fillWidth: true; text: "192.168.5.1"; enabled: !controller || !controller.running }
                        TextField { id: armPort; Layout.preferredWidth: 90; text: "6601"; inputMethodHints: Qt.ImhDigitsOnly; enabled: !controller || !controller.running }
                        Button {
                            Layout.preferredWidth: 126
                            text: controller && controller.armConnected ? qsTr("断开连接") : qsTr("连接机械臂")
                            onClicked: {
                                if (!controller) return
                                if (controller.armConnected) controller.disconnectArm()
                                else controller.connectArm(armHost.text, parseInt(armPort.text))
                            }
                        }
                    }
                    RowLayout {
                        spacing: 22
                        Label { text: (controller && controller.armConnected ? "● " : "○ ") + qsTr("机械臂 %1").arg(controller && controller.armConnected ? qsTr("已就绪") : qsTr("未连接")); color: controller && controller.armConnected ? "#16a34a" : page.secondaryTextColor }
                        Label { text: (controller && controller.wifiConnected ? "● " : "○ ") + qsTr("Wi-Fi %1").arg(controller && controller.wifiConnected ? qsTr("已连接") : qsTr("未连接")); color: controller && controller.wifiConnected ? "#16a34a" : page.secondaryTextColor }
                        Label { text: (controller && controller.receiverConnected ? "● " : "○ ") + qsTr("接收机 %1").arg(controller && controller.receiverConnected ? qsTr("已连接") : qsTr("未连接")); color: controller && controller.receiverConnected ? "#16a34a" : page.secondaryTextColor }
                    }
                }
                Rectangle { width: 1; Layout.fillHeight: true; color: page.borderColor }
                ColumnLayout {
                    Layout.preferredWidth: 410
                    spacing: 8
                    Label { text: qsTr("接收机设备信息"); color: page.textColor; font.pixelSize: 16; font.bold: true }
                    GridLayout {
                        columns: 6
                        columnSpacing: 8
                        Label { text: qsTr("HW:"); color: page.secondaryTextColor; font.pixelSize: 16 }
                        Label { text: controller && controller.currentHardware.length ? controller.currentHardware : "--"; color: page.textColor; font.pixelSize: 17; font.bold: true }
                        Label { text: qsTr("SN:"); color: page.secondaryTextColor; font.pixelSize: 16 }
                        Label { text: controller && controller.currentSn.length ? controller.currentSn : "--"; color: page.textColor; font.pixelSize: 17; font.bold: true }
                        Label { text: qsTr("版本:"); color: page.secondaryTextColor; font.pixelSize: 16 }
                        Label { text: controller && controller.currentVersion.length ? controller.currentVersion : "--"; color: page.textColor; font.pixelSize: 17; font.bold: true }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 72
            radius: 12
            color: controller && controller.errorText.length ? "#fff1f2" : "#eff6ff"
            border.color: controller && controller.errorText.length ? "#fecdd3" : "#bfdbfe"
            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                Label { text: "●"; color: controller && controller.errorText.length ? "#dc2626" : page.primaryColor; font.pixelSize: 14 }
                Label { text: qsTr("当前进度:"); color: page.textColor; font.pixelSize: 14; font.bold: true }
                Label { Layout.fillWidth: true; text: controller ? controller.progressText : qsTr("等待后端就绪"); color: controller && controller.errorText.length ? "#dc2626" : page.primaryColor; font.pixelSize: 14; wrapMode: Text.Wrap }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 410
                radius: 12; color: "white"; border.color: page.borderColor
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 14; spacing: 12
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("自动视觉测量结果 (mm)"); color: page.textColor; font.pixelSize: 15; font.bold: true }
                        Item { Layout.fillWidth: true }
                        CheckBox {
                            text: qsTr("模拟测试")
                            checked: controller ? controller.singleCameraDebug : false
                            enabled: controller && !controller.running && !controller.cameraBusy
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("仍会打开真实摄像头并执行完整标定流程，但视觉测量使用模拟数据，不要求两个参考圆")
                            onToggled: if (controller) controller.singleCameraDebug = checked
                        }
                    }
                    GridLayout {
                        Layout.fillWidth: true; columns: 4; columnSpacing: 8; rowSpacing: 8
                        Label { text: "" }
                        Label { text: qsTr("近点"); color: page.secondaryTextColor; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                        Label { text: qsTr("远点"); color: page.secondaryTextColor; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                        Label { text: qsTr("激光长度"); color: page.secondaryTextColor; horizontalAlignment: Text.AlignHCenter; Layout.fillWidth: true }
                        Label { text: qsTr("水平测量"); color: page.textColor }
                        TextField { id: horizontalNear; Layout.fillWidth: true; text: controller && controller.horizontalNear !== 0 ? controller.horizontalNear.toFixed(3) : "--"; horizontalAlignment: Text.AlignHCenter; readOnly: true }
                        TextField { id: horizontalFar; Layout.fillWidth: true; text: controller && controller.horizontalFar !== 0 ? controller.horizontalFar.toFixed(3) : "--"; horizontalAlignment: Text.AlignHCenter; readOnly: true }
                        TextField { id: horizontalLength; Layout.fillWidth: true; text: controller && controller.horizontalLength !== 0 ? controller.horizontalLength.toFixed(3) : "--"; horizontalAlignment: Text.AlignHCenter; readOnly: true }
                        Label { text: qsTr("垂直测量"); color: page.textColor }
                        TextField { id: verticalNear; Layout.fillWidth: true; text: controller && controller.verticalNear !== 0 ? controller.verticalNear.toFixed(3) : "--"; horizontalAlignment: Text.AlignHCenter; readOnly: true }
                        TextField { id: verticalFar; Layout.fillWidth: true; text: controller && controller.verticalFar !== 0 ? controller.verticalFar.toFixed(3) : "--"; horizontalAlignment: Text.AlignHCenter; readOnly: true }
                        TextField { id: verticalLength; Layout.fillWidth: true; text: controller && controller.verticalLength !== 0 ? controller.verticalLength.toFixed(3) : "--"; horizontalAlignment: Text.AlignHCenter; readOnly: true }
                        Label { text: qsTr("GRR20 水平线高"); color: page.textColor }
                        TextField { id: horizontalLineHeight; Layout.fillWidth: true; text: "620"; horizontalAlignment: Text.AlignHCenter; readOnly: true }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("实时激光距离:"); color: page.secondaryTextColor }
                        Label { Layout.fillWidth: true; text: controller && controller.laserDistance > 0 ? controller.laserDistance.toFixed(3) + " mm" : "--"; color: page.textColor; font.pixelSize: 16; font.bold: true }
                        Label { text: qsTr("当前计算高度:"); color: page.secondaryTextColor }
                        Label { Layout.fillWidth: true; text: controller && controller.laserHeight !== 0 ? controller.laserHeight.toFixed(3) + " mm" : "--"; color: page.textColor; font.pixelSize: 16; font.bold: true }
                    }
                    Label { visible: controller && controller.errorText.length > 0; text: controller ? controller.errorText : ""; color: "#dc2626"; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Item { Layout.fillHeight: true }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 10
                        Button { Layout.fillWidth: true; text: controller && controller.running ? qsTr("终止任务") : qsTr("开始标定"); enabled: controller && (controller.running || (controller.armConnected && page.selectedCount() > 0)); onClicked: controller.running ? controller.stopCalibration() : page.startTask() }
                        Button { Layout.fillWidth: true; text: qsTr("复位接收机标定"); enabled: controller && controller.receiverConnected && !controller.running; onClicked: controller.resetReceiverCalibration() }
                        Button { Layout.fillWidth: true; text: controller && controller.paused ? qsTr("继续") : qsTr("暂停"); enabled: controller && controller.running; onClicked: controller.togglePause() }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 440
                Layout.preferredHeight: 410
                radius: 12; color: "white"; border.color: page.borderColor
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 14; spacing: 10
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("托盘选择"); color: page.textColor; font.pixelSize: 15; font.bold: true }
                        Label { text: qsTr("已选择 %1 个槽位").arg(page.selectedCount()); color: page.secondaryTextColor; font.pixelSize: 11 }
                        Item { Layout.fillWidth: true }
                        Label { text: qsTr("红色表示标定失败"); color: "#ef4444"; font.pixelSize: 10 }
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10
                        Repeater {
                            model: [ {"title": qsTr("左托盘  2×5"), "tray": "left", "selection": leftModel}, {"title": qsTr("右托盘  2×5"), "tray": "right", "selection": rightModel} ]
                            Rectangle {
                                id: trayCard
                                property string trayKey: modelData.tray
                                property var selectionModel: modelData.selection
                                Layout.fillWidth: true; Layout.fillHeight: true; radius: 8; color: "#f8fafc"; border.color: page.borderColor
                                ColumnLayout {
                                    anchors.fill: parent; anchors.margins: 10; spacing: 8
                                    Label { Layout.alignment: Qt.AlignHCenter; text: modelData.title; color: page.textColor; font.bold: true }
                                    Grid {
                                        Layout.alignment: Qt.AlignHCenter; Layout.fillHeight: true; columns: 2; rows: 5; spacing: 8
                                        Repeater {
                                            model: trayCard.selectionModel
                                            Rectangle {
                                                width: 48; height: 48; radius: 7
                                                color: page.statusColor(trayCard.trayKey, index + 1, selected)
                                                border.width: (selected || (controller && controller.slotStatus(trayCard.trayKey, index + 1) !== "idle")) ? 2 : 1
                                                border.color: page.statusBorder(trayCard.trayKey, index + 1, selected)
                                                Label { anchors.centerIn: parent; text: index + 1; color: parent.border.color; font.bold: selected }
                                                MouseArea {
                                                    anchors.fill: parent; enabled: !controller || !controller.running; cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                                    onClicked: { trayCard.selectionModel.setProperty(index, "selected", !selected); ++page.selectionRevision }
                                                }
                                            }
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Button { Layout.fillWidth: true; text: qsTr("全选"); enabled: !controller || !controller.running; onClicked: page.setAll(trayCard.selectionModel, true) }
                                        Button { Layout.fillWidth: true; text: qsTr("取消"); enabled: !controller || !controller.running; onClicked: page.setAll(trayCard.selectionModel, false) }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 360
                Layout.preferredHeight: 410
                radius: 12; color: "white"; border.color: page.borderColor
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 14; spacing: 10
                    Label { text: qsTr("摄像头画面"); color: page.textColor; font.pixelSize: 15; font.bold: true }
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 8; color: "#eef7fa"; border.color: page.primaryColor; border.width: 2
                        Image {
                            id: cameraImage
                            anchors.fill: parent
                            anchors.margins: 5
                            source: controller ? controller.cameraImageUrl : ""
                            fillMode: Image.PreserveAspectFit
                            cache: false
                            visible: source.toString().length > 0
                        }
                        Column {
                            anchors.centerIn: parent; spacing: 8
                            visible: !cameraImage.visible
                            BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: controller && controller.cameraBusy; visible: running }
                            Label { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("摄像头处理画面"); color: page.secondaryTextColor }
                            Label { anchors.horizontalCenter: parent.horizontalCenter; width: 300; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; text: controller ? controller.cameraStatus : qsTr("等待视觉测量"); color: "#98a2b3"; font.pixelSize: 11 }
                        }
                    }
                    Label { Layout.fillWidth: true; text: controller ? controller.cameraStatus : qsTr("等待视觉测量"); color: page.secondaryTextColor; font.pixelSize: 11; elide: Text.ElideRight }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Label {
                            text: qsTr("面静置状态")
                            color: page.textColor
                            font.pixelSize: 12
                            font.bold: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            Repeater {
                                model: 6
                                delegate: RowLayout {
                                    spacing: 3
                                    readonly property bool completed: controller
                                                                         && ((controller.faceRestBits & (1 << index)) !== 0)
                                    Rectangle {
                                        Layout.preferredWidth: 13
                                        Layout.preferredHeight: 13
                                        radius: 3
                                        color: parent.completed ? "#22c55e" : "#ffffff"
                                        border.color: parent.completed ? "#16a34a" : "#cbd5e1"
                                    }
                                    Label {
                                        text: qsTr("面%1").arg(index + 1)
                                        color: parent.completed ? "#15803d" : page.secondaryTextColor
                                        font.pixelSize: 11
                                    }
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 360
            Layout.bottomMargin: 18
            radius: 12; color: "white"; border.color: page.borderColor
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 14; spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("标定结果与运行日志"); color: page.textColor; font.pixelSize: 15; font.bold: true }
                    Label { text: qsTr("成功 %1  失败 %2  进行中 %3").arg(controller ? controller.successCount : 0).arg(controller ? controller.failureCount : 0).arg(controller ? controller.activeCount : 0); color: page.secondaryTextColor }
                    Item { Layout.fillWidth: true }
                    Button { text: qsTr("清空日志"); enabled: controller; onClicked: controller.clearLog() }
                    Button { text: qsTr("导出数据"); enabled: controller && controller.results.length > 0; onClicked: controller.exportResults() }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 12
                    Rectangle {
                        Layout.fillWidth: true; Layout.fillHeight: true; radius: 8; color: "#f8fafc"; border.color: page.borderColor
                        ColumnLayout {
                            anchors.fill: parent; anchors.margins: 8; spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                Label { Layout.preferredWidth: 48; text: qsTr("托盘"); font.bold: true; color: page.secondaryTextColor }
                                Label { Layout.preferredWidth: 42; text: qsTr("槽位"); font.bold: true; color: page.secondaryTextColor }
                                Label { Layout.preferredWidth: 100; text: qsTr("SN"); font.bold: true; color: page.secondaryTextColor }
                                Label { Layout.preferredWidth: 90; text: qsTr("距离 mm"); font.bold: true; color: page.secondaryTextColor }
                                Label { Layout.preferredWidth: 90; text: qsTr("高度 mm"); font.bold: true; color: page.secondaryTextColor }
                                Label { Layout.fillWidth: true; text: qsTr("结果"); font.bold: true; color: page.secondaryTextColor }
                            }
                            ListView {
                                Layout.fillWidth: true; Layout.fillHeight: true
                                model: controller ? controller.results : []
                                clip: true
                                delegate: RowLayout {
                                    width: ListView.view.width; height: 28
                                    Label { Layout.preferredWidth: 48; text: modelData.trayText }
                                    Label { Layout.preferredWidth: 42; text: modelData.slot }
                                    Label { Layout.preferredWidth: 100; text: modelData.sn || "--" }
                                    Label { Layout.preferredWidth: 90; text: Number(modelData.distance).toFixed(2) }
                                    Label { Layout.preferredWidth: 90; text: Number(modelData.height).toFixed(2) }
                                    Label { Layout.fillWidth: true; text: modelData.result + (modelData.error ? ": " + modelData.error : ""); color: modelData.success ? "#16a34a" : "#dc2626"; elide: Text.ElideRight }
                                }
                                Label { anchors.centerIn: parent; visible: !controller || controller.results.length === 0; text: qsTr("暂无标定数据"); color: "#98a2b3" }
                            }
                        }
                    }
                    Rectangle {
                        Layout.preferredWidth: 540; Layout.fillHeight: true; radius: 8; color: "#f8fafc"; border.color: page.borderColor
                        ScrollView {
                            anchors.fill: parent; anchors.margins: 8; clip: true
                            TextArea {
                                id: logTextArea
                                text: controller ? controller.logText : ""
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.WrapAnywhere
                                color: page.textColor
                                font.family: "Consolas"
                                font.pixelSize: 11
                                background: null
                                onTextChanged: Qt.callLater(function() {
                                    logTextArea.cursorPosition = logTextArea.length
                                })
                            }
                        }
                    }
                }
            }
        }
    }
}
