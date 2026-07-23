#include "tcpcontroller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QtMqtt/qmqttclient.h>
#include <QtMqtt/qmqttsubscription.h>
#include <QtMqtt/qmqtttopicfilter.h>
#include <QtMqtt/qmqtttopicname.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wlanapi.h>
#endif

namespace {
const int kHandshakeRetryMs = 3000;
const int kHandshakeMaxAttempts = 3;
const int kScanTimeoutMs = 60000;
const int kWifiTimeoutMs = 45000;
const int kDeviceConnectTimeoutMs = 15000;
const int kDeviceInfoTimeoutMs = 20000;
const int kMqttTimeoutMs = 15000;
const int kCollectTimeoutMs = 20000;
const int kCalibrationTimeoutMs = 10 * 60 * 1000;
const int kStopCollectTimeoutMs = 30000;
const int kPutBackTimeoutMs = 120000;
}

TcpController::TcpController(QObject *parent)
    : QObject(parent)
    , m_armSocket(new QTcpSocket(this))
    , m_deviceSocket(new QTcpSocket(this))
    , m_mqttClient(new QMqttClient(this))
    , m_ftpProcess(new QProcess(this))
    , m_connected(false)
    , m_armReady(false)
    , m_taskActive(false)
    , m_wifiConnected(false)
    , m_deviceTcpConnected(false)
    , m_mqttConnected(false)
    , m_ftpDownloading(false)
    , m_mqttSubscribed(false)
    , m_failureAwaitingPutBack(false)
    , m_aborting(false)
    , m_cleaningUp(false)
    , m_statusText(QStringLiteral("未连接"))
    , m_currentProgress(QStringLiteral("等待连接机械臂"))
    , m_host(QStringLiteral("192.168.5.1"))
    , m_port(6601)
    , m_handshakeAttempts(0)
    , m_wifiConnectTicks(0)
    , m_flowState(FlowState::Idle)
    , m_currentQueueIndex(-1)
    , m_selectedCount(0)
    , m_successCount(0)
    , m_failureCount(0)
    , m_deviceHost(QStringLiteral("192.168.1.8"))
    , m_devicePort(25001)
    , m_mqttHost(QStringLiteral("192.168.1.8"))
    , m_mqttPort(26001)
    , m_ftpUrl(QStringLiteral("ftp://192.168.1.8:21/"))
    , m_ftpUser(QStringLiteral("anonymous"))
    , m_ftpPhase(0)
    , m_ftpTotalFiles(0)
    , m_ftpDownloadedFiles(0)
{
    for (int i = 0; i < 6; ++i) {
        m_leftSlotStates.append(QStringLiteral("idle"));
        m_rightSlotStates.append(QStringLiteral("idle"));
    }

    connect(m_armSocket, &QTcpSocket::connected, this, &TcpController::onArmConnected);
    connect(m_armSocket, &QTcpSocket::disconnected, this, &TcpController::onArmDisconnected);
    connect(m_armSocket, &QTcpSocket::readyRead, this, &TcpController::onArmReadyRead);
    connect(m_armSocket,
            static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error),
            this, &TcpController::onArmError);

    connect(m_deviceSocket, &QTcpSocket::connected, this, &TcpController::onDeviceConnected);
    connect(m_deviceSocket, &QTcpSocket::disconnected, this, &TcpController::onDeviceDisconnected);
    connect(m_deviceSocket, &QTcpSocket::readyRead, this, &TcpController::onDeviceReadyRead);
    connect(m_deviceSocket,
            static_cast<void (QTcpSocket::*)(QAbstractSocket::SocketError)>(&QTcpSocket::error),
            this, &TcpController::onDeviceError);

    m_wifiStatusTimer.setInterval(1500);
    connect(&m_wifiStatusTimer, &QTimer::timeout, this, &TcpController::checkWifiConnection);
    m_wifiStatusTimer.start();

    m_handshakeTimer.setSingleShot(true);
    connect(&m_handshakeTimer, &QTimer::timeout, this, &TcpController::onHandshakeTimeout);

    m_stepTimer.setSingleShot(true);
    connect(&m_stepTimer, &QTimer::timeout, this, &TcpController::onStepTimeout);

    m_mqttClient->setHostname(m_mqttHost);
    m_mqttClient->setPort(static_cast<quint16>(m_mqttPort));
    m_mqttClient->setProtocolVersion(QMqttClient::MQTT_3_1_1);
    m_mqttClient->setClientId(QStringLiteral("slam-calib-%1")
                                  .arg(QDateTime::currentMSecsSinceEpoch()));

    connect(m_mqttClient, &QMqttClient::stateChanged, this,
            [this](QMqttClient::ClientState state) {
        const bool connectedNow = state == QMqttClient::Connected;
        if (m_mqttConnected != connectedNow) {
            m_mqttConnected = connectedNow;
            emit mqttConnectedChanged();
        }

        if (!connectedNow)
            return;

        appendLog(QStringLiteral("MQTT 已连接: %1:%2").arg(m_mqttHost).arg(m_mqttPort));
        if (!m_mqttSubscribed) {
            QMqttSubscription *subscription = m_mqttClient->subscribe(
                QMqttTopicFilter(QStringLiteral("slam_app/response/data_collect")), 1);
            m_mqttSubscribed = subscription != nullptr;
            appendLog(m_mqttSubscribed
                          ? QStringLiteral("MQTT 已订阅: slam_app/response/data_collect")
                          : QStringLiteral("MQTT 订阅失败"));
        }

        if (m_taskActive && m_flowState == FlowState::ConnectingMqtt) {
            if (!publishCollectAction(QStringLiteral("AC_START")))
                failCurrentSlot(QStringLiteral("MQTT 发布 AC_START 失败"));
        }
    });

    connect(m_mqttClient, &QMqttClient::messageReceived, this,
            [this](const QByteArray &message, const QMqttTopicName &topic) {
        handleMqttMessage(message, topic.name());
    });

    connect(m_mqttClient, &QMqttClient::errorChanged, this,
            [this](QMqttClient::ClientError error) {
        if (error == QMqttClient::NoError)
            return;
        appendLog(QStringLiteral("MQTT 错误，错误码: %1").arg(static_cast<int>(error)));
        if (m_taskActive
            && (m_flowState == FlowState::ConnectingMqtt
                || m_flowState == FlowState::StartingCollection
                || m_flowState == FlowState::Calibrating
                || m_flowState == FlowState::StoppingCollection)) {
            failCurrentSlot(QStringLiteral("MQTT 通信失败"));
        }
    });

    connect(m_ftpProcess,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
        handleFtpProcessFinished(exitCode, static_cast<int>(exitStatus));
    });

    connect(m_ftpProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        Q_UNUSED(error)
        if (!m_ftpDownloading || m_flowState != FlowState::Downloading)
            return;
        m_ftpDownloading = false;
        emit ftpDownloadingChanged();
        failCurrentSlot(QStringLiteral("FTP 进程启动失败: %1").arg(m_ftpProcess->errorString()));
    });
}

