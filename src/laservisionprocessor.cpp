#include "laservisionprocessor.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QBuffer>
#include <QImage>
#include <QThread>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kFrameWidth = 1920;
constexpr int kFrameHeight = 1080;
constexpr double kShortReferenceLengthMm = 566.0;
constexpr double kLongReferenceLengthMm = 1057.0;
constexpr int kRequiredSamples = 4; // old PICTURECAL_TIMES=3 means first + 3 updates
constexpr int kMaximumFrames = 120;

struct FrameResult {
    bool success = false;
    double height = 0.0;
    QString error;
    cv::Mat annotated;
};

QString imageDataUrl(const cv::Mat &bgr)
{
    if (bgr.empty())
        return QString();
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    QImage image(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                 QImage::Format_RGB888);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly)
        || !image.copy().scaled(960, 540, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation).save(&buffer, "PNG")) {
        return QString();
    }
    return QStringLiteral("data:image/png;base64,")
        + QString::fromLatin1(bytes.toBase64());
}

bool findRedReference(const cv::Mat &image, bool shortFocus,
                      cv::Point2f *first, cv::Point2f *second,
                      QString *error)
{
    cv::Mat mask;
    const cv::Scalar lower = shortFocus ? cv::Scalar(0, 0, 60)
                                         : cv::Scalar(0, 0, 85);
    const cv::Scalar upper = shortFocus ? cv::Scalar(165, 165, 255)
                                         : cv::Scalar(85, 85, 215);
    cv::inRange(image, lower, upper, mask);
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(3, 3)));
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(7, 7)));

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);
    struct Candidate { cv::Point2f center; double area = 0.0; };
    std::vector<Candidate> candidates;
    for (const auto &contour : contours) {
        const double area = cv::contourArea(contour);
        if (area < 100.0 || area > 5000.0)
            continue;
        cv::Point2f center;
        float radius = 0.0f;
        cv::minEnclosingCircle(contour, center, radius);
        if (radius < 5.0f || radius > 100.0f)
            continue;
        const double ratio = area / (CV_PI * radius * radius);
        if (ratio < 0.55 || ratio > 1.1)
            continue;
        candidates.push_back({center, area});
    }
    if (candidates.size() < 2) {
        if (error)
            *error = QStringLiteral("未识别到两个红色参考圆（检测到 %1 个）")
                         .arg(candidates.size());
        return false;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  return a.area > b.area;
              });
    cv::Point2f a = candidates[0].center;
    cv::Point2f b = candidates[1].center;
    const double distance = cv::norm(a - b);
    if (distance < 20.0 || distance > 800.0) {
        if (error)
            *error = QStringLiteral("红色参考圆间距异常: %1 px")
                         .arg(distance, 0, 'f', 1);
        return false;
    }
    if (a.x > b.x)
        std::swap(a, b);
    *first = a;
    *second = b;
    return true;
}

bool likelyLaserColor(const cv::Vec3b &pixel)
{
    const int b = pixel[0];
    const int g = pixel[1];
    const int r = pixel[2];
    const int maximum = std::max({r, g, b});
    if (maximum < 220)
        return false;
    if (maximum == 255 && std::min({r, g, b}) > 240)
        return true;
    return g >= r && g >= b;
}

