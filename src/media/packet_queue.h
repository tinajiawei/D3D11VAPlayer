#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

#include "core/av_utils.h"

namespace me {

// 生产-消费者队列：解封装线程（生产者）→ 解码线程（消费者）。
// 语义：
//  - push：队列满时阻塞（背压，限速生产者）；
//  - pop： 队列空时阻塞（让消费者睡下，不空转）；
//  - push(nullptr)：EOF 哨兵，表示"后面没有数据了"，放在所有包之后；
//  - flush：临时清空（seek 用），队列仍可继续使用；
//  - abort：永久终止（退出用），唤醒所有阻塞线程，之后不再收数据。
class PacketQueue {
public:
    explicit PacketQueue(size_t max_packets = 1024, size_t max_bytes = 32 * 1024 * 1024);

    void push(AvPacketPtr packet, uint64_t gen = 0);  // nullptr => EOF 哨兵；gen=入队时的 seek 代数
    AvPacketPtr pop(uint64_t* out_gen = nullptr);       // nullptr => EOF 哨兵或已 abort；out_gen 返回该包代数
    void flush();
    void abort();
    void reset();  // 退出后复用队列（重开文件）：清除终止态与内容

    bool aborted() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AvPacketPtr> queue_;
    std::deque<uint64_t> gens_;  // 与 queue_ 同步的 seek 代数（防旧包混入新 seek）
    size_t max_packets_ = 0;
    size_t max_bytes_ = 0;
    size_t bytes_ = 0;
    bool aborted_ = false;
};

}  // namespace me
