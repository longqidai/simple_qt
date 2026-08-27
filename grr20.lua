-- GRR20 SLAM calibration program
-- Host protocol:
--   REQ $$MT,CONNECT       -> RES $$MT,CONNECTOK
--   REQ $$MT,LEFT,n        -> scan left tray slot n, RES $$MT,SN,<sn>
--   REQ $$MT,RIGHT,n       -> scan right tray slot n, RES $$MT,SN,<sn>
--   REQ $$MT,START         -> pick, return home and settle,
--                              RES $$MT,STARTOK then $$MT,HOMEOK
--   REQ $$MT,SCANREADY     -> run the SLAM trajectory,
--                              RES $$MT,SCANREADYOK then $$MT,RUNFINISH
--   REQ $$MT,PUTBACK       -> return the current device,
--                              RES $$MT,PUTBACKOK
--   REQ $$MT,OVER          -> finish the batch, RES $$MT,OVER
-- All responses use $* as the frame terminator.

local TCP_BIND_IP = "192.168.5.1"
local TCP_PORT = 6601
local TRAY_ROWS = 2
local TRAY_COLUMNS = 4
local SLOT_COUNT = TRAY_ROWS * TRAY_COLUMNS
-- 临时兼容损坏的 SLAM 二维码：仍执行一次拍码，但统一上报此 SN。
-- 二维码恢复后将此值改为空字符串即可恢复真实扫码结果。
local SCAN_RETRY_COUNT = 5
local APPROACH_HEIGHT = 150
local SCAN_CLEARANCE_HEIGHT = 150
local STARTUP_RELEASE_DROP = 230
local HOME_SETTLE_MS = 2000

local STATE_IDLE = "IDLE"
local STATE_SCANNING = "SCANNING"
local STATE_WAIT_START = "WAIT_START"
local STATE_WAIT_SCAN_READY = "WAIT_SCAN_READY"
local STATE_CALIBRATING = "CALIBRATING"
local STATE_WAIT_PUTBACK = "WAIT_PUTBACK"
local STATE_PUTTING_BACK = "PUTTING_BACK"

local state = STATE_IDLE
local currentSide = ""
local currentSlot = 0
local deviceHeld = false
local socket1 = nil
local gripperId = nil
local gripperReady = false

local function trace(message)
  -- 统一给机械臂控制台和控制器日志写入带 [SLAM] 前缀的运行信息。
  local text = "[SLAM] " .. tostring(message)
  print(text)
  Log(text)
end

local function waitMotion(fallbackMs)
  -- 优先等待当前运动真正结束；旧控制器没有 Sync 时才按给定时间兜底等待。
  if type(Sync) == "function" then
    Sync()
  else
    Wait(fallbackMs)
  end
end

local function sendFrame(frame)
  -- 第一步：确认上位机 TCP 连接仍然存在。
  if socket1 == nil then
    return false
  end
  -- 第二步：在协议正文后添加 $* 分帧符并发送给上位机。
  local result = TCPWrite(socket1, frame .. "$*")
  -- 第三步：记录发送内容，返回本次 TCPWrite 是否成功。
  trace("TX " .. frame)
  return result == 0
end

local function initializeGripper()
  -- 第一步：以站号 9、115200、偶校验创建机械爪 Modbus RTU 通道。
  local resultCreate
  resultCreate, gripperId = ModbusRTUCreate(9, 115200, "E", 8, 1)
  if resultCreate ~= 0 then
    trace("Modbus gripper initialization failed, code=" .. tostring(resultCreate))
    return false
  end

  -- 第二步：复位机械爪控制器，等待复位完成。
  SetHoldRegs(gripperId, 1000, 1, {0}, "U16")
  Wait(1000)
  -- 第三步：使能机械爪控制器，等待使能完成。
  SetHoldRegs(gripperId, 1000, 1, {1}, "U16")
  Wait(1000)
  -- 第四步：设置张开位置、最大允许力并执行动作，使爪子完全松开。
  -- 调用本函数前工具已下降到安全释放高度，因此即使夹有设备也不会高处掉落。
  SetHoldRegs(gripperId, 1001, 1, {0}, "U16")
  SetHoldRegs(gripperId, 1002, 2, {65535}, "U32")
  -- 参数写入后必须给夹爪控制器留出锁存时间。旧版已验证程序也是
  -- 先等待再写执行寄存器；连续写入在带负载时可能仍按旧目标动作。
  Wait(2000)
  SetHoldRegs(gripperId, 1000, 1, {9}, "U16")
  Wait(3000)
  -- 第五步：机械爪初始化完成，允许后续执行夹取动作。
  trace("Gripper ready")
  return true
