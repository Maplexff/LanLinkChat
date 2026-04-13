#include "peermanager.h"

#include <algorithm>
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

#include "network/discoveryservice.h"
#include "network/peerconnection.h"

namespace {

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString timestampIso()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

}

PeerManager::PeerManager(QObject *parent)
    : QObject(parent)
    , m_localPeerId(newId())
    , m_displayName(QHostInfo::localHostName().isEmpty() ? QStringLiteral("QtUser") : QHostInfo::localHostName())
    , m_discovery(new DiscoveryService(this))
    , m_server(new QTcpServer(this))
    , m_pruneTimer(new QTimer(this))
{
    m_server->listen(QHostAddress::AnyIPv4, 0);

    m_discovery->setLocalPeer(m_localPeerId, m_displayName, m_server->serverPort());
    m_discovery->start();

    m_pruneTimer->setInterval(5000);
    m_pruneTimer->start();

    connect(m_discovery, &DiscoveryService::peerAnnounced, this, &PeerManager::onPeerAnnounced);
    connect(m_server, &QTcpServer::newConnection, this, &PeerManager::acceptPendingConnections);
    connect(m_pruneTimer, &QTimer::timeout, this, &PeerManager::prunePeers);
}

QString PeerManager::localPeerId() const
{
    return m_localPeerId;
}

QString PeerManager::displayName() const
{
    return m_displayName;
}

QList<PeerInfo> PeerManager::peers() const
{
    return m_peers.values();
}

QList<GroupInfo> PeerManager::groups() const
{
    return m_groups.values();
}

void PeerManager::setDisplayName(const QString &displayName)
{
    const QString normalized = displayName.trimmed().isEmpty() ? QStringLiteral("QtUser") : displayName.trimmed();
    m_displayName = normalized;
    m_discovery->setLocalPeer(m_localPeerId, m_displayName, m_server->serverPort());
}

void PeerManager::refreshDiscovery()
{
    m_discovery->announceNow();
    emit noticeRaised(QStringLiteral("已发送手动发现广播，正在刷新局域网节点。"));
}

void PeerManager::sendDirectMessage(const QString &peerId, const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return;
    }

    PeerConnection *connection = ensureConnection(peerId);
    if (!connection) {
        emit noticeRaised(QStringLiteral("未能连接到目标节点。"));
        return;
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("fromId"), m_localPeerId);
    meta.insert(QStringLiteral("fromName"), m_displayName);
    meta.insert(QStringLiteral("text"), text);
    meta.insert(QStringLiteral("timestamp"), timestampIso());
    connection->sendPacket(QStringLiteral("direct_message"), meta);
}

GroupInfo PeerManager::createGroup(const QString &name, const QStringList &memberIds)
{
    GroupInfo group;
    group.id = newId();
    group.name = name.trimmed().isEmpty() ? QStringLiteral("未命名群聊") : name.trimmed();
    group.members = normalizedMembers(memberIds);

    m_groups.insert(group.id, group);
    emitGroupsChanged();

    QJsonArray membersArray;
    for (const QString &memberId : group.members) {
        membersArray.append(memberId);
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("groupId"), group.id);
    meta.insert(QStringLiteral("groupName"), group.name);
    meta.insert(QStringLiteral("members"), membersArray);
    meta.insert(QStringLiteral("creatorId"), m_localPeerId);
    meta.insert(QStringLiteral("creatorName"), m_displayName);

    for (const QString &memberId : group.members) {
        if (memberId == m_localPeerId) {
            continue;
        }
        if (PeerConnection *connection = ensureConnection(memberId)) {
            connection->sendPacket(QStringLiteral("group_created"), meta);
        }
    }

    return group;
}

void PeerManager::sendGroupMessage(const QString &groupId, const QString &text)
{
    if (!m_groups.contains(groupId) || text.trimmed().isEmpty()) {
        return;
    }

    const GroupInfo group = m_groups.value(groupId);
    QJsonObject meta;
    meta.insert(QStringLiteral("groupId"), group.id);
    meta.insert(QStringLiteral("groupName"), group.name);
    meta.insert(QStringLiteral("fromId"), m_localPeerId);
    meta.insert(QStringLiteral("fromName"), m_displayName);
    meta.insert(QStringLiteral("text"), text);
    meta.insert(QStringLiteral("timestamp"), timestampIso());

    for (const QString &memberId : group.members) {
        if (memberId == m_localPeerId) {
            continue;
        }
        if (PeerConnection *connection = ensureConnection(memberId)) {
            connection->sendPacket(QStringLiteral("group_message"), meta);
        }
    }
}

