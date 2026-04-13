#include "mainwindow.h"

#include <QAbstractItemView>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QCamera>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMediaDevices>
#include <QMessageBox>
#include <QIODevice>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextOption>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWidget>

#include "network/peermanager.h"

namespace {

QString peerConversationKey(const QString &peerId)
{
    return QStringLiteral("peer:%1").arg(peerId);
}

QString groupConversationKey(const QString &groupId)
{
    return QStringLiteral("group:%1").arg(groupId);
}

QString timestampLabel(const QDateTime &timestamp)
{
    return timestamp.toString(QStringLiteral("HH:mm:ss"));
}

QAudioFormat formatFromWire(int sampleRate, int channelCount, int sampleFormat)
{
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channelCount);
    format.setSampleFormat(static_cast<QAudioFormat::SampleFormat>(sampleFormat));
    return format;
}

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_manager(new PeerManager(this))
    , m_videoSink(new QVideoSink(this))
{
    buildUi();

    QSettings settings;
    const QString savedName = settings.value(QStringLiteral("displayName"), m_manager->displayName()).toString();
    m_nameEdit->setText(savedName);
    m_manager->setDisplayName(savedName);

    connect(m_manager, &PeerManager::peersChanged, this, &MainWindow::onPeersChanged);
    connect(m_manager, &PeerManager::groupsChanged, this, &MainWindow::onGroupsChanged);
    connect(m_manager, &PeerManager::directMessageReceived, this, &MainWindow::onDirectMessageReceived);
    connect(m_manager, &PeerManager::groupMessageReceived, this, &MainWindow::onGroupMessageReceived);
    connect(m_manager, &PeerManager::fileReceived, this, &MainWindow::onFileReceived);
    connect(m_manager, &PeerManager::noticeRaised, this, &MainWindow::onNoticeRaised);
    connect(m_manager, &PeerManager::callInvitationReceived, this, &MainWindow::onCallInvitationReceived);
    connect(m_manager, &PeerManager::callAccepted, this, &MainWindow::onCallAccepted);
    connect(m_manager, &PeerManager::callEnded, this, &MainWindow::onCallEnded);
    connect(m_manager, &PeerManager::remoteVideoFrameReceived, this, &MainWindow::onRemoteVideoFrameReceived);
    connect(m_manager, &PeerManager::remoteAudioChunkReceived, this, &MainWindow::onRemoteAudioChunkReceived);
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this, &MainWindow::processLocalFrame);

    onPeersChanged(m_manager->peers());
    onGroupsChanged(m_manager->groups());

    setWindowTitle(QStringLiteral("LanLinkChat - 局域网聊天"));
    resize(1280, 820);
    statusBar()->showMessage(QStringLiteral("已启动，等待局域网节点发现。"));
}

MainWindow::~MainWindow()
{
    stopCallMedia();
}

void MainWindow::applyDisplayName()
{
    const QString name = m_nameEdit->text().trimmed();
    m_manager->setDisplayName(name);
    m_nameEdit->setText(m_manager->displayName());

    QSettings settings;
    settings.setValue(QStringLiteral("displayName"), m_manager->displayName());
    statusBar()->showMessage(QStringLiteral("显示名已更新。"), 3000);
}

void MainWindow::triggerPeerDiscovery()
{
    m_manager->refreshDiscovery();
}

void MainWindow::sendTextMessage()
{
    const QString text = m_messageEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }

    if (!currentPeerId().isEmpty()) {
        const QString peerId = currentPeerId();
        m_manager->sendDirectMessage(peerId, text);
        appendHistoryLine(peerConversationKey(peerId),
                          QStringLiteral("[%1] 我: %2").arg(timestampLabel(QDateTime::currentDateTime()), text));
    } else if (!currentGroupId().isEmpty()) {
        const QString groupId = currentGroupId();
        m_manager->sendGroupMessage(groupId, text);
        appendHistoryLine(groupConversationKey(groupId),
                          QStringLiteral("[%1] 我: %2").arg(timestampLabel(QDateTime::currentDateTime()), text));
    }

    m_messageEdit->clear();
    refreshConversationView();
}

