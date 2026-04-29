#pragma once

#include <QObject>

#include "model/peerinfo.h"

class QUdpSocket;
class QTimer;

class DiscoveryService : public QObject
{
    Q_OBJECT

public:
    explicit DiscoveryService(QObject *parent = nullptr);

    void setLocalPeer(const QString &peerId, const QString &displayName, quint16 tcpPort);
    void start();
    void announceNow();

signals:
    void peerAnnounced(const PeerInfo &peer);

private slots:
    void sendAnnouncement();
    void readPendingDatagrams();

private:
    void sendAnnouncementTo(const QHostAddress &targetAddress, bool isReply);

    QString m_peerId;
    QString m_displayName;
    quint16 m_tcpPort = 0;
    QUdpSocket *m_socket = nullptr;
    QTimer *m_announceTimer = nullptr;
};
