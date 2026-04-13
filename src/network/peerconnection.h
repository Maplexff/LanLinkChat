#pragma once

#include <QObject>

#include <QJsonObject>

class QTcpSocket;

class PeerConnection : public QObject
{
    Q_OBJECT

public:
    explicit PeerConnection(QTcpSocket *socket, QObject *parent = nullptr);

    QTcpSocket *socket() const;
    QString peerId() const;
    QString peerName() const;
    void setPeerIdentity(const QString &peerId, const QString &peerName);
    void sendPacket(const QString &type, const QJsonObject &meta = {}, const QByteArray &binary = {});

signals:
    void packetReceived(PeerConnection *connection, const QString &type, const QJsonObject &meta, const QByteArray &binary);
    void connectionClosed(PeerConnection *connection);

private slots:
    void readSocket();

private:
    QTcpSocket *m_socket = nullptr;
    QByteArray m_buffer;
    QString m_peerId;
    QString m_peerName;
};
