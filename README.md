# simple_qt

基于 Qt 5.14.2 的 Windows SLAM 标定上位机。程序负责机械臂任务编排、设备 Wi-Fi 连接、设备信息读取、MQTT 采集控制、FTP 数据下载和槽位结果记录。

## 当前流程

1. 连接 GRR20 机械臂并完成 `$$MT,CONNECT` 握手。
2. 选择左右 2×3 托盘槽位。
3. 机械臂扫码，上报设备 SN。
4. 根据 SN 连接设备 Wi-Fi，并读取设备信息。
5. 通过 MQTT/Protobuf 在设备上创建扫描工程和扫描任务。
6. 订阅 `slam_app/report/device_status` 并等待设备上报 `init_ready=1`；创建工程和任务后，先发送带任务 ID、母工程 ID 的 `ModelingRequest(AC_PREINIT)`。
7. 预初始化完成后发送 `data_collect AC_START` 建立原始数据记录，再发送带任务 ID、母工程 ID 的 `ModelingRequest(AC_START)`。只有设备进入 `EC_ScanScanning` 才表示传感器真正打开并开始扫描。若固件不接受独立 `AC_PREINIT`，程序会自动切换成在 `AC_START` 中携带母工程 ID 的兼容方式，不直接判定槽位失败。
8. 通知机械臂抓取设备并执行 SLAM 标定轨迹。
9. 机械臂上报运动完成后，先发送 `ModelingRequest(AC_STOP)` 并等待设备保存扫描工程，再停止标定原始数据记录。
10. 保存该设备的 SN、Wi-Fi 和采集目录，立即通知机械臂放回设备并处理下一槽位。
11. 所有槽位测量、采集和放回完成后结束机械臂流程。
12. 上位机按 SN 逐台重连设备 Wi-Fi，通过 FTP 统一下载全部采集数据。

`data_collect` 只负责标定原始数据记录，单独调用不会进入设备完整扫描流程。程序还必须启动 `modeling`，并且只有确认 `EC_ScanScanning` 后才允许机械臂运动。

设备 Wi-Fi 状态采用连续检测去抖，短暂一次扫描不到热点不会立即判定断线；MQTT 发生瞬时中断时会重新连接、重新订阅，并恢复可安全重发的采集控制指令。

槽位状态：蓝色待处理、橙色测量中、紫色等待统一下载、天蓝色下载中、绿色成功、红色失败。

## 依赖

- Windows 10/11
- Qt 5.14.2 MinGW 64-bit
- MinGW 7.3 64-bit
- Qt MQTT 5.14.2
- 系统 `curl`（用于 FTP 下载）

不依赖 OpenCV。机械臂使用 VX500 视觉项目完成 SN 扫码。

## qmake 构建

```powershell
D:\Qt\Qt5.14.2\5.14.2\mingw73_64\bin\qmake.exe simple_qt.pro CONFIG+=debug
D:\Qt\Qt5.14.2\Tools\mingw730_64\bin\mingw32-make.exe -j4
```

在当前开发环境中，也可以用 VS Code 运行任务 `构建 simple_qt (Debug)`，然后选择 `调试 simple_qt (GDB)`。

## 默认连接地址

- 机械臂 TCP：`192.168.5.1:6601`
- 设备信息 TCP：`192.168.1.8:25001`
- MQTT：优先 `192.168.1.8:1883`，连接失败时回退 `192.168.1.8:26001`
- FTP：`ftp://192.168.1.8:21/`，使用设备固件预置的本地 FTP 账号登录

使用 SSCOM 模拟机械臂时，可在界面中把机械臂地址改为 `127.0.0.1:9000`。

## 目录

- `src/`：C++ 后端与状态机
- `qml/`：主界面
- `grr20.lua`：与当前上位机协议匹配的机械臂 Lua 副本
- `.vscode/`：当前 Windows/Qt 环境的构建与调试任务

## 数据输出

- FTP 数据：`下载/SLAM_calibration/<设备采集目录>/`
- 任务结果：`文档/SLAM_calibration/results.csv`

`results.csv` 使用带 BOM 的 UTF-8，Excel 可以直接识别中文。

## 安全提示

实体机械臂首次联调前，应在空托盘、低速度和急停可用的条件下确认 2×3 托盘角点、抓取高度及整段轨迹。FTP 下载完成仅表示数据采集成功，不等于 SLAM 标定算法已经计算并通过精度验证。
