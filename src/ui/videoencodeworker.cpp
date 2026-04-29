#include "videoencodeworker.h"

#include <QBuffer>
#include <QPainter>

namespace {

QSize transportVideoSize()
{
#ifdef Q_OS_LINUX
    return QSize(480, 270);
#else
    return QSize(640, 360);
#endif
}

int transportJpegQuality()
{
#ifdef Q_OS_LINUX
    return 68;
#else
    return 80;
#endif
}

QImage normalizeFrameForTransport(const QImage &frame, const QSize &targetSize)
{
    if (frame.isNull() || !targetSize.isValid()) {
        return {};
    }

    QImage normalized = frame.convertToFormat(QImage::Format_RGB32);
    if (normalized.isNull()) {
        return {};
    }

    QImage canvas(targetSize, QImage::Format_RGB32);
    canvas.fill(Qt::black);

    const QSize scaledSize = normalized.size().scaled(targetSize, Qt::KeepAspectRatio);
    const QImage scaledFrame = normalized.scaled(scaledSize, Qt::KeepAspectRatio, Qt::FastTransformation);
    if (scaledFrame.isNull()) {
        return {};
    }

    QPainter painter(&canvas);
    const QPoint topLeft((targetSize.width() - scaledSize.width()) / 2,
                         (targetSize.height() - scaledSize.height()) / 2);
    painter.drawImage(topLeft, scaledFrame);
    return canvas;
}

bool looksLikeJpeg(const QByteArray &bytes)
{
    return bytes.size() >= 4
        && static_cast<uchar>(bytes.at(0)) == 0xFF
        && static_cast<uchar>(bytes.at(1)) == 0xD8
        && static_cast<uchar>(bytes.at(bytes.size() - 2)) == 0xFF
        && static_cast<uchar>(bytes.at(bytes.size() - 1)) == 0xD9;
}

}

VideoEncodeWorker::VideoEncodeWorker(QObject *parent)
    : QObject(parent)
{
}

void VideoEncodeWorker::encodeFrame(const QString &peerId, const QImage &frame)
{
    if (peerId.isEmpty() || frame.isNull()) {
        emit frameEncoded(peerId, {}, QStringLiteral("JPG"), {});
        return;
    }

    const QImage scaledFrame = normalizeFrameForTransport(frame, transportVideoSize());
    if (scaledFrame.isNull()) {
        emit frameEncoded(peerId, {}, QStringLiteral("JPG"), {});
        return;
    }

    QByteArray imageBytes;
    QBuffer buffer(&imageBytes);
    buffer.open(QIODevice::WriteOnly);
    constexpr auto imageFormat = "JPG";
    if (!scaledFrame.save(&buffer, imageFormat, transportJpegQuality()) || !looksLikeJpeg(imageBytes)) {
        emit frameEncoded(peerId, {}, QStringLiteral("JPG"), {});
        return;
    }

    emit frameEncoded(peerId, imageBytes, QStringLiteral("JPG"), scaledFrame.size());
}
