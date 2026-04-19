#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QAudioSource>
#include <QCameraDevice>
#include <QCameraFormat>
#include <QDateTime>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidgetItem>
#include <QMediaDevices>
#include <QMessageBox>
#include <QIODevice>
#include <QKeySequence>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShortcut>
#include <QSettings>
#include <QStatusBar>
#include <QTextOption>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QVideoFrameFormat>

#include <algorithm>
#include <limits>
#include <QVector>

#include "network/peermanager.h"
#include "ui/opencvcamerathread.h"
#include "ui/videoencodeworker.h"
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
    return 66;
#else
    return 50;
#endif
}

int scoreCameraFormat(const QCameraFormat &format)
{
    if (format.pixelFormat() == QVideoFrameFormat::Format_Invalid) {
        return std::numeric_limits<int>::min();
    }

    int score = 0;
    switch (format.pixelFormat()) {
    case QVideoFrameFormat::Format_Jpeg:
        score += 2600;
        break;
    case QVideoFrameFormat::Format_NV12:
    case QVideoFrameFormat::Format_NV21:
    case QVideoFrameFormat::Format_YUV420P:
    case QVideoFrameFormat::Format_YV12:
        score += 2200;
        break;
    case QVideoFrameFormat::Format_YUYV:
    case QVideoFrameFormat::Format_UYVY:
        score += 1200;
        break;
    default:
        score += 1800;
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

QString cameraPixelFormatLabel(const QCameraFormat &format)
{
    switch (format.pixelFormat()) {
    case QVideoFrameFormat::Format_YUYV:
        return QStringLiteral("YUYV");
    case QVideoFrameFormat::Format_UYVY:
        return QStringLiteral("UYVY");
    case QVideoFrameFormat::Format_NV12:
        return QStringLiteral("NV12");
    case QVideoFrameFormat::Format_NV21:
        return QStringLiteral("NV21");
    case QVideoFrameFormat::Format_YUV420P:
        return QStringLiteral("YUV420P");
    case QVideoFrameFormat::Format_YV12:
        return QStringLiteral("YV12");
    case QVideoFrameFormat::Format_Jpeg:
        return QStringLiteral("JPEG");
    default:
        return QStringLiteral("PF:%1").arg(static_cast<int>(format.pixelFormat()));
    }
}

QString cameraFormatKey(const QCameraFormat &format)
{
    return QStringLiteral("%1x%2|%3|%4|%5")
        .arg(format.resolution().width())
        .arg(format.resolution().height())
        .arg(static_cast<int>(format.pixelFormat()))
        .arg(static_cast<int>(format.minFrameRate() * 100.0))
        .arg(static_cast<int>(format.maxFrameRate() * 100.0));
}

QString cameraFormatDisplayText(const QCameraFormat &format)
{
    const QSize size = format.resolution();
    QString text = QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
    if (format.maxFrameRate() > 0.0) {
        text.append(QStringLiteral("  %1 fps").arg(QString::number(format.maxFrameRate(), 'f', format.maxFrameRate() < 10.0 ? 1 : 0)));
    }
    text.append(QStringLiteral("  %1").arg(cameraPixelFormatLabel(format)));
    return text;
}

bool isUsableCameraFormat(const QCameraFormat &format)
{
    return format.pixelFormat() != QVideoFrameFormat::Format_Invalid && format.resolution().isValid();
}

bool isPackedYuv422Format(const QCameraFormat &format)
{
    return format.pixelFormat() == QVideoFrameFormat::Format_YUYV
        || format.pixelFormat() == QVideoFrameFormat::Format_UYVY;
}

QCameraFormat preferredCameraFormat(const QCameraDevice &device)
{
    const QList<QCameraFormat> formats = device.videoFormats();
    if (formats.isEmpty()) {
        return {};
    }

    QList<QCameraFormat> stableFormats;
    stableFormats.reserve(formats.size());
    for (const QCameraFormat &candidate : formats) {
        if (!isPackedYuv422Format(candidate)) {
            stableFormats.append(candidate);
        }
    }

    const QList<QCameraFormat> &candidates = stableFormats.isEmpty() ? formats : stableFormats;
    QCameraFormat best = candidates.constFirst();
    int bestScore = scoreCameraFormat(best);
    for (const QCameraFormat &candidate : candidates) {
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

QString cameraDevicePathFromId(const QByteArray &deviceId)
{
    const QString rawId = QString::fromLatin1(deviceId);
    if (rawId.isEmpty()) {
        return {};
    }

    static const QRegularExpression devicePathPattern(QStringLiteral("(/dev/video\\d+)"));
    const QRegularExpressionMatch match = devicePathPattern.match(rawId);
    if (match.hasMatch()) {
        return match.captured(1);
    }

    if (rawId.startsWith(QStringLiteral("/dev/video"))) {
        return rawId;
    }

    if (rawId.startsWith(QStringLiteral("video"))) {
        return QStringLiteral("/dev/%1").arg(rawId);
    }

    return {};
}

QImage scaledComparisonImage(const QImage &image)
{
    if (image.isNull()) {
        return {};
    }

    return image.scaled(QSize(96, 72), Qt::IgnoreAspectRatio, Qt::FastTransformation)
        .convertToFormat(QImage::Format_RGB32);
}

qreal averageRgbDifference(const QImage &left, const QImage &right, const QRect &rect)
{
    if (left.isNull() || right.isNull() || rect.isEmpty()) {
        return 0.0;
    }

    qint64 score = 0;
    int pixelCount = 0;
    const QRect boundedRect = rect.intersected(QRect(QPoint(0, 0), left.size()))
                                  .intersected(QRect(QPoint(0, 0), right.size()));
    for (int y = boundedRect.top(); y <= boundedRect.bottom(); ++y) {
        const QRgb *leftLine = reinterpret_cast<const QRgb *>(left.constScanLine(y));
        const QRgb *rightLine = reinterpret_cast<const QRgb *>(right.constScanLine(y));
        for (int x = boundedRect.left(); x <= boundedRect.right(); ++x) {
            score += qAbs(qRed(leftLine[x]) - qRed(rightLine[x]));
            score += qAbs(qGreen(leftLine[x]) - qGreen(rightLine[x]));
            score += qAbs(qBlue(leftLine[x]) - qBlue(rightLine[x]));
            ++pixelCount;
        }
    }

    return pixelCount == 0 ? 0.0 : static_cast<qreal>(score) / (pixelCount * 3);
}

qreal medianValue(QVector<qreal> values)
{
    if (values.isEmpty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());
    return values.at(values.size() / 2);
}

bool looksLikeLocalFrameCorruption(const QImage &candidate, const QImage &reference)
{
    if (candidate.isNull() || reference.isNull()) {
        return false;
    }

    const QImage left = scaledComparisonImage(candidate);
    const QImage right = scaledComparisonImage(reference);
    if (left.isNull() || right.isNull() || left.size() != right.size()) {
        return false;
    }

    QVector<qreal> bandDiffs;
    bandDiffs.reserve(12);
    for (int band = 0; band < 12; ++band) {
        bandDiffs.append(averageRgbDifference(left, right, QRect(0, band * 6, 96, 6)));
    }

    const qreal medianBandDiff = medianValue(bandDiffs);
    const qreal maxBandDiff = *std::max_element(bandDiffs.constBegin(), bandDiffs.constEnd());
    if (medianBandDiff <= 18.0
        && maxBandDiff >= 55.0
        && maxBandDiff >= qMax<qreal>(medianBandDiff * 4.0, 45.0)) {
        return true;
    }

    QVector<qreal> cellDiffs;
    cellDiffs.reserve(48);
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 8; ++column) {
            cellDiffs.append(averageRgbDifference(left, right, QRect(column * 12, row * 12, 12, 12)));
        }
    }

    const qreal medianCellDiff = medianValue(cellDiffs);
    const qreal maxCellDiff = *std::max_element(cellDiffs.constBegin(), cellDiffs.constEnd());
    int suspiciousCellCount = 0;
    for (qreal cellDiff : cellDiffs) {
        if (cellDiff >= 60.0
            && cellDiff >= qMax<qreal>(medianCellDiff * 5.0, 50.0)) {
            ++suspiciousCellCount;
        }
    }

    return medianCellDiff <= 16.0
        && maxCellDiff >= 65.0
        && suspiciousCellCount >= 1
        && suspiciousCellCount <= 10;
}

}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_ui(new Ui::MainWindow)
    , m_manager(new PeerManager(this))
    , m_mediaDevices(new QMediaDevices(this))
    , m_cameraThread(new OpenCvCameraThread(this))
    , m_videoRefreshTimer(new QTimer(this))
    , m_stateSaveTimer(new QTimer(this))
    , m_videoEncodeThread(new QThread(this))
    , m_videoEncodeWorker(new VideoEncodeWorker)
{
    setupUi();
    connectUi();

    loadConversationState();

    QSettings settings;
    const QString savedName = settings.value(QStringLiteral("displayName"), m_manager->displayName()).toString();
    m_ui->nameEdit->setText(savedName);
    m_manager->setDisplayName(savedName);
    populateCameraDevices();

    const QByteArray savedCameraId = settings.value(QStringLiteral("cameraDeviceId")).toByteArray();
    const QString savedCameraFormatKey = settings.value(QStringLiteral("cameraFormatKey")).toString();
    if (m_ui->cameraCombo) {
        for (int index = 0; index < m_ui->cameraCombo->count(); ++index) {
            if (m_ui->cameraCombo->itemData(index).toByteArray() == savedCameraId) {
                m_ui->cameraCombo->setCurrentIndex(index);
                break;
            }
        }
    }
    populateCameraFormats(savedCameraFormatKey);
    if (m_ui->cameraFormatCombo
        && !m_ui->cameraFormatCombo->currentData().toString().isEmpty()
        && isPackedYuv422Format(selectedCameraFormat(selectedCameraDevice()))
        && !isPackedYuv422Format(preferredCameraFormat(selectedCameraDevice()))) {
        m_ui->cameraFormatCombo->setCurrentIndex(0);
    }
    updateCameraFormatHint();
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
    connect(m_manager, &PeerManager::remoteVideoSendingChanged, this, &MainWindow::onRemoteVideoSendingChanged);
    connect(m_manager, &PeerManager::remoteVideoFrameReceived, this, &MainWindow::onRemoteVideoFrameReceived);
    connect(m_manager, &PeerManager::remoteAudioChunkReceived, this, &MainWindow::onRemoteAudioChunkReceived);
    connect(m_cameraThread, &OpenCvCameraThread::frameCaptured, this, &MainWindow::processLocalFrame);
    connect(m_cameraThread, &OpenCvCameraThread::cameraError, this, &MainWindow::onCameraErrorOccurred);
    connect(m_cameraThread, &OpenCvCameraThread::cameraActiveChanged, this, &MainWindow::onCameraActiveChanged);
    connect(m_cameraThread, &OpenCvCameraThread::captureFormatResolved, this, &MainWindow::onCameraCaptureFormatResolved);
    connect(m_videoRefreshTimer, &QTimer::timeout, this, &MainWindow::flushVideoFrames);
    connect(m_stateSaveTimer, &QTimer::timeout, this, [this]() {
        saveConversationState();
    });
    connect(m_mediaDevices, &QMediaDevices::videoInputsChanged, this, &MainWindow::onVideoInputsChanged);
    connect(m_mediaDevices, &QMediaDevices::audioInputsChanged, this, &MainWindow::onAudioInputsChanged);
    connect(m_mediaDevices, &QMediaDevices::audioOutputsChanged, this, &MainWindow::onAudioOutputsChanged);
    m_videoEncodeWorker->moveToThread(m_videoEncodeThread);
    connect(this, &MainWindow::encodeVideoFrameRequested, m_videoEncodeWorker, &VideoEncodeWorker::encodeFrame, Qt::QueuedConnection);
    connect(m_videoEncodeWorker, &VideoEncodeWorker::frameEncoded, this, &MainWindow::onEncodedVideoFrame, Qt::QueuedConnection);
    connect(m_videoEncodeThread, &QThread::finished, m_videoEncodeWorker, &QObject::deleteLater);
    m_videoEncodeThread->start();

    m_videoRefreshTimer->setTimerType(Qt::PreciseTimer);
    m_videoRefreshTimer->setInterval(33);
    m_videoRefreshTimer->start();
    m_stateSaveTimer->setSingleShot(true);
    m_stateSaveTimer->setInterval(500);

    onPeersChanged(m_manager->peers());
    onGroupsChanged(m_manager->groups());

    setWindowTitle(QStringLiteral("LanLinkChat - 局域网聊天"));
    resize(1280, 820);
    showStatusMessage(QStringLiteral("已启动，等待局域网节点发现。"), 3000);
}

MainWindow::~MainWindow()
{
    stopCallMedia();
    if (m_stateSaveTimer->isActive()) {
        m_stateSaveTimer->stop();
    }
    if (m_cameraThread) {
        m_cameraThread->stopCapture();
        m_cameraThread->wait();
    }
    if (m_videoEncodeThread) {
        m_videoEncodeThread->quit();
        m_videoEncodeThread->wait();
    }
    saveConversationState();
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
    m_ui->callStatusLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 500; padding: 0;"));
    if (m_ui->cameraFormatHintLabel) {
        m_ui->cameraFormatHintLabel->setStyleSheet(QStringLiteral("color: #666; font-size: 11px; padding-left: 4px;"));
    }
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
    connect(m_ui->cameraCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        populateCameraFormats();
        updateCameraFormatHint();
    });
    connect(m_ui->cameraFormatCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
        updateCameraFormatHint();
    });
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
    showStatusMessage(QStringLiteral("显示名已更新。"), 3000);
}

