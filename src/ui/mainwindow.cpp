#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QAbstractItemView>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QCamera>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QListWidgetItem>
#include <QMediaDevices>
#include <QMessageBox>
#include <QIODevice>
#include <QKeySequence>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSettings>
#include <QStatusBar>
#include <QTextOption>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <limits>

#include "network/peermanager.h"
#include "ui/videoframewidget.h"

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

int localVideoSendIntervalMs()
{
#ifdef Q_OS_LINUX
    return 150;
#else
    return 100;
#endif
}

int scoreCameraFormat(const QCameraFormat &format)
{
    if (format.pixelFormat() == QVideoFrameFormat::Format_Invalid) {
        return std::numeric_limits<int>::min();
    }

    int score = 0;
    switch (format.pixelFormat()) {
    case QVideoFrameFormat::Format_YUYV:
    case QVideoFrameFormat::Format_UYVY:
        score += 2000;
        break;
    case QVideoFrameFormat::Format_NV12:
    case QVideoFrameFormat::Format_NV21:
    case QVideoFrameFormat::Format_YUV420P:
    case QVideoFrameFormat::Format_YV12:
        score += 1500;
        break;
    case QVideoFrameFormat::Format_Jpeg:
        score += 500;
        break;
    default:
        score += 800;
        break;
    }

    const QSize resolution = format.resolution();
    score -= qAbs(resolution.width() - 640);
    score -= qAbs(resolution.height() - 360);

    if (format.maxFrameRate() >= 30.0) {
        score += 300;
    }

    return score;
}

QCameraFormat preferredCameraFormat(const QCameraDevice &device)
{
    const QList<QCameraFormat> formats = device.videoFormats();
    if (formats.isEmpty()) {
        return {};
    }

    QCameraFormat best = formats.constFirst();
    int bestScore = scoreCameraFormat(best);
    for (const QCameraFormat &candidate : formats) {
        const int candidateScore = scoreCameraFormat(candidate);
        if (candidateScore > bestScore) {
            best = candidate;
            bestScore = candidateScore;
        }
    }

    return best;
}

QAudioFormat formatFromWire(int sampleRate, int channelCount, int sampleFormat)
{
    QAudioFormat format;
    format.setSampleRate(sampleRate);
    format.setChannelCount(channelCount);
    format.setSampleFormat(static_cast<QAudioFormat::SampleFormat>(sampleFormat));
    return format;
}

int clampColor(int value)
{
    return qBound(0, value, 255);
}

enum class PackedYuv422Order {
    YUYV,
    UYVY,
    YVYU,
    VYUY
};

QRgb yuvToRgb(int yValue, int uValue, int vValue)
{
    const int c = qMax(0, yValue - 16);
    const int d = uValue - 128;
    const int e = vValue - 128;
    const int r = clampColor((298 * c + 409 * e + 128) >> 8);
    const int g = clampColor((298 * c - 100 * d - 208 * e + 128) >> 8);
    const int b = clampColor((298 * c + 516 * d + 128) >> 8);
    return qRgb(r, g, b);
}

QImage convertPackedYuv422FrameToImage(const QVideoFrame &frame, PackedYuv422Order order)
{
    QVideoFrame mappedFrame(frame);
    if (!mappedFrame.map(QVideoFrame::ReadOnly)) {
        return {};
    }

    const QSize size = mappedFrame.size();
    if (!size.isValid() || size.width() < 2 || size.height() < 1) {
        mappedFrame.unmap();
        return {};
    }

    const uchar *source = mappedFrame.bits(0);
    const int bytesPerLine = mappedFrame.bytesPerLine(0);
    if (!source || bytesPerLine <= 0) {
        mappedFrame.unmap();
        return {};
    }

    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) {
        mappedFrame.unmap();
        return {};
    }

    for (int y = 0; y < size.height(); ++y) {
        const uchar *line = source + y * bytesPerLine;
        QRgb *dest = reinterpret_cast<QRgb *>(image.scanLine(y));

        for (int x = 0; x + 1 < size.width(); x += 2) {
            const int offset = x * 2;
            int y0 = 0;
            int y1 = 0;
            int u = 128;
            int v = 128;

            switch (order) {
            case PackedYuv422Order::YUYV:
                y0 = line[offset + 0];
                u = line[offset + 1];
                y1 = line[offset + 2];
                v = line[offset + 3];
                break;
            case PackedYuv422Order::UYVY:
                u = line[offset + 0];
                y0 = line[offset + 1];
                v = line[offset + 2];
                y1 = line[offset + 3];
                break;
            case PackedYuv422Order::YVYU:
                y0 = line[offset + 0];
                v = line[offset + 1];
                y1 = line[offset + 2];
                u = line[offset + 3];
                break;
            case PackedYuv422Order::VYUY:
                v = line[offset + 0];
                y0 = line[offset + 1];
                u = line[offset + 2];
                y1 = line[offset + 3];
                break;
            }

            dest[x] = yuvToRgb(y0, u, v);
            dest[x + 1] = yuvToRgb(y1, u, v);
        }
    }

    mappedFrame.unmap();
    return image;
}

