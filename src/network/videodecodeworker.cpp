#include "videodecodeworker.h"

VideoDecodeWorker::VideoDecodeWorker(QObject *parent)
    : QObject(parent)
{
}

void VideoDecodeWorker::decodeFrame(const QString &peerId,
                                    const QByteArray &encodedFrame,
                                    const QString &imageFormat,
                                    qint64 announcedFrameNumber)
{
    QImage frame;
    if (!peerId.isEmpty() && !encodedFrame.isEmpty()) {
        frame.loadFromData(encodedFrame, imageFormat.toLatin1().constData());
        if (!frame.isNull()) {
            frame = frame.convertToFormat(QImage::Format_RGB32);
        }
    }

    emit frameDecoded(peerId, frame, imageFormat, encodedFrame.size(), announcedFrameNumber);
}
