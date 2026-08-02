#include "test_common.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "media/frame_queue.h"

void test_frame_queue() {
    std::fprintf(stderr, "[test] FrameQueue\n");
    {
        // 基本 FIFO
        me::FrameQueue q(4);
        for (int i = 0; i < 3; ++i) {
            auto f = me::make_frame();
            f->pts = i;
            CHECK(q.push(std::move(f)));
        }
        int expected = 0;
        for (;;) {
            auto f = q.pop_front();
            if (!f) break;
            CHECK_EQ(static_cast<long long>(f->pts), static_cast<long long>(expected++));
        }
        CHECK_EQ(expected, 3);
        q.abort();
    }
    {
        // 回归测试（真实 bug）：帧队列满时 push 阻塞，pop 腾出空间后必须唤醒生产者。
        // 曾经 pop_front 缺少 notify_one，生产者永远沉睡，播放器只能消费 max_frames 帧。
        me::FrameQueue q(4);
        std::atomic<int> produced{0};
        std::thread producer([&] {
            for (int i = 0; i < 10; ++i) {
                auto f = me::make_frame();
                f->pts = i;
                if (q.push(std::move(f))) ++produced;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));  // 让生产者先塞满并阻塞

        int consumed = 0;
        for (;;) {
            auto f = q.pop_front();
            if (!f) {
                if (produced.load() >= 10) break;  // 生产者完成且队列取空
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            ++consumed;
        }
        producer.join();
        CHECK_EQ(produced.load(), 10);
        CHECK_EQ(consumed, 10);
        q.abort();
    }
}
