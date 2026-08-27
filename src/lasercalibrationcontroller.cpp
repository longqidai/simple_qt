#include "lasercalibrationcontroller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <cmath>
#include <cstring>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <wlanapi.h>
#endif

namespace {
constexpr int kArmHandshakeIntervalMs = 3000;
constexpr int kArmMoveTimeoutMs = 90000;
constexpr int kWifiTimeoutAttempts = 35;
constexpr int kReceiverTimeoutAttempts = 20;
// 实机通常很快返回，但手工使用 SSCOM 时需要留出足够输入时间。
constexpr int kFaceTimeoutMs = 60000;
constexpr int kAngleTimeoutMs = 20000;
constexpr int kVisionTimeoutMs = 45000;

#ifdef Q_OS_WIN
bool installOpenWifiProfile(HANDLE clientHandle, const GUID &interfaceGuid,
                            const QString &ssid, DWORD *reasonCode)
{
    const QString escapedSsid = ssid.toHtmlEscaped();
    const QString profileXml = QStringLiteral(
        "<?xml version=\"1.0\"?>"
        "<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">"
        "<name>%1</name><SSIDConfig><SSID><name>%1</name></SSID>"
        "<nonBroadcast>false</nonBroadcast></SSIDConfig>"
        "<connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
        "<MSM><security><authEncryption><authentication>open</authentication>"
        "<encryption>none</encryption><useOneX>false</useOneX>"
        "</authEncryption></security></MSM></WLANProfile>")
                                   .arg(escapedSsid);
    DWORD localReasonCode = 0;
    const DWORD result = WlanSetProfile(
        clientHandle, &interfaceGuid, 0,
        reinterpret_cast<LPCWSTR>(profileXml.utf16()), nullptr, TRUE,
        nullptr, &localReasonCode);
    if (reasonCode)
        *reasonCode = localReasonCode;
    return result == ERROR_SUCCESS;
}
#endif

QString csvCell(QString value)
{
    value.replace('"', QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(value);
}
}

LaserCalibrationController::LaserCalibrationController(QObject *parent)
    : QObject(parent)
    , m_databaseConnectionName(QStringLiteral("laser-calibration-%1")
                                   .arg(reinterpret_cast<quintptr>(this)))
{
    configureSockets();
    configureTimers();
    //视觉信号槽
    connect(&m_vision, &LaserVisionProcessor::cameraMappingChanged,
            this, [this](int, int, const QString &description) {
        m_cameraStatus = description;
        emit cameraChanged();
        appendLog(QStringLiteral("摄像头映射: %1").arg(description));
    });
    connect(&m_vision, &LaserVisionProcessor::diagnosticMessage,
            this, [this](const QString &message) {
        appendLog(QStringLiteral("[视觉算法] %1").arg(message));
    });
    connect(&m_vision, &LaserVisionProcessor::measurementFinished,
            this, &LaserCalibrationController::handleVisionMeasurement);
    initializeDatabase();
    appendLog(QStringLiteral("激光标定后端已就绪，等待连接机械臂"));
}

LaserCalibrationController::~LaserCalibrationController()
{
    shutdown();
    if (m_database.isValid()) {
        m_database.close();
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_databaseConnectionName);
    }
}

bool LaserCalibrationController::armConnected() const { return m_armReady; }
bool LaserCalibrationController::receiverConnected() const
{ return m_receiverSocket.state() == QAbstractSocket::ConnectedState; }
bool LaserCalibrationController::wifiConnected() const { return m_wifiConnected; }
bool LaserCalibrationController::running() const { return m_running; }
bool LaserCalibrationController::paused() const { return m_paused; }
QString LaserCalibrationController::currentSn() const { return m_currentSn; }
QString LaserCalibrationController::currentHardware() const { return m_currentHardware; }
QString LaserCalibrationController::currentVersion() const { return m_currentVersion; }
QString LaserCalibrationController::progressText() const { return m_progressText; }
QString LaserCalibrationController::errorText() const { return m_errorText; }
QString LaserCalibrationController::logText() const { return m_logLines.join('\n'); }
double LaserCalibrationController::laserDistance() const { return m_laserDistance; }
double LaserCalibrationController::laserHeight() const { return m_laserHeight; }
double LaserCalibrationController::horizontalNear() const { return m_horizontalNear; }
double LaserCalibrationController::horizontalFar() const { return m_horizontalFar; }
double LaserCalibrationController::horizontalLength() const { return m_horizontalLength; }
double LaserCalibrationController::verticalNear() const { return m_verticalNear; }
double LaserCalibrationController::verticalFar() const { return m_verticalFar; }
double LaserCalibrationController::verticalLength() const { return m_verticalLength; }
bool LaserCalibrationController::cameraBusy() const { return m_vision.busy(); }
QString LaserCalibrationController::cameraStatus() const { return m_cameraStatus; }
QString LaserCalibrationController::cameraImageUrl() const { return m_cameraImageUrl; }
bool LaserCalibrationController::singleCameraDebug() const
{ return m_vision.singleCameraDebug(); }

void LaserCalibrationController::setSingleCameraDebug(bool enabled)
{
    if (m_running || m_vision.busy() || m_vision.singleCameraDebug() == enabled)
        return;
    m_vision.setSingleCameraDebug(enabled);
    m_cameraStatus = enabled
        ? QStringLiteral("模拟测试：打开真实摄像头并执行完整流程，视觉数据使用模拟值")
        : QStringLiteral("正式双摄像头模式：执行参考圆和激光点视觉测量");
    appendLog(m_cameraStatus);
    emit cameraChanged();
}
int LaserCalibrationController::currentFace() const { return m_currentFace; }
int LaserCalibrationController::faceRestBits() const { return m_faceRestBits; }
int LaserCalibrationController::slotRevision() const { return m_slotRevision; }
QVariantList LaserCalibrationController::results() const { return m_results; }

int LaserCalibrationController::successCount() const
{
    int count = 0;
    for (const QVariant &item : m_results)
        count += item.toMap().value(QStringLiteral("success")).toBool() ? 1 : 0;
    return count;
}

int LaserCalibrationController::failureCount() const
{
    int count = 0;
    for (const QVariant &item : m_results)
        count += item.toMap().value(QStringLiteral("success")).toBool() ? 0 : 1;
    return count;
}

int LaserCalibrationController::activeCount() const
{
    return m_running && m_currentIndex >= 0 ? 1 : 0;
}

void LaserCalibrationController::configureSockets()
{
    connect(&m_armSocket, &QTcpSocket::connected, this, [this] {
        appendLog(QStringLiteral("机械臂 TCP 已连接: %1:%2").arg(m_armHost).arg(m_armPort));
        setState(State::ArmHandshake, QStringLiteral("正在与机械臂握手"));
        sendArm("$$MT,CONNECT\r\n");
        m_handshakeTimer.start();
    });
    connect(&m_armSocket, &QTcpSocket::readyRead, this, [this] {
        m_armBuffer.append(m_armSocket.readAll());
        processArmBuffer();
    });
    connect(&m_armSocket, &QTcpSocket::disconnected, this, [this] {
        const bool wasReady = m_armReady;
        m_armReady = false;
        m_handshakeTimer.stop();
        if (wasReady)
            emit armConnectedChanged();
        appendLog(QStringLiteral("机械臂 TCP 已断开"));
        if (m_running && !m_shutdown)
            failCurrentSlot(QStringLiteral("机械臂连接中断"));
        else if (!m_shutdown)
            setProgress(QStringLiteral("机械臂未连接"));
    });
    connect(&m_armSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        appendLog(QStringLiteral("机械臂 TCP 错误: %1").arg(m_armSocket.errorString()));
        if (!m_running)
            setError(QStringLiteral("连接机械臂失败: %1").arg(m_armSocket.errorString()));
    });

    connect(&m_receiverSocket, &QTcpSocket::connected, this, [this] {
        m_receiverRetryTimer.stop();
        m_receiverAttempts = 0;
        m_receiverBuffer.clear();
        appendLog(QStringLiteral("接收机 TCP 已连接: %1:%2")
                      .arg(m_receiverHost).arg(m_receiverPort));
        emit receiverConnectedChanged();
        // 实机通常很快返回；SSCOM 人工模拟时需要留出输入和发送报文的时间。
        setState(State::ReadingDeviceInfo, QStringLiteral("正在读取接收机信息"), 60000);
        // 部分接收机在 TCP connected 信号刚到时命令解释器还没有完全就绪。
        QTimer::singleShot(500, this, [this] {
            if (m_running && m_state == State::ReadingDeviceInfo
                && m_receiverSocket.state() == QAbstractSocket::ConnectedState) {
                requestReceiverInfo();
            }
        });
    });
    connect(&m_receiverSocket, &QTcpSocket::readyRead, this, [this] {
        m_receiverBuffer.append(m_receiverSocket.readAll());
        processReceiverBuffer();
    });
    connect(&m_receiverSocket, &QTcpSocket::disconnected, this, [this] {
        emit receiverConnectedChanged();
        appendLog(QStringLiteral("接收机 TCP 已断开"));
        if (m_running && m_state == State::ConnectingReceiver) {
            if (!m_receiverRetryTimer.isActive())
                m_receiverRetryTimer.start();
            return;
        }
        if (m_running && m_state == State::ReadingDeviceInfo
            && m_receiverReconnectAttempts < 2) {
            ++m_receiverReconnectAttempts;
            appendLog(QStringLiteral("读取设备信息时连接被关闭，%1 秒后自动重连（%2/2）")
                          .arg(m_receiverReconnectAttempts)
                          .arg(m_receiverReconnectAttempts));
            setState(State::ConnectingReceiver,
                     QStringLiteral("接收机连接中断，正在自动恢复"), 30000);
            m_receiverAttempts = 0;
            QTimer::singleShot(1000 * m_receiverReconnectAttempts, this, [this] {
                if (!m_running || m_state != State::ConnectingReceiver)
                    return;
                m_receiverSocket.abort();
                m_receiverSocket.connectToHost(m_receiverHost,
                                               static_cast<quint16>(m_receiverPort));
                if (!m_receiverRetryTimer.isActive())
                    m_receiverRetryTimer.start();
            });
            return;
        }
        if (m_running && m_state != State::WaitingPutback
            && m_state != State::Aborting && !m_shutdown) {
            failCurrentSlot(QStringLiteral("接收机连接中断"));
        }
    });
    connect(&m_receiverSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),
            this, [this](QAbstractSocket::SocketError) {
        appendLog(QStringLiteral("接收机 TCP 错误: %1").arg(m_receiverSocket.errorString()));
        if (m_state == State::ConnectingReceiver && m_running
            && !m_receiverRetryTimer.isActive()) {
            m_receiverRetryTimer.start();
        }
    });
}

