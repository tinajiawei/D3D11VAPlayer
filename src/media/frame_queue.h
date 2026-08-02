#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

#include "core/av_utils.h"

namespace me {

// 帧队列：解码线程（生产者）→ 渲染/消费线程（消费者）。
// 与 PacketQueue 的关键区别：
//  - 上限很小（视频 8 帧左右）：解码后的帧占内存大，越靠后环节缓冲越小；
//  - pop 不阻塞：渲染线程宁可短暂空转，也不能被队列卡住而无法响应 seek/暂停。
class FrameQueue {
public:
    explicit FrameQueue(size_t max_frames);

    bool push(AvFramePtr frame);  // 满时阻塞；abort 后返回 false
    AvFramePtr pop_front();       // 非阻塞：空则返回 nullptr
    void flush();
    void abort();
    void reset();  // 退出后复用队列（重开文件）：清除终止态与内容

    bool empty() const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<AvFramePtr> queue_;
    size_t max_frames_ = 0;
    bool aborted_ = false;
};

}  // namespace me
