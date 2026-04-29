#pragma once

#include <QObject>

#include <QByteArray>
#include <QImage>
#include <QString>

class VideoDecodeWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoDecodeWorker(QObject *parent = nullptr);

public slots:
    void decodeFrame(const QString &peerId,
                     const QByteArray &encodedFrame,
                     const QString &imageFormat,
                     qint64 announcedFrameNumber);

signals:
    void frameDecoded(const QString &peerId,
                      const QImage &frame,
                      const QString &imageFormat,
                      int payloadSize,
                      qint64 announcedFrameNumber);
};
