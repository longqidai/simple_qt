-- GRR20 SLAM calibration program
-- Host protocol:
--   REQ $$MT,CONNECT       -> RES $$MT,CONNECTOK
--   REQ $$MT,LEFT,n        -> scan left tray slot n, RES $$MT,SN,<sn>
--   REQ $$MT,RIGHT,n       -> scan right tray slot n, RES $$MT,SN,<sn>
--   REQ $$MT,START         -> pick and run the SLAM trajectory,
--                              RES $$MT,STARTOK then $$MT,RUNFINISH
--   REQ $$MT,PUTBACK       -> return the current device,
--                              RES $$MT,PUTBACKOK
--   REQ $$MT,OVER          -> finish the batch, RES $$MT,OVER
-- All responses use $* as the frame terminator.

local TCP_BIND_IP = "192.168.5.1"
local TCP_PORT = 6601
local TRAY_ROWS = 2
local TRAY_COLUMNS = 3
local SLOT_COUNT = TRAY_ROWS * TRAY_COLUMNS
local SCAN_RETRY_COUNT = 5
local APPROACH_HEIGHT = 150

local STATE_IDLE = "IDLE"
local STATE_SCANNING = "SCANNING"
local STATE_WAIT_START = "WAIT_START"
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
  local text = "[SLAM] " .. tostring(message)
  print(text)
  Log(text)
end

local function waitMotion(fallbackMs)
  if type(Sync) == "function" then
    Sync()
  else
    Wait(fallbackMs)
  end
end

local function sendFrame(frame)
  if socket1 == nil then
    return false
  end
  local result = TCPWrite(socket1, frame .. "$*")
  trace("TX " .. frame)
  return result == 0
end

local function initializeGripper()
  local resultCreate
  resultCreate, gripperId = ModbusRTUCreate(9, 115200, "E", 8, 1)
  if resultCreate ~= 0 then
    trace("Modbus gripper initialization failed, code=" .. tostring(resultCreate))
    return false
  end

  -- Reset, enable, then open the gripper. These registers are inherited
  -- from the verified GRR20 program.
  SetHoldRegs(gripperId, 1000, 1, {0}, "U16")
  Wait(1000)
  SetHoldRegs(gripperId, 1000, 1, {1}, "U16")
  Wait(1000)
  SetHoldRegs(gripperId, 1001, 1, {0}, "U16")
  SetHoldRegs(gripperId, 1002, 2, {65535}, "U32")
  SetHoldRegs(gripperId, 1000, 1, {9}, "U16")
  Wait(2000)
  trace("Gripper ready")
  return true
end

local function closeGripper()
  SetHoldRegs(gripperId, 1001, 1, {65280}, "U16")
  SetHoldRegs(gripperId, 1002, 2, {65535}, "U32")
  SetHoldRegs(gripperId, 1000, 1, {9}, "U16")
  Wait(2000)
end

local function openGripper()
  SetHoldRegs(gripperId, 1001, 1, {0}, "U16")
  SetHoldRegs(gripperId, 1002, 2, {65535}, "U32")
  SetHoldRegs(gripperId, 1000, 1, {9}, "U16")
  Wait(2000)
end

local function moveHome()
  VelJ(30)
  VelL(20)
  AccJ(50)
  AccL(50)

  local currentPose = GetPose()
  if currentPose ~= nil and currentPose.pose ~= nil and currentPose.pose[3] < 500 then
    P50.pose[1] = currentPose.pose[1]
    P50.pose[2] = currentPose.pose[2]
    P50.pose[3] = 530
    MovJ(P50)
    waitMotion(5000)
  end
  MovJ(P41)
  waitMotion(5000)
end

local function createSlamTrays()
  -- Existing taught corner points are retained; only the interpolation grid
  -- changes from the old 2x5 laser tray to the SLAM 2x3 tray.
  CreateTray("slam_left_scan", {TRAY_ROWS, TRAY_COLUMNS}, {P27, P28, P29, P30})
  CreateTray("slam_left_pick", {TRAY_ROWS, TRAY_COLUMNS}, {P21, P22, P23, P24})
  CreateTray("slam_right_scan", {TRAY_ROWS, TRAY_COLUMNS}, {P31, P32, P33, P34})
  CreateTray("slam_right_pick", {TRAY_ROWS, TRAY_COLUMNS}, {P35, P36, P37, P38})
  trace("SLAM trays created: left/right 2x3")
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
  local sn = tostring(value or "")
  sn = string.gsub(sn, "[%c%s]", "")
  sn = string.gsub(sn, "%$%*", "")
  return sn
end

local function scanCurrentDevice()
  state = STATE_SCANNING
  local scanPoint = GetTrayPoint(scanTrayName(currentSide), currentSlot)
  trace("Move to scan point " .. currentSide .. ", slot=" .. tostring(currentSlot))
  MovJ(scanPoint)
  waitMotion(5000)
  Wait(500)

  for attempt = 1, SCAN_RETRY_COUNT do
    RunVX500Project("cs")
    local resultState = GetVX500ModelResultByJson(
      '{"type":"state","moduleName":"idemodule","moduleId":6}', 1, "cs")
    if resultState == "OK" then
      local content = GetVX500ModelResultByJson(
        '{"type":"content","moduleName":"idemodule","moduleId":6}', 1, "cs")
      local sn = cleanSn(content)
      if sn ~= "" then
        state = STATE_WAIT_START
        trace("Scan succeeded, SN=" .. sn)
        sendFrame("$$MT,SN," .. sn)
        return true
      end
    end
    trace("Scan attempt " .. tostring(attempt) .. " failed")
    Wait(300)
  end

  state = STATE_IDLE
  sendFrame("$$MT,FAILED,SCAN," .. currentSide .. "," .. tostring(currentSlot))
  return false