void MainWindow::sendFileToPeer()
{
    const QString peerId = currentPeerId();
    if (peerId.isEmpty()) {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty()) {
        return;
    }

    if (m_manager->sendFile(peerId, path)) {
        appendHistoryLine(peerConversationKey(peerId),
                          QStringLiteral("[%1] 我发送了文件: %2")
                              .arg(timestampLabel(QDateTime::currentDateTime()),
                                   QFileInfo(path).fileName()));
        refreshConversationView();
    }
}

void MainWindow::createGroup()
{
    if (m_peers.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("当前没有可加入群聊的在线或已发现节点。"));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("创建群聊"));

    auto *layout = new QVBoxLayout(&dialog);
    auto *nameEdit = new QLineEdit(&dialog);
    nameEdit->setPlaceholderText(QStringLiteral("例如：研发组 / 演示群"));
    auto *memberList = new QListWidget(&dialog);
    memberList->setSelectionMode(QAbstractItemView::MultiSelection);

    for (const PeerInfo &peer : m_peers) {
        auto *item = new QListWidgetItem(QStringLiteral("%1 [%2]").arg(peer.name, peer.online ? QStringLiteral("在线") : QStringLiteral("离线")), memberList);
        item->setData(Qt::UserRole, peer.id);
        if (peer.online) {
            item->setSelected(true);
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    layout->addWidget(new QLabel(QStringLiteral("群名称"), &dialog));
    layout->addWidget(nameEdit);
    layout->addWidget(new QLabel(QStringLiteral("选择成员"), &dialog));
    layout->addWidget(memberList);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QStringList members;
    for (QListWidgetItem *item : memberList->selectedItems()) {
        members.append(item->data(Qt::UserRole).toString());
    }

    GroupInfo group = m_manager->createGroup(nameEdit->text(), members);
    appendHistoryLine(groupConversationKey(group.id),
                      QStringLiteral("[%1] 群聊已创建，成员数: %2")
                          .arg(timestampLabel(QDateTime::currentDateTime()))
                          .arg(group.members.size()));
    selectGroup(group.id);
}

void MainWindow::startVideoCall()
{
    const QString peerId = currentPeerId();
    if (peerId.isEmpty()) {
        return;
    }

    if (!ensureCameraRunning()) {
        return;
    }
    startAudioCapture();
    m_activeCallPeerId = peerId;
    m_manager->inviteToCall(peerId);
    if (m_contentTabs) {
        m_contentTabs->setCurrentIndex(1);
    }
    if (m_callStatusLabel) {
        m_callStatusLabel->setText(QStringLiteral("正在呼叫 %1").arg(peerName(peerId)));
    }
    statusBar()->showMessage(QStringLiteral("已向 %1 发起音视频请求。").arg(peerName(peerId)), 4000);
    updateActionState();
}

void MainWindow::endVideoCall()
{
    if (m_activeCallPeerId.isEmpty()) {
        return;
    }

    m_manager->endCall(m_activeCallPeerId);
    statusBar()->showMessage(QStringLiteral("音视频通话已结束。"), 3000);
    m_activeCallPeerId.clear();
    m_remoteVideoLabel->setText(QStringLiteral("远端画面"));
    m_localVideoLabel->setText(QStringLiteral("本地画面"));
    if (m_callStatusLabel) {
        m_callStatusLabel->setText(QStringLiteral("未在通话中"));
    }
    stopCallMedia();
    updateActionState();
}

void MainWindow::refreshConversationView()
{
    m_transcript->setPlainText(m_history.value(currentConversationKey()).join(QStringLiteral("\n")));

    if (!currentPeerId().isEmpty()) {
        m_conversationTitle->setText(QStringLiteral("单聊 - %1").arg(peerName(currentPeerId())));
    } else if (!currentGroupId().isEmpty()) {
        m_conversationTitle->setText(QStringLiteral("群聊 - %1").arg(groupName(currentGroupId())));
    } else {
        m_conversationTitle->setText(QStringLiteral("未选择会话"));
    }

    if (m_callStatusLabel) {
        if (!m_activeCallPeerId.isEmpty()) {
            m_callStatusLabel->setText(QStringLiteral("正在与 %1 音视频通话").arg(peerName(m_activeCallPeerId)));
        } else if (!currentPeerId().isEmpty()) {
            m_callStatusLabel->setText(QStringLiteral("当前通话目标: %1").arg(peerName(currentPeerId())));
        } else {
            m_callStatusLabel->setText(QStringLiteral("请选择一个联系人发起音视频通话"));
        }
    }

    updateActionState();
}

void MainWindow::onPeersChanged(const QList<PeerInfo> &peers)
{
    const QString selectedPeerId = currentPeerId();
    const bool messageEditHadFocus = m_messageEdit->hasFocus();
    m_peers.clear();
    {
        const QSignalBlocker blocker(m_peerList);
        m_peerList->clear();

        for (const PeerInfo &peer : peers) {
            m_peers.insert(peer.id, peer);
            auto *item = new QListWidgetItem(QStringLiteral("%1  %2  %3:%4")
                                                 .arg(peer.name,
                                                      peer.online ? QStringLiteral("[在线]") : QStringLiteral("[离线]"),
                                                      peer.address.toString(),
                                                      QString::number(peer.port)),
                                             m_peerList);
            item->setData(Qt::UserRole, peer.id);
            if (peer.id == selectedPeerId) {
                m_peerList->setCurrentItem(item);
            }
        }
    }

    refreshConversationView();
    if (messageEditHadFocus && m_messageEdit->isEnabled()) {
        m_messageEdit->setFocus();
    }
}

void MainWindow::onGroupsChanged(const QList<GroupInfo> &groups)
{
    const QString selectedGroupId = currentGroupId();
    const bool messageEditHadFocus = m_messageEdit->hasFocus();
    m_groups.clear();
    {
        const QSignalBlocker blocker(m_groupList);
        m_groupList->clear();

        for (const GroupInfo &group : groups) {
            m_groups.insert(group.id, group);
            auto *item = new QListWidgetItem(QStringLiteral("%1 (%2人)").arg(group.name).arg(group.members.size()), m_groupList);
            item->setData(Qt::UserRole, group.id);
            if (group.id == selectedGroupId) {
                m_groupList->setCurrentItem(item);
            }
        }
    }

    refreshConversationView();
    if (messageEditHadFocus && m_messageEdit->isEnabled()) {
        m_messageEdit->setFocus();
    }
}

void MainWindow::onDirectMessageReceived(const QString &peerId, const QString &text, const QDateTime &timestamp)
{
    appendHistoryLine(peerConversationKey(peerId),
                      QStringLiteral("[%1] %2: %3").arg(timestampLabel(timestamp), peerName(peerId), text));
    refreshConversationView();
}

void MainWindow::onGroupMessageReceived(const QString &groupId, const QString &peerId, const QString &text, const QDateTime &timestamp)
{
    appendHistoryLine(groupConversationKey(groupId),
                      QStringLiteral("[%1] %2: %3").arg(timestampLabel(timestamp), peerName(peerId), text));
    refreshConversationView();
}

void MainWindow::onFileReceived(const QString &peerId, const QString &fileName, const QString &savedPath)
{
    appendHistoryLine(peerConversationKey(peerId),
                      QStringLiteral("[%1] %2 发送了文件: %3\n已保存到: %4")
                          .arg(timestampLabel(QDateTime::currentDateTime()), peerName(peerId), fileName, savedPath));
    refreshConversationView();
    statusBar()->showMessage(QStringLiteral("收到文件 %1").arg(fileName), 5000);
}

void MainWindow::onNoticeRaised(const QString &message)
{
    statusBar()->showMessage(message, 5000);
}

void MainWindow::onCallInvitationReceived(const QString &peerId)
{
    const auto result = QMessageBox::question(this,
                                              QStringLiteral("音视频通话"),
                                              QStringLiteral("%1 邀请你开始音视频通话，是否接受？").arg(peerName(peerId)));
    if (result == QMessageBox::Yes) {
        if (!ensureCameraRunning()) {
            return;
        }
        startAudioCapture();
        m_activeCallPeerId = peerId;
        m_manager->acceptCall(peerId);
        if (m_contentTabs) {
            m_contentTabs->setCurrentIndex(1);
        }
        if (m_callStatusLabel) {
            m_callStatusLabel->setText(QStringLiteral("正在与 %1 音视频通话").arg(peerName(peerId)));
        }
        statusBar()->showMessage(QStringLiteral("已接受 %1 的音视频请求。").arg(peerName(peerId)), 4000);
        updateActionState();
    }
}

void MainWindow::onCallAccepted(const QString &peerId)
{
    if (m_activeCallPeerId != peerId) {
        m_activeCallPeerId = peerId;
    }
    if (!ensureCameraRunning()) {
        return;
    }
    startAudioCapture();
    if (m_contentTabs) {
        m_contentTabs->setCurrentIndex(1);
    }
    if (m_callStatusLabel) {
        m_callStatusLabel->setText(QStringLiteral("正在与 %1 音视频通话").arg(peerName(peerId)));
    }
    statusBar()->showMessage(QStringLiteral("%1 已接受音视频请求。").arg(peerName(peerId)), 4000);
    updateActionState();
}

void MainWindow::onCallEnded(const QString &peerId)
{
    if (m_activeCallPeerId == peerId) {
        m_activeCallPeerId.clear();
        m_remoteVideoLabel->setText(QStringLiteral("远端画面"));
        m_localVideoLabel->setText(QStringLiteral("本地画面"));
        if (m_callStatusLabel) {
            m_callStatusLabel->setText(QStringLiteral("未在通话中"));
        }
        stopCallMedia();
    }
    statusBar()->showMessage(QStringLiteral("%1 已结束音视频通话。").arg(peerName(peerId)), 4000);
    updateActionState();
}

void MainWindow::onRemoteVideoFrameReceived(const QString &peerId, const QImage &frame)
{
    if (m_activeCallPeerId.isEmpty()) {
        m_activeCallPeerId = peerId;
        if (m_contentTabs) {
            m_contentTabs->setCurrentIndex(1);
        }
    }
    if (m_activeCallPeerId == peerId) {
        setVideoLabelImage(m_remoteVideoLabel, frame);
        if (m_callStatusLabel) {
            m_callStatusLabel->setText(QStringLiteral("正在与 %1 音视频通话").arg(peerName(peerId)));
        }
    }
}

void MainWindow::onRemoteAudioChunkReceived(const QString &peerId,
                                            const QByteArray &audioData,
                                            int sampleRate,
                                            int channelCount,
                                            int sampleFormat)
{
    if (audioData.isEmpty()) {
        return;
    }

    if (m_activeCallPeerId.isEmpty()) {
        m_activeCallPeerId = peerId;
    }

    if (m_activeCallPeerId != peerId) {
        return;
    }

    const QAudioFormat format = formatFromWire(sampleRate, channelCount, sampleFormat);
    if (!ensureAudioPlayback(format) || !m_audioOutputDevice) {
        return;
    }

    m_audioOutputDevice->write(audioData);
}

void MainWindow::processLocalFrame(const QVideoFrame &frame)
{
    const QImage image = frame.toImage();
    if (image.isNull()) {
        return;
    }

    setVideoLabelImage(m_localVideoLabel, image);

    if (!m_activeCallPeerId.isEmpty()) {
        if (!m_frameLimiter.isValid() || m_frameLimiter.elapsed() >= 100) {
            m_manager->sendVideoFrame(m_activeCallPeerId, image);
            m_frameLimiter.restart();
        }
    }
}

void MainWindow::readLocalAudioInput()
{
    if (!m_audioInputDevice || m_activeCallPeerId.isEmpty()) {
        return;
    }

    while (m_audioInputDevice->bytesAvailable() > 0) {
        const QByteArray audioData = m_audioInputDevice->read(4096);
        if (audioData.isEmpty()) {
            break;
        }

        m_manager->sendAudioChunk(m_activeCallPeerId,
                                  audioData,
                                  m_audioInputFormat.sampleRate(),
                                  m_audioInputFormat.channelCount(),
                                  static_cast<int>(m_audioInputFormat.sampleFormat()));
    }
}

void MainWindow::updateActionState()
{
    const bool hasPeer = !currentPeerId().isEmpty();
    const bool hasGroup = !currentGroupId().isEmpty();
    const bool hasConversation = hasPeer || hasGroup;

    m_sendButton->setEnabled(hasConversation);
    m_messageEdit->setEnabled(hasConversation);
    m_fileButton->setEnabled(hasPeer);
    m_videoButton->setEnabled(hasPeer && m_activeCallPeerId.isEmpty());
    m_hangupButton->setEnabled(!m_activeCallPeerId.isEmpty());
}

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *rootLayout = new QHBoxLayout(central);
    auto *splitter = new QSplitter(Qt::Horizontal, central);
    rootLayout->addWidget(splitter);

    auto *sidebar = new QWidget(splitter);
    auto *sidebarLayout = new QVBoxLayout(sidebar);

    auto *nameRow = new QHBoxLayout();
    m_nameEdit = new QLineEdit(sidebar);
    m_nameEdit->setPlaceholderText(QStringLiteral("我的显示名"));
    auto *nameButton = new QPushButton(QStringLiteral("更新"), sidebar);
    nameRow->addWidget(m_nameEdit);
    nameRow->addWidget(nameButton);

    m_groupButton = new QPushButton(QStringLiteral("创建群聊"), sidebar);
    m_refreshPeersButton = new QPushButton(QStringLiteral("手动发现"), sidebar);
    m_sidebarTabs = new QTabWidget(sidebar);
    m_peerList = new QListWidget(m_sidebarTabs);
    m_groupList = new QListWidget(m_sidebarTabs);
    m_sidebarTabs->addTab(m_peerList, QStringLiteral("联系人"));
    m_sidebarTabs->addTab(m_groupList, QStringLiteral("群聊"));

    sidebarLayout->addLayout(nameRow);
    sidebarLayout->addWidget(m_groupButton);
    sidebarLayout->addWidget(m_refreshPeersButton);
    sidebarLayout->addWidget(m_sidebarTabs);

    auto *content = new QWidget(splitter);
    auto *contentLayout = new QVBoxLayout(content);

    m_contentTabs = new QTabWidget(content);

    auto *chatPage = new QWidget(m_contentTabs);
    auto *chatLayout = new QVBoxLayout(chatPage);
    m_conversationTitle = new QLabel(QStringLiteral("未选择会话"), chatPage);
    m_conversationTitle->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));

    m_transcript = new QPlainTextEdit(chatPage);
    m_transcript->setReadOnly(true);
    m_transcript->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    auto *chatActionRow = new QHBoxLayout();
    m_fileButton = new QPushButton(QStringLiteral("发送文件"), chatPage);
    chatActionRow->addWidget(m_fileButton);
    chatActionRow->addStretch();

    auto *messageRow = new QHBoxLayout();
    m_messageEdit = new QLineEdit(chatPage);
    m_messageEdit->setPlaceholderText(QStringLiteral("输入消息内容"));
    m_sendButton = new QPushButton(QStringLiteral("发送"), chatPage);
    messageRow->addWidget(m_messageEdit);
    messageRow->addWidget(m_sendButton);

    chatLayout->addWidget(m_conversationTitle);
    chatLayout->addWidget(m_transcript, 1);
    chatLayout->addLayout(chatActionRow);
    chatLayout->addLayout(messageRow);

    auto *callPage = new QWidget(m_contentTabs);
    auto *callLayout = new QVBoxLayout(callPage);
    m_callStatusLabel = new QLabel(QStringLiteral("请选择一个联系人发起音视频通话"), callPage);
    m_callStatusLabel->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));

    auto *videoRow = new QHBoxLayout();
    m_localVideoLabel = new QLabel(QStringLiteral("本地画面"), callPage);
    m_remoteVideoLabel = new QLabel(QStringLiteral("远端画面"), callPage);
    for (QLabel *label : {m_localVideoLabel, m_remoteVideoLabel}) {
        label->setMinimumSize(320, 240);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("background:#111;color:#ddd;border:1px solid #444;"));
        videoRow->addWidget(label);
    }

    auto *callActionRow = new QHBoxLayout();
    m_videoButton = new QPushButton(QStringLiteral("发起音视频"), callPage);
    m_hangupButton = new QPushButton(QStringLiteral("挂断"), callPage);
    callActionRow->addWidget(m_videoButton);
    callActionRow->addWidget(m_hangupButton);
    callActionRow->addStretch();

    callLayout->addWidget(m_callStatusLabel);
    callLayout->addLayout(videoRow, 1);
    callLayout->addLayout(callActionRow);

    m_contentTabs->addTab(chatPage, QStringLiteral("聊天"));
    m_contentTabs->addTab(callPage, QStringLiteral("音视频"));
    contentLayout->addWidget(m_contentTabs);

    splitter->addWidget(sidebar);
    splitter->addWidget(content);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(central);

    connect(nameButton, &QPushButton::clicked, this, &MainWindow::applyDisplayName);
    connect(m_refreshPeersButton, &QPushButton::clicked, this, &MainWindow::triggerPeerDiscovery);
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::sendTextMessage);
    connect(m_messageEdit, &QLineEdit::returnPressed, this, &MainWindow::sendTextMessage);
    connect(m_fileButton, &QPushButton::clicked, this, &MainWindow::sendFileToPeer);
    connect(m_groupButton, &QPushButton::clicked, this, &MainWindow::createGroup);
    connect(m_videoButton, &QPushButton::clicked, this, &MainWindow::startVideoCall);
    connect(m_hangupButton, &QPushButton::clicked, this, &MainWindow::endVideoCall);
    connect(m_peerList, &QListWidget::currentItemChanged, this, &MainWindow::refreshConversationView);
    connect(m_groupList, &QListWidget::currentItemChanged, this, &MainWindow::refreshConversationView);
    connect(m_sidebarTabs, &QTabWidget::currentChanged, this, &MainWindow::refreshConversationView);

    updateActionState();
}

