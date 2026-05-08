/**
 * spsc_pairs_vs_mpmc_test.cpp
 *
 * 核心问题：
 *   当需要 N 个生产者 + N 个消费者时，两种架构哪个更优？
 *
 *   方案A  SPSC-Pairs : N 个独立 SPSC 队列，producer[i] 专线到 consumer[i]
 *                       每对绑定相邻核（producer→偶数核，consumer→奇数核）
 *                       零 CAS 竞争，但总 capacity = N × kRingCapacity
 *
 *   方案B  MPMC-Single: 1 个 MPMC 队列，N 个生产者 + N 个消费者共享
 *                       需要 CAS 协调，capacity = kRingCapacity
 *
 * 测试维度：
 *   - 并发度：N = 1, 2, 3（pair 数量 / MPMC 线程对数）
 *   - 消费者负载：轻(0)、中(64)、重(512) 三种 consumer_work
 *   - 绑核 vs 不绑核两种模式
 *
 * 关键指标：
 *   - 吞吐量 MOps/s（越高越好）
 *   - 延迟（每条消息平均耗时 ns）
 *   - 相对 SPSC-1P1C 基准的倍率
 *
 * 注意：SPSC-Pairs 的 total_ops = N × ops_per_pair，MPMC 使用相同 total_ops，
 *       确保两种方案的"总工作量"相同，吞吐量可直接比较。
 */

#include "benchmark_utils.h"
#include "mpmc_ring_buffer.h"
#include "spsc_ring_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// ─────────────────── 常量 ───────────────────
constexpr std::size_t kRingCapacity = 1 << 16; // 64K slots，SPSC/MPMC 共用
constexpr std::size_t kMaxPairs = 4;           // 最多测试 4 对
constexpr int kWarmupRounds = 1;

// ─────────────────── 配置 ───────────────────
struct Config {
  std::size_t ops_per_pair = 2'000'000; // 每对(SPSC)或等效分配(MPMC)的操作数
  int rounds = 5;
  // consumer_work 列表：0=轻, 64=中, 512=重
};

// ─────────────────── 工具 ───────────────────
struct StartLatch {
  std::atomic<int> count;
  explicit StartLatch(int n) : count(n) {}
  void arrive_and_wait() {
    count.fetch_sub(1, std::memory_order_acq_rel);
    while (count.load(std::memory_order_acquire) > 0)
      lf::cpu_relax();
  }
};

int online_cpu_count() {
  const long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<int>(n) : 1;
}

void bind_to_core(int core_id, int cpu_count) {
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core_id % cpu_count, &cs);
  pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

// xorshift64 模拟"有意义的"消费者计算，结果依赖输入，防止被优化掉
std::uint64_t do_work(std::uint64_t v, int iters) {
  std::uint64_t x = v ^ 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < iters; ++i) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    x *= 0x2545f4914f6cdd1dULL;
    x += static_cast<std::uint64_t>(i) * 0x6c62272e07bb0142ULL;
  }
  return x;
}

// ─────────────────── 结果结构 ───────────────────
struct Result {
  std::string name;
  std::size_t n_pairs;
  std::size_t total_ops;
  double elapsed_ms; // 平均（跨 rounds）

  double throughput_mops() const {
    return elapsed_ms > 0.0 ? static_cast<double>(total_ops) / elapsed_ms / 1e3
                            : 0.0;
  }
  double latency_ns() const {
    return elapsed_ms > 0.0 ? elapsed_ms * 1e6 / static_cast<double>(total_ops)
                            : 0.0;
  }
};

void print_results(const std::vector<Result> &results, double base_mops) {
  std::cout << std::left << std::setw(28) << "名称" << std::right
            << std::setw(8) << "对数" << std::setw(14) << "总ops(M)"
            << std::setw(12) << "耗时(ms)" << std::setw(14) << "吞吐(MOps)"
            << std::setw(12) << "延迟(ns)" << std::setw(10) << "vs基准" << '\n'
            << std::string(98, '-') << '\n';
  for (const auto &r : results) {
    std::cout << std::left << std::setw(28) << r.name << std::right
              << std::fixed << std::setprecision(2) << std::setw(8) << r.n_pairs
              << std::setw(14) << static_cast<double>(r.total_ops) / 1e6
              << std::setw(12) << r.elapsed_ms << std::setw(14)
              << r.throughput_mops() << std::setw(12) << r.latency_ns()
              << std::setw(9) << r.throughput_mops() / base_mops * 100.0 << "%"
              << '\n';
  }
}