void MainWindow::applyCameraSelection()
{
    if (!m_ui->cameraCombo) {
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("cameraDeviceId"), m_ui->cameraCombo->currentData().toByteArray());
    settings.setValue(QStringLiteral("cameraFormatKey"),
                      m_ui->cameraFormatCombo ? m_ui->cameraFormatCombo->currentData().toString() : QString());

    const QString cameraName = m_ui->cameraCombo->currentText().trimmed();
    const QCameraFormat format = selectedCameraFormat(selectedCameraDevice());
    const QString formatText = !isUsableCameraFormat(format)
        ? QStringLiteral("自动推荐")
        : cameraFormatDisplayText(format);
    if (m_cameraThread && m_cameraThread->isRunning()) {
        stopCamera();
        if (!m_activeCallPeerId.isEmpty()) {
            ensureCameraRunning();
        }
    }

    updateCameraFormatHint();
    showStatusMessage(QStringLiteral("摄像头设置已应用：%1，%2")
                          .arg(cameraName.isEmpty() ? QStringLiteral("默认设备") : cameraName, formatText),
                      4000);
}

void MainWindow::applyReceiveOnlySetting(bool enabled)
{
    QSettings settings;
    settings.setValue(QStringLiteral("receiveOnlyWithoutCamera"), enabled);
    showStatusMessage(enabled
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
        clearLocalCallFrame();
    } else if (!m_activeCallPeerId.isEmpty()) {
        if (!prepareCallCamera()) {
            if (m_ui->receiveRemoteVideoOnlyCheck) {
                const QSignalBlocker blocker(m_ui->receiveRemoteVideoOnlyCheck);
                m_ui->receiveRemoteVideoOnlyCheck->setChecked(true);
            }
            settings.setValue(QStringLiteral("receiveRemoteVideoOnly"), true);
            updateCallStatusLabel();
            showStatusMessage(QStringLiteral("恢复本地视频发送失败，已保持仅接收图像模式。"), 4000);
            return;
        }
    }

    if (!m_activeCallPeerId.isEmpty()) {
        m_manager->setVideoSendingEnabled(m_activeCallPeerId, !enabled);
        appendHistoryLine(peerConversationKey(m_activeCallPeerId),
                          QStringLiteral("[%1] %2")
                              .arg(timestampLabel(QDateTime::currentDateTime()),
                                   enabled ? QStringLiteral("我已暂停本地视频发送")
                                           : QStringLiteral("我已恢复本地视频发送")));
    }

    updateCallStatusLabel();
    showStatusMessage(enabled
                          ? QStringLiteral("已开启仅接收图像模式，本地不会发送视频。")
                          : QStringLiteral("已关闭仅接收图像模式，本地视频发送已恢复。"),
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
        if (m_manager->sendDirectMessage(peerId, text)) {
            appendHistoryLine(peerConversationKey(peerId),
                              QStringLiteral("[%1] 我: %2").arg(timestampLabel(QDateTime::currentDateTime()), text));
        } else {
            showStatusMessage(QStringLiteral("消息发送失败，对端当前不可达。"), 4000);
            return;
        }
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
    showStatusMessage(QStringLiteral("已向 %1 发起音视频请求。").arg(peerName(peerId)), 4000);
    updateActionState();
}

void MainWindow::endVideoCall()
{
    if (m_activeCallPeerId.isEmpty()) {
        return;
    }

    const QString peerId = m_activeCallPeerId;
    m_manager->endCall(peerId);
    showStatusMessage(QStringLiteral("音视频通话已结束。"), 3000);
    m_activeCallPeerId.clear();
    stopCallMedia();
    clearCallFrames();
    updateCallStatusLabel();
    appendHistoryLine(peerConversationKey(peerId),
                      QStringLiteral("[%1] 通话已由我方结束")
                          .arg(timestampLabel(QDateTime::currentDateTime())));
    updateActionState();
}

void MainWindow::refreshConversationView()
{
    const QString conversationKey = currentConversationKey();
    markConversationAsRead(conversationKey);
    updateTranscriptView(conversationKey);

    if (!currentPeerId().isEmpty()) {
        m_ui->conversationTitle->setText(QStringLiteral("单聊 - %1").arg(peerName(currentPeerId())));
    } else if (!currentGroupId().isEmpty()) {
        m_ui->conversationTitle->setText(QStringLiteral("群聊 - %1").arg(groupName(currentGroupId())));
    } else {
        m_ui->conversationTitle->setText(QStringLiteral("未选择会话"));
    }

    updateCallStatusLabel();

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
            auto *item = new QListWidgetItem(peerListLabel(peer), m_ui->peerList);
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
            auto *item = new QListWidgetItem(groupListLabel(group), m_ui->groupList);
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
    const QString conversationKey = peerConversationKey(peerId);
    appendHistoryLine(conversationKey,
                      QStringLiteral("[%1] %2: %3").arg(timestampLabel(timestamp), peerName(peerId), text));
    updateUnreadCount(conversationKey);
    refreshConversationView();
}

void MainWindow::onGroupMessageReceived(const QString &groupId, const QString &peerId, const QString &text, const QDateTime &timestamp)
{
    const QString conversationKey = groupConversationKey(groupId);
    appendHistoryLine(conversationKey,
                      QStringLiteral("[%1] %2: %3").arg(timestampLabel(timestamp), peerName(peerId), text));
    updateUnreadCount(conversationKey);
    refreshConversationView();
}

void MainWindow::onFileReceived(const QString &peerId, const QString &fileName, const QString &savedPath)
{
    const QString conversationKey = peerConversationKey(peerId);
    appendHistoryLine(conversationKey,
                      QStringLiteral("[%1] %2 发送了文件: %3\n已保存到: %4")
                          .arg(timestampLabel(QDateTime::currentDateTime()), peerName(peerId), fileName, savedPath));
    updateUnreadCount(conversationKey);
    refreshConversationView();
    showStatusMessage(QStringLiteral("收到文件 %1").arg(fileName), 5000);
}

void MainWindow::onNoticeRaised(const QString &message)
{
    showStatusMessage(message, 5000);
}

void MainWindow::onCallInvitationReceived(const QString &peerId)
{
    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Question);
    dialog.setWindowTitle(QStringLiteral("音视频通话邀请"));
    dialog.setText(QStringLiteral("%1 邀请你开始音视频通话。").arg(peerName(peerId)));
    dialog.setInformativeText(QStringLiteral("接受后会切换到音视频页，并尝试启用当前选择的摄像头和麦克风。"));
    dialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    dialog.setDefaultButton(QMessageBox::Yes);
    dialog.button(QMessageBox::Yes)->setText(QStringLiteral("接受"));
    dialog.button(QMessageBox::No)->setText(QStringLiteral("稍后再说"));
    const auto result = dialog.exec();
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
    m_manager->setVideoSendingEnabled(peerId, shouldSendLocalVideo());
    showStatusMessage(QStringLiteral("已接受 %1 的音视频请求。").arg(peerName(peerId)), 4000);
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
    m_manager->setVideoSendingEnabled(peerId, shouldSendLocalVideo());
    showStatusMessage(QStringLiteral("%1 已接受音视频请求。").arg(peerName(peerId)), 4000);
    updateActionState();
}