bool PeerManager::sendFile(const QString &peerId, const QString &filePath)
{
    PeerConnection *connection = ensureConnection(peerId);
    if (!connection) {
        emit noticeRaised(QStringLiteral("未能建立文件传输连接。"));
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit noticeRaised(QStringLiteral("文件打开失败。"));
        return false;
    }

    const QString transferId = newId();
    const QFileInfo info(file);

    QJsonObject begin;
    begin.insert(QStringLiteral("transferId"), transferId);
    begin.insert(QStringLiteral("fromId"), m_localPeerId);
    begin.insert(QStringLiteral("fromName"), m_displayName);
    begin.insert(QStringLiteral("fileName"), info.fileName());
    begin.insert(QStringLiteral("fileSize"), static_cast<qint64>(file.size()));
    begin.insert(QStringLiteral("timestamp"), timestampIso());
    connection->sendPacket(QStringLiteral("file_begin"), begin);

    constexpr qint64 chunkSize = 64 * 1024;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(chunkSize);
        QJsonObject meta;
        meta.insert(QStringLiteral("transferId"), transferId);
        connection->sendPacket(QStringLiteral("file_chunk"), meta, chunk);
    }

    QJsonObject end;
    end.insert(QStringLiteral("transferId"), transferId);
    connection->sendPacket(QStringLiteral("file_end"), end);
    return true;
}

void PeerManager::inviteToCall(const QString &peerId)
{
    if (PeerConnection *connection = ensureConnection(peerId)) {
        QJsonObject meta;
        meta.insert(QStringLiteral("fromId"), m_localPeerId);
        meta.insert(QStringLiteral("fromName"), m_displayName);
        connection->sendPacket(QStringLiteral("call_invite"), meta);
    }
}

void PeerManager::acceptCall(const QString &peerId)
{
    if (PeerConnection *connection = ensureConnection(peerId)) {
        QJsonObject meta;
        meta.insert(QStringLiteral("fromId"), m_localPeerId);
        meta.insert(QStringLiteral("fromName"), m_displayName);
        connection->sendPacket(QStringLiteral("call_accept"), meta);
    }
}

void PeerManager::endCall(const QString &peerId)
{
    if (PeerConnection *connection = ensureConnection(peerId)) {
        QJsonObject meta;
        meta.insert(QStringLiteral("fromId"), m_localPeerId);
        connection->sendPacket(QStringLiteral("call_end"), meta);
    }
}

void PeerManager::sendVideoFrame(const QString &peerId, const QImage &frame)
{
    if (frame.isNull()) {
        return;
    }

    PeerConnection *connection = ensureConnection(peerId);
    if (!connection) {
        return;
    }

    QByteArray jpegBytes;
    QBuffer buffer(&jpegBytes);
    buffer.open(QIODevice::WriteOnly);
    frame.scaled(640, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "JPG", 70);

    QJsonObject meta;
    meta.insert(QStringLiteral("fromId"), m_localPeerId);
    meta.insert(QStringLiteral("timestamp"), timestampIso());
    connection->sendPacket(QStringLiteral("video_frame"), meta, jpegBytes);
}

void PeerManager::sendAudioChunk(const QString &peerId, const QByteArray &audioData, int sampleRate, int channelCount, int sampleFormat)
{
    if (audioData.isEmpty()) {
        return;
    }

    PeerConnection *connection = ensureConnection(peerId);
    if (!connection) {
        return;
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("fromId"), m_localPeerId);
    meta.insert(QStringLiteral("timestamp"), timestampIso());
    meta.insert(QStringLiteral("sampleRate"), sampleRate);
    meta.insert(QStringLiteral("channelCount"), channelCount);
    meta.insert(QStringLiteral("sampleFormat"), sampleFormat);
    connection->sendPacket(QStringLiteral("audio_chunk"), meta, audioData);
}

void PeerManager::onPeerAnnounced(const PeerInfo &peer)
{
    PeerInfo current = m_peers.value(peer.id);
    current.id = peer.id;
    current.name = peer.name;
    current.address = peer.address;
    current.port = peer.port;
    current.lastSeen = peer.lastSeen;
    current.online = true;
    m_peers.insert(peer.id, current);

    if (!m_connectionsByPeerId.contains(peer.id) && m_localPeerId > peer.id) {
        ensureConnection(peer.id);
    }

    emitPeersChanged();
}

void PeerManager::acceptPendingConnections()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        auto *connection = new PeerConnection(socket, this);
        wireConnection(connection);
        sendHello(connection);
    }
}

