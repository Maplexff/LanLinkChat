#pragma once

#include <QImage>
#include <QPixmap>
#include <QWidget>

class VideoFrameWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoFrameWidget(QWidget *parent = nullptr);
    explicit VideoFrameWidget(const QString &placeholderText, QWidget *parent);

    void setFrame(const QImage &frame);
    void clearFrame();
    void setPlaceholderText(const QString &placeholderText);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateScaledFrameCache();

    QImage m_frame;
    QPixmap m_scaledFrame;
    QRect m_scaledFrameRect;
    QSize m_cachedContentSize;
    qint64 m_cachedFrameKey = 0;
    QString m_placeholderText;
};