qint64 imageDifferenceScore(const QImage &candidate, const QImage &reference)
{
    if (candidate.isNull() || reference.isNull()) {
        return std::numeric_limits<qint64>::max();
    }

    const QSize compareSize(80, 45);
    const QImage left = candidate.scaled(compareSize, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                            .convertToFormat(QImage::Format_RGB32);
    const QImage right = reference.scaled(compareSize, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                             .convertToFormat(QImage::Format_RGB32);

    qint64 score = 0;
    for (int y = 0; y < compareSize.height(); ++y) {
        const QRgb *leftLine = reinterpret_cast<const QRgb *>(left.constScanLine(y));
        const QRgb *rightLine = reinterpret_cast<const QRgb *>(right.constScanLine(y));
        for (int x = 0; x < compareSize.width(); ++x) {
            score += qAbs(qRed(leftLine[x]) - qRed(rightLine[x]));
            score += qAbs(qGreen(leftLine[x]) - qGreen(rightLine[x]));
            score += qAbs(qBlue(leftLine[x]) - qBlue(rightLine[x]));
        }
    }

    return score;
}

QImage bestPackedYuv422TransportImage(const QVideoFrame &frame, const QImage &referenceImage)
{
    const QList<PackedYuv422Order> candidates = {
        PackedYuv422Order::YUYV,
        PackedYuv422Order::UYVY,
        PackedYuv422Order::YVYU,
        PackedYuv422Order::VYUY
    };

    QImage bestImage;
    qint64 bestScore = std::numeric_limits<qint64>::max();
    for (PackedYuv422Order candidate : candidates) {
        const QImage image = convertPackedYuv422FrameToImage(frame, candidate).copy();
        const qint64 score = imageDifferenceScore(image, referenceImage);
        if (score < bestScore) {
            bestScore = score;
            bestImage = image;
        }
    }

    return bestImage;
}

QImage convertPlanarYuv420FrameToImage(const QVideoFrame &frame, bool nv21Layout, bool yv12Layout)
{
    QVideoFrame mappedFrame(frame);
    if (!mappedFrame.map(QVideoFrame::ReadOnly)) {
        return {};
    }

    const QSize size = mappedFrame.size();
    if (!size.isValid()) {
        mappedFrame.unmap();
        return {};
    }

    const uchar *yPlane = mappedFrame.bits(0);
    const int yStride = mappedFrame.bytesPerLine(0);
    if (!yPlane || yStride <= 0) {
        mappedFrame.unmap();
        return {};
    }

    const bool isSemiPlanar = mappedFrame.planeCount() >= 2 && mappedFrame.bits(1);
    const uchar *uPlane = nullptr;
    const uchar *vPlane = nullptr;
    int uStride = 0;
    int vStride = 0;
    const uchar *uvPlane = nullptr;
    int uvStride = 0;

    if (isSemiPlanar) {
        uvPlane = mappedFrame.bits(1);
        uvStride = mappedFrame.bytesPerLine(1);
        if (!uvPlane || uvStride <= 0) {
            mappedFrame.unmap();
            return {};
        }
    } else {
        uPlane = mappedFrame.bits(yv12Layout ? 2 : 1);
        vPlane = mappedFrame.bits(yv12Layout ? 1 : 2);
        uStride = mappedFrame.bytesPerLine(yv12Layout ? 2 : 1);
        vStride = mappedFrame.bytesPerLine(yv12Layout ? 1 : 2);
        if (!uPlane || !vPlane || uStride <= 0 || vStride <= 0) {
            mappedFrame.unmap();
            return {};
        }
    }

    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) {
        mappedFrame.unmap();
        return {};
    }

    for (int y = 0; y < size.height(); ++y) {
        const uchar *yRow = yPlane + y * yStride;
        QRgb *dest = reinterpret_cast<QRgb *>(image.scanLine(y));
        const int chromaY = y / 2;

        for (int x = 0; x < size.width(); ++x) {
            const int chromaX = x / 2;
            int u = 128;
            int v = 128;

            if (isSemiPlanar) {
                const uchar *uvRow = uvPlane + chromaY * uvStride;
                const int uvOffset = chromaX * 2;
                u = nv21Layout ? uvRow[uvOffset + 1] : uvRow[uvOffset + 0];
                v = nv21Layout ? uvRow[uvOffset + 0] : uvRow[uvOffset + 1];
            } else {
                u = *(uPlane + chromaY * uStride + chromaX);
                v = *(vPlane + chromaY * vStride + chromaX);
            }

            dest[x] = yuvToRgb(yRow[x], u, v);
        }
    }

    mappedFrame.unmap();
    return image;
}

QImage frameToDisplayImage(const QVideoFrame &frame)
{
    if (const QImage directImage = frame.toImage(); !directImage.isNull()) {
        const QImage rgbImage = directImage.convertToFormat(QImage::Format_RGB32);
        return rgbImage.isNull() ? QImage() : rgbImage.copy();
    }

    switch (frame.pixelFormat()) {
    case QVideoFrameFormat::Format_YUYV:
        return convertPackedYuv422FrameToImage(frame, PackedYuv422Order::YUYV).copy();
    case QVideoFrameFormat::Format_UYVY:
        return convertPackedYuv422FrameToImage(frame, PackedYuv422Order::UYVY).copy();
    case QVideoFrameFormat::Format_NV12:
        return convertPlanarYuv420FrameToImage(frame, false, false).copy();
    case QVideoFrameFormat::Format_NV21:
        return convertPlanarYuv420FrameToImage(frame, true, false).copy();
    case QVideoFrameFormat::Format_YUV420P:
        return convertPlanarYuv420FrameToImage(frame, false, false).copy();
    case QVideoFrameFormat::Format_YV12:
        return convertPlanarYuv420FrameToImage(frame, false, true).copy();
    default:
        return {};
    }
}

QImage frameToTransportImage(const QVideoFrame &frame, const QImage &displayImage)
{
#ifdef Q_OS_LINUX
    switch (frame.pixelFormat()) {
    case QVideoFrameFormat::Format_YUYV:
    case QVideoFrameFormat::Format_UYVY:
        if (!displayImage.isNull()) {
            return bestPackedYuv422TransportImage(frame, displayImage);
        }
        break;
    case QVideoFrameFormat::Format_NV12:
        return convertPlanarYuv420FrameToImage(frame, false, false).copy();
    case QVideoFrameFormat::Format_NV21:
        return convertPlanarYuv420FrameToImage(frame, true, false).copy();
    case QVideoFrameFormat::Format_YUV420P:
        return convertPlanarYuv420FrameToImage(frame, false, false).copy();
    case QVideoFrameFormat::Format_YV12:
        return convertPlanarYuv420FrameToImage(frame, false, true).copy();
    default:
        break;
    }
#endif

    return frameToDisplayImage(frame);
}

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_ui(new Ui::MainWindow)
    , m_manager(new PeerManager(this))
    , m_mediaDevices(new QMediaDevices(this))
    , m_videoSink(new QVideoSink(this))
    , m_videoRefreshTimer(new QTimer(this))
{
    setupUi();
    connectUi();

    QSettings settings;
    const QString savedName = settings.value(QStringLiteral("displayName"), m_manager->displayName()).toString();
    m_ui->nameEdit->setText(savedName);
    m_manager->setDisplayName(savedName);
    populateCameraDevices();

    const QByteArray savedCameraId = settings.value(QStringLiteral("cameraDeviceId")).toByteArray();
    if (m_ui->cameraCombo) {
        for (int index = 0; index < m_ui->cameraCombo->count(); ++index) {
            if (m_ui->cameraCombo->itemData(index).toByteArray() == savedCameraId) {
                m_ui->cameraCombo->setCurrentIndex(index);
                break;
            }
        }
    }
    if (m_ui->receiveOnlyCheck) {
        m_ui->receiveOnlyCheck->setChecked(settings.value(QStringLiteral("receiveOnlyWithoutCamera"), true).toBool());
    }
    if (m_ui->receiveRemoteVideoOnlyCheck) {
        m_ui->receiveRemoteVideoOnlyCheck->setChecked(settings.value(QStringLiteral("receiveRemoteVideoOnly"), false).toBool());
    }

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
    connect(m_videoRefreshTimer, &QTimer::timeout, this, &MainWindow::flushVideoFrames);
    connect(m_mediaDevices, &QMediaDevices::videoInputsChanged, this, &MainWindow::onVideoInputsChanged);
    connect(m_mediaDevices, &QMediaDevices::audioInputsChanged, this, &MainWindow::onAudioInputsChanged);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, &MainWindow::onAudioOutputsChanged);

    m_videoRefreshTimer->setInterval(66);
    m_videoRefreshTimer->start();

    onPeersChanged(m_manager->peers());
    onGroupsChanged(m_manager->groups());

    setWindowTitle(QStringLiteral("LanLinkChat - 局域网聊天"));
    resize(1280, 820);
    statusBar()->showMessage(QStringLiteral("已启动，等待局域网节点发现。"));
}