bool TcpController::connected() const { return m_connected; }
bool TcpController::armReady() const { return m_armReady; }
bool TcpController::armRunning() const { return m_taskActive; }
QString TcpController::statusText() const { return m_statusText; }
QString TcpController::currentProgress() const { return m_currentProgress; }
QStringList TcpController::debugLog() const { return m_debugLog; }
QString TcpController::host() const { return m_host; }
int TcpController::port() const { return m_port; }
QString TcpController::wifiNamePart() const { return m_wifiNamePart; }
bool TcpController::wifiConnected() const { return m_wifiConnected; }
bool TcpController::deviceTcpConnected() const { return m_deviceTcpConnected; }
bool TcpController::mqttConnected() const { return m_mqttConnected; }
bool TcpController::ftpDownloading() const { return m_ftpDownloading; }
QString TcpController::collectStatus() const { return m_collectStatus; }
QString TcpController::collectPath() const { return m_collectPath; }
QString TcpController::currentTray() const
{
    if (m_currentQueueIndex < 0 || m_currentQueueIndex >= m_taskQueue.size())
        return QString();
    return m_taskQueue.at(m_currentQueueIndex).left ? QStringLiteral("左托盘")
                                                    : QStringLiteral("右托盘");
}
int TcpController::currentSlot() const
{
    if (m_currentQueueIndex < 0 || m_currentQueueIndex >= m_taskQueue.size())
        return 0;
    return m_taskQueue.at(m_currentQueueIndex).slot;
}
QString TcpController::currentSn() const { return m_currentSn; }
QString TcpController::currentHardware() const { return m_currentHardware; }
QString TcpController::currentVersion() const { return m_currentVersion; }
QStringList TcpController::leftSlotStates() const { return m_leftSlotStates; }
QStringList TcpController::rightSlotStates() const { return m_rightSlotStates; }
int TcpController::selectedCount() const { return m_selectedCount; }
int TcpController::successCount() const { return m_successCount; }
int TcpController::failureCount() const { return m_failureCount; }
QString TcpController::lastError() const { return m_lastError; }

void TcpController::setHost(const QString &host)
{
    if (m_host == host)
        return;
    m_host = host;
    emit hostChanged();
}

void TcpController::setPort(int port)
{
    if (m_port == port)
        return;
    m_port = port;
    emit portChanged();
}

void TcpController::toggleConnection()
{
    if (m_armSocket->state() == QAbstractSocket::UnconnectedState)
        connectToServer();
    else
        disconnectFromServer();
}

void TcpController::connectToServer()
{
    if (m_armSocket->state() != QAbstractSocket::UnconnectedState)
        return;
    m_statusText = QStringLiteral("正在连接 %1:%2").arg(m_host).arg(m_port);
    emit statusTextChanged();
    setProgress(QStringLiteral("正在连接机械臂"));
    appendLog(m_statusText);
    m_armSocket->connectToHost(m_host, static_cast<quint16>(m_port));
}

void TcpController::disconnectFromServer()
{
    if (m_taskActive)
        abortTask();
    m_handshakeTimer.stop();
    m_armSocket->disconnectFromHost();
}

void TcpController::toggleArmTask(const QVariantList &leftSlots, const QVariantList &rightSlots)
{
    if (m_taskActive)
        abortTask();
    else
        beginTask(leftSlots, rightSlots);
}

void TcpController::clearTaskResults()
{
    if (m_taskActive)
        return;
    m_taskQueue.clear();
    m_resultRecorded.clear();
    m_currentQueueIndex = -1;
    m_leftSlotStates.clear();
    m_rightSlotStates.clear();
    for (int i = 0; i < 6; ++i) {
        m_leftSlotStates.append(QStringLiteral("idle"));
        m_rightSlotStates.append(QStringLiteral("idle"));
    }
    m_selectedCount = 0;
    m_successCount = 0;
    m_failureCount = 0;
    resetCurrentDevice();
    setLastError(QString());
    emit slotStatesChanged();
    emit statisticsChanged();
    emit currentDeviceChanged();
    setFlowState(m_armReady ? FlowState::Ready : FlowState::Idle,
                 m_armReady ? QStringLiteral("机械臂已就绪，请选择托盘槽位")
                            : QStringLiteral("等待连接机械臂"));
}

void TcpController::onArmConnected()
{
    m_connected = true;
    emit connectedChanged();
    m_statusText = QStringLiteral("TCP 已连接 %1:%2").arg(m_host).arg(m_port);
    emit statusTextChanged();
    m_armReady = false;
    emit armReadyChanged();
    m_handshakeAttempts = 0;
    setFlowState(FlowState::WaitingHandshake, QStringLiteral("等待机械臂握手响应"));
    appendLog(QStringLiteral("机械臂 TCP 连接成功"));
    onHandshakeTimeout();
}

void TcpController::onArmDisconnected()
{
    const bool wasTaskActive = m_taskActive;
    m_connected = false;
    m_armReady = false;
    emit connectedChanged();
    emit armReadyChanged();
    m_handshakeTimer.stop();
    stopStepTimer();
    m_statusText = QStringLiteral("未连接");
    emit statusTextChanged();
    appendLog(QStringLiteral("机械臂连接已断开"));

    if (wasTaskActive) {
        setCurrentSlotState(QStringLiteral("failed"));
        setLastError(QStringLiteral("机械臂连接中断"));
        recordCurrentResult(QStringLiteral("failed"), m_lastError);
        m_taskActive = false;
        emit armRunningChanged();
        cleanupDeviceConnections();
    }
    setFlowState(FlowState::Idle, QStringLiteral("等待连接机械臂"));
}

void TcpController::onArmError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError)
    m_statusText = QStringLiteral("连接失败: %1").arg(m_armSocket->errorString());
    emit statusTextChanged();
    appendLog(m_statusText);
    if (!m_connected)
        setProgress(m_statusText);
}

void TcpController::onArmReadyRead()
{
    m_armReceiveBuffer.append(m_armSocket->readAll());
    processArmBuffer();
}

void TcpController::onHandshakeTimeout()
{
    if (!m_connected || m_armReady)
        return;
    if (m_handshakeAttempts >= kHandshakeMaxAttempts) {
        appendLog(QStringLiteral("机械臂握手超时，可继续等待响应或重新连接"));
        setProgress(QStringLiteral("机械臂握手超时，请检查机械臂程序"));
        return;
    }
    ++m_handshakeAttempts;
    sendArmCommand(QStringLiteral("$$MT,CONNECT"));
    appendLog(QStringLiteral("发送机械臂握手指令（第 %1 次）").arg(m_handshakeAttempts));
    m_handshakeTimer.start(kHandshakeRetryMs);
}

bool TcpController::sendArmCommand(const QString &command)
{
    if (m_armSocket->state() != QAbstractSocket::ConnectedState) {
        appendLog(QStringLiteral("机械臂未连接，无法发送: %1").arg(command));
        return false;
    }
    QByteArray payload = command.toUtf8();
    if (!payload.endsWith("\r\n"))
        payload.append("\r\n");
    if (m_armSocket->write(payload) < 0) {
        appendLog(QStringLiteral("机械臂指令发送失败: %1").arg(m_armSocket->errorString()));
        return false;
    }
    m_armSocket->flush();
    appendLog(QStringLiteral("发送机械臂: %1").arg(command));
    return true;
}

void TcpController::processArmBuffer()
{
    while (true) {
        int delimiterIndex = -1;
        int delimiterLength = 0;
        const int crlf = m_armReceiveBuffer.indexOf("\r\n");
        const int marker = m_armReceiveBuffer.indexOf("$*");
        const int lf = m_armReceiveBuffer.indexOf('\n');

        if (crlf >= 0) {
            delimiterIndex = crlf;
            delimiterLength = 2;
        }
        if (marker >= 0 && (delimiterIndex < 0 || marker < delimiterIndex)) {
            delimiterIndex = marker;
            delimiterLength = 2;
        }
        if (lf >= 0 && (delimiterIndex < 0 || lf < delimiterIndex)) {
            delimiterIndex = lf;
            delimiterLength = 1;
        }
        if (delimiterIndex < 0)
            break;

        const QByteArray packet = m_armReceiveBuffer.left(delimiterIndex);
        m_armReceiveBuffer.remove(0, delimiterIndex + delimiterLength);
        const QString command = QString::fromUtf8(packet).trimmed();
        if (!command.isEmpty()) {
            appendLog(QStringLiteral("收到机械臂: %1").arg(command));
            emit commandReceived(command);
            handleArmCommand(command);
        }
    }

    if (m_armReceiveBuffer.size() > 8192) {
        appendLog(QStringLiteral("机械臂接收缓冲区异常，已清空"));
        m_armReceiveBuffer.clear();
    }
}

