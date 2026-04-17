#include "videoframewidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

VideoFrameWidget::VideoFrameWidget(QWidget *parent)
    : VideoFrameWidget(QString(), parent)
{
}

VideoFrameWidget::VideoFrameWidget(const QString &placeholderText, QWidget *parent)
    : QWidget(parent)
    , m_placeholderText(placeholderText)
{
    setMinimumSize(320, 240);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void VideoFrameWidget::setFrame(const QImage &frame)
{
    m_frame = frame;
    m_scaledFrame = QPixmap();
    m_scaledFrameRect = QRect();
    m_cachedContentSize = QSize();
    m_cachedFrameKey = m_frame.isNull() ? 0 : m_frame.cacheKey();
    update();
}

void VideoFrameWidget::clearFrame()
{
    if (m_frame.isNull()) {
        update();
        return;
    }

    m_frame = QImage();
    m_scaledFrame = QPixmap();
    m_scaledFrameRect = QRect();
    m_cachedContentSize = QSize();
    m_cachedFrameKey = 0;
    update();
}

void VideoFrameWidget::setPlaceholderText(const QString &placeholderText)
{
    if (m_placeholderText == placeholderText) {
        return;
    }

    m_placeholderText = placeholderText;
    update();
}

void VideoFrameWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    m_scaledFrame = QPixmap();
    m_scaledFrameRect = QRect();
    m_cachedContentSize = QSize();
}

void VideoFrameWidget::updateScaledFrameCache()
{
    if (m_frame.isNull()) {
        m_scaledFrame = QPixmap();
        m_scaledFrameRect = QRect();
        m_cachedContentSize = QSize();
        m_cachedFrameKey = 0;
        return;
    }

    const QRect contentRect = rect().adjusted(8, 8, -8, -8);
    if (!contentRect.isValid()) {
        m_scaledFrame = QPixmap();
        m_scaledFrameRect = QRect();
        m_cachedContentSize = QSize();
        return;
    }

    const qint64 frameKey = m_frame.cacheKey();
    if (!m_scaledFrame.isNull()
        && m_cachedContentSize == contentRect.size()
        && m_cachedFrameKey == frameKey) {
        return;
    }

    const QSize scaledSize = m_frame.size().scaled(contentRect.size(), Qt::KeepAspectRatio);
    if (!scaledSize.isValid()) {
        m_scaledFrame = QPixmap();
        m_scaledFrameRect = QRect();
        m_cachedContentSize = QSize();
        return;
    }

    const QImage scaledImage = m_frame.size() == scaledSize
        ? m_frame
        : m_frame.scaled(scaledSize, Qt::KeepAspectRatio, Qt::FastTransformation);
    m_scaledFrame = QPixmap::fromImage(scaledImage);
    m_scaledFrameRect = QRect(QPoint((width() - scaledSize.width()) / 2,
                                     (height() - scaledSize.height()) / 2),
                              scaledSize);
    m_cachedContentSize = contentRect.size();
    m_cachedFrameKey = frameKey;
}

void VideoFrameWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor(17, 17, 17));
    painter.setPen(QColor(68, 68, 68));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));

    const QRect contentRect = rect().adjusted(8, 8, -8, -8);
    if (!m_frame.isNull()) {
        updateScaledFrameCache();
        if (!m_scaledFrame.isNull() && m_scaledFrameRect.isValid()) {
            painter.drawPixmap(m_scaledFrameRect.topLeft(), m_scaledFrame);
            return;
        }
    }

    painter.setPen(QColor(221, 221, 221));
    painter.drawText(contentRect, Qt::AlignCenter, m_placeholderText);
}
