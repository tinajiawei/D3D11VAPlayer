#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

namespace me {

// 线程安全环形缓冲。用于 WASAPI 回调（消费者）与音频解码线程（生产者）之间的 PCM 交换。
// 与包/帧队列不同：这里全部操作都是非阻塞的——
// 音频回调是系统硬实时线程，在里面阻塞会爆音，宁可丢数据/写静音也不能等。
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity) : data_(capacity), capacity_(capacity) {}

    void resize_capacity(size_t new_capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<T> new_data(new_capacity);
        const size_t keep = std::min(count_, new_capacity);
        for (size_t i = 0; i < keep; ++i) {
            new_data[i] = data_[(head_ + count_ - keep + i) % capacity_];
        }
        data_ = std::move(new_data);
        capacity_ = new_capacity;
        head_ = 0;
        tail_ = keep % (capacity_ ? capacity_ : 1);
        count_ = keep;
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_ == 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = tail_ = count_ = 0;
        cv_.notify_all();
    }

    // 永久中止：唤醒所有阻塞的生产者（关闭播放器时调用，避免 join 卡死）。
    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        cv_.notify_all();
    }

    // 阻塞写入（音频解码线程用）：等待足够空间后一次性写入全部数据，实现背压。
    void push_blocking(const T* data, size_t n) {
        if (n > 4 * 1024 * 1024) return;  // 防御：异常大的写入（>16MB 样本）直接丢弃，避免永久等待
        if (n == 0) return;
        std::unique_lock<std::mutex> lock(mutex_);
        size_t written = 0;
        while (written < n) {
            // 单块可能大于环形容量（大帧 + 高倍速 + 多声道），分块写入而不是永久等待
            cv_.wait(lock, [&] { return aborted_ || capacity_ > count_; });
            if (aborted_) return;
            const size_t chunk = std::min(n - written, capacity_ - count_);
            for (size_t i = 0; i < chunk; ++i) {
                data_[(tail_ + i) % capacity_] = data[written + i];
            }
            tail_ = (tail_ + chunk) % capacity_;
            count_ += chunk;
            written += chunk;
            cv_.notify_all();
        }
    }

    // 非阻塞写入：空间不足时按 drop_oldest 决定丢弃最旧数据或直接失败。
    // 返回实际写入的元素个数。
    size_t push(const T* data, size_t n, bool drop_oldest = true) {
        if (n == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (capacity_ == 0) return 0;

        size_t space = capacity_ - count_;
        size_t skip = 0;
        if (n > space) {
            if (!drop_oldest) return 0;
            // 丢弃最旧元素并跳过输入头部，只保留最新内容（防下溢）
            const size_t overflow = n - space;
            const size_t drop = std::min(count_, overflow);
            count_ -= drop;
            head_ = (head_ + drop) % capacity_;
            skip = overflow - drop;
            space = capacity_ - count_;
        }
        const size_t write = std::min(n - skip, space);
        for (size_t i = 0; i < write; ++i) {
            data_[(tail_ + i) % capacity_] = data[skip + i];
        }
        tail_ = (tail_ + write) % capacity_;
        count_ += write;
        return write;
    }

    // 非阻塞读取：取走尽可能多的元素，返回实际取出的个数。
    size_t pop(T* out, size_t n) {
        if (n == 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t read = std::min(n, count_);
        for (size_t i = 0; i < read; ++i) {
            out[i] = data_[(head_ + i) % capacity_];
        }
        head_ = (head_ + read) % capacity_;
        count_ -= read;
        cv_.notify_all();  // 腾出空间，唤醒阻塞的 push_blocking 生产者
        return read;
    }

private:
    std::condition_variable cv_;
    mutable std::mutex mutex_;
    std::vector<T> data_;
    size_t capacity_ = 0;
    size_t head_ = 0;   // 读位置
    size_t tail_ = 0;   // 写位置
    size_t count_ = 0;  // 当前元素数（消除 head==tail 空/满歧义）
    bool aborted_ = false;
};

}  // namespace me
