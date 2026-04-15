#pragma once

#include <QImage>
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

private:
    QImage m_frame;
    QString m_placeholderText;
};
