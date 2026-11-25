#pragma once

#include <QObject>
#include <QQueue>
#include <QString>
#include <QUrl>

class QMediaPlayer;

// Centralized, queued voice prompts shared by SLAM and laser calibration.
// Queueing is important because tray-ready/change-tray/all-finished events can
// occur back-to-back and QSound would otherwise play them on top of each other.
class AudioFeedback final : public QObject
{
public:
    enum class Cue {
        AppStart,
        ConnectArm,
        LeftTrayReady,
        RightTrayReady,
        Reset,
        ChangeTray,
        AllFinished,
        ErrorCheck,
        ScanTimeout,
        BindTimeout,
        CalibrationTimeout
    };

    static void play(Cue cue);
    static void playFailure(const QString &reason);

private:
    AudioFeedback();

    static AudioFeedback &instance();
    static QUrl cueUrl(Cue cue);
    void enqueue(Cue cue);
    void playNext();

    QMediaPlayer *m_player = nullptr;
    QQueue<QUrl> m_queue;
    bool m_playing = false;
};
