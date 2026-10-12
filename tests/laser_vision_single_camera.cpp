// Standalone regression: link with moc_laservisionprocessor.o and Qt/OpenCV.
// Includes implementation to exercise the actual image algorithms without hardware.
#include "../src/laservisionprocessor.cpp"
#include <iostream>

int main()
{
    for (int y : {300, 600}) {
        cv::Mat frame(800, 800, CV_8UC3, cv::Scalar(100, 100, 100));
        cv::circle(frame, cv::Point(500, 100), 6, cv::Scalar(0, 0, 220), -1);
        cv::circle(frame, cv::Point(500, 700), 6, cv::Scalar(0, 0, 220), -1);
        cv::rectangle(frame, cv::Rect(30, 500, 700, 20), cv::Scalar(200, 30, 20), -1);
        cv::circle(frame, cv::Point(220, y), 6, cv::Scalar(100, 230, 100), -1);
        cv::circle(frame, cv::Point(220, y), 3, cv::Scalar(255, 255, 255), -1);
        const FrameResult result = processFrame(frame, false);
        const double expected = (500.0 - y) * 1060.0 / 600.0 + (y < 500 ? -10.0 : 10.0);
        if (!result.success || std::abs(result.height - expected) > 1.0) {
            std::cerr << "FAIL y=" << y << " height=" << result.height
                      << " error=" << result.error.toStdString() << std::endl;
            return 1;
        }
        std::cout << "PASS signed image measurement y=" << y << " height=" << result.height << std::endl;
    }
    // Zero distance is a valid geometry result, not an absent measurement.
    cv::Mat tape(800, 800, CV_8UC3, cv::Scalar(100, 100, 100));
    cv::rectangle(tape, cv::Rect(30, 500, 700, 20), cv::Scalar(200, 30, 20), -1);
    double distance = 999;
    cv::Vec4f line;
    cv::Point2f intersection;
    QString error;
    if (!findBlueReferenceDistance(tape, {220, 500}, {500, 100}, {500, 700},
                                   false, &distance, &line, &intersection, &error)
        || std::abs(distance) > 0.01)
        return 2;
    std::cout << "PASS zero distance and tape upper edge" << std::endl;
    return 0;
}
