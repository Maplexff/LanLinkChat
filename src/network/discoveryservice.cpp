#include "discoveryservice.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QSet>
#include <QTimer>
#include <QUdpSocket>

#include "network/protocol.h"

namespace {

QList<QHostAddress> discoveryTargets()
{
    QSet<QHostAddress> targets;
    targets.insert(QHostAddress::Broadcast);

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp)
            || !(flags & QNetworkInterface::IsRunning)
            || !(flags & QNetworkInterface::CanBroadcast)
            || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }

        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress broadcast = entry.broadcast();
            if (!broadcast.isNull() && broadcast.protocol() == QAbstractSocket::IPv4Protocol) {
                targets.insert(broadcast);
            }
        }
    }

    return targets.values();
}

}

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

    const QList<QHostAddress> targets = discoveryTargets();
    for (const QHostAddress &target : targets) {
        sendAnnouncementTo(target, false);
    }
}

void DiscoveryService::sendAnnouncementTo(const QHostAddress &targetAddress, bool isReply)
{
    if (m_peerId.isEmpty() || m_tcpPort == 0 || targetAddress.isNull()) {
        return;
    }

    QJsonObject obj;
    obj.insert(QStringLiteral("app"), QStringLiteral("LanLinkChat"));
    obj.insert(QStringLiteral("kind"), isReply ? QStringLiteral("reply") : QStringLiteral("announce"));
    obj.insert(QStringLiteral("peerId"), m_peerId);
    obj.insert(QStringLiteral("name"), m_displayName);
    obj.insert(QStringLiteral("port"), static_cast<int>(m_tcpPort));

    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    m_socket->writeDatagram(payload, targetAddress, Protocol::discoveryPort);
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

        if (obj.value(QStringLiteral("kind")).toString() != QLatin1String("reply")
            && datagram.senderAddress().protocol() == QAbstractSocket::IPv4Protocol) {
            sendAnnouncementTo(datagram.senderAddress(), true);
        }
    }
}