QString TcpController::normalizedArmCommand(const QString &command) const
{
    QString result = command.trimmed();
    while (result.startsWith(QLatin1Char('$')))
        result.remove(0, 1);
    while (result.endsWith(QLatin1Char('$')) || result.endsWith(QLatin1Char('*')))
        result.chop(1);
    return result.trimmed().toUpper();
}

void TcpController::handleArmCommand(const QString &command)
{
    const QString normalized = normalizedArmCommand(command);

    if (normalized == QStringLiteral("MT,CONNECTOK")) {
        m_handshakeTimer.stop();
        if (!m_armReady) {
            m_armReady = true;
            emit armReadyChanged();
        }
        setFlowState(FlowState::Ready, QStringLiteral("机械臂已就绪，请选择托盘槽位"));
        appendLog(QStringLiteral("机械臂协议握手成功"));
        return;
    }

    if (!m_taskActive)
        return;

    if (normalized.startsWith(QStringLiteral("MT,SN,"))) {
        handleScannedSn(command.mid(command.toUpper().indexOf(QStringLiteral("MT,SN,")) + 6)
                            .remove(QStringLiteral("$*")).trimmed());
        return;
    }
    if (normalized.startsWith(QStringLiteral("SN$"))) {
        handleScannedSn(command.mid(command.indexOf(QStringLiteral("SN$")) + 3)
                            .remove(QStringLiteral("$*")).trimmed());
        return;
    }

    const bool failedCommand = normalized.contains(QStringLiteral("FAILED"))
                               || normalized.contains(QStringLiteral(",FAIL"))
                               || normalized.contains(QStringLiteral(",ERROR"))
                               || normalized.endsWith(QStringLiteral(",NG"));
    if (failedCommand) {
        if (m_flowState == FlowState::WaitingPutBack) {
            setCurrentSlotState(QStringLiteral("failed"));
            setLastError(QStringLiteral("机械臂放回设备失败"));
            recordCurrentResult(QStringLiteral("failed"), m_lastError);
            QTimer::singleShot(0, this, &TcpController::startNextSlot);
        } else {
            failCurrentSlot(QStringLiteral("机械臂报告失败: %1").arg(command));
        }
        return;
    }

    if (normalized.contains(QStringLiteral("RUNFINISH"))) {
        if (m_flowState == FlowState::Calibrating) {
            appendLog(QStringLiteral("机械臂标定轨迹执行完成"));
            if (!publishCollectAction(QStringLiteral("AC_STOP")))
                failCurrentSlot(QStringLiteral("MQTT 发布 AC_STOP 失败"));
        }
        return;
    }

    const bool putBackDone = normalized.contains(QStringLiteral("PUTBACKOK"))
                             || (normalized == QStringLiteral("MT,OK")
                                 && m_flowState == FlowState::WaitingPutBack);
    if (putBackDone) {
        stopStepTimer();
        if (m_aborting) {
            sendArmCommand(QStringLiteral("$$MT,OVER"));
            m_taskActive = false;
            m_aborting = false;
            emit armRunningChanged();
            cleanupDeviceConnections();
            setFlowState(FlowState::Aborted, QStringLiteral("任务已中止"));
        } else {
            if (!m_failureAwaitingPutBack)
                markCurrentSuccess();
            QTimer::singleShot(0, this, &TcpController::startNextSlot);
        }
        return;
    }

    if (normalized.contains(QStringLiteral("STARTOK"))
        && m_flowState == FlowState::Calibrating) {
        setProgress(QStringLiteral("机械臂正在执行 SLAM 标定轨迹"));
        return;
    }

    if (normalized == QStringLiteral("MT,OVER"))
        finishTask();
}

void TcpController::beginTask(const QVariantList &leftSlots, const QVariantList &rightSlots)
{
    if (!m_connected || !m_armReady) {
        appendLog(QStringLiteral("机械臂尚未完成握手，不能开始标定"));
        setProgress(QStringLiteral("请先完成机械臂连接和握手"));
        return;
    }

    m_taskQueue.clear();
    m_leftSlotStates.clear();
    m_rightSlotStates.clear();
    for (int i = 0; i < 6; ++i) {
        m_leftSlotStates.append(QStringLiteral("idle"));
        m_rightSlotStates.append(QStringLiteral("idle"));
    }
    QSet<int> uniqueLeft;
    QSet<int> uniqueRight;

    for (const QVariant &value : leftSlots) {
        const int slot = value.toInt();
        if (slot >= 1 && slot <= 6 && !uniqueLeft.contains(slot)) {
            uniqueLeft.insert(slot);
            m_taskQueue.append({true, slot});
            m_leftSlotStates[slot - 1] = QStringLiteral("pending");
        }
    }
    for (const QVariant &value : rightSlots) {
        const int slot = value.toInt();
        if (slot >= 1 && slot <= 6 && !uniqueRight.contains(slot)) {
            uniqueRight.insert(slot);
            m_taskQueue.append({false, slot});
            m_rightSlotStates[slot - 1] = QStringLiteral("pending");
        }
    }

    if (m_taskQueue.isEmpty()) {
        appendLog(QStringLiteral("请至少选择一个托盘槽位"));
        return;
    }

    m_selectedCount = m_taskQueue.size();
    m_resultRecorded.fill(false, m_taskQueue.size());
    m_successCount = 0;
    m_failureCount = 0;
    m_currentQueueIndex = -1;
    m_taskActive = true;
    m_aborting = false;
    m_failureAwaitingPutBack = false;
    setLastError(QString());
    emit slotStatesChanged();
    emit statisticsChanged();
    emit armRunningChanged();
    appendLog(QStringLiteral("开始 SLAM 标定，共 %1 个槽位").arg(m_selectedCount));
    startNextSlot();
}

void TcpController::abortTask()
{
    if (!m_taskActive)
        return;
    const FlowState stateBeforeAbort = m_flowState;
    if (currentSlotState() != QStringLiteral("failed")
        && currentSlotState() != QStringLiteral("success")) {
        setCurrentSlotState(QStringLiteral("failed"));
        setLastError(QStringLiteral("用户中止任务"));
        recordCurrentResult(QStringLiteral("failed"), m_lastError);
    }
    m_aborting = true;
    stopStepTimer();
    setProgress(QStringLiteral("正在中止任务"));
    appendLog(QStringLiteral("用户请求中止 SLAM 标定任务"));

    if (m_mqttConnected
        && (stateBeforeAbort == FlowState::StartingCollection
            || stateBeforeAbort == FlowState::Calibrating
            || stateBeforeAbort == FlowState::StoppingCollection
            || stateBeforeAbort == FlowState::Downloading)) {
        publishCollectAction(QStringLiteral("AC_STOP"));
    }

    if (stateNeedsPutBack(stateBeforeAbort)) {
        requestPutBack(true);
        return;
    }

    sendArmCommand(QStringLiteral("$$MT,OVER"));
    m_taskActive = false;
    m_aborting = false;
    emit armRunningChanged();
    cleanupDeviceConnections();
    setFlowState(FlowState::Aborted, QStringLiteral("任务已中止"));
}

