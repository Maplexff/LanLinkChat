#pragma once

#include <QDateTime>
#include <QHostAddress>
#include <QMetaType>
#include <QString>

struct PeerInfo {
    QString id;
    QString name;
    QHostAddress address;
    quint16 port = 0;
    QDateTime lastSeen;
    bool online = false;
};

Q_DECLARE_METATYPE(PeerInfo)
Q_DECLARE_METATYPE(QList<PeerInfo>)
