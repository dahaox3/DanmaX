#include "platform/windows/FrameWakeup.h"
#include <windows.h>
#include <algorithm>

FrameWakeup::FrameWakeup(QObject* parent) : QObject(parent) {
    handle_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (handle_) notifier_.setHandle(handle_);
    notifier_.setEnabled(false);
    fallback_.setSingleShot(true); fallback_.setTimerType(Qt::PreciseTimer);
    connect(&notifier_, &QWinEventNotifier::activated, this, &FrameWakeup::ready);
    connect(&fallback_, &QChronoTimer::timeout, this, &FrameWakeup::ready);
}
FrameWakeup::~FrameWakeup() {
    stop(); notifier_.setHandle(nullptr);
    if (handle_) CloseHandle(handle_);
}
void FrameWakeup::start(std::chrono::nanoseconds delay) {
    stop();
    active_ = true;
    if (handle_) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, (delay.count() + 99) / 100);
        if (SetWaitableTimerEx(handle_, &due, 0, nullptr, nullptr, nullptr, 0)) {
            notifier_.setEnabled(true); return;
        }
    }
    fallback_.setInterval(std::max(delay, std::chrono::nanoseconds(1)));
    fallback_.start();
}
void FrameWakeup::stop() {
    active_ = false; notifier_.setEnabled(false); fallback_.stop();
    if (handle_) CancelWaitableTimer(handle_);
}
void FrameWakeup::ready() {
    if (!active_) return;
    stop(); emit timeout();
}