bool findLaserSpot(const cv::Mat &image, bool shortFocus,
                   cv::Point2f *center, cv::Mat *debugMask,
                   QString *error)
{
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    const int kernelSize = shortFocus ? 10 : 15;
    cv::Mat topHat;
    cv::morphologyEx(gray, topHat, cv::MORPH_TOPHAT,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(kernelSize, kernelSize)));
    cv::Mat mask;
    cv::threshold(topHat, mask, shortFocus ? 10 : 15, 255,
                  cv::THRESH_BINARY);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(kernelSize, kernelSize)));

    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(
        mask, labels, stats, centroids, 4, CV_32S);
    std::vector<int> candidates;
    for (int i = 1; i < count; ++i) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        const int left = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int top = stats.at<int>(i, cv::CC_STAT_TOP);
        const int width = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (area < 10 || area > 2000 || width <= 0 || height <= 0)
            continue;
        const double aspect = static_cast<double>(std::max(width, height))
            / std::min(width, height);
        if (aspect > 30.0)
            continue;

        int validColors = 0;
        int total = 0;
        for (int y = top; y < top + height; ++y) {
            for (int x = left; x < left + width; ++x) {
                if (labels.at<int>(y, x) != i)
                    continue;
                ++total;
                if (likelyLaserColor(image.at<cv::Vec3b>(y, x)))
                    ++validColors;
            }
        }
        if (total == 0 || static_cast<double>(validColors) / total < 0.3)
            continue;

        if (!shortFocus) {
            const cv::Point2f centroid(
                static_cast<float>(centroids.at<double>(i, 0)),
                static_cast<float>(centroids.at<double>(i, 1)));
            int greenHalo = 0;
            for (int dy = -15; dy <= 15; ++dy) {
                for (int dx = -15; dx <= 15; ++dx) {
                    const double d = std::sqrt(double(dx * dx + dy * dy));
                    if (d <= 5.0 || d > 15.0)
                        continue;
                    const int x = static_cast<int>(centroid.x) + dx;
                    const int y = static_cast<int>(centroid.y) + dy;
                    if (x < 0 || x >= image.cols || y < 0 || y >= image.rows)
                        continue;
                    const cv::Vec3b p = image.at<cv::Vec3b>(y, x);
                    const int maximum = std::max({int(p[0]), int(p[1]), int(p[2])});
                    if (p[1] > p[2] + 20 && p[1] > p[0] + 20
                        && maximum > 80 && maximum < 255) {
                        ++greenHalo;
                    }
                }
            }
            if (greenHalo < 20)
                continue;
        }
        candidates.push_back(i);
    }

    if (candidates.empty()) {
        if (debugMask)
            *debugMask = mask;
        if (error)
            *error = QStringLiteral("未识别到绿色激光点");
        return false;
    }
    const int best = *std::max_element(
        candidates.begin(), candidates.end(), [&stats](int a, int b) {
            return stats.at<int>(a, cv::CC_STAT_AREA)
                < stats.at<int>(b, cv::CC_STAT_AREA);
        });
    center->x = static_cast<float>(centroids.at<double>(best, 0));
    center->y = static_cast<float>(centroids.at<double>(best, 1));
    if (debugMask)
        *debugMask = labels == best;
    return true;
}

bool findLaserLineDistance(const cv::Mat &image, const cv::Point2f &spot,
                           const cv::Point2f &referenceA,
                           const cv::Point2f &referenceB,
                           bool shortFocus, double *distance,
                           cv::Vec4f *fittedLine, cv::Point2f *intersection,
                           QString *error)
{
    std::vector<cv::Mat> channels;
    cv::split(image, channels);
    cv::Mat greenStrong = (channels[1] > channels[2] + 30)
        & (channels[1] > channels[0] + 20) & (channels[1] > 190);
    cv::morphologyEx(greenStrong, greenStrong, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT,
                                               cv::Size(15, 1)));
    const int halfWidth = shortFocus ? 100 : 150;
    const int left = std::max(0, static_cast<int>(spot.x) - halfWidth);
    const int right = std::min(image.cols - 1,
                               static_cast<int>(spot.x) + halfWidth);
    cv::Mat roi = cv::Mat::zeros(greenStrong.size(), CV_8UC1);
    roi(cv::Rect(left, 0, right - left + 1, image.rows)).setTo(255);
    greenStrong.setTo(0, roi == 0);

    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(
        greenStrong, labels, stats, centroids, 8, CV_32S);
    int best = -1;
    double bestScore = 0.0;
    for (int i = 1; i < count; ++i) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        const int width = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (area < 30 || width <= 0 || height <= 0
            || centroids.at<double>(i, 1) < spot.y + 50.0) {
            continue;
        }
        std::vector<cv::Point> points;
        cv::findNonZero(labels == i, points);
        if (points.size() < 5)
            continue;
        const double score = std::sqrt(double(width * width + height * height))
            / std::min(width, height) * area;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    if (best < 0) {
        if (error)
            *error = QStringLiteral("未识别到激光线");
        return false;
    }
    std::vector<cv::Point> points;
    cv::findNonZero(labels == best, points);
    cv::fitLine(points, *fittedLine, cv::DIST_L2, 0, 0.01, 0.01);

    const double vx = (*fittedLine)[0];
    const double vy = (*fittedLine)[1];
    const double x0 = (*fittedLine)[2];
    const double y0 = (*fittedLine)[3];
    const cv::Point2f direction = referenceB - referenceA;
    const double a = vy;
    const double b = -vx;
    const double c = -vy * x0 + vx * y0;
    const double denominator = a * direction.x + b * direction.y;
    if (std::abs(denominator) < 1e-6) {
        if (error)
            *error = QStringLiteral("参考圆连线与激光线平行，无法计算高度");
        return false;
    }
    const double t = -(a * spot.x + b * spot.y + c) / denominator;
    *intersection = spot + direction * static_cast<float>(t);
    *distance = cv::norm(spot - *intersection);
    return std::isfinite(*distance) && *distance > 0.0;
}

