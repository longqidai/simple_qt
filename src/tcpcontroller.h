#ifndef TCPCONTROLLER_H
#define TCPCONTROLLER_H

#include <QByteArray>
#include <QObject>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVector>

class QMqttClient;
class QProcess;

class TcpController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool armReady READ armReady NOTIFY armReadyChanged)
    Q_PROPERTY(bool armRunning READ armRunning NOTIFY armRunningChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString currentProgress READ currentProgress NOTIFY currentProgressChanged)
    Q_PROPERTY(QStringList debugLog READ debugLog NOTIFY debugLogChanged)
    Q_PROPERTY(QString host READ host WRITE setHost NOTIFY hostChanged)
    Q_PROPERTY(int port READ port WRITE setPort NOTIFY portChanged)

    Q_PROPERTY(QString wifiNamePart READ wifiNamePart NOTIFY wifiNamePartChanged)
    Q_PROPERTY(bool wifiConnected READ wifiConnected NOTIFY wifiConnectedChanged)
    Q_PROPERTY(bool deviceTcpConnected READ deviceTcpConnected NOTIFY deviceTcpConnectedChanged)
    Q_PROPERTY(bool mqttConnected READ mqttConnected NOTIFY mqttConnectedChanged)
    Q_PROPERTY(bool ftpDownloading READ ftpDownloading NOTIFY ftpDownloadingChanged)
    Q_PROPERTY(QString collectStatus READ collectStatus NOTIFY collectStatusChanged)
    Q_PROPERTY(QString collectPath READ collectPath NOTIFY collectPathChanged)

    Q_PROPERTY(QString currentTray READ currentTray NOTIFY currentDeviceChanged)
    Q_PROPERTY(int currentSlot READ currentSlot NOTIFY currentDeviceChanged)
    Q_PROPERTY(QString currentSn READ currentSn NOTIFY currentDeviceChanged)
    Q_PROPERTY(QString currentHardware READ currentHardware NOTIFY currentDeviceChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY currentDeviceChanged)
    Q_PROPERTY(QStringList leftSlotStates READ leftSlotStates NOTIFY slotStatesChanged)
    Q_PROPERTY(QStringList rightSlotStates READ rightSlotStates NOTIFY slotStatesChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY statisticsChanged)
    Q_PROPERTY(int successCount READ successCount NOTIFY statisticsChanged)
    Q_PROPERTY(int failureCount READ failureCount NOTIFY statisticsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit TcpController(QObject *parent = nullptr);

    bool connected() const;
    bool armReady() const;
    bool armRunning() const;
    QString statusText() const;
    QString currentProgress() const;
    QStringList debugLog() const;
    QString host() const;
    int port() const;

    QString wifiNamePart() const;
    bool wifiConnected() const;
    bool deviceTcpConnected() const;
    bool mqttConnected() const;
    bool ftpDownloading() const;
    QString collectStatus() const;
    QString collectPath() const;

    QString currentTray() const;
    int currentSlot() const;
    QString currentSn() const;
    QString currentHardware() const;
    QString currentVersion() const;
    QStringList leftSlotStates() const;
    QStringList rightSlotStates() const;
    int selectedCount() const;
    int successCount() const;
    int failureCount() const;
    QString lastError() const;

    void setHost(const QString &host);
    void setPort(int port);

public slots:
    void toggleConnection();
    void connectToServer();
    void disconnectFromServer();
    void toggleArmTask(const QVariantList &leftSlots, const QVariantList &rightSlots);
    void clearTaskResults();

signals:
    void connectedChanged();
    void armReadyChanged();
    void armRunningChanged();
    void statusTextChanged();
    void currentProgressChanged();
    void debugLogChanged();
    void hostChanged();
    void portChanged();
    void wifiNamePartChanged();
    void wifiConnectedChanged();
    void deviceTcpConnectedChanged();
    void mqttConnectedChanged();
    void ftpDownloadingChanged();
    void collectStatusChanged();
    void collectPathChanged();
    void currentDeviceChanged();
    void slotStatesChanged();
    void statisticsChanged();
    void lastErrorChanged();
    void commandReceived(const QString &command);

private slots:
    void onArmConnected();
    void onArmDisconnected();
    void onArmError(QAbstractSocket::SocketError socketError);
    void onArmReadyRead();
    void onDeviceConnected();
    void onDeviceDisconnected();
    void onDeviceError(QAbstractSocket::SocketError socketError);
    void onDeviceReadyRead();
    void checkWifiConnection();
    void onHandshakeTimeout();
    void onStepTimeout();

private:
    enum class FlowState {
        Idle,
        WaitingHandshake,
        Ready,
        WaitingScan,
        ConnectingWifi,
        ConnectingDevice,
        ReadingDeviceInfo,
        ConnectingMqtt,
        StartingCollection,
        Calibrating,
        StoppingCollection,
        Downloading,
        WaitingPutBack,
        Recovering,
        Completed,
        Aborted
    };

    struct SlotTarget {
        bool left;
        int slot;
    };

    void setProgress(const QString &text);
    void setFlowState(FlowState state, const QString &progress, int timeoutMs = 0);
    void appendLog(const QString &text);
    void setLastError(const QString &text);
    bool sendArmCommand(const QString &command);
    void processArmBuffer();
    void handleArmCommand(const QString &command);
    QString normalizedArmCommand(const QString &command) const;

    void beginTask(const QVariantList &leftSlots, const QVariantList &rightSlots);
    void abortTask();
    void startNextSlot();
    void finishTask();
    void resetCurrentDevice();
    void setCurrentSlotState(const QString &state);
    QString currentSlotState() const;
    void markCurrentSuccess();
    void failCurrentSlot(const QString &reason);
    void recordCurrentResult(const QString &result, const QString &reason = QString());
    bool stateNeedsPutBack(FlowState state) const;
    void requestPutBack(bool afterFailure);

    void handleScannedSn(const QString &sn);
    void connectWifiByNamePart(const QString &namePart);
    bool isConnectedToTargetWifi() const;
    void connectDeviceTcp();
    void requestDeviceInfo();
    void processDeviceBuffer();
    void parseDeviceTextLine(const QString &line);
    void tryCompleteDeviceInfo();

    void connectMqtt();
    bool publishCollectAction(const QString &action);
    void handleMqttMessage(const QByteArray &message, const QString &topic);

    void startFtpDownload();
    void handleFtpProcessFinished(int exitCode, int exitStatus);
    void startNextFtpFile();
    void finishFtpDownload();

    void stopStepTimer();
    void cleanupDeviceConnections();
    void updateStatistics();

private:
    QTcpSocket *m_armSocket;
    QTcpSocket *m_deviceSocket;
    QMqttClient *m_mqttClient;
    QProcess *m_ftpProcess;

    QTimer m_wifiStatusTimer;
    QTimer m_handshakeTimer;
    QTimer m_stepTimer;

    bool m_connected;
    bool m_armReady;
    bool m_taskActive;
    bool m_wifiConnected;
    bool m_deviceTcpConnected;
    bool m_mqttConnected;
    bool m_ftpDownloading;
    bool m_mqttSubscribed;
    bool m_failureAwaitingPutBack;
    bool m_aborting;
    bool m_cleaningUp;

    QString m_statusText;
    QString m_currentProgress;
    QStringList m_debugLog;
    QString m_host;
    int m_port;
    QByteArray m_armReceiveBuffer;
    QByteArray m_deviceReceiveBuffer;
    int m_handshakeAttempts;
    int m_wifiConnectTicks;

    FlowState m_flowState;
    QVector<SlotTarget> m_taskQueue;
    QVector<bool> m_resultRecorded;
    int m_currentQueueIndex;
    QStringList m_leftSlotStates;
    QStringList m_rightSlotStates;
    int m_selectedCount;
    int m_successCount;
    int m_failureCount;
    QString m_lastError;

    QString m_wifiNamePart;
    QString m_currentSn;
    QString m_currentHardware;
    QString m_currentVersion;

    QString m_deviceHost;
    int m_devicePort;
    QString m_mqttHost;
    int m_mqttPort;

    QUrl m_ftpUrl;
    QString m_ftpUser;
    QString m_ftpPassword;
    QString m_ftpLocalDir;
    QString m_curlExecutable;
    QStringList m_ftpFiles;
    int m_ftpPhase;
    int m_ftpTotalFiles;
    int m_ftpDownloadedFiles;

    QString m_collectStatus;
    QString m_collectPath;
};

#endif // TCPCONTROLLER_H
