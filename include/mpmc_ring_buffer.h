#pragma once
/**
 * MPMCRingBuffer - 多生产者/多消费者无锁环形缓冲区
 *
 * 算法：Dmitry Vyukov 的 MPMC 有界队列（序列号方案）
 * 参考：https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
 *
 * 核心思路：
 *   每个 slot 维护一个序列号(sequence)，编码当前状态：
 *     seq == pos           → slot 空闲，可写入（生产者可 CAS）
 *     seq == pos + 1       → slot 已写，可读取（消费者可 CAS）
 *     seq  < pos           → slot 仍被占用（队列满）
 *     seq  > pos + 1       → slot 已被后续 slot
 * 周期覆盖（不可能在正常运行中出现）
 *
 *   初始化：slot[i].seq = i
 *   生产者成功写入 pos p 后：slot[p & MASK].seq = p + 1
 *   消费者成功读出 pos p 后：slot[p & MASK].seq = p + Capacity
 *
 * 每个 slot 独占一条 cache line（alignas(64)），消除 false sharing。
 *
个人理解：
就是每个slot维护一个序号，
然后生产者也有自己的序列号，自己不停地增加，放入。
消费者也有自己的序列号，自己不停地增加，拿出。
每个slot的序号和生产者的序号对比，如果相等，说明这个slot是空的，可以放入；如果消费者的序号和slot的序号相
等，说明这个slot是满的，可以拿出；如果消费者的序号比slot的序号小，说明这个slot还在被占用，队列满了；如果消费者的序号比slot的序号大，说明这个slot已经被后续的生产者占用了。
所以生产者和消费者通过比较自己的序号和slot的序号来判断这个slot的状态，从而决定是否可以放入或者拿出数据。
 * 约束：
 *   - Capacity 必须是 2 的幂
 *   - 支持任意数量的生产者/消费者线程（MPMC）
 */
#include <atomic>
#include <cstddef>

namespace lf {

template <typename T, size_t Capacity> class MPMCRingBuffer {
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of 2");
  static constexpr size_t MASK = Capacity - 1;

  /**
   * 每个 slot 占一个完整 cache line：
   *   - sequence 在前，保证 atomic load 时不跨越 cache line
   *   - data 紧随其后
   *   - 整体对齐到 64 字节，数组中相邻 slot 不共享 cache line
   */
  struct alignas(64) Slot {
    std::atomic<size_t> sequence{0};
    T data{};
  };

  alignas(64) Slot buffer_[Capacity];
  alignas(64) std::atomic<size_t> enqueue_pos_{0};
  alignas(64) std::atomic<size_t> dequeue_pos_{0};

public:
  MPMCRingBuffer() noexcept {
    for (size_t i = 0; i < Capacity; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MPMCRingBuffer(const MPMCRingBuffer &) = delete;
  MPMCRingBuffer &operator=(const MPMCRingBuffer &) = delete;

  /**
   * push - 多线程安全入队
   * @return true 成功，false 队列满
   */
  bool push(T item) noexcept {
    size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = buffer_[pos & MASK];
      const size_t seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        // slot 空闲，尝试占位
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                               std::memory_order_relaxed)) {
          // CAS 成功：独占此 slot，写入数据并发布
          slot.data = std::move(item);
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
        // CAS 失败：其他生产者抢先，pos 已被 compare_exchange_weak
        // 更新为新值，继续
      } else if (diff < 0) {
        // seq < pos：slot 仍被消费者占用（队列满）
        return false;
      } else {
        // diff > 0：pos 落后于 enqueue_pos_，重新读取
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  /**
   * pop - 多线程安全出队
   * @return true 成功，false 队列空
   */
  bool pop(T &item) noexcept {
    size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    for (;;) {
      Slot &slot = buffer_[pos & MASK];
      const size_t seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff =
          static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

      if (diff == 0) {
        // slot 已写，尝试占位消费
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                                               std::memory_order_relaxed)) {
          item = std::move(slot.data);
          // 释放 slot：seq = pos + Capacity 表示此 slot 可被下一轮生产者使用
          slot.sequence.store(pos + Capacity, std::memory_order_release);
          return true;
        }
        // CAS 失败：其他消费者抢先，继续
      } else if (diff < 0) {
        // seq < pos + 1：slot 尚未写入（队列空）
        return false;
      } else {
        // diff > 0：pos 落后，重新读取
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  size_t size_approx() const noexcept {
    return enqueue_pos_.load(std::memory_order_acquire) -
           dequeue_pos_.load(std::memory_order_acquire);
  }

  bool empty() const noexcept { return size_approx() == 0; }
  bool full() const noexcept { return size_approx() >= Capacity; }
  static constexpr size_t capacity() noexcept { return Capacity; }
};

} // namespace lf
