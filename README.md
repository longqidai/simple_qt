# simple_qt

基于 Qt 5.14.2 的 Windows SLAM 标定上位机。程序负责机械臂任务编排、设备 Wi-Fi 连接、设备信息读取、MQTT 采集控制、FTP 数据下载和槽位结果记录。

## 当前流程

1. 连接 GRR20 机械臂并完成 `$$MT,CONNECT` 握手。
2. 选择左右 2×3 托盘槽位。
3. 机械臂扫码，上报设备 SN。
4. 根据 SN 连接设备 Wi-Fi，并读取设备信息。
5. 通过 MQTT 启动采集。
6. 通知机械臂抓取设备并执行 SLAM 轨迹。
7. 机械臂上报运动完成后停止采集。
8. 通过 FTP 下载数据。
9. 通知机械臂放回设备并处理下一槽位。

槽位状态：蓝色待处理、橙色进行中、绿色成功、红色失败。

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
- MQTT：`192.168.1.8:26001`
- FTP：`ftp://192.168.1.8:21/`

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