void LaserCalibrationController::configureTimers()
{
    //设置定时器
    m_handshakeTimer.setInterval(kArmHandshakeIntervalMs);
    connect(&m_handshakeTimer, &QTimer::timeout, this, [this] {
        if (m_armSocket.state() == QAbstractSocket::ConnectedState && !m_armReady)
            sendArm("$$MT,CONNECT\r\n");
    });

    m_stepTimer.setSingleShot(true);
    connect(&m_stepTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Aborting) {
            appendLog(QStringLiteral("停止流程等待机械臂超时，已完成本地清理"));
            finishAbort();
            return;
        }
        if (m_running)
            failCurrentSlot(QStringLiteral("当前步骤等待超时: %1").arg(m_progressText));
    });

    m_wifiTimer.setInterval(1000);
    connect(&m_wifiTimer, &QTimer::timeout, this, [this] {
        if (!m_running || m_state != State::ConnectingWifi)
            return;
        const QString ssid = connectedWifiName();
        if (!ssid.isEmpty() && ssid.contains(m_currentSn, Qt::CaseInsensitive)) {
            QString localAddress;
            if (!receiverNetworkReady(&localAddress)) {
                ++m_wifiAttempts;
                if (m_wifiAttempts == 1 || m_wifiAttempts % 3 == 0) {
                    appendLog(QStringLiteral(
                        "已连接目标 Wi-Fi %1，等待 DHCP/路由切换到接收机网段")
                                  .arg(ssid));
                }
                if (m_wifiAttempts >= kWifiTimeoutAttempts) {
                    m_wifiTimer.stop();
                    failCurrentSlot(QStringLiteral(
                        "已连接设备 Wi-Fi，但电脑未取得接收机同网段 IP"));
                }
                return;
            }
            m_wifiTimer.stop();
            m_wifiConnected = true;
            emit wifiConnectedChanged();
            appendLog(QStringLiteral("接收机 Wi-Fi 已就绪: %1，本机地址: %2")
                          .arg(ssid, localAddress));
            connectReceiver();
            return;
        }
        ++m_wifiAttempts;
        if (m_wifiAttempts % 5 == 0)
            connectWifiByNamePart(m_currentSn);
        if (m_wifiAttempts >= kWifiTimeoutAttempts) {
            m_wifiTimer.stop();
            failCurrentSlot(QStringLiteral("连接接收机 Wi-Fi 超时"));
        }
    });

    m_receiverRetryTimer.setInterval(1200);
    connect(&m_receiverRetryTimer, &QTimer::timeout, this, [this] {
        if (!m_running || m_state != State::ConnectingReceiver)
            return;
        if (++m_receiverAttempts > kReceiverTimeoutAttempts) {
            m_receiverRetryTimer.stop();
            failCurrentSlot(QStringLiteral("连接接收机信息端口超时"));
            return;
        }
        if (m_receiverSocket.state() == QAbstractSocket::UnconnectedState)
            m_receiverSocket.connectToHost(m_receiverHost,
                                            static_cast<quint16>(m_receiverPort));
    });
}

void LaserCalibrationController::connectArm(const QString &host, int port)
{
    if (m_running) {
        setError(QStringLiteral("标定进行中，不能更换机械臂连接"));
        return;
    }
    m_shutdown = false;
    m_errorText.clear();
    emit errorChanged();
    m_armHost = host.trimmed().isEmpty() ? QStringLiteral("192.168.5.1") : host.trimmed();
    m_armPort = port > 0 && port <= 65535 ? port : 6601;
    m_armReady = false;
    emit armConnectedChanged();
    m_armBuffer.clear();
    m_armSocket.abort();
    setProgress(QStringLiteral("正在连接机械臂 %1:%2").arg(m_armHost).arg(m_armPort));
    appendLog(m_progressText);
    m_armSocket.connectToHost(m_armHost, static_cast<quint16>(m_armPort));
}

void LaserCalibrationController::disconnectArm()
{
    if (m_running) {
        stopCalibration();
        return;
    }
    m_handshakeTimer.stop();
    m_armSocket.abort();
    if (m_armReady) {
        m_armReady = false;
        emit armConnectedChanged();
    }
    setProgress(QStringLiteral("机械臂未连接"));
}

bool LaserCalibrationController::startCalibration(const QVariantList &leftSlots,
                                                   const QVariantList &rightSlots,
                                                   const QVariantMap &measurements)
{
    if (m_running) {
        setError(QStringLiteral("已有激光标定任务正在运行"));
        return false;
    }
    if (!m_armReady) {
        setError(QStringLiteral("请先连接并完成机械臂握手"));
        return false;
    }
    QString reason;
    if (!validateMeasurements(measurements, &reason)) {//检查水平线高
        setError(reason);
        return false;
    }

    m_slots.clear();
    auto appendSlots = [this](const QString &tray, const QVariantList &values) {
        for (const QVariant &value : values) {
            const int slot = value.toInt();
            // 激光标定托盘固定为 2×5；即便由 QML 以外的调用方启动，
            // 也不能把超出 1~10 的槽位下发给机械臂。
            if (slot >= 1 && slot <= 10)
                m_slots.append(Slot{tray, slot, QStringLiteral("pending")});
        }
    };
    appendSlots(QStringLiteral("left"), leftSlots);
    appendSlots(QStringLiteral("right"), rightSlots);
    if (m_slots.isEmpty()) {
        setError(QStringLiteral("请至少选择一个托盘槽位"));
        return false;
    }

    m_results.clear();
    m_currentIndex = -1;
    m_currentFace = 0;
    m_faceRestBits = 0;
    m_visionTarget = VisionTarget::None;
    m_vision.cancelPendingResult();
    m_laserDistance = 0.0;
    m_laserHeight = 0.0;
    m_verticalCloseRawHeight = 0.0;
    m_verticalCloseRange = 0.0;
    m_verticalFarRawHeight = 0.0;
    m_verticalFarRange = 0.0;
    m_horizontalFarRawHeight = 0.0;
    m_horizontalFarRange = 0.0;
    m_horizontalNear = 0.0;
    m_horizontalFar = 0.0;
    m_horizontalLength = 0.0;
    m_verticalNear = 0.0;
    m_verticalFar = 0.0;
    m_verticalLength = 0.0;
    m_cameraImageUrl.clear();
    m_cameraStatus = QStringLiteral("等待视觉测量");
    m_slotRevision++;
    emit slotRevisionChanged();
    emit faceChanged();
    emit faceRestBitsChanged();
    emit measurementChanged();
    emit cameraChanged();
    emit resultsChanged();
    m_running = true;
    m_paused = false;
    m_shutdown = false;
    emit runningChanged();
    emit pausedChanged();
    setError(QString());
    appendLog(QStringLiteral("开始激光标定，共 %1 个槽位").arg(m_slots.size()));
    beginNextSlot();
    return true;
}

void LaserCalibrationController::stopCalibration()
{
    if (!m_running)
        return;
    appendLog(QStringLiteral("用户请求终止激光标定，正在安全关闭激光并放回设备"));
    m_visionTarget = VisionTarget::None;
    m_vision.cancelPendingResult();
    emit cameraChanged();
    setState(State::Aborting, QStringLiteral("正在安全终止并放回当前设备"), 15000);
    ensureReceiverSafe();
    if (m_armReady)
        sendArm("$$MT,FAILED\r\n");
    else
        finishAbort();
}

void LaserCalibrationController::togglePause()
{
    if (!m_running)
        return;
    m_paused = !m_paused;
    emit pausedChanged();
    appendLog(m_paused ? QStringLiteral("任务将在当前动作完成后暂停")
                       : QStringLiteral("任务继续"));
}

void LaserCalibrationController::resetReceiverCalibration()
{
    if (!receiverConnected()) {
        setError(QStringLiteral("接收机 TCP 未连接，无法复位激光标定"));
        return;
    }
    sendReceiver("set laser reset calibration\r\n");
    appendLog(QStringLiteral("已发送激光外参复位指令"));
}

QString LaserCalibrationController::slotStatus(const QString &tray, int slot) const
{
    for (const Slot &item : m_slots) {
        if (item.tray == tray && item.slot == slot)
            return item.status;
    }
    return QStringLiteral("idle");
}

void LaserCalibrationController::shutdown()
{
    if (m_shutdown)
        return;
    m_shutdown = true;
    m_stepTimer.stop();
    m_vision.cancelPendingResult();
    m_wifiTimer.stop();
    m_receiverRetryTimer.stop();
    m_handshakeTimer.stop();
    ensureReceiverSafe();
    m_receiverSocket.abort();
    m_armSocket.abort();
    m_running = false;
    m_armReady = false;
    m_wifiConnected = false;
}

void LaserCalibrationController::clearLog()
{
    if (m_logLines.isEmpty())
        return;
    m_logLines.clear();
    emit logChanged();
    emit logCleared();
}

void LaserCalibrationController::setState(State state, const QString &progress, int timeoutMs)
{
    m_state = state;
    setProgress(progress);
    m_stepTimer.stop();
    if (timeoutMs > 0)
        m_stepTimer.start(timeoutMs);
}

void LaserCalibrationController::setProgress(const QString &text)
{
    if (m_progressText == text)
        return;
    m_progressText = text;
    emit progressChanged();
}