void MainWindow::appendHistoryLine(const QString &conversationKey, const QString &line)
{
    if (conversationKey.isEmpty()) {
        return;
    }
    m_history[conversationKey].append(line);
}

QString MainWindow::currentPeerId() const
{
    if (m_sidebarTabs->currentWidget() != m_peerList || !m_peerList->currentItem()) {
        return {};
    }
    return m_peerList->currentItem()->data(Qt::UserRole).toString();
}

QString MainWindow::currentGroupId() const
{
    if (m_sidebarTabs->currentWidget() != m_groupList || !m_groupList->currentItem()) {
        return {};
    }
    return m_groupList->currentItem()->data(Qt::UserRole).toString();
}

QString MainWindow::currentConversationKey() const
{
    if (!currentPeerId().isEmpty()) {
        return peerConversationKey(currentPeerId());
    }
    if (!currentGroupId().isEmpty()) {
        return groupConversationKey(currentGroupId());
    }
    return {};
}

QString MainWindow::peerName(const QString &peerId) const
{
    return m_peers.contains(peerId) ? m_peers.value(peerId).name : peerId;
}

QString MainWindow::groupName(const QString &groupId) const
{
    return m_groups.contains(groupId) ? m_groups.value(groupId).name : groupId;
}

void MainWindow::selectGroup(const QString &groupId)
{
    m_sidebarTabs->setCurrentWidget(m_groupList);
    for (int row = 0; row < m_groupList->count(); ++row) {
        QListWidgetItem *item = m_groupList->item(row);
        if (item->data(Qt::UserRole).toString() == groupId) {
            m_groupList->setCurrentItem(item);
            break;
        }
    }
    refreshConversationView();
}