FrameResult processFrame(const cv::Mat &frame, bool shortFocus)
{
    FrameResult result;
    if (frame.empty()) {
        result.error = QStringLiteral("摄像头返回空图像");
        return result;
    }
    cv::Point2f referenceA, referenceB;
    if (!findRedReference(frame, shortFocus, &referenceA, &referenceB,
                          &result.error)) {
        return result;
    }
    const double referencePixels = cv::norm(referenceB - referenceA);
    const double pixelPerMm = referencePixels
        / (shortFocus ? kShortReferenceLengthMm : kLongReferenceLengthMm);
    if (!std::isfinite(pixelPerMm) || pixelPerMm <= 0.0) {
        result.error = QStringLiteral("红色参考物比例无效");
        return result;
    }

    cv::Point2f spot;
    cv::Mat spotMask;
    if (!findLaserSpot(frame, shortFocus, &spot, &spotMask, &result.error))
        return result;
    double lineDistance = 0.0;
    cv::Vec4f laserLine;
    cv::Point2f foot;
    if (!findLaserLineDistance(frame, spot, referenceA, referenceB,
                               shortFocus, &lineDistance, &laserLine, &foot,
                               &result.error)) {
        return result;
    }
    result.height = lineDistance / pixelPerMm;
    if (!std::isfinite(result.height) || result.height <= 0.0
        || result.height > 5000.0) {
        result.error = QStringLiteral("视觉计算高度超出有效范围: %1 mm")
                           .arg(result.height, 0, 'f', 3);
        return result;
    }

    result.annotated = frame.clone();
    cv::circle(result.annotated, referenceA, 6, cv::Scalar(255, 0, 0), -1);
    cv::circle(result.annotated, referenceB, 6, cv::Scalar(255, 0, 0), -1);
    cv::line(result.annotated, referenceA, referenceB,
             cv::Scalar(0, 0, 255), 2);
    cv::circle(result.annotated, spot, 8, cv::Scalar(0, 255, 255), 2);
    cv::circle(result.annotated, foot, 8, cv::Scalar(255, 255, 0), 2);
    cv::line(result.annotated, spot, foot, cv::Scalar(255, 0, 255), 2);
    const cv::Point2f lineA(laserLine[2] - 1500 * laserLine[0],
                            laserLine[3] - 1500 * laserLine[1]);
    const cv::Point2f lineB(laserLine[2] + 1500 * laserLine[0],
                            laserLine[3] + 1500 * laserLine[1]);
    cv::line(result.annotated, lineA, lineB, cv::Scalar(0, 255, 0), 2);
    cv::putText(result.annotated,
                std::string("height=") + std::to_string(result.height) + " mm",
                cv::Point(30, 50), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                cv::Scalar(0, 255, 255), 2);
    result.success = true;
    return result;
}

