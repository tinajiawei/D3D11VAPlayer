#include "test_common.h"

#include <atomic>
#include <thread>

#include "core/ring_buffer.h"

void test_ring_buffer() {
    std::fprintf(stderr, "[test] RingBuffer\n");
    {
        // 基本 FIFO
        me::RingBuffer<int> rb(4);
        CHECK_EQ(rb.capacity(), 4u);
        CHECK(rb.empty());

        int in[] = {1, 2, 3, 4};
        CHECK_EQ(rb.push(in, 4), 4u);
        CHECK_EQ(rb.size(), 4u);

        int out[4] = {};
        CHECK_EQ(rb.pop(out, 2), 2u);
        CHECK_EQ(out[0], 1);
        CHECK_EQ(out[1], 2);
        CHECK_EQ(rb.size(), 2u);
    }
    {
        // 满时丢弃最旧：容量 3 塞 4 个，只保留最新 3 个，返回实际写入 3
        me::RingBuffer<int> rb(3);
        int in[] = {1, 2, 3, 4};
        CHECK_EQ(rb.push(in, 4, /*drop_oldest=*/true), 3u);
        CHECK_EQ(rb.size(), 3u);
        int out[3] = {};
        CHECK_EQ(rb.pop(out, 3), 3u);
        CHECK_EQ(out[0], 2);  // 1 被丢弃，保留 2/3/4
    }
    {
        // 生产者/消费者并发：大缓冲区 + 不触发丢弃，总数守恒
        me::RingBuffer<int> rb(20000);
        std::atomic<long long> produced{0}, consumed{0};
        std::thread writer([&] {
            for (int i = 0; i < 10000; ++i) {
                if (rb.push(&i, 1, /*drop_oldest=*/false)) ++produced;
            }
        });
        std::thread reader([&] {
            int v;
            while (consumed < 10000) {
                if (rb.pop(&v, 1)) ++consumed;
            }
        });
        writer.join();
        reader.join();
        CHECK_EQ(produced.load(), 10000ll);
        CHECK_EQ(consumed.load(), 10000ll);
    }
    {
        // push_blocking 单块大于容量：分块写入，不能死锁（旧实现 n>capacity 会永久等待）
        me::RingBuffer<int> rb(8);
        std::atomic<bool> writer_done{false};
        std::thread writer([&] {
            int data[64];
            for (int i = 0; i < 64; ++i) data[i] = i;
            rb.push_blocking(data, 64);
            writer_done.store(true);
        });
        int out[64] = {};
        size_t total = 0;
        while (total < 64) {
            total += rb.pop(out + total, 8);
        }
        writer.join();
        CHECK(writer_done.load());
        for (int i = 0; i < 64; ++i) CHECK_EQ(out[i], i);
    }
}
