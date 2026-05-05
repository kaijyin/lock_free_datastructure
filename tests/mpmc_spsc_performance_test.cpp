#include "benchmark_utils.h"
#include "mpmc_ring_buffer.h"
#include "spsc_ring_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kRingCapacity = 1 << 16;

struct Config {
  std::size_t total_ops = 6'000'000;
  int rounds = 3;
  int consumer_work = 64;
};

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

int online_cpu_count() {
  const long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? static_cast<int>(count) : 1;
}

void bind_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

void print_usage(const char *program) {
  std::cout
      << "用法: " << program << " [--ops N] [--rounds N] [--consumer-work N]\n"
      << "  --ops N     每轮总消息数，默认 6000000\n"
      << "  --rounds N  每种配置重复次数，默认 3\n"
      << "  --consumer-work N  每条消息的复杂 consumer 计算轮数，默认 64\n";
}

std::size_t parse_size_arg(const std::string &text, const char *name) {
  try {
    const auto value = static_cast<std::size_t>(std::stoull(text));
    if (value == 0) {
      throw std::invalid_argument("zero");
    }
    return value;
  } catch (const std::exception &) {
    throw std::runtime_error(std::string(name) + " 参数非法: " + text);
  }
}

Config parse_args(int argc, char **argv) {
  Config config;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next_value = [&](const char *name) {
      if (i + 1 >= argc) {
        throw std::runtime_error(std::string(name) + " 缺少参数值");
      }
      return std::string(argv[++i]);
    };

    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (arg == "--ops") {
      config.total_ops = parse_size_arg(next_value("--ops"), "--ops");
      continue;
    }
    if (arg == "--rounds") {
      config.rounds =
          static_cast<int>(parse_size_arg(next_value("--rounds"), "--rounds"));
      continue;
    }
    if (arg == "--consumer-work") {
      config.consumer_work = static_cast<int>(
          parse_size_arg(next_value("--consumer-work"), "--consumer-work"));
      continue;
    }
    throw std::runtime_error("未知参数: " + arg);
  }
  return config;
}

std::uint64_t do_consumer_work(std::uint64_t value, int iterations) {
  std::uint64_t x = value + 0x9e3779b97f4a7c15ULL;
  for (int i = 0; i < iterations; ++i) {
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    x *= 0x2545f4914f6cdd1dULL;
    x += static_cast<std::uint64_t>(i) * 0x9e3779b97f4a7c15ULL;
  }
  return x;
}

template <typename Queue>
lf::BenchResult
bench_spsc_average(const std::string &name, std::size_t total_ops, int rounds,
                   bool bind_cores, int cpu_count, int consumer_work,
                   std::uint64_t &checksum_out) {
  checksum_out = 0;
  double total_ms = 0.0;

  for (int round = 0; round < rounds; ++round) {
    Queue queue;
    StartLatch latch(2);
    lf::BenchTimer timer;
    std::atomic<bool> timer_started{false};
    std::uint64_t round_sum = 0;

    std::thread producer([&] {
      if (bind_cores) {
        bind_to_core(0 % cpu_count);
      }
      latch.arrive_and_wait();
      bool expected = false;
      if (timer_started.compare_exchange_strong(expected, true)) {
        timer.start();
      }
      for (std::uint64_t i = 0; i < total_ops; ++i) {
        while (!queue.push(i)) {
          lf::cpu_relax();
        }
      }
    });

    std::thread consumer([&] {
      if (bind_cores) {
        bind_to_core(1 % cpu_count);
      }
      latch.arrive_and_wait();
      bool expected = false;
      if (timer_started.compare_exchange_strong(expected, true)) {
        timer.start();
      }
      std::uint64_t value = 0;
      for (std::size_t i = 0; i < total_ops; ++i) {
        while (!queue.pop(value)) {
          lf::cpu_relax();
        }
        if (consumer_work > 0) {
          round_sum ^= do_consumer_work(value, consumer_work);
        } else {
          round_sum += value;
        }
      }
    });

    producer.join();
    consumer.join();
    total_ms += timer.elapsed_ms();
    checksum_out ^= round_sum;
  }

  return lf::BenchResult{name, 2, total_ops,
                         total_ms / static_cast<double>(rounds)};
}

