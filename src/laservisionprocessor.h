#ifndef LASERVISIONPROCESSOR_H
#define LASERVISIONPROCESSOR_H

#include <QObject>
#include <QFutureWatcher>
#include <QString>
#include <QStringList>

// 单远焦摄像头：后台执行垂直远点和水平远点视觉测量。
// 近点由控制器提供工装常量；singleCameraDebug 只控制是否使用模拟数值，
// 正式模式同样只需要一只摄像头。保留旧映射信号的近焦参数（固定为 -1）。
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
    // 一次视觉测量结束后，把算法关键步骤的摘要送回主线程写入运行日志。
    // 只输出阶段性结果，不逐帧刷屏。
    void diagnosticMessage(const QString &message);

private:
    struct Result {
        bool success = false;
        double height = 0.0;
        QString error;
        QString imageDataUrl;
        int cameraIndex = -1;
        int shortCameraIndex = -1;
        int longCameraIndex = -1;
        QStringList diagnostics;
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