void MainWindow::onCallEnded(const QString &peerId)
{
    if (m_activeCallPeerId != peerId) {
        updateActionState();
        return;
    }

    m_activeCallPeerId.clear();
    stopCallMedia();
    clearCallFrames();
    updateCallStatusLabel();
    appendHistoryLine(peerConversationKey(peerId),
                      QStringLiteral("[%1] %2 已结束音视频通话")
                          .arg(timestampLabel(QDateTime::currentDateTime()), peerName(peerId)));
    showStatusMessage(QStringLiteral("%1 已结束音视频通话。").arg(peerName(peerId)), 4000);
    QMessageBox::information(this,
                             QStringLiteral("通话已结束"),
                             QStringLiteral("%1 已结束音视频通话。").arg(peerName(peerId)));
    updateActionState();
}

void MainWindow::onRemoteVideoSendingChanged(const QString &peerId, bool enabled)
{
    if (m_activeCallPeerId != peerId) {
        return;
    }

    if (!enabled) {
        clearRemoteCallFrame();
        if (m_ui->remoteVideoWidget) {
            m_ui->remoteVideoWidget->setPlaceholderText(QStringLiteral("对方已暂停视频"));
        }
        appendHistoryLine(peerConversationKey(peerId),
                          QStringLiteral("[%1] %2 已暂停视频发送")
                              .arg(timestampLabel(QDateTime::currentDateTime()), peerName(peerId)));
        showStatusMessage(QStringLiteral("%1 已暂停视频发送。").arg(peerName(peerId)), 4000);
        QMessageBox::information(this,
                                 QStringLiteral("视频状态变更"),
                                 QStringLiteral("%1 已暂停视频发送。").arg(peerName(peerId)));
        return;
    }

    if (m_ui->remoteVideoWidget) {
        m_ui->remoteVideoWidget->setPlaceholderText(QStringLiteral("远端画面"));
    }
    appendHistoryLine(peerConversationKey(peerId),
                      QStringLiteral("[%1] %2 已恢复视频发送")
                          .arg(timestampLabel(QDateTime::currentDateTime()), peerName(peerId)));
    showStatusMessage(QStringLiteral("%1 已恢复视频发送。").arg(peerName(peerId)), 4000);
}

