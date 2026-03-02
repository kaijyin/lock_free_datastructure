#pragma once
/**
 * SPSCLinkedQueue - 单生产者/单消费者无锁链表队列
 *
 * ============================================================
 * 与 SPSCRingBuffer 的根本区别
 * ============================================================
 *  RingBuffer（数组）：
 *    优点：连续内存，Cache 友好；无动态分配
 *    缺点：容量固定，必须是 2 的幂；Capacity 满时 push 失败
 *
 *  LinkedQueue（链表）：
 *    优点：逻辑无界（受限于节点池大小）；不会因容量满而丢弃数据
 *    缺点：指针追踪（pointer chasing）；需要节点内存管理
 *
 * ============================================================
 * 算法核心（无 CAS 热路径）
 * ============================================================
 *  经典 "dummy node" 单链表：
 *    head → [dummy] → [node1] → [node2] → ... → [nodeN]  ← tail
 *
 *  push（仅生产者）：
 *    1. 从私有空闲链表/归还栈/池线性分配一个节点
 *    2. node->data = item
 *    3. tail->next.store(node, release)   ← 关键：release 保证 data 可见
 *    4. tail = node
 *    全程无 CAS，无原子 RMW 操作
 *
 *  pop（仅消费者）：
 *    1. next = head->next.load(acquire)   ← 与 push 的 release 配对
 *    2. 若 next == nullptr → 空，返回 false
 *    3. item = next->data
 *    4. 将旧 head（dummy）归还到 return_stack
 *    5. head = next（next 成为新 dummy）
 *    全程无 CAS
 *
 * ============================================================
 * 节点内存管理（零动态分配）
 * ============================================================
 *  三层分配策略（均在生产者侧）：
 *    1. prod_.free_head  —— 生产者私有空闲链表，非 atomic，O(1)，零开销
 *    2. cons_.return_stack —— 消费者归还节点的栈
 *       生产者通过 exchange(nullptr, acquire) 一次性取走全部，批量补充私有链表
 *       消费者通过 store(release) 将节点压栈，无 CAS
 *    3. 线性分配游标 prod_.alloc_idx —— 池中从未使用的节点
 *
 *  内存序保证：
 *    消费者：node->next.store(top, relaxed) → return_stack.store(node, release)
 *    生产者：return_stack.exchange(nullptr, acquire) → 读
 * returned->next(relaxed) release/acquire 配对保证生产者看到消费者对 node->next
 * 的写入
 *
 * ============================================================
 * 约束
 * ============================================================
 *  - 严格 1 个生产者线程 + 1 个消费者线程
 *  - PoolSize 决定节点池总量（节点复用，只要队列深度 < PoolSize 即可）
 */
#include <atomic>
#include <cstddef>

namespace lf {

template <typename T, size_t PoolSize = (1 << 17)> // 默认 128K 节点
class SPSCLinkedQueue {
  // ---- 节点 ----
  struct Node {
    T data{};
    std::atomic<Node *> next{nullptr};
  };

  // ---- 生产者侧（独占 cache line）----
  struct alignas(64) ProdSide {
    Node *tail{nullptr};      // 指向链表尾（含 dummy），仅生产者读写
    Node *free_head{nullptr}; // 生产者私有空闲链表，非 atomic
    size_t alloc_idx{1};      // 线性分配游标（0 号为初始 dummy，从 1 起）
  } prod_;

  // ---- 消费者侧（独占 cache line）----
  // 关键设计：
  //   local_ret  —— 纯消费者私有，非 atomic，consumer 自由读写，无任何竞争
  //   return_pub —— 共享通道（consumer 写，producer 读/exchange）
  //                 consumer 只做 store(release)，不读旧值，彻底消除竞态
  struct alignas(64) ConsSide {
    Node *head{nullptr};      // dummy 节点指针，仅消费者读写
    Node *local_ret{nullptr}; // 消费者私有的待归还节点链，非 atomic
    std::atomic<Node *> return_pub{nullptr}; // 发布通道：consumer→producer
  } cons_;