end

local function closeGripper()
  -- 第一步：把目标位置设为闭合位置 65280。
  SetHoldRegs(gripperId, 1001, 1, {65280}, "U16")
  -- 第二步：设置最大允许夹持力。
  SetHoldRegs(gripperId, 1002, 2, {65535}, "U32")
  -- 第三步：等待位置和力参数被控制器锁存。不能与执行命令连续写入。
  Wait(2000)
  -- 第四步：发送执行命令并等待机械爪闭合稳定。
  SetHoldRegs(gripperId, 1000, 1, {9}, "U16")
  Wait(3000)
end

local function openGripper()
  -- 第一步：把目标位置设为完全张开位置 0。
  SetHoldRegs(gripperId, 1001, 1, {0}, "U16")
  -- 第二步：设置动作允许力。
  SetHoldRegs(gripperId, 1002, 2, {65535}, "U32")
  -- 第三步：等待位置和力参数被控制器锁存。旧版程序在此等待 2 秒；
  -- 若直接执行，夹爪带着设备时可能只松开一部分。
  Wait(2000)
  -- 第四步：发送执行命令并等待机械爪完全张开稳定。
  SetHoldRegs(gripperId, 1000, 1, {9}, "U16")
  Wait(3000)
end

local function initializeGripperSafely()
  -- 第一步：从 P41/Home 垂直下降 230 mm；若上次异常退出时还夹着设备，先把设备降到安全高度。
  trace("Startup safety release: lower gripper 230 mm before initialization")
  VelL(5)
  RelMovLUser({0, 0, -STARTUP_RELEASE_DROP, 0, 0, 0})
  waitMotion(60000)

  -- 第二步：在低位初始化并张开机械爪，避免设备从 Home 高度直接跌落。
  local ready = initializeGripper()
  if ready then
    -- 第三步：初始化成功后，软件状态同步为“当前没有夹持设备”。
    deviceHeld = false
    trace("Startup safety release: gripper opened at lowered height")
  else
    trace("Startup safety release: gripper initialization failed; no open command issued")
  end

  -- 第四步：无论机械爪初始化是否成功，都按原路径垂直恢复 230 mm 高度。
  VelL(20)
  RelMovLUser({0, 0, STARTUP_RELEASE_DROP, 0, 0, 0})
  waitMotion(30000)
  -- 第五步：返回机械爪是否可用，由主流程决定是否允许夹取。
  trace("Startup safety release: gripper height restored")
  return ready
end

local function moveHome()
  -- 第一步：设置回 Home 使用的关节/直线速度和加速度。
  VelJ(30)
  VelL(20)
  AccJ(50)
  AccL(50)

  -- 第二步：读取当前位姿；若 Z 轴低于 500 mm，先垂直抬到 530 mm 的安全过渡高度。
  local currentPose = GetPose()
  if currentPose ~= nil and currentPose.pose ~= nil and currentPose.pose[3] < 500 then
    P50.pose[1] = currentPose.pose[1]
    P50.pose[2] = currentPose.pose[2]
    P50.pose[3] = 530
    MovJ(P50)
    waitMotion(5000)
  end
  -- 第三步：到达安全高度后再关节运动到 P41/Home，避免低位横扫托盘或设备。
  MovJ(P41)
  waitMotion(5000)
end

local function createSlamTrays()
  -- 第一步：用 P27～P30 四个角点建立左托盘 2×4 扫码网格。
  CreateTray("slam_left_scan", {TRAY_ROWS, TRAY_COLUMNS}, {P27, P28, P29, P30})
  -- 第二步：用 P21～P24 四个角点建立左托盘 2×4 夹取网格。
  CreateTray("slam_left_pick", {TRAY_ROWS, TRAY_COLUMNS}, {P21, P22, P23, P24})
  -- 第三步：用 P31～P34 四个角点建立右托盘 2×4 扫码网格。
  CreateTray("slam_right_scan", {TRAY_ROWS, TRAY_COLUMNS}, {P31, P32, P33, P34})
  -- 第四步：用 P35～P38 四个角点建立右托盘 2×4 夹取网格。
  CreateTray("slam_right_pick", {TRAY_ROWS, TRAY_COLUMNS}, {P35, P36, P37, P38})
  -- 四个网格创建完成后，槽位 1～8 可通过 GetTrayPoint 自动插值得到实际点位。
  trace("SLAM trays created: left/right 2x4")
end

local function scanTrayName(side)
  if side == "LEFT" then
    return "slam_left_scan"
  end
  return "slam_right_scan"
