#pragma once
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <memory>

class QQuickWindow;
class DanmakuItem;

class FrameTraceRecorder final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
  public:
    struct Context {
        quint64 tick{}, mediaGeneration{}, resets{};
        qint64 tickNs{};
        double position{}, rate{};
        int maxFps{};
        bool playing{}, visible{};
        int effectiveMaxFps{};
    };
    explicit FrameTraceRecorder(const QString& dataDirectory);
    ~FrameTraceRecorder() override;
    bool enabled() const { return armed_ || recording(); }
    bool recording() const { return bool(capture_); } // GUI thread
    QString status() const { return status_; }
    void setEnabled(bool enabled);
    void setPlaybackRunning(bool running);
    void attach(QQuickWindow* window, DanmakuItem* item, std::function<Context()> context);
  signals:
    void changed();
  private:
    struct Capture;
    void start();
    void finish(const QString& reason);
    void setStatus(const QString& status);
    QString directory_, marker_, status_;
    bool armed_{}, running_{};
    QPointer<QQuickWindow> window_;
    QPointer<DanmakuItem> item_;
    std::function<Context()> context_;
    QList<QMetaObject::Connection> connections_;
    std::shared_ptr<Capture> capture_;
    QTimer deadline_;
    QThreadPool writer_;
    quint64 serial_{};
};
