#include "test_common.h"

#include <chrono>
#include <cmath>
#include <thread>

#include "core/clock.h"

void test_clock() {
    std::fprintf(stderr, "[test] PlaybackClock\n");
    {
        me::PlaybackClock clock;
        clock.set_pos(10.0);
        CHECK(std::fabs(clock.get() - 10.0) < 1e-3);  // 1ms 容差：QPC 两次调用间隔可能超过 1µs
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(clock.get() > 10.04);  // 1.0x 下应前进约 50ms
    }
    {
        // 变速：2.0x 下前进速度翻倍
        me::PlaybackClock clock;
        clock.set_pos(0.0);
        clock.set_rate(2.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(clock.get() > 0.19);
    }
    {
        // 暂停：位置冻结
        me::PlaybackClock clock;
        clock.set_pos(5.0);
        clock.set_paused(true);
        const double frozen = clock.get();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_EQ(static_cast<long long>(clock.get() * 1000),
                 static_cast<long long>(frozen * 1000));
        clock.set_paused(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(clock.get() > frozen + 0.04);
    }
    {
        // set_pos 后立即跳变（seek 语义）
        me::PlaybackClock clock;
        clock.set_pos(100.0);
        CHECK(std::fabs(clock.get() - 100.0) < 1e-3);
    }
}