end

local function pickTrayName(side)
  if side == "LEFT" then
    return "slam_left_pick"
  end
  return "slam_right_pick"
end

local function cleanSn(value)
  -- 第一步：把扫码结果转换为字符串；空结果按空字符串处理。
  local sn = tostring(value or "")
  -- 第二步：去除换行、空格等控制字符。
  sn = string.gsub(sn, "[%c%s]", "")
  -- 第三步：去除本协议可能附带的 $* 分帧符，只保留实际 SN。
  sn = string.gsub(sn, "%$%*", "")
  return sn
end

local function scanCurrentDevice()
  -- 第一步：进入扫码状态，并根据左右托盘和槽位号计算当前扫码点。
  state = STATE_SCANNING
  local scanPoint = GetTrayPoint(scanTrayName(currentSide), currentSlot)
  -- 第二步：移动到二维码扫码点，等待机械臂完全停止后再让视觉模块读码。
  trace("Move to scan point " .. currentSide .. ", slot=" .. tostring(currentSlot))
  MovJ(scanPoint)
  waitMotion(5000)
  Wait(500)

  -- 第三步：最多尝试 SCAN_RETRY_COUNT 次，运行 VX500 的 cs 扫码工程。
  for attempt = 1, SCAN_RETRY_COUNT do
    RunVX500Project("cs")
    -- 第四步：先读取扫码模块运行状态，只有返回 OK 才继续读取二维码内容。
    local resultState = GetVX500ModelResultByJson(
      '{"type":"state","moduleName":"idemodule","moduleId":6}', 1, "cs")
    if resultState == "OK" then
      -- 第五步：读取二维码正文并清理成设备 SN。
      local content = GetVX500ModelResultByJson(
        '{"type":"content","moduleName":"idemodule","moduleId":6}', 1, "cs")
      local sn = cleanSn(content)
      if sn ~= "" then
        -- 第六步：扫码成功后等待 START，并把 SN 发给上位机，由上位机连接对应设备 Wi-Fi。
        state = STATE_WAIT_START
        trace("Scan succeeded, SN=" .. sn)
        sendFrame("$$MT,SN," .. sn)
        return true
      end
    end
    trace("Scan attempt " .. tostring(attempt) .. " failed")
    Wait(300)
  end

  -- 第七步：全部重试仍失败，恢复空闲并明确上报失败的托盘和槽位。
  state = STATE_IDLE
  sendFrame("$$MT,FAILED,SCAN," .. currentSide .. "," .. tostring(currentSlot))
  return false
end

local function pickCurrentDevice()
  -- 第一步：计算扫码点、实际夹取点、扫码安全抬升点和夹取点上方的接近点。
  local scanPoint = GetTrayPoint(scanTrayName(currentSide), currentSlot)
  local pickPoint = GetTrayPoint(pickTrayName(currentSide), currentSlot)
  local scanClearancePoint = RelPointUser(scanPoint, {0, 0, SCAN_CLEARANCE_HEIGHT, 0, 0, 0})
  local approachPoint = RelPointUser(pickPoint, {0, 0, APPROACH_HEIGHT, 0, 0, 0})

  trace("Pick device " .. currentSide .. ", slot=" .. tostring(currentSlot))
  -- 第二步：扫码位置靠近托盘，必须先垂直抬高 150 mm，禁止直接横向扫过设备。
  trace("Lift clear of scanned device before moving to pick point")
  VelL(20)
  MovL(scanClearancePoint)
  waitMotion(10000)
  -- 第三步：在安全高度关节运动到夹取点正上方。
  trace("Move above pick point, then descend vertically")
  MovJ(approachPoint)
  waitMotion(5000)
  -- 第四步：降低速度，垂直下降到实际夹取点。
  VelL(5)
  MovL(pickPoint)
  waitMotion(5000)
  -- 第五步：闭合机械爪，并记录当前确实夹着设备。
  closeGripper()
  deviceHeld = true
  -- 第六步：夹紧后垂直抬回接近点，先脱离托盘再进行关节运动。
  VelL(20)
  MovL(approachPoint)
  waitMotion(5000)
  -- 第七步：夹着设备回到 P41/Home，后续在这里静置并启动真实扫描。
  MovJ(P41)
  waitMotion(5000)
end