end

local function pickCurrentDevice()
  local pickPoint = GetTrayPoint(pickTrayName(currentSide), currentSlot)
  local approachPoint = RelPointUser(pickPoint, {0, 0, APPROACH_HEIGHT, 0, 0, 0})

  trace("Pick device " .. currentSide .. ", slot=" .. tostring(currentSlot))
  MovJ(approachPoint)
  waitMotion(5000)
  VelL(5)
  MovL(pickPoint)
  waitMotion(5000)
  closeGripper()
  deviceHeld = true
  VelL(20)
  MovL(approachPoint)
  waitMotion(5000)
  MovJ(P41)
  waitMotion(5000)
end

local function runSlamTrajectory()
  trace("Run SLAM calibration trajectory")
  VelJ(30)
  VelL(30)
  AccJ(50)
  AccL(50)

  -- The old host drove these points one by one. SLAM runs them as one
  -- deterministic robot-side trajectory to avoid network wait time.
  MovJ(P42)
  waitMotion(6000)
  MovJ(P47)
  waitMotion(6000)
  MovJ(P43)
  waitMotion(6000)
  MovJ(P44)
  waitMotion(6000)
  MovJ(P45)
  waitMotion(6000)
  MovJ(P46)
  waitMotion(6000)
  MovJ(P41)
  waitMotion(6000)
end

local function putBackCurrentDevice()
  if not deviceHeld then
    state = STATE_IDLE
    sendFrame("$$MT,PUTBACKOK")
    return true
  end

  state = STATE_PUTTING_BACK
  local pickPoint = GetTrayPoint(pickTrayName(currentSide), currentSlot)
  local approachPoint = RelPointUser(pickPoint, {0, 0, APPROACH_HEIGHT, 0, 0, 0})
  trace("Put back device " .. currentSide .. ", slot=" .. tostring(currentSlot))

  MovJ(approachPoint)
  waitMotion(5000)
  VelL(5)
  MovL(pickPoint)
  waitMotion(5000)
  openGripper()
  deviceHeld = false
  VelL(20)
  MovL(approachPoint)
  waitMotion(5000)
  MovJ(P41)
  waitMotion(5000)

  state = STATE_IDLE
  sendFrame("$$MT,PUTBACKOK")
  return true
end

local function handleSlotCommand(side, slotText)
  local slot = tonumber(slotText)
  if slot == nil or slot < 1 or slot > SLOT_COUNT then
    sendFrame("$$MT,FAILED,SLOT," .. tostring(slotText))
    return
  end
  if deviceHeld then
    sendFrame("$$MT,FAILED,BUSY")
    return
  end

  currentSide = side
  currentSlot = slot
  scanCurrentDevice()
end

local function handleCommand(command)
  command = string.gsub(command or "", "^%s+", "")
  command = string.gsub(command, "%s+$", "")
  if command == "" then
    return
  end
  trace("RX " .. command)

  if string.find(command, "$$MT,CONNECT", 1, true) ~= nil then
    sendFrame("$$MT,CONNECTOK")
    return
  end

  local side, slotText = string.match(command, "%$%$MT,([A-Z]+),(%d+)")
  if side == "LEFT" or side == "RIGHT" then
    handleSlotCommand(side, slotText)
    return
  end

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
    sendFrame("$$MT,STARTOK")
    pickCurrentDevice()
    runSlamTrajectory()
    state = STATE_WAIT_PUTBACK
    sendFrame("$$MT,RUNFINISH")
    return
  end

  if string.find(command, "$$MT,PUTBACK", 1, true) ~= nil then
    putBackCurrentDevice()
    return
  end

  if string.find(command, "$$MT,FAILED", 1, true) ~= nil then
    if deviceHeld then
      putBackCurrentDevice()
    else
      state = STATE_IDLE
      sendFrame("$$MT,FAILEDOK")
    end
    return
  end

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
  local normalized = tostring(payload or "")
  normalized = string.gsub(normalized, "%$%*", "\n")
  for command in string.gmatch(normalized, "[^\r\n]+") do
    handleCommand(command)
  end
end

local function startTcpServer()
  if socket1 ~= nil then
    TCPDestroy(socket1)
    socket1 = nil
  end

  local createResult
  createResult, socket1 = TCPCreate(true, TCP_BIND_IP, TCP_PORT)
  if createResult ~= 0 then
    trace("Create TCP server failed, code=" .. tostring(createResult))
    socket1 = nil
    return false
  end

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

-- Program initialization. No calibration device is touched until START.
VelJ(30)
VelL(20)
AccJ(50)
AccL(50)
moveHome()
gripperReady = initializeGripper()
createSlamTrays()

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