bool MainWindow::ensureCameraRunning()
{
    if (m_camera) {
        return true;
    }

    if (QMediaDevices::defaultVideoInput().isNull()) {
        QMessageBox::warning(this, QStringLiteral("摄像头不可用"), QStringLiteral("当前系统没有检测到可用摄像头。"));
        return false;
    }

    m_camera = new QCamera(QMediaDevices::defaultVideoInput(), this);
    m_captureSession.setCamera(m_camera);
    m_captureSession.setVideoSink(m_videoSink);
    m_camera->start();
    return true;
}

bool MainWindow::startAudioCapture()
{
    if (m_audioSource && m_audioInputDevice) {
        return true;
    }

    const QAudioDevice inputDevice = QMediaDevices::defaultAudioInput();
    if (inputDevice.isNull()) {
        statusBar()->showMessage(QStringLiteral("未检测到麦克风，将仅发送视频。"), 4000);
        return false;
    }

    m_audioInputFormat = inputDevice.preferredFormat();
    if (m_audioInputFormat.sampleFormat() == QAudioFormat::Unknown) {
        m_audioInputFormat.setSampleRate(48000);
        m_audioInputFormat.setChannelCount(1);
        m_audioInputFormat.setSampleFormat(QAudioFormat::Int16);
    }

    if (!inputDevice.isFormatSupported(m_audioInputFormat)) {
        statusBar()->showMessage(QStringLiteral("当前麦克风格式不受支持，将仅发送视频。"), 4000);
        return false;
    }

    m_audioSource = new QAudioSource(inputDevice, m_audioInputFormat, this);
    m_audioSource->setBufferSize(16 * 1024);
    m_audioInputDevice = m_audioSource->start();
    if (!m_audioInputDevice) {
        delete m_audioSource;
        m_audioSource = nullptr;
        statusBar()->showMessage(QStringLiteral("麦克风启动失败，将仅发送视频。"), 4000);
        return false;
    }

    connect(m_audioInputDevice, &QIODevice::readyRead, this, &MainWindow::readLocalAudioInput);
    return true;
}

