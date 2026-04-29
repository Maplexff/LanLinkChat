#include "peerconnection.h"

#include <QTcpSocket>

#include "network/protocol.h"

PeerConnection::PeerConnection(QTcpSocket *socket, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
{
    m_socket->setParent(this);

    connect(m_socket, &QTcpSocket::readyRead, this, &PeerConnection::readSocket);
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        emit connectionClosed(this);
        deleteLater();
    });
}

QTcpSocket *PeerConnection::socket() const
{
    return m_socket;
}

QString PeerConnection::peerId() const
{
    return m_peerId;
}

QString PeerConnection::peerName() const
{
    return m_peerName;
}

void PeerConnection::setPeerIdentity(const QString &peerId, const QString &peerName)
{
    m_peerId = peerId;
    m_peerName = peerName;
}

void PeerConnection::sendPacket(const QString &type, const QJsonObject &meta, const QByteArray &binary)
{
    m_socket->write(Protocol::buildPacket(type, meta, binary));
}

void PeerConnection::readSocket()
{
    m_buffer.append(m_socket->readAll());

    QString type;
    QJsonObject meta;
    QByteArray binary;
    while (Protocol::takePacket(m_buffer, type, meta, binary)) {
        emit packetReceived(this, type, meta, binary);
    }
}