void LaserCalibrationController::setError(const QString &text)
{
    if (m_errorText == text)
        return;
    m_errorText = text;
    emit errorChanged();
}

void LaserCalibrationController::appendLog(const QString &text)
{
    if (text.isEmpty())
        return;
    const QString entry = QStringLiteral("[%1] %2")
                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                   text);
    m_logLines.append(entry);
    while (m_logLines.size() > 500)
        m_logLines.removeFirst();
    emit logChanged();
    emit logEntryAdded(entry);
}

void LaserCalibrationController::sendArm(const QByteArray &command)
{
    if (m_armSocket.state() != QAbstractSocket::ConnectedState) {
        appendLog(QStringLiteral("机械臂指令未发送（TCP 未连接）: %1")
                      .arg(QString::fromLatin1(command).trimmed()));
        return;
    }
    m_armSocket.write(command);
    m_armSocket.flush();
    appendLog(QStringLiteral("发送机械臂: %1")
                  .arg(QString::fromLatin1(command).trimmed()));
}

bool LaserCalibrationController::sendReceiver(const QByteArray &command)
{
    if (m_receiverSocket.state() != QAbstractSocket::ConnectedState) {
        appendLog(QStringLiteral("接收机指令未发送（TCP 未连接）: %1")
                      .arg(QString::fromLatin1(command).trimmed()));
        return false;
    }
    if (m_receiverSocket.write(command) < 0)
        return false;
    m_receiverSocket.flush();
    appendLog(QStringLiteral("发送接收机: %1")
                  .arg(QString::fromLatin1(command).trimmed()));
    return true;
}

void LaserCalibrationController::processArmBuffer()
{
    if (m_state == State::WaitingSn) {
        const QString pending = QString::fromUtf8(m_armBuffer).trimmed();
        static const QRegularExpression snPattern(
            QStringLiteral("(?:SNFF)?([A-Za-z][A-Za-z0-9_-]{6,})"));
        const QRegularExpressionMatch match = snPattern.match(pending);
        if (match.hasMatch() && !pending.contains(QStringLiteral("MT,"))) {
            m_armBuffer.clear();
            handleScannedSn(match.captured(1));
            return;
        }
    }

    while (!m_armBuffer.isEmpty()) {
        const int marker = m_armBuffer.indexOf("$*");
        const int newline = m_armBuffer.indexOf('\n');
        int end = -1;
        int removeLength = 0;
        if (marker >= 0 && (newline < 0 || marker < newline)) {
            end = marker;
            removeLength = marker + 2;
        } else if (newline >= 0) {
            end = newline;
            removeLength = newline + 1;
        }
        if (end < 0)
            break;
        QByteArray frameData = m_armBuffer.left(end);
        m_armBuffer.remove(0, removeLength);
        frameData.replace('\0', ' ');
        const QString frame = QString::fromUtf8(frameData).trimmed();
        if (!frame.isEmpty())
            processArmFrame(frame);
    }
}

void LaserCalibrationController::processArmFrame(const QString &frame)
{
    appendLog(QStringLiteral("收到机械臂: %1").arg(frame));
    // SSCOM 联调时通常只开一个 TCP Server。允许把接收机的激光标定
    // 上报从机械臂模拟通道送入；真实设备仍从 25001 接收机通道上报。
    if (frame.contains(QStringLiteral("$LASERBACK"), Qt::CaseInsensitive)) {
        appendLog(QStringLiteral("检测到 SSCOM 模拟激光报文，转交接收机协议解析"));
        processReceiverLine(frame);
        return;
    }
    if (frame.contains(QStringLiteral("CONNECTOK"), Qt::CaseInsensitive)) {
        m_handshakeTimer.stop();
        if (!m_armReady) {
            m_armReady = true;
            emit armConnectedChanged();
        }
        if (!m_running)
            setState(State::Idle, QStringLiteral("机械臂已就绪，可选择槽位开始标定"));
        return;
    }
    if (frame.contains(QStringLiteral("FAILED"), Qt::CaseInsensitive)) {
        if (m_state == State::WaitingPutback || m_state == State::Aborting) {
            if (m_state == State::Aborting)
                finishAbort();
            else
                completeCurrentSlot(false, m_pendingReason.isEmpty()
                                             ? QStringLiteral("机械臂报告失败")
                                             : m_pendingReason);
        } else if (m_running) {
            failCurrentSlot(QStringLiteral("机械臂报告动作失败"));
        }
        return;
    }
    if (frame.contains(QStringLiteral("MT,OK"), Qt::CaseInsensitive)) {
        handleArmOk();
        return;
    }
    if (m_state == State::WaitingSn)
        handleScannedSn(frame);
}

void LaserCalibrationController::handleArmOk()
{
    m_stepTimer.stop();
    switch (m_state) {
    case State::WaitingPoint1:
        requestPlanarCalibration(1);
        break;
    case State::WaitingPoint2:
        requestPlanarCalibration(2);
        break;
    case State::WaitingPoint3:
        // 旧版在机械臂到达远点后重新开启测距，等待报文稳定后才拍远焦图。
        sendReceiver("log laserrangingb ontime 1\r\n");
        sendReceiver("set laser survey auto\r\n");
        appendLog(QStringLiteral("机械臂已到达垂直远点，重新开启激光测距并等待稳定"));
        QTimer::singleShot(800, this, [this] {
            if (m_running && m_state == State::WaitingPoint3)
                startVisionMeasurement(VisionTarget::VerticalFar);
        });
        break;
    case State::WaitingPoint4:
        appendLog(QStringLiteral("机械臂已到达 NEXT4 校准姿态，开始面 3 校准"));
        requestPlanarCalibration(3);
        break;
    case State::WaitingPoint5:
        requestPlanarCalibration(4);
        break;
    case State::WaitingPoint6:
        requestPlanarCalibration(5);
        break;
    case State::WaitingPoint7:
        requestPlanarCalibration(6);
        break;
    case State::WaitingPutback:
        completeCurrentSlot(m_pendingSuccess, m_pendingReason);
        break;
    case State::Aborting:
        finishAbort();
        break;
    default:
        appendLog(QStringLiteral("已忽略与当前步骤不匹配的机械臂 OK"));
        break;
    }
}

void LaserCalibrationController::handleScannedSn(const QString &sn)
{
    if (m_state != State::WaitingSn)
        return;
    const QString cleanSn = normalizeSn(sn);
    if (cleanSn.isEmpty()) {
        failCurrentSlot(QStringLiteral("机械臂扫码结果无效: %1").arg(sn));
        return;
    }
    m_stepTimer.stop();
    m_currentSn = cleanSn;
    emit deviceInfoChanged();
    appendLog(QStringLiteral("机械臂扫码完成，SN=%1").arg(cleanSn));
    setState(State::ConnectingWifi,
             QStringLiteral("正在连接接收机 Wi-Fi: %1").arg(cleanSn));
    m_wifiConnected = false;
    emit wifiConnectedChanged();
    m_wifiAttempts = 0;
    connectWifiByNamePart(cleanSn);
    m_wifiTimer.start();
}

void LaserCalibrationController::processReceiverBuffer()
{
    //二进制报文处理
    const QByteArray snHeader("\xaa\x44\x12\x1c\x25", 5);
    const QByteArray rangeHeader("\xaa\x44\x12\x1c\x89\x13", 6);

    bool changed = false;
    bool extracted = true;
    while (extracted) {
        extracted = false;
        int start = m_receiverBuffer.indexOf(rangeHeader);
        if (start >= 0 && m_receiverBuffer.size() >= start + 40) {
            const QByteArray frame = m_receiverBuffer.mid(start, 40);
            m_receiverBuffer.remove(start, 40);
            processLaserRangeFrame(frame);//解析激光测距报文
            extracted = true;
            continue;
        }
        start = m_receiverBuffer.indexOf(snHeader);
        if (start >= 0 && m_receiverBuffer.size() >= start + 144) {//解析SN报文
            const QByteArray frame = m_receiverBuffer.mid(start, 144);
            m_receiverBuffer.remove(start, 144);
            const QString sn = QString::fromLatin1(frame.mid(52, 9)).trimmed();
            if (!sn.isEmpty() && sn != m_currentSn) {
                m_currentSn = sn;
                changed = true;
            }
            if (!sn.isEmpty())
                m_receiverSnReceived = true;
            appendLog(QStringLiteral("接收机版本报文 SN: %1").arg(sn));
            extracted = true;
            continue;
        }
        start = m_receiverBuffer.indexOf("$$SI");//$$SI 硬件号报文
        if (start >= 0 && m_receiverBuffer.size() >= start + 62) {
            const QByteArray frame = m_receiverBuffer.mid(start, 62);
            m_receiverBuffer.remove(start, 62);
            quint8 crc = 0;
            for (int i = 0; i < 61; ++i)
                crc ^= static_cast<quint8>(frame.at(i));
            if (crc == static_cast<quint8>(frame.at(61))) {
                const QString hw = QString::fromLatin1(frame.mid(29, 3)).trimmed();
                if (!hw.isEmpty() && hw != m_currentHardware) {
                    m_currentHardware = hw;
                    changed = true;
                }
                appendLog(QStringLiteral("接收机硬件号: %1").arg(hw));
            } else {
                appendLog(QStringLiteral("接收机 $$SI 报文 CRC 校验失败"));
            }
            extracted = true;
        }
    }

    // TCP 可能把二进制设备信息报文拆成多段。已看到报文头但长度尚未
    // 收齐时必须保留缓冲区，不能让下面的文本换行解析误删半个报文。
    const int pendingRange = m_receiverBuffer.indexOf(rangeHeader);
    const int pendingSn = m_receiverBuffer.indexOf(snHeader);
    const int pendingSi = m_receiverBuffer.indexOf("$$SI");
    if ((pendingRange >= 0 && m_receiverBuffer.size() < pendingRange + 40)
        || (pendingSn >= 0 && m_receiverBuffer.size() < pendingSn + 144)
        || (pendingSi >= 0 && m_receiverBuffer.size() < pendingSi + 62)) {
        if (changed)
            emit deviceInfoChanged();
        tryFinishReceiverInfo();
        return;
    }

    int newline = -1;
    while ((newline = m_receiverBuffer.indexOf('\n')) >= 0) {
        QByteArray data = m_receiverBuffer.left(newline);
        m_receiverBuffer.remove(0, newline + 1);
        data.replace('\0', ' ');
        const QString line = QString::fromUtf8(data).trimmed();
        if (!line.isEmpty())
            processReceiverLine(line);//检查文本中的关键字段，把解析结果保存
    }
    while (!m_receiverBuffer.isEmpty() && m_receiverBuffer.at(0) == '\0')
        m_receiverBuffer.remove(0, 1);

    if (changed)
        emit deviceInfoChanged();
    tryFinishReceiverInfo();
}

