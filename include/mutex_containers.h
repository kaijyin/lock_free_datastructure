#pragma once
/**
 * MutexQueue - 基于 std::mutex 的无界 MPMC 队列（对照组）
 * MutexRingBuffer - 基于 std::mutex 的有界 MPMC 环形缓冲区（对照组）
 *
 * 用途：作为无锁数据结构的性能基准对照
 */
#include <condition_variable>
#include <deque>
#include <mutex>

namespace lf {

// ============================================================
// MutexQueue - 无界队列，mutex 保护
// ============================================================
template <typename T> class MutexQueue {
  std::deque<T> queue_;
  mutable std::mutex mutex_;

public:
  MutexQueue() = default;
  MutexQueue(const MutexQueue &) = delete;
  MutexQueue &operator=(const MutexQueue &) = delete;

  bool push(T item) {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.push_back(std::move(item));
    return true; // 无界队列永远成功
  }

  bool pop(T &item) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (queue_.empty())
      return false;
    item = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.empty();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return queue_.size();
  }
};

// ============================================================
// MutexRingBuffer - 有界环形缓冲区，mutex 保护
// ============================================================
template <typename T, size_t Capacity> class MutexRingBuffer {
  static_assert(Capacity > 0, "Capacity must be > 0");

  T buffer_[Capacity];
  size_t head_{0}; // 下一个读位置
  size_t tail_{0}; // 下一个写位置
  size_t size_{0};
  mutable std::mutex mutex_;

public:
  MutexRingBuffer() = default;
  MutexRingBuffer(const MutexRingBuffer &) = delete;
  MutexRingBuffer &operator=(const MutexRingBuffer &) = delete;

  bool push(T item) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (size_ == Capacity)
      return false;
    buffer_[tail_] = std::move(item);
    tail_ = (tail_ + 1) % Capacity;
    ++size_;
    return true;
  }

  bool pop(T &item) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (size_ == 0)
      return false;
    item = std::move(buffer_[head_]);
    head_ = (head_ + 1) % Capacity;
    --size_;
    return true;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return size_ == 0;
  }

  bool full() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return size_ == Capacity;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return size_;
  }

  static constexpr size_t capacity() noexcept { return Capacity; }
};

} // namespace lf
