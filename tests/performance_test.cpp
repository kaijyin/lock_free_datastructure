/**
 * performance_test.cpp
 *
 * 性能对比测试：无锁 vs Mutex
 *
 * 测试场景：
 *  A. 单线程吞吐量  - 同一线程连续 push / pop（测内存带宽上限）
 *  B. SPSC 吞吐量   - 1 生产者 + 1 消费者（最常见的管道模式）
 *  C. MPMC 吞吐量   - N 生产者 + N 消费者（N = 2/4/8）
 *  D. Ping-Pong 延迟 - 两个线程来回传递消息，测单程传递延迟
 *
 * 数据结构：
 *  - SPSC_RingBuffer (无锁，仅支持 1P+1C)
 *  - MPMC_RingBuffer (无锁，支持任意 NP+NC)
 *  - MS_Queue        (无锁无界，支持任意 NP+NC)
 *  - Mutex_Queue     (mutex 保护的无界队列)
 *  - Mutex_RingBuffer (mutex 保护的有界队列)
 */
#include "benchmark_utils.h"
#include "mpmc_ring_buffer.h"
#include "ms_queue.h"
#include "mutex_containers.h"
#include "spsc_linked_queue.h"
#include "spsc_ring_buffer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

// ---- 简单线程同步 Latch（兼容 GCC 11，无需 C++20 std::barrier）----
struct StartLatch {
  std::atomic<int> count;
  explicit StartLatch(int n) : count(n) {}
  void arrive_and_wait() {
    count.fetch_sub(1, std::memory_order_acq_rel);
    while (count.load(std::memory_order_acquire) > 0) {
      lf::cpu_relax();
    }
  }
};

using namespace lf;

// 环形缓冲区容量（2 的幂）
constexpr size_t RING_CAP = 1 << 16; // 65536 slots
// MS队列节点池大小（需 > 最大同时在队列中的元素数）
constexpr size_t MS_POOL = 1 << 18; // 256K 节点

// ============================================================
// A. 单线程吞吐量测试
//    同一线程交替 push / pop，测内存访问瓶颈
// ============================================================
template <typename Queue>
BenchResult bench_single_thread(const std::string &name, size_t total_ops) {
  Queue q;
  BenchTimer timer;

  // warm up
  for (size_t i = 0; i < 1000; ++i) {
    uint64_t dummy;
    q.push(static_cast<uint64_t>(i));
    q.pop(dummy);
  }

  timer.start();
  for (size_t i = 0; i < total_ops; ++i) {
    uint64_t dummy;
    q.push(static_cast<uint64_t>(i));
    q.pop(dummy);
  }
  double ms = timer.elapsed_ms();

  return BenchResult{name, 1, total_ops, ms};
}

// ============================================================
// B. SPSC 吞吐量测试
//    1 个生产者线程，1 个消费者线程
// ============================================================
template <typename Queue>
BenchResult bench_spsc(const std::string &name, size_t total_ops) {
  Queue q;
  BenchTimer timer;
  StartLatch latch(2);
  std::atomic<bool> timer_started{false};

  std::thread producer([&] {
    latch.arrive_and_wait();
    bool exp = false;
    if (timer_started.compare_exchange_strong(exp, true))
      timer.start();
    for (uint64_t i = 0; i < total_ops; ++i) {
      while (!q.push(i)) {
        cpu_relax();
      }
    }
  });

  std::thread consumer([&] {
    latch.arrive_and_wait();
    bool exp = false;
    if (timer_started.compare_exchange_strong(exp, true))
      timer.start();
    uint64_t v;
    for (size_t i = 0; i < total_ops; ++i) {
      while (!q.pop(v)) {
        cpu_relax();
      }
    }
  });

  producer.join();
  consumer.join();

  return BenchResult{name, 2, total_ops, timer.elapsed_ms()};
}

