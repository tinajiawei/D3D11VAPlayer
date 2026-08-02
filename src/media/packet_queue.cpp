#include "media/packet_queue.h"

namespace me {

PacketQueue::PacketQueue(size_t max_packets, size_t max_bytes)
    : max_packets_(max_packets), max_bytes_(max_bytes) {}

void PacketQueue::push(AvPacketPtr packet, uint64_t gen) {
    std::unique_lock<std::mutex> lock(mutex_);

    // EOF 哨兵必须无条件入队（它没有大小，也不能被背压挡住，否则消费者永远等不到结束信号）。
    if (!packet) {
        queue_.push_back(nullptr);
        gens_.push_back(gen);
        cv_.notify_all();
        return;
    }

    // 有界背压：满了就等，直到被消费或 abort。
    cv_.wait(lock, [&] {
        return aborted_ ||
               (queue_.size() < max_packets_ && bytes_ + static_cast<size_t>(packet->size) <= max_bytes_);
    });
    if (aborted_) return;

    bytes_ += static_cast<size_t>(packet->size);
    queue_.push_back(std::move(packet));
    gens_.push_back(gen);
    cv_.notify_one();
}

AvPacketPtr PacketQueue::pop(uint64_t* out_gen) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return aborted_ || !queue_.empty(); });
    if (queue_.empty()) return nullptr;  // 只有 abort 后才会走到这里

    AvPacketPtr packet = std::move(queue_.front());
    if (out_gen) *out_gen = gens_.empty() ? 0 : gens_.front();
    queue_.pop_front();
    if (!gens_.empty()) gens_.pop_front();
    if (packet) bytes_ -= static_cast<size_t>(packet->size);
    // 腾出空间后唤醒可能阻塞在 push 的生产者
    if (packet) cv_.notify_one();
    return packet;
}

void PacketQueue::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    gens_.clear();
    bytes_ = 0;
    cv_.notify_all();
}
void PacketQueue::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    queue_.clear();
    gens_.clear();
    bytes_ = 0;
    cv_.notify_all();  // 必须唤醒：否则阻塞在 pop/push 的线程永远醒不过来
}
void PacketQueue::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = false;
    queue_.clear();
    gens_.clear();
    bytes_ = 0;
    cv_.notify_all();
}
bool PacketQueue::aborted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aborted_;
}

size_t PacketQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace me