void LaserCalibrationController::processReceiverLine(const QString &line)
{
    appendLog(QStringLiteral("接收机响应: %1").arg(line));
    // SSCOM 或真实串流前面可能残留提示符/不可见字符，不能只用 startsWith。
    const int laserBackStart = line.indexOf(QStringLiteral("$LASERBACK"), 0,
                                             Qt::CaseInsensitive);
    if (laserBackStart >= 0) {
        const LaserBack report = parseLaserBack(line.mid(laserBackStart));
        if (report.valid) {
            appendLog(QStringLiteral(
                "$LASERBACK 解析成功：面=%1，静置状态=%2，全部完成=%3，错误码=%4，当前流程状态=%5")
                          .arg(report.panelId)
                          .arg(report.restState)
                          .arg(report.sideState)
                          .arg(report.errorCode)
                          .arg(static_cast<int>(m_state)));
            handleLaserBack(report);
        } else {
            appendLog(QStringLiteral("无法解析 $LASERBACK 报文"));
        }
        return;
    }
    //设备信息
    QJsonParseError error;
    const QJsonDocument json = QJsonDocument::fromJson(line.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && json.isObject()) {
        const QJsonObject object = json.object();
        if (object.contains(QStringLiteral("sn"))) {
            m_currentSn = object.value(QStringLiteral("sn")).toString();
            m_receiverSnReceived = !m_currentSn.isEmpty();
        }
        if (object.contains(QStringLiteral("hw")))
            m_currentHardware = object.value(QStringLiteral("hw")).toString();
        if (object.contains(QStringLiteral("version")))
            m_currentVersion = object.value(QStringLiteral("version")).toString();
        emit deviceInfoChanged();
        tryFinishReceiverInfo();
        return;
    }

    static const QRegularExpression versionPattern(
        QStringLiteral("(?:VERSIONB?|VER)\\s*[:=, ]+\\s*([^,;\\s]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch versionMatch = versionPattern.match(line);
    if (versionMatch.hasMatch()) {
        m_currentVersion = versionMatch.captured(1);
        emit deviceInfoChanged();
    }
}

void LaserCalibrationController::processLaserRangeFrame(const QByteArray &frame)
{
    if (frame.size() < 32)
        return;
    float metres = 0.0f;
    std::memcpy(&metres, frame.constData() + 28, sizeof(float));
    if (!std::isfinite(metres) || metres < 0.0f)
        return;
    m_laserDistance = static_cast<double>(metres) * 1000.0;
    emit measurementChanged();
}

LaserCalibrationController::LaserBack
LaserCalibrationController::parseLaserBack(const QString &line) const
{
    LaserBack result;
    QString body = line.trimmed();
    const int asterisk = body.lastIndexOf('*');
    if (asterisk >= 0)
        body = body.left(asterisk);
    const QStringList fields = body.split(',', Qt::KeepEmptyParts);
    if (fields.size() < 6
        || fields.first().compare(QStringLiteral("$LASERBACK"),
                                  Qt::CaseInsensitive) != 0) {
        return result;
    }

    bool ok = false;
    result.restBits = fields.at(2).toInt(&ok);
    if (!ok) return result;
    result.restState = fields.at(3).toInt(&ok);
    if (!ok) return result;
    result.sideState = fields.at(4).toInt(&ok);
    if (!ok) return result;
    result.panelId = fields.at(5).toInt(&ok);
    if (!ok) return result;
    // 完整实机协议包含六个面的36项参数（共51个字段）；SSCOM测试允许
    // 只发送前6个状态字段，或者发送旧版21字段简化报文。
    if (fields.size() >= 51) {
        for (int i = 6; i <= 11; ++i) {
            result.faceParameters.append(fields.at(i).toDouble(&ok));
            if (!ok) return LaserBack();
        }
        for (int i = 42; i <= 47; ++i) {
            result.bias.append(fields.at(i).toDouble(&ok));
            if (!ok) return LaserBack();
        }
        result.pitch = fields.at(48).toDouble(&ok);
        if (!ok) return LaserBack();
        result.yaw = fields.at(49).toDouble(&ok);
        if (!ok) return LaserBack();
        result.errorCode = fields.at(50).toInt(&ok);
        if (!ok) return LaserBack();
    } else if (fields.size() >= 21) {
        for (int i = 6; i <= 11; ++i) {
            result.faceParameters.append(fields.at(i).toDouble(&ok));
            if (!ok) return LaserBack();
        }
        for (int i = 12; i <= 17; ++i) {
            result.bias.append(fields.at(i).toDouble(&ok));
            if (!ok) return LaserBack();
        }
        result.yaw = fields.at(18).toDouble(&ok);
        if (!ok) return LaserBack();
        result.pitch = fields.at(19).toDouble(&ok);
        if (!ok) return LaserBack();
        result.errorCode = fields.at(20).toInt(&ok);
        if (!ok) return LaserBack();
    }
    result.valid = true;
    return result;
}

void LaserCalibrationController::handleLaserBack(const LaserBack &report)
{
    appendLog(QStringLiteral(
        "[面校准解析] 报文有效，面=%1，当前面状态=%2，六面掩码=0x%3，整机完成=%4，错误码=%5")
                  .arg(report.panelId).arg(report.restState)
                  .arg(report.restBits & 0x3f, 2, 16, QLatin1Char('0'))
                  .arg(report.sideState).arg(report.errorCode));
    // $LASERBACK 的 restBits 是六个面的累计完成掩码。部分 SSCOM
    // 简化报文会在每一面都固定填写 1，因此还要根据“当前面已静置完成”
    // 补上对应位，并与本槽位已经完成的位合并，保证状态只增不减。
    int restBits = report.restBits & 0x3f;
    // 部分固件最后一个字段上报的是机械臂点位（1~7），而不是面号；
    // 点位7仍对应面6。因此超出1~6时使用状态机当前正在校准的面。
    const int completedFace = (report.panelId >= 1 && report.panelId <= 6)
        ? report.panelId : m_currentFace;
    if (report.restState == 2
        && completedFace >= 1 && completedFace <= 6) {
        restBits |= (1 << (completedFace - 1));
    }
    restBits |= m_faceRestBits;
    if (m_faceRestBits != restBits) {
        m_faceRestBits = restBits;
        emit faceRestBitsChanged();
        appendLog(QStringLiteral("面静置状态已更新：%1")
                      .arg(m_faceRestBits, 6, 2, QLatin1Char('0')));
    }

    if (report.panelId >= 1 && report.panelId <= 6
        && report.panelId != m_currentFace) {
        m_currentFace = report.panelId;
        emit faceChanged();
    }
    if (report.errorCode != 0) {
        const QString detail = report.errorCode == 1
            ? QStringLiteral("激光外参校准失败，需要复位整个流程")
            : report.errorCode == 2
                ? QStringLiteral("当前面已经校准，请检查机械臂姿态")
                : QStringLiteral("未知激光标定错误 %1").arg(report.errorCode);
        failCurrentSlot(detail);
        return;
    }

    if (m_state == State::WaitingAngle) {
        if (report.sideState != 1)
            return;
        m_stepTimer.stop();
        m_finalPitch = report.pitch;
        m_finalYaw = report.yaw;
        m_finalBias = report.bias;
        appendLog(QStringLiteral("激光角度校准完成: pitch=%1, yaw=%2")
                      .arg(m_finalPitch, 0, 'f', 6)
                      .arg(m_finalYaw, 0, 'f', 6));
        // NEXT1 负责夹取，NEXT2～NEXT7 对应后续姿态。角度校准完成后
        // 不再增加额外标定点编号，直接通知机械臂执行“回 Home + 安全放回”。
        ensureReceiverSafe();
        m_pendingSuccess = true;
        m_pendingReason.clear();
        sendArm("$$MT,SUCCESS\r\n");
        setState(State::WaitingPutback,
                 QStringLiteral("标定成功，等待机械臂安全放回设备"),
                 kArmMoveTimeoutMs);
        return;
    }

    if (report.restState != 2)
        return;
    appendLog(QStringLiteral("面 %1 静置与面校准完成").arg(m_currentFace));
    switch (m_state) {
    case State::WaitingFace1:
        moveToPoint(2);
        break;
    case State::WaitingFace2:
        sendReceiver("set laser on\r\n");
        sendReceiver("log laserrangingb ontime 1\r\n");
        sendReceiver("set laser survey auto\r\n");
        appendLog(QStringLiteral("激光与自动测距已开启，等待稳定后使用近焦摄像头测量垂直近点"));
        QTimer::singleShot(800, this, [this] {
            if (m_running && m_state == State::WaitingFace2)
                startVisionMeasurement(VisionTarget::VerticalNear);
        });
        break;
    case State::WaitingFace3:
        moveToPoint(5);
        break;
    case State::WaitingFace4:
        moveToPoint(6);
        break;
    case State::WaitingFace5:
        moveToPoint(7);
        break;
    case State::WaitingFace6:
        // 六面校准完成后，按旧版顺序重新开启激光和测距，再测水平远点。
        sendReceiver("set laser on\r\n");
        sendReceiver("log laserrangingb ontime 1\r\n");
        sendReceiver("set laser survey auto\r\n");
        appendLog(QStringLiteral("六面校准完成，等待测距稳定后测量水平远点"));
        QTimer::singleShot(800, this, [this] {
            if (m_running && m_state == State::WaitingFace6)
                startVisionMeasurement(VisionTarget::HorizontalFar);
        });
        break;
    default:
        break;
    }
}

bool LaserCalibrationController::validateMeasurements(const QVariantMap &measurements,
                                                         QString *reason)
{
    if (!finiteNumber(measurements.value(QStringLiteral("horizontalLineHeight")),
                      &m_horizontalLineHeight)
        || m_horizontalLineHeight <= 0.0) {
        if (reason)
            *reason = QStringLiteral("请选择有效的 GRR20 水平线高参数");
        return false;
    }
    return true;
}

void LaserCalibrationController::beginNextSlot()
{
    continueIfNotPaused([this] {//是否暂停
        ++m_currentIndex;
        if (m_currentIndex >= m_slots.size()) {
            finishBatch();
            return;
        }
        beginCurrentSlot();
    });
}

void LaserCalibrationController::beginCurrentSlot()
{
    Slot &slot = m_slots[m_currentIndex];
    m_currentResultRecorded = false;
    m_pendingSuccess = false;
    m_pendingReason.clear();
    m_currentSn.clear();
    m_currentHardware.clear();
    m_currentVersion.clear();
    m_receiverSnReceived = false;
    m_finalPitch = 0.0;
    m_finalYaw = 0.0;
    m_finalBias.clear();
    m_visionTarget = VisionTarget::None;
    m_vision.cancelPendingResult();
    m_laserDistance = 0.0;
    m_laserHeight = 0.0;
    m_verticalCloseRawHeight = 0.0;
    m_verticalCloseRange = 0.0;
    m_verticalFarRawHeight = 0.0;
    m_verticalFarRange = 0.0;
    m_horizontalFarRawHeight = 0.0;
    m_horizontalFarRange = 0.0;
    m_currentFace = 0;
    m_faceRestBits = 0;
    m_receiverBuffer.clear();
    emit deviceInfoChanged();
    emit faceChanged();
    emit faceRestBitsChanged();
    emit measurementChanged();
    updateSlotStatus(slot.tray, slot.slot, QStringLiteral("active"));
    setError(QString());
    //拼接机械臂指令，左托盘使用 SETMOD1，右托盘使用 SETMOD2
    const QByteArray mode = slot.tray == QStringLiteral("left")
        ? QByteArray("$$MT,SETMOD1\r\n") : QByteArray("$$MT,SETMOD2\r\n");
    sendArm(mode);
    QTimer::singleShot(120, this, [this, slot] {
        if (!m_running || m_currentIndex < 0)
            return;
        sendArm(QByteArray::number(slot.slot) + "\r\n");
        QTimer::singleShot(120, this, [this] {
            if (!m_running)
                return;
            sendArm("$$MT,NEXT\r\n");  //扫码
            setState(State::WaitingSn,
                     QStringLiteral("等待机械臂扫描 %1托盘 %2 号位")
                         .arg(m_slots.at(m_currentIndex).tray == QStringLiteral("left")
                                  ? QStringLiteral("左") : QStringLiteral("右"))
                         .arg(m_slots.at(m_currentIndex).slot),
                     60000);
        });
    });
}

void LaserCalibrationController::connectReceiver()
{
    setState(State::ConnectingReceiver,
             QStringLiteral("正在连接接收机信息端口 %1:%2")
                 .arg(m_receiverHost).arg(m_receiverPort));
    m_receiverAttempts = 0;
    m_receiverReconnectAttempts = 0;
    m_receiverSocket.abort();
    m_receiverSocket.connectToHost(m_receiverHost,
                                   static_cast<quint16>(m_receiverPort));
    m_receiverRetryTimer.start();
}

void LaserCalibrationController::requestReceiverInfo()
{
    if (!m_running || m_state != State::ReadingDeviceInfo)
        return;

    // $$SI 查询硬件号；文档规定 log versionb 查询 SN/版本报文。
    // 首次清理当前 TCP 端口的旧日志订阅，后续只补查尚未收到的字段。
    if (m_receiverAttempts == 0) {
        sendReceiver("unlogall thisport\r\n$$SI\r\n");
        sendReceiver("log versionb\r\n");
    } else {
        if (m_currentHardware.isEmpty())
            sendReceiver("$$SI\r\n");
        if (!m_receiverSnReceived)
            sendReceiver("log versionb\r\n");
    }
    ++m_receiverAttempts;

    // 真实 TCP 偶尔丢包或拆包。读取阶段每 5 秒轻量重询，状态离开后停止。
    QTimer::singleShot(5000, this, [this] {
        if (m_running && m_state == State::ReadingDeviceInfo
            && (m_currentHardware.isEmpty() || !m_receiverSnReceived)) {
            appendLog(QStringLiteral(
                "尚未收到完整接收机信息，重新查询 $$SI / log versionb"));
            requestReceiverInfo();
        }
    });
}

void LaserCalibrationController::tryFinishReceiverInfo()
{
    if (m_state != State::ReadingDeviceInfo
        || m_currentHardware.isEmpty() || !m_receiverSnReceived)
        return;
    m_stepTimer.stop();
    emit deviceInfoChanged();
    appendLog(QStringLiteral("接收机信息读取完成，SN=%1，HW=%2，版本=%3")
                  .arg(m_currentSn,
                       m_currentHardware,
                       m_currentVersion.isEmpty() ? QStringLiteral("--")
                                                  : m_currentVersion));
    startPoint1();
}

void LaserCalibrationController::startPoint1()
{
    // 旧版在设备信息读取完成后才开启激光标定结果上报。
    if (!sendReceiver("log laserback ontime 1\r\n")) {
        failCurrentSlot(QStringLiteral("开启接收机激光标定结果上报失败"));
        return;
    }
    if (!sendReceiver("set laser reset calibration\r\n")) {
        failCurrentSlot(QStringLiteral("发送接收机标定复位指令失败"));
        return;
    }
    QTimer::singleShot(500, this, [this] {
        if (m_running && m_state == State::ReadingDeviceInfo)
            moveToPoint(1);
    });
}

void LaserCalibrationController::requestPlanarCalibration(int face)
{
    m_currentFace = face;
    emit faceChanged();
    if (!sendReceiver("set laser planar calibration\r\n")) {
        failCurrentSlot(QStringLiteral("发送面校准指令失败"));
        return;
    }
    appendLog(QStringLiteral("[面校准] 面 %1 指令发送成功，开始等待 $LASERBACK 结果")
                  .arg(face));
    // WaitingFace2 后面紧接 WaitingPoint3，枚举并非完全等距，显式映射。
    State target = State::WaitingFace1;
    if (face == 2) target = State::WaitingFace2;
    else if (face == 3) target = State::WaitingFace3;
    else if (face == 4) target = State::WaitingFace4;
    else if (face == 5) target = State::WaitingFace5;
    else if (face == 6) target = State::WaitingFace6;
    setState(target, QStringLiteral("等待接收机完成面 %1 校准").arg(face),
             kFaceTimeoutMs);
}

void LaserCalibrationController::moveToPoint(int point)
{
    if (!m_armReady) {
        failCurrentSlot(QStringLiteral("机械臂未就绪，无法移动到标定点 %1").arg(point));
        return;
    }
    sendArm(QStringLiteral("$$MT,NEXT%1\r\n").arg(point).toLatin1());
    State target = State::WaitingPoint1;
    if (point == 2) target = State::WaitingPoint2;
    else if (point == 3) target = State::WaitingPoint3;
    else if (point == 4) target = State::WaitingPoint4;
    else if (point == 5) target = State::WaitingPoint5;
    else if (point == 6) target = State::WaitingPoint6;
    else if (point == 7) target = State::WaitingPoint7;
    setState(target,
             QStringLiteral("等待机械臂到达激光标定点 %1").arg(point),
             kArmMoveTimeoutMs);
}

void LaserCalibrationController::startVisionMeasurement(VisionTarget target)
{
    if (!m_running)
        return;
    const bool shortFocus = target == VisionTarget::VerticalNear;
    m_visionTarget = target;
    QString name;
    State state = State::MeasuringVerticalNear;
    if (target == VisionTarget::VerticalNear) {
        name = QStringLiteral("垂直近点");
        state = State::MeasuringVerticalNear;
    } else if (target == VisionTarget::VerticalFar) {
        name = QStringLiteral("垂直远点");
        state = State::MeasuringVerticalFar;
    } else if (target == VisionTarget::HorizontalFar) {
        name = QStringLiteral("水平远点");
        state = State::MeasuringHorizontalFar;
    } else {
        failCurrentSlot(QStringLiteral("视觉测量目标无效"));
        return;
    }

    m_cameraStatus = QStringLiteral("正在使用%1摄像头测量%2")
                         .arg(shortFocus ? QStringLiteral("近焦")
                                         : QStringLiteral("远焦"),
                              name);
    emit cameraChanged();
    setState(state, QStringLiteral("正在进行%1视觉测量").arg(name),
             kVisionTimeoutMs);
    appendLog(QStringLiteral("启动%1视觉测量，沿用旧版 1920×1080 摄像头与参考物参数")
                  .arg(name));
    if (!m_vision.requestMeasurement(shortFocus)) {//开始测量
        failCurrentSlot(QStringLiteral("摄像头正在处理上一项测量，请稍后重试"));
        return;
    }
    emit cameraChanged();//发送信号
}

void LaserCalibrationController::handleVisionMeasurement(
    bool success, double height, const QString &error,
    const QString &imageDataUrl, int cameraIndex)
{
    if (!imageDataUrl.isEmpty())
        m_cameraImageUrl = imageDataUrl;
    emit cameraChanged();
    if (!m_running || m_visionTarget == VisionTarget::None)
        return;

    const VisionTarget completedTarget = m_visionTarget;
    const bool expectedState =
        (completedTarget == VisionTarget::VerticalNear
         && m_state == State::MeasuringVerticalNear)
        || (completedTarget == VisionTarget::VerticalFar
            && m_state == State::MeasuringVerticalFar)
        || (completedTarget == VisionTarget::HorizontalFar
            && m_state == State::MeasuringHorizontalFar);
    if (!expectedState)
        return;
    m_stepTimer.stop();
    m_visionTarget = VisionTarget::None;
    if (!success) {
        appendLog(QStringLiteral("[视觉算法] 本次测量失败：%1").arg(error));
        m_cameraStatus = QStringLiteral("视觉测量失败: %1").arg(error);
        emit cameraChanged();
        failCurrentSlot(m_cameraStatus);
        return;
    }
    // Simulation mode validates camera opening and the complete state machine,
    // but does not depend on a bench target or receiver ranging values.
    if (m_vision.singleCameraDebug()) {
        if (completedTarget == VisionTarget::VerticalNear)
            m_laserDistance = 400.0;
        else
            m_laserDistance = 900.0;
    }
    if (!std::isfinite(m_laserDistance) || m_laserDistance <= 0.0) {
        appendLog(QStringLiteral(
            "[视觉/测距校验] 图像算法已成功，但激光测距值无效：%1")
                      .arg(m_laserDistance, 0, 'f', 3));
        m_cameraStatus = QStringLiteral("视觉测量成功，但未收到有效激光测距报文");
        emit cameraChanged();
        failCurrentSlot(m_cameraStatus);
        return;
    }

    QString name;
    if (completedTarget == VisionTarget::VerticalNear) {
        name = QStringLiteral("垂直近点");
        m_verticalCloseRawHeight = height;
        m_verticalCloseRange = m_laserDistance;
    } else if (completedTarget == VisionTarget::VerticalFar) {
        name = QStringLiteral("垂直远点");
        m_verticalFarRawHeight = height;
        m_verticalFarRange = m_laserDistance;
    } else {
        name = QStringLiteral("水平远点");
        m_horizontalFarRawHeight = height;
        m_horizontalFarRange = m_laserDistance;
    }
    m_laserHeight = height;
    m_cameraStatus = QStringLiteral("%1测量完成：高度 %2 mm，距离 %3 mm，摄像头 %4")
                         .arg(name)
                         .arg(height, 0, 'f', 3)
                         .arg(m_laserDistance, 0, 'f', 3)
                         .arg(cameraIndex);
    emit measurementChanged();
    emit cameraChanged();
    appendLog(m_cameraStatus);

    if (completedTarget == VisionTarget::VerticalNear) {
        // 移动到远点前先停止连续测距，避免机械臂运动中的距离污染远点结果。
        sendReceiver("log laserrangingb ontime 0\r\n");
        moveToPoint(3);
    } else if (completedTarget == VisionTarget::VerticalFar) {
        // 远点数据已锁定，后续面校准期间不需要连续测距。
        sendReceiver("log laserrangingb ontime 0\r\n");
        appendLog(QStringLiteral(
            "NEXT3 仅用于垂直远点视觉测量；测量完成，发送 NEXT4，"
            "待机械臂回复 MT,OK 后执行面 3 校准"));
        moveToPoint(4);
    } else {
        QString reason;
        if (!calculateAngleInputs(&reason)) {    //计算角度参数
            failCurrentSlot(reason);
            return;
        }
        // 旧版在发送六参数角度标定前关闭激光与连续测距。
        sendReceiver("set laser off\r\n");
        sendReceiver("log laserrangingb ontime 0\r\n");
        startAngleCalibration();
    }
}

bool LaserCalibrationController::calculateAngleInputs(QString *reason)
{
    const double rangeDifference = std::abs(m_verticalFarRange
                                             - m_verticalCloseRange);
    appendLog(QStringLiteral(
        "[角度算法] 原始输入：垂直近点(视觉=%1,测距=%2)，垂直远点(视觉=%3,测距=%4)，水平远点(视觉=%5,测距=%6)，水平线高=%7")
                  .arg(m_verticalCloseRawHeight, 0, 'f', 3)
                  .arg(m_verticalCloseRange, 0, 'f', 3)
                  .arg(m_verticalFarRawHeight, 0, 'f', 3)
                  .arg(m_verticalFarRange, 0, 'f', 3)
                  .arg(m_horizontalFarRawHeight, 0, 'f', 3)
                  .arg(m_horizontalFarRange, 0, 'f', 3)
                  .arg(m_horizontalLineHeight, 0, 'f', 3));
    if (m_verticalCloseRawHeight <= 0.0 || m_verticalFarRawHeight <= 0.0
        || m_horizontalFarRawHeight <= 0.0 || m_verticalCloseRange <= 0.0
        || m_verticalFarRange <= 0.0 || m_horizontalFarRange <= 0.0) {
        if (reason)
            *reason = QStringLiteral("视觉或激光测距参数不完整，不能执行角度校准");
        return false;
    }
    if (rangeDifference < 1.0) {
        if (reason)
            *reason = QStringLiteral("垂直近、远点激光距离过于接近，无法计算近点高度");
        return false;
    }

    // 完全沿用旧版 GRR20 计算：水平近点使用设备类型对应的投线高度；
    // 垂直近点由近、远两次视觉高度和激光距离外推，反向场景保留 -7 mm 补偿。
    const double sinB = std::abs(m_verticalCloseRange) / rangeDifference;
    const double heightDifference = std::abs(m_verticalFarRawHeight
                                               - m_verticalCloseRawHeight);
    appendLog(QStringLiteral(
        "[角度算法] 中间量计算成功：距离差=%1，高度差=%2，sinB=%3")
                  .arg(rangeDifference, 0, 'f', 3)
                  .arg(heightDifference, 0, 'f', 3)
                  .arg(sinB, 0, 'f', 6));
    m_horizontalNear = m_horizontalLineHeight;
    m_horizontalFar = m_horizontalFarRawHeight;
    m_horizontalLength = m_horizontalFarRange;
    if (m_verticalFarRawHeight > m_verticalCloseRawHeight) {
        m_verticalNear = m_verticalCloseRawHeight - heightDifference * sinB;
    } else {
        m_verticalNear = m_verticalCloseRawHeight + heightDifference * sinB - 7.0;
    }
    m_verticalFar = m_verticalFarRawHeight;
    m_verticalLength = m_verticalFarRange;
    const double values[] = {m_horizontalNear, m_horizontalFar,
                             m_horizontalLength, m_verticalNear,
                             m_verticalFar, m_verticalLength};
    for (double value : values) {
        if (!std::isfinite(value)) {
            if (reason)
                *reason = QStringLiteral("角度校准计算结果包含无效数字");
            return false;
        }
    }
    emit measurementChanged(); //得到最终六个参数
    appendLog(QStringLiteral(
        "角度校准输入完成：水平[%1,%2,%3]，垂直[%4,%5,%6]")
                  .arg(m_horizontalNear, 0, 'f', 3)
                  .arg(m_horizontalFar, 0, 'f', 3)
                  .arg(m_horizontalLength, 0, 'f', 3)
                  .arg(m_verticalNear, 0, 'f', 3)
                  .arg(m_verticalFar, 0, 'f', 3)
                  .arg(m_verticalLength, 0, 'f', 3));
    return true;
}

void LaserCalibrationController::startAngleCalibration()
{
    const QString command = QStringLiteral(
        "set laser angle calibration %1 %2 %3 %4 %5 %6\r\n")
        .arg(m_horizontalNear, 0, 'g', 15)
        .arg(m_horizontalFar, 0, 'g', 15)
        .arg(m_horizontalLength, 0, 'g', 15)
        .arg(m_verticalNear, 0, 'g', 15)
        .arg(m_verticalFar, 0, 'g', 15)
        .arg(m_verticalLength, 0, 'g', 15);
    if (!sendReceiver(command.toUtf8())) {  //发送完整laser参数
        failCurrentSlot(QStringLiteral("发送激光角度校准指令失败"));
        return;
    }
    appendLog(QStringLiteral("[角度校准] 六参数指令发送成功，等待接收机返回最终 pitch/yaw/bias"));
    setState(State::WaitingAngle, QStringLiteral("等待激光角度校准最终结果"),
             kAngleTimeoutMs);

    if (m_vision.singleCameraDebug()) {
        appendLog(QStringLiteral(
            "模拟测试：使用默认外参 pitch=0.125000, yaw=-0.250000, "
            "bias=[0.001,-0.002,0.003,-0.004,0.005,-0.006]"));
        QTimer::singleShot(500, this, [this] {
            if (!m_running || m_state != State::WaitingAngle)
                return;
            if (m_currentHardware.isEmpty())
                m_currentHardware = QStringLiteral("SIM-HW");
            if (m_currentVersion.isEmpty())
                m_currentVersion = QStringLiteral("SIM-V1.0");
            emit deviceInfoChanged();
            LaserBack report;
            report.valid = true;
            report.sideState = 1;
            report.panelId = 6;
            report.pitch = 0.125;
            report.yaw = -0.250;
            report.bias = QVector<double>{0.001, -0.002, 0.003,
                                          -0.004, 0.005, -0.006};
            report.errorCode = 0;
            handleLaserBack(report);
        });
    }
}

void LaserCalibrationController::completeCurrentSlot(bool success, const QString &reason)
{
    m_stepTimer.stop();
    m_visionTarget = VisionTarget::None;
    m_vision.cancelPendingResult();
    m_wifiTimer.stop();
    m_receiverRetryTimer.stop();
    m_pendingSuccess = success;
    m_pendingReason = reason;
    if (!m_currentResultRecorded) {
        updateSlotStatus(m_slots.at(m_currentIndex).tray,
                         m_slots.at(m_currentIndex).slot,
                         success ? QStringLiteral("success") : QStringLiteral("failed"));
        addResult(success, reason);//保存数据
        m_currentResultRecorded = true;
    }
    m_receiverSocket.disconnectFromHost();
    if (m_wifiConnected) {
        m_wifiConnected = false;
        emit wifiConnectedChanged();
    }
    if (success)
        appendLog(QStringLiteral("当前槽位激光标定成功"));
    else
        appendLog(QStringLiteral("当前槽位激光标定失败: %1").arg(reason));
    QTimer::singleShot(300, this, [this] {
        if (!m_running)
            return;
        // 机械臂已断线时绝不能继续发下一槽位命令。否则后续槽位会被误标为
        // “扫码超时”，同时机械臂实际位置也未知，存在碰撞风险。
        if (!m_armReady) {
            m_running = false;
            m_paused = false;
            m_currentIndex = -1;
            m_state = State::Idle;
            emit runningChanged();
            emit pausedChanged();
            emit resultsChanged();
            setError(QStringLiteral("机械臂连接已中断，批次已停止；重新连接后可重新开始"));
            setProgress(QStringLiteral("激光标定批次已安全停止"));
            appendLog(QStringLiteral("机械臂未就绪，未启动下一槽位"));
            return;
        }
        beginNextSlot();
    });
}

void LaserCalibrationController::failCurrentSlot(const QString &reason)
{
    if (!m_running || m_state == State::Aborting)
        return;
    if (m_state == State::WaitingPutback) {
        completeCurrentSlot(false, m_pendingReason.isEmpty() ? reason : m_pendingReason);
        return;
    }
    setError(reason);
    m_visionTarget = VisionTarget::None;
    m_vision.cancelPendingResult();
    emit cameraChanged();
    appendLog(QStringLiteral("当前槽位失败，执行激光关闭和设备放回: %1").arg(reason));
    ensureReceiverSafe();
    m_pendingSuccess = false;
    m_pendingReason = reason;
    if (m_armReady) {
        sendArm("$$MT,FAILED\r\n");
        setState(State::WaitingPutback, QStringLiteral("当前槽位失败，等待机械臂放回设备"),
                 kArmMoveTimeoutMs);
    } else {
        completeCurrentSlot(false, reason);
    }
}

void LaserCalibrationController::finishBatch()
{
    m_stepTimer.stop();
    m_visionTarget = VisionTarget::None;
    m_vision.cancelPendingResult();
    m_wifiTimer.stop();
    m_receiverRetryTimer.stop();
    ensureReceiverSafe();
    m_receiverSocket.disconnectFromHost();
    m_running = false;
    m_paused = false;
    m_currentIndex = -1;
    m_state = State::Idle;
    emit runningChanged();
    emit pausedChanged();
    emit resultsChanged();
    const QString summary = QStringLiteral("全部激光标定完成：成功 %1，失败 %2")
                                .arg(successCount()).arg(failureCount());
    setProgress(summary);
    appendLog(summary);
}

void LaserCalibrationController::ensureReceiverSafe()
{
    if (m_receiverSocket.state() == QAbstractSocket::ConnectedState) {
        sendReceiver("set laser off\r\n");
        sendReceiver("log laserrangingb ontime 0\r\n");
        sendReceiver("unlog laserback\r\n");
    }
}

void LaserCalibrationController::finishAbort()
{
    m_stepTimer.stop();
    m_visionTarget = VisionTarget::None;
    m_vision.cancelPendingResult();
    if (m_currentIndex >= 0 && m_currentIndex < m_slots.size()
        && !m_currentResultRecorded) {
        updateSlotStatus(m_slots.at(m_currentIndex).tray,
                         m_slots.at(m_currentIndex).slot,
                         QStringLiteral("failed"));
        addResult(false, QStringLiteral("用户终止任务"));
        m_currentResultRecorded = true;
    }
    m_receiverSocket.abort();
    m_running = false;
    m_paused = false;
    m_state = State::Idle;
    emit runningChanged();
    emit pausedChanged();
    setError(QStringLiteral("任务已终止"));
    setProgress(QStringLiteral("任务已终止，机械臂连接可继续使用"));
}

void LaserCalibrationController::continueIfNotPaused(const std::function<void()> &action)
{
    if (!m_running)
        return;
    if (!m_paused) {
        action();
        return;
    }
    setProgress(QStringLiteral("任务已暂停，等待继续"));
    QTimer::singleShot(250, this, [this, action] {
        continueIfNotPaused(action);
    });
}

void LaserCalibrationController::updateSlotStatus(const QString &tray, int slot,
                                                   const QString &status)
{
    for (Slot &item : m_slots) {
        if (item.tray == tray && item.slot == slot) {
            item.status = status;
            ++m_slotRevision;
            emit slotRevisionChanged();
            emit resultsChanged();
            return;
        }
    }
}

void LaserCalibrationController::addResult(bool success, const QString &reason)
{
    if (m_currentIndex < 0 || m_currentIndex >= m_slots.size())
        return;
    const Slot &slot = m_slots.at(m_currentIndex);
    QVariantMap row;
    row.insert(QStringLiteral("time"),
               QDateTime::currentDateTime().toString(Qt::ISODate));
    row.insert(QStringLiteral("tray"), slot.tray);
    row.insert(QStringLiteral("trayText"),
               slot.tray == QStringLiteral("left") ? QStringLiteral("左")
                                                    : QStringLiteral("右"));
    row.insert(QStringLiteral("slot"), slot.slot);
    row.insert(QStringLiteral("sn"), m_currentSn);
    row.insert(QStringLiteral("hardware"), m_currentHardware);
    row.insert(QStringLiteral("version"), m_currentVersion);
    row.insert(QStringLiteral("distance"), m_laserDistance);
    row.insert(QStringLiteral("height"), m_laserHeight);
    row.insert(QStringLiteral("pitch"), m_finalPitch);
    row.insert(QStringLiteral("yaw"), m_finalYaw);
    row.insert(QStringLiteral("success"), success);
    row.insert(QStringLiteral("result"), success ? QStringLiteral("成功")
                                                 : QStringLiteral("失败"));
    row.insert(QStringLiteral("error"),
               success && m_vision.singleCameraDebug()
                   ? QStringLiteral("模拟测试默认数据，非真实标定结果")
                   : reason);
    QVariantList bias;
    for (double value : m_finalBias)
        bias.append(value);
    row.insert(QStringLiteral("bias"), bias);
    m_results.append(row);
    saveResultToDatabase(row); //保存数据
    emit resultsChanged();
}

bool LaserCalibrationController::initializeDatabase()
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty())
        return false;
    QDir().mkpath(root);
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                           m_databaseConnectionName);
    m_database.setDatabaseName(QDir(root).filePath(QStringLiteral("laser_calibration.db")));
    if (!m_database.open()) {
        appendLog(QStringLiteral("激光标定数据库打开失败: %1")
                      .arg(m_database.lastError().text()));
        return false;
    }
    QSqlQuery query(m_database);
    const bool ok = query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS laser_results ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, time TEXT NOT NULL, tray TEXT, slot INTEGER,"
        "sn TEXT, hardware TEXT, version TEXT, distance REAL, height REAL,"
        "pitch REAL, yaw REAL, success INTEGER, error TEXT, bias TEXT)"));
    if (!ok)
        appendLog(QStringLiteral("创建激光标定结果表失败: %1").arg(query.lastError().text()));
    return ok;
}