// ============================================================
// C. MPMC 吞吐量测试
//    np 个生产者线程，nc 个消费者线程
// ============================================================
template <typename Queue>
BenchResult bench_mpmc(const std::string &name, size_t np, size_t nc,
                       size_t total_ops) {
  Queue q;
  BenchTimer timer;
  const size_t ops_per_prod = total_ops / np;
  const size_t real_total = ops_per_prod * np;

  std::atomic<size_t> consumed_count{0};
  StartLatch latch(static_cast<int>(np + nc));
  std::atomic<bool> timer_started{false};

  std::vector<std::thread> prods, cons;

  for (size_t p = 0; p < np; ++p) {
    prods.emplace_back([&, p] {
      latch.arrive_and_wait();
      bool exp = false;
      if (timer_started.compare_exchange_strong(exp, true))
        timer.start();
      uint64_t base = p * ops_per_prod;
      for (uint64_t i = base; i < base + ops_per_prod; ++i) {
        while (!q.push(i)) {
          cpu_relax();
        }
      }
    });
  }

  for (size_t c = 0; c < nc; ++c) {
    cons.emplace_back([&] {
      latch.arrive_and_wait();
      bool exp = false;
      if (timer_started.compare_exchange_strong(exp, true))
        timer.start();
      uint64_t v;
      while (consumed_count.load(std::memory_order_relaxed) < real_total) {
        if (q.pop(v)) {
          consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto &t : prods)
    t.join();
  for (auto &t : cons)
    t.join();

  return BenchResult{name, np + nc, real_total, timer.elapsed_ms()};
}

// ============================================================
// D. Ping-Pong 延迟测试
//    线程 A push 到 q1，线程 B 从 q1 pop 后 push 到 q2，线程 A 从 q2 pop
//    测量单程延迟 = 总时间 / (2 * round_trips)
// ============================================================
template <typename Queue>
BenchResult bench_pingpong(const std::string &name, size_t rounds) {
  Queue q1, q2;
  BenchTimer timer;
  StartLatch latch(2);
  std::atomic<bool> timer_started{false};

  std::thread pinger([&] {
    latch.arrive_and_wait();
    bool exp = false;
    if (timer_started.compare_exchange_strong(exp, true))
      timer.start();
    uint64_t v;
    for (size_t i = 0; i < rounds; ++i) {
      while (!q1.push(static_cast<uint64_t>(i))) {
        cpu_relax();
      }
      while (!q2.pop(v)) {
        cpu_relax();
      }
    }
  });

  std::thread ponger([&] {
    latch.arrive_and_wait();
    bool exp = false;
    if (timer_started.compare_exchange_strong(exp, true))
      timer.start();
    uint64_t v;
    for (size_t i = 0; i < rounds; ++i) {
      while (!q1.pop(v)) {
        cpu_relax();
      }
      while (!q2.push(v)) {
        cpu_relax();
      }
    }
  });

  pinger.join();
  ponger.join();

  // 每个 round 包含 2 次单向传递
  return BenchResult{name, 2, rounds * 2, timer.elapsed_ms()};
}

// ============================================================
// main
// ============================================================
int main() {
  constexpr size_t SINGLE_OPS = 10'000'000;
  constexpr size_t SPSC_OPS = 10'000'000;
  constexpr size_t MPMC_OPS = 5'000'000;
  constexpr size_t PINGPONG_ROUNDS = 500'000;

  std::cout << "================================================\n";
  std::cout << "  无锁数据结构 vs Mutex 性能对比测试\n";
  std::cout << "  CPU 核心数: " << std::thread::hardware_concurrency() << "\n";
  std::cout << "  Ring Buffer 容量: " << RING_CAP << "\n";
  std::cout << "================================================\n";

  // ---- A. 单线程吞吐量 ----
  {
    std::cout << "\n[A] 单线程吞吐量（" << SINGLE_OPS / 1'000'000
              << "M ops）\n";
    std::vector<BenchResult> results;

    results.push_back(bench_single_thread<SPSCRingBuffer<uint64_t, RING_CAP>>(
        "SPSC_RingBuffer", SINGLE_OPS));
    results.push_back(bench_single_thread<SPSCLinkedQueue<uint64_t, MS_POOL>>(
        "SPSC_LinkedQueue", SINGLE_OPS));
    results.push_back(bench_single_thread<MPMCRingBuffer<uint64_t, RING_CAP>>(
        "MPMC_RingBuffer", SINGLE_OPS));
    results.push_back(bench_single_thread<MSQueue<uint64_t, MS_POOL>>(
        "MS_Queue", SINGLE_OPS));
    results.push_back(bench_single_thread<MutexRingBuffer<uint64_t, RING_CAP>>(
        "Mutex_RingBuffer", SINGLE_OPS));
    results.push_back(
        bench_single_thread<MutexQueue<uint64_t>>("Mutex_Queue", SINGLE_OPS));

    print_results(results);
  }

  // ---- B. SPSC 吞吐量 ----
  {
    std::cout << "\n[B] SPSC 吞吐量 (1P+1C, " << SPSC_OPS / 1'000'000
              << "M ops)\n";
    std::vector<BenchResult> results;

    results.push_back(bench_spsc<SPSCRingBuffer<uint64_t, RING_CAP>>(
        "SPSC_RingBuffer", SPSC_OPS));
    results.push_back(bench_spsc<SPSCLinkedQueue<uint64_t, MS_POOL>>(
        "SPSC_LinkedQueue", SPSC_OPS));
    results.push_back(bench_spsc<MPMCRingBuffer<uint64_t, RING_CAP>>(
        "MPMC_RingBuffer", SPSC_OPS));
    results.push_back(
        bench_spsc<MSQueue<uint64_t, MS_POOL>>("MS_Queue", SPSC_OPS));
    results.push_back(bench_spsc<MutexRingBuffer<uint64_t, RING_CAP>>(
        "Mutex_RingBuffer", SPSC_OPS));
    results.push_back(
        bench_spsc<MutexQueue<uint64_t>>("Mutex_Queue", SPSC_OPS));

    print_results(results);
  }

  // ---- C. MPMC 吞吐量 ----
  for (size_t np_nc : {2u, 4u, 8u}) {
    std::cout << "\n[C] MPMC 吞吐量 (" << np_nc << "P+" << np_nc << "C, "
              << MPMC_OPS / 1'000'000 << "M ops)\n";
    std::vector<BenchResult> results;

    results.push_back(bench_mpmc<MPMCRingBuffer<uint64_t, RING_CAP>>(
        "MPMC_RingBuffer", np_nc, np_nc, MPMC_OPS));
    results.push_back(bench_mpmc<MSQueue<uint64_t, MS_POOL>>("MS_Queue", np_nc,
                                                             np_nc, MPMC_OPS));
    results.push_back(bench_mpmc<MutexRingBuffer<uint64_t, RING_CAP>>(
        "Mutex_RingBuffer", np_nc, np_nc, MPMC_OPS));
    results.push_back(bench_mpmc<MutexQueue<uint64_t>>("Mutex_Queue", np_nc,
                                                       np_nc, MPMC_OPS));

    print_results(results);
  }

  // ---- D. Ping-Pong 延迟 ----
  {
    std::cout << "\n[D] Ping-Pong 延迟（" << PINGPONG_ROUNDS / 1000
              << "K rounds）\n";
    std::vector<BenchResult> results;

    results.push_back(bench_pingpong<SPSCRingBuffer<uint64_t, 64>>(
        "SPSC_RingBuffer", PINGPONG_ROUNDS));
    results.push_back(bench_pingpong<SPSCLinkedQueue<uint64_t, 4096>>(
        "SPSC_LinkedQueue", PINGPONG_ROUNDS));
    results.push_back(bench_pingpong<MPMCRingBuffer<uint64_t, 64>>(
        "MPMC_RingBuffer", PINGPONG_ROUNDS));
    results.push_back(
        bench_pingpong<MSQueue<uint64_t, 1024>>("MS_Queue", PINGPONG_ROUNDS));
    results.push_back(bench_pingpong<MutexRingBuffer<uint64_t, 64>>(
        "Mutex_RingBuffer", PINGPONG_ROUNDS));
    results.push_back(
        bench_pingpong<MutexQueue<uint64_t>>("Mutex_Queue", PINGPONG_ROUNDS));

    print_results(results);

    // 输出单程延迟
    std::cout << "  单程延迟（ns）：\n";
    for (const auto &r : results) {
      // 每个 "op" = 一次单向传递，所以 ns_per_op 就是单程延迟
      std::cout << "    " << std::left << std::setw(24) << r.name << ": "
                << std::fixed << std::setprecision(1) << r.ns_per_op()
                << " ns\n";
    }
  }

  std::cout << "\n================================================\n";
  std::cout << "测试完成\n";
  return 0;
}
