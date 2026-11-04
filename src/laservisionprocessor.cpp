#include "laservisionprocessor.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QBuffer>
#include <QImage>
#include <QMap>
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
// 视觉测量只使用画面中央区域。左右各裁掉 10%，去除边缘墙面、支架等
// 与测量无关的内容；不缩放 ROI，避免插值改变像素距离和轮廓形状。
constexpr double kHorizontalCropRatio = 0.10;
// 两套镜头画面中的红色参考圆分别代表下面两段已知实际长度。
constexpr double kShortReferenceLengthMm = 570.0;
constexpr double kLongReferenceLengthMm = 1060.0;
// 一次测量收集 4 个稳定单帧结果，最多查看 120 帧，最后取中位数。
constexpr int kRequiredSamples = 4; // old PICTURECAL_TIMES=3 means first + 3 updates
constexpr int kMaximumFrames = 300;

// processFrame() 对一张图的处理结果；Result 则是多帧融合后的任务结果。
struct FrameResult {
    bool success = false;
    double height = 0.0;
    QString error;
    QStringList diagnostics;
    cv::Mat annotated;
};

cv::Mat measurementRoi(const cv::Mat &frame)
{
    if (frame.empty())
        return cv::Mat();

    const int crop = static_cast<int>(std::lround(frame.cols
                                                   * kHorizontalCropRatio));
    const int roiWidth = frame.cols - crop * 2;
    if (crop <= 0 || roiWidth <= 0)
        return frame.clone();

    // clone() 让后台处理持有独立且连续的图像数据，避免依赖原始帧生命周期。
    return frame(cv::Rect(crop, 0, roiWidth, frame.rows)).clone();
}

QString imageDataUrl(const cv::Mat &bgr)
{
    // 处理结果最终要交给 QML 的 Image 显示。这里把 OpenCV 图像转换成
    // “data:image/png;base64,...”字符串，QML 不需要额外的临时图片文件。
    if (bgr.empty())
        return QString();

    // OpenCV 默认通道顺序是 BGR，QImage::Format_RGB888 要求 RGB。
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    QImage image(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                 QImage::Format_RGB888);

    // image 暂时借用 rgb 的内存，所以先 copy() 取得独立数据，再缩放并编码 PNG。
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
    // 最新 wifiTcp：红色跨越 HSV 色相两端，合并两段掩码识别小参考圆。
    Q_UNUSED(shortFocus);
    cv::Mat hsv, mask1, mask2, mask;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(0, 70, 60), cv::Scalar(10, 255, 255), mask1);
    cv::inRange(hsv, cv::Scalar(160, 70, 60), cv::Scalar(179, 255, 255), mask2);
    mask = mask1 | mask2;

    // 第2步：开运算去掉零散红色噪点，闭运算填补参考圆内部的小缺口。
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(3, 3)));
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(5, 5)));

    // 第3步：提取所有彼此独立的外轮廓，每个轮廓都是一个候选红色物体。
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL,
                     cv::CHAIN_APPROX_SIMPLE);
    struct Candidate { cv::Point2f center; double area = 0.0; };
    std::vector<Candidate> candidates;
    for (const auto &contour : contours) {
        // 第4步：先用面积和最小包围圆半径排除过小噪点、过大背景块。
        const double area = cv::contourArea(contour);
        if (area < 15.0 || area > 800.0)
            continue;
        cv::Point2f center;
        float radius = 0.0f;
        cv::minEnclosingCircle(contour, center, radius);
        if (radius < 2.5f || radius > 25.0f)
            continue;
        // 轮廓面积 / 包围圆面积越接近 1，形状越像实心圆。
        const double ratio = area / (CV_PI * radius * radius);
        if (ratio < 0.45 || ratio > 1.15)
            continue;
        candidates.push_back({center, area});
    }
    if (candidates.size() < 2) {
        if (error)
            *error = QStringLiteral("未识别到两个红色参考圆（检测到 %1 个）")
                         .arg(candidates.size());
        return false;
    }
    // 第5步：按面积从大到小排序，选最可信的两个圆作为比例尺端点。
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  return a.area > b.area;
              });
    cv::Point2f a = candidates[0].center;
    cv::Point2f b = candidates[1].center;
    // 第6步：检查两圆像素间距，防止把相邻噪点或相距过远的物体误认成标尺。
    const double distance = cv::norm(a - b);
    if (distance < 20.0 || distance > 800.0) {
        if (error)
            *error = QStringLiteral("红色参考圆间距异常: %1 px")
                         .arg(distance, 0, 'f', 1);
        return false;
    }
    // 固定输出顺序为左圆、右圆，便于后面的方向和绘图保持一致。
    if (a.x > b.x)
        std::swap(a, b);
    *first = a;
    *second = b;
    return true;
}