bool MainWindow::ensureAudioPlayback(const QAudioFormat &format)
{
    if (m_audioSink && m_audioOutputDevice && m_audioOutputFormat == format) {
        return true;
    }

    stopAudioPlayback();

    const QAudioDevice outputDevice = QMediaDevices::defaultAudioOutput();
    if (outputDevice.isNull()) {
        statusBar()->showMessage(QStringLiteral("未检测到扬声器或耳机，无法播放对端语音。"), 4000);
        return false;
    }

    if (!outputDevice.isFormatSupported(format)) {
        statusBar()->showMessage(QStringLiteral("当前扬声器不支持对端音频格式，语音已忽略。"), 4000);
        return false;
    }

    m_audioOutputFormat = format;
    m_audioSink = new QAudioSink(outputDevice, m_audioOutputFormat, this);
    m_audioSink->setBufferSize(32 * 1024);
    m_audioOutputDevice = m_audioSink->start();
    if (!m_audioOutputDevice) {
        delete m_audioSink;
        m_audioSink = nullptr;
        statusBar()->showMessage(QStringLiteral("扬声器启动失败，无法播放对端语音。"), 4000);
        return false;
    }

    return true;
}

void MainWindow::stopAudioCapture()
{
    if (m_audioInputDevice) {
        m_audioInputDevice->disconnect(this);
        m_audioInputDevice = nullptr;
    }

    if (!m_audioSource) {
        return;
    }

    m_audioSource->stop();
    m_audioSource->deleteLater();
    m_audioSource = nullptr;
    m_audioInputFormat = QAudioFormat();
}

void MainWindow::stopAudioPlayback()
{
    m_audioOutputDevice = nullptr;

    if (!m_audioSink) {
        return;
    }

    m_audioSink->stop();
    m_audioSink->deleteLater();
    m_audioSink = nullptr;
    m_audioOutputFormat = QAudioFormat();
}

void MainWindow::stopCallMedia()
{
    stopAudioCapture();
    stopAudioPlayback();
    stopCamera();
}

void MainWindow::stopCamera()
{
    if (!m_camera) {
        return;
    }

    m_camera->stop();
    m_camera->deleteLater();
    m_camera = nullptr;
    m_localVideoLabel->setText(QStringLiteral("本地画面"));
}

void MainWindow::setVideoLabelImage(QLabel *label, const QImage &image)
{
    label->setPixmap(QPixmap::fromImage(image).scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
