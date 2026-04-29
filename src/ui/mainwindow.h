#pragma once

#include <QDateTime>
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QResizeEvent>

#include "model/chattypes.h"
#include "model/peerinfo.h"

class OpenCvCameraThread;
class PeerManager;
class QAudioSink;
class QAudioSource;
class QCameraDevice;
class QCameraFormat;
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
class QThread;
class QLabel;
class VideoFrameWidget;
class VideoEncodeWorker;
namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    void encodeVideoFrameRequested(const QString &peerId, const QImage &frame);

private slots:
    void applyDisplayName();
    void applyCameraSelection();
    void applyReceiveOnlySetting(bool enabled);
    void applyReceiveRemoteVideoOnlySetting(bool enabled);
    void triggerPeerDiscovery();
    void addManualPeer();
    void sendTextMessage();
    void sendFileToPeer();
    void selectDownloadDirectory();
    void showPeerContextMenu(const QPoint &position);
    void removeSelectedPeer();
    void manageHiddenPeers();
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
    void onRemoteVideoSendingChanged(const QString &peerId, bool enabled);
    void onRemoteVideoFrameReceived(const QString &peerId, const QImage &frame);
    void onRemoteAudioChunkReceived(const QString &peerId, const QByteArray &audioData, int sampleRate, int channelCount, int sampleFormat);
    void onEncodedVideoFrame(const QString &peerId,
                             const QByteArray &encodedFrame,
                             const QString &imageFormat,
                             const QSize &frameSize);
    void processLocalFrame(const QImage &frame);
    void readLocalAudioInput();
    void flushVideoFrames();
    void updateActionState();
    void onVideoInputsChanged();
    void onAudioInputsChanged();
    void onAudioOutputsChanged();
    void onCameraErrorOccurred(const QString &message);
    void onCameraActiveChanged(bool active);
    void onCameraCaptureFormatResolved(const QSize &resolution,
                                       double frameRate,
                                       const QString &backendName);
    void onAudioSourceStateChanged();
    void onAudioSinkStateChanged();

private:
    void setupUi();
    void connectUi();
    void appendHistoryLine(const QString &conversationKey, const QString &line);
    void loadConversationState();
    void saveConversationState() const;
    void scheduleConversationStateSave();
    void updateTranscriptView(const QString &conversationKey, bool forceFullRefresh = false);
    bool sendFilesToPeer(const QStringList &filePaths);
    void clearCallFrames();
    void clearLocalCallFrame();
    void clearRemoteCallFrame();
    void updateCallStatusLabel();
    void populateCameraDevices();
    void populateCameraFormats(const QString &preferredFormatKey = {});
    bool prepareCallCamera();
    bool shouldSendLocalVideo() const;
    QString currentPeerId() const;
    QString currentGroupId() const;
    QString currentConversationKey() const;
    QString peerName(const QString &peerId) const;
    QString groupName(const QString &groupId) const;
    QString peerListLabel(const PeerInfo &peer) const;
    QString groupListLabel(const GroupInfo &group) const;
    QString selectedCameraDevicePath() const;
    int selectedCameraDeviceIndex() const;
    QCameraDevice selectedCameraDevice() const;
    QCameraFormat selectedCameraFormat(const QCameraDevice &device) const;
    void selectGroup(const QString &groupId);
    void markConversationAsRead(const QString &conversationKey);
    void updateUnreadCount(const QString &conversationKey);
    void updatePeerListLabels();
    void updateGroupListLabels();
    void updateCameraFormatHint();
    bool ensureCameraRunning();
    bool startAudioCapture();
    bool ensureAudioPlayback(const QAudioFormat &format);
    void stopAudioCapture();
    void stopAudioPlayback();
    void stopCallMedia();
    void stopCamera();
    void setVideoLabelImage(VideoFrameWidget *widget, const QImage &image);
    void refreshVideoLabels();
    void showStatusMessage(const QString &message, int timeoutMs = 3000);
    void reportCameraIssue(const QString &message, bool showDialog = false);
    void reportAudioInputIssue(const QString &message);
    void reportAudioOutputIssue(const QString &message);

    Ui::MainWindow *m_ui = nullptr;
    PeerManager *m_manager = nullptr;
    QHash<QString, PeerInfo> m_peers;
    QHash<QString, GroupInfo> m_groups;
    QHash<QString, QStringList> m_history;
    QHash<QString, int> m_unreadCounts;
    QString m_activeCallPeerId;
    QString m_displayedConversationKey;
    int m_displayedConversationLineCount = 0;

    QMediaDevices *m_mediaDevices = nullptr;
    OpenCvCameraThread *m_cameraThread = nullptr;
    QTimer *m_videoRefreshTimer = nullptr;
    QTimer *m_stateSaveTimer = nullptr;
    QThread *m_videoEncodeThread = nullptr;
    VideoEncodeWorker *m_videoEncodeWorker = nullptr;
    QAudioSource *m_audioSource = nullptr;
    QIODevice *m_audioInputDevice = nullptr;
    QAudioSink *m_audioSink = nullptr;
    QIODevice *m_audioOutputDevice = nullptr;
    bool m_cameraErrorDialogVisible = false;
    bool m_cameraActive = false;
    QAudioFormat m_audioInputFormat;
    QAudioFormat m_audioOutputFormat;
    QElapsedTimer m_frameLimiter;
    QElapsedTimer m_frameWarningLimiter;
    QElapsedTimer m_localFrameLogTimer;
    QImage m_lastLocalFrame;
    QImage m_lastRemoteFrame;
    QImage m_pendingLocalFrame;
    QImage m_pendingRemoteFrame;
    QImage m_pendingVideoEncodeFrame;
    QString m_pendingVideoPeerId;
    bool m_videoEncodeInFlight = false;
    int m_filteredLocalFrameCount = 0;
    QSize m_activeCameraResolution;
    double m_activeCameraFrameRate = 0.0;
    QString m_cameraBackendName;
    QString m_lastStatusMessage;
    QElapsedTimer m_statusMessageLimiter;
};
