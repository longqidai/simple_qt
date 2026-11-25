#include "lasercalibrationcontroller.h"
#include "audiofeedback.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSettings>
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
constexpr int kVisionTimeoutMs = 150000;
constexpr int kMaximumArmBufferBytes = 10 * 1024;

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

// 解析 TcpController 写出的标准 CSV 行，兼容字段内的逗号和双引号。
QStringList parseCsvLine(const QString &line)
{
    QStringList fields;
    QString field;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"')) {
            if (quoted && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                field.append(QLatin1Char('"'));
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == QLatin1Char(',') && !quoted) {
            fields.append(field);
            field.clear();
        } else {
            field.append(ch);
        }
    }
    fields.append(field);
    return fields;
}
}

LaserCalibrationController::LaserCalibrationController(QObject *parent)
    : QObject(parent)
    , m_databaseConnectionName(QStringLiteral("laser-calibration-%1")
                                   .arg(reinterpret_cast<quintptr>(this)))
{
    QSettings settings;
    const QString savedArmHost = settings.value(QStringLiteral("arm/host")).toString().trimmed();
    const int savedArmPort = settings.value(QStringLiteral("arm/port"), m_armPort).toInt();
    if (!savedArmHost.isEmpty())
        m_armHost = savedArmHost;
    if (savedArmPort > 0 && savedArmPort <= 65535)
        m_armPort = savedArmPort;

    configureSockets();
    configureTimers();


    //视觉算法信号槽
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
QString LaserCalibrationController::armHost() const { return m_armHost; }
int LaserCalibrationController::armPort() const { return m_armPort; }
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
        : QStringLiteral("正式单远焦摄像头模式：近点固定，实测两个远点");
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
        m_armBuffer.append(m_armSocket.readAll());//把机械臂 TCP Socket 当前已经收到的全部数据追加到接收缓冲区 m_armBuffer 后面
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
            setError(QStringLiteral("连接机械臂 %1:%2 失败: %3")
                         .arg(m_armHost).arg(m_armPort).arg(m_armSocket.errorString()));
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
        const QByteArray received = m_receiverSocket.readAll();
        m_receiverBuffer.append(received);
        if (m_faceDiagnosticsActive && m_running && !received.isEmpty()) {
            m_faceRxBytes += received.size();
            ++m_faceRxChunks;
            m_faceLastRx = received.right(256);
            // TCP 一次 readyRead 可能包含半条或多条报文，不能按收包次数推断报文数。
            // 限制日志频率和长度，避免周期上报刷满界面。
            if (m_faceRxChunks <= 8 || m_faceRxChunks % 20 == 0) {
                QString preview = QString::fromLatin1(received.left(256));
                preview.replace('\r', QStringLiteral("<CR>"));
                preview.replace('\n', QStringLiteral("<LF>"));
                preview.replace(QChar(0), QStringLiteral("<NUL>"));
                appendLog(QStringLiteral("[接收机原始收包] 面=%1，第%2块，本次=%3字节，累计=%4字节，缓存=%5字节；预览(最多256字节)=%6；HEX=%7")
                              .arg(m_currentFace).arg(m_faceRxChunks).arg(received.size())
                              .arg(m_faceRxBytes).arg(m_receiverBuffer.size()).arg(preview)
                              .arg(QString::fromLatin1(received.left(256).toHex(' '))));
            }
        }
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
        if (m_running && m_state != State::WaitingReturnHome
            && m_state != State::WaitingPutback
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
        if (m_running && m_faceDiagnosticsActive) {
            appendLog(QStringLiteral("[面校准超时诊断] 面=%1，TCP状态=%2，待发送=%3字节，收到=%4字节/%5块，有效LASERBACK=%6，解析失败=%7，缓存剩余=%8字节")
                          .arg(m_currentFace).arg(int(m_receiverSocket.state()))
                          .arg(m_receiverSocket.bytesToWrite()).arg(m_faceRxBytes)
                          .arg(m_faceRxChunks).arg(m_faceValidReports)
                          .arg(m_faceInvalidReports).arg(m_receiverBuffer.size()));
            appendLog(m_faceRxBytes == 0
                          ? QStringLiteral("[面校准诊断] 本次等待期间TCP没有收到任何字节；需检查设备是否接受指令及结果上报端口，不能据此认定设备未执行。")
                          : m_faceValidReports == 0
                              ? QStringLiteral("[面校准诊断] TCP有数据，但未解析到有效LASERBACK；请检查原始HEX、报文格式和结束符。")
                              : QStringLiteral("[面校准诊断] 已收到有效LASERBACK，但流程未推进；请检查面号、静置状态和错误码。"));
            appendLog(QStringLiteral("[面校准诊断] 缓存头HEX(最多256字节)=%1；最后收包尾HEX(最多256字节)=%2")
                          .arg(QString::fromLatin1(m_receiverBuffer.left(256).toHex(' ')))
                          .arg(QString::fromLatin1(m_faceLastRx.toHex(' '))));
            m_faceDiagnosticsActive = false;
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
            connectReceiver(); //连接设备
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
    const QString requestedHost = host.trimmed().isEmpty() ? m_armHost : host.trimmed();
    const int requestedPort = port > 0 && port <= 65535 ? port : m_armPort;
    if (m_armHost != requestedHost || m_armPort != requestedPort) {
        m_armHost = requestedHost;
        m_armPort = requestedPort;
        emit armEndpointChanged();
    }
    QSettings settings;
    settings.setValue(QStringLiteral("arm/host"), m_armHost);
    settings.setValue(QStringLiteral("arm/port"), m_armPort);
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
    if (!validateMeasurements(measurements, &reason)) {   //检查水平线高
        setError(reason);
        return false;
    }

    m_slots.clear();
    const int maximumSlot = m_deviceModel == QStringLiteral("SLAM") ? 8 : 10;
    auto appendSlots = [this, maximumSlot](const QString &tray, const QVariantList &values) {
        for (const QVariant &value : values) {
            const int slot = value.toInt();
            // GRR激光托盘为2×5，SLAM激光托盘为2×4。后端再次限制范围，
            // 防止QML以外的调用方把SLAM的9、10号槽位下发给机械臂。
            if (slot >= 1 && slot <= maximumSlot)
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
    m_consumedFaceCompletionMask = 0;
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
    AudioFeedback::play(AudioFeedback::Cue::Reset);
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
    // 面校准统计只属于当前面的等待阶段，不能串入后续运动/视觉超时。
    if (state != State::WaitingFace1 && state != State::WaitingFace2
        && state != State::WaitingFace3 && state != State::WaitingFace4
        && state != State::WaitingFace5 && state != State::WaitingFace6)
        m_faceDiagnosticsActive = false;
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

bool LaserCalibrationController::sendArm(const QByteArray &command)
{
    if (m_armSocket.state() != QAbstractSocket::ConnectedState) {
        appendLog(QStringLiteral("机械臂指令未发送（TCP 未连接）: %1")
                      .arg(QString::fromLatin1(command).trimmed()));
        return false;
    }
    const qint64 accepted = m_armSocket.write(command);
    if (accepted != command.size()) {
        appendLog(QStringLiteral("[机械臂发送失败] 写入=%1/%2 字节，错误=%3，指令=%4")
                      .arg(accepted).arg(command.size()).arg(m_armSocket.errorString())
                      .arg(QString::fromLatin1(command).trimmed()));
        return false;
    }
    m_armSocket.flush();
    appendLog(QStringLiteral("发送机械臂: %1")
                  .arg(QString::fromLatin1(command).trimmed()));
    return true;
}

bool LaserCalibrationController::sendReceiver(const QByteArray &command)
{
    if (m_receiverSocket.state() != QAbstractSocket::ConnectedState) {
        appendLog(QStringLiteral("接收机指令未发送（TCP 未连接）: %1")
                      .arg(QString::fromLatin1(command).trimmed()));
        return false;
    }
    const qint64 accepted = m_receiverSocket.write(command);
    if (accepted != command.size()) {
        appendLog(QStringLiteral("[接收机发送失败] 写入=%1/%2字节，错误=%3，指令=%4")
                      .arg(accepted).arg(command.size()).arg(m_receiverSocket.errorString())
                      .arg(QString::fromLatin1(command).trimmed()));
        return false;
    }
    m_receiverSocket.flush();
    appendLog(QStringLiteral("发送接收机: %1")
                  .arg(QString::fromLatin1(command).trimmed()));
    return true;
}

void LaserCalibrationController::processArmBuffer()
{
    // TCP 会拆包或合包；只消费带 $* 或换行结束符的完整帧。
    if (m_armBuffer.size() > kMaximumArmBufferBytes) {
        appendLog(QStringLiteral("机械臂接收缓冲区超过 10 KB，已清空异常数据"));
        m_armBuffer.clear();
        if (m_running)
            failCurrentSlot(QStringLiteral("机械臂报文持续无结束符，协议数据异常"));
        return;
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
            AudioFeedback::play(AudioFeedback::Cue::ConnectArm);
        }
        if (!m_running)
            setState(State::Idle, QStringLiteral("机械臂已就绪，可选择槽位开始标定"));
        return;
    }
    // 此报文表示机械臂无法安全自动复位。停止整批流程，等待人工检查，
    // 绝不能继续向下一个槽位发送运动指令。
    if (frame.contains(QStringLiteral("MANUAL_RECOVERY"), Qt::CaseInsensitive)) {
        const QString reason = QStringLiteral("机械臂要求人工恢复，请检查设备和机械臂位置后重新开始");
        m_stepTimer.stop();
        ensureReceiverSafe();
        setError(reason);
        appendLog(reason);
        if (m_running && m_currentIndex >= 0 && m_currentIndex < m_slots.size()
            && !m_currentResultRecorded) {
            updateSlotStatus(m_slots.at(m_currentIndex).tray,
                             m_slots.at(m_currentIndex).slot,
                             QStringLiteral("failed"));
            addResult(false, reason);
            m_currentResultRecorded = true;
        }
        m_running = false;
        m_paused = false;
        m_state = State::Idle;
        emit runningChanged();
        emit pausedChanged();
        return;
    }
    if (frame.contains(QStringLiteral("FAILED"), Qt::CaseInsensitive)) {
        if (m_state == State::WaitingPutback || m_state == State::Aborting) {
            if (m_state == State::Aborting)
                finishAbort();
            else if (m_currentResultRecorded)
                appendLog(QStringLiteral("已忽略机械臂重复发送的放回结果"));
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
    switch (m_state) {
    case State::WaitingPoint1:
        m_stepTimer.stop();
        requestPlanarCalibration(1);
        break;
    case State::WaitingPoint2:
        m_stepTimer.stop();
        // 这里虽然复用 WaitingPoint2 这个“第二次面校准”的逻辑状态，
        // 但机械臂实际到达的是 NEXT3。旧上位机 wifiTcp_test3 在面1完成后
        // 会跳过 NEXT2，直接在 NEXT3 姿态执行第二次面校准。
        // 到位后先打开激光、开启连续测距并进入自动测量，
        // 再发送第二面的平面校准指令。不能等第二面校准结束后才开激光，
        // 否则接收机执行第二面校准时使用的激光/测距状态与旧上位机不同。
        sendReceiver("set laser on\r\n");
        sendReceiver("log laserrangingb ontime 1\r\n");
        sendReceiver("set laser survey auto\r\n");
        appendLog(QStringLiteral("机械臂已到达第二面姿态，已开启激光和连续测距，开始面 2 校准"));
        requestPlanarCalibration(2);
        break;
    case State::WaitingPoint3:
        m_stepTimer.stop();
        // 旧版在机械臂到达远点后重新开启测距，等待报文稳定后才拍远焦图。
        sendReceiver("log laserrangingb ontime 1\r\n");
        sendReceiver("set laser survey auto\r\n");
        appendLog(QStringLiteral("机械臂已到达垂直远点，重新开启激光测距并等待稳定"));
        QTimer::singleShot(800, this, [this] {
            if (m_running && m_state == State::WaitingPoint3)
                startVisionMeasurement(VisionTarget::VerticalFar);//开启测量
        });
        break;
    case State::WaitingPoint4:
        m_stepTimer.stop();
        appendLog(QStringLiteral("机械臂已到达 NEXT4 校准姿态，开始面 3 校准"));
        requestPlanarCalibration(3);
        break;
    case State::WaitingPoint5:
        m_stepTimer.stop();
        requestPlanarCalibration(4);
        break;
    case State::WaitingPoint6:
        m_stepTimer.stop();
        requestPlanarCalibration(5);
        break;
    case State::WaitingPoint7:
        m_stepTimer.stop();
        requestPlanarCalibration(6);
        break;
    case State::WaitingReturnHome:
        m_stepTimer.stop();
        // 与 wifiTcp_test3 一致：NEXT8 的 OK 只表示机械臂已经从最后一个
        // 标定姿态安全回到 P41/Home。此时才能执行托盘放回动作。
        if (!sendArm("$$MT,SUCCESS\r\n")) {
            failCurrentSlot(QStringLiteral("机械臂已回 Home，但发送放回指令失败"));
            break;
        }
        setState(State::WaitingPutback,
                 QStringLiteral("机械臂已回 Home，等待安全放回设备"),
                 kArmMoveTimeoutMs);
        break;
    case State::WaitingPutback:
        if (m_currentResultRecorded) {
            appendLog(QStringLiteral("已忽略机械臂重复发送的放回 OK"));
            break;
        }
        m_stepTimer.stop();
        // SUCCESS 后机械臂只完成放回并回复 OK，仍在当前槽位循环内；
        // END 让 GRR 回到 BEGIN，随后才能接收下一槽位的 SETMOD。
        sendArm("$$MT,END\r\n");
        completeCurrentSlot(m_pendingSuccess, m_pendingReason);
        break;
    case State::Aborting:
        m_stepTimer.stop();
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
    const QString cleanSn = normalizeSn(sn); //处理sn号，删掉不需要的字符
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
    const QByteArray snHeader("\xaa\x44\x12\x1c\x25", 5);//SN二进制报文头
    const QByteArray rangeHeader("\xaa\x44\x12\x1c\x89\x13", 6);//激光测距二进制报文头

    bool changed = false;
    bool extracted = true;
    while (extracted) {
        extracted = false;
        int start = m_receiverBuffer.indexOf(rangeHeader);
        if (start >= 0 && m_receiverBuffer.size() >= start + 40) {//每个测距报文固定40字节
            const QByteArray frame = m_receiverBuffer.mid(start, 40);
            m_receiverBuffer.remove(start, 40);
            processLaserRangeFrame(frame);      //解析激光测距报文
            extracted = true;
            continue;
        }
        start = m_receiverBuffer.indexOf(snHeader);
        if (start >= 0 && m_receiverBuffer.size() >= start + 144) {   //解析SN报文，如果找到，并且已经收齐144字节说明完整
            const QByteArray frame = m_receiverBuffer.mid(start, 144);
            m_receiverBuffer.remove(start, 144);
            const QString sn = QString::fromLatin1(frame.mid(52, 9)).trimmed();//SN号位于报文第52字节开始的位置，长度9字节
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
        start = m_receiverBuffer.indexOf("$$SI");   //$$SI 硬件号报文
        if (start >= 0 && m_receiverBuffer.size() >= start + 62) {
            const QByteArray frame = m_receiverBuffer.mid(start, 62);
            m_receiverBuffer.remove(start, 62);//拿出这条报文，并且删掉
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
    if ((pendingRange >= 0 && m_receiverBuffer.size() < pendingRange + 40)//报文不完整直接返回
        || (pendingSn >= 0 && m_receiverBuffer.size() < pendingSn + 144)
        || (pendingSi >= 0 && m_receiverBuffer.size() < pendingSi + 62)) {
        if (m_faceDiagnosticsActive && !m_faceBufferWaitLogged) {
            m_faceBufferWaitLogged = true;
            appendLog(QStringLiteral("[接收机拆帧等待] 缓存=%1字节，测距头位置=%2(需40字节)，SN头位置=%3(需144字节)，SI头位置=%4(需62字节)；暂缓文本解析")
                          .arg(m_receiverBuffer.size()).arg(pendingRange).arg(pendingSn).arg(pendingSi));
        }
        if (changed)
            emit deviceInfoChanged();
        tryFinishReceiverInfo();
        return;
    }

    int newline = -1;
    if (m_faceDiagnosticsActive && !m_faceBufferWaitLogged
        && !m_receiverBuffer.isEmpty() && !m_receiverBuffer.contains('\n')) {
        m_faceBufferWaitLogged = true;
        appendLog(QStringLiteral("[接收机拆帧等待] 缓存有%1字节，但尚无LF换行结束符，等待后续数据")
                      .arg(m_receiverBuffer.size()));
    }
    while ((newline = m_receiverBuffer.indexOf('\n')) >= 0) {//取出一条完整报文
        QByteArray data = m_receiverBuffer.left(newline);
        m_receiverBuffer.remove(0, newline + 1);
        data.replace('\0', ' ');
        const QString line = QString::fromUtf8(data).trimmed();
        if (!line.isEmpty())
            processReceiverLine(line);    //laser planer检查文本中的关键字段，把解析结果保存
    }
    while (!m_receiverBuffer.isEmpty() && m_receiverBuffer.at(0) == '\0')//删除残留空字节
        m_receiverBuffer.remove(0, 1);

    if (changed) //更新设备信息
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
        const LaserBack report = parseLaserBack(line.mid(laserBackStart));//处理报文
        if (report.valid) {
            if (m_faceDiagnosticsActive)
                ++m_faceValidReports;
            appendLog(QStringLiteral(
                "$LASERBACK 解析成功：面=%1，静置状态=%2，全部完成=%3，错误码=%4，当前流程状态=%5")
                          .arg(report.panelId)
                          .arg(report.restState)
                          .arg(report.sideState)
                          .arg(report.errorCode)
                          .arg(static_cast<int>(m_state)));
            handleLaserBack(report);  //保存pitch/yaw/bias
        } else {
            if (m_faceDiagnosticsActive)
                ++m_faceInvalidReports;
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
    std::memcpy(&metres, frame.constData() + 28, sizeof(float));//从偏移28的位置读取一个 float
    if (!std::isfinite(metres) || metres < 0.0f)
        return;
    m_laserDistance = static_cast<double>(metres) * 1000.0;//得到激光距离
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
    result.restBits = fields.at(2).toInt(&ok);//读取第三个字段表示六个面的累计静置状态位
    if (!ok) return result;
    result.restState = fields.at(3).toInt(&ok);//解析当前面的静置状态
    if (!ok) return result;
    result.sideState = fields.at(4).toInt(&ok);//解析整个六面标定是否完成
    if (!ok) return result;
    result.panelId = fields.at(5).toInt(&ok);//解析当前面编号，正常应该为 1～6
    if (!ok) return result;
    // 完整实机协议包含六个面的36项参数（共51个字段）；SSCOM测试允许
    // 只发送前6个状态字段，或者发送旧版21字段简化报文。
    if (fields.size() >= 51) {
        for (int i = 6; i <= 11; ++i) {      //共6个当前面参数
            result.faceParameters.append(fields.at(i).toDouble(&ok));
            if (!ok) return LaserBack();
        }
        for (int i = 42; i <= 47; ++i) {           //6个六面完成后的综合偏差参数bias
            result.bias.append(fields.at(i).toDouble(&ok));
            if (!ok) return LaserBack();
        }
        result.pitch = fields.at(48).toDouble(&ok);  //读取激光俯仰角偏差
        if (!ok) return LaserBack();
        result.yaw = fields.at(49).toDouble(&ok);  //读取激光水平角偏差
        if (!ok) return LaserBack();
        result.errorCode = fields.at(50).toInt(&ok);  //读取设备返回的错误码
        if (!ok) return LaserBack();
    } else if (fields.size() >= 21) {   //旧版报文
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
    // $LASERBACK 的 restBits 是接收机物理面的累计完成掩码。
    int restBits = report.restBits & 0x3f;           //去掉最高两位
    // 部分固件最后一个字段上报的是机械臂点位（1~7），而不是面号；
    // 点位7仍对应面6。因此超出1~6时使用状态机当前正在校准的面。
    const int completedFace = (report.panelId >= 1 && report.panelId <= 6)
        ? report.panelId : m_currentFace;
    if (report.restState == 2
        && completedFace >= 1 && completedFace <= 6) {
        restBits |= (1 << (completedFace - 1));  //把当前面的 bit 设置成1
    }
    const int accumulatedBits = m_faceRestBits | restBits;
    if (m_faceRestBits != accumulatedBits) {
        m_faceRestBits = accumulatedBits;
        emit faceRestBitsChanged();   //状态确实发生变化时更新界面
        appendLog(QStringLiteral("面静置状态已更新：%1")
                      .arg(m_faceRestBits, 6, 2, QLatin1Char('0')));
    }

    // 不使用 panelId 覆盖 m_currentFace：GRR 的逻辑动作顺序与接收机物理面号
    // 并不相同，例如部分 GRR20 路径第一次校准会完成物理面3（mask=0x04）。
    if (report.errorCode != 0) {   //错误码不为0：有错误
        const QString detail = report.errorCode == 1
            ? QStringLiteral("激光外参校准失败，需要复位整个流程")
            : report.errorCode == 2
                ? QStringLiteral("当前面已经校准，请检查机械臂姿态")
                : QStringLiteral("未知激光标定错误 %1").arg(report.errorCode);
        failCurrentSlot(detail);
        return;
    }

    if (m_state == State::WaitingAngle) {   //全部测量完成
        if (report.sideState != 1)
            return;
        m_stepTimer.stop();
        m_finalPitch = report.pitch;
        m_finalYaw = report.yaw;
        m_finalBias = report.bias;
        appendLog(QStringLiteral("激光角度校准完成: pitch=%1, yaw=%2")
                      .arg(m_finalPitch, 0, 'f', 6)
                      .arg(m_finalYaw, 0, 'f', 6));

        // 与 wifiTcp_test3 的 dataSidSuccessSaveSlot() 保持一致：
        // 接收机完成六参数角度校准并返回最终 pitch/yaw 后，还要把结果写回设备。
        // 设备协议要求参数顺序为 yaw、pitch；旧上位机会连续发送两次相同命令，
        // 这里保留相同的重复写入行为，避免只计算出结果却没有保存到设备。
        const QString writeBackCommand = QStringLiteral("set laser angle %1 %2\r\n")
                                             .arg(m_finalYaw)
                                             .arg(m_finalPitch);
        appendLog(QStringLiteral("开始将角度标定结果写回设备: yaw=%1, pitch=%2")
                      .arg(m_finalYaw, 0, 'f', 6)
                      .arg(m_finalPitch, 0, 'f', 6));
        if (!sendReceiver(writeBackCommand.toUtf8())
            || !sendReceiver(writeBackCommand.toUtf8())) {
            failCurrentSlot(QStringLiteral("角度标定结果写回设备失败"));
            return;
        }
        appendLog(QStringLiteral("角度标定结果已按旧上位机流程连续写回设备两次"));

        // GRR13/17/20 严格保持 wifiTcp_test3 的安全路径：先发 NEXT8 回
        // P41/Home，收到到位 OK 后才发 SUCCESS。若 GRR17 在最后姿态直接
        // 收到 SUCCESS，会直奔托盘上方，可能向左绕行并撞到围挡。
        // SLAM_GRR 不支持 NEXT8；它收到 SUCCESS 后会在机械臂程序内部先
        // moveHome() 再放回，所以必须继续使用其原有协议。
        ensureReceiverSafe(); //关闭激光
        m_pendingSuccess = true;
        m_pendingReason.clear();
        if (m_deviceModel == QStringLiteral("SLAM")) {
            if (!sendArm("$$MT,SUCCESS\r\n")) {
                failCurrentSlot(QStringLiteral("发送机械臂放回指令失败"));
                return;
            }
            setState(State::WaitingPutback,
                     QStringLiteral("标定成功，等待机械臂安全放回设备"),
                     kArmMoveTimeoutMs);
            return;
        }
        if (!sendArm("$$MT,NEXT8\r\n")) {
            failCurrentSlot(QStringLiteral("发送机械臂回 Home 指令 NEXT8 失败"));
            return;
        }
        setState(State::WaitingReturnHome,
                 QStringLiteral("标定成功，等待机械臂回到 Home 点"),
                 kArmMoveTimeoutMs);
        return;
    }

    const bool waitingForFace = m_state == State::WaitingFace1
        || m_state == State::WaitingFace2
        || m_state == State::WaitingFace3
        || m_state == State::WaitingFace4
        || m_state == State::WaitingFace5
        || m_state == State::WaitingFace6;
    if (!waitingForFace)
        return;

    // 与 wifiTcp_test3 相同：任一尚未消费的物理面完成位出现，就说明当前
    // 逻辑步骤已经完成。不能强制等待 restState==2，否则 mask 已更新但
    // restState 仍为1的固件会一直等待到超时。
    const int newlyCompleted = restBits & (~m_consumedFaceCompletionMask) & 0x3f;
    appendLog(QStringLiteral("[面完成判定] 逻辑面=%1，物理面=%2，状态=%3，掩码=0x%4，已消费=0x%5，新增=0x%6")
                  .arg(m_currentFace).arg(report.panelId).arg(report.restState)
                  .arg(restBits, 2, 16, QLatin1Char('0'))
                  .arg(m_consumedFaceCompletionMask, 2, 16, QLatin1Char('0'))
                  .arg(newlyCompleted, 2, 16, QLatin1Char('0')));
    if (newlyCompleted == 0)
        return;

    m_consumedFaceCompletionMask |= restBits;
    m_stepTimer.stop();
    m_faceDiagnosticsActive = false;
    appendLog(QStringLiteral("逻辑面 %1 校准完成（接收机物理面 %2）")
                  .arg(m_currentFace).arg(report.panelId));
    switch (m_state) {
    case State::WaitingFace1:
        // 严格复现旧上位机当前实际运行路线：面1完成后不走 NEXT2（右向姿态），
        // 而是直接发送 NEXT3，在该姿态完成第二次面校准。这里复用
        // WaitingPoint2 仅表示“等待第二次面校准姿态”，并不表示发送 NEXT2。
        if (!m_armReady) {
            failCurrentSlot(QStringLiteral("机械臂未就绪，无法移动到第二次面校准姿态 NEXT3"));
            return;
        }
        if (!sendArm("$$MT,NEXT3\r\n")) {
            failCurrentSlot(QStringLiteral("发送机械臂 NEXT3 指令失败"));
            return;
        }
        setState(State::WaitingPoint2,
                 QStringLiteral("等待机械臂到达 NEXT3 第二次面校准姿态"),
                 kArmMoveTimeoutMs);
        break;
    case State::WaitingFace2:
        // 单摄像头方案：固定近点，直接移动到垂直远点。
        m_verticalNear = m_verticalLineHeight;
        m_horizontalNear = m_horizontalLineHeight;
        emit measurementChanged();
        appendLog(QStringLiteral("%1 近点固定：水平=%2 mm，垂直=%3 mm；跳过近焦测量")
                      .arg(m_deviceModel).arg(m_horizontalNear).arg(m_verticalNear));
        // 第二次面校准开始前已经开启激光和连续测距；完成后只停止连续测距，
        // 再按旧上位机流程重复发送 NEXT3。第二次 NEXT3 到位后才开始垂直远点
        // 测量，不能直接沿用第一次 NEXT3 的到位事件。
        sendReceiver("log laserrangingb ontime 0\r\n");
        moveToPoint(3);
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
        sendReceiver("log laserrangingb ontime 1\r\n");//重新开启报文
        sendReceiver("set laser survey auto\r\n");
        appendLog(QStringLiteral("六面校准完成，等待测距稳定后测量水平远点"));
        QTimer::singleShot(800, this, [this] {
            if (m_running && m_state == State::WaitingFace6)
                startVisionMeasurement(VisionTarget::HorizontalFar);//开启测量
        });
        break;
    default:
        break;
    }
}

bool LaserCalibrationController::validateMeasurements(const QVariantMap &measurements,
                                                         QString *reason)
{
    m_deviceModel = measurements.value(QStringLiteral("deviceModel"),
                                       QStringLiteral("GRR20")).toString().trimmed().toUpper();
    if (m_deviceModel == QStringLiteral("GRR20_M8_N8"))
        m_deviceModel = QStringLiteral("GRR20");

    // 与已在产线验证的 wifiTcp_test3 一致，固定近点由型号决定。
    if (m_deviceModel == QStringLiteral("GRR13")) {
        m_horizontalLineHeight = 721.0;
        m_verticalLineHeight = 507.0;
    } else if (m_deviceModel == QStringLiteral("SLAM")) {
        // SLAM 设备使用其专用托盘和激光安装方向，对应固定近点如下。
        m_horizontalLineHeight = 680.0;
        m_verticalLineHeight = 436.0;
    } else if (m_deviceModel == QStringLiteral("GRR17")) {
        m_horizontalLineHeight = 770.0;
        m_verticalLineHeight = 490.0;
    } else if (m_deviceModel == QStringLiteral("GRR20")) {
        m_horizontalLineHeight = 615.0;
        m_verticalLineHeight = 511.0;
    } else {
        if (reason)
            *reason = QStringLiteral("请选择有效的设备类型/型号：GRR13、GRR17、GRR20 或 SLAM");
        return false;
    }
    return true;
}

void LaserCalibrationController::beginNextSlot()
{
    continueIfNotPaused([this] {    //是否暂停
        const int previousIndex = m_currentIndex;
        ++m_currentIndex;
        if (m_currentIndex >= m_slots.size()) {
            if (previousIndex >= 0 && previousIndex < m_slots.size()) {
                AudioFeedback::play(m_slots.at(previousIndex).tray == QStringLiteral("left")
                                        ? AudioFeedback::Cue::LeftTrayReady
                                        : AudioFeedback::Cue::RightTrayReady);
            }
            finishBatch();
            return;
        }
        if (previousIndex >= 0 && previousIndex < m_slots.size()
            && m_slots.at(previousIndex).tray == QStringLiteral("left")
            && m_slots.at(m_currentIndex).tray == QStringLiteral("right")) {
            AudioFeedback::play(AudioFeedback::Cue::LeftTrayReady);
            AudioFeedback::play(AudioFeedback::Cue::ChangeTray);
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
    m_consumedFaceCompletionMask = 0;
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
    if (!sendArm(mode)) {
        failCurrentSlot(QStringLiteral("发送机械臂托盘模式失败"));
        return;
    }
    QTimer::singleShot(120, this, [this, slot] {
        if (!m_running || m_currentIndex < 0)
            return;
        if (!sendArm(QByteArray::number(slot.slot) + "\r\n")) {
            failCurrentSlot(QStringLiteral("发送机械臂槽位号失败"));
            return;
        }
        QTimer::singleShot(120, this, [this] {
            if (!m_running)
                return;
            if (!sendArm("$$MT,NEXT\r\n")) {
                failCurrentSlot(QStringLiteral("发送机械臂扫码指令失败"));
                return;
            }
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
    if (m_receiverAttempts == 0) {// 第一次查询
        sendReceiver("unlogall thisport\r\n$$SI\r\n");//关闭当前TCP端口之前残留的所有周期报文订阅//查询硬件号
        sendReceiver("log versionb\r\n");//返回一个144字节二进制报文
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
    if (!sendReceiver("log laserback ontime 1\r\n")) {//订阅 $LASERBACK激光标定结果报文
        failCurrentSlot(QStringLiteral("开启接收机激光标定结果上报失败"));
        return;
    }
    if (!sendReceiver("set laser reset calibration\r\n")) {//清除上一次激光外参标定状态
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
    m_faceDiagnosticsActive = true;
    m_faceRxBytes = 0;
    m_faceRxChunks = 0;
    m_faceValidReports = 0;
    m_faceInvalidReports = 0;
    m_faceBufferWaitLogged = false;
    m_faceLastRx.clear();
    m_currentFace = face;
    emit faceChanged();
    if (!sendReceiver("set laser planar calibration\r\n")) {//面校准，接收机返回LASERBACK结果报文
        failCurrentSlot(QStringLiteral("发送面校准指令失败"));
        return;
    }
    appendLog(QStringLiteral("[面校准] 面 %1 指令已写入TCP（不代表设备确认执行），开始等待 $LASERBACK 结果")
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
    // 仅做诊断，不自动重发面校准指令，避免设备重复执行校准。
    QTimer::singleShot(5000, this, [this, target, face] {
        if (!m_running || !m_faceDiagnosticsActive || m_state != target
            || m_currentFace != face)
            return;
        appendLog(QStringLiteral("[面校准等待5秒] 面=%1，收到=%2字节/%3块，有效LASERBACK=%4，解析失败=%5，缓存=%6字节，待发送=%7字节")
                      .arg(face).arg(m_faceRxBytes).arg(m_faceRxChunks)
                      .arg(m_faceValidReports).arg(m_faceInvalidReports)
                      .arg(m_receiverBuffer.size()).arg(m_receiverSocket.bytesToWrite()));
        if (m_faceRxBytes == 0)
            appendLog(QStringLiteral("[面校准诊断] 指令发出后尚未收到TCP数据；写入成功不代表接收机已确认执行。"));
    });
}

void LaserCalibrationController::moveToPoint(int point)
{
    if (!m_armReady) {
        failCurrentSlot(QStringLiteral("机械臂未就绪，无法移动到标定点 %1").arg(point));
        return;
    }
    if (!sendArm(QStringLiteral("$$MT,NEXT%1\r\n").arg(point).toLatin1())) {
        failCurrentSlot(QStringLiteral("发送机械臂 NEXT%1 指令失败").arg(point));
        return;
    }
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
    if (target == VisionTarget::VerticalNear) {
        failCurrentSlot(QStringLiteral("单摄像头方案不再执行近点视觉测量"));
        return;
    }
    const bool shortFocus = false;
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
    emit cameraChanged();//更新摄像头区域说明
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
    emit cameraChanged();//更新摄像头画面
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
    if (!std::isfinite(m_laserDistance) || m_laserDistance <= 0.0) {   //检查接收机激光距离
        appendLog(QStringLiteral(
            "[视觉/测距校验] 图像算法已成功，但激光测距值无效：%1")
                      .arg(m_laserDistance, 0, 'f', 3));
        m_cameraStatus = QStringLiteral("视觉测量成功，但未收到有效激光测距报文");
        emit cameraChanged();
        failCurrentSlot(m_cameraStatus);
        return;
    }

    // wifiTcp_test3 按换算后的毫米高度补偿。GRR13 不补偿；
    // GRR17/GRR20 仅在绝对高度超过 10 mm 时向零点方向补偿 10 mm。
    // SLAM 使用自己的固定近点，且当前不套用 GRR17/GRR20 的视觉补偿。
    const double rawHeight = height;
    if (m_deviceModel == QStringLiteral("GRR17")
        || m_deviceModel == QStringLiteral("GRR20")) {
        if (height > 10.0)
            height -= 10.0;
        else if (height < -10.0)
            height += 10.0;
    }
    appendLog(QStringLiteral("[%1 视觉补偿] 原始=%2 mm，标定使用=%3 mm")
                  .arg(m_deviceModel).arg(rawHeight, 0, 'f', 3).arg(height, 0, 'f', 3));

    QString name;//根据测量目标保存结果
    if (completedTarget == VisionTarget::VerticalNear) {
        name = QStringLiteral("垂直近点");
        m_verticalCloseRawHeight = height;     //来自摄像头算法
        m_verticalCloseRange = m_laserDistance;  //来自接收机激光测距报文
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
        startAngleCalibration();//发送 set laser angle calibration 六个参数
    }
}

bool LaserCalibrationController::calculateAngleInputs(QString *reason)
{
    // 当前 GRR 型号使用各自固定近点；只需两次远点实测。
    if (!std::isfinite(m_verticalFarRawHeight) || !std::isfinite(m_horizontalFarRawHeight)
        || !std::isfinite(m_verticalFarRange) || m_verticalFarRange <= 0.0
        || !std::isfinite(m_horizontalFarRange) || m_horizontalFarRange <= 0.0) {
        if (reason)
            *reason = QStringLiteral("远点视觉或激光测距参数无效，不能执行角度校准");
        return false;
    }
    m_horizontalNear = m_horizontalLineHeight;
    m_verticalNear = m_verticalLineHeight;
    m_horizontalFar = m_horizontalFarRawHeight;
    m_horizontalLength = m_horizontalFarRange;
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
    }                           //              固定620   2远焦高度  激光测距  固定520mm  1远焦高度 激光测距
    emit measurementChanged(); //得到最终六个参数水平近点、水平远点、水平激光长度,垂直近点、垂直远点、垂直激光长度
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
    if (!sendReceiver(command.toUtf8())) {  //发送完整laser参数,接收机返回LASERBACK
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
    AudioFeedback::playFailure(reason);
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
    AudioFeedback::play(AudioFeedback::Cue::AllFinished);
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
    saveResultToDatabase(row); //保存数据库数据
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
    const bool laserTableOk = query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS laser_results ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, time TEXT NOT NULL, tray TEXT, slot INTEGER,"
        "sn TEXT, hardware TEXT, version TEXT, distance REAL, height REAL,"
        "pitch REAL, yaw REAL, success INTEGER, error TEXT, bias TEXT)"));
    if (!laserTableOk) {
        appendLog(QStringLiteral("创建激光标定结果表失败: %1").arg(query.lastError().text()));
        return false;
    }

    const bool slamTableOk = query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS slam_results ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, time TEXT NOT NULL, tray TEXT, slot INTEGER,"
        "sn TEXT, hardware TEXT, version TEXT, result TEXT, data_path TEXT, error TEXT,"
        "UNIQUE(time,tray,slot,sn,result,data_path,error))"));
    if (!slamTableOk) {
        appendLog(QStringLiteral("创建 SLAM 标定结果表失败: %1").arg(query.lastError().text()));
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS app_meta (key TEXT PRIMARY KEY,value TEXT)"))) {
        appendLog(QStringLiteral("创建数据库迁移标记表失败: %1").arg(query.lastError().text()));
        return false;
    }

    importLegacySlamResults();
    return true;
}

void LaserCalibrationController::importLegacySlamResults()
{
    QSqlQuery migrationQuery(m_database);
    migrationQuery.prepare(QStringLiteral(
        "SELECT value FROM app_meta WHERE key='slam_csv_import_v1'"));
    if (migrationQuery.exec() && migrationQuery.next())
        return;

    // 兼容开发阶段已完成导入但尚未写迁移标记的数据库。
    QSqlQuery existingQuery(m_database);
    if (existingQuery.exec(QStringLiteral("SELECT COUNT(*) FROM slam_results"))
        && existingQuery.next() && existingQuery.value(0).toInt() > 0) {
        QSqlQuery markQuery(m_database);
        markQuery.exec(QStringLiteral(
            "INSERT OR REPLACE INTO app_meta(key,value) "
            "VALUES('slam_csv_import_v1','completed')"));
        return;
    }

    const QString csvPath = QDir(
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                .filePath(QStringLiteral("SLAM_calibration/results.csv"));
    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    bool firstLine = true;
    int imported = 0;
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (firstLine) {
            firstLine = false;
            line.remove(QChar(0xFEFF));
            if (line.startsWith(QStringLiteral("time,"), Qt::CaseInsensitive))
                continue;
        }
        if (line.trimmed().isEmpty())
            continue;
        const QStringList fields = parseCsvLine(line);
        if (fields.size() < 9)
            continue;
        QVariantMap row;
        row.insert(QStringLiteral("time"), fields.at(0));
        row.insert(QStringLiteral("tray"), fields.at(1));
        row.insert(QStringLiteral("slot"), fields.at(2).toInt());
        row.insert(QStringLiteral("sn"), fields.at(3));
        row.insert(QStringLiteral("hardware"), fields.at(4));
        row.insert(QStringLiteral("version"), fields.at(5));
        row.insert(QStringLiteral("result"), fields.at(6));
        row.insert(QStringLiteral("dataPath"), fields.at(7));
        row.insert(QStringLiteral("error"), fields.mid(8).join(QStringLiteral(",")));

        QSqlQuery query(m_database);
        query.prepare(QStringLiteral(
            "INSERT OR IGNORE INTO slam_results(time,tray,slot,sn,hardware,version,"
            "result,data_path,error) VALUES(?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(row.value(QStringLiteral("time")));
        query.addBindValue(row.value(QStringLiteral("tray")));
        query.addBindValue(row.value(QStringLiteral("slot")));
        query.addBindValue(row.value(QStringLiteral("sn")));
        query.addBindValue(row.value(QStringLiteral("hardware")));
        query.addBindValue(row.value(QStringLiteral("version")));
        query.addBindValue(row.value(QStringLiteral("result")));
        query.addBindValue(row.value(QStringLiteral("dataPath")));
        query.addBindValue(row.value(QStringLiteral("error")));
        if (query.exec() && query.numRowsAffected() > 0)
            ++imported;
    }
    if (imported > 0)
        appendLog(QStringLiteral("已导入 %1 条 SLAM 历史标定记录").arg(imported));

    QSqlQuery markQuery(m_database);
    markQuery.exec(QStringLiteral(
        "INSERT OR REPLACE INTO app_meta(key,value) "
        "VALUES('slam_csv_import_v1','completed')"));
}

void LaserCalibrationController::saveSlamResult(const QVariantMap &row)
{
    if (!m_database.isOpen())
        return;
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral(
        "INSERT OR IGNORE INTO slam_results(time,tray,slot,sn,hardware,version,"
        "result,data_path,error) VALUES(?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(row.value(QStringLiteral("time")));
    query.addBindValue(row.value(QStringLiteral("tray")));
    query.addBindValue(row.value(QStringLiteral("slot")));
    query.addBindValue(row.value(QStringLiteral("sn")));
    query.addBindValue(row.value(QStringLiteral("hardware")));
    query.addBindValue(row.value(QStringLiteral("version")));
    query.addBindValue(row.value(QStringLiteral("result")));
    query.addBindValue(row.value(QStringLiteral("dataPath")));
    query.addBindValue(row.value(QStringLiteral("error")));
    if (!query.exec())
        appendLog(QStringLiteral("保存 SLAM 标定数据库失败: %1").arg(query.lastError().text()));
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
    query.prepare(QStringLiteral(
        "SELECT id,time,record_type,tray,slot,sn,hardware,version,distance,height,"
        "pitch,yaw,success,error,bias,data_path FROM ("
        "SELECT id,time,'laser' AS record_type,tray,slot,sn,hardware,version,"
        "distance,height,pitch,yaw,success,error,bias,'' AS data_path FROM laser_results "
        "UNION ALL "
        "SELECT id,time,'slam' AS record_type,tray,slot,sn,hardware,version,"
        "NULL AS distance,NULL AS height,NULL AS pitch,NULL AS yaw,"
        "CASE WHEN lower(result)='success' THEN 1 ELSE 0 END AS success,"
        "error,'[]' AS bias,data_path FROM slam_results) "
        "WHERE (? = '' OR sn LIKE ?) ORDER BY time DESC,id DESC"));
    query.addBindValue(normalizedSn);
    query.addBindValue(QStringLiteral("%") + normalizedSn + QStringLiteral("%"));
    if (!query.exec())
        return rows;

    while (query.next()) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), query.value(0));
        row.insert(QStringLiteral("time"), query.value(1));
        const QString recordType = query.value(2).toString();
        row.insert(QStringLiteral("mode"), recordType);
        const QString tray = query.value(3).toString();
        row.insert(QStringLiteral("tray"), tray);
        row.insert(QStringLiteral("trayText"),
                   tray == QStringLiteral("left") ? QStringLiteral("左")
                       : (tray == QStringLiteral("right") ? QStringLiteral("右") : tray));
        row.insert(QStringLiteral("slot"), query.value(4));
        row.insert(QStringLiteral("sn"), query.value(5));
        row.insert(QStringLiteral("hardware"), query.value(6));
        row.insert(QStringLiteral("version"), query.value(7));
        row.insert(QStringLiteral("distance"), query.value(8));
        row.insert(QStringLiteral("height"), query.value(9));
        row.insert(QStringLiteral("pitch"), query.value(10));
        row.insert(QStringLiteral("yaw"), query.value(11));
        row.insert(QStringLiteral("success"), query.value(12).toInt() != 0);
        row.insert(QStringLiteral("result"), query.value(12).toInt() != 0
                       ? QStringLiteral("成功") : QStringLiteral("失败"));
        row.insert(QStringLiteral("error"), query.value(13));
        row.insert(QStringLiteral("bias"), query.value(14));
        row.insert(QStringLiteral("dataPath"), query.value(15));
        rows.append(row);
    }
    return rows;
}

QString LaserCalibrationController::normalizeSn(const QString &value) const
{
    QString candidate = value.trimmed();
    candidate.remove(QStringLiteral("$*"));
    candidate.remove('\r');
    candidate.remove('\n');
    if (candidate.startsWith(QStringLiteral("SNFF"), Qt::CaseInsensitive))
        candidate = candidate.mid(4);

    // SLAM 条码可能是 SNL88L00017，而设备热点是 LS200-L88L00017。
    // SLAM 模式下统一保存/匹配热点实际使用的 9 位 L88L00017。
    if (m_deviceModel == QStringLiteral("SLAM")) {
        static const QRegularExpression fullSlamToken(
            QStringLiteral("^SN([A-Za-z0-9_-]{9})$"),
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch fullMatch = fullSlamToken.match(candidate);
        if (fullMatch.hasMatch())
            candidate = fullMatch.captured(1);
    }

    // GRR 扫码报文为 SNFF + 9 位 SN；SLAM 已在上面去掉条码中的 SN 前缀。
    // 完整匹配可避免半包和日志文字被当成设备号。
    static const QRegularExpression token(QStringLiteral("^([A-Za-z0-9_-]{9})$"));
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