void LaserCalibrationController::saveResultToDatabase(const QVariantMap &row)
{
    if (!m_database.isOpen())
        return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT INTO laser_results(time,tray,slot,sn,hardware,version,distance,height,"
        "pitch,yaw,success,error,bias) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(row.value(QStringLiteral("time")));
    query.addBindValue(row.value(QStringLiteral("tray")));
    query.addBindValue(row.value(QStringLiteral("slot")));
    query.addBindValue(row.value(QStringLiteral("sn")));
    query.addBindValue(row.value(QStringLiteral("hardware")));
    query.addBindValue(row.value(QStringLiteral("version")));
    query.addBindValue(row.value(QStringLiteral("distance")));
    query.addBindValue(row.value(QStringLiteral("height")));
    query.addBindValue(row.value(QStringLiteral("pitch")));
    query.addBindValue(row.value(QStringLiteral("yaw")));
    query.addBindValue(row.value(QStringLiteral("success")).toBool() ? 1 : 0);
    query.addBindValue(row.value(QStringLiteral("error")));
    query.addBindValue(QString::fromUtf8(QJsonDocument::fromVariant(
        row.value(QStringLiteral("bias"))).toJson(QJsonDocument::Compact)));
    if (!query.exec())
        appendLog(QStringLiteral("保存激光标定数据库失败: %1").arg(query.lastError().text()));
}

QString LaserCalibrationController::defaultExportPath() const
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (root.isEmpty())
        root = QDir::homePath();
    root = QDir(root).filePath(QStringLiteral("Laser_calibration"));
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("laser_results_%1.csv")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
}

