#pragma once

#include <QObject>

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

class VideoEncodeWorker : public QObject
{
    Q_OBJECT

public:
    explicit VideoEncodeWorker(QObject *parent = nullptr);

public slots:
    void encodeFrame(const QString &peerId, const QImage &frame);

signals:
    void frameEncoded(const QString &peerId,
                      const QByteArray &encodedFrame,
                      const QString &imageFormat,
                      const QSize &frameSize);
};