MainWindow::~MainWindow()
{
    stopCallMedia();
    delete m_ui;
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    refreshVideoLabels();
}

void MainWindow::setupUi()
{
    m_ui->setupUi(this);
    m_ui->mainSplitter->setStretchFactor(1, 1);
    m_ui->conversationTitle->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600;"));
    m_ui->callStatusLabel->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 500; padding: 2px 0;"));
    m_ui->transcript->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_ui->messageEdit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_ui->messageEdit->setMinimumHeight(96);
    m_ui->chatVerticalSplitter->setChildrenCollapsible(false);
    m_ui->chatVerticalSplitter->setStretchFactor(0, 1);
    m_ui->chatVerticalSplitter->setStretchFactor(1, 0);
    m_ui->chatVerticalSplitter->setSizes({560, 160});
    m_ui->localVideoWidget->setPlaceholderText(QStringLiteral("本地画面"));
    m_ui->remoteVideoWidget->setPlaceholderText(QStringLiteral("远端画面"));
}

void MainWindow::connectUi()
{
    connect(m_ui->nameApplyButton, &QPushButton::clicked, this, &MainWindow::applyDisplayName);
    connect(m_ui->refreshPeersButton, &QPushButton::clicked, this, &MainWindow::triggerPeerDiscovery);
    connect(m_ui->sendButton, &QPushButton::clicked, this, &MainWindow::sendTextMessage);
    connect(m_ui->fileButton, &QPushButton::clicked, this, &MainWindow::sendFileToPeer);
    connect(m_ui->groupButton, &QPushButton::clicked, this, &MainWindow::createGroup);
    connect(m_ui->cameraApplyButton, &QPushButton::clicked, this, &MainWindow::applyCameraSelection);
    connect(m_ui->receiveOnlyCheck, &QCheckBox::toggled, this, &MainWindow::applyReceiveOnlySetting);
    connect(m_ui->receiveRemoteVideoOnlyCheck, &QCheckBox::toggled, this, &MainWindow::applyReceiveRemoteVideoOnlySetting);
    connect(m_ui->videoButton, &QPushButton::clicked, this, &MainWindow::startVideoCall);
    connect(m_ui->hangupButton, &QPushButton::clicked, this, &MainWindow::endVideoCall);
    connect(m_ui->peerList, &QListWidget::currentItemChanged, this, &MainWindow::refreshConversationView);
    connect(m_ui->groupList, &QListWidget::currentItemChanged, this, &MainWindow::refreshConversationView);
    connect(m_ui->sidebarTabs, &QTabWidget::currentChanged, this, &MainWindow::refreshConversationView);

    auto *sendShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Return")), m_ui->messageEdit);
    connect(sendShortcut, &QShortcut::activated, this, &MainWindow::sendTextMessage);
    auto *sendShortcutNumPad = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Enter")), m_ui->messageEdit);
    connect(sendShortcutNumPad, &QShortcut::activated, this, &MainWindow::sendTextMessage);
}