void TcpController::startNextSlot()
{
    if (!m_taskActive || m_aborting)
        return;

    cleanupDeviceConnections();
    ++m_currentQueueIndex;
    if (m_currentQueueIndex >= m_taskQueue.size()) {
        finishTask();
        return;
    }

    resetCurrentDevice();
    setCurrentSlotState(QStringLiteral("running"));
    emit currentDeviceChanged();

    const SlotTarget target = m_taskQueue.at(m_currentQueueIndex);
    const QString side = target.left ? QStringLiteral("LEFT") : QStringLiteral("RIGHT");
    const QString display = target.left ? QStringLiteral("左托盘") : QStringLiteral("右托盘");
    setFlowState(FlowState::WaitingScan,
                 QStringLiteral("等待机械臂扫描 %1 %2 号位").arg(display).arg(target.slot),
                 kScanTimeoutMs);
    if (!sendArmCommand(QStringLiteral("$$MT,%1,%2").arg(side).arg(target.slot)))
        failCurrentSlot(QStringLiteral("发送槽位指令失败"));
}

void TcpController::finishTask()
{
    stopStepTimer();
    if (m_taskActive)
        sendArmCommand(QStringLiteral("$$MT,OVER"));
    m_taskActive = false;
    m_aborting = false;
    m_failureAwaitingPutBack = false;
    emit armRunningChanged();
    cleanupDeviceConnections();
    setFlowState(FlowState::Completed,
                 QStringLiteral("全部标定完成：成功 %1，失败 %2")
                     .arg(m_successCount).arg(m_failureCount));
    appendLog(QStringLiteral("SLAM 标定任务完成：成功 %1，失败 %2")
                  .arg(m_successCount).arg(m_failureCount));
}

void TcpController::resetCurrentDevice()
{
    m_wifiNamePart.clear();
    m_currentSn.clear();
    m_currentHardware.clear();
    m_currentVersion.clear();
    m_collectStatus.clear();
    m_collectPath.clear();
    m_ftpLocalDir.clear();
    m_armReceiveBuffer.squeeze();
    m_wifiConnectTicks = 0;
    setLastError(QString());
    emit wifiNamePartChanged();
    emit currentDeviceChanged();
    emit collectStatusChanged();
    emit collectPathChanged();
}

void TcpController::setCurrentSlotState(const QString &state)
{
    if (m_currentQueueIndex < 0 || m_currentQueueIndex >= m_taskQueue.size())
        return;
    const SlotTarget target = m_taskQueue.at(m_currentQueueIndex);
    QStringList &states = target.left ? m_leftSlotStates : m_rightSlotStates;
    if (states.at(target.slot - 1) == state)
        return;
    states[target.slot - 1] = state;
    emit slotStatesChanged();
    updateStatistics();
}

QString TcpController::currentSlotState() const
{
    if (m_currentQueueIndex < 0 || m_currentQueueIndex >= m_taskQueue.size())
        return QStringLiteral("idle");
    const SlotTarget target = m_taskQueue.at(m_currentQueueIndex);
    return (target.left ? m_leftSlotStates : m_rightSlotStates).at(target.slot - 1);
}

void TcpController::markCurrentSuccess()
{
    if (currentSlotState() == QStringLiteral("failed"))
        return;
    setCurrentSlotState(QStringLiteral("success"));
    recordCurrentResult(QStringLiteral("success"));
}

void TcpController::failCurrentSlot(const QString &reason)
{
    if (!m_taskActive)
        return;
    const FlowState failedState = m_flowState;
    stopStepTimer();
    setCurrentSlotState(QStringLiteral("failed"));
    setLastError(reason);
    recordCurrentResult(QStringLiteral("failed"), reason);
    const QString failureProgress = QStringLiteral("%1 %2 号位失败：%3")
                                        .arg(currentTray()).arg(currentSlot()).arg(reason);
    setFlowState(FlowState::Recovering, failureProgress);
    appendLog(QStringLiteral("当前槽位标定失败: %1").arg(reason));

    if (m_mqttConnected
        && (failedState == FlowState::StartingCollection
            || failedState == FlowState::Calibrating
            || failedState == FlowState::StoppingCollection
            || failedState == FlowState::Downloading)) {
        publishCollectAction(QStringLiteral("AC_STOP"));
        setFlowState(FlowState::Recovering, failureProgress);
    }

    if (failedState == FlowState::WaitingPutBack) {
        QTimer::singleShot(0, this, &TcpController::startNextSlot);
    } else if (stateNeedsPutBack(failedState)) {
        requestPutBack(true);
    } else {
        QTimer::singleShot(300, this, &TcpController::startNextSlot);
    }
}

void TcpController::recordCurrentResult(const QString &result, const QString &reason)
{
    if (m_currentQueueIndex < 0 || m_currentQueueIndex >= m_taskQueue.size())
        return;
    if (m_currentQueueIndex < m_resultRecorded.size() && m_resultRecorded.at(m_currentQueueIndex))
        return;

    // Automated protocol checks must never create fake production records.
    if (qEnvironmentVariableIsSet("SIMPLE_QT_DISABLE_RESULT_RECORDING")) {
        if (m_currentQueueIndex < m_resultRecorded.size())
            m_resultRecorded[m_currentQueueIndex] = true;
        return;
    }

    const QString resultDirectory = QDir(
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                        .filePath(QStringLiteral("SLAM_calibration"));
    if (!QDir().mkpath(resultDirectory)) {
        appendLog(QStringLiteral("无法创建标定结果目录: %1").arg(resultDirectory));
        return;
    }

    const QString filePath = QDir(resultDirectory).filePath(QStringLiteral("results.csv"));
    const QByteArray utf8Bom("\xEF\xBB\xBF", 3);
    QFile file(filePath);
    const bool needsHeader = !file.exists() || file.size() == 0;

    // Excel on Windows may otherwise interpret UTF-8 Chinese text as the local ANSI code page.
    // Migrate an existing UTF-8 CSV safely, and always add a BOM to newly created files.
    if (!needsHeader) {
        QFile existing(filePath);
        if (existing.open(QIODevice::ReadOnly)) {
            const QByteArray existingData = existing.readAll();
            existing.close();
            if (!existingData.startsWith(utf8Bom)) {
                QSaveFile migrated(filePath);
                if (!migrated.open(QIODevice::WriteOnly)
                    || migrated.write(utf8Bom) != utf8Bom.size()
                    || migrated.write(existingData) != existingData.size()
                    || !migrated.commit()) {
                    appendLog(QStringLiteral("无法为结果文件补充 UTF-8 标记，请先关闭 Excel: %1")
                                  .arg(migrated.errorString()));
                    return;
                }
            }
        }
    }

    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        appendLog(QStringLiteral("无法写入标定结果: %1").arg(file.errorString()));
        return;
    }

    if (needsHeader && file.write(utf8Bom) != utf8Bom.size()) {
        appendLog(QStringLiteral("无法写入结果文件编码标记: %1").arg(file.errorString()));
        return;
    }

    auto csvField = [](QString value) {
        value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"%1\"").arg(value);
    };

    QTextStream stream(&file);
    stream.setCodec("UTF-8");
    if (needsHeader)
        stream << "time,tray,slot,sn,hardware,version,result,data_path,error\n";
    const QString dataPath = result == QStringLiteral("success") && !m_ftpLocalDir.isEmpty()
                                 ? m_ftpLocalDir
                                 : m_collectPath;
    stream << csvField(QDateTime::currentDateTime().toString(Qt::ISODate)) << ','
           << csvField(currentTray()) << ','
           << currentSlot() << ','
           << csvField(m_currentSn) << ','
           << csvField(m_currentHardware) << ','
           << csvField(m_currentVersion) << ','
           << csvField(result) << ','
           << csvField(dataPath) << ','
           << csvField(reason) << '\n';
    file.close();

    if (m_currentQueueIndex < m_resultRecorded.size())
        m_resultRecorded[m_currentQueueIndex] = true;
    appendLog(QStringLiteral("标定结果已记录: %1").arg(filePath));
}