template <typename Queue>
lf::BenchResult
bench_mpmc_average(const std::string &name, std::size_t producers,
                   std::size_t consumers, std::size_t total_ops, int rounds,
                   bool bind_cores, int cpu_count, int consumer_work,
                   std::uint64_t &checksum_out) {
  checksum_out = 0;
  double total_ms = 0.0;
  const std::size_t ops_per_producer = total_ops / producers;
  const std::size_t real_total_ops = ops_per_producer * producers;

  for (int round = 0; round < rounds; ++round) {
    Queue queue;
    StartLatch latch(static_cast<int>(producers + consumers));
    lf::BenchTimer timer;
    std::atomic<bool> timer_started{false};
    std::atomic<std::size_t> consumed_count{0};
    std::vector<std::uint64_t> consumer_sums(consumers, 0);
    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::size_t p = 0; p < producers; ++p) {
      threads.emplace_back([&, p] {
        if (bind_cores) {
          bind_to_core(
              static_cast<int>(p % static_cast<std::size_t>(cpu_count)));
        }
        latch.arrive_and_wait();
        bool expected = false;
        if (timer_started.compare_exchange_strong(expected, true)) {
          timer.start();
        }
        const std::uint64_t begin = p * ops_per_producer;
        const std::uint64_t end = begin + ops_per_producer;
        for (std::uint64_t value = begin; value < end; ++value) {
          while (!queue.push(value)) {
            lf::cpu_relax();
          }
        }
      });
    }

    for (std::size_t c = 0; c < consumers; ++c) {
      threads.emplace_back([&, c] {
        if (bind_cores) {
          const std::size_t core = producers + c;
          bind_to_core(
              static_cast<int>(core % static_cast<std::size_t>(cpu_count)));
        }
        latch.arrive_and_wait();
        bool expected = false;
        if (timer_started.compare_exchange_strong(expected, true)) {
          timer.start();
        }

        std::uint64_t value = 0;
        while (consumed_count.load(std::memory_order_relaxed) <
               real_total_ops) {
          if (queue.pop(value)) {
            if (consumer_work > 0) {
              consumer_sums[c] ^= do_consumer_work(value, consumer_work);
            } else {
              consumer_sums[c] += value;
            }
            consumed_count.fetch_add(1, std::memory_order_relaxed);
          } else {
            lf::cpu_relax();
          }
        }
      });
    }

    for (auto &thread : threads) {
      thread.join();
    }

    total_ms += timer.elapsed_ms();
    for (std::uint64_t sum : consumer_sums) {
      checksum_out ^= sum;
    }
  }

  return lf::BenchResult{name, producers + consumers, real_total_ops,
                         total_ms / static_cast<double>(rounds)};
}

void print_mode_header(bool bind_cores) {
  std::cout << "\n==============================================\n";
  std::cout << "模式: " << (bind_cores ? "绑核" : "不绑核") << "\n";
  std::cout << "==============================================\n";
}

void print_scenario_header(const std::string &label, int consumer_work) {
  std::cout << "\n----------------------------------------------\n";
  std::cout << "场景: " << label;
  if (consumer_work > 0) {
    std::cout << " (consumer_work=" << consumer_work << ")";
  }
  std::cout << "\n----------------------------------------------\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Config config = parse_args(argc, argv);
    const int cpu_count = online_cpu_count();

    std::cout << "================================================\n";
    std::cout << "  SPSC vs MPMC 吞吐量 O3 性能对比\n";
    std::cout << "  online_cpus=" << cpu_count
              << ", total_ops=" << config.total_ops
              << ", rounds=" << config.rounds
              << ", consumer_work=" << config.consumer_work
              << ", ring_capacity=" << kRingCapacity << "\n";
    std::cout << "================================================\n";

    for (bool bind_cores : {true, false}) {
      print_mode_header(bind_cores);
      for (const auto &scenario : std::vector<std::pair<std::string, int>>{
               {"轻消费者", 0}, {"复杂消费者", config.consumer_work}}) {
        print_scenario_header(scenario.first, scenario.second);
        std::vector<lf::BenchResult> results;
        std::uint64_t checksum = 0;

        results.push_back(bench_spsc_average<
                          lf::SPSCRingBuffer<std::uint64_t, kRingCapacity>>(
            "SPSC 1P1C", config.total_ops, config.rounds, bind_cores, cpu_count,
            scenario.second, checksum));

        for (const auto &[producers, consumers] :
             std::vector<std::pair<std::size_t, std::size_t>>{
                 {1, 1}, {2, 2}, {3, 3}, {4, 4}}) {
          results.push_back(bench_mpmc_average<
                            lf::MPMCRingBuffer<std::uint64_t, kRingCapacity>>(
              "MPMC " + std::to_string(producers) + "P" +
                  std::to_string(consumers) + "C",
              producers, consumers, config.total_ops, config.rounds, bind_cores,
              cpu_count, scenario.second, checksum));
        }

        lf::print_results(results);

        const auto spsc_mops = results.front().throughput_mops();
        std::cout << "相对 SPSC 1P1C 吞吐占比:\n";
        for (const auto &result : results) {
          std::cout << "  " << std::left << std::setw(18) << result.name << ": "
                    << std::fixed << std::setprecision(2)
                    << (result.throughput_mops() / spsc_mops * 100.0) << "%\n";
        }
        std::cout << "checksum xor = " << checksum << "\n";
      }
    }

    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "错误: " << ex.what() << std::endl;
    return 1;
  }
}