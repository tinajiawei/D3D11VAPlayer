#include "media/frame_queue.h"

namespace me {

FrameQueue::FrameQueue(size_t max_frames) : max_frames_(max_frames) {}

bool FrameQueue::push(AvFramePtr frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return aborted_ || queue_.size() < max_frames_; });
    if (aborted_) return false;
    queue_.push_back(std::move(frame));
    cv_.notify_one();
    return true;
}

AvFramePtr FrameQueue::pop_front() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return nullptr;
    AvFramePtr frame = std::move(queue_.front());
    queue_.pop_front();
    cv_.notify_one();  // 腾出空间，唤醒阻塞在 push 上的生产者（缺失会导致解码线程永久沉睡）
    return frame;
}

void FrameQueue::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    cv_.notify_all();
}

void FrameQueue::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    queue_.clear();
    cv_.notify_all();
}

void FrameQueue::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = false;
    queue_.clear();
    cv_.notify_all();
}

bool FrameQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

size_t FrameQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace me