bool TcpController::stateNeedsPutBack(FlowState state) const
{
    return state == FlowState::Calibrating
           || state == FlowState::StoppingCollection
           || state == FlowState::Downloading
           || state == FlowState::WaitingPutBack;
}

void TcpController::requestPutBack(bool afterFailure)
{
    m_failureAwaitingPutBack = afterFailure;
    if (!sendArmCommand(QStringLiteral("$$MT,PUTBACK"))) {
        if (m_aborting) {
            m_taskActive = false;
            m_aborting = false;
            emit armRunningChanged();
            setFlowState(FlowState::Aborted, QStringLiteral("任务已中止，机械臂放回指令发送失败"));
        } else {
            if (!afterFailure) {
                setCurrentSlotState(QStringLiteral("failed"));
                setLastError(QStringLiteral("机械臂放回指令发送失败"));
                recordCurrentResult(QStringLiteral("failed"), m_lastError);
            }
            QTimer::singleShot(0, this, &TcpController::startNextSlot);
        }
        return;
    }
    setFlowState(FlowState::WaitingPutBack,
                 afterFailure ? QStringLiteral("当前设备失败，等待机械臂放回设备")
                              : QStringLiteral("等待机械臂放回设备"),
                 kPutBackTimeoutMs);
}

void TcpController::handleScannedSn(const QString &sn)
{
    if (m_flowState != FlowState::WaitingScan)
        return;
    QString cleanSn = sn.trimmed();
    cleanSn.remove(QStringLiteral("$*"));
    if (cleanSn.isEmpty()) {
        failCurrentSlot(QStringLiteral("机械臂返回的 SN 为空"));
        return;
    }
    stopStepTimer();
    m_currentSn = cleanSn;
    m_wifiNamePart = cleanSn;
    emit currentDeviceChanged();
    emit wifiNamePartChanged();
    appendLog(QStringLiteral("扫描到设备 SN: %1").arg(cleanSn));
    setFlowState(FlowState::ConnectingWifi,
                 QStringLiteral("正在连接设备 Wi-Fi：%1").arg(cleanSn),
                 kWifiTimeoutMs);
    connectWifiByNamePart(cleanSn);
}

bool TcpController::isConnectedToTargetWifi() const
{
#ifdef Q_OS_WIN
    if (m_wifiNamePart.isEmpty())
        return false;
    HANDLE clientHandle = nullptr;
    DWORD negotiatedVersion = 0;
    if (WlanOpenHandle(2, nullptr, &negotiatedVersion, &clientHandle) != ERROR_SUCCESS)
        return false;

    bool connectedToTarget = false;
    PWLAN_INTERFACE_INFO_LIST interfaceList = nullptr;
    if (WlanEnumInterfaces(clientHandle, nullptr, &interfaceList) == ERROR_SUCCESS
        && interfaceList) {
        for (DWORD i = 0; i < interfaceList->dwNumberOfItems; ++i) {
            DWORD dataSize = 0;
            WLAN_OPCODE_VALUE_TYPE opcodeType;
            PWLAN_CONNECTION_ATTRIBUTES attributes = nullptr;
            const DWORD result = WlanQueryInterface(
                clientHandle, &interfaceList->InterfaceInfo[i].InterfaceGuid,
                wlan_intf_opcode_current_connection, nullptr, &dataSize,
                reinterpret_cast<PVOID *>(&attributes), &opcodeType);
            if (result == ERROR_SUCCESS && attributes
                && attributes->isState == wlan_interface_state_connected) {
                const DOT11_SSID &dot11Ssid = attributes->wlanAssociationAttributes.dot11Ssid;
                const QString ssid = QString::fromUtf8(
                    reinterpret_cast<const char *>(dot11Ssid.ucSSID),
                    static_cast<int>(dot11Ssid.uSSIDLength));
                const QString profile = QString::fromWCharArray(attributes->strProfileName);
                connectedToTarget = ssid.contains(m_wifiNamePart, Qt::CaseInsensitive)
                                    || profile.contains(m_wifiNamePart, Qt::CaseInsensitive);
            }
            if (attributes)
                WlanFreeMemory(attributes);
            if (connectedToTarget)
                break;
        }
        WlanFreeMemory(interfaceList);
    }
    WlanCloseHandle(clientHandle, nullptr);
    return connectedToTarget;
#else
    return false;
#endif
}

void TcpController::checkWifiConnection()
{
    const bool connectedNow = isConnectedToTargetWifi();
    if (m_wifiConnected != connectedNow) {
        m_wifiConnected = connectedNow;
        emit wifiConnectedChanged();
        appendLog(connectedNow
                      ? QStringLiteral("设备 Wi-Fi 连接成功: %1").arg(m_wifiNamePart)
                      : QStringLiteral("目标设备 Wi-Fi 未连接"));
    }

    if (!m_taskActive)
        return;

    if (m_flowState == FlowState::ConnectingWifi) {
        if (connectedNow) {
            stopStepTimer();
            connectDeviceTcp();
        } else {
            ++m_wifiConnectTicks;
            if (m_wifiConnectTicks % 3 == 0)
                connectWifiByNamePart(m_wifiNamePart);
        }
        return;
    }

    const bool wifiRequired = m_flowState == FlowState::ConnectingDevice
                              || m_flowState == FlowState::ReadingDeviceInfo
                              || m_flowState == FlowState::ConnectingMqtt
                              || m_flowState == FlowState::StartingCollection
                              || m_flowState == FlowState::Calibrating
                              || m_flowState == FlowState::StoppingCollection
                              || m_flowState == FlowState::Downloading;
    if (wifiRequired && !connectedNow)
        failCurrentSlot(QStringLiteral("设备 Wi-Fi 连接中断"));
}

