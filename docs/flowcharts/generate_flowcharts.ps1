param([string]$OutputDirectory = $PSScriptRoot)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Escape-Xml([string]$Text) {
    return [System.Security.SecurityElement]::Escape($Text)
}

function Split-Label([string]$Label) {
    return @($Label -split '\|', 2)
}

function New-SequenceSvg {
    param(
        [string]$Title,
        [string[]]$Participants,
        [object[]]$Events,
        [string]$OutputPath
    )

    $margin = 110
    $laneGap = 440
    $width = $margin * 2 + $laneGap * ($Participants.Count - 1)
    $top = 165
    $rowHeight = 78
    $stageHeight = 58
    $height = $top + 120
    foreach ($event in $Events) {
        $height += if ($event.kind -eq 'stage') { $stageHeight } else { $rowHeight }
    }

    $xs = @()
    for ($i = 0; $i -lt $Participants.Count; ++$i) {
        $xs += $margin + $laneGap * $i
    }

    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine('<?xml version="1.0" encoding="UTF-8"?>')
    [void]$sb.AppendLine("<svg xmlns=`"http://www.w3.org/2000/svg`" width=`"$width`" height=`"$height`" viewBox=`"0 0 $width $height`">")
    [void]$sb.AppendLine(@'
  <defs>
    <marker id="arrow" markerWidth="12" markerHeight="9" refX="10" refY="4.5" orient="auto" markerUnits="strokeWidth">
      <path d="M0,0 L10,4.5 L0,9 z" fill="#334155"/>
    </marker>
    <style>
      .title{font:700 34px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#172033}
      .participant{font:600 20px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#26324a}
      .label{font:500 17px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#172033}
      .sub{font:400 14px Consolas,"Microsoft YaHei",sans-serif;fill:#475569}
      .stage{font:700 19px "Microsoft YaHei","Noto Sans CJK SC",sans-serif;fill:#1d4ed8}
      .step{font:700 14px Consolas,sans-serif;fill:#2563eb}
      .life{stroke:#c4b5fd;stroke-width:2;stroke-dasharray:7 7}
      .arrow{stroke:#334155;stroke-width:2;fill:none;marker-end:url(#arrow)}
      .return{stroke:#64748b;stroke-width:2;stroke-dasharray:8 6;fill:none;marker-end:url(#arrow)}
    </style>
  </defs>
  <rect width="100%" height="100%" fill="#ffffff"/>
'@)
    [void]$sb.AppendLine("  <text class=`"title`" x=`"$($width / 2)`" y=`"55`" text-anchor=`"middle`">$(Escape-Xml $Title)</text>")

    for ($i = 0; $i -lt $Participants.Count; ++$i) {
        $x = $xs[$i]
        [void]$sb.AppendLine("  <rect x=`"$($x - 135)`" y=`"85`" width=`"270`" height=`"58`" rx=`"9`" fill=`"#f0edff`" stroke=`"#c4b5fd`"/>")
        [void]$sb.AppendLine("  <text class=`"participant`" x=`"$x`" y=`"121`" text-anchor=`"middle`">$(Escape-Xml $Participants[$i])</text>")
        [void]$sb.AppendLine("  <line class=`"life`" x1=`"$x`" y1=`"143`" x2=`"$x`" y2=`"$($height - 80)`"/>")
    }

    $y = $top
    $step = 0
    foreach ($event in $Events) {
        if ($event.kind -eq 'stage') {
            [void]$sb.AppendLine("  <rect x=`"35`" y=`"$($y - 28)`" width=`"$($width - 70)`" height=`"44`" rx=`"8`" fill=`"#eff6ff`" stroke=`"#93c5fd`"/>")
            [void]$sb.AppendLine("  <text class=`"stage`" x=`"55`" y=`"$y`">$(Escape-Xml $event.label)</text>")
            $y += $stageHeight
            continue
        }

        ++$step
        $parts = @($event.label -split '\|', 2)
        $main = Escape-Xml $parts[0]
        $sub = if ($parts.Count -gt 1) { Escape-Xml $parts[1] } else { '' }
        [void]$sb.AppendLine("  <text class=`"step`" x=`"36`" y=`"$($y + 5)`">$('{0:D2}' -f $step)</text>")

        if ($event.kind -in @('self','wait')) {
            $x = $xs[[int]$event.at]
            $boxX = [Math]::Max(42, [Math]::Min($x - 190, $width - 422))
            $textX = $boxX + 190
            $fill = if ($event.kind -eq 'wait') { '#fff7d6' } else { '#f8fafc' }
            $stroke = if ($event.kind -eq 'wait') { '#f59e0b' } else { '#94a3b8' }
            [void]$sb.AppendLine("  <rect x=`"$boxX`" y=`"$($y - 28)`" width=`"380`" height=`"58`" rx=`"10`" fill=`"$fill`" stroke=`"$stroke`" stroke-width=`"1.5`"/>")
            [void]$sb.AppendLine("  <text class=`"label`" x=`"$textX`" y=`"$($y - 3)`" text-anchor=`"middle`">$main</text>")
            if ($sub) { [void]$sb.AppendLine("  <text class=`"sub`" x=`"$textX`" y=`"$($y + 20)`" text-anchor=`"middle`">$sub</text>") }
        } else {
            $x1 = $xs[[int]$event.from]
            $x2 = $xs[[int]$event.to]
            $class = if ($event.kind -eq 'return') { 'return' } else { 'arrow' }
            [void]$sb.AppendLine("  <line class=`"$class`" x1=`"$x1`" y1=`"$y`" x2=`"$x2`" y2=`"$y`"/>")
            $mid = ($x1 + $x2) / 2
            [void]$sb.AppendLine("  <rect x=`"$($mid - 198)`" y=`"$($y - 34)`" width=`"396`" height=`"31`" rx=`"5`" fill=`"#ffffff`" opacity=`"0.96`"/>")
            [void]$sb.AppendLine("  <text class=`"label`" x=`"$mid`" y=`"$($y - 12)`" text-anchor=`"middle`">$main</text>")
            if ($sub) { [void]$sb.AppendLine("  <text class=`"sub`" x=`"$mid`" y=`"$($y + 22)`" text-anchor=`"middle`">$sub</text>") }
        }
        $y += $rowHeight
    }

    for ($i = 0; $i -lt $Participants.Count; ++$i) {
        $x = $xs[$i]
        [void]$sb.AppendLine("  <rect x=`"$($x - 135)`" y=`"$($height - 70)`" width=`"270`" height=`"48`" rx=`"9`" fill=`"#f0edff`" stroke=`"#c4b5fd`"/>")
        [void]$sb.AppendLine("  <text class=`"participant`" x=`"$x`" y=`"$($height - 39)`" text-anchor=`"middle`">$(Escape-Xml $Participants[$i])</text>")
    }
    [void]$sb.AppendLine('</svg>')
    [System.IO.File]::WriteAllText($OutputPath, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))
}

function E([string]$kind, [string]$label, [int]$from = -1, [int]$to = -1, [int]$at = -1) {
    return [pscustomobject]@{ kind=$kind; label=$label; from=$from; to=$to; at=$at }
}

$slamEvents = @(
    (E stage 'A. 程序启动、机械臂连接与任务建立'),
    (E arrow '点击“连接机械臂”|Main.qml → toggleConnection()' 0 1),
    (E arrow '建立机械臂 TCP|connectToServer() / QTcpSocket::connectToHost()' 1 2),
    (E return 'TCP connected 信号' 2 1),
    (E arrow '发送握手 $$MT,CONNECT' 1 2),
    (E return '返回 $$MT,CONNECTOK|handleArmCommand() 设置 armReady' 2 1),
    (E arrow '选择槽位并点击开始|toggleArmTask() → beginTask()' 0 1),
    (E self '建立槽位任务队列|beginTask() → startNextSlot()' -1 -1 1),

    (E stage 'B. 单个槽位循环：扫码、网络与设备准备'),
    (E arrow '下发槽位 $$MT,LEFT/RIGHT,slot|startNextSlot()' 1 2),
    (E arrow '机械臂移动到槽位并扫描 SN' 2 2),
    (E return '返回 MT,SN,...|processArmBuffer() → handleScannedSn()' 2 1),
    (E self '按 SN 搜索并切换设备 Wi-Fi|connectWifiByNamePart() / checkWifiConnection()' -1 -1 1),
    (E arrow '连接设备信息 TCP' 1 3),
    (E arrow '请求设备信息|unlogall thisport + $$SI + log versionb' 1 3),
    (E return '返回硬件号、SN、版本|processDeviceBuffer()' 3 1),
    (E arrow '连接 MQTT 并订阅状态/响应主题|connectMqtt() → subscribeMqttTopics()' 1 3),
    (E return '周期上报 init_ready=1|handleDeviceStatusReport()' 3 1),

    (E stage 'C. 创建扫描工程、任务与预初始化'),
    (E arrow '创建扫描工程|publishProjectCreate(): app_slam/request/project' 1 3),
    (E return '返回工程 ID|slam_app/response/project' 3 1),
    (E arrow '创建扫描任务|publishTaskCreate(): app_slam/request/task' 1 3),
    (E return '返回任务 ID|slam_app/response/task' 3 1),
    (E arrow '扫描预初始化 AC_PREINIT|publishModelingPreinit()' 1 3),
    (E return '预初始化成功|handleModelingResponse()' 3 1),
    (E stage 'D. 机械臂到 P100、真实扫描与同步标定轨迹'),
    (E arrow '允许抓取 $$MT,START|仅在 Wi-Fi/MQTT/采集目录检查通过后发送' 1 2),
    (E arrow '抓取设备、经 Home 到达 P100' 2 2),
    (E return '返回 STARTOK、HOMEOK' 2 1),
    (E arrow '启动真实扫描 AC_START|publishModelingAction(true)' 1 3),
    (E return '周期上报 status 1→2→3→4|等待设备自检、静止初始化、传感器与SLAM初始化' 3 1),
    (E return '周期上报 status=5（真实扫描中）' 3 1),
    (E arrow '开启原始数据采集 AC_START|publishCollectAction()' 1 3),
    (E return 'collecting + 采集目录 path|handleMqttMessage() 保存 m_collectPath' 3 1),
    (E arrow '释放标定轨迹 $$MT,SCANREADY' 1 2),
    (E arrow '执行 SLAM 标定轨迹' 2 2),
    (E return '返回 SCANREADYOK；P7100到位返回 TRAJECTORYFINISH' 2 1),
    (E arrow '停止原始采集 AC_STOP|publishCollectAction()' 1 3),
    (E return 'collect stopped + 最终采集目录' 3 1),
    (E arrow '停止真实扫描 AC_STOP|publishModelingAction(false)' 1 3),
    (E return 'status=6 保存中 → status=7 保存完成' 3 1),
    (E return '机械臂安全回程完成 RUNFINISH' 2 1),

    (E stage 'E. 当前实现：加入下载队列、放回、下一槽位'),
    (E self '保存 DeferredDownload|queueCurrentDownload()，槽位=waiting_download' -1 -1 1),
    (E arrow '放回设备 $$MT,PUTBACK' 1 2),
    (E arrow '回 Home 并安全放回当前槽位' 2 2),
    (E return '返回 PUTBACKOK / MT,OK' 2 1),
    (E self '尚有槽位：startNextSlot() 重复 B～E' -1 -1 1),

    (E stage 'F. 全部设备放回后统一 FTP 下载'),
    (E arrow '全部机械臂任务结束 $$MT,OVER|beginBatchDownloads()' 1 2),
    (E self '逐条取 DeferredDownload 并重连对应 Wi-Fi|startNextDeferredDownload()' -1 -1 1),
    (E arrow '提前连接 MQTT 并订阅 data_collect 响应|与 FTP 下载并行建立，避免下载后再次等待' 1 3),
    (E arrow 'curl --list-only 读取 FTP 目录' 1 3),
    (E return '返回文件列表' 3 1),
    (E arrow 'curl 逐文件下载，支持断点续传|startNextFtpFile()' 1 3),
    (E return 'QProcess::finished → 下一文件' 3 1),
    (E arrow '下载完成发送 data_collect AC_FINISH|requestCollectFinish()' 1 3),
    (E return 'collect finished，设备数据和 FTP 目录已删除' 3 1),
    (E self '记录成功并处理下一设备|finishFtpDownload() → startNextDeferredDownload()' -1 -1 1),
    (E self '全部设备下载与清理完成|finishTask()' -1 -1 1),

    (E stage 'G. 异常或人工中止的安全收尾'),
    (E self '进入兜底停止状态|beginGuaranteedMeasurementStop()' -1 -1 1),
    (E arrow '反复发送 modeling / data_collect AC_STOP|确认真实扫描和原始采集均停止' 1 3),
    (E arrow '尽力发送 AC_FINISH 清理残留目录|失败会记录告警，但不会永久卡住批次' 1 3),
    (E self '按机械臂状态安全放回或要求人工恢复' -1 -1 1)
)

$laserEvents = @(
    (E stage 'A. 程序初始化、机械臂握手与任务开始'),
    (E self '构造控制器并注册信号槽|configureSockets() / configureTimers() / measurementFinished' -1 -1 1),
    (E arrow '点击连接机械臂|connectArm()' 0 1),
    (E arrow '建立机械臂 TCP' 1 2),
    (E return 'QTcpSocket::connected' 2 1),
    (E arrow '发送握手 $$MT,CONNECT' 1 2),
    (E return '返回 CONNECTOK，机械臂就绪' 2 1),
    (E arrow '选择设备型号、槽位并点击开始|GRR13/17/20 或 SLAM；SLAM 仅1～8号槽' 0 1),
    (E self '选择匹配机械臂工程|SLAM 使用 SLAM_GRR；GRR 系列使用对应 GRR 工程' -1 -1 1),
    (E self '校验水平线高、建立槽位队列|beginNextSlot() → beginCurrentSlot()' -1 -1 1),

    (E stage 'B. 单个槽位循环：扫码、Wi-Fi 与接收机信息'),
    (E arrow '设置托盘与槽位|SETMOD1/SETMOD2 + 槽位号 + $$MT,NEXT' 1 2),
    (E arrow '机械臂移动并扫描 SN' 2 2),
    (E return '返回 SN|processArmBuffer() → handleScannedSn()' 2 1),
    (E self '按 SN 连接设备 Wi-Fi，等待 DHCP|connectWifiByNamePart() / m_wifiTimer' -1 -1 1),
    (E arrow '连接接收机 TCP 192.168.1.8:25001|connectReceiver()' 1 3),
    (E arrow '查询硬件号与版本|unlogall + $$SI + log versionb' 1 3),
    (E return '返回 $$SI 与144字节 versionb|processReceiverBuffer()' 3 1),
    (E arrow '订阅 LASERBACK 并复位标定|log laserback ontime 1 + set laser reset calibration' 1 3),

    (E stage 'C. 面1、面2与垂直远点视觉测量'),
    (E arrow '移动标定点1 $$MT,NEXT1' 1 2),
    (E return '到位返回 MT,OK' 2 1),
    (E arrow '面1校准 set laser planar calibration' 1 3),
    (E return '等待并返回 $LASERBACK：面1 restState=2' 3 1),
    (E arrow '移动标定点2 $$MT,NEXT2' 1 2),
    (E return '到位返回 MT,OK' 2 1),
    (E arrow '面2校准 set laser planar calibration' 1 3),
    (E return '等待并返回 $LASERBACK：面2 restState=2' 3 1),
    (E self '各型号固定近点高度（水平 / 垂直）|GRR13：721 / 507 mm；GRR17：770 / 490 mm；GRR20：615 / 511 mm；SLAM：680 / 436 mm' -1 -1 1),
    (E self '各型号远点测量位置（不是固定高度）|GRR13/17/20及SLAM：垂直远点 NEXT3 / P47；水平远点 NEXT7 / P46；高度与激光长度均现场实测' -1 -1 1),
    (E arrow '移动垂直远点 $$MT,NEXT3' 1 2),
    (E return '到位返回 MT,OK' 2 1),
    (E arrow '开启周期测距与自动测距|log laserrangingb ontime 1 + set laser survey auto' 1 3),
    (E return '持续返回40字节激光测距帧|processLaserRangeFrame() 更新 m_laserDistance' 3 1),
    (E arrow '提交垂直远点视觉任务|startVisionMeasurement() → requestMeasurement(false)' 1 4),
    (E self '后台线程打开远焦摄像头并执行OpenCV|QtConcurrent::run(runMeasurement)' -1 -1 4),
    (E return 'measurementFinished(success,height,image)' 4 1),
    (E self '保存垂直远点高度和测距|handleVisionMeasurement()' -1 -1 1),

    (E stage 'D. 面3～面6与水平远点视觉测量'),
    (E arrow '移动点4，执行面3校准|NEXT4 → MT,OK → planar calibration' 1 2),
    (E arrow '面3校准命令' 1 3),
    (E return '$LASERBACK：面3完成' 3 1),
    (E arrow '移动点5，执行面4校准|NEXT5 → MT,OK' 1 2),
    (E arrow '面4校准命令' 1 3),
    (E return '$LASERBACK：面4完成' 3 1),
    (E arrow '移动点6，执行面5校准|NEXT6 → MT,OK' 1 2),
    (E arrow '面5校准命令' 1 3),
    (E return '$LASERBACK：面5完成' 3 1),
    (E arrow '移动点7，执行面6校准|NEXT7 → MT,OK' 1 2),
    (E arrow '面6校准命令' 1 3),
    (E return '$LASERBACK：面6完成' 3 1),
    (E arrow '重新开启激光、周期测距和自动测距' 1 3),
    (E return '持续更新水平远点激光距离' 3 1),
    (E arrow '提交水平远点视觉任务' 1 4),
    (E self '后台执行远焦摄像头与OpenCV算法' -1 -1 4),
    (E return 'measurementFinished(success,height,image)' 4 1),
    (E self '保存水平远点高度和测距|handleVisionMeasurement()' -1 -1 1),

    (E stage 'E. 六参数角度校准、保存结果与放回'),
    (E self '组成六参数|水平[近点,远点,长度] + 垂直[近点,远点,长度]' -1 -1 1),
    (E arrow '关闭激光和周期测距' 1 3),
    (E arrow '发送角度校准六参数|set laser angle calibration ...' 1 3),
    (E return '最终 $LASERBACK：sideState=1 + pitch/yaw/bias' 3 1),
    (E self '保存最终结果与数据库记录|handleLaserBack() / addResult() / saveResultToDatabase()' -1 -1 1),
    (E arrow '通知成功并安全放回 $$MT,SUCCESS' 1 2),
    (E arrow '机械臂回 Home 并放回设备' 2 2),
    (E return '返回 MT,OK' 2 1),
    (E self 'completeCurrentSlot() → beginNextSlot()|尚有槽位则重复 B～E' -1 -1 1),
    (E self '全部槽位完成|finishBatch()，关闭激光/测距/订阅' -1 -1 1)
)

New-SequenceSvg -Title 'simple_qt：SLAM 标定正常流程（当前版本）' `
    -Participants @('操作界面 / QML','上位机 TcpController','机械臂 GRR20','SLAM 设备') `
    -Events $slamEvents -OutputPath (Join-Path $OutputDirectory 'SLAM标定流程.svg')

New-SequenceSvg -Title 'simple_qt：激光标定正常流程（当前单摄像头版本）' `
    -Participants @('操作界面 / QML','LaserCalibrationController','机械臂','接收机设备','LaserVisionProcessor') `
    -Events $laserEvents -OutputPath (Join-Path $OutputDirectory '激光标定流程.svg')