void MainWindow::applyDisplayName()
{
    const QString name = m_ui->nameEdit->text().trimmed();
    m_manager->setDisplayName(name);
    m_ui->nameEdit->setText(m_manager->displayName());

    QSettings settings;
    settings.setValue(QStringLiteral("displayName"), m_manager->displayName());
    statusBar()->showMessage(QStringLiteral("显示名已更新。"), 3000);
}

void MainWindow::applyCameraSelection()
{
    if (!m_ui->cameraCombo) {
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("cameraDeviceId"), m_ui->cameraCombo->currentData().toByteArray());

    const QString cameraName = m_ui->cameraCombo->currentText().trimmed();
    if (m_camera) {
        stopCamera();
        if (!m_activeCallPeerId.isEmpty()) {
            ensureCameraRunning();
        }
    }

    statusBar()->showMessage(QStringLiteral("摄像头已切换为：%1").arg(cameraName.isEmpty() ? QStringLiteral("默认设备") : cameraName), 3000);
}

void MainWindow::applyReceiveOnlySetting(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("receiveOnlyWithoutCamera"), enabled);
    statusBar()->showMessage(enabled
                                 ? QStringLiteral("已开启无摄像头时仅接收模式。")
                             : QStringLiteral("已关闭无摄像头时仅接收模式。"),
                             3000);
}

void MainWindow::applyReceiveRemoteVideoOnlySetting(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("receiveRemoteVideoOnly"), enabled);

    if (enabled) {
        stopCamera();
    }

    statusBar()->showMessage(enabled
                                 ? QStringLiteral("已开启仅接收图像模式，本地不会发送视频。")
                                 : QStringLiteral("已关闭仅接收图像模式。"),
                             3000);
    refreshConversationView();
}

void MainWindow::triggerPeerDiscovery()
{
    m_manager->refreshDiscovery();
}

