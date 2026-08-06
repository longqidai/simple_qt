import QtQuick 2.14
import QtQuick.Controls 2.14
import QtQuick.Layouts 1.14

ScrollView {
    id: laserPage
    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    property color primaryColor: "#2563eb"
    property color textColor: "#172033"
    property color secondaryTextColor: "#667085"
    property color borderColor: "#dfe5ee"
    property int selectionRevision: 0
    property int currentImagePage: 0
    property int totalImagePages: 6
    property string previewMessage: qsTr("激光标定后端暂未接入，当前页面仅用于界面预览")

    signal switchToSlamRequested()

    ListModel { id: laserLeftSelection }
    ListModel { id: laserRightSelection }

    Component.onCompleted: {
        for (var i = 0; i < 6; ++i) {
            laserLeftSelection.append({"selected": false})
            laserRightSelection.append({"selected": false})
        }
    }

    function selectedCount() {
        var revision = selectionRevision
        var count = 0
        for (var i = 0; i < laserLeftSelection.count; ++i) {
            if (laserLeftSelection.get(i).selected)
                ++count
        }
        for (var j = 0; j < laserRightSelection.count; ++j) {
            if (laserRightSelection.get(j).selected)
                ++count
        }
        return count + revision * 0
    }

    function setAll(model, value) {
        for (var i = 0; i < model.count; ++i)
            model.setProperty(i, "selected", value)
        ++selectionRevision
    }

    ColumnLayout {
        x: 20
        width: Math.max(0, laserPage.availableWidth - 40)
        spacing: 14

        // ========== 顶部标题栏 ==========
        Item {
            Layout.fillWidth: true
            Layout.topMargin: 16
            implicitHeight: 54

            RowLayout {
                anchors.fill: parent
                spacing: 16

                Column {
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 2
                    Label { 
                        text: qsTr("激光标定工具")
                        color: laserPage.textColor
                        font.pixelSize: 25
                        font.bold: true
                    }
                    Label { 
                        text: qsTr("接收机外参自动标定")
                        color: laserPage.secondaryTextColor
                        font.pixelSize: 13
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    Layout.preferredWidth: 264
                    Layout.preferredHeight: 40
                    radius: 10
                    color: "#e9eef6"
                    border.color: laserPage.borderColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 3
                        spacing: 3

                        Button {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: qsTr("SLAM 标定")
                            onClicked: laserPage.switchToSlamRequested()
                            background: Rectangle { 
                                radius: 7
                                color: parent.hovered ? "#f8fafc" : "transparent"
                            }
                            contentItem: Label { 
                                text: parent.text
                                color: laserPage.textColor
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        Button {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: qsTr("激光标定")
                            background: Rectangle { 
                                radius: 7
                                color: "white"
                                border.color: laserPage.primaryColor
                            }
                            contentItem: Label { 
                                text: parent.text
                                color: laserPage.primaryColor
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
            }
        }

        // ========== 提示消息 ==========
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 58
            radius: 12
            color: "#fffbeb"
            border.color: "#fde68a"
            visible: laserPage.previewMessage.length > 0

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 10
                Rectangle { 
                    width: 10
                    height: 10
                    radius: 5
                    color: "#f59e0b"
                }
                Label { 
                    text: laserPage.previewMessage
                    color: "#92400e"
                    font.bold: true
                    Layout.fillWidth: true
                }
            }
        }

        // ========== 主要内容区域（网格布局）==========
        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 3
            rows: 2
            columnSpacing: 14
            rowSpacing: 14

            // ========== 第一行，第一列：TCP 连接信息 + 接收机设备信息（合并） ==========
            Rectangle {
                Layout.column: 0
                Layout.columnSpan: 1
                Layout.row: 0
                Layout.fillWidth: true
                Layout.preferredHeight: 130
                radius: 12
                color: "white"
                border.color: laserPage.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    Label { 
                        text: qsTr("TCP 连接信息")
                        color: laserPage.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }

                    RowLayout {
                        spacing: 8
                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            color: "#98a2b3"
                        }
                        Label {
                            text: qsTr("机械臂")
                            color: laserPage.textColor
                            font.pixelSize: 12
                        }
                        Label {
                            text: qsTr("未连接")
                            color: laserPage.secondaryTextColor
                            font.pixelSize: 12
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        spacing: 8
                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            color: "#98a2b3"
                        }
                        Label {
                            text: qsTr("接收机")
                            color: laserPage.textColor
                            font.pixelSize: 12
                        }
                        Label {
                            text: qsTr("未连接")
                            color: laserPage.secondaryTextColor
                            font.pixelSize: 12
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Button {
                        Layout.fillWidth: true
                        text: qsTr("连接接收机")
                        enabled: false
                    }
                }
            }

            // ========== 第一行，第二列：接收机设备信息（紧跟TCP连接信息右边） ==========
            Rectangle {
                Layout.column: 1
                Layout.row: 0
                Layout.fillWidth: true
                Layout.preferredHeight: 130
                radius: 12
                color: "white"
                border.color: laserPage.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8

                    Label { 
                        text: qsTr("接收机设备信息")
                        color: laserPage.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        RowLayout {
                            spacing: 6
                            Label { 
                                text: qsTr("HW:")
                                color: laserPage.secondaryTextColor
                                font.pixelSize: 12
                            }
                            Label { 
                                text: qsTr("--")
                                color: laserPage.textColor
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }

                        RowLayout {
                            spacing: 6
                            Label { 
                                text: qsTr("SN:")
                                color: laserPage.secondaryTextColor
                                font.pixelSize: 12
                            }
                            Label { 
                                text: qsTr("--")
                                color: laserPage.textColor
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }

                        RowLayout {
                            spacing: 6
                            Label { 
                                text: qsTr("版本:")
                                color: laserPage.secondaryTextColor
                                font.pixelSize: 12
                            }
                            Label { 
                                text: qsTr("--")
                                color: laserPage.textColor
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ========== 第一行，第三列：面标示意图（跨行扩大）==========
            Rectangle {
                Layout.column: 2
                Layout.row: 0
                Layout.rowSpan: 2
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 12
                color: "white"
                border.color: laserPage.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    // 标题和按钮行
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label { 
                            text: qsTr("面标示意图")
                            color: laserPage.textColor
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Label { 
                            text: qsTr("撮像头 %1").arg(laserPage.currentImagePage + 1)
                            color: laserPage.secondaryTextColor
                            font.pixelSize: 11
                        }

                        Item { Layout.fillWidth: true }

                        Button {
                            Layout.preferredWidth: 50
                            text: qsTr("折叠")
                            enabled: false
                            font.pixelSize: 10
                        }
                        Button {
                            Layout.preferredWidth: 50
                            text: qsTr("录制")
                            enabled: false
                            font.pixelSize: 10
                        }
                        Button {
                            Layout.preferredWidth: 50
                            text: qsTr("放大")
                            enabled: false
                            font.pixelSize: 10
                        }
                    }

                    // 示意图区域
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: "#e8f4f8"
                        border.color: laserPage.primaryColor
                        border.width: 2

                        Label {
                            anchors.centerIn: parent
                            text: qsTr("TextLabel")
                            color: laserPage.secondaryTextColor
                            font.pixelSize: 12
                        }
                    }

                    // 面标页码导航 - 放在下方
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        radius: 6
                        color: "#f8fafc"
                        border.color: laserPage.borderColor

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 6

                            Label {
                                text: qsTr("面标显示:")
                                color: laserPage.textColor
                                font.pixelSize: 11
                                font.bold: true
                            }

                            Repeater {
                                model: laserPage.totalImagePages
                                Rectangle {
                                    Layout.preferredWidth: 32
                                    Layout.preferredHeight: 24
                                    radius: 4
                                    color: index === laserPage.currentImagePage ? laserPage.primaryColor : "#e5e7eb"
                                    border.color: laserPage.borderColor
                                    border.width: index === laserPage.currentImagePage ? 2 : 1

                                    Label {
                                        anchors.centerIn: parent
                                        text: qsTr("面%1").arg(index + 1)
                                        color: index === laserPage.currentImagePage ? "white" : laserPage.textColor
                                        font.pixelSize: 9
                                        font.bold: index === laserPage.currentImagePage
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: laserPage.currentImagePage = index
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }
                        }
                    }
                }
            }

            // ========== 第二行，第一列：第一步标定测量信息 ==========
            Rectangle {
                Layout.column: 0
                Layout.row: 1
                Layout.fillWidth: true
                Layout.preferredHeight: 430
                radius: 12
                color: "white"
                border.color: laserPage.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 12

                    Label { 
                        text: qsTr("第一步:标定测量信息 (mm)")
                        color: laserPage.textColor
                        font.pixelSize: 14
                        font.bold: true
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 4
                        columnSpacing: 8
                        rowSpacing: 8

                        Label { 
                            text: qsTr("")
                            color: laserPage.textColor
                            font.pixelSize: 11
                        }
                        Label { 
                            text: qsTr("近点")
                            color: laserPage.secondaryTextColor
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            font.pixelSize: 11
                        }
                        Label { 
                            text: qsTr("远点")
                            color: laserPage.secondaryTextColor
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            font.pixelSize: 11
                        }
                        Label { 
                            text: qsTr("激光长度")
                            color: laserPage.secondaryTextColor
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                            font.pixelSize: 11
                        }

                        Label { 
                            text: qsTr("水平测量")
                            color: laserPage.textColor
                            font.pixelSize: 11
                        }
                        TextField { 
                            Layout.fillWidth: true
                            readOnly: true
                            text: "888"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }
                        TextField { 
                            Layout.fillWidth: true
                            readOnly: true
                            text: "780"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }
                        TextField { 
                            Layout.fillWidth: true
                            readOnly: true
                            text: "4758"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Label { 
                            text: qsTr("垂直测量")
                            color: laserPage.textColor
                            font.pixelSize: 11
                        }
                        TextField { 
                            Layout.fillWidth: true
                            readOnly: true
                            text: "592"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }
                        TextField { 
                            Layout.fillWidth: true
                            readOnly: true
                            text: "440"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }
                        TextField { 
                            Layout.fillWidth: true
                            readOnly: true
                            text: "4800"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Label { 
                            text: qsTr("水平线高")
                            color: laserPage.textColor
                            font.pixelSize: 11
                        }
                        TextField { 
                            Layout.fillWidth: true
                            readOnly: true
                            placeholderText: "--"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }
                        
                    }

                    Item { 
                        Layout.fillWidth: true
                        Layout.preferredHeight: 12
                    }

                    // 激光距离和激光高度
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        Label { 
                            text: qsTr("激光长度:")
                            color: laserPage.secondaryTextColor
                            font.pixelSize: 12
                        }
                        TextField { 
                            Layout.preferredWidth: 80
                            readOnly: true
                            placeholderText: "--"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Label { 
                            text: qsTr("激光高度:")
                            color: laserPage.secondaryTextColor
                            font.pixelSize: 12
                        }
                        TextField { 
                            Layout.preferredWidth: 80
                            readOnly: true
                            placeholderText: "--"
                            font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Item { Layout.fillHeight: true }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Button { 
                            Layout.fillWidth: true
                            text: qsTr("开始标定")
                            enabled: false
                        }
                        Button { 
                            Layout.fillWidth: true
                            text: qsTr("复位")
                            enabled: false
                        }
                        Button { 
                            Layout.fillWidth: true
                            text: qsTr("暂停")
                            enabled: false
                        }
                    }
                }
            }

            // ========== 第二行，第二列：托盘槽位选择 ==========
            Rectangle {
                Layout.column: 1
                Layout.row: 1
                Layout.fillWidth: true
                Layout.preferredHeight: 430
                radius: 12
                color: "white"
                border.color: laserPage.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    RowLayout {
                        Layout.fillWidth: true
                        Label { 
                            text: qsTr("第二步:与机械臂同向面向")
                            color: laserPage.textColor
                            font.pixelSize: 14
                            font.bold: true
                        }
                        Label { 
                            text: qsTr("已选择 %1 个").arg(laserPage.selectedCount())
                            color: laserPage.secondaryTextColor
                            font.pixelSize: 11
                        }
                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 10

                        // 左托盘
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 8
                            color: "#f8fafc"
                            border.color: laserPage.borderColor

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 8

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: qsTr("左")
                                    color: laserPage.textColor
                                    font.bold: true
                                    font.pixelSize: 13
                                }

                                Grid {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.fillHeight: true
                                    columns: 3
                                    rows: 2
                                    spacing: 6

                                    Repeater {
                                        model: laserLeftSelection
                                        Rectangle {
                                            width: 40
                                            height: 40
                                            radius: 6
                                            color: selected ? "#dbeafe" : "white"
                                            border.width: selected ? 2 : 1
                                            border.color: selected ? laserPage.primaryColor : "#cbd5e1"

                                            Label {
                                                anchors.centerIn: parent
                                                text: index + 1
                                                color: selected ? laserPage.primaryColor : laserPage.textColor
                                                font.bold: selected
                                                font.pixelSize: 12
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    laserLeftSelection.setProperty(index, "selected", !selected)
                                                    ++laserPage.selectionRevision
                                                }
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Button { 
                                        Layout.fillWidth: true
                                        text: qsTr("全选")
                                        font.pixelSize: 11
                                        onClicked: laserPage.setAll(laserLeftSelection, true)
                                    }
                                    Button { 
                                        Layout.fillWidth: true
                                        text: qsTr("取消")
                                        font.pixelSize: 11
                                        onClicked: laserPage.setAll(laserLeftSelection, false)
                                    }
                                }
                            }
                        }

                        // 右托盘
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            radius: 8
                            color: "#f8fafc"
                            border.color: laserPage.borderColor

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 8

                                Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: qsTr("右")
                                    color: laserPage.textColor
                                    font.bold: true
                                    font.pixelSize: 13
                                }

                                Grid {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.fillHeight: true
                                    columns: 3
                                    rows: 2
                                    spacing: 6

                                    Repeater {
                                        model: laserRightSelection
                                        Rectangle {
                                            width: 40
                                            height: 40
                                            radius: 6
                                            color: selected ? "#dbeafe" : "white"
                                            border.width: selected ? 2 : 1
                                            border.color: selected ? laserPage.primaryColor : "#cbd5e1"

                                            Label {
                                                anchors.centerIn: parent
                                                text: index + 1
                                                color: selected ? laserPage.primaryColor : laserPage.textColor
                                                font.bold: selected
                                                font.pixelSize: 12
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    laserRightSelection.setProperty(index, "selected", !selected)
                                                    ++laserPage.selectionRevision
                                                }
                                            }
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Button { 
                                        Layout.fillWidth: true
                                        text: qsTr("全选")
                                        font.pixelSize: 11
                                        onClicked: laserPage.setAll(laserRightSelection, true)
                                    }
                                    Button { 
                                        Layout.fillWidth: true
                                        text: qsTr("取消")
                                        font.pixelSize: 11
                                        onClicked: laserPage.setAll(laserRightSelection, false)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ========== 标定结果数据表 ==========
        Rectangle {
            Layout.fillWidth: true
            Layout.bottomMargin: 18
            implicitHeight: 320
            radius: 12
            color: "white"
            border.color: laserPage.borderColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    Label { 
                        text: qsTr("标定结果")
                        color: laserPage.textColor
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Item { Layout.fillWidth: true }
                    Button { 
                        text: qsTr("导出数据")
                        enabled: false
                    }
                }

                // 统计卡片
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    Rectangle {
                        Layout.preferredWidth: 140
                        Layout.preferredHeight: 60
                        radius: 8
                        color: "#f0f4f8"
                        border.color: laserPage.borderColor

                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("标定成功")
                                color: laserPage.secondaryTextColor
                                font.pixelSize: 11
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "0"
                                color: "#16a34a"
                                font.pixelSize: 18
                                font.bold: true
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 140
                        Layout.preferredHeight: 60
                        radius: 8
                        color: "#f0f4f8"
                        border.color: laserPage.borderColor

                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("标定失败")
                                color: laserPage.secondaryTextColor
                                font.pixelSize: 11
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "0"
                                color: "#dc2626"
                                font.pixelSize: 18
                                font.bold: true
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 140
                        Layout.preferredHeight: 60
                        radius: 8
                        color: "#f0f4f8"
                        border.color: laserPage.borderColor

                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("进行中")
                                color: laserPage.secondaryTextColor
                                font.pixelSize: 11
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "0"
                                color: "#f97316"
                                font.pixelSize: 18
                                font.bold: true
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }
                }

                // 数据表
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 8
                    color: "#f8fafc"
                    border.color: laserPage.borderColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            Label { 
                                Layout.preferredWidth: 50
                                text: qsTr("托盘")
                                color: laserPage.secondaryTextColor
                                font.bold: true
                                font.pixelSize: 11
                            }
                            Label { 
                                Layout.preferredWidth: 50
                                text: qsTr("槽位")
                                color: laserPage.secondaryTextColor
                                font.bold: true
                                font.pixelSize: 11
                            }
                            Label { 
                                Layout.fillWidth: true
                                text: qsTr("激光距离 (mm)")
                                color: laserPage.secondaryTextColor
                                font.bold: true
                                font.pixelSize: 11
                            }
                            Label { 
                                Layout.fillWidth: true
                                text: qsTr("激光高度 (mm)")
                                color: laserPage.secondaryTextColor
                                font.bold: true
                                font.pixelSize: 11
                            }
                            Label { 
                                Layout.preferredWidth: 60
                                text: qsTr("结果")
                                color: laserPage.secondaryTextColor
                                font.bold: true
                                font.pixelSize: 11
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: laserPage.borderColor
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            text: qsTr("暂无标定数据")
                            color: "#98a2b3"
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }
    }
}
