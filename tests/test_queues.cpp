#include "test_common.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "media/packet_queue.h"

void test_queues() {
    std::fprintf(stderr, "[test] PacketQueue\n");
    {
        // 基本 FIFO + EOF 哨兵
        me::PacketQueue q(64, 1 << 20);
        for (int i = 0; i < 5; ++i) {
            auto pkt = me::make_packet();
            pkt->pts = i;
            pkt->size = 1;
            q.push(std::move(pkt));
        }
        q.push(nullptr);  // EOF
        int expected = 0;
        for (;;) {
            auto pkt = q.pop();
            if (!pkt) break;  // EOF 哨兵
            CHECK_EQ(static_cast<long long>(pkt->pts), static_cast<long long>(expected++));
        }
        CHECK_EQ(expected, 5);
    }
    {
        // 有界背压：队列大小不超过上限
        me::PacketQueue q(8, 1 << 20);
        std::atomic<int> max_seen{0};
        std::thread producer([&] {
            for (int i = 0; i < 100; ++i) {
                auto pkt = me::make_packet();
                pkt->size = 1;
                q.push(std::move(pkt));
                max_seen.store(static_cast<int>(q.size()));
            }
            q.push(nullptr);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(max_seen.load() <= 8);
        int count = 0;
        for (;;) {
            auto pkt = q.pop();
            if (!pkt) break;
            ++count;
            if (count % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        producer.join();
        CHECK_EQ(count, 100);
    }
    {
        // abort 唤醒阻塞的 pop
        me::PacketQueue q(4, 1 << 20);
        std::thread consumer([&] {
            auto pkt = q.pop();  // 空队列，会阻塞
            CHECK(pkt == nullptr);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.abort();
        consumer.join();
    }
    {
        // abort 后 reset：队列可复用（重开文件场景）
        me::PacketQueue q(4, 1 << 20);
        q.abort();
        auto pkt = q.pop();  // abort 后返回 nullptr
        CHECK(pkt == nullptr);
        q.reset();
        for (int i = 0; i < 3; ++i) {
            auto p = me::make_packet();
            p->pts = i;
            p->size = 1;
            q.push(std::move(p));
        }
        CHECK_EQ(q.size(), 3u);
        q.push(nullptr);
        int expected = 0;
        for (;;) {
            auto p = q.pop();
            if (!p) break;
            CHECK_EQ(static_cast<long long>(p->pts), static_cast<long long>(expected++));
        }
        CHECK_EQ(expected, 3);
    }
    {
        // flush 清空但队列仍可用
        me::PacketQueue q(64, 1 << 20);
        for (int i = 0; i < 3; ++i) {
            auto pkt = me::make_packet();
            pkt->size = 1;
            q.push(std::move(pkt));
        }
        q.flush();
        CHECK(q.size() == 0);
        auto pkt = me::make_packet();
        pkt->size = 1;
        q.push(std::move(pkt));
        CHECK(q.size() == 1);
        q.abort();
    }
}