void MainWindow::sendTextMessage()
{
    const QString text = m_ui->messageEdit->toPlainText().trimmed();
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

    m_ui->messageEdit->clear();
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

    if (!prepareCallCamera()) {
        return;
    }
    startAudioCapture();
    m_activeCallPeerId = peerId;
    m_manager->inviteToCall(peerId);
    if (m_ui->contentTabs) {
        m_ui->contentTabs->setCurrentIndex(1);
    }
    if (m_ui->callStatusLabel) {
        m_ui->callStatusLabel->setText(shouldSendLocalVideo()
                                       ? QStringLiteral("正在呼叫 %1").arg(peerName(peerId))
                                       : QStringLiteral("正在呼叫 %1（仅接收图像）").arg(peerName(peerId)));
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
    m_lastRemoteFrame = QImage();
    m_lastLocalFrame = QImage();
    m_pendingRemoteFrame = QImage();
    m_pendingLocalFrame = QImage();
    refreshVideoLabels();
    if (m_ui->callStatusLabel) {
        m_ui->callStatusLabel->setText(QStringLiteral("未在通话中"));
    }
    stopCallMedia();
    updateActionState();
}

void MainWindow::refreshConversationView()
{
    m_ui->transcript->setPlainText(m_history.value(currentConversationKey()).join(QStringLiteral("\n")));

    if (!currentPeerId().isEmpty()) {
        m_ui->conversationTitle->setText(QStringLiteral("单聊 - %1").arg(peerName(currentPeerId())));
    } else if (!currentGroupId().isEmpty()) {
        m_ui->conversationTitle->setText(QStringLiteral("群聊 - %1").arg(groupName(currentGroupId())));
    } else {
        m_ui->conversationTitle->setText(QStringLiteral("未选择会话"));
    }

    if (m_ui->callStatusLabel) {
        if (!m_activeCallPeerId.isEmpty()) {
            m_ui->callStatusLabel->setText(shouldSendLocalVideo()
                                           ? QStringLiteral("正在与 %1 音视频通话").arg(peerName(m_activeCallPeerId))
                                           : QStringLiteral("正在与 %1 通话（仅接收图像）").arg(peerName(m_activeCallPeerId)));
        } else if (!currentPeerId().isEmpty()) {
            m_ui->callStatusLabel->setText(QStringLiteral("当前通话目标: %1").arg(peerName(currentPeerId())));
        } else {
            m_ui->callStatusLabel->setText(QStringLiteral("请选择一个联系人发起音视频通话"));
        }
    }

    updateActionState();
}

void MainWindow::onPeersChanged(const QList<PeerInfo> &peers)
{
    const QString selectedPeerId = currentPeerId();
    const bool messageEditHadFocus = m_ui->messageEdit->hasFocus();
    m_peers.clear();
    {
        const QSignalBlocker blocker(m_ui->peerList);
        m_ui->peerList->clear();

        for (const PeerInfo &peer : peers) {
            m_peers.insert(peer.id, peer);
            auto *item = new QListWidgetItem(QStringLiteral("%1  %2  %3:%4")
                                                 .arg(peer.name,
                                                      peer.online ? QStringLiteral("[在线]") : QStringLiteral("[离线]"),
                                                      peer.address.toString(),
                                                      QString::number(peer.port)),
                                             m_ui->peerList);
            item->setData(Qt::UserRole, peer.id);
            if (peer.id == selectedPeerId) {
                m_ui->peerList->setCurrentItem(item);
            }
        }
    }

    refreshConversationView();
    if (messageEditHadFocus && m_ui->messageEdit->isEnabled()) {
        m_ui->messageEdit->setFocus();
    }
}

void MainWindow::onGroupsChanged(const QList<GroupInfo> &groups)
{
    const QString selectedGroupId = currentGroupId();
    const bool messageEditHadFocus = m_ui->messageEdit->hasFocus();
    m_groups.clear();
    {
        const QSignalBlocker blocker(m_ui->groupList);
        m_ui->groupList->clear();

        for (const GroupInfo &group : groups) {
            m_groups.insert(group.id, group);
            auto *item = new QListWidgetItem(QStringLiteral("%1 (%2人)").arg(group.name).arg(group.members.size()), m_ui->groupList);
            item->setData(Qt::UserRole, group.id);
            if (group.id == selectedGroupId) {
                m_ui->groupList->setCurrentItem(item);
            }
        }
    }

    refreshConversationView();
    if (messageEditHadFocus && m_ui->messageEdit->isEnabled()) {
        m_ui->messageEdit->setFocus();
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
        if (!prepareCallCamera()) {
            return;
        }
        startAudioCapture();
        m_activeCallPeerId = peerId;
        m_manager->acceptCall(peerId);
        if (m_ui->contentTabs) {
            m_ui->contentTabs->setCurrentIndex(1);
        }
        if (m_ui->callStatusLabel) {
            m_ui->callStatusLabel->setText(shouldSendLocalVideo()
                                           ? QStringLiteral("正在与 %1 音视频通话").arg(peerName(peerId))
                                           : QStringLiteral("正在与 %1 通话（仅接收图像）").arg(peerName(peerId)));
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
    if (!prepareCallCamera()) {
        return;
    }
    startAudioCapture();
    if (m_ui->contentTabs) {
        m_ui->contentTabs->setCurrentIndex(1);
    }
    if (m_ui->callStatusLabel) {
        m_ui->callStatusLabel->setText(shouldSendLocalVideo()
                                       ? QStringLiteral("正在与 %1 音视频通话").arg(peerName(peerId))
                                       : QStringLiteral("正在与 %1 通话（仅接收图像）").arg(peerName(peerId)));
    }
    statusBar()->showMessage(QStringLiteral("%1 已接受音视频请求。").arg(peerName(peerId)), 4000);
    updateActionState();
}

void MainWindow::onCallEnded(const QString &peerId)
{
    if (m_activeCallPeerId == peerId) {
        m_activeCallPeerId.clear();
        m_lastRemoteFrame = QImage();
        m_lastLocalFrame = QImage();
        m_pendingRemoteFrame = QImage();
        m_pendingLocalFrame = QImage();
        refreshVideoLabels();
        if (m_ui->callStatusLabel) {
            m_ui->callStatusLabel->setText(QStringLiteral("未在通话中"));
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
        if (m_ui->contentTabs) {
            m_ui->contentTabs->setCurrentIndex(1);
        }
    }
    if (m_activeCallPeerId == peerId) {
        m_pendingRemoteFrame = frame;
        if (m_ui->callStatusLabel) {
            m_ui->callStatusLabel->setText(shouldSendLocalVideo()
                                           ? QStringLiteral("正在与 %1 音视频通话").arg(peerName(peerId))
                                           : QStringLiteral("正在与 %1 通话（仅接收图像）").arg(peerName(peerId)));
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
    if (!frame.isValid()) {
        return;
    }

    const QImage displayImage = frameToDisplayImage(frame);
    if (displayImage.isNull()) {
        if (!m_frameWarningLimiter.isValid() || m_frameWarningLimiter.elapsed() >= 3000) {
            qWarning().nospace()
                << "Skipping local video frame: pixelFormat=" << frame.pixelFormat()
                << ", size=" << frame.size();
            statusBar()->showMessage(QStringLiteral("摄像头视频帧转换失败，已跳过异常帧。"), 3000);
            m_frameWarningLimiter.restart();
        }
        return;
    }

    m_pendingLocalFrame = displayImage;

    if (!m_localFrameLogTimer.isValid() || m_localFrameLogTimer.elapsed() >= 3000) {
        const QImage transportImage = frameToTransportImage(frame, displayImage);
        qInfo() << "Local video frame ready:"
                << "pixelFormat =" << frame.pixelFormat()
                << "frameSize =" << frame.size()
                << "displayImageSize =" << displayImage.size()
                << "transportImageSize =" << transportImage.size();
        m_localFrameLogTimer.restart();
    }

    if (!m_activeCallPeerId.isEmpty() && shouldSendLocalVideo()) {
        if (!m_frameLimiter.isValid() || m_frameLimiter.elapsed() >= localVideoSendIntervalMs()) {
            const QImage transportImage = frameToTransportImage(frame, displayImage);
            if (!transportImage.isNull()) {
                m_manager->sendVideoFrame(m_activeCallPeerId, transportImage);
            }
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

void MainWindow::flushVideoFrames()
{
    if (!m_pendingLocalFrame.isNull()) {
        m_lastLocalFrame = m_pendingLocalFrame;
        m_pendingLocalFrame = QImage();
        setVideoLabelImage(m_ui->localVideoWidget, m_lastLocalFrame);
    }

    if (!m_pendingRemoteFrame.isNull()) {
        m_lastRemoteFrame = m_pendingRemoteFrame;
        m_pendingRemoteFrame = QImage();
        setVideoLabelImage(m_ui->remoteVideoWidget, m_lastRemoteFrame);
    }
}

void MainWindow::updateActionState()
{
    const bool hasPeer = !currentPeerId().isEmpty();
    const bool hasGroup = !currentGroupId().isEmpty();
    const bool hasConversation = hasPeer || hasGroup;

    m_ui->sendButton->setEnabled(hasConversation);
    m_ui->messageEdit->setEnabled(hasConversation);
    m_ui->fileButton->setEnabled(hasPeer);
    m_ui->videoButton->setEnabled(hasPeer && m_activeCallPeerId.isEmpty());
    m_ui->hangupButton->setEnabled(!m_activeCallPeerId.isEmpty());

}

void MainWindow::appendHistoryLine(const QString &conversationKey, const QString &line)
{
    if (conversationKey.isEmpty()) {
        return;
    }
    m_history[conversationKey].append(line);
}

void MainWindow::populateCameraDevices()
{
    if (!m_ui->cameraCombo) {
        return;
    }

    const QByteArray previousId = m_ui->cameraCombo->currentData().toByteArray();
    m_ui->cameraCombo->clear();

    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    for (const QCameraDevice &device : devices) {
        m_ui->cameraCombo->addItem(device.description(), device.id());
    }

    if (m_ui->cameraCombo->count() == 0) {
        m_ui->cameraCombo->addItem(QStringLiteral("未检测到摄像头"), QByteArray());
        m_ui->cameraCombo->setEnabled(false);
        if (m_ui->cameraApplyButton) {
            m_ui->cameraApplyButton->setEnabled(false);
        }
        return;
    }

    m_ui->cameraCombo->setEnabled(true);
    if (m_ui->cameraApplyButton) {
        m_ui->cameraApplyButton->setEnabled(true);
    }

    for (int index = 0; index < m_ui->cameraCombo->count(); ++index) {
        if (m_ui->cameraCombo->itemData(index).toByteArray() == previousId) {
            m_ui->cameraCombo->setCurrentIndex(index);
            return;
        }
    }
}

QString MainWindow::currentPeerId() const
{
    if (m_ui->sidebarTabs->currentWidget() != m_ui->peersTab || !m_ui->peerList->currentItem()) {
        return {};
    }
    return m_ui->peerList->currentItem()->data(Qt::UserRole).toString();
}

QString MainWindow::currentGroupId() const
{
    if (m_ui->sidebarTabs->currentWidget() != m_ui->groupsTab || !m_ui->groupList->currentItem()) {
        return {};
    }
    return m_ui->groupList->currentItem()->data(Qt::UserRole).toString();
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

QCameraDevice MainWindow::selectedCameraDevice() const
{
    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    const QByteArray selectedId = m_ui->cameraCombo ? m_ui->cameraCombo->currentData().toByteArray() : QByteArray();

    for (const QCameraDevice &device : devices) {
        if (!selectedId.isEmpty() && device.id() == selectedId) {
            return device;
        }
    }

    return QMediaDevices::defaultVideoInput();
}

void MainWindow::selectGroup(const QString &groupId)
{
    m_ui->sidebarTabs->setCurrentWidget(m_ui->groupsTab);
    for (int row = 0; row < m_ui->groupList->count(); ++row) {
        QListWidgetItem *item = m_ui->groupList->item(row);
        if (item->data(Qt::UserRole).toString() == groupId) {
            m_ui->groupList->setCurrentItem(item);
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

    const QCameraDevice cameraDevice = selectedCameraDevice();
    if (cameraDevice.isNull()) {
        return false;
    }

    m_camera = new QCamera(cameraDevice, this);
    connect(m_camera, &QCamera::errorOccurred, this, &MainWindow::onCameraErrorOccurred);
    connect(m_camera, &QCamera::activeChanged, this, &MainWindow::onCameraActiveChanged);
    if (!cameraDevice.videoFormats().isEmpty()) {
        qInfo() << "Available camera formats for" << cameraDevice.description() << ":";
        for (const QCameraFormat &candidate : cameraDevice.videoFormats()) {
            qInfo().nospace()
                << "  "
                << candidate.resolution().width() << "x" << candidate.resolution().height()
                << ", pixelFormat=" << candidate.pixelFormat()
                << ", minFrameRate=" << candidate.minFrameRate()
                << ", maxFrameRate=" << candidate.maxFrameRate();
        }

        const QCameraFormat requestedFormat = preferredCameraFormat(cameraDevice);
        if (requestedFormat.pixelFormat() != QVideoFrameFormat::Format_Invalid) {
            m_camera->setCameraFormat(requestedFormat);
            qInfo().nospace()
                << "Requested camera format for " << cameraDevice.description()
                << ": " << requestedFormat.resolution().width() << "x" << requestedFormat.resolution().height()
                << ", pixelFormat=" << requestedFormat.pixelFormat()
                << ", maxFrameRate=" << requestedFormat.maxFrameRate();
        }
    }

    m_captureSession.setCamera(m_camera);
    m_captureSession.setVideoSink(m_videoSink);
    m_camera->start();

    if (m_camera->error() != QCamera::NoError) {
        reportCameraIssue(QStringLiteral("摄像头启动失败：%1").arg(m_camera->errorString()), true);
        return false;
    }

    const QCameraFormat activeFormat = m_camera->cameraFormat();
    qInfo().nospace()
        << "Active camera format for " << cameraDevice.description()
        << ": " << activeFormat.resolution().width() << "x" << activeFormat.resolution().height()
        << ", pixelFormat=" << activeFormat.pixelFormat()
        << ", maxFrameRate=" << activeFormat.maxFrameRate();
    return true;
}

bool MainWindow::prepareCallCamera()
{
    if (!shouldSendLocalVideo()) {
        stopCamera();
        statusBar()->showMessage(QStringLiteral("当前为仅接收图像模式，本地不会发送视频。"), 4000);
        return true;
    }

    if (ensureCameraRunning()) {
        return true;
    }

    const bool receiveOnlyEnabled = m_ui->receiveOnlyCheck && m_ui->receiveOnlyCheck->isChecked();
    if (!receiveOnlyEnabled) {
        QMessageBox::warning(this, QStringLiteral("摄像头不可用"), QStringLiteral("当前系统没有检测到可用摄像头。"));
        return false;
    }

    QMessageBox::information(this,
                             QStringLiteral("仅接收模式"),
                             QStringLiteral("当前没有可用摄像头，将继续通话并仅接收对端画面，本地不会发送视频。"));
    if (m_ui->callStatusLabel && !m_activeCallPeerId.isEmpty()) {
        m_ui->callStatusLabel->setText(QStringLiteral("正在与 %1 音视频通话（仅接收视频）").arg(peerName(m_activeCallPeerId)));
    }
    statusBar()->showMessage(QStringLiteral("当前没有可用摄像头，已切换到仅接收模式。"), 4000);
    return true;
}

bool MainWindow::shouldSendLocalVideo() const
{
    return !(m_ui->receiveRemoteVideoOnlyCheck && m_ui->receiveRemoteVideoOnlyCheck->isChecked());
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
    connect(m_audioSource, &QAudioSource::stateChanged, this, &MainWindow::onAudioSourceStateChanged);
    m_audioInputDevice = m_audioSource->start();
    if (!m_audioInputDevice) {
        delete m_audioSource;
        m_audioSource = nullptr;
        reportAudioInputIssue(QStringLiteral("麦克风启动失败，将仅发送视频。"));
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
    connect(m_audioSink, &QAudioSink::stateChanged, this, &MainWindow::onAudioSinkStateChanged);
    m_audioOutputDevice = m_audioSink->start();
    if (!m_audioOutputDevice) {
        delete m_audioSink;
        m_audioSink = nullptr;
        reportAudioOutputIssue(QStringLiteral("扬声器启动失败，无法播放对端语音。"));
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
    m_lastLocalFrame = QImage();
    m_pendingLocalFrame = QImage();
    refreshVideoLabels();
}

void MainWindow::setVideoLabelImage(VideoFrameWidget *widget, const QImage &image)
{
    if (!widget) {
        return;
    }

    if (image.isNull()) {
        widget->clearFrame();
        return;
    }

    widget->setFrame(image);
}

void MainWindow::refreshVideoLabels()
{
    setVideoLabelImage(m_ui->localVideoWidget, m_lastLocalFrame);
    setVideoLabelImage(m_ui->remoteVideoWidget, m_lastRemoteFrame);
}

void MainWindow::reportCameraIssue(const QString &message, bool showDialog)
{
    statusBar()->showMessage(message, 5000);
    qWarning() << message;

    if (!showDialog || m_cameraErrorDialogVisible) {
        return;
    }

    m_cameraErrorDialogVisible = true;
    QMessageBox::warning(this, QStringLiteral("摄像头异常"), message);
    m_cameraErrorDialogVisible = false;
}

void MainWindow::reportAudioInputIssue(const QString &message)
{
    statusBar()->showMessage(message, 5000);
    qWarning() << message;
}

void MainWindow::reportAudioOutputIssue(const QString &message)
{
    statusBar()->showMessage(message, 5000);
    qWarning() << message;
}

void MainWindow::onVideoInputsChanged()
{
    const QByteArray currentId = m_ui->cameraCombo ? m_ui->cameraCombo->currentData().toByteArray() : QByteArray();
    populateCameraDevices();

    if (!currentId.isEmpty() && selectedCameraDevice().isNull()) {
        reportCameraIssue(QStringLiteral("当前选择的摄像头已不可用，请重新选择设备。"), true);
        stopCamera();
    } else {
        statusBar()->showMessage(QStringLiteral("摄像头设备列表已更新。"), 3000);
    }
}

void MainWindow::onAudioInputsChanged()
{
    statusBar()->showMessage(QStringLiteral("麦克风设备列表已更新。"), 3000);
}

void MainWindow::onAudioOutputsChanged()
{
    statusBar()->showMessage(QStringLiteral("扬声器设备列表已更新。"), 3000);
}

void MainWindow::onCameraErrorOccurred()
{
    if (!m_camera) {
        return;
    }

    reportCameraIssue(QStringLiteral("摄像头错误：%1").arg(m_camera->errorString()), true);
}

void MainWindow::onCameraActiveChanged(bool active)
{
    if (active) {
        statusBar()->showMessage(QStringLiteral("摄像头已启动。"), 2000);
        return;
    }

    if (m_camera && m_camera->error() == QCamera::NoError && shouldSendLocalVideo() && !m_activeCallPeerId.isEmpty()) {
        reportCameraIssue(QStringLiteral("摄像头已停止工作，本地视频发送已中断。"), false);
    }
}

void MainWindow::onAudioSourceStateChanged()
{
    if (!m_audioSource) {
        return;
    }

    if (m_audioSource->error() != QtAudio::NoError) {
        reportAudioInputIssue(QStringLiteral("麦克风异常，错误码：%1").arg(static_cast<int>(m_audioSource->error())));
    }
}

void MainWindow::onAudioSinkStateChanged()
{
    if (!m_audioSink) {
        return;
    }

    if (m_audioSink->error() != QtAudio::NoError) {
        reportAudioOutputIssue(QStringLiteral("扬声器异常，错误码：%1").arg(static_cast<int>(m_audioSink->error())));
    }
}
