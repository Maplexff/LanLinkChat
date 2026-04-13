#include "discoveryservice.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QTimer>
#include <QUdpSocket>

#include "network/protocol.h"

DiscoveryService::DiscoveryService(QObject *parent)
    : QObject(parent)
    , m_socket(new QUdpSocket(this))
    , m_announceTimer(new QTimer(this))
{
    m_announceTimer->setInterval(2000);

    connect(m_socket, &QUdpSocket::readyRead, this, &DiscoveryService::readPendingDatagrams);
    connect(m_announceTimer, &QTimer::timeout, this, &DiscoveryService::sendAnnouncement);
}

void DiscoveryService::setLocalPeer(const QString &peerId, const QString &displayName, quint16 tcpPort)
{
    m_peerId = peerId;
    m_displayName = displayName;
    m_tcpPort = tcpPort;
}

void DiscoveryService::start()
{
    m_socket->bind(QHostAddress::AnyIPv4,
                   Protocol::discoveryPort,
                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    m_announceTimer->start();
    sendAnnouncement();
}

void DiscoveryService::announceNow()
{
    sendAnnouncement();
}

void DiscoveryService::sendAnnouncement()
{
    if (m_peerId.isEmpty() || m_tcpPort == 0) {
        return;
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("app"), QStringLiteral("LanLinkChat"));
    obj.insert(QStringLiteral("peerId"), m_peerId);
    obj.insert(QStringLiteral("name"), m_displayName);
    obj.insert(QStringLiteral("port"), static_cast<int>(m_tcpPort));

    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    m_socket->writeDatagram(payload, QHostAddress::Broadcast, Protocol::discoveryPort);
}

void DiscoveryService::readPendingDatagrams()
{
    while (m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram();
        const QJsonDocument doc = QJsonDocument::fromJson(datagram.data());
        if (!doc.isObject()) {
            continue;
        }

        const QJsonObject obj = doc.object();
        if (obj.value(QStringLiteral("app")).toString() != QLatin1String("LanLinkChat")) {
            continue;
        }

        PeerInfo peer;
        peer.id = obj.value(QStringLiteral("peerId")).toString();
        if (peer.id.isEmpty() || peer.id == m_peerId) {
            continue;
        }

        peer.name = obj.value(QStringLiteral("name")).toString();
        peer.port = static_cast<quint16>(obj.value(QStringLiteral("port")).toInt());
        peer.address = datagram.senderAddress();
        peer.lastSeen = QDateTime::currentDateTime();
        peer.online = true;
        emit peerAnnounced(peer);
    }
}
