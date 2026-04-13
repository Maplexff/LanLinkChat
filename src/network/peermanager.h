#pragma once

#include <QObject>

#include <QHash>
#include <QImage>
#include <QPointer>

#include "model/chattypes.h"
#include "model/peerinfo.h"

class DiscoveryService;
class PeerConnection;
class QFile;
class QTcpServer;
class QTcpSocket;
class QTimer;

class PeerManager : public QObject
{
    Q_OBJECT

public:
    explicit PeerManager(QObject *parent = nullptr);

    QString localPeerId() const;
    QString displayName() const;
    QList<PeerInfo> peers() const;
    QList<GroupInfo> groups() const;

    void setDisplayName(const QString &displayName);
    void refreshDiscovery();
    void sendDirectMessage(const QString &peerId, const QString &text);
    GroupInfo createGroup(const QString &name, const QStringList &memberIds);
    void sendGroupMessage(const QString &groupId, const QString &text);
    bool sendFile(const QString &peerId, const QString &filePath);
    void inviteToCall(const QString &peerId);
    void acceptCall(const QString &peerId);
    void endCall(const QString &peerId);
    void sendVideoFrame(const QString &peerId, const QImage &frame);
    void sendAudioChunk(const QString &peerId, const QByteArray &audioData, int sampleRate, int channelCount, int sampleFormat);

signals:
    void peersChanged(const QList<PeerInfo> &peers);
    void groupsChanged(const QList<GroupInfo> &groups);
    void directMessageReceived(const QString &peerId, const QString &text, const QDateTime &timestamp);
    void groupMessageReceived(const QString &groupId, const QString &peerId, const QString &text, const QDateTime &timestamp);
    void fileReceived(const QString &peerId, const QString &fileName, const QString &savedPath);
    void noticeRaised(const QString &message);
    void callInvitationReceived(const QString &peerId);
    void callAccepted(const QString &peerId);
    void callEnded(const QString &peerId);
    void remoteVideoFrameReceived(const QString &peerId, const QImage &frame);
    void remoteAudioChunkReceived(const QString &peerId, const QByteArray &audioData, int sampleRate, int channelCount, int sampleFormat);

private slots:
    void onPeerAnnounced(const PeerInfo &peer);
    void acceptPendingConnections();
    void onPacketReceived(PeerConnection *connection, const QString &type, const QJsonObject &meta, const QByteArray &binary);
    void onConnectionClosed(PeerConnection *connection);
    void prunePeers();

private:
    struct IncomingFile {
        QString peerId;
        QString fileName;
        qint64 expectedSize = 0;
        qint64 receivedSize = 0;
        QString savePath;
        QPointer<QFile> file;
    };

    QStringList normalizedMembers(const QStringList &memberIds) const;
    void emitPeersChanged();
    void emitGroupsChanged();
    void sendHello(PeerConnection *connection);
    void registerConnection(PeerConnection *connection, const QString &peerId, const QString &peerName);
    PeerConnection *ensureConnection(const QString &peerId);
    void wireConnection(PeerConnection *connection);
    void handleHello(PeerConnection *connection, const QJsonObject &meta);
    void handleIncomingFileBegin(const QJsonObject &meta);
    void handleIncomingFileChunk(const QJsonObject &meta, const QByteArray &binary);
    void handleIncomingFileEnd(const QJsonObject &meta);
    QString peerDisplayName(const QString &peerId) const;
    QString appDataDownloadPath() const;

    QString m_localPeerId;
    QString m_displayName;
    DiscoveryService *m_discovery = nullptr;
    QTcpServer *m_server = nullptr;
    QTimer *m_pruneTimer = nullptr;
    QHash<QString, PeerInfo> m_peers;
    QHash<QString, GroupInfo> m_groups;
    QHash<QString, PeerConnection *> m_connectionsByPeerId;
    QHash<PeerConnection *, QString> m_peerIdsByConnection;
    QHash<QString, IncomingFile> m_incomingFiles;
};