void TcpController::connectWifiByNamePart(const QString &namePart)
{
#ifdef Q_OS_WIN
    HANDLE clientHandle = nullptr;
    DWORD negotiatedVersion = 0;
    DWORD result = WlanOpenHandle(2, nullptr, &negotiatedVersion, &clientHandle);
    if (result != ERROR_SUCCESS) {
        appendLog(QStringLiteral("无法打开 Windows Wi-Fi 服务，错误码: %1").arg(result));
        return;
    }

    PWLAN_INTERFACE_INFO_LIST interfaceList = nullptr;
    result = WlanEnumInterfaces(clientHandle, nullptr, &interfaceList);
    if (result != ERROR_SUCCESS || !interfaceList || interfaceList->dwNumberOfItems == 0) {
        appendLog(QStringLiteral("未找到可用无线网卡，错误码: %1").arg(result));
        if (interfaceList)
            WlanFreeMemory(interfaceList);
        WlanCloseHandle(clientHandle, nullptr);
        return;
    }

    bool networkFound = false;
    bool connectStarted = false;
    for (DWORD interfaceIndex = 0;
         interfaceIndex < interfaceList->dwNumberOfItems && !connectStarted;
         ++interfaceIndex) {
        const WLAN_INTERFACE_INFO &interfaceInfo = interfaceList->InterfaceInfo[interfaceIndex];
        WlanScan(clientHandle, &interfaceInfo.InterfaceGuid, nullptr, nullptr, nullptr);
        PWLAN_AVAILABLE_NETWORK_LIST networkList = nullptr;
        result = WlanGetAvailableNetworkList(clientHandle, &interfaceInfo.InterfaceGuid,
                                             0, nullptr, &networkList);
        if (result != ERROR_SUCCESS || !networkList)
            continue;

        for (DWORD networkIndex = 0; networkIndex < networkList->dwNumberOfItems; ++networkIndex) {
            WLAN_AVAILABLE_NETWORK &network = networkList->Network[networkIndex];
            const QString ssid = QString::fromUtf8(
                reinterpret_cast<const char *>(network.dot11Ssid.ucSSID),
                static_cast<int>(network.dot11Ssid.uSSIDLength));
            const QString profileName = QString::fromWCharArray(network.strProfileName);
            if (!ssid.contains(namePart, Qt::CaseInsensitive)
                && !profileName.contains(namePart, Qt::CaseInsensitive))
                continue;

            networkFound = true;
            WLAN_CONNECTION_PARAMETERS parameters = {};
            parameters.dot11BssType = network.dot11BssType;
            if (!profileName.isEmpty()) {
                parameters.wlanConnectionMode = wlan_connection_mode_profile;
                parameters.strProfile = network.strProfileName;
            } else if (network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_80211_OPEN
                       && network.dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_NONE) {
                parameters.wlanConnectionMode = wlan_connection_mode_discovery_unsecure;
                parameters.pDot11Ssid = &network.dot11Ssid;
            } else {
                appendLog(QStringLiteral("找到 Wi-Fi“%1”，但没有已保存的密码配置")
                              .arg(profileName.isEmpty() ? ssid : profileName));
                continue;
            }

            result = WlanConnect(clientHandle, &interfaceInfo.InterfaceGuid, &parameters, nullptr);
            if (result == ERROR_SUCCESS) {
                connectStarted = true;
                appendLog(QStringLiteral("已发起 Wi-Fi 连接: %1")
                              .arg(profileName.isEmpty() ? ssid : profileName));
            }
            break;
        }
        WlanFreeMemory(networkList);
    }

    if (!networkFound)
        appendLog(QStringLiteral("未找到名称包含“%1”的 Wi-Fi").arg(namePart));
    else if (!connectStarted)
        appendLog(QStringLiteral("未能发起目标 Wi-Fi 连接"));

    WlanFreeMemory(interfaceList);
    WlanCloseHandle(clientHandle, nullptr);
#else
    Q_UNUSED(namePart)
    appendLog(QStringLiteral("自动连接 Wi-Fi 仅支持 Windows"));
#endif
}

void TcpController::connectDeviceTcp()
{
    m_deviceReceiveBuffer.clear();
    m_deviceSocket->abort();
    setFlowState(FlowState::ConnectingDevice,
                 QStringLiteral("正在连接设备信息端口 %1:%2")
                     .arg(m_deviceHost).arg(m_devicePort),
                 kDeviceConnectTimeoutMs);
    m_deviceSocket->connectToHost(m_deviceHost, static_cast<quint16>(m_devicePort));
}

void TcpController::onDeviceConnected()
{
    m_deviceTcpConnected = true;
    emit deviceTcpConnectedChanged();
    appendLog(QStringLiteral("设备 TCP 已连接: %1:%2").arg(m_deviceHost).arg(m_devicePort));
    setFlowState(FlowState::ReadingDeviceInfo,
                 QStringLiteral("正在读取设备信息"),
                 kDeviceInfoTimeoutMs);
    requestDeviceInfo();
}

void TcpController::onDeviceDisconnected()
{
    if (m_deviceTcpConnected) {
        m_deviceTcpConnected = false;
        emit deviceTcpConnectedChanged();
    }
    if (!m_cleaningUp && m_taskActive
        && (m_flowState == FlowState::ConnectingDevice
            || m_flowState == FlowState::ReadingDeviceInfo)) {
        failCurrentSlot(QStringLiteral("设备信息 TCP 连接断开"));
    }
}

void TcpController::onDeviceError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError)
    appendLog(QStringLiteral("设备 TCP 错误: %1").arg(m_deviceSocket->errorString()));
    if (!m_cleaningUp && m_taskActive
        && (m_flowState == FlowState::ConnectingDevice
            || m_flowState == FlowState::ReadingDeviceInfo)) {
        failCurrentSlot(QStringLiteral("无法连接设备信息端口"));
    }
}

void TcpController::requestDeviceInfo()
{
    if (m_deviceSocket->state() != QAbstractSocket::ConnectedState) {
        failCurrentSlot(QStringLiteral("设备 TCP 未连接"));
        return;
    }
    const QByteArray request("unlogall thisport\r\n$$SI\r\nlog versionb\r\n");
    if (m_deviceSocket->write(request) < 0) {
        failCurrentSlot(QStringLiteral("设备信息请求发送失败"));
        return;
    }
    m_deviceSocket->flush();
    appendLog(QStringLiteral("已请求设备 SN、硬件号和版本号"));
}

void TcpController::onDeviceReadyRead()
{
    m_deviceReceiveBuffer.append(m_deviceSocket->readAll());
    processDeviceBuffer();
}

void TcpController::processDeviceBuffer()
{
    bool changed = false;
    while (true) {
        const int frameStart = m_deviceReceiveBuffer.indexOf("$$SI");
        if (frameStart < 0 || m_deviceReceiveBuffer.size() < frameStart + 62)
            break;
        const QByteArray frame = m_deviceReceiveBuffer.mid(frameStart, 62);
        quint8 crc = 0;
        for (int i = 0; i < 61; ++i)
            crc ^= static_cast<quint8>(frame.at(i));
        const quint8 receivedCrc = static_cast<quint8>(frame.at(61));
        if (crc == receivedCrc) {
            const QString hardware = QString::fromLatin1(frame.mid(29, 3)).trimmed();
            if (!hardware.isEmpty() && hardware != m_currentHardware) {
                m_currentHardware = hardware;
                changed = true;
                appendLog(QStringLiteral("设备硬件号: %1").arg(hardware));
            }
        } else {
            appendLog(QStringLiteral("设备 $$SI 帧 CRC 校验失败"));
        }
        m_deviceReceiveBuffer.remove(0, frameStart + 62);
    }

    int lineEnd = -1;
    while ((lineEnd = m_deviceReceiveBuffer.indexOf('\n')) >= 0) {
        QByteArray lineData = m_deviceReceiveBuffer.left(lineEnd);
        m_deviceReceiveBuffer.remove(0, lineEnd + 1);
        lineData.replace('\0', ' ');
        const QString line = QString::fromUtf8(lineData).trimmed();
        if (!line.isEmpty()) {
            const QString oldSn = m_currentSn;
            const QString oldHw = m_currentHardware;
            const QString oldVersion = m_currentVersion;
            parseDeviceTextLine(line);
            changed = changed || oldSn != m_currentSn
                      || oldHw != m_currentHardware
                      || oldVersion != m_currentVersion;
        }
    }

    if (changed)
        emit currentDeviceChanged();
    tryCompleteDeviceInfo();
}