  // 节点池（静态预分配）
  alignas(64) Node pool_[PoolSize];

  // ---- 节点分配（仅生产者调用）----
  Node *alloc_node() noexcept {
    // 1. 私有空闲链表（最快，零原子操作）
    if (prod_.free_head) {
      Node *n = prod_.free_head;
      prod_.free_head = n->next.load(std::memory_order_relaxed);
      return n;
    }
    // 2. 从消费者发布的归还链批量取回
    //    acquire 与 consumer 的 release-store 配对，保证看到 node->next 的写入
    Node *returned =
        cons_.return_pub.exchange(nullptr, std::memory_order_acquire);
    if (returned) {
      prod_.free_head = returned->next.load(std::memory_order_relaxed);
      return returned;
    }
    // 3. 线性分配（池中从未使用的节点）
    if (prod_.alloc_idx < PoolSize) {
      return &pool_[prod_.alloc_idx++];
    }
    return nullptr; // 池满（极少发生）
  }

  // ---- 节点归还（仅消费者调用）----
  //
  // 正确性保证：
  //   local_ret 是纯消费者私有变量，从不与 producer 共享。
  //   只有在 return_pub 为空时，才将整条 local_ret 链一次性 release-store 给
  //   producer。 consumer 不读 return_pub 旧值来拼链，因此不会出现"读到已被
  //   producer 取走的节点 再链入归还链"的竞态（ABA / double-free bug）。
  void return_node(Node *node) noexcept {
    // 将 node 前插到 consumer 私有链
    node->next.store(cons_.local_ret, std::memory_order_relaxed);
    cons_.local_ret = node;

    // 如果发布通道为空，整批发布给 producer
    // relaxed load 足够：即使偶尔看到 stale non-null（producer 刚 exchange 走但
    // 本线程缓存未刷新），最坏情况只是延迟一次发布，不会产生内存安全问题
    if (!cons_.return_pub.load(std::memory_order_relaxed)) {
      // release：确保 local_ret 链中所有 node->next 写入对 producer acquire
      // 可见
      cons_.return_pub.store(cons_.local_ret, std::memory_order_release);
      cons_.local_ret = nullptr;
    }
  }

public:
  SPSCLinkedQueue() noexcept {
    pool_[0].next.store(nullptr, std::memory_order_relaxed);
    prod_.tail = &pool_[0];
    cons_.head = &pool_[0];
  }

  SPSCLinkedQueue(const SPSCLinkedQueue &) = delete;
  SPSCLinkedQueue &operator=(const SPSCLinkedQueue &) = delete;

  /**
   * push - 仅由生产者线程调用
   * 热路径：alloc_node + 1×relaxed store(data) + 1×release store(next)
   * @return true 成功，false 节点池耗尽
   */
  bool push(T item) noexcept {
    Node *node = alloc_node();
    if (!node) [[unlikely]]
      return false;

    node->data = std::move(item);
    node->next.store(nullptr, std::memory_order_relaxed);
    // release：保证 data 先于 next 指针对 consumer 可见
    prod_.tail->next.store(node, std::memory_order_release);
    prod_.tail = node;
    return true;
  }

  /**
   * pop - 仅由消费者线程调用
   * 热路径：1×acquire load(next) + return_node(1×relaxed store + 条件 release
   * store)
   * @return true 成功，false 队列为空
   */
  bool pop(T &item) noexcept {
    // acquire：与 push 中 tail->next.store(release) 配对
    Node *next = cons_.head->next.load(std::memory_order_acquire);
    if (!next)
      return false;

    item = std::move(next->data);
    return_node(cons_.head); // 旧 dummy 归还
    cons_.head = next;       // next 成为新 dummy
    return true;
  }

  bool empty() const noexcept {
    return cons_.head->next.load(std::memory_order_acquire) == nullptr;
  }

  static constexpr size_t pool_size() noexcept { return PoolSize; }
};

} // namespace lf
