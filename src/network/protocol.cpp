#include "protocol.h"

#include <QBuffer>
#include <QDataStream>
#include <QJsonDocument>

namespace Protocol {

QByteArray buildPacket(const QString &type, const QJsonObject &meta, const QByteArray &binary)
{
    QByteArray body;
    QBuffer bodyBuffer(&body);
    bodyBuffer.open(QIODevice::WriteOnly);

    QDataStream bodyStream(&bodyBuffer);
    bodyStream.setVersion(QDataStream::Qt_6_5);
    bodyStream << type;
    bodyStream << QJsonDocument(meta).toJson(QJsonDocument::Compact);
    bodyStream << binary;

    QByteArray packet;
    QBuffer packetBuffer(&packet);
    packetBuffer.open(QIODevice::WriteOnly);

    QDataStream packetStream(&packetBuffer);
    packetStream.setVersion(QDataStream::Qt_6_5);
    packetStream << quint32(body.size());
    packet.append(body);
    return packet;
}

bool takePacket(QByteArray &buffer, QString &type, QJsonObject &meta, QByteArray &binary)
{
    if (buffer.size() < static_cast<int>(sizeof(quint32))) {
        return false;
    }

    QByteArray headerBytes = buffer.left(static_cast<int>(sizeof(quint32)));
    QBuffer headerBuffer(&headerBytes);
    headerBuffer.open(QIODevice::ReadOnly);

    QDataStream headerStream(&headerBuffer);
    headerStream.setVersion(QDataStream::Qt_6_5);

    quint32 bodySize = 0;
    headerStream >> bodySize;

    const int packetSize = static_cast<int>(sizeof(quint32) + bodySize);
    if (buffer.size() < packetSize) {
        return false;
    }

    QByteArray body = buffer.mid(static_cast<int>(sizeof(quint32)), static_cast<int>(bodySize));
    buffer.remove(0, packetSize);

    QBuffer bodyBuffer(&body);
    bodyBuffer.open(QIODevice::ReadOnly);

    QDataStream bodyStream(&bodyBuffer);
    bodyStream.setVersion(QDataStream::Qt_6_5);

    QByteArray metaBytes;
    bodyStream >> type;
    bodyStream >> metaBytes;
    bodyStream >> binary;

    const QJsonDocument doc = QJsonDocument::fromJson(metaBytes);
    meta = doc.object();
    return !type.isEmpty();
}

}
