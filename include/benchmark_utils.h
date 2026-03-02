#pragma once
/**
 * benchmark_utils.h - 性能测试工具
 *
 * 提供：
 *   - BenchTimer: 高精度计时器
 *   - BenchResult: 测试结果结构
 *   - print_table: 格式化输出对比表格
 *   - cpu_relax: 自旋等待中的 CPU 放松指令
 *   - spin_push / spin_pop: 带自旋重试的入队/出队
 */
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace lf {

// ---- CPU 放松（避免 spinloop 过度消耗总线资源）----
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ volatile("yield" ::: "memory");
#else
  // fallback: 内存屏障
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// ---- 高精度计时器 ----
class BenchTimer {
  using Clock = std::chrono::high_resolution_clock;
  Clock::time_point start_;

public:
  void start() noexcept { start_ = Clock::now(); }

  double elapsed_ms() const noexcept {
    auto dur = Clock::now() - start_;
    return std::chrono::duration<double, std::milli>(dur).count();
  }

  double elapsed_ns() const noexcept {
    auto dur = Clock::now() - start_;
    return std::chrono::duration<double, std::nano>(dur).count();
  }
};

// ---- 测试结果 ----
struct BenchResult {
  std::string name;
  size_t num_threads; // 生产者+消费者线程数
  size_t total_ops;   // 总操作次数（入队/出队各一次算一对）
  double elapsed_ms;

  double throughput_mops() const noexcept {
    return (total_ops / 1e6) / (elapsed_ms / 1e3);
  }
  double ns_per_op() const noexcept { return (elapsed_ms * 1e6) / total_ops; }
};

// ---- 格式化输出表格 ----
inline void print_table_header() {
  std::cout << "\n";
  std::cout << std::left << std::setw(32) << "Name" << std::setw(10)
            << "Threads" << std::setw(14) << "Total Ops" << std::setw(16)
            << "Throughput(Mops)" << std::setw(16) << "Latency(ns/op)"
            << std::setw(12) << "Time(ms)"
            << "\n";
  std::cout << std::string(100, '-') << "\n";
}

inline void print_result(const BenchResult &r) {
  std::cout << std::left << std::setw(32) << r.name << std::setw(10)
            << r.num_threads << std::setw(14) << r.total_ops << std::setw(16)
            << std::fixed << std::setprecision(2) << r.throughput_mops()
            << std::setw(16) << std::fixed << std::setprecision(2)
            << r.ns_per_op() << std::setw(12) << std::fixed
            << std::setprecision(2) << r.elapsed_ms << "\n";
}

inline void print_results(const std::vector<BenchResult> &results) {
  print_table_header();
  for (const auto &r : results) {
    print_result(r);
  }
  std::cout << "\n";
}

// ---- 自旋重试入队：直到成功为止 ----
template <typename Queue>
inline void spin_push(Queue &q, typename Queue::value_type item,
                      int spin_limit = 1000) noexcept {
  int spins = 0;
  while (!q.push(std::move(item))) {
    cpu_relax();
    if (++spins > spin_limit) {
      spins = 0;
      // 让出时间片
      std::this_thread::yield();
    }
  }
}

} // namespace lf
