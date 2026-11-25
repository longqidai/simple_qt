#include "audiofeedback.h"

#include <QMediaPlayer>
#include <QTimer>

AudioFeedback::AudioFeedback()
    : m_player(new QMediaPlayer(this))
{
    m_player->setVolume(100);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status != QMediaPlayer::EndOfMedia
            && status != QMediaPlayer::InvalidMedia) {
            return;
        }
        if (!m_playing)
            return;
        m_playing = false;
        QTimer::singleShot(0, this, [this] { playNext(); });
    });
}

AudioFeedback &AudioFeedback::instance()
{
    // Keep the player alive until process exit. This also avoids destroying a
    // multimedia backend after QGuiApplication has already been torn down.
    static AudioFeedback *feedback = new AudioFeedback;
    return *feedback;
}

void AudioFeedback::play(Cue cue)
{
    instance().enqueue(cue);
}

void AudioFeedback::playFailure(const QString &reason)
{
    if (!reason.contains(QStringLiteral("超时"), Qt::CaseInsensitive)) {
        play(Cue::ErrorCheck);
        return;
    }

    if (reason.contains(QStringLiteral("扫码"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("扫描"), Qt::CaseInsensitive)) {
        play(Cue::ScanTimeout);
        return;
    }

    if (reason.contains(QStringLiteral("标定"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("校准"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("角度"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("视觉"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("采集"), Qt::CaseInsensitive)
        || reason.contains(QStringLiteral("LASERBACK"), Qt::CaseInsensitive)) {
        play(Cue::CalibrationTimeout);
        return;
    }

    play(Cue::BindTimeout);
}

QUrl AudioFeedback::cueUrl(Cue cue)
{
    switch (cue) {
    case Cue::AppStart:           return QUrl(QStringLiteral("qrc:/audio/appStart.wav"));
    case Cue::ConnectArm:         return QUrl(QStringLiteral("qrc:/audio/connectArm.wav"));
    case Cue::LeftTrayReady:      return QUrl(QStringLiteral("qrc:/audio/leftAllReady.wav"));
    case Cue::RightTrayReady:     return QUrl(QStringLiteral("qrc:/audio/rightAllReady.wav"));
    case Cue::Reset:              return QUrl(QStringLiteral("qrc:/audio/reset.wav"));
    case Cue::AllFinished:        return QUrl(QStringLiteral("qrc:/audio/allEnd.wav"));
    case Cue::ChangeTray:         return QUrl(QStringLiteral("qrc:/audio/changePlat.wav"));
    case Cue::ErrorCheck:         return QUrl(QStringLiteral("qrc:/audio/errCheck.wav"));
    case Cue::ScanTimeout:        return QUrl(QStringLiteral("qrc:/audio/scanQRovertime.wav"));
    case Cue::BindTimeout:        return QUrl(QStringLiteral("qrc:/audio/bindOvertime.wav"));
    case Cue::CalibrationTimeout: return QUrl(QStringLiteral("qrc:/audio/calResultOvertime.wav"));
    }
    return {};
}

void AudioFeedback::enqueue(Cue cue)
{
    const QUrl url = cueUrl(cue);
    if (!url.isValid())
        return;
    m_queue.enqueue(url);
    playNext();
}

void AudioFeedback::playNext()
{
    if (m_playing || m_queue.isEmpty())
        return;
    m_playing = true;
    m_player->setMedia(m_queue.dequeue());
    m_player->play();
}
