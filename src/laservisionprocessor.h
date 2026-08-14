#ifndef LASERVISIONPROCESSOR_H
#define LASERVISIONPROCESSOR_H

#include <QObject>
#include <QFutureWatcher>
#include <QString>

// Runs the legacy two-camera OpenCV measurement outside the QML thread.
// The USB cameras are intentionally mapped exactly like the previous host:
// of the last two usable DirectShow cameras, the first is short-focus and
// the second is long-focus.  Keep the two cameras in the same USB-Hub order.
class LaserVisionProcessor final : public QObject
{
    Q_OBJECT

public:
    explicit LaserVisionProcessor(QObject *parent = nullptr);
    ~LaserVisionProcessor() override;

    bool busy() const;
    bool singleCameraDebug() const;
    void setSingleCameraDebug(bool enabled);
    bool requestMeasurement(bool shortFocus);
    void cancelPendingResult();

signals:
    void measurementFinished(bool success, double height,
                             const QString &error,
                             const QString &imageDataUrl,
                             int cameraIndex);
    void cameraMappingChanged(int shortCameraIndex, int longCameraIndex,
                              const QString &description);

private:
    struct Result {
        bool success = false;
        double height = 0.0;
        QString error;
        QString imageDataUrl;
        int cameraIndex = -1;
        int shortCameraIndex = -1;
        int longCameraIndex = -1;
    };

    static Result runMeasurement(bool shortFocus,
                                 int shortCameraIndex,
                                 int longCameraIndex,
                                 bool singleCameraDebug);

    QFutureWatcher<Result> m_watcher;
    int m_shortCameraIndex = -1;
    int m_longCameraIndex = -1;
    bool m_singleCameraDebug = false;
    quint64 m_generation = 0;
    quint64 m_runningGeneration = 0;
};

#endif
