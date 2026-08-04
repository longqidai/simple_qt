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
const int kMqttTimeoutMs = 25000;
const int kDeviceReadyTimeoutMs = 90000;
const int kMqttReconnectBaseDelayMs = 1200;
const int kWifiDisconnectGraceTicks = 4;
const int kProjectTimeoutMs = 20000;
const int kTaskTimeoutMs = 20000;
const int kPreinitTimeoutMs = 60000;
const int kModelingStartTimeoutMs = 120000;
const int kCollectTimeoutMs = 20000;
const int kArmHomeTimeoutMs = 120000;
const int kCalibrationTimeoutMs = 10 * 60 * 1000;
const int kStopCollectTimeoutMs = 30000;
const int kModelingStopTimeoutMs = 5 * 60 * 1000;
const int kPutBackTimeoutMs = 120000;
const int kGuaranteedStopRetryMs = 8000;

const quint32 kProtocolSuccess = 0xF2040001u;

struct ProtoField {
    int number = 0;
    int wireType = 0;
    quint64 value = 0;
    QByteArray bytes;
};

void appendProtoVarint(QByteArray &output, quint64 value)
{
    while (value >= 0x80u) {
        output.append(static_cast<char>((value & 0x7fu) | 0x80u));
        value >>= 7u;
    }
    output.append(static_cast<char>(value));
}

void appendProtoKey(QByteArray &output, int fieldNumber, int wireType)
{
    appendProtoVarint(output, (static_cast<quint64>(fieldNumber) << 3u)
                                  | static_cast<quint64>(wireType));
}

void appendProtoUInt(QByteArray &output, int fieldNumber, quint64 value)
{
    appendProtoKey(output, fieldNumber, 0);
    appendProtoVarint(output, value);
}

void appendProtoSInt(QByteArray &output, int fieldNumber, qint64 value)
{
    const quint64 magnitude = value < 0
                                  ? static_cast<quint64>(-(value + 1)) + 1u
                                  : static_cast<quint64>(value);
    const quint64 encoded = value < 0 ? magnitude * 2u - 1u : magnitude * 2u;
    appendProtoUInt(output, fieldNumber, encoded);
}

void appendProtoBytes(QByteArray &output, int fieldNumber, const QByteArray &value)
{
    appendProtoKey(output, fieldNumber, 2);
    appendProtoVarint(output, static_cast<quint64>(value.size()));
    output.append(value);
}

QByteArray makeProtoHeader(quint32 sequence, const QString &sessionId)
{
    QByteArray header;
    appendProtoUInt(header, 1, sequence);
    appendProtoSInt(header, 2, QDateTime::currentMSecsSinceEpoch() * 1000000ll);
    appendProtoBytes(header, 3, sessionId.toUtf8());
    return header;
}

bool readProtoVarint(const QByteArray &input, int &position, quint64 &value)
{
    value = 0;
    int shift = 0;
    while (position < input.size() && shift < 70) {
        const quint8 byte = static_cast<quint8>(input.at(position++));
        value |= static_cast<quint64>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0)
            return true;
        shift += 7;
    }
    return false;
}

bool readProtoField(const QByteArray &input, int &position, ProtoField &field)
{
    quint64 key = 0;
    if (!readProtoVarint(input, position, key) || key == 0)
        return false;
    field = ProtoField{};
    field.number = static_cast<int>(key >> 3u);
    field.wireType = static_cast<int>(key & 0x07u);

    switch (field.wireType) {
    case 0:
        return readProtoVarint(input, position, field.value);
    case 1:
        if (position + 8 > input.size())
            return false;
        position += 8;
        return true;
    case 2: {
        quint64 size = 0;
        if (!readProtoVarint(input, position, size)
            || size > static_cast<quint64>(input.size() - position)) {
            return false;
        }
        field.bytes = input.mid(position, static_cast<int>(size));
        position += static_cast<int>(size);
        return true;
    }
    case 5:
        if (position + 4 > input.size())
            return false;
        position += 4;
        return true;
    default:
        return false;
    }
}

bool protoVarintField(const QByteArray &message, int wantedField, quint64 &value)
{
    int position = 0;
    ProtoField field;
    while (position < message.size() && readProtoField(message, position, field)) {
        if (field.number == wantedField && field.wireType == 0) {
            value = field.value;
            return true;
        }
    }
    return false;
}

bool protoBytesField(const QByteArray &message, int wantedField, QByteArray &value)
{
    int position = 0;
    ProtoField field;
    while (position < message.size() && readProtoField(message, position, field)) {
        if (field.number == wantedField && field.wireType == 2) {
            value = field.bytes;
            return true;
        }
    }
    return false;
}

qint64 decodeProtoSInt(quint64 value)
{
    return static_cast<qint64>((value >> 1u) ^ (quint64(0) - (value & 1u)));
}

struct ProtoError {
    quint32 code = 0;
    QString description;
};

ProtoError parseProtoErrorAt(const QByteArray &response, int fieldNumber)
{
    ProtoError result;
    QByteArray errorMessage;
    if (!protoBytesField(response, fieldNumber, errorMessage))
        return result;
    quint64 code = 0;
    if (protoVarintField(errorMessage, 1, code))
        result.code = static_cast<quint32>(code);
    QByteArray description;
    if (protoBytesField(errorMessage, 2, description))
        result.description = QString::fromUtf8(description);
    return result;
}

ProtoError parseProtoError(const QByteArray &response)
{
    return parseProtoErrorAt(response, 2);
}

