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
class QCameraDevice;
class QMediaDevices;
class QCheckBox;
class QComboBox;
class QIODevice;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;
class QVideoFrame;
class QVideoSink;
class QLabel;
class VideoFrameWidget;
namespace Ui { class MainWindow; }

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
    void applyCameraSelection();
    void applyReceiveOnlySetting(bool enabled);
    void applyReceiveRemoteVideoOnlySetting(bool enabled);
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
    void flushVideoFrames();
    void updateActionState();
    void onVideoInputsChanged();
    void onAudioInputsChanged();
    void onAudioOutputsChanged();
    void onCameraErrorOccurred();
    void onCameraActiveChanged(bool active);
    void onAudioSourceStateChanged();
    void onAudioSinkStateChanged();

private:
    void setupUi();
    void connectUi();
    void appendHistoryLine(const QString &conversationKey, const QString &line);
    void populateCameraDevices();
    bool prepareCallCamera();
    bool shouldSendLocalVideo() const;
    QString currentPeerId() const;
    QString currentGroupId() const;
    QString currentConversationKey() const;
    QString peerName(const QString &peerId) const;
    QString groupName(const QString &groupId) const;
    QCameraDevice selectedCameraDevice() const;
    void selectGroup(const QString &groupId);
    bool ensureCameraRunning();
    bool startAudioCapture();
    bool ensureAudioPlayback(const QAudioFormat &format);
    void stopAudioCapture();
    void stopAudioPlayback();
    void stopCallMedia();
    void stopCamera();
    void setVideoLabelImage(VideoFrameWidget *widget, const QImage &image);
    void refreshVideoLabels();
    void reportCameraIssue(const QString &message, bool showDialog = false);
    void reportAudioInputIssue(const QString &message);
    void reportAudioOutputIssue(const QString &message);

    Ui::MainWindow *m_ui = nullptr;
    PeerManager *m_manager = nullptr;
    QHash<QString, PeerInfo> m_peers;
    QHash<QString, GroupInfo> m_groups;
    QHash<QString, QStringList> m_history;
    QString m_activeCallPeerId;

    QMediaDevices *m_mediaDevices = nullptr;
    QCamera *m_camera = nullptr;
    QMediaCaptureSession m_captureSession;
    QVideoSink *m_videoSink = nullptr;
    QTimer *m_videoRefreshTimer = nullptr;
    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_audioInputDevice = nullptr;
    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_audioOutputDevice = nullptr;
    bool m_cameraErrorDialogVisible = false;
    QAudioFormat m_audioInputFormat;
    QAudioFormat m_audioOutputFormat;
    QElapsedTimer m_frameLimiter;
    QElapsedTimer m_frameWarningLimiter;
    QElapsedTimer m_localFrameLogTimer;
    QImage m_lastLocalFrame;
    QImage m_lastRemoteFrame;
    QImage m_pendingLocalFrame;
    QImage m_pendingRemoteFrame;
};