std::vector<int> availableCameraIndexes()
{
    std::vector<int> indexes;
    for (int index = 0; index < 5; ++index) {
        cv::VideoCapture camera(index, cv::CAP_DSHOW);
        if (!camera.isOpened())
            continue;
        cv::Mat frame;
        for (int retry = 0; retry < 3 && frame.empty(); ++retry) {
            camera.read(frame);
            if (frame.empty())
                QThread::msleep(80);
        }
        camera.release();
        if (!frame.empty())
            indexes.push_back(index);
    }
    return indexes;
}

} // namespace

LaserVisionProcessor::LaserVisionProcessor(QObject *parent)
    : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
        const Result result = m_watcher.result();
        m_shortCameraIndex = result.shortCameraIndex;
        m_longCameraIndex = result.longCameraIndex;
        emit cameraMappingChanged(
            m_shortCameraIndex, m_longCameraIndex,
            m_shortCameraIndex >= 0 && m_longCameraIndex >= 0
                ? (m_shortCameraIndex == m_longCameraIndex
                       ? QStringLiteral("单摄像头调试：USB %1 同时模拟近焦和远焦")
                             .arg(m_shortCameraIndex)
                       : QStringLiteral("近焦 USB %1，远焦 USB %2（沿用旧版 Hub 顺序）")
                             .arg(m_shortCameraIndex).arg(m_longCameraIndex))
                : QStringLiteral("未找到两只可用 USB 摄像头"));
        if (m_runningGeneration != m_generation)
            return;
        emit measurementFinished(result.success, result.height, result.error,
                                 result.imageDataUrl, result.cameraIndex);
    });
}

LaserVisionProcessor::~LaserVisionProcessor()
{
    ++m_generation;
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
}

bool LaserVisionProcessor::busy() const
{
    return m_watcher.isRunning();
}

bool LaserVisionProcessor::singleCameraDebug() const
{
    return m_singleCameraDebug;
}

void LaserVisionProcessor::setSingleCameraDebug(bool enabled)
{
    if (busy() || m_singleCameraDebug == enabled)
        return;
    m_singleCameraDebug = enabled;
    m_shortCameraIndex = -1;
    m_longCameraIndex = -1;
}

bool LaserVisionProcessor::requestMeasurement(bool shortFocus)
{
    if (busy())
        return false;
    m_runningGeneration = ++m_generation;
    m_watcher.setFuture(QtConcurrent::run(
        &LaserVisionProcessor::runMeasurement, shortFocus,
        m_shortCameraIndex, m_longCameraIndex, m_singleCameraDebug));
    return true;
}

void LaserVisionProcessor::cancelPendingResult()
{
    ++m_generation;
}