local function runSlamTrajectory()
  -- 第一步：设置 SLAM 标定轨迹使用的速度和加速度。
  trace("Run SLAM calibration trajectory")
  VelJ(30)
  VelL(30)
  AccJ(50)
  AccL(50)

  -- 第二步：从 Home 依次到达各个已示教标定姿态；每一步都等待运动完全结束。
  -- P42：标定面 2。
  MovJ(P42)
  waitMotion(6000)
  -- P47：标定面 3。
  MovJ(P47)
  waitMotion(6000)
  -- P43：标定面 4。
  MovJ(P43)
  waitMotion(6000)
  -- P44：标定面 5。
  MovJ(P44)
  waitMotion(6000)
  -- P45：标定面 6。
  MovJ(P45)
  waitMotion(6000)
  -- P46：标定面 7。
  MovJ(P46)
  waitMotion(6000)
  -- 第三步：全部姿态完成后返回 P41/Home，等待上位机停止设备测量并下发放回指令。
  MovJ(P41)
  waitMotion(6000)
end

local function putBackCurrentDevice()
  -- 第一步：若软件判断没有夹持设备，直接恢复空闲并确认放回完成，避免重复运动。
  if not deviceHeld then
    state = STATE_IDLE
    sendFrame("$$MT,PUTBACKOK")
    return true
  end

  -- 第二步：进入放回状态，重新计算当前原槽位夹取点及其上方 150 mm 接近点。
  state = STATE_PUTTING_BACK
  local pickPoint = GetTrayPoint(pickTrayName(currentSide), currentSlot)
  local approachPoint = RelPointUser(pickPoint, {0, 0, APPROACH_HEIGHT, 0, 0, 0})
  trace("Put back device " .. currentSide .. ", slot=" .. tostring(currentSlot))

  -- 第三步：先移动到原槽位正上方，避免携带设备低位横移。
  MovJ(approachPoint)
  waitMotion(5000)
  -- 第四步：降低速度，垂直下降到槽位放置高度。
  VelL(5)
  MovL(pickPoint)
  waitMotion(5000)
  -- 第五步：松开机械爪并更新夹持状态，设备此时已经放回原槽位。
  openGripper()
  deviceHeld = false
  -- 第六步：垂直抬回安全接近点，再返回 P41/Home。
  VelL(20)
  MovL(approachPoint)
  waitMotion(5000)
  MovJ(P41)
  waitMotion(5000)

  -- 第七步：恢复空闲并向上位机发送 PUTBACKOK，允许其继续下一个槽位。
  state = STATE_IDLE
  sendFrame("$$MT,PUTBACKOK")
  return true
end

local function handleSlotCommand(side, slotText)
  -- 第一步：把协议中的槽位文本转换为数字，并限制在 1～8。
  local slot = tonumber(slotText)
  if slot == nil or slot < 1 or slot > SLOT_COUNT then
    sendFrame("$$MT,FAILED,SLOT," .. tostring(slotText))
    return
  end
  -- 第二步：若机械臂仍夹着上一台设备，拒绝处理新槽位，防止设备未放回就继续运行。
  if deviceHeld then
    sendFrame("$$MT,FAILED,BUSY")
    return
  end

  -- 第三步：保存当前左右托盘和槽位，然后立即执行对应槽位扫码。
  currentSide = side
  currentSlot = slot
  scanCurrentDevice()
end