bool LaserCalibrationController::exportResults(const QString &filePath)
{
    if (m_results.isEmpty()) {
        setError(QStringLiteral("当前没有可导出的激光标定结果"));
        return false;
    }
    QString target = filePath.trimmed();
    if (target.startsWith(QStringLiteral("file:")))
        target = QUrl(target).toLocalFile();
    if (target.isEmpty())
        target = defaultExportPath();
    QFile file(target);
    QDir().mkpath(QFileInfo(file).absolutePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        setError(QStringLiteral("导出结果失败: %1").arg(file.errorString()));
        return false;
    }
    file.write("\xEF\xBB\xBF");
    QTextStream out(&file);
    out.setCodec("UTF-8");
    out << "time,tray,slot,sn,hardware,version,distance_mm,height_mm,pitch,yaw,result,error,BiasBuf\r\n";
    for (const QVariant &item : m_results) {
        const QVariantMap row = item.toMap();
        out << csvCell(row.value(QStringLiteral("time")).toString()) << ','
            << csvCell(row.value(QStringLiteral("trayText")).toString()) << ','
            << row.value(QStringLiteral("slot")).toInt() << ','
            << csvCell(row.value(QStringLiteral("sn")).toString()) << ','
            << csvCell(row.value(QStringLiteral("hardware")).toString()) << ','
            << csvCell(row.value(QStringLiteral("version")).toString()) << ','
            << QString::number(row.value(QStringLiteral("distance")).toDouble(), 'f', 3) << ','
            << QString::number(row.value(QStringLiteral("height")).toDouble(), 'f', 3) << ','
            << QString::number(row.value(QStringLiteral("pitch")).toDouble(), 'f', 8) << ','
            << QString::number(row.value(QStringLiteral("yaw")).toDouble(), 'f', 8) << ','
            << csvCell(row.value(QStringLiteral("result")).toString()) << ','
            << csvCell(row.value(QStringLiteral("error")).toString()) << ','
            << csvCell(QString::fromUtf8(QJsonDocument::fromVariant(
                   row.value(QStringLiteral("bias"))).toJson(QJsonDocument::Compact)))
            << "\r\n";
    }
    file.close();
    appendLog(QStringLiteral("激光标定结果已导出: %1").arg(target));
    setProgress(QStringLiteral("结果已导出到 %1").arg(target));
    return true;
}