void TcpController::parseDeviceTextLine(const QString &line)
{
    appendLog(QStringLiteral("设备信息响应: %1").arg(line));

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isObject()) {
        const QJsonObject object = document.object();
        const QString sn = object.value(QStringLiteral("sn")).toString();
        const QString hw = object.value(QStringLiteral("hw")).toString();
        const QString version = object.value(QStringLiteral("version")).toString();
        if (!sn.isEmpty()) m_currentSn = sn;
        if (!hw.isEmpty()) m_currentHardware = hw;
        if (!version.isEmpty()) m_currentVersion = version;
        return;
    }

    const QRegularExpression snExpression(
        QStringLiteral("(?:^|[,\\s])SN\\s*[:=,]\\s*([A-Za-z0-9_-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression hwExpression(
        QStringLiteral("(?:^|[,\\s])(?:HW|HARDWARE)\\s*[:=,]\\s*([A-Za-z0-9_.-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression versionExpression(
        QStringLiteral("(?:VERSIONB?|VER)\\s*[:=, ]+\\s*([^,;\\s]+)"),
        QRegularExpression::CaseInsensitiveOption);

    QRegularExpressionMatch match = snExpression.match(line);
    if (match.hasMatch())
        m_currentSn = match.captured(1);
    match = hwExpression.match(line);
    if (match.hasMatch())
        m_currentHardware = match.captured(1);
    match = versionExpression.match(line);
    if (match.hasMatch())
        m_currentVersion = match.captured(1);
}

void TcpController::tryCompleteDeviceInfo()
{
    if (m_flowState != FlowState::ReadingDeviceInfo)
        return;
    if (m_currentHardware.isEmpty())
        return;

    stopStepTimer();
    appendLog(QStringLiteral("设备信息读取完成，SN=%1，HW=%2，版本=%3")
                  .arg(m_currentSn,
                       m_currentHardware,
                       m_currentVersion.isEmpty() ? QStringLiteral("--") : m_currentVersion));
    emit currentDeviceChanged();
    connectMqtt();
    m_deviceSocket->disconnectFromHost();
}

void TcpController::connectMqtt()
{
    setFlowState(FlowState::ConnectingMqtt,
                 QStringLiteral("正在连接 MQTT 并准备采集"),
                 kMqttTimeoutMs);
    if (m_mqttClient->state() == QMqttClient::Connected) {
        if (!publishCollectAction(QStringLiteral("AC_START")))
            failCurrentSlot(QStringLiteral("MQTT 发布 AC_START 失败"));
    } else if (m_mqttClient->state() == QMqttClient::Disconnected) {
        m_mqttSubscribed = false;
        m_mqttClient->connectToHost();
    }
}

bool TcpController::publishCollectAction(const QString &action)
{
    if (m_mqttClient->state() != QMqttClient::Connected)
        return false;
    const QMqttTopicName topic(QStringLiteral("app_slam/request/data_collect"));
    const QJsonObject object{{QStringLiteral("action"), action}};
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const qint32 messageId = m_mqttClient->publish(topic, payload, 1, false);
    if (messageId < 0)
        return false;

    appendLog(QStringLiteral("MQTT 发布 %1，消息 ID=%2").arg(action).arg(messageId));
    if (action == QStringLiteral("AC_START")) {
        setFlowState(FlowState::StartingCollection,
                     QStringLiteral("等待设备开始采集"),
                     kCollectTimeoutMs);
    } else {
        setFlowState(FlowState::StoppingCollection,
                     QStringLiteral("等待设备停止采集"),
                     kStopCollectTimeoutMs);
    }
    return true;
}

void TcpController::handleMqttMessage(const QByteArray &message, const QString &topic)
{
    if (topic != QStringLiteral("slam_app/response/data_collect"))
        return;
    appendLog(QStringLiteral("MQTT 收到采集响应: %1").arg(QString::fromUtf8(message)));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(message, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        failCurrentSlot(QStringLiteral("MQTT 采集响应格式错误"));
        return;
    }

    const QJsonObject object = document.object();
    const int errorCode = object.value(QStringLiteral("error")).toInt(-1);
    const QString description = object.value(QStringLiteral("desc")).toString().trimmed();
    const QString path = object.value(QStringLiteral("path")).toString().trimmed();
    if (!path.isEmpty() && path != m_collectPath) {
        m_collectPath = path;
        emit collectPathChanged();
    }

    if (errorCode != 0) {
        m_collectStatus = QStringLiteral("采集异常");
        emit collectStatusChanged();
        failCurrentSlot(QStringLiteral("设备采集失败，错误码 %1").arg(errorCode));
        return;
    }

    if (description.compare(QStringLiteral("collecting"), Qt::CaseInsensitive) == 0) {
        m_collectStatus = QStringLiteral("采集中");
        emit collectStatusChanged();
        if (m_flowState == FlowState::StartingCollection) {
            stopStepTimer();
            if (!sendArmCommand(QStringLiteral("$$MT,START"))) {
                failCurrentSlot(QStringLiteral("无法通知机械臂开始标定"));
                return;
            }
            setFlowState(FlowState::Calibrating,
                         QStringLiteral("机械臂正在执行 SLAM 标定轨迹"),
                         kCalibrationTimeoutMs);
        }
        return;
    }

    if (description.compare(QStringLiteral("collect stopped"), Qt::CaseInsensitive) == 0) {
        m_collectStatus = QStringLiteral("采集已停止");
        emit collectStatusChanged();
        if (m_flowState == FlowState::StoppingCollection) {
            stopStepTimer();
            startFtpDownload();
        }
        return;
    }

    m_collectStatus = description.isEmpty() ? QStringLiteral("未知状态") : description;
    emit collectStatusChanged();
}

void TcpController::startFtpDownload()
{
    if (!m_wifiConnected) {
        failCurrentSlot(QStringLiteral("FTP 下载前设备 Wi-Fi 已断开"));
        return;
    }
    if (m_collectPath.isEmpty()) {
        failCurrentSlot(QStringLiteral("设备未返回采集目录"));
        return;
    }

    m_curlExecutable = QStandardPaths::findExecutable(QStringLiteral("curl.exe"));
    if (m_curlExecutable.isEmpty())
        m_curlExecutable = QStandardPaths::findExecutable(QStringLiteral("curl"));
    if (m_curlExecutable.isEmpty()) {
        failCurrentSlot(QStringLiteral("系统中未找到 curl，无法下载 FTP 数据"));
        return;
    }

    QString remotePath = m_collectPath;
    if (!remotePath.startsWith(QLatin1Char('/')))
        remotePath.prepend(QLatin1Char('/'));
    if (!remotePath.endsWith(QLatin1Char('/')))
        remotePath.append(QLatin1Char('/'));
    m_ftpUrl.setHost(m_deviceHost);
    m_ftpUrl.setPort(21);
    m_ftpUrl.setPath(remotePath);

    const QString directoryName = QDir::cleanPath(m_collectPath).section(QLatin1Char('/'), -1);
    m_ftpLocalDir = QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                        .filePath(QStringLiteral("SLAM_calibration/%1")
                                      .arg(directoryName.isEmpty()
                                               ? QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss"))
                                               : directoryName));
    if (!QDir().mkpath(m_ftpLocalDir)) {
        failCurrentSlot(QStringLiteral("无法创建下载目录: %1").arg(m_ftpLocalDir));
        return;
    }

    m_ftpFiles.clear();
    m_ftpPhase = 1;
    m_ftpTotalFiles = 0;
    m_ftpDownloadedFiles = 0;
    m_ftpDownloading = true;
    emit ftpDownloadingChanged();
    setFlowState(FlowState::Downloading, QStringLiteral("正在读取 FTP 数据目录"));

    QStringList arguments;
    arguments << QStringLiteral("--fail")
              << QStringLiteral("--show-error")
              << QStringLiteral("--silent")
              << QStringLiteral("--connect-timeout") << QStringLiteral("10")
              << QStringLiteral("--list-only")
              << QStringLiteral("--ftp-pasv")
              << QStringLiteral("--user")
              << QStringLiteral("%1:%2").arg(m_ftpUser, m_ftpPassword)
              << QString::fromUtf8(m_ftpUrl.toEncoded());
    appendLog(QStringLiteral("读取 FTP 目录: %1").arg(m_ftpUrl.toString()));
    m_ftpProcess->start(m_curlExecutable, arguments);
}

void TcpController::handleFtpProcessFinished(int exitCode, int exitStatus)
{
    Q_UNUSED(exitStatus)
    if (!m_ftpDownloading || m_flowState != FlowState::Downloading)
        return;

    const QString errorText = QString::fromLocal8Bit(m_ftpProcess->readAllStandardError()).trimmed();
    if (m_ftpPhase == 1) {
        if (exitCode != 0) {
            m_ftpDownloading = false;
            emit ftpDownloadingChanged();
            failCurrentSlot(QStringLiteral("读取 FTP 目录失败: %1")
                                .arg(errorText.isEmpty() ? QString::number(exitCode) : errorText));
            return;
        }
        const QString listing = QString::fromUtf8(m_ftpProcess->readAllStandardOutput());
        const QStringList lines = listing.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                                Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QString fileName = line.trimmed();
            if (!fileName.isEmpty() && fileName != QStringLiteral(".")
                && fileName != QStringLiteral("..") && !fileName.endsWith(QLatin1Char('/')))
                m_ftpFiles.append(fileName);
        }
        if (m_ftpFiles.isEmpty()) {
            m_ftpDownloading = false;
            emit ftpDownloadingChanged();
            failCurrentSlot(QStringLiteral("FTP 采集目录中没有文件"));
            return;
        }
        m_ftpPhase = 2;
        m_ftpTotalFiles = m_ftpFiles.size();
        appendLog(QStringLiteral("FTP 共 %1 个文件").arg(m_ftpTotalFiles));
        startNextFtpFile();
        return;
    }

    if (m_ftpPhase == 2) {
        if (exitCode != 0) {
            m_ftpDownloading = false;
            emit ftpDownloadingChanged();
            failCurrentSlot(QStringLiteral("FTP 文件下载失败: %1")
                                .arg(errorText.isEmpty() ? QString::number(exitCode) : errorText));
            return;
        }
        ++m_ftpDownloadedFiles;
        startNextFtpFile();
    }
}

