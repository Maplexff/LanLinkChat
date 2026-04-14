#pragma once

#include <QImage>
#include <QWidget>

class VideoFrameWidget : public QWidget
{
    Q_OBJECT

public:
    explicit VideoFrameWidget(const QString &placeholderText, QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void clearFrame();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_frame;
    QString m_placeholderText;
};
