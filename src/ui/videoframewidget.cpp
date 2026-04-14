#include "videoframewidget.h"

#include <QPainter>
#include <QPaintEvent>

VideoFrameWidget::VideoFrameWidget(const QString &placeholderText, QWidget *parent)
    : QWidget(parent)
    , m_placeholderText(placeholderText)
{
    setMinimumSize(320, 240);
    setAutoFillBackground(false);
}

void VideoFrameWidget::setFrame(const QImage &frame)
{
    m_frame = frame.isNull() ? QImage() : frame.copy();
    update();
}

void VideoFrameWidget::clearFrame()
{
    if (m_frame.isNull()) {
        update();
        return;
    }

    m_frame = QImage();
    update();
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
        const QSize targetSize = m_frame.size().scaled(contentRect.size(), Qt::KeepAspectRatio);
        const QRect targetRect(QPoint((width() - targetSize.width()) / 2,
                                      (height() - targetSize.height()) / 2),
                               targetSize);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(targetRect, m_frame);
        return;
    }

    painter.setPen(QColor(221, 221, 221));
    painter.drawText(contentRect, Qt::AlignCenter, m_placeholderText);
}
