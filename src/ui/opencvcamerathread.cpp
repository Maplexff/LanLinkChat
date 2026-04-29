#include "opencvcamerathread.h"

#include <QDebug>
#include <QMutexLocker>
#include <QVector>

#ifdef LANLINKCHAT_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#endif

namespace {

#ifdef LANLINKCHAT_HAS_OPENCV
struct CaptureAttempt {
    QString label;
    int fourcc = 0;
    QSize resolution;
    double frameRate = 0.0;
    bool avoidMjpegResult = false;
    bool requestRawFrames = false;
};

QString fourccLabel(double fourccValue)
{
    const int value = static_cast<int>(fourccValue);
    if (value <= 0) {
        return QStringLiteral("unknown");
    }

    QString label;
    label.reserve(4);
    for (int shift = 0; shift < 4; ++shift) {
        const char c = static_cast<char>((value >> (shift * 8)) & 0xFF);
        label.append((c >= 32 && c <= 126) ? QChar::fromLatin1(c) : QChar('?'));
    }
    return label.trimmed().isEmpty() ? QStringLiteral("unknown") : label;
}

int fourccValue(char c1, char c2, char c3, char c4)
{
    return cv::VideoWriter::fourcc(c1, c2, c3, c4);
}

bool isMjpegFourcc(double fourccValue)
{
    const QString label = fourccLabel(fourccValue).toUpper();
    return label == QStringLiteral("MJPG")
        || label == QStringLiteral("JPEG");
}

bool openCaptureDevice(cv::VideoCapture &capture, const QString &devicePath, int deviceIndex)
{
#ifdef Q_OS_LINUX
    constexpr int apiPreference = cv::CAP_V4L2;
#else
    constexpr int apiPreference = cv::CAP_ANY;
#endif

    if (!devicePath.isEmpty() && capture.open(devicePath.toStdString(), apiPreference)) {
        return true;
    }

    if (deviceIndex >= 0 && capture.open(deviceIndex, apiPreference)) {
        return true;
    }

    return false;
}

void applyCaptureAttempt(cv::VideoCapture &capture,
                         const CaptureAttempt &attempt)
{
    capture.set(cv::CAP_PROP_BUFFERSIZE, 1.0);
    capture.set(cv::CAP_PROP_CONVERT_RGB, attempt.requestRawFrames ? 0.0 : 1.0);
    if (attempt.fourcc > 0) {
        capture.set(cv::CAP_PROP_FOURCC, attempt.fourcc);
    }
    if (attempt.resolution.isValid()) {
        capture.set(cv::CAP_PROP_FRAME_WIDTH, attempt.resolution.width());
        capture.set(cv::CAP_PROP_FRAME_HEIGHT, attempt.resolution.height());
    }
    if (attempt.frameRate > 0.0) {
        capture.set(cv::CAP_PROP_FPS, attempt.frameRate);
    }
}

QImage matToImage(const cv::Mat &frame, const QString &fourcc)
{
    if (frame.empty()) {
        return {};
    }

    const QString normalizedFourcc = fourcc.trimmed().toUpper();
    switch (frame.type()) {
    case CV_8UC2: {
        cv::Mat rgbFrame;
        if (normalizedFourcc == QStringLiteral("YUYV")
            || normalizedFourcc == QStringLiteral("YUY2")) {
            cv::cvtColor(frame, rgbFrame, cv::COLOR_YUV2RGB_YUY2);
        } else if (normalizedFourcc == QStringLiteral("UYVY")) {
            cv::cvtColor(frame, rgbFrame, cv::COLOR_YUV2RGB_UYVY);
        } else if (normalizedFourcc == QStringLiteral("YVYU")) {
            cv::cvtColor(frame, rgbFrame, cv::COLOR_YUV2RGB_YVYU);
        } else {
            return {};
        }

        QImage image(rgbFrame.data,
                     rgbFrame.cols,
                     rgbFrame.rows,
                     static_cast<int>(rgbFrame.step),
                     QImage::Format_RGB888);
        return image.copy().convertToFormat(QImage::Format_RGB32);
    }
    case CV_8UC3: {
        cv::Mat rgbFrame;
        cv::cvtColor(frame, rgbFrame, cv::COLOR_BGR2RGB);
        QImage image(rgbFrame.data,
                     rgbFrame.cols,
                     rgbFrame.rows,
                     static_cast<int>(rgbFrame.step),
                     QImage::Format_RGB888);
        return image.copy().convertToFormat(QImage::Format_RGB32);
    }
    case CV_8UC4: {
        QImage image(frame.data,
                     frame.cols,
                     frame.rows,
                     static_cast<int>(frame.step),
                     QImage::Format_ARGB32);
        return image.copy().convertToFormat(QImage::Format_RGB32);
    }
    case CV_8UC1: {
        QImage image(frame.data,
                     frame.cols,
                     frame.rows,
                     static_cast<int>(frame.step),
                     QImage::Format_Grayscale8);
        return image.copy().convertToFormat(QImage::Format_RGB32);
    }
    default:
        break;
    }

    return {};
}

QSize preferredRawResolution(const QSize &requestedResolution)
{
    if (!requestedResolution.isValid()) {
        return QSize(640, 480);
    }

    if (requestedResolution.width() >= 640) {
        return QSize(640, 480);
    }

    if (requestedResolution.width() >= 320) {
        return QSize(320, 240);
    }

    return QSize(160, 120);
}

QVector<CaptureAttempt> buildCaptureAttempts(const QSize &requestedResolution, double requestedFrameRate)
{
    const QSize safeResolution = requestedResolution.isValid() ? requestedResolution : QSize(640, 480);
    const double safeFrameRate = requestedFrameRate > 0.0 ? qMin(requestedFrameRate, 30.0) : 30.0;
    const double conservativeFrameRate = qMin(safeFrameRate, 15.0);
    const QSize rawResolution = preferredRawResolution(safeResolution);

    QVector<CaptureAttempt> attempts;
#ifdef Q_OS_LINUX
    attempts.append({QStringLiteral("YUYV"), fourccValue('Y', 'U', 'Y', 'V'), rawResolution, conservativeFrameRate, true, true});
    attempts.append({QStringLiteral("YUY2"), fourccValue('Y', 'U', 'Y', '2'), rawResolution, conservativeFrameRate, true, true});
    attempts.append({QStringLiteral("UYVY"), fourccValue('U', 'Y', 'V', 'Y'), rawResolution, conservativeFrameRate, true, true});
    if (rawResolution != QSize(320, 240)) {
        attempts.append({QStringLiteral("YUYV 320x240"), fourccValue('Y', 'U', 'Y', 'V'), QSize(320, 240), conservativeFrameRate, true, true});
        attempts.append({QStringLiteral("YUY2 320x240"), fourccValue('Y', 'U', 'Y', '2'), QSize(320, 240), conservativeFrameRate, true, true});
    }
    attempts.append({QStringLiteral("AUTO raw-preferred"), 0, rawResolution, conservativeFrameRate, false, true});
    attempts.append({QStringLiteral("MJPG last-resort"), fourccValue('M', 'J', 'P', 'G'), safeResolution, safeFrameRate, false, false});
#else
    attempts.append({QStringLiteral("AUTO"), 0, safeResolution, safeFrameRate, false, false});
    attempts.append({QStringLiteral("MJPG"), fourccValue('M', 'J', 'P', 'G'), safeResolution, safeFrameRate, false, false});
#endif
    return attempts;
}

bool readFirstUsableFrame(cv::VideoCapture &capture, const QString &fourcc, QImage *image)
{
    for (int index = 0; index < 12; ++index) {
        cv::Mat frame;
        if (!capture.read(frame) || frame.empty()) {
            OpenCvCameraThread::msleep(10);
            continue;
        }

        const QImage converted = matToImage(frame, fourcc);
        if (!converted.isNull()) {
            if (image) {
                *image = converted;
            }
            return true;
        }
    }

    return false;
}
#endif

}