// ═══════════════════════════════════════════════════════════════
//  方案A：SPSC-Pairs
//    N 个独立 SPSC 队列，producer[i]→queue[i]→consumer[i]
//    pair i 绑核策略：producer → core 2i, consumer → core 2i+1
//    计时从"所有线程就绪"到"所有消费者完成"
// ═══════════════════════════════════════════════════════════════
using SPSCQueue = lf::SPSCRingBuffer<std::uint64_t, kRingCapacity>;

Result bench_spsc_pairs(std::size_t n_pairs, std::size_t ops_per_pair,
                        int rounds, bool bind_cores, int cpu_count,
                        int consumer_work) {
  const std::size_t total_ops = n_pairs * ops_per_pair;
  double total_ms = 0.0;

  for (int rd = 0; rd < kWarmupRounds + rounds; ++rd) {
    // 每轮重新构造所有队列
    std::vector<SPSCQueue> queues(n_pairs);
    StartLatch latch(static_cast<int>(n_pairs * 2));
    lf::BenchTimer timer;
    std::atomic<bool> timer_started{false};
    std::vector<std::uint64_t> csums(n_pairs, 0);
    std::vector<std::thread> threads;
    threads.reserve(n_pairs * 2);

    for (std::size_t i = 0; i < n_pairs; ++i) {
      // producer[i] → core 2i
      threads.emplace_back([&, i] {
        if (bind_cores)
          bind_to_core(static_cast<int>(i * 2), cpu_count);
        latch.arrive_and_wait();
        bool exp = false;
        if (timer_started.compare_exchange_strong(exp, true))
          timer.start();
        for (std::uint64_t v = 0; v < ops_per_pair; ++v) {
          while (!queues[i].push(v))
            lf::cpu_relax();
        }
      });
      // consumer[i] → core 2i+1
      threads.emplace_back([&, i] {
        if (bind_cores)
          bind_to_core(static_cast<int>(i * 2 + 1), cpu_count);
        latch.arrive_and_wait();
        bool exp = false;
        if (timer_started.compare_exchange_strong(exp, true))
          timer.start();
        std::uint64_t v = 0;
        for (std::size_t j = 0; j < ops_per_pair; ++j) {
          while (!queues[i].pop(v))
            lf::cpu_relax();
          if (consumer_work > 0)
            csums[i] ^= do_work(v, consumer_work);
          else
            csums[i] += v;
        }
      });
    }
    for (auto &t : threads)
      t.join();
    if (rd >= kWarmupRounds)
      total_ms += timer.elapsed_ms();
  }

  return Result{"SPSC-Pairs " + std::to_string(n_pairs) + "P" +
                    std::to_string(n_pairs) + "C",
                n_pairs, total_ops, total_ms / static_cast<double>(rounds)};
}

// ═══════════════════════════════════════════════════════════════
//  方案B：MPMC-Single
//    1 个 MPMC 队列，N 个 producer + N 个 consumer 共享
//    producer[i] → core i，consumer[i] → core n_pairs+i
// ═══════════════════════════════════════════════════════════════
using MPMCQueue = lf::MPMCRingBuffer<std::uint64_t, kRingCapacity>;

Result bench_mpmc_single(std::size_t n_pairs, std::size_t ops_per_pair,
                         int rounds, bool bind_cores, int cpu_count,
                         int consumer_work) {
  const std::size_t total_ops = n_pairs * ops_per_pair;
  double total_ms = 0.0;

  for (int rd = 0; rd < kWarmupRounds + rounds; ++rd) {
    MPMCQueue queue;
    StartLatch latch(static_cast<int>(n_pairs * 2));
    lf::BenchTimer timer;
    std::atomic<bool> timer_started{false};
    std::atomic<std::size_t> consumed{0};
    std::vector<std::uint64_t> csums(n_pairs, 0);
    std::vector<std::thread> threads;
    threads.reserve(n_pairs * 2);

    for (std::size_t i = 0; i < n_pairs; ++i) {
      threads.emplace_back([&, i] {
        if (bind_cores)
          bind_to_core(static_cast<int>(i), cpu_count);
        latch.arrive_and_wait();
        bool exp = false;
        if (timer_started.compare_exchange_strong(exp, true))
          timer.start();
        const std::uint64_t begin = i * ops_per_pair;
        const std::uint64_t end = begin + ops_per_pair;
        for (std::uint64_t v = begin; v < end; ++v) {
          while (!queue.push(v))
            lf::cpu_relax();
        }
      });
      threads.emplace_back([&, i] {
        if (bind_cores)
          bind_to_core(static_cast<int>(n_pairs + i), cpu_count);
        latch.arrive_and_wait();
        bool exp = false;
        if (timer_started.compare_exchange_strong(exp, true))
          timer.start();
        std::uint64_t v = 0;
        while (consumed.load(std::memory_order_relaxed) < total_ops) {
          if (queue.pop(v)) {
            if (consumer_work > 0)
              csums[i] ^= do_work(v, consumer_work);
            else
              csums[i] += v;
            consumed.fetch_add(1, std::memory_order_relaxed);
          } else {
            lf::cpu_relax();
          }
        }
      });
    }
    for (auto &t : threads)
      t.join();
    if (rd >= kWarmupRounds)
      total_ms += timer.elapsed_ms();
  }

  return Result{"MPMC-Single " + std::to_string(n_pairs) + "P" +
                    std::to_string(n_pairs) + "C",
                n_pairs, total_ops, total_ms / static_cast<double>(rounds)};
}

