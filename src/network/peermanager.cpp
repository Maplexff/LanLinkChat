#include "peermanager.h"

#include <algorithm>
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainter>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include "network/discoveryservice.h"
#include "network/peerconnection.h"
#include "network/videodecodeworker.h"

namespace {

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString timestampIso()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

QString uniqueFilePath(const QString &directoryPath, const QString &originalFileName)
{
    const QString sanitizedName = originalFileName.trimmed().isEmpty()
        ? QStringLiteral("received_file")
        : originalFileName.trimmed();
    const QFileInfo info(sanitizedName);
    const QString baseName = info.completeBaseName().isEmpty() ? sanitizedName : info.completeBaseName();
    const QString suffix = info.suffix();

    QString candidate = QDir(directoryPath).filePath(sanitizedName);
    if (!QFileInfo::exists(candidate)) {
        return candidate;
    }

    for (int index = 1; index < 10000; ++index) {
        const QString numberedName = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(baseName).arg(index)
            : QStringLiteral("%1 (%2).%3").arg(baseName).arg(index).arg(suffix);
        candidate = QDir(directoryPath).filePath(numberedName);
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }

    return QDir(directoryPath).filePath(QStringLiteral("%1_%2").arg(QUuid::createUuid().toString(QUuid::WithoutBraces), sanitizedName));
}

QSize transportVideoSize()
{
#ifdef Q_OS_LINUX
    return QSize(480, 270);
#else
    return QSize(640, 360);
#endif
}

int transportJpegQuality()
{
#ifdef Q_OS_LINUX
    return 68;
#else
    return 80;
#endif
}

bool looksLikeJpeg(const QByteArray &bytes)
{
    return bytes.size() >= 4
        && static_cast<uchar>(bytes.at(0)) == 0xFF
        && static_cast<uchar>(bytes.at(1)) == 0xD8
        && static_cast<uchar>(bytes.at(bytes.size() - 2)) == 0xFF
        && static_cast<uchar>(bytes.at(bytes.size() - 1)) == 0xD9;
}

bool looksLikePng(const QByteArray &bytes)
{
    static const QByteArray signature = QByteArray::fromHex("89504E470D0A1A0A");
    return bytes.startsWith(signature);
}

bool looksLikeImageFormat(const QByteArray &bytes, const QByteArray &format)
{
    const QByteArray normalizedFormat = format.trimmed().toUpper();
    if (normalizedFormat == "PNG") {
        return looksLikePng(bytes);
    }
    return looksLikeJpeg(bytes);
}

QImage normalizeFrameForTransport(const QImage &frame, const QSize &targetSize)
{
    if (frame.isNull() || !targetSize.isValid()) {
        return {};
    }

    QImage normalized = frame.convertToFormat(QImage::Format_RGB32);
    if (normalized.isNull()) {
        return {};
    }

    QImage canvas(targetSize, QImage::Format_RGB32);
    canvas.fill(Qt::black);

    const QSize scaledSize = normalized.size().scaled(targetSize, Qt::KeepAspectRatio);
    const QImage scaledFrame = normalized.scaled(scaledSize, Qt::KeepAspectRatio, Qt::FastTransformation);
    if (scaledFrame.isNull()) {
        return {};
    }

    QPainter painter(&canvas);
    const QPoint topLeft((targetSize.width() - scaledSize.width()) / 2,
                         (targetSize.height() - scaledSize.height()) / 2);
    painter.drawImage(topLeft, scaledFrame);
    return canvas;
}

}

PeerManager::PeerManager(QObject *parent)
    : QObject(parent)
    , m_localPeerId(newId())
    , m_displayName(QHostInfo::localHostName().isEmpty() ? QStringLiteral("QtUser") : QHostInfo::localHostName())
    , m_discovery(new DiscoveryService(this))
    , m_server(new QTcpServer(this))
    , m_pruneTimer(new QTimer(this))
    , m_videoDecodeThread(new QThread(this))
    , m_videoDecodeWorker(new VideoDecodeWorker)
{
    m_videoLogTimer.start();

    m_server->listen(QHostAddress::AnyIPv4, 0);

    m_discovery->setLocalPeer(m_localPeerId, m_displayName, m_server->serverPort());
    m_discovery->start();

    m_pruneTimer->setInterval(5000);
    m_pruneTimer->start();

    connect(m_discovery, &DiscoveryService::peerAnnounced, this, &PeerManager::onPeerAnnounced);
    connect(m_server, &QTcpServer::newConnection, this, &PeerManager::acceptPendingConnections);
    connect(m_pruneTimer, &QTimer::timeout, this, &PeerManager::prunePeers);
    m_videoDecodeWorker->moveToThread(m_videoDecodeThread);
    connect(this, &PeerManager::decodeVideoFrameRequested, m_videoDecodeWorker, &VideoDecodeWorker::decodeFrame, Qt::QueuedConnection);
    connect(m_videoDecodeWorker, &VideoDecodeWorker::frameDecoded, this, &PeerManager::onVideoFrameDecoded, Qt::QueuedConnection);
    connect(m_videoDecodeThread, &QThread::finished, m_videoDecodeWorker, &QObject::deleteLater);
    m_videoDecodeThread->start();
}