OpenCvCameraThread::OpenCvCameraThread(QObject *parent)
    : QThread(parent)
{
}

OpenCvCameraThread::~OpenCvCameraThread()
{
    stopCapture();
    wait();
}

void OpenCvCameraThread::configure(const QString &devicePath,
                                   int deviceIndex,
                                   const QString &deviceDescription,
                                   const QSize &resolution,
                                   double targetFrameRate)
{
    QMutexLocker locker(&m_mutex);
    m_devicePath = devicePath;
    m_deviceIndex = deviceIndex;
    m_deviceDescription = deviceDescription;
    m_resolution = resolution;
    m_targetFrameRate = targetFrameRate;
    m_stopRequested = false;
}

void OpenCvCameraThread::stopCapture()
{
    m_stopRequested = true;
}

void OpenCvCameraThread::run()
{
#ifndef LANLINKCHAT_HAS_OPENCV
    emit cameraError(QStringLiteral("当前构建未启用 OpenCV 采集，请先安装 OpenCV，并让构建系统找到其 include/lib 后重新编译。"));
    emit cameraActiveChanged(false);
    return;
#else
    QString devicePath;
    QString deviceDescription;
    QSize resolution;
    double targetFrameRate = 30.0;
    int deviceIndex = -1;
    {
        QMutexLocker locker(&m_mutex);
        devicePath = m_devicePath;
        deviceDescription = m_deviceDescription;
        resolution = m_resolution;
        targetFrameRate = m_targetFrameRate;
        deviceIndex = m_deviceIndex;
    }

    cv::VideoCapture capture;
    QString selectedAttemptLabel;
    QImage firstFrame;
    for (const CaptureAttempt &attempt : buildCaptureAttempts(resolution, targetFrameRate)) {
        if (m_stopRequested.load()) {
            emit cameraActiveChanged(false);
            return;
        }

        cv::VideoCapture candidate;
        if (!openCaptureDevice(candidate, devicePath, deviceIndex)) {
            continue;
        }

        applyCaptureAttempt(candidate, attempt);
        const double actualFourcc = candidate.get(cv::CAP_PROP_FOURCC);
        const QString actualFourccLabel = fourccLabel(actualFourcc);
        qInfo().nospace()
            << "Trying OpenCV camera capture: requested=" << attempt.label
            << ", actualFourcc=" << actualFourccLabel
            << ", size=" << static_cast<int>(candidate.get(cv::CAP_PROP_FRAME_WIDTH))
            << "x" << static_cast<int>(candidate.get(cv::CAP_PROP_FRAME_HEIGHT))
            << ", fps=" << candidate.get(cv::CAP_PROP_FPS)
            << ", convertRgb=" << candidate.get(cv::CAP_PROP_CONVERT_RGB);

        if (attempt.avoidMjpegResult && isMjpegFourcc(actualFourcc)) {
            qWarning() << "Skipping camera attempt because driver kept MJPEG despite raw request:" << attempt.label;
            candidate.release();
            continue;
        }

        QImage candidateFirstFrame;
        if (!readFirstUsableFrame(candidate, actualFourccLabel, &candidateFirstFrame)) {
            qWarning() << "Skipping camera attempt because no usable frame was captured:" << attempt.label;
            candidate.release();
            continue;
        }

        capture = std::move(candidate);
        selectedAttemptLabel = attempt.label;
        firstFrame = candidateFirstFrame;
        break;
    }

    if (!capture.isOpened()) {
        emit cameraError(QStringLiteral("OpenCV 无法以稳定格式打开摄像头：%1。检测到 MJPEG 解码错误时，请优先选择 YUYV/YUY2 或降低帧率。")
                             .arg(deviceDescription.isEmpty() ? devicePath : deviceDescription));
        emit cameraActiveChanged(false);
        return;
    }

    const QSize actualResolution(static_cast<int>(capture.get(cv::CAP_PROP_FRAME_WIDTH)),
                                 static_cast<int>(capture.get(cv::CAP_PROP_FRAME_HEIGHT)));
    const double actualFrameRate = capture.get(cv::CAP_PROP_FPS);
    const QString backendName = QStringLiteral("OpenCV/V4L2 %1 %2")
                                    .arg(fourccLabel(capture.get(cv::CAP_PROP_FOURCC)), selectedAttemptLabel);

    emit captureFormatResolved(actualResolution, actualFrameRate, backendName);
    emit cameraActiveChanged(true);
    if (!firstFrame.isNull()) {
        emit frameCaptured(firstFrame);
    }

    int consecutiveFailures = 0;
    while (!m_stopRequested.load()) {
        cv::Mat frame;
        if (!capture.read(frame) || frame.empty()) {
            ++consecutiveFailures;
            if (consecutiveFailures >= 12) {
                emit cameraError(QStringLiteral("OpenCV 采集摄像头画面失败：%1").arg(deviceDescription.isEmpty() ? devicePath : deviceDescription));
                break;
            }
            msleep(10);
            continue;
        }

        consecutiveFailures = 0;
        const QString actualFourccLabel = fourccLabel(capture.get(cv::CAP_PROP_FOURCC));
        const QImage image = matToImage(frame, actualFourccLabel);
        if (!image.isNull()) {
            emit frameCaptured(image);
        }
    }

    capture.release();
    emit cameraActiveChanged(false);
#endif
}