bool likelyLaserColor(const cv::Vec3b &pixel)
{
    // 激光中心可能过曝成白色，外围仍表现为绿色。因此这里接受两类像素：
    // 1. 三通道都接近 255 的白色高亮核心；2. 绿色通道为最大值的亮像素。
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
    // 第1步：转灰度后做 Top-hat（原图减去开运算结果），突出尺寸较小、
    // 亮度明显高于周围背景的激光光斑。
    cv::Mat gray;
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    const int kernelSize = shortFocus ? 8 : 12;
    cv::Mat topHat;
    cv::morphologyEx(gray, topHat, cv::MORPH_TOPHAT,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(kernelSize, kernelSize)));
    // 第2步：把增强后的亮斑二值化，再用闭运算连接光斑内部的断裂区域。
    cv::Mat mask;
    cv::threshold(topHat, mask, shortFocus ? 8 : 12, 255,
                  cv::THRESH_BINARY);
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE,
                                               cv::Size(4, 4)));

    // 第3步：把二值图分成多个连通区域，并取得每块的面积、外接矩形和质心。
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(
        mask, labels, stats, centroids, 4, CV_32S);
    std::vector<int> candidates;
    for (int i = 1; i < count; ++i) {
        // 第4步：面积限制去掉微小噪点和大面积灯光；长宽比限制去掉细长反光线。
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        const int left = stats.at<int>(i, cv::CC_STAT_LEFT);
        const int top = stats.at<int>(i, cv::CC_STAT_TOP);
        const int width = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (area < 4 || area > 1200 || width <= 0 || height <= 0)
            continue;
        const double aspect = static_cast<double>(std::max(width, height))
            / std::min(width, height);
        if (aspect > 2.2)
            continue;

        // 第5步：逐像素检查候选区域，至少 25% 必须符合激光颜色特征。
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
        if (total == 0 || static_cast<double>(validColors) / total < 0.25)
            continue;

        if (!shortFocus) {
            // 第6步（仅远焦）：检查光斑外围 3～8 px 的圆环中是否存在足够多
            // 的绿色像素。远焦下用这一特征排除白灯、金属高光等过曝亮点。
            const cv::Point2f centroid(
                static_cast<float>(centroids.at<double>(i, 0)),
                static_cast<float>(centroids.at<double>(i, 1)));
            int greenHalo = 0;
            int haloSamples = 0;
            for (int dy = -8; dy <= 8; ++dy) {
                for (int dx = -8; dx <= 8; ++dx) {
                    const double d = std::sqrt(double(dx * dx + dy * dy));
                    if (d <= 3.0 || d > 8.0)
                        continue;
                    const int x = static_cast<int>(centroid.x) + dx;
                    const int y = static_cast<int>(centroid.y) + dy;
                    if (x < 0 || x >= image.cols || y < 0 || y >= image.rows)
                        continue;
                    ++haloSamples;
                    const cv::Vec3b p = image.at<cv::Vec3b>(y, x);
                    const int maximum = std::max({int(p[0]), int(p[1]), int(p[2])});
                    if (p[1] > p[2] + 15 && p[1] > p[0] + 15
                        && maximum > 70 && maximum < 250) {
                        ++greenHalo;
                    }
                }
            }
            if (haloSamples == 0 || double(greenHalo) / haloSamples < 0.04)
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
    // 第7步：多个候选都合格时取面积最大的一个，并把它的质心作为激光点。
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

bool findBlueReferenceDistance(const cv::Mat &image, const cv::Point2f &spot,
                               const cv::Point2f &referenceA,
                               const cv::Point2f &referenceB,
                               bool shortFocus, double *distance,
                               cv::Vec4f *fittedLine, cv::Point2f *intersection,
                               QString *error)
{
    // 第1步：参考线已由较淡的绿色光线更换为蓝色胶带。先把 BGR 转成
    // HSV，再按蓝色色相、饱和度和亮度生成二值图；绿色激光点仍由
    // findLaserSpot() 独立检测，不会与蓝色胶带共用阈值。
    cv::Mat hsv;
    cv::cvtColor(image, hsv, cv::COLOR_BGR2HSV);
    cv::Mat blueTapeMask;
    cv::inRange(hsv, cv::Scalar(90, 60, 40), cv::Scalar(130, 255, 255),
                blueTapeMask);
    // 与最新 wifiTcp 一致，水平闭运算连接蓝胶带断点。
    cv::morphologyEx(blueTapeMask, blueTapeMask, cv::MORPH_CLOSE,
                     cv::getStructuringElement(cv::MORPH_RECT, cv::Size(15, 1)));
    // 第3步：胶带应位于激光点附近。只保留激光点左右一定宽度的区域，
    // 防止画面中其他蓝色物体被当成参考胶带。
    const int halfWidth = shortFocus ? 100 : 150;
    const int left = std::max(0, static_cast<int>(spot.x) - halfWidth);
    const int right = std::min(image.cols - 1,
                               static_cast<int>(spot.x) + halfWidth);
    cv::Mat roi = cv::Mat::zeros(blueTapeMask.size(), CV_8UC1);
    roi(cv::Rect(left, 0, right - left + 1, image.rows)).setTo(255);
    blueTapeMask.setTo(0, roi == 0);

    // 第4步：允许胶带位于光点上下两侧，按面积和细长度选择最佳区域。
    cv::Mat labels, stats, centroids;
    const int count = cv::connectedComponentsWithStats(
        blueTapeMask, labels, stats, centroids, 8, CV_32S);
    int best = -1;
    double bestScore = 0.0;
    for (int i = 1; i < count; ++i) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        const int width = stats.at<int>(i, cv::CC_STAT_WIDTH);
        const int height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        if (area < 30 || width <= 0 || height <= 0) {
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
            *error = QStringLiteral("未识别到蓝色参考胶带");
        return false;
    }
    // 第5步：距离基准是蓝色胶带的“上边缘”，不能用整块胶带的所有
    // 像素拟合，否则得到的是胶带中线。逐列寻找该连通区域最上方的
    // 一个像素，组成上边缘点集，再用最小二乘法拟合成无限长直线。
    std::vector<cv::Point> upperEdgePoints;
    const int componentLeft = stats.at<int>(best, cv::CC_STAT_LEFT);
    const int componentTop = stats.at<int>(best, cv::CC_STAT_TOP);
    const int componentWidth = stats.at<int>(best, cv::CC_STAT_WIDTH);
    const int componentHeight = stats.at<int>(best, cv::CC_STAT_HEIGHT);
    for (int x = componentLeft; x < componentLeft + componentWidth; ++x) {
        for (int y = componentTop; y < componentTop + componentHeight; ++y) {
            if (labels.at<int>(y, x) == best) {
                upperEdgePoints.emplace_back(x, y);
                break;
            }
        }
    }
    if (upperEdgePoints.size() < 5) {
        if (error)
            *error = QStringLiteral("蓝色参考胶带上边缘点不足，无法拟合");
        return false;
    }
    // fittedLine 的四个值依次是方向 (vx, vy) 和线上一点 (x0, y0)。
    cv::fitLine(upperEdgePoints, *fittedLine, cv::DIST_L2, 0, 0.01, 0.01);

    // 第6步：红色参考圆 A→B 决定“高度测量方向”。从激光点沿该方向
    // 作直线，与蓝色胶带上边缘拟合线求交；两点间像素距离就是待换算的高度。
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
            *error = QStringLiteral("参考圆连线与蓝色胶带平行，无法计算高度");
        return false;
    }
    const double t = -(a * spot.x + b * spot.y + c) / denominator;
    *intersection = spot + direction * static_cast<float>(t);
    *distance = cv::norm(spot - *intersection);
    // 在光点同一列求胶带高度：光点在胶带下方时，距离取负。
    if (std::abs(vx) > 1e-6) {
        const double lineY = y0 + (spot.x - x0) * vy / vx;
        if (spot.y > lineY + 1.5)
            *distance = -*distance;
    }
    return std::isfinite(*distance);
}

FrameResult processFrame(const cv::Mat &frame, bool shortFocus)
{
    // 单帧完整处理顺序：红色参考圆定比例 → 绿色激光点定位 →
    // 蓝色胶带拟合 → 求交点和像素距离 → 换算毫米 → 生成标注图。
    FrameResult result;
    if (frame.empty()) {
        result.error = QStringLiteral("摄像头返回空图像");
        return result;
    }
    result.diagnostics.append(QStringLiteral("图像读取成功：%1×%2")
                                  .arg(frame.cols).arg(frame.rows));
    // 第1步：找到两个红色参考圆，得到已知长度在图像中的两个端点。
    cv::Point2f referenceA, referenceB;
    if (!findRedReference(frame, shortFocus, &referenceA, &referenceB,
                          &result.error)) {
        return result;
    }
    // 第2步：用“两圆像素距离 / 实际参考长度”求出当前画面的 px/mm。
    // 近焦和远焦装置的实际参考长度不同，因此使用不同常量。
    const double referencePixels = cv::norm(referenceB - referenceA);
    result.diagnostics.append(QStringLiteral(
        "红色参考圆识别成功：A=(%1,%2)，B=(%3,%4)，间距=%5 px")
                                  .arg(referenceA.x, 0, 'f', 1)
                                  .arg(referenceA.y, 0, 'f', 1)
                                  .arg(referenceB.x, 0, 'f', 1)
                                  .arg(referenceB.y, 0, 'f', 1)
                                  .arg(referencePixels, 0, 'f', 2));
    const double pixelPerMm = referencePixels
        / (shortFocus ? kShortReferenceLengthMm : kLongReferenceLengthMm);
    if (!std::isfinite(pixelPerMm) || pixelPerMm <= 0.0) {
        result.error = QStringLiteral("红色参考物比例无效");
        return result;
    }
    result.diagnostics.append(QStringLiteral(
        "像素比例计算成功：参考长度=%1 mm，比例=%2 px/mm")
                                  .arg(shortFocus ? kShortReferenceLengthMm
                                                  : kLongReferenceLengthMm,
                                       0, 'f', 1)
                                  .arg(pixelPerMm, 0, 'f', 6));

    // 第3步：定位绿色激光光斑中心。spotMask 供调试保留，正常计算使用 spot。
    cv::Point2f spot;
    cv::Mat spotMask;
    if (!findLaserSpot(frame, shortFocus, &spot, &spotMask, &result.error))
        return result;
    result.diagnostics.append(QStringLiteral("绿色激光点识别成功：中心=(%1,%2)")
                                  .arg(spot.x, 0, 'f', 1)
                                  .arg(spot.y, 0, 'f', 1));
    // 第4步：拟合蓝色参考胶带，并沿红色参考圆确定的方向求激光点到
    // 胶带交点的距离。lineDistance 的单位仍然是像素。
    double lineDistance = 0.0;
    cv::Vec4f referenceLine;
    cv::Point2f foot;
    if (!findBlueReferenceDistance(frame, spot, referenceA, referenceB,
                                   shortFocus, &lineDistance, &referenceLine,
                                   &foot, &result.error)) {
        return result;
    }
    result.diagnostics.append(QStringLiteral(
        "蓝色参考胶带上边缘拟合成功：交点=(%1,%2)，激光点到胶带上边缘距离=%3 px")
                                  .arg(foot.x, 0, 'f', 1)
                                  .arg(foot.y, 0, 'f', 1)
                                  .arg(lineDistance, 0, 'f', 2));
    // 第5步：像素距离除以 px/mm，得到设备标定需要的实际高度（毫米）。
    result.height = lineDistance / pixelPerMm;
    // 型号相关补偿由控制器在得到毫米高度后执行。
    if (!std::isfinite(result.height) || std::abs(result.height) > 5000.0) {
        result.error = QStringLiteral("视觉计算高度超出有效范围: %1 mm")
                           .arg(result.height, 0, 'f', 3);
        return result;
    }
    result.diagnostics.append(QStringLiteral(
        "视觉高度计算成功：有符号距离=%1 px，比例=%2 px/mm，原始高度=%3 mm")
                                  .arg(lineDistance, 0, 'f', 2)
                                  .arg(pixelPerMm, 0, 'f', 6)
                                  .arg(result.height, 0, 'f', 3));

    // 第6步：在原图副本上画出参考圆、参考圆连线、激光点、交点、
    // 测量线、蓝色胶带拟合线和最终高度，供界面显示及现场排查。
    result.annotated = frame.clone();
    cv::circle(result.annotated, referenceA, 6, cv::Scalar(255, 0, 0), -1);
    cv::circle(result.annotated, referenceB, 6, cv::Scalar(255, 0, 0), -1);
    cv::line(result.annotated, referenceA, referenceB,
             cv::Scalar(0, 0, 255), 2);
    cv::circle(result.annotated, spot, 8, cv::Scalar(0, 255, 255), 2);
    cv::circle(result.annotated, foot, 8, cv::Scalar(255, 255, 0), 2);
    cv::line(result.annotated, spot, foot, cv::Scalar(255, 0, 255), 2);
    const cv::Point2f lineA(referenceLine[2] - 1500 * referenceLine[0],
                            referenceLine[3] - 1500 * referenceLine[1]);
    const cv::Point2f lineB(referenceLine[2] + 1500 * referenceLine[0],
                            referenceLine[3] + 1500 * referenceLine[1]);
    cv::line(result.annotated, lineA, lineB, cv::Scalar(255, 0, 0), 2);
    cv::putText(result.annotated,
                std::string("height=") + std::to_string(result.height) + " mm",
                cv::Point(30, 50), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                cv::Scalar(0, 255, 255), 2);
    // 到这里说明本帧的所有识别与换算步骤均成功。
    result.success = true;
    return result;
}

std::vector<int> availableCameraIndexes()
{
    // Windows 摄像头索引不是设备数量。依次试开 0～4，并且必须实际读到
    // 一帧图像才算“可用”，仅 isOpened() 成功还不能证明摄像头能出图。
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
    // m_watcher 监视后台 runMeasurement()。后台函数返回 Result 后，Qt 自动
    // 发出 finished；下面的 Lambda 回到本对象线程整理结果并发出业务信号。
    connect(&m_watcher, &QFutureWatcher<Result>::finished, this, [this] {
        const Result result = m_watcher.result();

        // 保存本次扫描得到的近焦/远焦 USB 索引，下次测量可直接复用。
        m_shortCameraIndex = result.shortCameraIndex;
        m_longCameraIndex = result.longCameraIndex;
        emit cameraMappingChanged(
            m_shortCameraIndex, m_longCameraIndex,
            m_longCameraIndex >= 0
                ? QStringLiteral("单远焦摄像头 USB %1，模式：%2")
                      .arg(m_longCameraIndex)
                      .arg(m_singleCameraDebug ? QStringLiteral("模拟数值") : QStringLiteral("真实测量"))
                : QStringLiteral("未找到可用测量摄像头"));
        // cancelPendingResult() 会增加 generation。若编号不一致，说明这是一份
        // 已取消任务的迟到结果，只保留摄像头映射，不再交给标定控制器。
        if (m_runningGeneration != m_generation)
            return;

        // 逐条转发算法过程日志，最后把成功状态、高度、错误、图片和摄像头
        // 索引通过 measurementFinished 交给 LaserCalibrationController。
        for (const QString &message : result.diagnostics)
            emit diagnosticMessage(message);
        emit measurementFinished(result.success, result.height, result.error,
                                 result.imageDataUrl, result.cameraIndex);
    });
}

LaserVisionProcessor::~LaserVisionProcessor()
{
    // 让尚未返回的结果失效；对象销毁前等待后台任务结束，避免线程继续访问资源。
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
    // 测量进行中不能切换模式。模式改变后清空旧映射，下次重新扫描摄像头。
    if (busy() || m_singleCameraDebug == enabled)
        return;
    m_singleCameraDebug = enabled;
    m_shortCameraIndex = -1;
    m_longCameraIndex = -1;
}

bool LaserVisionProcessor::requestMeasurement(bool shortFocus)
{
    // 同一时间只允许一个视觉任务，避免两个线程争用同一 USB 摄像头。
    if (busy() || shortFocus)
        return false;

    // QtConcurrent::run 在线程池执行耗时的打开摄像头、读帧和 OpenCV 算法，
    // 因此界面线程不会卡住；本函数提交任务后立即返回 true。
    m_runningGeneration = ++m_generation;
    m_watcher.setFuture(QtConcurrent::run(
        &LaserVisionProcessor::runMeasurement, shortFocus,//开启
        m_shortCameraIndex, m_longCameraIndex, m_singleCameraDebug));
    return true;//后台线程结束后m_watcher 会自动发出：QFutureWatcher::finished
}

void LaserVisionProcessor::cancelPendingResult()
{
    // 不能强行中断 OpenCV 正在读帧的线程，只让其最终结果不再进入标定流程。
    ++m_generation;
}

LaserVisionProcessor::Result LaserVisionProcessor::runMeasurement(
    bool shortFocus, int shortCameraIndex, int longCameraIndex,
    bool singleCameraDebug)
{
    // 本函数在线程池中运行。shortFocus=true 代表近焦任务，否则是远焦任务。
    // 所有结果先写入局部 Result，函数返回后由 m_watcher 转回界面线程。
    Result result;
    const QString focusName = shortFocus ? QStringLiteral("近焦")
                                         : QStringLiteral("远焦");
    result.diagnostics.append(QStringLiteral(
        "%1视觉任务开始：模式=%2，目标采样=%3 帧，最多检查=%4 帧")
                                  .arg(focusName,
                                       singleCameraDebug
                                           ? QStringLiteral("模拟单摄像头")
                                           : QStringLiteral("正式单摄像头"))
                                  .arg(kRequiredSamples)
                                  .arg(kMaximumFrames));
    if (longCameraIndex < 0) {
        // 第1步：没有缓存映射时扫描摄像头索引 0～4。
        const std::vector<int> cameras = availableCameraIndexes();
        QStringList cameraTexts;
        for (int camera : cameras)
            cameraTexts.append(QString::number(camera));
        result.diagnostics.append(QStringLiteral("摄像头扫描完成：可用索引=[%1]")
                                      .arg(cameraTexts.join(',')));
        const std::size_t requiredCount = 1u;
        if (cameras.size() < requiredCount) {
            result.error = QStringLiteral(
                "摄像头数量不足：%1模式需要 %2 只，当前检测到 %3 只")
                               .arg(singleCameraDebug
                                        ? QStringLiteral("单摄像头调试")
                                        : QStringLiteral("正式单摄像头"))
                               .arg(requiredCount)
                               .arg(cameras.size());
            return result;
        }
        // 单远焦方案：仅有一只时直接使用；多只时取最后一个可用索引。
        // 与此前外置摄像头枚举顺序保持一致，不再要求近焦摄像头。
        longCameraIndex = cameras.back();
    }
    shortCameraIndex = -1;
    result.shortCameraIndex = shortCameraIndex;
    result.longCameraIndex = longCameraIndex;
    result.cameraIndex = longCameraIndex;
    result.diagnostics.append(QStringLiteral(
        "摄像头选择完成：近焦已停用(%1)，远焦=%2，本次使用=%3")
                                  .arg(shortCameraIndex)
                                  .arg(longCameraIndex)
                                  .arg(result.cameraIndex));

    // 第3步：通过 Windows DirectShow 打开目标摄像头，并请求 1920×1080、30 fps。
    // set() 是请求参数，实际能否完全满足仍取决于摄像头驱动。
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
    result.diagnostics.append(QStringLiteral(
        "摄像头 %1 打开成功，请求参数=%2×%3@30fps")
                                  .arg(result.cameraIndex)
                                  .arg(kFrameWidth).arg(kFrameHeight));
    // 第4步：丢弃刚打开后的前 8 帧，让自动曝光、白平衡和缓存趋于稳定。
    for (int i = 0; i < 8; ++i) {
        cv::Mat warmup;
        camera.read(warmup);
        QThread::msleep(35);
    }

    // 模拟模式仍真正打开 USB 摄像头并读取画面，用于检查硬件和上层流程；
    // 但不执行参考圆、激光点、蓝色胶带算法，而是返回预设高度。
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

        cv::Mat annotated = measurementRoi(frame);
        cv::putText(annotated, "SIMULATED MEASUREMENT",
                    cv::Point(30, 55), cv::FONT_HERSHEY_SIMPLEX, 1.1,
                    cv::Scalar(0, 215, 255), 2, cv::LINE_AA);
        result.height = shortFocus ? 615.0 : 625.0;
        result.imageDataUrl = imageDataUrl(annotated);
        result.diagnostics.append(QStringLiteral(
            "模拟测量成功：读取真实摄像头画面，使用默认高度=%1 mm（不执行参考圆/激光点算法）")
                                      .arg(result.height, 0, 'f', 3));
        result.success = true;
        return result;
    }
    // 第5步（正式模式）：连续读取多帧。每一帧都进入 processFrame()，完成
    // 参考圆、激光点、蓝色胶带、毫米换算和标注图生成。
    std::vector<double> heights;
    cv::Mat latestAnnotated;
    QString latestError = QStringLiteral("尚未获得有效图像");
    QStringList latestSuccessDiagnostics;
    QStringList latestAttemptDiagnostics;
    QMap<QString, int> failureCounts;
    int framesRead = 0;
    int rejectedByStability = 0;
    int resetByJump = 0;
    for (int frameIndex = 0;
         frameIndex < kMaximumFrames
         && static_cast<int>(heights.size()) < kRequiredSamples;
         ++frameIndex) {
        cv::Mat frame;
        if (!camera.read(frame) || frame.empty()) {
            latestError = QStringLiteral("摄像头读帧失败");
            ++failureCounts[latestError];
            QThread::msleep(30);
            continue;
        }
        ++framesRead;
        // 使用未经放大的中央原始像素 ROI 计算；裁剪只排除左右干扰区，
        // 不会改变 px/mm 比例，也不会制造额外的“虚拟像素”。
        const cv::Mat roi = measurementRoi(frame);
        const FrameResult measurement = processFrame(roi, shortFocus);//单帧处理
        latestAttemptDiagnostics = measurement.diagnostics;
        if (!measurement.success) {
            latestError = measurement.error;
            ++failureCounts[latestError];
            continue;
        }
        latestAnnotated = measurement.annotated;
        latestSuccessDiagnostics = measurement.diagnostics;
        // 第6步：进行跨帧稳定性检查。新值与已有样本中位数比较：
        // 差值 >18 mm 认为画面/目标发生突跳，清空旧样本重新积累；
        // 差值在 5～18 mm 之间认为本帧不稳定，丢弃但保留已有样本。
        if (!heights.empty()) {
            std::vector<double> sorted = heights;
            std::sort(sorted.begin(), sorted.end());
            const double median = sorted[sorted.size() / 2];
            const double difference = std::abs(measurement.height - median);
            if (difference > 18.0) {
                heights.clear();
                ++resetByJump;
            } else if (difference > 5.0) {
                ++rejectedByStability;
                continue;
            }
        }
        heights.push_back(measurement.height);
    }

    // 采集结束立即释放摄像头，避免后续任务或其他程序无法再次打开。
    camera.release();

    // 汇总各类失败原因，便于日志判断卡在参考圆、激光点还是蓝色胶带。
    QStringList failureSummary;
    for (auto it = failureCounts.cbegin(); it != failureCounts.cend(); ++it)
        failureSummary.append(QStringLiteral("%1=%2").arg(it.key()).arg(it.value()));
    result.diagnostics.append(QStringLiteral(
        "图像采样结束：成功读取=%1，有效样本=%2/%3，稳定性拒绝=%4，突跳重置=%5")
                                  .arg(framesRead).arg(heights.size())
                                  .arg(kRequiredSamples)
                                  .arg(rejectedByStability).arg(resetByJump));
    if (!failureSummary.isEmpty())
        result.diagnostics.append(QStringLiteral("无效帧统计：%1")
                                      .arg(failureSummary.join(QStringLiteral("；"))));
    if (heights.size() < kRequiredSamples) {
        // 未收齐 4 个稳定样本时任务失败；如果已有标注图，仍返回给界面排查。
        if (!latestAttemptDiagnostics.isEmpty()) {
            result.diagnostics.append(QStringLiteral("最后一次算法尝试已完成的步骤："));
            result.diagnostics.append(latestAttemptDiagnostics);
        }
        result.error = QStringLiteral("视觉测量失败：%1（有效帧 %2/%3）")
                           .arg(latestError)
                           .arg(heights.size()).arg(kRequiredSamples);
        if (!latestAnnotated.empty())
            result.imageDataUrl = imageDataUrl(latestAnnotated);
        return result;
    }
    // 第7步：收齐样本后排序并取中位数，降低单帧抖动和离群值的影响。
    std::sort(heights.begin(), heights.end());
    result.height = heights.size() % 2
        ? heights[heights.size() / 2]
        : (heights[heights.size() / 2 - 1] + heights[heights.size() / 2]) / 2.0;
    // 使用最后一个成功帧的标注图作为界面预览。
    result.imageDataUrl = imageDataUrl(latestAnnotated);
    result.diagnostics.append(latestSuccessDiagnostics);
    QStringList sampleTexts;
    for (double sample : heights)
        sampleTexts.append(QString::number(sample, 'f', 3));
    result.diagnostics.append(QStringLiteral(
        "多帧融合成功：排序样本=[%1] mm，中位数=%2 mm")
                                  .arg(sampleTexts.join(QStringLiteral(", ")))
                                  .arg(result.height, 0, 'f', 3));
    // 返回 Result 后 QFutureWatcher 自动发出 finished，构造函数中的 Lambda
    // 再转发 diagnosticMessage 和 measurementFinished 信号。
    result.success = true;
    return result;
}