QVariantList LaserCalibrationController::queryResults(const QString &sn) const
{
    QVariantList rows;
    if (!m_database.isOpen())
        return rows;

    QSqlQuery query(m_database);
    const QString normalizedSn = sn.trimmed();
    if (normalizedSn.isEmpty()) {
        query.prepare(QStringLiteral(
            "SELECT id,time,tray,slot,sn,hardware,version,distance,height,pitch,yaw,"
            "success,error,bias FROM laser_results ORDER BY id DESC"));
    } else {
        query.prepare(QStringLiteral(
            "SELECT id,time,tray,slot,sn,hardware,version,distance,height,pitch,yaw,"
            "success,error,bias FROM laser_results WHERE sn LIKE ? ORDER BY id DESC"));
        query.addBindValue(QStringLiteral("%") + normalizedSn + QStringLiteral("%"));
    }
    if (!query.exec())
        return rows;

    while (query.next()) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), query.value(0));
        row.insert(QStringLiteral("time"), query.value(1));
        const QString tray = query.value(2).toString();
        row.insert(QStringLiteral("tray"), tray);
        row.insert(QStringLiteral("trayText"),
                   tray == QStringLiteral("left") ? QStringLiteral("左")
                                                    : QStringLiteral("右"));
        row.insert(QStringLiteral("slot"), query.value(3));
        row.insert(QStringLiteral("sn"), query.value(4));
        row.insert(QStringLiteral("hardware"), query.value(5));
        row.insert(QStringLiteral("version"), query.value(6));
        row.insert(QStringLiteral("distance"), query.value(7));
        row.insert(QStringLiteral("height"), query.value(8));
        row.insert(QStringLiteral("pitch"), query.value(9));
        row.insert(QStringLiteral("yaw"), query.value(10));
        row.insert(QStringLiteral("success"), query.value(11).toInt() != 0);
        row.insert(QStringLiteral("result"), query.value(11).toInt() != 0
                       ? QStringLiteral("成功") : QStringLiteral("失败"));
        row.insert(QStringLiteral("error"), query.value(12));
        row.insert(QStringLiteral("bias"), query.value(13));
        rows.append(row);
    }
    return rows;
}