bool protoSucceeded(const ProtoError &error)
{
    return error.code == 0 || error.code == kProtocolSuccess;
}

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
    , m_batchDownloadPhase(false)
    , m_armSequenceFinished(false)
    , m_armMotionStarted(false)
    , m_armMotionFinished(false)
    , m_armAtHomeReady(false)
    , m_collectReadyForModeling(false)
    , m_guaranteedStopActive(false)
    , m_guaranteedStopAfterAbort(false)
    , m_guaranteedStopNeedsPutBack(false)
    , m_guaranteedStopAttempts(0)
    , m_statusText(QStringLiteral("未连接"))
    , m_currentProgress(QStringLiteral("等待连接机械臂"))
    , m_host(QStringLiteral("192.168.5.1"))
    , m_port(6601)
    , m_handshakeAttempts(0)
    , m_wifiConnectTicks(0)
    , m_wifiDisconnectTicks(0)
    , m_flowState(FlowState::Idle)
    , m_currentQueueIndex(-1)
    , m_currentDownloadIndex(-1)
    , m_selectedCount(0)
    , m_successCount(0)
    , m_failureCount(0)
    , m_deviceHost(QStringLiteral("192.168.1.8"))
    , m_devicePort(25001)
    , m_mqttHost(QStringLiteral("192.168.1.8"))
    , m_mqttPort(1883)
    , m_mqttPortCandidates({1883, 26001})
    , m_mqttPortCandidateIndex(0)
    , m_mqttPortFailureCount(0)
    , m_mqttReconnectAttempts(0)
    , m_protoSequence(0)
    , m_protoSessionId(QStringLiteral("slam-calib-%1")
                           .arg(QDateTime::currentMSecsSinceEpoch()))
    , m_modelingProjectId(-1)
    , m_modelingTaskId(-1)
    , m_modelingStatus(-1)
    , m_modelingActive(false)
    , m_collectActive(false)
    , m_deviceStatusReceived(false)
    , m_deviceInitReady(false)
    , m_retryModelingAfterDeviceReady(false)
    , m_resumeModelingAfterPreinit(false)
    , m_modelingStartRetries(0)
    , m_collectStartRecoveryAttempts(0)
    , m_retryCollectAfterStop(false)
    , m_ftpUrl(QStringLiteral("ftp://192.168.1.8:21/"))
    , m_ftpUser(QStringLiteral("ftpuser"))
    , m_ftpPassword(QStringLiteral("ftpuser"))
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

    m_deviceRetryTimer.setSingleShot(true);
    connect(&m_deviceRetryTimer, &QTimer::timeout, this, [this]() {
        if (m_taskActive && m_wifiConnected
            && m_flowState == FlowState::ConnectingDevice) {
            connectDeviceTcp();
        }
    });

    m_mqttReconnectTimer.setSingleShot(true);
    connect(&m_mqttReconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_taskActive || m_cleaningUp || !m_wifiConnected
            || m_mqttClient->state() != QMqttClient::Disconnected) {
            return;
        }
        ++m_mqttReconnectAttempts;
        appendLog(QStringLiteral("MQTT 自动重连第 %1 次: %2:%3")
                      .arg(m_mqttReconnectAttempts).arg(m_mqttHost).arg(m_mqttPort));
        m_mqttSubscribed = false;
        m_mqttClient->setHostname(m_mqttHost);
        m_mqttClient->setPort(static_cast<quint16>(m_mqttPort));
        m_mqttClient->connectToHost();
    });

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
        const bool wasConnected = m_mqttConnected;
        if (m_mqttConnected != connectedNow) {
            m_mqttConnected = connectedNow;
            emit mqttConnectedChanged();
        }

        if (!connectedNow) {
            if (m_taskActive && !m_cleaningUp
                && state == QMqttClient::Disconnected) {
                if (wasConnected)
                    appendLog(QStringLiteral("MQTT 连接暂时中断，进入自动恢复流程"));
                scheduleMqttReconnect();
            }
            return;
        }

        const bool reconnected = m_mqttReconnectAttempts > 0;
        m_mqttReconnectTimer.stop();
        m_mqttPortFailureCount = 0;
        m_mqttReconnectAttempts = 0;
        appendLog(QStringLiteral("MQTT 已连接: %1:%2").arg(m_mqttHost).arg(m_mqttPort));
        if (!m_mqttSubscribed)
            m_mqttSubscribed = subscribeMqttTopics();

        if (m_guaranteedStopActive) {
            if (!m_mqttSubscribed) {
                appendLog(QStringLiteral("兜底停止阶段 MQTT 订阅失败，保持当前设备连接并继续重试"));
                setFlowState(FlowState::EnsuringMeasurementStopped,
                             QStringLiteral("正在确保设备结束测量：等待 MQTT 恢复"),
                             kGuaranteedStopRetryMs);
                return;
            }
            QTimer::singleShot(200, this, &TcpController::continueGuaranteedMeasurementStop);
            return;
        }

        if (m_taskActive && m_flowState == FlowState::ConnectingMqtt) {
            if (!m_mqttSubscribed) {
                failCurrentSlot(QStringLiteral("MQTT 扫描控制主题订阅失败"));
                return;
            }
            QTimer::singleShot(300, this, [this]() {
                if (!m_taskActive || m_flowState != FlowState::ConnectingMqtt)
                    return;
                setFlowState(FlowState::WaitingDeviceReady,
                             QStringLiteral("MQTT 已连接，等待设备初始化完成"),
                             kDeviceReadyTimeoutMs);
                appendLog(QStringLiteral("等待设备状态 init_ready=1 后再启动扫描流程"));
                if (m_deviceStatusReceived && m_deviceInitReady) {
                    appendLog(QStringLiteral("已缓存设备状态 init_ready=1，直接创建扫描工程"));
                    QTimer::singleShot(0, this, [this]() {
                        if (!m_taskActive || m_flowState != FlowState::WaitingDeviceReady
                            || m_mqttClient->state() != QMqttClient::Connected) {
                            return;
                        }
                        if (!publishProjectCreate())
                            failCurrentSlot(QStringLiteral("设备就绪后无法创建扫描工程"));
                    });
                    return;
                }
                QTimer::singleShot(6000, this, [this]() {
                    if (!m_taskActive || m_flowState != FlowState::WaitingDeviceReady
                        || m_deviceStatusReceived || m_retryModelingAfterDeviceReady)
                        return;
                    appendLog(QStringLiteral("6 秒内未收到 device_status，按旧固件兼容模式尝试启动；若设备未就绪将自动等待并重试"));
                    if (!publishProjectCreate())
                        failCurrentSlot(QStringLiteral("兼容模式下无法创建扫描工程"));
                });
            });
        } else if (reconnected && m_taskActive) {
            appendLog(QStringLiteral("MQTT 已自动恢复，继续当前流程"));
            QTimer::singleShot(300, this, &TcpController::resumeMqttOperation);
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
        const QString errorName = error == QMqttClient::TransportInvalid
                                      ? QStringLiteral("TransportInvalid（底层 TCP 未建立）")
                                      : QStringLiteral("错误码 %1").arg(static_cast<int>(error));
        appendLog(QStringLiteral("MQTT %1，目标 %2:%3")
                      .arg(errorName, m_mqttHost).arg(m_mqttPort));
        if (m_taskActive && m_flowState == FlowState::ConnectingMqtt
            && error == QMqttClient::TransportInvalid) {
            ++m_mqttPortFailureCount;
            if (m_mqttPortFailureCount < 3) {
                appendLog(QStringLiteral("MQTT %1:%2 暂时不可用，先重试当前端口（%3/3）")
                              .arg(m_mqttHost).arg(m_mqttPort)
                              .arg(m_mqttPortFailureCount));
            } else if (m_mqttPortCandidateIndex + 1 < m_mqttPortCandidates.size()) {
                ++m_mqttPortCandidateIndex;
                m_mqttPort = m_mqttPortCandidates.at(m_mqttPortCandidateIndex);
                m_mqttPortFailureCount = 0;
                appendLog(QStringLiteral("MQTT 当前端口连续失败，改试 %1:%2")
                              .arg(m_mqttHost).arg(m_mqttPort));
            } else {
                appendLog(QStringLiteral("MQTT 所有候选端口均不可用，继续在超时范围内重连"));
            }
            m_mqttClient->disconnectFromHost();
            QTimer::singleShot(800, this, [this]() {
                if (!m_taskActive || m_flowState != FlowState::ConnectingMqtt)
                    return;
                m_mqttClient->setPort(static_cast<quint16>(m_mqttPort));
                m_mqttSubscribed = false;
                m_mqttClient->connectToHost();
            });
            return;
        }
        if (m_taskActive && !m_cleaningUp) {
            appendLog(QStringLiteral("MQTT 异常暂不判定槽位失败，将自动重连"));
            scheduleMqttReconnect();
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
    m_deferredDownloads.clear();
    m_currentQueueIndex = -1;
    m_currentDownloadIndex = -1;
    m_batchDownloadPhase = false;
    m_armSequenceFinished = false;
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
    if (!m_batchDownloadPhase)
        stopStepTimer();
    m_statusText = QStringLiteral("未连接");
    emit statusTextChanged();
    appendLog(QStringLiteral("机械臂连接已断开"));

    // All devices have already been returned before the deferred-download phase.
    // An arm disconnect must not discard the data that is still waiting on FTP.
    if (wasTaskActive && m_batchDownloadPhase) {
        appendLog(QStringLiteral("机械臂已完成全部测量，继续执行统一下载"));
        return;
    }

    if (wasTaskActive && measurementMayBeActive()) {
        setCurrentSlotState(QStringLiteral("failed"));
        setLastError(QStringLiteral("机械臂连接中断"));
        recordCurrentResult(QStringLiteral("failed"), m_lastError);
        m_aborting = true;
        appendLog(QStringLiteral("机械臂连接中断，但设备可能仍在测量；先保持设备网络并执行兜底停止"));
        beginGuaranteedMeasurementStop(true, false, m_lastError);
        return;
    }

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
    if (!m_connected && !m_batchDownloadPhase)
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

    if (normalized.contains(QStringLiteral("HOMEOK"))) {
        m_armAtHomeReady = true;
        appendLog(QStringLiteral("机械臂已夹取设备、回到 Home 并完成静置（HOMEOK）"));
        tryStartModelingWhenArmHome();
        return;
    }

    if (normalized.contains(QStringLiteral("SCANREADYOK"))) {
        appendLog(QStringLiteral("机械臂已收到设备真实扫描就绪通知，开始执行 SLAM 标定轨迹"));
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
        if (m_armMotionStarted && !m_armMotionFinished) {
            m_armMotionFinished = true;
            appendLog(QStringLiteral("机械臂标定轨迹执行完成"));
        }
        if (m_flowState == FlowState::Calibrating && m_modelingStatus == 5) {
            appendLog(QStringLiteral("机械臂轨迹和设备真实扫描均已启动，停止并保存本次扫描"));
            if (!publishModelingAction(false))
                failCurrentSlot(QStringLiteral("无法通知设备停止真实扫描"));
        } else if (m_armMotionFinished
                   && m_flowState != FlowState::StoppingModeling
                   && m_flowState != FlowState::StoppingCollection
                   && m_flowState != FlowState::EnsuringMeasurementStopped
                   && m_flowState != FlowState::WaitingPutBack) {
            setProgress(QStringLiteral("机械臂轨迹已完成，等待设备进入真实扫描状态"));
            appendLog(QStringLiteral("机械臂先完成轨迹，继续等待设备进入 EC_ScanScanning"));
        }
        return;
    }

    const bool putBackDone = normalized.contains(QStringLiteral("PUTBACKOK"))
                             || (normalized == QStringLiteral("MT,OK")
                                 && m_flowState == FlowState::WaitingPutBack);
    if (putBackDone) {
        stopStepTimer();
        m_armMotionStarted = false;
        m_armMotionFinished = false;
        if (m_aborting) {
            sendArmCommand(QStringLiteral("$$MT,OVER"));
            m_taskActive = false;
            m_aborting = false;
            emit armRunningChanged();
            cleanupDeviceConnections();
            setFlowState(FlowState::Aborted, QStringLiteral("任务已中止"));
        } else {
            if (!m_failureAwaitingPutBack
                && currentSlotState() != QStringLiteral("waiting_download"))
                markCurrentSuccess();
            QTimer::singleShot(0, this, &TcpController::startNextSlot);
        }
        return;
    }

    if (normalized.contains(QStringLiteral("STARTOK"))) {
        if (m_armMotionStarted) {
            appendLog(QStringLiteral("机械臂已确认启动（STARTOK），正在夹取设备并返回 Home 静置"));
        } else {
            appendLog(QStringLiteral("收到机械臂 STARTOK，但本槽位尚未记录启动指令，仅记录该响应"));
        }
        return;
    }

    if (normalized == QStringLiteral("MT,OVER") && !m_batchDownloadPhase)
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
    m_deferredDownloads.clear();
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
    m_currentDownloadIndex = -1;
    m_taskActive = true;
    m_aborting = false;
    m_failureAwaitingPutBack = false;
    m_batchDownloadPhase = false;
    m_armSequenceFinished = false;
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

    if (m_batchDownloadPhase) {
        cleanupDeviceConnections();
        m_taskActive = false;
        m_aborting = false;
        m_batchDownloadPhase = false;
        emit armRunningChanged();
        setFlowState(FlowState::Aborted, QStringLiteral("统一下载任务已中止"));
        return;
    }

    if (measurementMayBeActive()) {
        beginGuaranteedMeasurementStop(true, stateNeedsPutBack(stateBeforeAbort),
                                       QStringLiteral("用户中止任务"));
        return;
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
    if (measurementMayBeActive()) {
        failCurrentSlot(QStringLiteral("切换下一槽位前检测到设备测量尚未停止"));
        return;
    }

    cleanupDeviceConnections();
    ++m_currentQueueIndex;
    if (m_currentQueueIndex >= m_taskQueue.size()) {
        beginBatchDownloads();
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
    if (m_taskActive && measurementMayBeActive()) {
        setCurrentSlotState(QStringLiteral("failed"));
        setLastError(QStringLiteral("任务结束时检测到设备测量尚未停止"));
        recordCurrentResult(QStringLiteral("failed"), m_lastError);
        m_aborting = true;
        beginGuaranteedMeasurementStop(true, stateNeedsPutBack(m_flowState), m_lastError);
        return;
    }

    stopStepTimer();
    if (m_taskActive && !m_armSequenceFinished)
        sendArmCommand(QStringLiteral("$$MT,OVER"));
    m_taskActive = false;
    m_aborting = false;
    m_failureAwaitingPutBack = false;
    m_batchDownloadPhase = false;
    emit armRunningChanged();
    cleanupDeviceConnections();
    setFlowState(FlowState::Completed,
                 QStringLiteral("全部测量与数据下载完成：成功 %1，失败 %2")
                     .arg(m_successCount).arg(m_failureCount));
    appendLog(QStringLiteral("SLAM 测量与统一下载任务完成：成功 %1，失败 %2")
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
    m_modelingProjectId = -1;
    m_modelingTaskId = -1;
    m_modelingStatus = -1;
    m_modelingActive = false;
    m_collectActive = false;
    m_armMotionStarted = false;
    m_armMotionFinished = false;
    m_armAtHomeReady = false;
    m_collectReadyForModeling = false;
    m_guaranteedStopActive = false;
    m_guaranteedStopAfterAbort = false;
    m_guaranteedStopNeedsPutBack = false;
    m_guaranteedStopAttempts = 0;
    m_guaranteedStopReason.clear();
    m_deviceStatusReceived = false;
    m_deviceInitReady = false;
    m_retryModelingAfterDeviceReady = false;
    m_resumeModelingAfterPreinit = false;
    m_modelingStartRetries = 0;
    m_collectStartRecoveryAttempts = 0;
    m_retryCollectAfterStop = false;
    m_ftpLocalDir.clear();
    m_armReceiveBuffer.squeeze();
    m_wifiConnectTicks = 0;
    m_wifiDisconnectTicks = 0;
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

void TcpController::queueCurrentDownload()
{
    if (m_currentQueueIndex < 0 || m_currentQueueIndex >= m_taskQueue.size())
        return;

    for (const DeferredDownload &download : m_deferredDownloads) {
        if (download.taskIndex == m_currentQueueIndex)
            return;
    }

    m_deferredDownloads.append({m_currentQueueIndex,
                                m_wifiNamePart,
                                m_currentSn,
                                m_currentHardware,
                                m_currentVersion,
                                m_collectPath});
    setCurrentSlotState(QStringLiteral("waiting_download"));
    m_collectStatus = QStringLiteral("采集已停止，等待统一下载");
    emit collectStatusChanged();
    appendLog(QStringLiteral("已加入统一下载队列：%1 %2 号位，SN=%3，目录=%4")
                  .arg(currentTray()).arg(currentSlot()).arg(m_currentSn, m_collectPath));
}

void TcpController::beginBatchDownloads()
{
    cleanupDeviceConnections();
    m_batchDownloadPhase = true;
    m_currentDownloadIndex = -1;

    if (!m_armSequenceFinished) {
        if (!sendArmCommand(QStringLiteral("$$MT,OVER")))
            appendLog(QStringLiteral("机械臂结束指令发送失败，但继续统一下载"));
        m_armSequenceFinished = true;
    }

    if (m_deferredDownloads.isEmpty()) {
        appendLog(QStringLiteral("没有等待下载的数据，直接结束任务"));
        finishTask();
        return;
    }

    appendLog(QStringLiteral("全部设备测量和放回完成，开始统一下载 %1 台设备的数据")
                  .arg(m_deferredDownloads.size()));
    setFlowState(FlowState::Recovering,
                 QStringLiteral("全部设备测量完成，准备统一下载 %1 份数据")
                     .arg(m_deferredDownloads.size()));
    QTimer::singleShot(0, this, &TcpController::startNextDeferredDownload);
}

void TcpController::startNextDeferredDownload()
{
    if (!m_taskActive || !m_batchDownloadPhase || m_aborting)
        return;

    cleanupDeviceConnections();

    while (++m_currentDownloadIndex < m_deferredDownloads.size()) {
        const DeferredDownload &download = m_deferredDownloads.at(m_currentDownloadIndex);
        m_currentQueueIndex = download.taskIndex;
        if (currentSlotState() == QStringLiteral("failed")) {
            appendLog(QStringLiteral("跳过已失败槽位的数据下载：%1 %2 号位")
                          .arg(currentTray()).arg(currentSlot()));
            continue;
        }

        m_wifiNamePart = download.wifiNamePart;
        m_currentSn = download.sn;
        m_currentHardware = download.hardware;
        m_currentVersion = download.version;
        m_collectPath = download.collectPath;
        m_collectStatus = QStringLiteral("等待统一下载");
        m_ftpLocalDir.clear();
        m_wifiConnectTicks = 0;
        setLastError(QString());
        setCurrentSlotState(QStringLiteral("downloading"));
        emit wifiNamePartChanged();
        emit currentDeviceChanged();
        emit collectStatusChanged();
        emit collectPathChanged();

        setFlowState(FlowState::ConnectingWifi,
                     QStringLiteral("统一下载 %1/%2：正在连接 %3 的 Wi-Fi")
                         .arg(m_currentDownloadIndex + 1)
                         .arg(m_deferredDownloads.size())
                         .arg(m_currentSn),
                     kWifiTimeoutMs);
        appendLog(QStringLiteral("统一下载 %1/%2：%3 %4 号位，SN=%5")
                      .arg(m_currentDownloadIndex + 1)
                      .arg(m_deferredDownloads.size())
                      .arg(currentTray()).arg(currentSlot()).arg(m_currentSn));
        connectWifiByNamePart(m_wifiNamePart);
        return;
    }

    finishTask();
}

void TcpController::failCurrentDownload(const QString &reason)
{
    if (!m_taskActive || !m_batchDownloadPhase)
        return;

    stopStepTimer();
    if (m_ftpProcess->state() != QProcess::NotRunning)
        m_ftpProcess->kill();
    if (m_ftpDownloading) {
        m_ftpDownloading = false;
        emit ftpDownloadingChanged();
    }
    m_ftpPhase = 0;
    m_ftpFiles.clear();
    setCurrentSlotState(QStringLiteral("failed"));
    setLastError(reason);
    recordCurrentResult(QStringLiteral("failed"), reason);
    setFlowState(FlowState::Recovering,
                 QStringLiteral("%1 %2 号位下载失败，继续下一台：%3")
                     .arg(currentTray()).arg(currentSlot()).arg(reason));
    appendLog(QStringLiteral("统一下载失败，继续下一台: %1").arg(reason));
    QTimer::singleShot(500, this, &TcpController::startNextDeferredDownload);
}

void TcpController::failCurrentSlot(const QString &reason)
{
    if (!m_taskActive)
        return;
    if (m_guaranteedStopActive) {
        appendLog(QStringLiteral("兜底停止期间又收到异常，继续以停止设备为最高优先级：%1")
                      .arg(reason));
        return;
    }
    if (m_batchDownloadPhase) {
        failCurrentDownload(reason);
        return;
    }
    const FlowState failedState = m_flowState;
    stopStepTimer();
    setCurrentSlotState(QStringLiteral("failed"));
    setLastError(reason);
    recordCurrentResult(QStringLiteral("failed"), reason);
    const QString failureProgress = QStringLiteral("%1 %2 号位失败：%3")
                                        .arg(currentTray()).arg(currentSlot()).arg(reason);
    setFlowState(FlowState::Recovering, failureProgress);
    appendLog(QStringLiteral("当前槽位标定失败: %1").arg(reason));

    if (measurementMayBeActive()) {
        beginGuaranteedMeasurementStop(false, stateNeedsPutBack(failedState), reason);
        return;
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
    return m_armMotionStarted
           || state == FlowState::Calibrating
           || state == FlowState::StoppingCollection
           || state == FlowState::StoppingModeling
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
    m_armAtHomeReady = false;
    m_collectReadyForModeling = false;
    emit currentDeviceChanged();
    emit wifiNamePartChanged();
    appendLog(QStringLiteral("扫描到设备 SN: %1").arg(cleanSn));
    if (!sendArmCommand(QStringLiteral("$$MT,START"))) {
        failCurrentSlot(QStringLiteral("扫码完成后无法立即启动机械臂"));
        return;
    }
    m_armMotionStarted = true;
    m_armMotionFinished = false;
    appendLog(QStringLiteral("扫码完成：机械臂安全抬升并夹取设备返回 Home；上位机并行连接设备"));
    setFlowState(FlowState::ConnectingWifi,
                 QStringLiteral("机械臂正在夹取并返回 Home，同时连接设备 Wi-Fi：%1").arg(cleanSn),
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
    const bool detectedNow = isConnectedToTargetWifi();
    if (detectedNow) {
        m_wifiDisconnectTicks = 0;
        if (!m_wifiConnected) {
            m_wifiConnected = true;
            emit wifiConnectedChanged();
            appendLog(QStringLiteral("设备 Wi-Fi 连接成功: %1").arg(m_wifiNamePart));
        }
    } else if (m_wifiConnected) {
        ++m_wifiDisconnectTicks;
        if (m_wifiDisconnectTicks == 1) {
            appendLog(QStringLiteral("暂时未检测到目标 Wi-Fi，进入 %1 次检测宽限期")
                          .arg(kWifiDisconnectGraceTicks));
        }
        if (m_wifiDisconnectTicks >= kWifiDisconnectGraceTicks) {
            m_wifiConnected = false;
            emit wifiConnectedChanged();
            appendLog(QStringLiteral("连续 %1 次未检测到目标 Wi-Fi，确认连接已中断")
                          .arg(kWifiDisconnectGraceTicks));
        }
    }
    const bool connectedNow = m_wifiConnected;

    if (!m_taskActive)
        return;

    if (m_guaranteedStopActive) {
        if (!connectedNow) {
            ++m_wifiConnectTicks;
            if (m_wifiConnectTicks % 3 == 0) {
                appendLog(QStringLiteral("兜底停止阶段重新连接设备 Wi-Fi：%1")
                              .arg(m_wifiNamePart));
                connectWifiByNamePart(m_wifiNamePart);
            }
        } else if (m_mqttClient->state() == QMqttClient::Disconnected
                   && !m_mqttReconnectTimer.isActive()) {
            appendLog(QStringLiteral("设备 Wi-Fi 已恢复，重新连接 MQTT 以发送停止指令"));
            scheduleMqttReconnect();
        }
        return;
    }

    if (m_flowState == FlowState::ConnectingWifi) {
        if (connectedNow) {
            if (m_batchDownloadPhase) {
                const int scheduledDownloadIndex = m_currentDownloadIndex;
                setFlowState(FlowState::Downloading,
                             QStringLiteral("统一下载 %1/%2：Wi-Fi 已连接，等待 FTP 就绪")
                                 .arg(m_currentDownloadIndex + 1)
                                 .arg(m_deferredDownloads.size()));
                appendLog(QStringLiteral("设备 Wi-Fi 已连接，等待网络地址就绪后开始 FTP"));
                QTimer::singleShot(2500, this, [this, scheduledDownloadIndex]() {
                    if (!m_taskActive || !m_batchDownloadPhase
                        || m_currentDownloadIndex != scheduledDownloadIndex
                        || m_flowState != FlowState::Downloading)
                        return;
                    if (!m_wifiConnected) {
                        failCurrentDownload(QStringLiteral("统一下载前设备 Wi-Fi 已断开"));
                        return;
                    }
                    startFtpDownload();
                });
                return;
            }
            setFlowState(FlowState::ConnectingDevice,
                         QStringLiteral("设备 Wi-Fi 已连接，等待网络地址就绪"),
                         kDeviceConnectTimeoutMs);
            appendLog(QStringLiteral("等待 DHCP/路由就绪后连接设备信息端口"));
            m_deviceRetryTimer.start(2500);
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
                               || m_flowState == FlowState::WaitingDeviceReady
                               || m_flowState == FlowState::CreatingProject
                               || m_flowState == FlowState::CreatingTask
                               || m_flowState == FlowState::Preinitializing
                               || m_flowState == FlowState::StartingModeling
                               || m_flowState == FlowState::StartingCollection
                               || m_flowState == FlowState::WaitingArmHome
                               || m_flowState == FlowState::Calibrating
                               || m_flowState == FlowState::StoppingCollection
                               || m_flowState == FlowState::StoppingModeling
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
            QString generatedProfileName;
            if (!profileName.isEmpty()) {
                parameters.wlanConnectionMode = wlan_connection_mode_profile;
                parameters.strProfile = network.strProfileName;
            } else if (network.dot11DefaultAuthAlgorithm == DOT11_AUTH_ALGO_80211_OPEN
                       && network.dot11DefaultCipherAlgorithm == DOT11_CIPHER_ALGO_NONE) {
                DWORD reasonCode = 0;
                if (!installOpenWifiProfile(clientHandle, interfaceInfo.InterfaceGuid,
                                            ssid, &reasonCode)) {
                    appendLog(QStringLiteral("创建 Wi-Fi 配置失败: %1，原因码: %2")
                                  .arg(ssid).arg(reasonCode));
                    continue;
                }
                generatedProfileName = ssid;
                parameters.wlanConnectionMode = wlan_connection_mode_profile;
                parameters.strProfile = reinterpret_cast<LPCWSTR>(
                    generatedProfileName.utf16());
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

    // 扫描列表可能仍是旧缓存。SLAM 热点名称是固定前缀加 SN，
    // 因此可先创建配置并发起定向连接，让 Windows 驱动自行查找目标热点。
    if (!networkFound && interfaceList->dwNumberOfItems > 0) {
        const QString expectedSsid = namePart.startsWith(QStringLiteral("SLAM-"),
                                                         Qt::CaseInsensitive)
                                         ? namePart
                                         : QStringLiteral("SLAM-%1").arg(namePart);
        const WLAN_INTERFACE_INFO &interfaceInfo = interfaceList->InterfaceInfo[0];
        DWORD reasonCode = 0;
        if (installOpenWifiProfile(clientHandle, interfaceInfo.InterfaceGuid,
                                   expectedSsid, &reasonCode)) {
            WLAN_CONNECTION_PARAMETERS parameters = {};
            parameters.wlanConnectionMode = wlan_connection_mode_profile;
            parameters.strProfile = reinterpret_cast<LPCWSTR>(expectedSsid.utf16());
            parameters.dot11BssType = dot11_BSS_type_infrastructure;
            result = WlanConnect(clientHandle, &interfaceInfo.InterfaceGuid,
                                 &parameters, nullptr);
            if (result == ERROR_SUCCESS) {
                networkFound = true;
                connectStarted = true;
                appendLog(QStringLiteral("扫描缓存未显示热点，已定向连接: %1")
                              .arg(expectedSsid));
            }
        } else {
            appendLog(QStringLiteral("创建定向 Wi-Fi 配置失败: %1，原因码: %2")
                          .arg(expectedSsid).arg(reasonCode));
        }
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
    if (!m_taskActive || !m_wifiConnected
        || m_flowState != FlowState::ConnectingDevice) {
        return;
    }
    if (m_deviceSocket->state() == QAbstractSocket::ConnectingState
        || m_deviceSocket->state() == QAbstractSocket::ConnectedState) {
        return;
    }
    m_deviceReceiveBuffer.clear();
    m_deviceSocket->abort();
    setProgress(QStringLiteral("正在连接设备信息端口 %1:%2")
                    .arg(m_deviceHost).arg(m_devicePort));
    m_deviceSocket->connectToHost(m_deviceHost, static_cast<quint16>(m_devicePort));
}

void TcpController::onDeviceConnected()
{
    m_deviceRetryTimer.stop();
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
    if (m_cleaningUp || !m_taskActive)
        return;
    if (m_flowState == FlowState::ConnectingDevice) {
        if (!m_deviceRetryTimer.isActive())
            m_deviceRetryTimer.start(1500);
        return;
    }
    if (m_flowState == FlowState::ReadingDeviceInfo) {
        failCurrentSlot(QStringLiteral("设备信息 TCP 连接断开"));
    }
}

void TcpController::onDeviceError(QAbstractSocket::SocketError socketError)
{
    Q_UNUSED(socketError)
    appendLog(QStringLiteral("设备 TCP 错误: %1").arg(m_deviceSocket->errorString()));
    if (m_cleaningUp || !m_taskActive)
        return;
    if (m_flowState == FlowState::ConnectingDevice) {
        appendLog(QStringLiteral("网络尚未就绪，1.5 秒后重试设备 TCP"));
        if (!m_deviceRetryTimer.isActive())
            m_deviceRetryTimer.start(1500);
        return;
    }
    if (m_flowState == FlowState::ReadingDeviceInfo) {
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
    m_mqttReconnectTimer.stop();
    m_mqttReconnectAttempts = 0;
    m_mqttPortCandidateIndex = 0;
    m_mqttPortFailureCount = 0;
    m_mqttPort = m_mqttPortCandidates.at(m_mqttPortCandidateIndex);
    m_mqttClient->setHostname(m_mqttHost);
    m_mqttClient->setPort(static_cast<quint16>(m_mqttPort));
    setFlowState(FlowState::ConnectingMqtt,
                  QStringLiteral("正在连接 MQTT %1:%2 并准备真实扫描")
                      .arg(m_mqttHost).arg(m_mqttPort),
                  kMqttTimeoutMs);
    if (m_mqttClient->state() == QMqttClient::Connected) {
        if (!m_mqttSubscribed)
            m_mqttSubscribed = subscribeMqttTopics();
        if (!m_mqttSubscribed || !publishProjectCreate())
            failCurrentSlot(QStringLiteral("MQTT 准备扫描工程失败"));
    } else if (m_mqttClient->state() == QMqttClient::Disconnected) {
        m_mqttSubscribed = false;
        m_mqttClient->connectToHost();
    }
}

void TcpController::scheduleMqttReconnect()
{
    if (!m_taskActive || m_cleaningUp || !m_wifiConnected
        || m_mqttClient->state() == QMqttClient::Connected
        || m_mqttClient->state() == QMqttClient::Connecting
        || m_mqttReconnectTimer.isActive()) {
        return;
    }
    const int delay = qMin(kMqttReconnectBaseDelayMs
                               * qMax(1, m_mqttReconnectAttempts + 1),
                           6000);
    appendLog(QStringLiteral("MQTT 将在 %1 毫秒后自动重连").arg(delay));
    m_mqttReconnectTimer.start(delay);
}

void TcpController::resumeMqttOperation()
{
    if (!m_taskActive || m_mqttClient->state() != QMqttClient::Connected
        || !m_mqttSubscribed) {
        return;
    }

    if (m_guaranteedStopActive) {
        continueGuaranteedMeasurementStop();
        return;
    }

    switch (m_flowState) {
    case FlowState::Preinitializing:
        if (!publishModelingPreinit())
            failCurrentSlot(QStringLiteral("MQTT 恢复后无法重新发送扫描预初始化指令"));
        break;
    case FlowState::StartingCollection:
        if (!publishCollectAction(QStringLiteral("AC_START")))
            failCurrentSlot(QStringLiteral("MQTT 恢复后无法重新发送采集启动指令"));
        break;
    case FlowState::WaitingArmHome:
        tryStartModelingWhenArmHome();
        break;
    case FlowState::StartingModeling:
        if (!publishModelingAction(true))
            failCurrentSlot(QStringLiteral("MQTT 恢复后无法重新发送真实扫描指令"));
        break;
    case FlowState::StoppingModeling:
        if (!publishModelingAction(false))
            failCurrentSlot(QStringLiteral("MQTT 恢复后无法重新发送停止扫描指令"));
        break;
    case FlowState::StoppingCollection:
        if (!publishCollectAction(QStringLiteral("AC_STOP")))
            failCurrentSlot(QStringLiteral("MQTT 恢复后无法重新发送停止采集指令"));
        break;
    case FlowState::CreatingProject:
    case FlowState::CreatingTask:
        appendLog(QStringLiteral("MQTT 恢复后继续等待之前的 QoS 1 响应"));
        break;
    default:
        // Device status and modeling reports are periodic. Re-subscribing is
        // sufficient while waiting for readiness or while the arm is moving.
        break;
    }
}

bool TcpController::subscribeMqttTopics()
{
    const QVector<QPair<QString, quint8>> topics{
        {QStringLiteral("slam_app/report/device_status"), 0},
        {QStringLiteral("slam_app/response/project"), 1},
        {QStringLiteral("slam_app/response/task"), 1},
        {QStringLiteral("slam_app/response/modeling"), 1},
        {QStringLiteral("slam_app/report/modeling"), 0},
        {QStringLiteral("slam_app/response/data_collect"), 1}
    };

    bool allSubscribed = true;
    for (const auto &entry : topics) {
        QMqttSubscription *subscription = m_mqttClient->subscribe(
            QMqttTopicFilter(entry.first), entry.second);
        if (subscription) {
            appendLog(QStringLiteral("MQTT 已订阅: %1").arg(entry.first));
        } else {
            allSubscribed = false;
            appendLog(QStringLiteral("MQTT 订阅失败: %1").arg(entry.first));
        }
    }
    return allSubscribed;
}

bool TcpController::publishProjectCreate()
{
    if (m_mqttClient->state() != QMqttClient::Connected)
        return false;

    m_modelingProjectId = -1;
    m_modelingTaskId = -1;
    m_modelingStatus = -1;
    m_modelingActive = false;
    m_collectActive = false;
    m_collectReadyForModeling = false;
    m_protoSessionId = QStringLiteral("slam-calib-%1-%2")
                           .arg(m_currentSn.isEmpty() ? QStringLiteral("device") : m_currentSn)
                           .arg(QDateTime::currentMSecsSinceEpoch());

    QByteArray payload;
    appendProtoBytes(payload, 1, makeProtoHeader(++m_protoSequence, m_protoSessionId));
    appendProtoUInt(payload, 2, 7); // Application.AC_ADD
    const QString projectName = QStringLiteral("CAL_%1")
                                    .arg(QDateTime::currentDateTime().toString(
                                        QStringLiteral("yyyyMMdd_HHmmss")));
    appendProtoBytes(payload, 4, projectName.toUtf8());
    appendProtoBytes(payload, 5, QStringLiteral("SLAM calibration").toUtf8());

    const qint32 messageId = m_mqttClient->publish(
        QMqttTopicName(QStringLiteral("app_slam/request/project")), payload, 1, false);
    if (messageId < 0)
        return false;
    appendLog(QStringLiteral("MQTT 创建扫描工程 %1，消息 ID=%2")
                  .arg(projectName).arg(messageId));
    setFlowState(FlowState::CreatingProject,
                 QStringLiteral("正在创建设备扫描工程"),
                 kProjectTimeoutMs);
    return true;
}

bool TcpController::publishTaskCreate()
{
    if (m_mqttClient->state() != QMqttClient::Connected || m_modelingProjectId <= 0)
        return false;

    QByteArray payload;
    appendProtoBytes(payload, 1, makeProtoHeader(++m_protoSequence, m_protoSessionId));
    appendProtoUInt(payload, 2, 7); // Application.AC_ADD
    appendProtoSInt(payload, 3, m_modelingProjectId);
    const QString taskName = QStringLiteral("CAL_%1_%2")
                                 .arg(m_currentSn.isEmpty() ? QStringLiteral("DEVICE") : m_currentSn)
                                 .arg(QDateTime::currentDateTime().toString(
                                     QStringLiteral("HHmmss")));
    appendProtoBytes(payload, 5, taskName.toUtf8());
    appendProtoBytes(payload, 6, QByteArrayLiteral("simple_qt"));

    const qint32 messageId = m_mqttClient->publish(
        QMqttTopicName(QStringLiteral("app_slam/request/task")), payload, 1, false);
    if (messageId < 0)
        return false;
    appendLog(QStringLiteral("MQTT 创建扫描任务 %1，工程 ID=%2，消息 ID=%3")
                  .arg(taskName).arg(m_modelingProjectId).arg(messageId));
    setFlowState(FlowState::CreatingTask,
                 QStringLiteral("正在创建设备扫描任务"),
                 kTaskTimeoutMs);
    return true;
}

bool TcpController::publishModelingPreinit()
{
    if (m_mqttClient->state() != QMqttClient::Connected || m_modelingTaskId <= 0)
        return false;

    QByteArray payload;
    appendProtoBytes(payload, 1, makeProtoHeader(++m_protoSequence, m_protoSessionId));
    appendProtoUInt(payload, 2, 9); // Application.AC_PREINIT
    appendProtoUInt(payload, 3, static_cast<quint32>(m_modelingTaskId));
    appendProtoUInt(payload, 4, 2); // RM_RECORD_CALC
    appendProtoUInt(payload, 5, 0); // MT_HAND
    if (m_modelingProjectId > 0)
        appendProtoUInt(payload, 6, static_cast<quint32>(m_modelingProjectId));
    appendProtoUInt(payload, 7, 0); // No Ntrip/RTK initialization.

    const qint32 messageId = m_mqttClient->publish(
        QMqttTopicName(QStringLiteral("app_slam/request/modeling")), payload, 1, false);
    if (messageId < 0)
        return false;

    appendLog(QStringLiteral("MQTT 发布扫描预初始化 AC_PREINIT，任务 ID=%1，母工程 ID=%2，消息 ID=%3")
                  .arg(m_modelingTaskId).arg(m_modelingProjectId).arg(messageId));
    setFlowState(FlowState::Preinitializing,
                 QStringLiteral("正在预初始化设备扫描模块"),
                 kPreinitTimeoutMs);
    return true;
}

bool TcpController::publishModelingAction(bool start, bool updateFlow)
{
    if (m_mqttClient->state() != QMqttClient::Connected || m_modelingTaskId <= 0)
        return false;

    QByteArray payload;
    appendProtoBytes(payload, 1, makeProtoHeader(++m_protoSequence, m_protoSessionId));
    appendProtoUInt(payload, 2, start ? 1 : 2); // AC_START / AC_STOP
    appendProtoUInt(payload, 3, static_cast<quint32>(m_modelingTaskId));
    if (start) {
        appendProtoUInt(payload, 4, 2); // RM_RECORD_CALC: record and run SLAM
        appendProtoUInt(payload, 5, 0); // MT_HAND
        if (m_modelingProjectId > 0)
            appendProtoUInt(payload, 6, static_cast<quint32>(m_modelingProjectId));
        appendProtoUInt(payload, 7, 0); // No Ntrip/RTK initialization.
    }

    const qint32 messageId = m_mqttClient->publish(
        QMqttTopicName(QStringLiteral("app_slam/request/modeling")), payload, 1, false);
    if (messageId < 0)
        return false;

    if (start)
        m_modelingActive = true; // Also ensures a failed start is stopped best-effort.
    appendLog(QStringLiteral("MQTT 发布真实扫描 %1，任务 ID=%2，母工程 ID=%3，消息 ID=%4")
                  .arg(start ? QStringLiteral("AC_START") : QStringLiteral("AC_STOP"))
                  .arg(m_modelingTaskId).arg(m_modelingProjectId).arg(messageId));
    if (updateFlow) {
        if (start) {
            setFlowState(FlowState::StartingModeling,
                         QStringLiteral("设备正在启动传感器和 SLAM 扫描"),
                         kModelingStartTimeoutMs);
        } else {
            setFlowState(FlowState::StoppingModeling,
                         QStringLiteral("设备正在停止扫描并保存工程"),
                         kModelingStopTimeoutMs);
        }
    }
    return true;
}

bool TcpController::publishCollectAction(const QString &action, bool updateFlow)
{
    if (m_mqttClient->state() != QMqttClient::Connected)
        return false;
    const QMqttTopicName topic(QStringLiteral("app_slam/request/data_collect"));
    const QJsonObject object{{QStringLiteral("action"), action}};
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const qint32 messageId = m_mqttClient->publish(topic, payload, 1, false);
    if (messageId < 0)
        return false;

    if (action == QStringLiteral("AC_START")) {
        m_collectActive = true; // Stop it best-effort even if its response is lost.
        m_collectReadyForModeling = false;
    }
    appendLog(QStringLiteral("MQTT 发布 %1，消息 ID=%2").arg(action).arg(messageId));
    if (updateFlow) {
        if (action == QStringLiteral("AC_START")) {
            setFlowState(FlowState::StartingCollection,
                         QStringLiteral("正在开启标定原始数据记录"),
                         kCollectTimeoutMs);
        } else {
            setFlowState(FlowState::StoppingCollection,
                         QStringLiteral("正在停止标定原始数据记录"),
                         kStopCollectTimeoutMs);
        }
    }
    return true;
}

void TcpController::tryStartModelingWhenArmHome()
{
    if (!m_taskActive || m_aborting || m_guaranteedStopActive
        || !m_collectReadyForModeling || !m_armAtHomeReady
        || (m_flowState != FlowState::StartingCollection
            && m_flowState != FlowState::WaitingArmHome)) {
        return;
    }

    if (m_mqttClient->state() != QMqttClient::Connected || !m_mqttSubscribed) {
        setFlowState(FlowState::WaitingArmHome,
                     QStringLiteral("机械臂已在 Home 静置，等待 MQTT 恢复后启动真实扫描"),
                     kArmHomeTimeoutMs);
        scheduleMqttReconnect();
        return;
    }

    stopStepTimer();
    appendLog(QStringLiteral("机械臂和原始采集均已准备完成，设备保持静止，开始真实扫描初始化"));
    if (!publishModelingAction(true))
        failCurrentSlot(QStringLiteral("机械臂回到 Home 后无法启动设备真实扫描"));
}

void TcpController::retryCollectStartAfterStop()
{
    if (!m_taskActive || !m_retryCollectAfterStop)
        return;

    m_retryCollectAfterStop = false;
    m_collectActive = false;
    if (!m_collectPath.isEmpty()) {
        m_collectPath.clear();
        emit collectPathChanged();
    }
    m_collectStatus = QStringLiteral("旧采集会话已停止，准备重试");
    emit collectStatusChanged();
    setFlowState(FlowState::Recovering,
                 QStringLiteral("已释放设备残留采集会话，正在重试创建采集目录"));

    QTimer::singleShot(1200, this, [this]() {
        if (!m_taskActive || m_batchDownloadPhase
            || m_flowState != FlowState::Recovering) {
            return;
        }
        appendLog(QStringLiteral("重试当前槽位的标定原始数据采集 AC_START（%1/1）")
                      .arg(m_collectStartRecoveryAttempts));
        if (!publishCollectAction(QStringLiteral("AC_START")))
            failCurrentSlot(QStringLiteral("释放残留采集会话后无法重试 AC_START"));
    });
}

bool TcpController::measurementMayBeActive() const
{
    return m_modelingActive || m_collectActive;
}

void TcpController::beginGuaranteedMeasurementStop(bool afterAbort, bool needsPutBack,
                                                   const QString &reason)
{
    if (!m_taskActive)
        return;

    if (m_guaranteedStopActive) {
        m_guaranteedStopAfterAbort = m_guaranteedStopAfterAbort || afterAbort;
        m_guaranteedStopNeedsPutBack = m_guaranteedStopNeedsPutBack || needsPutBack;
        if (m_guaranteedStopReason.isEmpty())
            m_guaranteedStopReason = reason;
        return;
    }

    m_guaranteedStopActive = true;
    m_guaranteedStopAfterAbort = afterAbort;
    m_guaranteedStopNeedsPutBack = needsPutBack;
    m_guaranteedStopAttempts = 0;
    m_guaranteedStopReason = reason;
    m_retryCollectAfterStop = false;
    m_wifiConnectTicks = 0;
    appendLog(QStringLiteral("进入设备测量兜底停止：必须确认真实扫描和原始采集都已结束后才能继续"));
    continueGuaranteedMeasurementStop();
}

void TcpController::continueGuaranteedMeasurementStop()
{
    if (!m_taskActive || !m_guaranteedStopActive)
        return;

    setFlowState(FlowState::EnsuringMeasurementStopped,
                 QStringLiteral("正在确保设备结束测量，请勿关闭程序或切换网络"),
                 kGuaranteedStopRetryMs);

    if (!m_wifiConnected) {
        appendLog(QStringLiteral("兜底停止等待设备 Wi-Fi，保持当前槽位且不进入下一台"));
        if (!m_wifiNamePart.isEmpty())
            connectWifiByNamePart(m_wifiNamePart);
        return;
    }

    if (m_mqttClient->state() != QMqttClient::Connected) {
        appendLog(QStringLiteral("兜底停止等待 MQTT 恢复，连接成功后将自动重发停止指令"));
        if (m_mqttClient->state() == QMqttClient::Disconnected) {
            m_mqttSubscribed = false;
            m_mqttClient->setHostname(m_mqttHost);
            m_mqttClient->setPort(static_cast<quint16>(m_mqttPort));
            m_mqttClient->connectToHost();
        }
        return;
    }

    if (!m_mqttSubscribed)
        m_mqttSubscribed = subscribeMqttTopics();
    if (!m_mqttSubscribed) {
        appendLog(QStringLiteral("兜底停止无法订阅确认主题，稍后继续重试"));
        return;
    }

    if (m_modelingActive) {
        if (m_modelingTaskId <= 0) {
            appendLog(QStringLiteral("真实扫描没有有效任务 ID，按未成功启动处理并继续停止原始采集"));
            m_modelingActive = false;
            QTimer::singleShot(0, this, &TcpController::continueGuaranteedMeasurementStop);
            return;
        }
        if (publishModelingAction(false, false)) {
            appendLog(QStringLiteral("兜底停止已发送真实扫描 AC_STOP，等待设备确认保存结束"));
        } else {
            appendLog(QStringLiteral("兜底停止发送真实扫描 AC_STOP 失败，稍后自动重试"));
        }
        return;
    }

    if (m_collectActive) {
        if (publishCollectAction(QStringLiteral("AC_STOP"), false)) {
            appendLog(QStringLiteral("兜底停止已发送原始采集 AC_STOP，等待设备确认停止"));
        } else {
            appendLog(QStringLiteral("兜底停止发送原始采集 AC_STOP 失败，稍后自动重试"));
        }
        return;
    }

    finishGuaranteedMeasurementStop();
}

void TcpController::finishGuaranteedMeasurementStop()
{
    if (!m_guaranteedStopActive)
        return;

    stopStepTimer();
    const bool afterAbort = m_guaranteedStopAfterAbort;
    const bool needsPutBack = m_guaranteedStopNeedsPutBack;
    m_guaranteedStopActive = false;
    m_guaranteedStopAfterAbort = false;
    m_guaranteedStopNeedsPutBack = false;
    m_guaranteedStopAttempts = 0;
    m_guaranteedStopReason.clear();
    appendLog(QStringLiteral("设备已确认结束真实扫描和原始数据采集，兜底停止完成"));

    if (needsPutBack && m_connected && m_armReady) {
        requestPutBack(true);
        return;
    }

    if (afterAbort || (needsPutBack && (!m_connected || !m_armReady))) {
        if (m_connected)
            sendArmCommand(QStringLiteral("$$MT,OVER"));
        m_taskActive = false;
        m_aborting = false;
        emit armRunningChanged();
        cleanupDeviceConnections();
        setFlowState(FlowState::Aborted,
                     needsPutBack && (!m_connected || !m_armReady)
                         ? QStringLiteral("设备测量已停止，但机械臂连接中断，无法确认放回")
                         : QStringLiteral("任务已中止，设备测量已确认停止"));
        return;
    }

    QTimer::singleShot(0, this, &TcpController::startNextSlot);
}

void TcpController::handleMqttMessage(const QByteArray &message, const QString &topic)
{
    if (topic == QStringLiteral("slam_app/report/device_status")) {
        handleDeviceStatusReport(message);
        return;
    }
    if (topic == QStringLiteral("slam_app/response/project")) {
        handleProjectResponse(message);
        return;
    }
    if (topic == QStringLiteral("slam_app/response/task")) {
        handleTaskResponse(message);
        return;
    }
    if (topic == QStringLiteral("slam_app/response/modeling")) {
        handleModelingResponse(message);
        return;
    }
    if (topic == QStringLiteral("slam_app/report/modeling")) {
        handleModelingReport(message);
        return;
    }
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

    const bool alreadyCollecting = description.contains(QStringLiteral("already active"),
                                                           Qt::CaseInsensitive);

    if (m_guaranteedStopActive) {
        const bool confirmedStopped = description.compare(QStringLiteral("collect stopped"),
                                                           Qt::CaseInsensitive) == 0
                                      || description.contains(QStringLiteral("not active"),
                                                              Qt::CaseInsensitive)
                                      || description.contains(QStringLiteral("not collecting"),
                                                              Qt::CaseInsensitive)
                                      || description.contains(QStringLiteral("already stopped"),
                                                              Qt::CaseInsensitive);
        if (confirmedStopped) {
            m_collectStatus = QStringLiteral("采集已停止");
            m_collectActive = false;
            emit collectStatusChanged();
            appendLog(QStringLiteral("兜底停止已确认原始数据采集结束"));
            QTimer::singleShot(0, this, &TcpController::continueGuaranteedMeasurementStop);
        } else if (alreadyCollecting
                   || description.compare(QStringLiteral("collecting"),
                                          Qt::CaseInsensitive) == 0) {
            m_collectActive = true;
            appendLog(QStringLiteral("兜底停止收到设备仍在采集的响应，将继续重发 AC_STOP"));
        } else if (errorCode != 0) {
            appendLog(QStringLiteral("兜底停止收到采集错误 %1：%2，保持当前设备并继续重试")
                          .arg(errorCode).arg(description));
        }
        return;
    }

    const bool collectDirectoryFailure = errorCode == 3
                                         && description.contains(QStringLiteral("collect dir"),
                                                                 Qt::CaseInsensitive);
    if (m_flowState == FlowState::StartingCollection
        && collectDirectoryFailure
        && m_collectStartRecoveryAttempts < 1) {
        stopStepTimer();
        ++m_collectStartRecoveryAttempts;
        m_retryCollectAfterStop = true;
        m_collectStatus = QStringLiteral("正在释放残留采集会话");
        emit collectStatusChanged();
        appendLog(QStringLiteral("设备创建采集目录失败；先发送 AC_STOP 释放残留会话，再自动重试一次"));
        if (!publishCollectAction(QStringLiteral("AC_STOP"))) {
            m_retryCollectAfterStop = false;
            failCurrentSlot(QStringLiteral("设备采集目录创建失败，且无法发送残留会话清理指令"));
        }
        return;
    }

    if (m_flowState == FlowState::StoppingCollection
        && m_retryCollectAfterStop
        && errorCode == 2) {
        appendLog(QStringLiteral("设备回复当前没有活动采集会话，直接重试创建采集目录"));
        retryCollectStartAfterStop();
        return;
    }

    if (errorCode != 0 && !alreadyCollecting) {
        m_collectStatus = QStringLiteral("采集异常");
        emit collectStatusChanged();
        if (m_flowState == FlowState::StartingCollection
            || m_flowState == FlowState::StoppingCollection) {
            failCurrentSlot(QStringLiteral("设备采集失败，错误码 %1：%2")
                                .arg(errorCode).arg(description));
        }
        return;
    }

    if (description.compare(QStringLiteral("collecting"), Qt::CaseInsensitive) == 0
        || alreadyCollecting) {
        m_collectStatus = QStringLiteral("采集中");
        m_collectActive = true;
        emit collectStatusChanged();
        if (m_flowState == FlowState::StartingCollection
            || m_flowState == FlowState::WaitingArmHome) {
            stopStepTimer();
            m_collectReadyForModeling = true;
            if (alreadyCollecting)
                appendLog(QStringLiteral("设备已处于采集状态，按幂等成功继续流程"));
            if (!m_armAtHomeReady) {
                setFlowState(FlowState::WaitingArmHome,
                             QStringLiteral("原始采集已就绪，等待机械臂夹取设备并回到 Home 静置"),
                             kArmHomeTimeoutMs);
                appendLog(QStringLiteral("原始采集准备完成，暂不启动真实扫描；等待机械臂 HOMEOK"));
            }
            tryStartModelingWhenArmHome();
        }
        return;
    }

    if (description.compare(QStringLiteral("collect stopped"), Qt::CaseInsensitive) == 0) {
        m_collectStatus = QStringLiteral("采集已停止");
        m_collectActive = false;
        emit collectStatusChanged();
        if (m_flowState == FlowState::StoppingCollection) {
            stopStepTimer();
            if (m_retryCollectAfterStop) {
                appendLog(QStringLiteral("设备残留采集会话已停止，旧目录=%1")
                              .arg(path.isEmpty() ? QStringLiteral("--") : path));
                retryCollectStartAfterStop();
                return;
            }
            queueCurrentDownload();
            requestPutBack(false);
        }
        return;
    }

    m_collectStatus = description.isEmpty() ? QStringLiteral("未知状态") : description;
    emit collectStatusChanged();
}

void TcpController::handleDeviceStatusReport(const QByteArray &message)
{
    quint64 rawReady = 0;
    if (!protoVarintField(message, 4, rawReady))
        return;

    const bool ready = rawReady != 0;
    const bool changed = !m_deviceStatusReceived || ready != m_deviceInitReady;
    m_deviceStatusReceived = true;
    m_deviceInitReady = ready;
    if (changed) {
        appendLog(ready
                      ? QStringLiteral("设备初始化状态: init_ready=1，可以启动扫描")
                      : QStringLiteral("设备初始化状态: init_ready=0，继续等待设备就绪"));
    }

    const ProtoError statusError = parseProtoErrorAt(message, 5);
    if (statusError.code != 0 && !protoSucceeded(statusError) && changed) {
        appendLog(QStringLiteral("设备状态附带错误 0x%1：%2")
                      .arg(statusError.code, 8, 16, QLatin1Char('0'))
                      .arg(statusError.description));
    }

    if (!m_taskActive || m_flowState != FlowState::WaitingDeviceReady)
        return;
    if (!ready) {
        setProgress(QStringLiteral("设备正在初始化，进入就绪状态后自动继续"));
        return;
    }

    stopStepTimer();
    const bool retryModeling = m_retryModelingAfterDeviceReady;
    m_retryModelingAfterDeviceReady = false;
    setProgress(retryModeling
                    ? QStringLiteral("设备已重新就绪，准备重试真实扫描")
                    : QStringLiteral("设备初始化完成，准备创建扫描工程"));
    QTimer::singleShot(300, this, [this, retryModeling]() {
        if (!m_taskActive || m_flowState != FlowState::WaitingDeviceReady
            || m_mqttClient->state() != QMqttClient::Connected) {
            return;
        }
        if (retryModeling) {
            appendLog(QStringLiteral("设备恢复 init_ready=1，自动重试真实扫描"));
            if (!publishModelingAction(true))
                failCurrentSlot(QStringLiteral("设备恢复就绪后无法重试真实扫描"));
        } else if (!publishProjectCreate()) {
            failCurrentSlot(QStringLiteral("设备初始化完成后无法创建扫描工程"));
        }
    });
}

void TcpController::handleProjectResponse(const QByteArray &message)
{
    if (!m_taskActive || m_flowState != FlowState::CreatingProject)
        return;
    const ProtoError error = parseProtoError(message);
    if (!protoSucceeded(error)) {
        failCurrentSlot(QStringLiteral("创建扫描工程失败，错误码 0x%1：%2")
                            .arg(error.code, 8, 16, QLatin1Char('0'))
                            .arg(error.description));
        return;
    }

    QByteArray project;
    quint64 encodedId = 0;
    if (!protoBytesField(message, 4, project)
        || !protoVarintField(project, 1, encodedId)) {
        failCurrentSlot(QStringLiteral("创建扫描工程响应缺少工程 ID"));
        return;
    }
    m_modelingProjectId = static_cast<qint32>(decodeProtoSInt(encodedId));
    if (m_modelingProjectId <= 0) {
        failCurrentSlot(QStringLiteral("设备返回了无效的扫描工程 ID"));
        return;
    }
    appendLog(QStringLiteral("扫描工程创建成功，ID=%1").arg(m_modelingProjectId));
    if (!publishTaskCreate())
        failCurrentSlot(QStringLiteral("MQTT 创建扫描任务失败"));
}

void TcpController::handleTaskResponse(const QByteArray &message)
{
    if (!m_taskActive || m_flowState != FlowState::CreatingTask)
        return;
    const ProtoError error = parseProtoError(message);
    if (!protoSucceeded(error)) {
        failCurrentSlot(QStringLiteral("创建扫描任务失败，错误码 0x%1：%2")
                            .arg(error.code, 8, 16, QLatin1Char('0'))
                            .arg(error.description));
        return;
    }

    quint64 encodedId = 0;
    if (!protoVarintField(message, 3, encodedId)) {
        failCurrentSlot(QStringLiteral("创建扫描任务响应缺少任务 ID"));
        return;
    }
    m_modelingTaskId = static_cast<qint32>(decodeProtoSInt(encodedId));
    if (m_modelingTaskId <= 0) {
        failCurrentSlot(QStringLiteral("设备返回了无效的扫描任务 ID"));
        return;
    }
    appendLog(QStringLiteral("扫描任务创建成功，ID=%1").arg(m_modelingTaskId));
    m_modelingStartRetries = 0;
    m_resumeModelingAfterPreinit = false;
    if (!publishModelingPreinit())
        failCurrentSlot(QStringLiteral("扫描任务创建后无法发送扫描预初始化指令"));
}

void TcpController::handleModelingResponse(const QByteArray &message)
{
    if (!m_taskActive)
        return;
    const ProtoError error = parseProtoError(message);
    quint64 rawAction = 0;
    const bool hasAction = protoVarintField(message, 3, rawAction);
    appendLog(QStringLiteral("MQTT 收到扫描控制响应，动作=%1，错误码=0x%2，说明=%3")
                  .arg(hasAction ? QString::number(rawAction) : QStringLiteral("--"))
                  .arg(error.code, 8, 16, QLatin1Char('0'))
                  .arg(error.description.isEmpty() ? QStringLiteral("--") : error.description));

    if (m_guaranteedStopActive) {
        const bool stopResponse = hasAction && rawAction == 2;
        const bool alreadyStopped = error.description.contains(QStringLiteral("not active"),
                                                                 Qt::CaseInsensitive)
                                    || error.description.contains(QStringLiteral("not running"),
                                                                  Qt::CaseInsensitive)
                                    || error.description.contains(QStringLiteral("stopped"),
                                                                  Qt::CaseInsensitive);
        if (stopResponse && alreadyStopped) {
            m_modelingActive = false;
            appendLog(QStringLiteral("兜底停止响应确认设备已经不在真实扫描状态"));
            QTimer::singleShot(0, this, &TcpController::continueGuaranteedMeasurementStop);
        } else if (stopResponse && protoSucceeded(error)) {
            appendLog(QStringLiteral("设备已接受兜底停止扫描指令，继续等待状态 7 确认保存结束"));
        } else if (stopResponse) {
            appendLog(QStringLiteral("兜底停止扫描响应异常，保持连接并继续重试：%1")
                          .arg(error.description));
        }
        return;
    }

    if (m_flowState == FlowState::Preinitializing) {
        // QoS 1 reconnects can deliver an older START/STOP response while the
        // preinitialization request is pending. Only action 9 belongs here.
        if (hasAction && rawAction != 9) {
            appendLog(QStringLiteral("预初始化期间忽略旧扫描响应，动作=%1").arg(rawAction));
            return;
        }

        if (!protoSucceeded(error)) {
            failCurrentSlot(QStringLiteral("设备扫描预初始化失败，错误码 0x%1：%2")
                                .arg(error.code, 8, 16, QLatin1Char('0'))
                                .arg(error.description));
            return;
        }

        stopStepTimer();
        const bool resumeModeling = m_resumeModelingAfterPreinit;
        m_resumeModelingAfterPreinit = false;
        appendLog(QStringLiteral("设备扫描预初始化 AC_PREINIT 成功"));
        if (resumeModeling) {
            if (!publishModelingAction(true))
                failCurrentSlot(QStringLiteral("预初始化成功后无法重新启动真实扫描"));
        } else if (!publishCollectAction(QStringLiteral("AC_START"))) {
            failCurrentSlot(QStringLiteral("设备预初始化后无法开启标定原始数据记录"));
        }
        return;
    }

    const bool preinitRequired = error.description.contains(QStringLiteral("preinit"),
                                                              Qt::CaseInsensitive)
                                 && error.description.contains(QStringLiteral("required"),
                                                               Qt::CaseInsensitive);
    if (!protoSucceeded(error) && preinitRequired
        && m_flowState == FlowState::StartingModeling
        && m_modelingStartRetries < 2) {
        ++m_modelingStartRetries;
        m_modelingActive = false;
        m_resumeModelingAfterPreinit = true;
        appendLog(QStringLiteral("设备报告 preinit required，先执行 AC_PREINIT，再进行第 %1 次 AC_START 重试")
                      .arg(m_modelingStartRetries));
        if (!publishModelingPreinit())
            failCurrentSlot(QStringLiteral("设备要求预初始化，但 AC_PREINIT 发送失败"));
        return;
    }

    const bool alreadyModeling = error.description.contains(QStringLiteral("already active"),
                                                             Qt::CaseInsensitive);
    if (!protoSucceeded(error)
        && !alreadyModeling
        && (m_flowState == FlowState::StartingModeling
            || m_flowState == FlowState::StoppingModeling)) {
        failCurrentSlot(QStringLiteral("设备扫描控制失败，错误码 0x%1：%2")
                            .arg(error.code, 8, 16, QLatin1Char('0'))
                            .arg(error.description));
        return;
    }

    if (m_flowState == FlowState::StartingModeling) {
        if (alreadyModeling)
            appendLog(QStringLiteral("设备已处于真实扫描启动流程，按幂等成功继续等待状态上报"));
        setProgress(QStringLiteral("设备已接受扫描指令，等待传感器真正启动"));
    } else if (m_flowState == FlowState::StoppingModeling) {
        setProgress(QStringLiteral("设备已接受停止指令，等待扫描数据保存完成"));
    }
}

void TcpController::handleModelingReport(const QByteArray &message)
{
    if (!m_taskActive)
        return;
    quint64 rawStatus = 0;
    if (!protoVarintField(message, 4, rawStatus))
        return;
    const int status = static_cast<int>(rawStatus);
    const ProtoError error = parseProtoErrorAt(message, 6);
    if (status != m_modelingStatus) {
        m_modelingStatus = status;
        appendLog(QStringLiteral("设备扫描状态变化: %1，错误码=0x%2")
                      .arg(status)
                      .arg(error.code, 8, 16, QLatin1Char('0')));
    }

    if (m_guaranteedStopActive) {
        if (status == 7 || status == 8 || status == 9) {
            m_modelingActive = false;
            appendLog(status == 7
                          ? QStringLiteral("兜底停止已确认真实扫描保存并结束（状态 7）")
                          : QStringLiteral("兜底停止确认真实扫描已退出（状态 %1）").arg(status));
            QTimer::singleShot(0, this, &TcpController::continueGuaranteedMeasurementStop);
        } else if (status == 6) {
            setProgress(QStringLiteral("正在确保设备结束测量：扫描数据保存中"));
        }
        return;
    }

    if (status == 8 || status == 9) {
        m_modelingActive = false;
        if (m_flowState == FlowState::StartingModeling
            || m_flowState == FlowState::Preinitializing
            || m_flowState == FlowState::StartingCollection
            || m_flowState == FlowState::Calibrating
            || m_flowState == FlowState::StoppingCollection
            || m_flowState == FlowState::StoppingModeling) {
            failCurrentSlot(QStringLiteral("设备真实扫描异常，状态 %1，错误码 0x%2：%3")
                                .arg(status)
                                .arg(error.code, 8, 16, QLatin1Char('0'))
                                .arg(error.description));
        }
        return;
    }

    if (m_flowState == FlowState::StartingModeling) {
        if (status == 1)
            setProgress(QStringLiteral("设备正在进行扫描前自检"));
        else if (status == 2)
            setProgress(QStringLiteral("设备扫描模块已就绪，等待静止初始化"));
        else if (status == 3)
            setProgress(QStringLiteral("设备正在打开传感器"));
        else if (status == 4)
            setProgress(QStringLiteral("设备正在初始化 SLAM 算法"));
        else if (status == 5) {
            stopStepTimer();
            m_modelingActive = true;
            appendLog(QStringLiteral("设备已进入 EC_ScanScanning，传感器已真实启动"));
            if (m_armMotionFinished) {
                failCurrentSlot(QStringLiteral("设备真实扫描启动过晚：机械臂轨迹已结束，本次没有有效的同步扫描数据"));
            } else if (!sendArmCommand(QStringLiteral("$$MT,SCANREADY"))) {
                failCurrentSlot(QStringLiteral("设备真实扫描已启动，但无法通知机械臂执行标定轨迹"));
            } else {
                appendLog(QStringLiteral("已通知机械臂设备扫描就绪（SCANREADY），释放 SLAM 标定轨迹"));
                setFlowState(FlowState::Calibrating,
                             QStringLiteral("设备正在真实扫描，机械臂开始执行 SLAM 标定轨迹"),
                             kCalibrationTimeoutMs);
            }
        }
        return;
    }

    if (m_flowState == FlowState::StoppingModeling) {
        if (status == 6) {
            setProgress(QStringLiteral("设备正在保存扫描工程"));
        } else if (status == 7) {
            stopStepTimer();
            m_modelingActive = false;
            appendLog(QStringLiteral("设备真实扫描已完成并保存"));
            if (!publishCollectAction(QStringLiteral("AC_STOP")))
                failCurrentSlot(QStringLiteral("真实扫描完成后无法停止标定数据记录"));
        }
    }
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

    QString remotePath = QDir::fromNativeSeparators(m_collectPath);
    const QString collectMarker = QStringLiteral("/collect/");
    const int collectPosition = remotePath.indexOf(collectMarker);
    if (collectPosition >= 0)
        remotePath = remotePath.mid(collectPosition);
    if (!remotePath.startsWith(QLatin1Char('/')))
        remotePath.prepend(QLatin1Char('/'));
    if (!remotePath.endsWith(QLatin1Char('/')))
        remotePath.append(QLatin1Char('/'));
    appendLog(QStringLiteral("FTP 路径映射: %1 -> %2").arg(m_collectPath, remotePath));
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
              << QStringLiteral("--max-time") << QStringLiteral("1800")
              << QStringLiteral("--speed-limit") << QStringLiteral("1024")
              << QStringLiteral("--speed-time") << QStringLiteral("60")
              << QStringLiteral("--retry") << QStringLiteral("5")
              << QStringLiteral("--retry-delay") << QStringLiteral("3")
              << QStringLiteral("--retry-all-errors")
              << QStringLiteral("--continue-at") << QStringLiteral("-")
              << QStringLiteral("--ftp-pasv")
              << QStringLiteral("--user")
              << QStringLiteral("%1:%2").arg(m_ftpUser, m_ftpPassword)
              << QStringLiteral("--output") << localFile
              << QString::fromUtf8(fileUrl.toEncoded());
    appendLog(QStringLiteral("下载 FTP 文件（支持断点续传）: %1").arg(fileName));
    m_ftpProcess->start(m_curlExecutable, arguments);
}

void TcpController::finishFtpDownload()
{
    m_ftpDownloading = false;
    m_ftpPhase = 0;
    emit ftpDownloadingChanged();
    appendLog(QStringLiteral("FTP 数据下载完成: %1").arg(m_ftpLocalDir));
    if (m_batchDownloadPhase) {
        m_collectStatus = QStringLiteral("数据下载完成");
        emit collectStatusChanged();
        markCurrentSuccess();
        QTimer::singleShot(0, this, &TcpController::startNextDeferredDownload);
        return;
    }
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
        m_deviceRetryTimer.stop();
        failCurrentSlot(QStringLiteral("连接设备信息端口超时"));
        break;
    case FlowState::ReadingDeviceInfo:
        failCurrentSlot(QStringLiteral("读取设备硬件信息超时"));
        break;
    case FlowState::ConnectingMqtt:
        failCurrentSlot(QStringLiteral("连接 MQTT 超时"));
        break;
    case FlowState::WaitingDeviceReady:
        failCurrentSlot(QStringLiteral("等待设备就绪超时（init_ready 仍为 0）"));
        break;
    case FlowState::CreatingProject:
        failCurrentSlot(QStringLiteral("创建设备扫描工程超时"));
        break;
    case FlowState::CreatingTask:
        failCurrentSlot(QStringLiteral("创建设备扫描任务超时"));
        break;
    case FlowState::Preinitializing:
        failCurrentSlot(QStringLiteral("等待设备扫描预初始化响应超时"));
        break;
    case FlowState::StartingModeling:
        failCurrentSlot(QStringLiteral("等待设备传感器进入真实扫描超时"));
        break;
    case FlowState::StartingCollection:
        failCurrentSlot(QStringLiteral("等待设备开始采集超时"));
        break;
    case FlowState::WaitingArmHome:
        failCurrentSlot(QStringLiteral("等待机械臂夹取设备并回到 Home 静置超时"));
        break;
    case FlowState::Calibrating:
        failCurrentSlot(QStringLiteral("机械臂标定运行超时"));
        break;
    case FlowState::StoppingCollection:
        failCurrentSlot(QStringLiteral("等待设备停止采集超时"));
        break;
    case FlowState::StoppingModeling:
        failCurrentSlot(QStringLiteral("等待设备停止扫描并保存工程超时"));
        break;
    case FlowState::EnsuringMeasurementStopped:
        ++m_guaranteedStopAttempts;
        appendLog(QStringLiteral("尚未确认设备完全停止，第 %1 次重新连接/重发停止指令；任务不会切换到下一槽位")
                      .arg(m_guaranteedStopAttempts));
        continueGuaranteedMeasurementStop();
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
    m_deviceRetryTimer.stop();
    m_mqttReconnectTimer.stop();
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
    m_mqttPortFailureCount = 0;
    m_mqttReconnectAttempts = 0;
    m_modelingActive = false;
    m_collectActive = false;
    m_armAtHomeReady = false;
    m_collectReadyForModeling = false;
    m_guaranteedStopActive = false;
    m_guaranteedStopAfterAbort = false;
    m_guaranteedStopNeedsPutBack = false;
    m_guaranteedStopAttempts = 0;
    m_guaranteedStopReason.clear();
    m_modelingStatus = -1;
    m_deviceStatusReceived = false;
    m_deviceInitReady = false;
    m_retryModelingAfterDeviceReady = false;
    m_resumeModelingAfterPreinit = false;

    if (m_wifiConnected) {
        m_wifiConnected = false;
        emit wifiConnectedChanged();
    }
    m_wifiDisconnectTicks = 0;
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