PeerManager::~PeerManager()
{
    if (m_videoDecodeThread) {
        m_videoDecodeThread->quit();
        m_videoDecodeThread->wait();
    }
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

bool PeerManager::sendDirectMessage(const QString &peerId, const QString &text)
{
    if (text.trimmed().isEmpty()) {
        return false;
    }

    PeerConnection *connection = ensureConnection(peerId);
    if (!connection) {
        emit noticeRaised(QStringLiteral("未能连接到目标节点。"));
        return false;
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("fromId"), m_localPeerId);
    meta.insert(QStringLiteral("fromName"), m_displayName);
    meta.insert(QStringLiteral("text"), text);
    meta.insert(QStringLiteral("timestamp"), timestampIso());
    connection->sendPacket(QStringLiteral("direct_message"), meta);
    return true;
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

void PeerManager::setVideoSendingEnabled(const QString &peerId, bool enabled)
{
    if (PeerConnection *connection = ensureConnection(peerId)) {
        QJsonObject meta;
        meta.insert(QStringLiteral("fromId"), m_localPeerId);
        meta.insert(QStringLiteral("enabled"), enabled);
        connection->sendPacket(QStringLiteral("call_video_state"), meta);
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

    const QImage scaledFrame = normalizeFrameForTransport(frame, transportVideoSize());
    if (scaledFrame.isNull()) {
        qWarning() << "Dropping local video frame: failed to normalize image before encoding";
        return;
    }

    QByteArray imageBytes;
    QBuffer buffer(&imageBytes);
    buffer.open(QIODevice::WriteOnly);
    const QByteArray imageFormat = "JPG";
    const int imageQuality = transportJpegQuality();
    if (!scaledFrame.save(&buffer, imageFormat.constData(), imageQuality)
        || !looksLikeImageFormat(imageBytes, imageFormat)) {
        qWarning() << "Dropping local video frame: failed to encode a valid" << imageFormat << "payload";
        return;
    }

    sendEncodedVideoFrame(peerId, imageBytes, QString::fromLatin1(imageFormat), scaledFrame.size());
}

void PeerManager::sendEncodedVideoFrame(const QString &peerId,
                                        const QByteArray &encodedFrame,
                                        const QString &imageFormat,
                                        const QSize &frameSize)
{
    if (encodedFrame.isEmpty()) {
        return;
    }

    PeerConnection *connection = ensureConnection(peerId);
    if (!connection) {
        return;
    }

    const QByteArray normalizedFormat = imageFormat.trimmed().toLatin1().toUpper();
    if (!looksLikeImageFormat(encodedFrame, normalizedFormat)) {
        qWarning() << "Dropping local video frame: invalid encoded payload for" << normalizedFormat;
        return;
    }

    QJsonObject meta;
    meta.insert(QStringLiteral("fromId"), m_localPeerId);
    meta.insert(QStringLiteral("timestamp"), timestampIso());
    const quint64 frameNumber = ++m_sentVideoFrames[peerId];
    meta.insert(QStringLiteral("frameNumber"), static_cast<qint64>(frameNumber));
    meta.insert(QStringLiteral("width"), frameSize.width());
    meta.insert(QStringLiteral("height"), frameSize.height());
    meta.insert(QStringLiteral("imageFormat"), QString::fromLatin1(normalizedFormat));
    connection->sendPacket(QStringLiteral("video_frame"), meta, encodedFrame);

    if (m_videoLogTimer.elapsed() >= 3000) {
        qInfo() << "Video tx peer =" << peerId
                << "frame =" << frameNumber
                << "size =" << encodedFrame.size()
                << "resolution =" << frameSize
                << "format =" << normalizedFormat;
        m_videoLogTimer.restart();
    }
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

    emitPeersChanged();
}

void PeerManager::acceptPendingConnections()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
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

    QString peerId = m_peerIdsByConnection.value(connection);
    if (peerId.isEmpty()) {
        QString metaPeerId = meta.value(QStringLiteral("fromId")).toString();
        QString metaPeerName = meta.value(QStringLiteral("fromName")).toString();

        if (metaPeerId.isEmpty()) {
            metaPeerId = meta.value(QStringLiteral("creatorId")).toString();
            metaPeerName = meta.value(QStringLiteral("creatorName")).toString();
        }

        if (!metaPeerId.isEmpty() && metaPeerId != m_localPeerId) {
            registerConnection(connection,
                               metaPeerId,
                               metaPeerName.isEmpty() ? peerDisplayName(metaPeerId) : metaPeerName);
            peerId = metaPeerId;
        }
    }

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

    if (type == QLatin1String("call_video_state")) {
        emit remoteVideoSendingChanged(peerId, meta.value(QStringLiteral("enabled")).toBool(true));
        return;
    }

    if (type == QLatin1String("video_frame")) {
        const QByteArray imageFormat = meta.value(QStringLiteral("imageFormat")).toString().toLatin1().toUpper();
        const QByteArray effectiveFormat = imageFormat.isEmpty() ? QByteArray("JPG") : imageFormat;
        const bool signatureValid = effectiveFormat == "PNG" ? looksLikePng(binary) : looksLikeJpeg(binary);
        if (!signatureValid) {
            qWarning() << "Dropping remote video frame from" << peerId << ": invalid" << effectiveFormat << "payload, size =" << binary.size();
            return;
        }
        const qint64 announcedFrameNumber = static_cast<qint64>(meta.value(QStringLiteral("frameNumber")).toDouble(-1));
        if (!m_videoDecodeInFlight) {
            m_videoDecodeInFlight = true;
            emit decodeVideoFrameRequested(peerId,
                                           binary,
                                           QString::fromLatin1(effectiveFormat),
                                           announcedFrameNumber);
        } else {
            m_pendingVideoDecode.peerId = peerId;
            m_pendingVideoDecode.encodedFrame = binary;
            m_pendingVideoDecode.imageFormat = QString::fromLatin1(effectiveFormat);
            m_pendingVideoDecode.announcedFrameNumber = announcedFrameNumber;
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

void PeerManager::onVideoFrameDecoded(const QString &peerId,
                                      const QImage &frame,
                                      const QString &imageFormat,
                                      int payloadSize,
                                      qint64 announcedFrameNumber)
{
    m_videoDecodeInFlight = false;

    if (!frame.isNull()) {
        const quint64 receivedFrameNumber = ++m_receivedVideoFrames[peerId];
        if (m_videoLogTimer.elapsed() >= 3000) {
            qInfo() << "Video rx peer =" << peerId
                    << "frame =" << receivedFrameNumber
                    << "announced =" << announcedFrameNumber
                    << "size =" << payloadSize
                    << "resolution =" << frame.size()
                    << "format =" << imageFormat;
            m_videoLogTimer.restart();
        }

        emit remoteVideoFrameReceived(peerId, frame);
    } else {
        qWarning() << "Dropping remote video frame from" << peerId
                   << ":" << imageFormat << "decode failed, size =" << payloadSize;
    }

    if (m_pendingVideoDecode.encodedFrame.isEmpty() || m_pendingVideoDecode.peerId.isEmpty()) {
        return;
    }

    const PendingVideoDecode next = m_pendingVideoDecode;
    m_pendingVideoDecode = PendingVideoDecode();
    m_videoDecodeInFlight = true;
    emit decodeVideoFrameRequested(next.peerId,
                                   next.encodedFrame,
                                   next.imageFormat,
                                   next.announcedFrameNumber);
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

    if (!peerId.isEmpty()) {
        emit callEnded(peerId);
        emit noticeRaised(QStringLiteral("%1 的连接已断开。").arg(peerDisplayName(peerId)));
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
    if (peer.id.isEmpty() || peer.port == 0 || peer.address.isNull() || !peer.online) {
        return nullptr;
    }

    auto *socket = new QTcpSocket(this);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    socket->connectToHost(peer.address, peer.port);
    if (!socket->waitForConnected(600)) {
        socket->deleteLater();
        return nullptr;
    }

    auto *connection = new PeerConnection(socket, this);
    wireConnection(connection);
    registerConnection(connection, peerId, peer.name);
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
    const QString savePath = availableDownloadPath(fileName);
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

    if (incoming.expectedSize > 0 && incoming.receivedSize != incoming.expectedSize) {
        if (incoming.file) {
            incoming.file->deleteLater();
        }
        QFile::remove(incoming.savePath);
        emit noticeRaised(QStringLiteral("收到来自 %1 的文件 %2 时发生中断，已丢弃不完整文件。")
                              .arg(peerDisplayName(incoming.peerId), incoming.fileName));
        return;
    }

    emit fileReceived(incoming.peerId, incoming.fileName, incoming.savePath);
    if (incoming.file) {
        incoming.file->deleteLater();
    }
}

QString PeerManager::availableDownloadPath(const QString &fileName) const
{
    return uniqueFilePath(appDataDownloadPath(), fileName);
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