// ─────────────────── 输出帮助 ───────────────────
void print_header(bool bind_cores, int consumer_work, int cpu_count,
                  std::size_t ops_per_pair, int rounds) {
  std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
            << "║  模式: " << std::left << std::setw(44)
            << (bind_cores ? "绑核（producer→偶核, consumer→奇核）"
                           : "不绑核（OS 自由调度）")
            << " ║\n"
            << "║  consumer_work=" << std::setw(4) << consumer_work
            << "  cpu=" << std::setw(3) << cpu_count
            << "  ops/pair=" << std::setw(9) << ops_per_pair
            << "  rounds=" << rounds << "             ║\n"
            << "╚══════════════════════════════════════════════════════╝\n";
}

} // namespace

int main(int argc, char **argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto nv = [&](const char *name) -> std::string {
      if (i + 1 >= argc)
        throw std::runtime_error(std::string(name) + " 缺少值");
      return argv[++i];
    };
    if (a == "--ops")
      cfg.ops_per_pair = std::stoull(nv("--ops"));
    else if (a == "--rounds")
      cfg.rounds = std::stoi(nv("--rounds"));
    else if (a == "--help") {
      std::cout << "用法: " << argv[0] << " [--ops N] [--rounds N]\n"
                << "  --ops N     每对操作数，默认 2000000\n"
                << "  --rounds N  统计轮数，默认 5\n";
      return 0;
    }
  }

  const int cpu_count = online_cpu_count();
  std::cout << "══════════════════════════════════════════════════════\n"
            << "  SPSC-Pairs  vs  MPMC-Single  吞吐 & 延迟对比\n"
            << "  在线核数=" << cpu_count << "  ring_capacity=" << kRingCapacity
            << "  ops/pair=" << cfg.ops_per_pair << "  rounds=" << cfg.rounds
            << "\n"
            << "══════════════════════════════════════════════════════\n";

  // consumer_work 三档：轻(0) / 中(64) / 重(512)
  for (int cw : {0, 64, 512}) {
    const std::string work_label = cw == 0    ? "轻消费(无计算)"
                                   : cw == 64 ? "中消费(64轮)"
                                              : "重消费(512轮)";

    for (bool bind : {true, false}) {
      print_header(bind, cw, cpu_count, cfg.ops_per_pair, cfg.rounds);
      std::cout << "  [" << work_label << "]\n\n";

      std::vector<Result> all;

      // N = 1, 2, 3（不超过可用核数/2）
      const int max_pairs =
          std::min(static_cast<int>(kMaxPairs), cpu_count / 2);

      for (int n = 1; n <= max_pairs; ++n) {
        all.push_back(bench_spsc_pairs(static_cast<std::size_t>(n),
                                       cfg.ops_per_pair, cfg.rounds, bind,
                                       cpu_count, cw));
        all.push_back(bench_mpmc_single(static_cast<std::size_t>(n),
                                        cfg.ops_per_pair, cfg.rounds, bind,
                                        cpu_count, cw));
      }

      // 以 SPSC-Pairs 1P1C 为基准
      const double base_mops = all.front().throughput_mops();
      print_results(all, base_mops);

      // 每个 N 单独打印 SPSC vs MPMC 的直接倍率
      std::cout << "\n  SPSC-Pairs vs MPMC-Single 直接对比：\n";
      for (int n = 1; n <= max_pairs; ++n) {
        const auto &spsc = all[static_cast<std::size_t>((n - 1) * 2)];
        const auto &mpmc = all[static_cast<std::size_t>((n - 1) * 2 + 1)];
        const double ratio = spsc.throughput_mops() / mpmc.throughput_mops();
        std::cout << "    N=" << n << "  SPSC=" << std::fixed
                  << std::setprecision(2) << spsc.throughput_mops() << " MOps/s"
                  << "  MPMC=" << mpmc.throughput_mops() << " MOps/s"
                  << "  SPSC/MPMC=" << ratio << "x\n";
      }
      std::cout << '\n';
    }
  }

  return 0;
}