local function handleCommand(command)
  -- 第一步：去除 TCP 命令首尾空白；空命令直接忽略。
  command = string.gsub(command or "", "^%s+", "")
  command = string.gsub(command, "%s+$", "")
  if command == "" then
    return
  end
  trace("RX " .. command)

  -- CONNECT：完成上位机和机械臂的应用层握手。
  if string.find(command, "$$MT,CONNECT", 1, true) ~= nil then
    sendFrame("$$MT,CONNECTOK")
    return
  end

  -- LEFT/RIGHT,n：选择托盘槽位并启动二维码扫描。
  local side, slotText = string.match(command, "%$%$MT,([A-Z]+),(%d+)")
  if side == "LEFT" or side == "RIGHT" then
    handleSlotCommand(side, slotText)
    return
  end

  -- START：必须在扫码成功后执行；确认机械爪可用，再夹取设备并返回 Home。
  if string.find(command, "$$MT,START", 1, true) ~= nil then
    if state ~= STATE_WAIT_START then
      sendFrame("$$MT,FAILED,STATE,START")
      return
    end
    if not gripperReady then
      state = STATE_IDLE
      sendFrame("$$MT,FAILED,GRIPPER")
      return
    end
    state = STATE_CALIBRATING
    -- 先回复 STARTOK，让上位机可以并行连接设备 Wi-Fi，不必等机械臂夹取完成。
    sendFrame("$$MT,STARTOK")
    pickCurrentDevice()
    -- 回到 Home 后固定静置 2 秒，消除刚性运动造成的机械振动。
    trace("Device held at home; wait for stationary scan initialization")
    Wait(HOME_SETTLE_MS)
    -- HOMEOK 表示机械臂和原始采集准备完成；机械臂继续停在 Home 等待设备真正进入扫描状态。
    state = STATE_WAIT_SCAN_READY
    sendFrame("$$MT,HOMEOK")
    return
  end

  -- SCANREADY：只有上位机确认设备状态为“正在扫描”后才会发送，用于释放机械臂标定轨迹。
  if string.find(command, "$$MT,SCANREADY", 1, true) ~= nil then
    if state ~= STATE_WAIT_SCAN_READY or not deviceHeld then
      sendFrame("$$MT,FAILED,STATE,SCANREADY")
      return
    end
    state = STATE_CALIBRATING
    sendFrame("$$MT,SCANREADYOK")
    -- 执行完整 SLAM 姿态序列，完成后等待上位机停止设备测量。
    runSlamTrajectory()
    state = STATE_WAIT_PUTBACK
    sendFrame("$$MT,RUNFINISH")
    return
  end

  -- PUTBACK：上位机确认设备扫描已经停止后，要求机械臂放回当前设备。
  if string.find(command, "$$MT,PUTBACK", 1, true) ~= nil then
    putBackCurrentDevice()
    return
  end

  -- FAILED：上位机报告流程异常时，若仍夹着设备则优先安全放回，否则确认异常已处理。
  if string.find(command, "$$MT,FAILED", 1, true) ~= nil then
    if deviceHeld then
      putBackCurrentDevice()
    else
      state = STATE_IDLE
      sendFrame("$$MT,FAILEDOK")
    end
    return
  end

  -- OVER：整批结束；仍夹着设备时先放回，否则直接回 Home，最后清空当前槽位状态。
  if string.find(command, "$$MT,OVER", 1, true) ~= nil then
    if deviceHeld then
      putBackCurrentDevice()
    else
      moveHome()
    end
    state = STATE_IDLE
    currentSide = ""
    currentSlot = 0
    sendFrame("$$MT,OVER")
    return
  end

  trace("Ignore unknown command: " .. command)
end

local function processTcpPayload(payload)
  -- 第一步：把一次 TCPRead 收到的内容转换成字符串，并将 $* 分帧符转换为换行。
  local normalized = tostring(payload or "")
  normalized = string.gsub(normalized, "%$%*", "\n")
  -- 第二步：TCP 可能一次收到多条命令，逐条拆分后交给状态机处理。
  for command in string.gmatch(normalized, "[^\r\n]+") do
    handleCommand(command)
  end
end

local function startTcpServer()
  -- 第一步：销毁残留的旧 Socket，确保不会同时存在两个监听实例。
  if socket1 ~= nil then
    TCPDestroy(socket1)
    socket1 = nil
  end

  -- 第二步：在 192.168.5.1:6601 创建机械臂 TCP Server。
  local createResult
  createResult, socket1 = TCPCreate(true, TCP_BIND_IP, TCP_PORT)
  if createResult ~= 0 then
    trace("Create TCP server failed, code=" .. tostring(createResult))
    socket1 = nil
    return false
  end

  -- 第三步：启动监听；失败时释放 Socket，下一轮主循环会自动重试。
  local startResult = TCPStart(socket1, 0)
  if startResult ~= 0 then
    trace("Start TCP server failed, code=" .. tostring(startResult))
    TCPDestroy(socket1)
    socket1 = nil
    return false
  end
  trace("TCP server ready at " .. TCP_BIND_IP .. ":" .. tostring(TCP_PORT))
  return true
end

-- 程序启动步骤 1：设置基础速度和加速度。
VelJ(30)
VelL(20)
AccJ(50)
AccL(50)
-- 程序启动步骤 2：先安全回到 P41/Home。
moveHome()
-- 程序启动步骤 3：下降到安全高度后松爪，再恢复高度，处理上次异常残留设备。
gripperReady = initializeGripperSafely()
-- 程序启动步骤 4：创建左右托盘的扫码与夹取插值网格。
createSlamTrays()

-- 程序主循环：维持 TCP Server，读取上位机指令；断线后销毁旧连接并自动重新监听。
while true do
  if socket1 == nil then
    if not startTcpServer() then
      Wait(1000)
    end
  else
    local readResult, payload = TCPRead(socket1, 0, "string")
    if readResult == 0 then
      processTcpPayload(payload)
    else
      trace("TCP connection lost, code=" .. tostring(readResult))
      TCPDestroy(socket1)
      socket1 = nil
      Wait(500)
    end
  end
end
