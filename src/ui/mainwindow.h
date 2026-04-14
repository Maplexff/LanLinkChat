#pragma once

#include <QDateTime>
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QResizeEvent>

#include <QMediaCaptureSession>

#include "model/chattypes.h"
#include "model/peerinfo.h"

class PeerManager;
class QAudioSink;
class QAudioSource;
class QCamera;
class QIODevice;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QLabel;
class QVideoFrame;
class QVideoSink;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void applyDisplayName();
    void triggerPeerDiscovery();
    void sendTextMessage();
    void sendFileToPeer();
    void createGroup();
    void startVideoCall();
    void endVideoCall();
    void refreshConversationView();
    void onPeersChanged(const QList<PeerInfo> &peers);
    void onGroupsChanged(const QList<GroupInfo> &groups);
    void onDirectMessageReceived(const QString &peerId, const QString &text, const QDateTime &timestamp);
    void onGroupMessageReceived(const QString &groupId, const QString &peerId, const QString &text, const QDateTime &timestamp);
    void onFileReceived(const QString &peerId, const QString &fileName, const QString &savedPath);
    void onNoticeRaised(const QString &message);
    void onCallInvitationReceived(const QString &peerId);
    void onCallAccepted(const QString &peerId);
    void onCallEnded(const QString &peerId);
    void onRemoteVideoFrameReceived(const QString &peerId, const QImage &frame);
    void onRemoteAudioChunkReceived(const QString &peerId, const QByteArray &audioData, int sampleRate, int channelCount, int sampleFormat);
    void processLocalFrame(const QVideoFrame &frame);
    void readLocalAudioInput();
    void updateActionState();

private:
    void buildUi();
    void appendHistoryLine(const QString &conversationKey, const QString &line);
    QString currentPeerId() const;
    QString currentGroupId() const;
    QString currentConversationKey() const;
    QString peerName(const QString &peerId) const;
    QString groupName(const QString &groupId) const;
    void selectGroup(const QString &groupId);
    bool ensureCameraRunning();
    bool startAudioCapture();
    bool ensureAudioPlayback(const QAudioFormat &format);
    void stopAudioCapture();
    void stopAudioPlayback();
    void stopCallMedia();
    void stopCamera();
    void setVideoLabelImage(QLabel *label, const QImage &image);
    void refreshVideoLabels();

    PeerManager *m_manager = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QListWidget *m_peerList = nullptr;
    QListWidget *m_groupList = nullptr;
    QTabWidget *m_sidebarTabs = nullptr;
    QTabWidget *m_contentTabs = nullptr;
    QPlainTextEdit *m_transcript = nullptr;
    QLineEdit *m_messageEdit = nullptr;
    QPushButton *m_refreshPeersButton = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_fileButton = nullptr;
    QPushButton *m_groupButton = nullptr;
    QPushButton *m_videoButton = nullptr;
    QPushButton *m_hangupButton = nullptr;
    QLabel *m_localVideoLabel = nullptr;
    QLabel *m_remoteVideoLabel = nullptr;
    QLabel *m_conversationTitle = nullptr;
    QLabel *m_callStatusLabel = nullptr;

    QHash<QString, PeerInfo> m_peers;
    QHash<QString, GroupInfo> m_groups;
    QHash<QString, QStringList> m_history;
    QString m_activeCallPeerId;

    QCamera *m_camera = nullptr;
    QMediaCaptureSession m_captureSession;
    QVideoSink *m_videoSink = nullptr;
    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_audioInputDevice = nullptr;
    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_audioOutputDevice = nullptr;
    QAudioFormat m_audioInputFormat;
    QAudioFormat m_audioOutputFormat;
    QElapsedTimer m_frameLimiter;
    QElapsedTimer m_frameWarningLimiter;
    QImage m_lastLocalFrame;
    QImage m_lastRemoteFrame;
};
