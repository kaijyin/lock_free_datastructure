#pragma once
/**
 * SPSCRingBuffer - 单生产者/单消费者无锁环形缓冲区
 *
 * 算法原理:
 *   - write_pos_ 仅由生产者线程写入
 *   - read_pos_  仅由消费者线程写入
 *   - 两者各占独立的 cache line，彻底消除 false sharing
 *   - 使用"缓存对端指针"技术减少跨线程的 cache coherence 流量:
 *     生产者缓存上次读到的 read_pos，仅在确实满时才重新 acquire load
 *     消费者同理缓存 write_pos
 *   - 单调递增计数器 + 2^N 容量 → 用位掩码代替取模，零开销索引计算
 *
 * 内存序:
 *   store(release) 配对 load(acquire)，确保数据可见性
 *   不使用 seq_cst，减少 MFENCE 指令开销
 *
 * 实际就是因为满了也没关系，不阻塞直接返回。所以每次都就算读过去的容量，大不了失败就是，不会对正确性产生影响。
 * 通过引入cached_read和cached_write，只在满或者空时才用acquire
 * load更新它们，减少了不必要的acquire load，提升性能。
 * 约束:
 *   - Capacity 必须是 2 的幂
 *   - 严格 1 个生产者线程 + 1 个消费者线程
 */
#include <atomic>
#include <cassert>
#include <cstddef>
#include <type_traits>

namespace lf {

template <typename T, size_t Capacity> class SPSCRingBuffer {
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of 2");
  static constexpr size_t MASK = Capacity - 1;

  // ---- 生产者侧：一个 cache line ----
  struct alignas(64) ProducerSide {
    std::atomic<size_t> pos{0}; // 生产者写，消费者读(acquire)
    size_t cached_read{0};      // 仅生产者使用，避免频繁 acquire load
  };

  // ---- 消费者侧：一个 cache line ----
  struct alignas(64) ConsumerSide {
    std::atomic<size_t> pos{0}; // 消费者写，生产者读(acquire)
    size_t cached_write{0};     // 仅消费者使用
  };

  ProducerSide prod_;
  ConsumerSide cons_;

  // buffer 放在独立 cache line，避免与控制变量共享
  alignas(64) T buffer_[Capacity];

public:
  SPSCRingBuffer() = default;

  // 禁止拷贝/移动（含有 atomic 成员）
  SPSCRingBuffer(const SPSCRingBuffer &) = delete;
  SPSCRingBuffer &operator=(const SPSCRingBuffer &) = delete;

  /**
   * push - 仅由生产者线程调用
   * @return true 成功入队，false 队列已满
   */
  bool push(T item) noexcept {
    const size_t wp = prod_.pos.load(std::memory_order_relaxed);
    // 先用缓存值判断是否满；缓存值是保守的（只会让我们以为更满），是安全的
    if (wp - prod_.cached_read >= Capacity) {
      prod_.cached_read = cons_.pos.load(std::memory_order_acquire);
      if (wp - prod_.cached_read >= Capacity) {
        return false; // 真的满了
      }
    }
    buffer_[wp & MASK] = std::move(item);
    // release：确保数据写入在 pos 更新之前对消费者可见
    prod_.pos.store(wp + 1, std::memory_order_release);
    return true;
  }

  /**
   * pop - 仅由消费者线程调用
   * @return true 成功出队，false 队列为空
   */
  bool pop(T &item) noexcept {
    const size_t rp = cons_.pos.load(std::memory_order_relaxed);
    if (rp == cons_.cached_write) {
      cons_.cached_write = prod_.pos.load(std::memory_order_acquire);
      if (rp == cons_.cached_write) {
        return false; // 真的空了
      }
    }
    item = std::move(buffer_[rp & MASK]);
    cons_.pos.store(rp + 1, std::memory_order_release);
    return true;
  }

  /** 近似 size，非原子读，仅供参考 */
  size_t size_approx() const noexcept {
    return prod_.pos.load(std::memory_order_acquire) -
           cons_.pos.load(std::memory_order_acquire);
  }

  bool empty() const noexcept { return size_approx() == 0; }
  bool full() const noexcept { return size_approx() >= Capacity; }
  static constexpr size_t capacity() noexcept { return Capacity; }
};

} // namespace lf
