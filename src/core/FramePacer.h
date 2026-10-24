#pragma once
#include <algorithm>
#include <cstdint>

namespace danmaku {
// Monotonic deadlines retain their phase when an OS wakeup arrives late.
// Missed slots are skipped; no queue of historical frames is generated.
class FramePacer {
  public:
    void configure(int fps) { fps_ = std::max(0, fps); reset(); }
    void reset() { origin_ = -1; next_ = 0; }
    bool started() const { return origin_ >= 0; }
    std::int64_t delay(std::int64_t now) const {
        return fps_ == 0 || !started() ? 0 : std::max<std::int64_t>(0, next_ - now);
    }
    void published(std::int64_t now) {
        if (fps_ == 0) return;
        if (!started()) origin_ = now;
        const auto elapsed = std::max<std::int64_t>(0, now - origin_);
        const auto slot = (elapsed / billion) * fps_ + (elapsed % billion) * fps_ / billion + 1;
        next_ = origin_ + (slot / fps_) * billion + ((slot % fps_) * billion + fps_ - 1) / fps_;
    }
  private:
    static constexpr std::int64_t billion = 1000000000;
    int fps_{};
    std::int64_t origin_{-1}, next_{};
};
}