void PeerManager::onPacketReceived(PeerConnection *connection, const QString &type, const QJsonObject &meta, const QByteArray &binary)
{
    if (type == QLatin1String("hello")) {
        handleHello(connection, meta);
        return;
    }

    const QString peerId = m_peerIdsByConnection.value(connection);

    if (type == QLatin1String("direct_message")) {
        const QDateTime ts = QDateTime::fromString(meta.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
        emit directMessageReceived(peerId, meta.value(QStringLiteral("text")).toString(), ts.isValid() ? ts : QDateTime::currentDateTime());
        return;
    }

    if (type == QLatin1String("group_created")) {
        GroupInfo group;
        group.id = meta.value(QStringLiteral("groupId")).toString();
        group.name = meta.value(QStringLiteral("groupName")).toString();
        const QJsonArray membersArray = meta.value(QStringLiteral("members")).toArray();
        for (const QJsonValue &value : membersArray) {
            group.members.append(value.toString());
        }
        m_groups.insert(group.id, group);
        emitGroupsChanged();
        emit noticeRaised(QStringLiteral("已收到群聊：%1").arg(group.name));
        return;
    }

    if (type == QLatin1String("group_message")) {
        const QDateTime ts = QDateTime::fromString(meta.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
        emit groupMessageReceived(meta.value(QStringLiteral("groupId")).toString(),
                                  peerId,
                                  meta.value(QStringLiteral("text")).toString(),
                                  ts.isValid() ? ts : QDateTime::currentDateTime());
        return;
    }

    if (type == QLatin1String("file_begin")) {
        handleIncomingFileBegin(meta);
        return;
    }

    if (type == QLatin1String("file_chunk")) {
        handleIncomingFileChunk(meta, binary);
        return;
    }

    if (type == QLatin1String("file_end")) {
        handleIncomingFileEnd(meta);
        return;
    }

    if (type == QLatin1String("call_invite")) {
        emit callInvitationReceived(peerId);
        return;
    }

    if (type == QLatin1String("call_accept")) {
        emit callAccepted(peerId);
        return;
    }

    if (type == QLatin1String("call_end")) {
        emit callEnded(peerId);
        return;
    }

    if (type == QLatin1String("video_frame")) {
        QImage frame;
        frame.loadFromData(binary, "JPG");
        if (!frame.isNull()) {
            emit remoteVideoFrameReceived(peerId, frame);
        }
        return;
    }

    if (type == QLatin1String("audio_chunk")) {
        emit remoteAudioChunkReceived(peerId,
                                      binary,
                                      meta.value(QStringLiteral("sampleRate")).toInt(),
                                      meta.value(QStringLiteral("channelCount")).toInt(),
                                      meta.value(QStringLiteral("sampleFormat")).toInt());
    }
}

void PeerManager::onConnectionClosed(PeerConnection *connection)
{
    const QString peerId = m_peerIdsByConnection.take(connection);
    if (!peerId.isEmpty() && m_connectionsByPeerId.value(peerId) == connection) {
        m_connectionsByPeerId.remove(peerId);
    }

    if (m_peers.contains(peerId)) {
        PeerInfo peer = m_peers.value(peerId);
        peer.online = false;
        m_peers.insert(peerId, peer);
        emitPeersChanged();
    }
}

void PeerManager::prunePeers()
{
    const QDateTime now = QDateTime::currentDateTime();
    bool changed = false;

    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
        const bool connected = m_connectionsByPeerId.contains(it.key());
        const bool online = connected || it->lastSeen.secsTo(now) < 8;
        if (it->online != online) {
            it->online = online;
            changed = true;
        }
    }

    if (changed) {
        emitPeersChanged();
    }
}

QStringList PeerManager::normalizedMembers(const QStringList &memberIds) const
{
    QStringList members = memberIds;
    members.removeAll(QString());
    if (!members.contains(m_localPeerId)) {
        members.append(m_localPeerId);
    }
    members.removeDuplicates();
    return members;
}

void PeerManager::emitPeersChanged()
{
    QList<PeerInfo> list = m_peers.values();
    std::sort(list.begin(), list.end(), [](const PeerInfo &left, const PeerInfo &right) {
        if (left.online != right.online) {
            return left.online && !right.online;
        }
        return left.name.toLower() < right.name.toLower();
    });
    emit peersChanged(list);
}

void PeerManager::emitGroupsChanged()
{
    QList<GroupInfo> list = m_groups.values();
    std::sort(list.begin(), list.end(), [](const GroupInfo &left, const GroupInfo &right) {
        return left.name.toLower() < right.name.toLower();
    });
    emit groupsChanged(list);
}

void PeerManager::sendHello(PeerConnection *connection)
{
    QJsonObject meta;
    meta.insert(QStringLiteral("peerId"), m_localPeerId);
    meta.insert(QStringLiteral("name"), m_displayName);
    connection->sendPacket(QStringLiteral("hello"), meta);
}

void PeerManager::registerConnection(PeerConnection *connection, const QString &peerId, const QString &peerName)
{
    connection->setPeerIdentity(peerId, peerName);

    if (PeerConnection *existing = m_connectionsByPeerId.value(peerId)) {
        if (existing != connection) {
            existing->socket()->disconnectFromHost();
        }
    }

    m_connectionsByPeerId.insert(peerId, connection);
    m_peerIdsByConnection.insert(connection, peerId);

    PeerInfo peer = m_peers.value(peerId);
    peer.id = peerId;
    peer.name = peerName;
    if (connection->socket()) {
        peer.address = connection->socket()->peerAddress();
    }
    peer.lastSeen = QDateTime::currentDateTime();
    peer.online = true;
    m_peers.insert(peerId, peer);
    emitPeersChanged();
}

PeerConnection *PeerManager::ensureConnection(const QString &peerId)
{
    if (PeerConnection *connection = m_connectionsByPeerId.value(peerId)) {
        if (connection->socket()->state() == QAbstractSocket::ConnectedState) {
            return connection;
        }
    }

    const PeerInfo peer = m_peers.value(peerId);
    if (peer.id.isEmpty() || peer.port == 0 || peer.address.isNull()) {
        return nullptr;
    }

    auto *socket = new QTcpSocket(this);
    socket->connectToHost(peer.address, peer.port);
    if (!socket->waitForConnected(3000)) {
        socket->deleteLater();
        return nullptr;
    }

    auto *connection = new PeerConnection(socket, this);
    wireConnection(connection);
    sendHello(connection);
    return connection;
}

void PeerManager::wireConnection(PeerConnection *connection)
{
    connect(connection, &PeerConnection::packetReceived, this, &PeerManager::onPacketReceived);
    connect(connection, &PeerConnection::connectionClosed, this, &PeerManager::onConnectionClosed);
}

void PeerManager::handleHello(PeerConnection *connection, const QJsonObject &meta)
{
    const QString peerId = meta.value(QStringLiteral("peerId")).toString();
    const QString peerName = meta.value(QStringLiteral("name")).toString();
    if (peerId.isEmpty() || peerId == m_localPeerId) {
        return;
    }
    registerConnection(connection, peerId, peerName);
}

void PeerManager::handleIncomingFileBegin(const QJsonObject &meta)
{
    const QString transferId = meta.value(QStringLiteral("transferId")).toString();
    if (transferId.isEmpty()) {
        return;
    }

    QDir().mkpath(appDataDownloadPath());
    const QString fileName = meta.value(QStringLiteral("fileName")).toString();
    const QString savePath = QDir(appDataDownloadPath()).filePath(fileName);
    auto *file = new QFile(savePath, this);
    if (!file->open(QIODevice::WriteOnly)) {
        delete file;
        return;
    }

    IncomingFile incoming;
    incoming.peerId = meta.value(QStringLiteral("fromId")).toString();
    incoming.fileName = fileName;
    incoming.expectedSize = static_cast<qint64>(meta.value(QStringLiteral("fileSize")).toDouble());
    incoming.savePath = savePath;
    incoming.file = file;
    m_incomingFiles.insert(transferId, incoming);
}

void PeerManager::handleIncomingFileChunk(const QJsonObject &meta, const QByteArray &binary)
{
    const QString transferId = meta.value(QStringLiteral("transferId")).toString();
    if (!m_incomingFiles.contains(transferId)) {
        return;
    }

    IncomingFile &incoming = m_incomingFiles[transferId];
    if (incoming.file) {
        incoming.file->write(binary);
        incoming.receivedSize += binary.size();
    }
}

void PeerManager::handleIncomingFileEnd(const QJsonObject &meta)
{
    const QString transferId = meta.value(QStringLiteral("transferId")).toString();
    if (!m_incomingFiles.contains(transferId)) {
        return;
    }

    IncomingFile incoming = m_incomingFiles.take(transferId);
    if (incoming.file) {
        incoming.file->flush();
        incoming.file->close();
    }

    emit fileReceived(incoming.peerId, incoming.fileName, incoming.savePath);
    if (incoming.file) {
        incoming.file->deleteLater();
    }
}

QString PeerManager::peerDisplayName(const QString &peerId) const
{
    if (peerId == m_localPeerId) {
        return m_displayName;
    }
    return m_peers.value(peerId).name;
}

QString PeerManager::appDataDownloadPath() const
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("downloads"));
}
