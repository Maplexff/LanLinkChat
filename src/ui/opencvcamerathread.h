#pragma once

#include <QImage>
#include <QMutex>
#include <QSize>
#include <QString>
#include <QThread>

#include <atomic>

class OpenCvCameraThread : public QThread
{
    Q_OBJECT

public:
    explicit OpenCvCameraThread(QObject *parent = nullptr);
    ~OpenCvCameraThread() override;

    void configure(const QString &devicePath,
                   int deviceIndex,
                   const QString &deviceDescription,
                   const QSize &resolution,
                   double targetFrameRate);
    void stopCapture();

signals:
    void frameCaptured(const QImage &frame);
    void cameraError(const QString &message);
    void cameraActiveChanged(bool active);
    void captureFormatResolved(const QSize &resolution,
                               double frameRate,
                               const QString &backendName);

protected:
    void run() override;

private:
    QString m_devicePath;
    int m_deviceIndex = -1;
    QString m_deviceDescription;
    QSize m_resolution;
    double m_targetFrameRate = 30.0;
    QMutex m_mutex;
    std::atomic_bool m_stopRequested = false;
};
