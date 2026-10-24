#pragma once
#include <QObject>
#include <QChronoTimer>
#include <QWinEventNotifier>
#include <chrono>

// One asynchronous Windows timer, with Qt's public event notifier dispatching
// back to the GUI thread. No sleeping/spinning in the GUI or per-frame threads.
class FrameWakeup final : public QObject {
    Q_OBJECT
  public:
    explicit FrameWakeup(QObject* parent = nullptr);
    ~FrameWakeup() override;
    void start(std::chrono::nanoseconds delay);
    void stop();
    bool isActive() const { return active_; }
    bool highResolution() const { return handle_ != nullptr; }
  signals:
    void timeout();
  private:
    void ready();
    void* handle_{};
    QWinEventNotifier notifier_;
    QChronoTimer fallback_;
    bool active_{};
};
