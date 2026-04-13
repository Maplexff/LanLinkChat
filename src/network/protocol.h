#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace Protocol {

constexpr quint16 discoveryPort = 45454;

QByteArray buildPacket(const QString &type, const QJsonObject &meta = {}, const QByteArray &binary = {});
bool takePacket(QByteArray &buffer, QString &type, QJsonObject &meta, QByteArray &binary);

}