void MainWindow::onRemoteVideoFrameReceived(const QString &peerId, const QImage &frame)
{
    if (m_activeCallPeerId == peerId) {
        if (m_ui->remoteVideoWidget) {
            m_ui->remoteVideoWidget->setPlaceholderText(QStringLiteral("远端画面"));
        }
        m_pendingRemoteFrame = frame;
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

    if (m_activeCallPeerId != peerId) {
        return;
    }

    const QAudioFormat format = formatFromWire(sampleRate, channelCount, sampleFormat);
    if (!ensureAudioPlayback(format) || !m_audioOutputDevice) {
        return;
    }

    m_audioOutputDevice->write(audioData);
}

void MainWindow::onEncodedVideoFrame(const QString &peerId,
                                     const QByteArray &encodedFrame,
                                     const QString &imageFormat,
                                     const QSize &frameSize)
{
    m_videoEncodeInFlight = false;

    if (!peerId.isEmpty()
        && peerId == m_activeCallPeerId
        && shouldSendLocalVideo()
        && !encodedFrame.isEmpty()) {
        m_manager->sendEncodedVideoFrame(peerId, encodedFrame, imageFormat, frameSize);
    }

    if (m_pendingVideoEncodeFrame.isNull() || m_pendingVideoPeerId.isEmpty()) {
        return;
    }

    const QString nextPeerId = m_pendingVideoPeerId;
    const QImage nextFrame = m_pendingVideoEncodeFrame;
    m_pendingVideoPeerId.clear();
    m_pendingVideoEncodeFrame = QImage();

    if (nextPeerId != m_activeCallPeerId || !shouldSendLocalVideo()) {
        return;
    }

    m_videoEncodeInFlight = true;
    emit encodeVideoFrameRequested(nextPeerId, nextFrame);
}

void MainWindow::processLocalFrame(const QImage &frame)
{
    if (frame.isNull() || !m_cameraActive || !shouldSendLocalVideo()) {
        return;
    }

    const QImage displayImage = frame.format() == QImage::Format_RGB32
        ? frame
        : frame.convertToFormat(QImage::Format_RGB32);
    if (displayImage.isNull()) {
        if (!m_frameWarningLimiter.isValid() || m_frameWarningLimiter.elapsed() >= 3000) {
            showStatusMessage(QStringLiteral("摄像头视频帧转换失败，已跳过异常帧。"), 3000);
            m_frameWarningLimiter.restart();
        }
        return;
    }

    const QImage referenceFrame = !m_pendingLocalFrame.isNull() ? m_pendingLocalFrame : m_lastLocalFrame;
    if (m_filteredLocalFrameCount < 8 && looksLikeLocalFrameCorruption(displayImage, referenceFrame)) {
        ++m_filteredLocalFrameCount;
        if (!m_frameWarningLimiter.isValid() || m_frameWarningLimiter.elapsed() >= 3000) {
            qWarning() << "Filtered likely corrupted local camera frame, count =" << m_filteredLocalFrameCount;
            showStatusMessage(QStringLiteral("检测到摄像头异常帧，已自动过滤。"), 3000);
            m_frameWarningLimiter.restart();
        }
        return;
    }

    m_filteredLocalFrameCount = 0;
    m_pendingLocalFrame = displayImage;

    const bool shouldLog = !m_localFrameLogTimer.isValid() || m_localFrameLogTimer.elapsed() >= 3000;
    const bool shouldSend = !m_activeCallPeerId.isEmpty()
        && shouldSendLocalVideo()
        && (!m_frameLimiter.isValid() || m_frameLimiter.elapsed() >= localVideoSendIntervalMs());

    if (shouldLog) {
        qInfo() << "Local video frame ready:"
                << "frameSize =" << displayImage.size()
                << "imageFormat =" << displayImage.format();
        m_localFrameLogTimer.restart();
    }

    if (shouldSend) {
        if (!displayImage.isNull()) {
            if (!m_videoEncodeInFlight) {
                m_videoEncodeInFlight = true;
                emit encodeVideoFrameRequested(m_activeCallPeerId, displayImage);
            } else {
                m_pendingVideoPeerId = m_activeCallPeerId;
                m_pendingVideoEncodeFrame = displayImage;
            }
        }
        m_frameLimiter.restart();
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
    scheduleConversationStateSave();
}

void MainWindow::loadConversationState()
{
    QSettings settings;
    const QByteArray historyData = settings.value(QStringLiteral("conversationHistory")).toByteArray();
    if (!historyData.isEmpty()) {
        const QJsonDocument historyDocument = QJsonDocument::fromJson(historyData);
        if (historyDocument.isObject()) {
            const QJsonObject root = historyDocument.object();
            for (auto it = root.begin(); it != root.end(); ++it) {
                const QJsonArray lines = it.value().toArray();
                QStringList conversationLines;
                conversationLines.reserve(lines.size());
                for (const QJsonValue &line : lines) {
                    conversationLines.append(line.toString());
                }
                if (!conversationLines.isEmpty()) {
                    m_history.insert(it.key(), conversationLines);
                }
            }
        }
    }

    const QByteArray unreadData = settings.value(QStringLiteral("conversationUnread")).toByteArray();
    if (unreadData.isEmpty()) {
        return;
    }

    const QJsonDocument unreadDocument = QJsonDocument::fromJson(unreadData);
    if (!unreadDocument.isObject()) {
        return;
    }

    const QJsonObject root = unreadDocument.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const int unreadCount = it.value().toInt();
        if (unreadCount > 0) {
            m_unreadCounts.insert(it.key(), unreadCount);
        }
    }
}

void MainWindow::saveConversationState() const
{
    QJsonObject historyRoot;
    for (auto it = m_history.begin(); it != m_history.end(); ++it) {
        QJsonArray lines;
        for (const QString &line : it.value()) {
            lines.append(line);
        }
        historyRoot.insert(it.key(), lines);
    }

    QJsonObject unreadRoot;
    for (auto it = m_unreadCounts.begin(); it != m_unreadCounts.end(); ++it) {
        if (it.value() > 0) {
            unreadRoot.insert(it.key(), it.value());
        }
    }

    QSettings settings;
    settings.setValue(QStringLiteral("conversationHistory"), QJsonDocument(historyRoot).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("conversationUnread"), QJsonDocument(unreadRoot).toJson(QJsonDocument::Compact));
}

void MainWindow::scheduleConversationStateSave()
{
    if (!m_stateSaveTimer) {
        saveConversationState();
        return;
    }

    m_stateSaveTimer->start();
}

void MainWindow::updateTranscriptView(const QString &conversationKey, bool forceFullRefresh)
{
    const QStringList lines = m_history.value(conversationKey);
    const int lineCount = lines.size();
    const bool sameConversation = m_displayedConversationKey == conversationKey;

    if (!forceFullRefresh && sameConversation && lineCount == m_displayedConversationLineCount) {
        return;
    }

    if (!forceFullRefresh
        && sameConversation
        && lineCount == m_displayedConversationLineCount + 1
        && !lines.isEmpty()) {
        m_ui->transcript->appendPlainText(lines.constLast());
    } else {
        m_ui->transcript->setPlainText(lines.join(QStringLiteral("\n")));
    }

    m_displayedConversationKey = conversationKey;
    m_displayedConversationLineCount = lineCount;
    if (m_ui->transcript->verticalScrollBar()) {
        m_ui->transcript->verticalScrollBar()->setValue(m_ui->transcript->verticalScrollBar()->maximum());
    }
}

void MainWindow::clearCallFrames()
{
    clearRemoteCallFrame();
    clearLocalCallFrame();
    refreshVideoLabels();
}

void MainWindow::clearLocalCallFrame()
{
    m_lastLocalFrame = QImage();
    m_pendingLocalFrame = QImage();
    m_pendingVideoEncodeFrame = QImage();
    m_pendingVideoPeerId.clear();
    m_filteredLocalFrameCount = 0;
    refreshVideoLabels();
}

void MainWindow::clearRemoteCallFrame()
{
    m_lastRemoteFrame = QImage();
    m_pendingRemoteFrame = QImage();
    if (m_ui->remoteVideoWidget) {
        m_ui->remoteVideoWidget->setPlaceholderText(QStringLiteral("远端画面"));
    }
    refreshVideoLabels();
}

void MainWindow::updateCallStatusLabel()
{
    if (!m_ui->callStatusLabel) {
        return;
    }

    if (!m_activeCallPeerId.isEmpty()) {
        m_ui->callStatusLabel->setText(shouldSendLocalVideo()
                                       ? QStringLiteral("正在与 %1 音视频通话").arg(peerName(m_activeCallPeerId))
                                       : QStringLiteral("正在与 %1 通话（仅接收图像）").arg(peerName(m_activeCallPeerId)));
        return;
    }

    if (!currentPeerId().isEmpty()) {
        m_ui->callStatusLabel->setText(QStringLiteral("当前通话目标: %1").arg(peerName(currentPeerId())));
    } else {
        m_ui->callStatusLabel->setText(QStringLiteral("请选择一个联系人发起音视频通话"));
    }
}

void MainWindow::populateCameraDevices()
{
    if (!m_ui->cameraCombo) {
        return;
    }

    const QByteArray previousId = m_ui->cameraCombo->currentData().toByteArray();
    const QSignalBlocker blocker(m_ui->cameraCombo);
    m_ui->cameraCombo->clear();

    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    for (const QCameraDevice &device : devices) {
        m_ui->cameraCombo->addItem(device.description(), device.id());
    }

    if (m_ui->cameraCombo->count() == 0) {
        m_ui->cameraCombo->addItem(QStringLiteral("未检测到摄像头"), QByteArray());
        m_ui->cameraCombo->setEnabled(false);
        populateCameraFormats();
        if (m_ui->cameraApplyButton) {
            m_ui->cameraApplyButton->setEnabled(false);
        }
        updateCameraFormatHint();
        return;
    }

    m_ui->cameraCombo->setEnabled(true);
    if (m_ui->cameraApplyButton) {
        m_ui->cameraApplyButton->setEnabled(true);
    }

    for (int index = 0; index < m_ui->cameraCombo->count(); ++index) {
        if (m_ui->cameraCombo->itemData(index).toByteArray() == previousId) {
            m_ui->cameraCombo->setCurrentIndex(index);
            populateCameraFormats();
            updateCameraFormatHint();
            return;
        }
    }

    populateCameraFormats();
    updateCameraFormatHint();
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

QString MainWindow::peerListLabel(const PeerInfo &peer) const
{
    QString label = QStringLiteral("%1  %2  %3:%4")
                        .arg(peer.name,
                             peer.online ? QStringLiteral("[在线]") : QStringLiteral("[离线]"),
                             peer.address.toString(),
                             QString::number(peer.port));
    const int unreadCount = m_unreadCounts.value(peerConversationKey(peer.id));
    if (unreadCount > 0) {
        label.append(QStringLiteral("  (%1条未读)").arg(unreadCount));
    }
    return label;
}

QString MainWindow::groupListLabel(const GroupInfo &group) const
{
    QString label = QStringLiteral("%1 (%2人)").arg(group.name).arg(group.members.size());
    const int unreadCount = m_unreadCounts.value(groupConversationKey(group.id));
    if (unreadCount > 0) {
        label.append(QStringLiteral("  (%1条未读)").arg(unreadCount));
    }
    return label;
}

QString MainWindow::selectedCameraDevicePath() const
{
    return cameraDevicePathFromId(m_ui->cameraCombo ? m_ui->cameraCombo->currentData().toByteArray() : QByteArray());
}

int MainWindow::selectedCameraDeviceIndex() const
{
    return m_ui->cameraCombo ? m_ui->cameraCombo->currentIndex() : -1;
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

void MainWindow::populateCameraFormats(const QString &preferredFormatKey)
{
    if (!m_ui->cameraFormatCombo) {
        return;
    }

    const QString currentKey = preferredFormatKey.isEmpty()
        ? m_ui->cameraFormatCombo->currentData().toString()
        : preferredFormatKey;

    const QSignalBlocker blocker(m_ui->cameraFormatCombo);
    m_ui->cameraFormatCombo->clear();

    const QCameraDevice device = selectedCameraDevice();
    if (device.isNull()) {
        m_ui->cameraFormatCombo->addItem(QStringLiteral("无可用格式"), QString());
        m_ui->cameraFormatCombo->setEnabled(false);
        return;
    }

    QList<QCameraFormat> formats = device.videoFormats();
    if (formats.isEmpty()) {
        m_ui->cameraFormatCombo->addItem(QStringLiteral("自动推荐"), QString());
        m_ui->cameraFormatCombo->setEnabled(false);
        return;
    }

    std::sort(formats.begin(), formats.end(), [](const QCameraFormat &left, const QCameraFormat &right) {
        const int leftArea = left.resolution().width() * left.resolution().height();
        const int rightArea = right.resolution().width() * right.resolution().height();
        if (leftArea != rightArea) {
            return leftArea > rightArea;
        }
        if (left.maxFrameRate() != right.maxFrameRate()) {
            return left.maxFrameRate() > right.maxFrameRate();
        }
        return cameraPixelFormatLabel(left) < cameraPixelFormatLabel(right);
    });

    m_ui->cameraFormatCombo->setEnabled(true);
    m_ui->cameraFormatCombo->addItem(QStringLiteral("自动推荐"), QString());
    for (const QCameraFormat &format : formats) {
        m_ui->cameraFormatCombo->addItem(cameraFormatDisplayText(format), cameraFormatKey(format));
    }

    int targetIndex = 0;
    if (!currentKey.isEmpty()) {
        for (int index = 0; index < m_ui->cameraFormatCombo->count(); ++index) {
            if (m_ui->cameraFormatCombo->itemData(index).toString() == currentKey) {
                targetIndex = index;
                break;
            }
        }
    }
    m_ui->cameraFormatCombo->setCurrentIndex(targetIndex);
}

QCameraFormat MainWindow::selectedCameraFormat(const QCameraDevice &device) const
{
    if (device.isNull() || !m_ui->cameraFormatCombo) {
        return {};
    }

    const QString selectedKey = m_ui->cameraFormatCombo->currentData().toString();
    if (selectedKey.isEmpty()) {
        return preferredCameraFormat(device);
    }

    for (const QCameraFormat &format : device.videoFormats()) {
        if (cameraFormatKey(format) == selectedKey) {
            return format;
        }
    }

    return preferredCameraFormat(device);
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

void MainWindow::markConversationAsRead(const QString &conversationKey)
{
    if (conversationKey.isEmpty() || m_unreadCounts.value(conversationKey) == 0) {
        return;
    }

    m_unreadCounts.remove(conversationKey);
    updatePeerListLabels();
    updateGroupListLabels();
    scheduleConversationStateSave();
}

void MainWindow::updateUnreadCount(const QString &conversationKey)
{
    if (conversationKey.isEmpty() || conversationKey == currentConversationKey()) {
        return;
    }

    m_unreadCounts[conversationKey] = m_unreadCounts.value(conversationKey) + 1;
    updatePeerListLabels();
    updateGroupListLabels();
    scheduleConversationStateSave();
}

void MainWindow::updatePeerListLabels()
{
    for (int row = 0; row < m_ui->peerList->count(); ++row) {
        QListWidgetItem *item = m_ui->peerList->item(row);
        if (!item) {
            continue;
        }

        const QString peerId = item->data(Qt::UserRole).toString();
        if (m_peers.contains(peerId)) {
            item->setText(peerListLabel(m_peers.value(peerId)));
        }
    }
}

void MainWindow::updateGroupListLabels()
{
    for (int row = 0; row < m_ui->groupList->count(); ++row) {
        QListWidgetItem *item = m_ui->groupList->item(row);
        if (!item) {
            continue;
        }

        const QString groupId = item->data(Qt::UserRole).toString();
        if (m_groups.contains(groupId)) {
            item->setText(groupListLabel(m_groups.value(groupId)));
        }
    }
}

void MainWindow::updateCameraFormatHint()
{
    if (!m_ui->cameraFormatHintLabel) {
        return;
    }

#ifndef LANLINKCHAT_HAS_OPENCV
    m_ui->cameraFormatHintLabel->setText(QStringLiteral("当前构建未启用 OpenCV 采集，请安装 OpenCV 后重新编译。"));
    m_ui->cameraFormatHintLabel->setToolTip(m_ui->cameraFormatHintLabel->text());
    return;
#endif

    const QCameraDevice device = selectedCameraDevice();
    if (device.isNull()) {
        m_ui->cameraFormatHintLabel->setText(QStringLiteral("当前未检测到摄像头，无法选择分辨率。"));
        m_ui->cameraFormatHintLabel->setToolTip(m_ui->cameraFormatHintLabel->text());
        return;
    }

    QString prefix = QStringLiteral("自动推荐");
    QCameraFormat format = selectedCameraFormat(device);
    if (m_ui->cameraFormatCombo && !m_ui->cameraFormatCombo->currentData().toString().isEmpty()) {
        prefix = QStringLiteral("当前选择");
    }

    if (m_cameraActive) {
        prefix = QStringLiteral("当前生效");
    }

    if (m_cameraActive && m_activeCameraResolution.isValid()) {
        QString detail = QStringLiteral("%1 %2x%3")
                             .arg(prefix,
                                  QString::number(m_activeCameraResolution.width()),
                                  QString::number(m_activeCameraResolution.height()));
        if (m_activeCameraFrameRate > 0.0) {
            detail.append(QStringLiteral(" @%1fps")
                              .arg(QString::number(m_activeCameraFrameRate,
                                                   'f',
                                                   m_activeCameraFrameRate < 10.0 ? 1 : 0)));
        }
        m_ui->cameraFormatHintLabel->setText(detail);
        QString tooltip = detail;
        if (!m_cameraBackendName.isEmpty()) {
            tooltip.append(QStringLiteral("\n采集后端：%1").arg(m_cameraBackendName));
        }
        m_ui->cameraFormatHintLabel->setToolTip(tooltip);
        return;
    }

    if (!isUsableCameraFormat(format)) {
        m_ui->cameraFormatHintLabel->setText(QStringLiteral("%1 无可用格式").arg(prefix));
        m_ui->cameraFormatHintLabel->setToolTip(QStringLiteral("%1：该设备未返回可用视频格式。").arg(prefix));
        return;
    }

    const QString formatText = cameraFormatDisplayText(format);
    m_ui->cameraFormatHintLabel->setText(QStringLiteral("%1 %2").arg(prefix, formatText));
    m_ui->cameraFormatHintLabel->setToolTip(QStringLiteral("%1：%2，共检测到 %3 种格式。")
                                                .arg(prefix,
                                                     formatText,
                                                     QString::number(device.videoFormats().size())));
}

bool MainWindow::ensureCameraRunning()
{
#ifndef LANLINKCHAT_HAS_OPENCV
    reportCameraIssue(QStringLiteral("当前构建未启用 OpenCV 采集，请安装 OpenCV 并让构建系统找到其 include/lib 后重新编译。"), true);
    return false;
#else
    if (m_cameraThread && m_cameraThread->isRunning()) {
        return true;
    }

    const QCameraDevice cameraDevice = selectedCameraDevice();
    if (cameraDevice.isNull()) {
        return false;
    }

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
    }

    const QCameraFormat requestedFormat = selectedCameraFormat(cameraDevice);
    const QSize requestedResolution = isUsableCameraFormat(requestedFormat)
        ? requestedFormat.resolution()
        : QSize(640, 360);
    const double requestedFrameRate = isUsableCameraFormat(requestedFormat) && requestedFormat.maxFrameRate() > 0.0
        ? requestedFormat.maxFrameRate()
        : 30.0;
    const QString devicePath = selectedCameraDevicePath();
    const int deviceIndex = selectedCameraDeviceIndex();

    qInfo().nospace()
        << "Requested OpenCV camera for " << cameraDevice.description()
        << ": path=" << devicePath
        << ", index=" << deviceIndex
        << ", resolution=" << requestedResolution.width() << "x" << requestedResolution.height()
        << ", maxFrameRate=" << requestedFrameRate;

    m_cameraActive = false;
    m_activeCameraResolution = QSize();
    m_activeCameraFrameRate = 0.0;
    m_cameraBackendName.clear();
    m_cameraThread->configure(devicePath,
                              deviceIndex,
                              cameraDevice.description(),
                              requestedResolution,
                              requestedFrameRate);
    m_cameraThread->start();
    if (!m_cameraThread->wait(1) && m_cameraThread->isRunning()) {
        updateCameraFormatHint();
        return true;
    }

    if (!m_cameraThread->isRunning() && !m_cameraActive) {
        return false;
    }

    updateCameraFormatHint();
    return true;
#endif
}

bool MainWindow::prepareCallCamera()
{
    if (!shouldSendLocalVideo()) {
        stopCamera();
        showStatusMessage(QStringLiteral("当前为仅接收图像模式，本地不会发送视频。"), 4000);
        return true;
    }

    if (ensureCameraRunning()) {
        return true;
    }

    const bool receiveOnlyEnabled = m_ui->receiveOnlyCheck && m_ui->receiveOnlyCheck->isChecked();
    if (!receiveOnlyEnabled) {
        reportCameraIssue(QStringLiteral("当前系统没有检测到可用摄像头。"), true);
        return false;
    }

    if (m_ui->callStatusLabel && !m_activeCallPeerId.isEmpty()) {
        m_ui->callStatusLabel->setText(QStringLiteral("正在与 %1 音视频通话（仅接收视频）").arg(peerName(m_activeCallPeerId)));
    }
    showStatusMessage(QStringLiteral("当前没有可用摄像头，已切换到仅接收模式。"), 4000);
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
        showStatusMessage(QStringLiteral("未检测到麦克风，将仅发送视频。"), 4000);
        return false;
    }

    m_audioInputFormat = inputDevice.preferredFormat();
    if (m_audioInputFormat.sampleFormat() == QAudioFormat::Unknown) {
        m_audioInputFormat.setSampleRate(48000);
        m_audioInputFormat.setChannelCount(1);
        m_audioInputFormat.setSampleFormat(QAudioFormat::Int16);
    }

    if (!inputDevice.isFormatSupported(m_audioInputFormat)) {
        showStatusMessage(QStringLiteral("当前麦克风格式不受支持，将仅发送视频。"), 4000);
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
        showStatusMessage(QStringLiteral("未检测到扬声器或耳机，无法播放对端语音。"), 4000);
        return false;
    }

    if (!outputDevice.isFormatSupported(format)) {
        showStatusMessage(QStringLiteral("当前扬声器不支持对端音频格式，语音已忽略。"), 4000);
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
    if (!m_cameraThread) {
        updateCameraFormatHint();
        return;
    }

    m_cameraThread->stopCapture();
    if (m_cameraThread->isRunning()) {
        m_cameraThread->wait();
    }
    m_cameraActive = false;
    m_activeCameraResolution = QSize();
    m_activeCameraFrameRate = 0.0;
    m_cameraBackendName.clear();
    m_lastLocalFrame = QImage();
    m_pendingLocalFrame = QImage();
    m_pendingVideoEncodeFrame = QImage();
    m_pendingVideoPeerId.clear();
    refreshVideoLabels();
    updateCameraFormatHint();
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

void MainWindow::showStatusMessage(const QString &message, int timeoutMs)
{
    const QString normalizedMessage = message.trimmed();
    if (normalizedMessage.isEmpty()) {
        return;
    }

    if (m_lastStatusMessage == normalizedMessage
        && m_statusMessageLimiter.isValid()
        && m_statusMessageLimiter.elapsed() < 1200) {
        return;
    }

    m_lastStatusMessage = normalizedMessage;
    m_statusMessageLimiter.restart();
    statusBar()->showMessage(normalizedMessage, timeoutMs);
}

void MainWindow::reportCameraIssue(const QString &message, bool showDialog)
{
    showStatusMessage(message, 5000);
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
    showStatusMessage(message, 5000);
    qWarning() << message;
}

void MainWindow::reportAudioOutputIssue(const QString &message)
{
    showStatusMessage(message, 5000);
    qWarning() << message;
}

void MainWindow::onVideoInputsChanged()
{
    const QByteArray currentId = m_ui->cameraCombo ? m_ui->cameraCombo->currentData().toByteArray() : QByteArray();
    populateCameraDevices();

    const bool cameraStillPresent = m_ui->cameraCombo
        && m_ui->cameraCombo->findData(currentId) >= 0;
    if (!currentId.isEmpty() && !cameraStillPresent) {
        reportCameraIssue(QStringLiteral("当前选择的摄像头已不可用，请重新选择设备。"), true);
        stopCamera();
    } else {
        showStatusMessage(QStringLiteral("摄像头设备列表已更新。"), 3000);
    }
}

void MainWindow::onAudioInputsChanged()
{
    showStatusMessage(QStringLiteral("麦克风设备列表已更新。"), 3000);
}

void MainWindow::onAudioOutputsChanged()
{
    showStatusMessage(QStringLiteral("扬声器设备列表已更新。"), 3000);
}

void MainWindow::onCameraErrorOccurred(const QString &message)
{
    reportCameraIssue(message, true);
}

void MainWindow::onCameraActiveChanged(bool active)
{
    m_cameraActive = active;
    if (active) {
        showStatusMessage(QStringLiteral("OpenCV 摄像头已启动。"), 2000);
        updateCameraFormatHint();
        return;
    }

    updateCameraFormatHint();
}

void MainWindow::onCameraCaptureFormatResolved(const QSize &resolution,
                                               double frameRate,
                                               const QString &backendName)
{
    m_activeCameraResolution = resolution;
    m_activeCameraFrameRate = frameRate;
    m_cameraBackendName = backendName;
    updateCameraFormatHint();
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
