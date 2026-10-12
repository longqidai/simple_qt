#ifndef LASERCALIBRATIONCONTROLLER_H
#define LASERCALIBRATIONCONTROLLER_H

#include <QObject>
#include <QSqlDatabase>
#include <QTcpSocket>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <functional>

#include "laservisionprocessor.h"

class LaserCalibrationController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool armConnected READ armConnected NOTIFY armConnectedChanged)
    Q_PROPERTY(bool receiverConnected READ receiverConnected NOTIFY receiverConnectedChanged)
    Q_PROPERTY(bool wifiConnected READ wifiConnected NOTIFY wifiConnectedChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(QString currentSn READ currentSn NOTIFY deviceInfoChanged)
    Q_PROPERTY(QString currentHardware READ currentHardware NOTIFY deviceInfoChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY deviceInfoChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(QString logText READ logText NOTIFY logChanged)
    Q_PROPERTY(double laserDistance READ laserDistance NOTIFY measurementChanged)
    Q_PROPERTY(double laserHeight READ laserHeight NOTIFY measurementChanged)
    Q_PROPERTY(double horizontalNear READ horizontalNear NOTIFY measurementChanged)
    Q_PROPERTY(double horizontalFar READ horizontalFar NOTIFY measurementChanged)
    Q_PROPERTY(double horizontalLength READ horizontalLength NOTIFY measurementChanged)
    Q_PROPERTY(double verticalNear READ verticalNear NOTIFY measurementChanged)
    Q_PROPERTY(double verticalFar READ verticalFar NOTIFY measurementChanged)
    Q_PROPERTY(double verticalLength READ verticalLength NOTIFY measurementChanged)
    Q_PROPERTY(bool cameraBusy READ cameraBusy NOTIFY cameraChanged)
    Q_PROPERTY(QString cameraStatus READ cameraStatus NOTIFY cameraChanged)
    Q_PROPERTY(QString cameraImageUrl READ cameraImageUrl NOTIFY cameraChanged)
    Q_PROPERTY(bool singleCameraDebug READ singleCameraDebug WRITE setSingleCameraDebug NOTIFY cameraChanged)
    Q_PROPERTY(int currentFace READ currentFace NOTIFY faceChanged)
    Q_PROPERTY(int faceRestBits READ faceRestBits NOTIFY faceRestBitsChanged)
    Q_PROPERTY(int successCount READ successCount NOTIFY resultsChanged)
    Q_PROPERTY(int failureCount READ failureCount NOTIFY resultsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY resultsChanged)
    Q_PROPERTY(int slotRevision READ slotRevision NOTIFY slotRevisionChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY resultsChanged)

public:
    explicit LaserCalibrationController(QObject *parent = nullptr);
    ~LaserCalibrationController() override;

    bool armConnected() const;
    bool receiverConnected() const;
    bool wifiConnected() const;
    bool running() const;
    bool paused() const;
    QString currentSn() const;
    QString currentHardware() const;
    QString currentVersion() const;
    QString progressText() const;
    QString errorText() const;
    QString logText() const;
    double laserDistance() const;
    double laserHeight() const;
    double horizontalNear() const;
    double horizontalFar() const;
    double horizontalLength() const;
    double verticalNear() const;
    double verticalFar() const;
    double verticalLength() const;
    bool cameraBusy() const;
    QString cameraStatus() const;
    QString cameraImageUrl() const;
    bool singleCameraDebug() const;
    void setSingleCameraDebug(bool enabled);
    int currentFace() const;
    int faceRestBits() const;
    int successCount() const;
    int failureCount() const;
    int activeCount() const;
    int slotRevision() const;
    QVariantList results() const;

    Q_INVOKABLE void connectArm(const QString &host = QStringLiteral("192.168.5.1"),
                                int port = 6601);
    Q_INVOKABLE void disconnectArm();
    Q_INVOKABLE bool startCalibration(const QVariantList &leftSlots,
                                      const QVariantList &rightSlots,
                                      const QVariantMap &measurements);
    Q_INVOKABLE void stopCalibration();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void resetReceiverCalibration();
    Q_INVOKABLE QString slotStatus(const QString &tray, int slot) const;
    Q_INVOKABLE bool exportResults(const QString &filePath = QString());
    Q_INVOKABLE QVariantList queryResults(const QString &sn = QString()) const;
    Q_INVOKABLE void clearLog();
    Q_INVOKABLE void shutdown();

signals:
    void armConnectedChanged();
    void receiverConnectedChanged();
    void wifiConnectedChanged();
    void runningChanged();
    void pausedChanged();
    void deviceInfoChanged();
    void progressChanged();
    void errorChanged();
    void logChanged();
    void logEntryAdded(const QString &entry);
    void logCleared();
    void measurementChanged();
    void cameraChanged();
    void faceChanged();
    void faceRestBitsChanged();
    void resultsChanged();
    void slotRevisionChanged();

private:
    enum class State {
        Idle,
        ArmHandshake,
        WaitingSn,
        ConnectingWifi,
        ConnectingReceiver,
        ReadingDeviceInfo,
        WaitingPoint1,
        WaitingFace1,
        WaitingPoint2,
        WaitingFace2,
        MeasuringVerticalNear,
        WaitingPoint3,
        MeasuringVerticalFar,
        WaitingPoint4,
        WaitingFace3,
        WaitingPoint5,
        WaitingFace4,
        WaitingPoint6,
        WaitingFace5,
        WaitingPoint7,
        WaitingFace6,
        MeasuringHorizontalFar,
        WaitingAngle,
        WaitingPutback,
        Aborting
    };

    enum class VisionTarget {
        None,
        VerticalNear,
        VerticalFar,
        HorizontalFar
    };

    struct Slot {
        QString tray;
        int slot = 0;
        QString status = QStringLiteral("pending");
    };

    struct LaserBack {
        bool valid = false;
        int restBits = 0;
        int restState = 0;
        int sideState = 0;
        int panelId = 0;
        QVector<double> faceParameters;
        QVector<double> bias;
        double yaw = 0.0;
        double pitch = 0.0;
        int errorCode = 0;
    };

    void configureSockets();
    void configureTimers();
    void setState(State state, const QString &progress, int timeoutMs = 0);
    void setProgress(const QString &text);
    void setError(const QString &text);
    void appendLog(const QString &text);
    void sendArm(const QByteArray &command);
    bool sendReceiver(const QByteArray &command);
    void processArmBuffer();
    void processArmFrame(const QString &frame);
    void processReceiverBuffer();
    void processReceiverLine(const QString &line);
    void processLaserRangeFrame(const QByteArray &frame);
    LaserBack parseLaserBack(const QString &line) const;
    void handleLaserBack(const LaserBack &report);
    void handleArmOk();
    void handleScannedSn(const QString &sn);

    bool validateMeasurements(const QVariantMap &measurements, QString *reason);
    void beginNextSlot();
    void beginCurrentSlot();
    void connectWifiByNamePart(const QString &namePart);
    QString connectedWifiName() const;
    bool receiverNetworkReady(QString *localAddress = nullptr) const;
    void connectReceiver();
    void requestReceiverInfo();
    void tryFinishReceiverInfo();
    void startPoint1();
    void requestPlanarCalibration(int face);
    void moveToPoint(int point);
    void startAngleCalibration();
    void startVisionMeasurement(VisionTarget target);
    void handleVisionMeasurement(bool success, double height,
                                 const QString &error,
                                 const QString &imageDataUrl,
                                 int cameraIndex);
    bool calculateAngleInputs(QString *reason);
    void completeCurrentSlot(bool success, const QString &reason);
    void failCurrentSlot(const QString &reason);
    void finishBatch();
    void ensureReceiverSafe();
    void finishAbort();
    void continueIfNotPaused(const std::function<void()> &action);

    void updateSlotStatus(const QString &tray, int slot, const QString &status);
    void addResult(bool success, const QString &reason);
    bool initializeDatabase();
    void saveResultToDatabase(const QVariantMap &row);
    QString defaultExportPath() const;
    static QString normalizeSn(const QString &value);
    static bool finiteNumber(const QVariant &value, double *result);

    QTcpSocket m_armSocket;
    QTcpSocket m_receiverSocket;
    QTimer m_handshakeTimer;
    QTimer m_stepTimer;
    QTimer m_wifiTimer;
    QTimer m_receiverRetryTimer;
    LaserVisionProcessor m_vision; //构造

    QByteArray m_armBuffer;
    QByteArray m_receiverBuffer;
    // 当前面校准等待期间的诊断数据，不参与标定状态判定。
    bool m_faceDiagnosticsActive = false;
    qint64 m_faceRxBytes = 0;
    int m_faceRxChunks = 0;
    int m_faceValidReports = 0;
    int m_faceInvalidReports = 0;
    bool m_faceBufferWaitLogged = false;
    QByteArray m_faceLastRx;
    State m_state = State::Idle;
    bool m_armReady = false;
    bool m_wifiConnected = false;
    bool m_running = false;
    bool m_paused = false;
    bool m_shutdown = false;
    bool m_currentResultRecorded = false;
    bool m_pendingSuccess = false;
    bool m_receiverSnReceived = false;
    int m_wifiAttempts = 0;
    int m_receiverAttempts = 0;
    int m_receiverReconnectAttempts = 0;
    int m_currentIndex = -1;
    int m_currentFace = 0;
    // $LASERBACK 的六位静置完成掩码：bit0~bit5 分别对应面1~面6。
    int m_faceRestBits = 0;
    int m_slotRevision = 0;
    VisionTarget m_visionTarget = VisionTarget::None;

    QString m_armHost = QStringLiteral("192.168.5.1");
    int m_armPort = 6601;
    QString m_receiverHost = QStringLiteral("192.168.1.8");
    int m_receiverPort = 25001;
    QString m_currentSn;
    QString m_currentHardware;
    QString m_currentVersion;
    QString m_progressText = QStringLiteral("等待连接机械臂");
    QString m_errorText;
    QString m_pendingReason;
    QString m_cameraStatus = QStringLiteral("等待首次视觉测量");
    QString m_cameraImageUrl;
    QStringList m_logLines;

    double m_laserDistance = 0.0;
    double m_laserHeight = 0.0;
    double m_horizontalNear = 0.0;
    double m_horizontalFar = 0.0;
    double m_horizontalLength = 0.0;
    double m_verticalNear = 0.0;
    double m_verticalFar = 0.0;
    double m_verticalLength = 0.0;
    double m_horizontalLineHeight = 0.0;
    double m_verticalCloseRawHeight = 0.0;
    double m_verticalCloseRange = 0.0;
    double m_verticalFarRawHeight = 0.0;
    double m_verticalFarRange = 0.0;
    double m_horizontalFarRawHeight = 0.0;
    double m_horizontalFarRange = 0.0;
    double m_finalPitch = 0.0;
    double m_finalYaw = 0.0;
    QVector<double> m_finalBias;

    QList<Slot> m_slots;
    QVariantList m_results;
    QSqlDatabase m_database;
    QString m_databaseConnectionName;
};

#endif