QString LaserCalibrationController::normalizeSn(const QString &value)
{
    QString candidate = value.trimmed();
    candidate.remove(QStringLiteral("$*"));
    candidate.remove('\r');
    candidate.remove('\n');
    if (candidate.startsWith(QStringLiteral("SNFF"), Qt::CaseInsensitive))
        candidate = candidate.mid(4);
    static const QRegularExpression token(QStringLiteral("([A-Za-z][A-Za-z0-9_-]{6,})"));
    const QRegularExpressionMatch match = token.match(candidate);
    if (!match.hasMatch())
        return QString();
    const QString result = match.captured(1);
    if (result.contains(QStringLiteral("MT"), Qt::CaseInsensitive)
        && result.contains(','))
        return QString();
    return result;
}

bool LaserCalibrationController::finiteNumber(const QVariant &value, double *result)
{
    bool ok = false;
    const double number = value.toString().trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(number))
        return false;
    if (result)
        *result = number;
    return true;
}

QString LaserCalibrationController::connectedWifiName() const
{
#ifdef Q_OS_WIN
    HANDLE clientHandle = nullptr;
    DWORD version = 0;
    if (WlanOpenHandle(2, nullptr, &version, &clientHandle) != ERROR_SUCCESS)
        return QString();
    QString result;
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(clientHandle, nullptr, &interfaces) == ERROR_SUCCESS
        && interfaces) {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            WLAN_OPCODE_VALUE_TYPE opcodeType;
            PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
            DWORD size = 0;
            if (WlanQueryInterface(clientHandle,
                                   &interfaces->InterfaceInfo[i].InterfaceGuid,
                                   wlan_intf_opcode_current_connection,
                                   nullptr, &size,
                                   reinterpret_cast<PVOID *>(&attributes),
                                   &opcodeType) == ERROR_SUCCESS && attributes) {
                const DOT11_SSID &ssid = attributes->wlanAssociationAttributes.dot11Ssid;
                result = QString::fromUtf8(reinterpret_cast<const char *>(ssid.ucSSID),
                                           static_cast<int>(ssid.uSSIDLength));
                WlanFreeMemory(attributes);
                if (!result.isEmpty())
                    break;
            }
        }
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(clientHandle, nullptr);
    return result;
#else
    return QString();
#endif
}

bool LaserCalibrationController::receiverNetworkReady(QString *localAddress) const
{
    const QHostAddress receiver(m_receiverHost);
    const quint32 receiverIpv4 = receiver.toIPv4Address();
    if (receiverIpv4 == 0)
        return false;

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &networkInterface : interfaces) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp)
            || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
            const quint32 localIpv4 = entry.ip().toIPv4Address();
            const quint32 mask = entry.netmask().toIPv4Address();
            if (localIpv4 == 0 || mask == 0)
                continue;
            if ((localIpv4 & mask) == (receiverIpv4 & mask)) {
                if (localAddress)
                    *localAddress = entry.ip().toString();
                return true;
            }
        }
    }
    return false;
}

void LaserCalibrationController::connectWifiByNamePart(const QString &namePart)
{
#ifdef Q_OS_WIN
    HANDLE clientHandle = nullptr;
    DWORD version = 0;
    DWORD result = WlanOpenHandle(2, nullptr, &version, &clientHandle);
    if (result != ERROR_SUCCESS) {
        appendLog(QStringLiteral("无法打开 Windows Wi-Fi 服务，错误码 %1").arg(result));
        return;
    }
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    result = WlanEnumInterfaces(clientHandle, nullptr, &interfaces);
    if (result != ERROR_SUCCESS || !interfaces || interfaces->dwNumberOfItems == 0) {
        appendLog(QStringLiteral("没有可用无线网卡"));
        if (interfaces) WlanFreeMemory(interfaces);
        WlanCloseHandle(clientHandle, nullptr);
        return;
    }

    bool found = false;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems && !found; ++i) {
        const WLAN_INTERFACE_INFO &interfaceInfo = interfaces->InterfaceInfo[i];
        WlanScan(clientHandle, &interfaceInfo.InterfaceGuid, nullptr, nullptr, nullptr);
        PWLAN_AVAILABLE_NETWORK_LIST networks = nullptr;
        if (WlanGetAvailableNetworkList(clientHandle, &interfaceInfo.InterfaceGuid,
                                        0, nullptr, &networks) != ERROR_SUCCESS
            || !networks) {
            continue;
        }
        for (DWORD n = 0; n < networks->dwNumberOfItems; ++n) {
            WLAN_AVAILABLE_NETWORK &network = networks->Network[n];
            const QString ssid = QString::fromUtf8(
                reinterpret_cast<const char *>(network.dot11Ssid.ucSSID),
                static_cast<int>(network.dot11Ssid.uSSIDLength));
            const QString profile = QString::fromWCharArray(network.strProfileName);
            if (!ssid.contains(namePart, Qt::CaseInsensitive)
                && !profile.contains(namePart, Qt::CaseInsensitive)) {
                continue;
            }
            found = true;
            WLAN_CONNECTION_PARAMETERS parameters = {};
            QString profileStorage;
            parameters.dot11BssType = network.dot11BssType;
            if (!profile.isEmpty()) {
                parameters.wlanConnectionMode = wlan_connection_mode_profile;
                parameters.strProfile = network.strProfileName;
            } else if (network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_80211_OPEN
                       && network.dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_NONE) {
                DWORD reasonCode = 0;
                if (!installOpenWifiProfile(clientHandle, interfaceInfo.InterfaceGuid,
                                            ssid, &reasonCode)) {
                    appendLog(QStringLiteral("创建开放热点配置失败: %1（%2）")
                                  .arg(ssid).arg(reasonCode));
                    found = false;
                    continue;
                }
                profileStorage = ssid;
                parameters.wlanConnectionMode = wlan_connection_mode_profile;
                parameters.strProfile = reinterpret_cast<LPCWSTR>(profileStorage.utf16());
            } else {
                appendLog(QStringLiteral("找到热点 %1，但 Windows 没有保存密码").arg(ssid));
                found = false;
                continue;
            }
            result = WlanConnect(clientHandle, &interfaceInfo.InterfaceGuid,
                                 &parameters, nullptr);
            appendLog(result == ERROR_SUCCESS
                          ? QStringLiteral("已发起 Wi-Fi 连接: %1").arg(ssid)
                          : QStringLiteral("发起 Wi-Fi 连接失败: %1（%2）")
                                .arg(ssid).arg(result));
            break;
        }
        WlanFreeMemory(networks);
    }
    if (!found)
        appendLog(QStringLiteral("暂未找到名称包含 %1 的接收机 Wi-Fi").arg(namePart));
    WlanFreeMemory(interfaces);
    WlanCloseHandle(clientHandle, nullptr);
#else
    Q_UNUSED(namePart)
    appendLog(QStringLiteral("自动连接 Wi-Fi 仅支持 Windows"));
#endif
}