LaserVisionProcessor::Result LaserVisionProcessor::runMeasurement(
    bool shortFocus, int shortCameraIndex, int longCameraIndex,
    bool singleCameraDebug)
{
    Result result;
    if (shortCameraIndex < 0 || longCameraIndex < 0) {
        const std::vector<int> cameras = availableCameraIndexes();
        const std::size_t requiredCount = singleCameraDebug ? 1u : 2u;
        if (cameras.size() < requiredCount) {
            result.error = QStringLiteral(
                "摄像头数量不足：%1模式需要 %2 只，当前检测到 %3 只")
                               .arg(singleCameraDebug
                                        ? QStringLiteral("单摄像头调试")
                                        : QStringLiteral("正式双摄像头"))
                               .arg(requiredCount)
                               .arg(cameras.size());
            return result;
        }
        if (singleCameraDebug) {
            // Debug only: reuse one physical camera for both focus roles.
            shortCameraIndex = cameras.back();
            longCameraIndex = cameras.back();
        } else {
            // Legacy NEW_CAMERA mapping: 2 cameras => 0/1, 3 cameras => 1/2.
            shortCameraIndex = cameras[cameras.size() - 2];
            longCameraIndex = cameras.back();
        }
    }
    result.shortCameraIndex = shortCameraIndex;
    result.longCameraIndex = longCameraIndex;
    result.cameraIndex = shortFocus ? shortCameraIndex : longCameraIndex;

    cv::VideoCapture camera(result.cameraIndex, cv::CAP_DSHOW);
    if (!camera.isOpened()) {
        result.error = QStringLiteral("无法打开%1摄像头（索引 %2），可能被其他程序占用")
                           .arg(shortFocus ? QStringLiteral("近焦")
                                           : QStringLiteral("远焦"))
                           .arg(result.cameraIndex);
        return result;
    }
    camera.set(cv::CAP_PROP_FRAME_WIDTH, kFrameWidth);
    camera.set(cv::CAP_PROP_FRAME_HEIGHT, kFrameHeight);
    camera.set(cv::CAP_PROP_FPS, 30);
    for (int i = 0; i < 8; ++i) {
        cv::Mat warmup;
        camera.read(warmup);
        QThread::msleep(35);
    }

    // Simulation mode still opens the selected USB camera and obtains a real
    // frame, so camera availability and the surrounding workflow are tested.
    // The numeric result is synthetic, however: reference circles and a laser
    // spot are deliberately not required in this mode.
    if (singleCameraDebug) {
        cv::Mat frame;
        for (int retry = 0; retry < 20 && frame.empty(); ++retry) {
            camera.read(frame);
            if (frame.empty())
                QThread::msleep(40);
        }
        camera.release();
        if (frame.empty()) {
            result.error = QStringLiteral("模拟测试失败：摄像头已打开，但未读取到图像");
            return result;
        }

        cv::Mat annotated = frame.clone();
        cv::putText(annotated, "SIMULATED MEASUREMENT",
                    cv::Point(30, 55), cv::FONT_HERSHEY_SIMPLEX, 1.1,
                    cv::Scalar(0, 215, 255), 2, cv::LINE_AA);
        result.height = shortFocus ? 615.0 : 625.0;
        result.imageDataUrl = imageDataUrl(annotated);
        result.success = true;
        return result;
    }

    std::vector<double> heights;
    cv::Mat latestAnnotated;
    QString latestError = QStringLiteral("尚未获得有效图像");
    for (int frameIndex = 0;
         frameIndex < kMaximumFrames
         && static_cast<int>(heights.size()) < kRequiredSamples;
         ++frameIndex) {
        cv::Mat frame;
        if (!camera.read(frame) || frame.empty()) {
            latestError = QStringLiteral("摄像头读帧失败");
            QThread::msleep(30);
            continue;
        }
        const FrameResult measurement = processFrame(frame, shortFocus);
        if (!measurement.success) {
            latestError = measurement.error;
            continue;
        }
        latestAnnotated = measurement.annotated;
        if (!heights.empty()) {
            std::vector<double> sorted = heights;
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            const double difference = std::abs(measurement.height - median);
            if (difference > 18.0) {
                heights.clear();
            } else if (difference > 5.0) {
                continue;
            }
        }
        heights.push_back(measurement.height);
    }
    camera.release();
    if (heights.size() < kRequiredSamples) {
        result.error = QStringLiteral("视觉测量失败：%1（有效帧 %2/%3）")
                           .arg(latestError)
                           .arg(heights.size()).arg(kRequiredSamples);
        if (!latestAnnotated.empty())
            result.imageDataUrl = imageDataUrl(latestAnnotated);
        return result;
    }
    std::sort(heights.begin(), heights.end());
    result.height = heights.size() % 2
        ? heights[heights.size() / 2]
        : (heights[heights.size() / 2 - 1] + heights[heights.size() / 2]) / 2.0;
    result.imageDataUrl = imageDataUrl(latestAnnotated);
    result.success = true;
    return result;
}
