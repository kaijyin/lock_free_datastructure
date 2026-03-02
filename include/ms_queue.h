#pragma once
/**
 * MSQueue - Michael-Scott 无锁 MPMC 无界队列
 *
 * 算法：Michael & Scott 1996 论文的经典实现
 * 内存管理：使用对象池 + Treiber 无锁自由链表（ABA 保护）
 *
 * ABA 问题解决方案：
 *   使用带版本计数的标记指针（tagged pointer），打包为 uint64_t：
 *     bits [31:0]  = 节点池索引 (最大 2^32 节点)
 *     bits [63:32] = 版本计数器 (每次 CAS 成功递增)
 *   32 位版本计数在正常使用中不会发生回绕（超过 42 亿次 CAS 才会）
 *
 * 节点池：
 *   - 预分配固定大小的节点池
 *   - 分配：先从 Treiber 自由链表取，取不到再用原子计数器分配新节点
 *   - 回收：pop 后将旧哑节点归还自由链表
 *
 * 约束：
 *   - PoolSize 决定同时在队列中的最大节点数
 *   - 不支持动态扩容（有界池）
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lf {

template <typename T, size_t PoolSize = (1 << 17)> // 默认 128K 节点
class MSQueue {
  static constexpr uint32_t NULL_IDX = std::numeric_limits<uint32_t>::max();

  // ---- 标记指针工具函数 ----
  static uint64_t pack(uint32_t idx, uint32_t ver) noexcept {
    return (static_cast<uint64_t>(ver) << 32) | idx;
  }
  static uint32_t idx_of(uint64_t tp) noexcept {
    return static_cast<uint32_t>(tp);
  }
  static uint32_t ver_of(uint64_t tp) noexcept {
    return static_cast<uint32_t>(tp >> 32);
  }

  // ---- 节点结构 ----
  struct Node {
    T data{};
    // next 复用：队列中存队列后继，自由链表中存链表后继
    // 均为带版本的标记指针
    std::atomic<uint64_t> next{pack(NULL_IDX, 0)};
  };

  // ---- 节点池 ----
  Node pool_[PoolSize]; // pool_[0] 保留为初始哑节点

  // 自由链表（Treiber Stack）
  alignas(64) std::atomic<uint64_t> free_head_{pack(NULL_IDX, 0)};
  // 单调分配计数器（仅在自由链表为空时使用）
  alignas(64) std::atomic<uint32_t> alloc_cnt_{1}; // 0 为哑节点，从 1 开始分配

  // ---- 队列头尾（标记指针）----
  alignas(64) std::atomic<uint64_t> head_; // 始终指向哑节点
  alignas(64) std::atomic<uint64_t> tail_;

  // ---- 内存管理 ----
  uint32_t alloc_node() noexcept {
    // 先尝试从自由链表分配（CAS Treiber pop）
    for (;;) {
      uint64_t h = free_head_.load(std::memory_order_acquire);
      uint32_t idx = idx_of(h);
      if (idx == NULL_IDX)
        break; // 自由链表空

      uint64_t next = pool_[idx].next.load(std::memory_order_acquire);
      uint64_t new_h = pack(idx_of(next), ver_of(h) + 1);
      if (free_head_.compare_exchange_weak(h, new_h, std::memory_order_release,
                                           std::memory_order_relaxed)) {
        return idx;
      }
    }
    // 自由链表空，从池尾分配
    uint32_t idx = alloc_cnt_.fetch_add(1, std::memory_order_relaxed);
    return (idx < PoolSize) ? idx : NULL_IDX;
  }

  void free_node(uint32_t idx) noexcept {
    // Treiber Stack push
    for (;;) {
      uint64_t h = free_head_.load(std::memory_order_acquire);
      pool_[idx].next.store(h, std::memory_order_relaxed);
      uint64_t new_h = pack(idx, ver_of(h) + 1);
      if (free_head_.compare_exchange_weak(h, new_h, std::memory_order_release,
                                           std::memory_order_relaxed)) {
        return;
      }
    }
  }

public:
  MSQueue() noexcept {
    // pool_[0] 是永久哑节点，next 指向 NULL
    pool_[0].next.store(pack(NULL_IDX, 0), std::memory_order_relaxed);
    head_.store(pack(0, 0), std::memory_order_relaxed);
    tail_.store(pack(0, 0), std::memory_order_relaxed);
  }

  MSQueue(const MSQueue &) = delete;
  MSQueue &operator=(const MSQueue &) = delete;

  /**
   * push - 入队（永不阻塞，池耗尽时返回 false）
   */
  bool push(T item) noexcept {
    uint32_t new_idx = alloc_node();
    if (new_idx == NULL_IDX)
      return false;

    pool_[new_idx].data = std::move(item);
    pool_[new_idx].next.store(pack(NULL_IDX, 0), std::memory_order_relaxed);

    for (;;) {
      uint64_t t = tail_.load(std::memory_order_acquire);
      uint32_t t_idx = idx_of(t);
      uint64_t t_next = pool_[t_idx].next.load(std::memory_order_acquire);

      // 验证 tail 仍有效
      if (t != tail_.load(std::memory_order_acquire))
        continue;

      uint32_t t_next_idx = idx_of(t_next);
      if (t_next_idx == NULL_IDX) {
        // tail 指向最后一个节点，尝试链接新节点
        uint64_t new_next = pack(new_idx, ver_of(t_next) + 1);
        if (pool_[t_idx].next.compare_exchange_weak(
                t_next, new_next, std::memory_order_release,
                std::memory_order_relaxed)) {
          // 链接成功，尝试推进 tail（失败也没关系，其他线程会帮忙）
          tail_.compare_exchange_strong(t, pack(new_idx, ver_of(t) + 1),
                                        std::memory_order_release,
                                        std::memory_order_relaxed);
          return true;
        }
      } else {
        // tail 落后，帮助推进
        tail_.compare_exchange_weak(t, pack(t_next_idx, ver_of(t) + 1),
                                    std::memory_order_release,
                                    std::memory_order_relaxed);
      }
    }
  }

  /**
   * pop - 出队（永不阻塞，空时返回 false）
   */
  bool pop(T &item) noexcept {
    for (;;) {
      uint64_t h = head_.load(std::memory_order_acquire);
      uint64_t t = tail_.load(std::memory_order_acquire);
      uint32_t h_idx = idx_of(h);
      uint64_t h_next = pool_[h_idx].next.load(std::memory_order_acquire);

      if (h != head_.load(std::memory_order_acquire))
        continue;

      uint32_t h_next_idx = idx_of(h_next);

      if (h_idx == idx_of(t)) {
        if (h_next_idx == NULL_IDX)
          return false; // 队列空
        // tail 落后，帮助推进
        tail_.compare_exchange_weak(t, pack(h_next_idx, ver_of(t) + 1),
                                    std::memory_order_release,
                                    std::memory_order_relaxed);
      } else {
        // 读取数据（从 next 节点读，因为 head 是哑节点）
        item = pool_[h_next_idx].data;
        if (head_.compare_exchange_weak(h, pack(h_next_idx, ver_of(h) + 1),
                                        std::memory_order_release,
                                        std::memory_order_relaxed)) {
          free_node(h_idx); // 旧哑节点回池
          return true;
        }
      }
    }
  }

  /** 队列是否为空（非精确，仅供参考） */
  bool empty() const noexcept {
    uint64_t h = head_.load(std::memory_order_acquire);
    uint64_t t = tail_.load(std::memory_order_acquire);
    uint32_t h_next_idx =
        idx_of(pool_[idx_of(h)].next.load(std::memory_order_acquire));
    return idx_of(h) == idx_of(t) && h_next_idx == NULL_IDX;
  }
};

} // namespace lf
