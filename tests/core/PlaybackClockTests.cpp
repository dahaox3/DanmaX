#include "core/PlaybackClock.h"
#include "core/MediaClock.h"
#include "core/FramePacer.h"
#include <cmath>
#include <chrono>
#include <iostream>

using namespace std::chrono_literals;
int main() {
    danmaku::PlaybackClock clock;
    auto check = [](bool condition, const char* message) {
        if (!condition)
            std::cerr << message << '\n';
        return condition;
    };
    bool ok = true;
    danmaku::FramePacer pacer;
    pacer.configure(70); pacer.published(0);
    ok &= check(pacer.delay(0) == 14285715, "Fractional frame period uses nanosecond deadlines");
    pacer.published(15000000);
    ok &= check(pacer.delay(15000000) == 13571429, "Late wakeup does not shift every later frame");
    pacer.published(1000000000);
    ok &= check(pacer.delay(1000000000) == 14285715, "A long stall skips missed frames");
    pacer.configure(144); pacer.published(0);
    for (int i = 1; i < 2000; ++i) {
        const auto deadline = (std::int64_t(i) * 1000000000 + 143) / 144;
        pacer.published(deadline + 200000);
        const auto next = (std::int64_t(i + 1) * 1000000000 + 143) / 144;
        ok &= check(pacer.delay(deadline + 200000) == next - deadline - 200000, "Pacing phase never accumulates wakeup drift");
    }
    pacer.reset(); ok &= check(pacer.delay(1234) == 0, "Resume starts with an immediate frame");
    pacer.configure(0); pacer.published(1234); ok &= check(pacer.delay(1234) == 0, "Uncapped mode stays display driven");
    clock.advance(20ms);
    ok &= check(clock.position() == 0us, "Initially paused clock must not advance");
    clock.setPaused(false);
    clock.advance(9ms);
    clock.advance(27ms);
    ok &= check(clock.position() == 36ms, "Variable frame intervals must accumulate exactly");
    clock.setPaused(true);
    clock.advance(10s);
    ok &= check(clock.position() == 36ms, "Pause must exclude wall-clock time");
    clock.setPaused(false);
    clock.advance(-1s);
    clock.advance(14ms);
    ok &= check(clock.position() == 50ms, "Resume must not jump or accept negative deltas");
    clock.reset();
    ok &= check(clock.position() == 0us && !clock.paused(), "Reset preserves playback state");
    danmaku::MediaClock media;
    media.synchronize(10, 1, true, 0, true);
    media.advance(0.2);
    media.synchronize(10.2, 2, true, 0.2);
    media.advance(0.3);
    ok &= check(std::abs(media.position() - 10.4) < 1e-9 &&
                std::abs(media.takeMotion() - 0.4) < 1e-9, "Rate change divides motion at sample boundary");
    media.synchronize(10.4, 2, false, 0.3);
    media.advance(100);
    ok &= check(std::abs(media.position() - 10.4) < 1e-9 && media.takeMotion() == 0,
                "Paused media excludes wall time");
    media.synchronize(10.4, 0.5, true, 100);
    media.advance(102);
    ok &= check(std::abs(media.takeMotion() - 1) < 1e-9, "Resume and half speed");
    ok &= check(media.synchronize(3, 1, true, 102), "Backward seek reported");
    ok &= check(media.takeMotion() == 0 && media.position() == 3, "Seek resets motion");
    media.freeze(103);
    media.takeMotion();
    media.advance(200);
    ok &= check(media.position() == 4 && media.takeMotion() == 0, "Timeout freeze");
    return ok ? 0 : 1;
}