void TcpController::startNextFtpFile()
{
    if (m_ftpFiles.isEmpty()) {
        finishFtpDownload();
        return;
    }

    const QString fileName = m_ftpFiles.takeFirst();
    QUrl fileUrl = m_ftpUrl;
    fileUrl.setPath(m_ftpUrl.path() + fileName);
    const QString localFile = QDir(m_ftpLocalDir).filePath(QFileInfo(fileName).fileName());
    setProgress(QStringLiteral("正在下载标定数据 %1/%2")
                    .arg(m_ftpDownloadedFiles + 1).arg(m_ftpTotalFiles));

    QStringList arguments;
    arguments << QStringLiteral("--fail")
              << QStringLiteral("--show-error")
              << QStringLiteral("--silent")
              << QStringLiteral("--connect-timeout") << QStringLiteral("10")
              << QStringLiteral("--max-time") << QStringLiteral("300")
              << QStringLiteral("--ftp-pasv")
              << QStringLiteral("--user")
              << QStringLiteral("%1:%2").arg(m_ftpUser, m_ftpPassword)
              << QStringLiteral("--output") << localFile
              << QString::fromUtf8(fileUrl.toEncoded());
    appendLog(QStringLiteral("下载 FTP 文件: %1").arg(fileName));
    m_ftpProcess->start(m_curlExecutable, arguments);
}

void TcpController::finishFtpDownload()
{
    m_ftpDownloading = false;
    m_ftpPhase = 0;
    emit ftpDownloadingChanged();
    appendLog(QStringLiteral("FTP 数据下载完成: %1").arg(m_ftpLocalDir));
    requestPutBack(false);
}

void TcpController::onStepTimeout()
{
    if (!m_taskActive)
        return;
    switch (m_flowState) {
    case FlowState::WaitingScan:
        failCurrentSlot(QStringLiteral("等待机械臂扫码超时"));
        break;
    case FlowState::ConnectingWifi:
        failCurrentSlot(QStringLiteral("连接设备 Wi-Fi 超时"));
        break;
    case FlowState::ConnectingDevice:
        failCurrentSlot(QStringLiteral("连接设备信息端口超时"));
        break;
    case FlowState::ReadingDeviceInfo:
        failCurrentSlot(QStringLiteral("读取设备硬件信息超时"));
        break;
    case FlowState::ConnectingMqtt:
        failCurrentSlot(QStringLiteral("连接 MQTT 超时"));
        break;
    case FlowState::StartingCollection:
        failCurrentSlot(QStringLiteral("等待设备开始采集超时"));
        break;
    case FlowState::Calibrating:
        failCurrentSlot(QStringLiteral("机械臂标定运行超时"));
        break;
    case FlowState::StoppingCollection:
        failCurrentSlot(QStringLiteral("等待设备停止采集超时"));
        break;
    case FlowState::WaitingPutBack:
        setCurrentSlotState(QStringLiteral("failed"));
        setLastError(QStringLiteral("等待机械臂放回设备超时"));
        recordCurrentResult(QStringLiteral("failed"), m_lastError);
        if (m_aborting) {
            sendArmCommand(QStringLiteral("$$MT,OVER"));
            m_taskActive = false;
            m_aborting = false;
            emit armRunningChanged();
            cleanupDeviceConnections();
            setFlowState(FlowState::Aborted, QStringLiteral("任务已中止，放回设备未确认"));
        } else {
            startNextSlot();
        }
        break;
    default:
        break;
    }
}

void TcpController::setProgress(const QString &text)
{
    if (m_currentProgress == text)
        return;
    m_currentProgress = text;
    emit currentProgressChanged();
}

void TcpController::setFlowState(FlowState state, const QString &progress, int timeoutMs)
{
    m_flowState = state;
    setProgress(progress);
    stopStepTimer();
    if (timeoutMs > 0)
        m_stepTimer.start(timeoutMs);
}

void TcpController::setLastError(const QString &text)
{
    if (m_lastError == text)
        return;
    m_lastError = text;
    emit lastErrorChanged();
}

void TcpController::appendLog(const QString &text)
{
    const QString entry = QStringLiteral("[%1] %2")
                              .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")), text);
    m_debugLog.append(entry);
    while (m_debugLog.size() > 500)
        m_debugLog.removeFirst();
    emit debugLogChanged();
}

void TcpController::stopStepTimer()
{
    if (m_stepTimer.isActive())
        m_stepTimer.stop();
}

void TcpController::cleanupDeviceConnections()
{
    m_cleaningUp = true;
    if (m_ftpDownloading) {
        m_ftpDownloading = false;
        emit ftpDownloadingChanged();
    }
    if (m_ftpProcess->state() != QProcess::NotRunning)
        m_ftpProcess->kill();
    m_ftpPhase = 0;
    m_ftpFiles.clear();

    m_deviceSocket->abort();
    if (m_deviceTcpConnected) {
        m_deviceTcpConnected = false;
        emit deviceTcpConnectedChanged();
    }
    m_deviceReceiveBuffer.clear();

    if (m_mqttClient->state() != QMqttClient::Disconnected)
        m_mqttClient->disconnectFromHost();
    if (m_mqttConnected) {
        m_mqttConnected = false;
        emit mqttConnectedChanged();
    }
    m_mqttSubscribed = false;

    if (m_wifiConnected) {
        m_wifiConnected = false;
        emit wifiConnectedChanged();
    }
    m_cleaningUp = false;
}

void TcpController::updateStatistics()
{
    int successes = 0;
    int failures = 0;
    for (const QString &state : m_leftSlotStates) {
        if (state == QStringLiteral("success")) ++successes;
        if (state == QStringLiteral("failed")) ++failures;
    }
    for (const QString &state : m_rightSlotStates) {
        if (state == QStringLiteral("success")) ++successes;
        if (state == QStringLiteral("failed")) ++failures;
    }
    if (successes == m_successCount && failures == m_failureCount)
        return;
    m_successCount = successes;
    m_failureCount = failures;
    emit statisticsChanged();
}
