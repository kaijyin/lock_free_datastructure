#include "benchmark_utils.h"

#include <algorithm>
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

struct Config {
  std::size_t total_ops = 20'000'000;
  int rounds = 5;
  bool bind_cores = true;
};

void print_usage(const char *program) {
  std::cout << "用法: " << program << " [--ops N] [--rounds N] [--no-bind]\n"
            << "  --ops N     每轮 SPSC push/pop 对数，默认 20000000\n"
            << "  --rounds N  重复轮数，默认 5\n"
            << "  --no-bind   不绑定 producer/consumer 到不同核心\n";
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
    if (arg == "--no-bind") {
      config.bind_cores = false;
      continue;
    }
    throw std::runtime_error("未知参数: " + arg);
  }
  return config;
}

void bind_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

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

struct PlainProducerSide {
  std::atomic<std::size_t> pos{0};
  std::size_t cached_read{0};
};

struct alignas(64) AlignedProducerSide {
  std::atomic<std::size_t> pos{0};
  std::size_t cached_read{0};
};

struct PlainConsumerSide {
  std::atomic<std::size_t> pos{0};
  std::size_t cached_write{0};
};

struct alignas(64) AlignedConsumerSide {
  std::atomic<std::size_t> pos{0};
  std::size_t cached_write{0};
};

template <typename T, std::size_t Capacity> struct PlainBufferBlock {
  T data[Capacity];
};

template <typename T, std::size_t Capacity>
struct alignas(64) AlignedBufferBlock {
  T data[Capacity];
};

template <typename T, std::size_t Capacity, typename ProducerSide,
          typename ConsumerSide, typename BufferBlock>
class GenericSPSCQueue {
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of 2");
  static constexpr std::size_t kMask = Capacity - 1;

public:
  using value_type = T;

  bool push(T item) noexcept {
    const std::size_t wp = prod_.pos.load(std::memory_order_relaxed);
    if (wp - prod_.cached_read >= Capacity) {
      prod_.cached_read = cons_.pos.load(std::memory_order_acquire);
      if (wp - prod_.cached_read >= Capacity) {
        return false;
      }
    }

    buffer_.data[wp & kMask] = std::move(item);
    prod_.pos.store(wp + 1, std::memory_order_release);
    return true;
  }

  bool pop(T &item) noexcept {
    const std::size_t rp = cons_.pos.load(std::memory_order_relaxed);
    if (rp == cons_.cached_write) {
      cons_.cached_write = prod_.pos.load(std::memory_order_acquire);
      if (rp == cons_.cached_write) {
        return false;
      }
    }

    item = std::move(buffer_.data[rp & kMask]);
    cons_.pos.store(rp + 1, std::memory_order_release);
    return true;
  }

private:
  ProducerSide prod_;
  ConsumerSide cons_;
  BufferBlock buffer_;
};

using SPSCAligned64 =
    GenericSPSCQueue<std::uint64_t, 1 << 16, AlignedProducerSide,
                     AlignedConsumerSide,
                     AlignedBufferBlock<std::uint64_t, 1 << 16>>;

using SPSCControlAligned =
    GenericSPSCQueue<std::uint64_t, 1 << 16, AlignedProducerSide,
                     AlignedConsumerSide,
                     PlainBufferBlock<std::uint64_t, 1 << 16>>;

using SPSCNoAlign = GenericSPSCQueue<std::uint64_t, 1 << 16, PlainProducerSide,
                                     PlainConsumerSide,
                                     PlainBufferBlock<std::uint64_t, 1 << 16>>;

template <typename Queue>
lf::BenchResult
bench_spsc_average(const std::string &name, std::size_t total_ops, int rounds,
                   bool bind_cores, std::uint64_t &checksum_out) {
  double total_ms = 0.0;
  checksum_out = 0;

  for (int round = 0; round < rounds; ++round) {
    Queue queue;
    StartLatch latch(2);
    lf::BenchTimer timer;
    std::atomic<bool> timer_started{false};
    std::uint64_t consumed_sum = 0;

    std::thread producer([&] {
      if (bind_cores) {
        bind_to_core(0);
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
        bind_to_core(1);
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
        consumed_sum += value;
      }
    });

    producer.join();
    consumer.join();

    total_ms += timer.elapsed_ms();
    checksum_out ^= consumed_sum;
  }

  return lf::BenchResult{name, 2, total_ops,
                         total_ms / static_cast<double>(rounds)};
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Config config = parse_args(argc, argv);

    std::cout << "==============================================\n";
    std::cout << "  SPSC alignas(64) O3 性能对比\n";
    std::cout << "  total_ops=" << config.total_ops
              << ", rounds=" << config.rounds << ", capacity=" << (1 << 16)
              << ", bind_cores=" << (config.bind_cores ? "true" : "false")
              << "\n";
    std::cout << "==============================================\n";

    std::vector<lf::BenchResult> results;
    std::uint64_t aligned_checksum = 0;
    std::uint64_t control_aligned_checksum = 0;
    std::uint64_t no_align_checksum = 0;

    results.push_back(bench_spsc_average<SPSCAligned64>(
        "SPSC full alignas(64)", config.total_ops, config.rounds,
        config.bind_cores, aligned_checksum));
    results.push_back(bench_spsc_average<SPSCControlAligned>(
        "SPSC control align only", config.total_ops, config.rounds,
        config.bind_cores, control_aligned_checksum));
    results.push_back(bench_spsc_average<SPSCNoAlign>(
        "SPSC no alignas", config.total_ops, config.rounds, config.bind_cores,
        no_align_checksum));

    lf::print_results(results);

    const double full_vs_no_align =
        results[2].elapsed_ms / results[0].elapsed_ms;
    const double control_vs_no_align =
        results[2].elapsed_ms / results[1].elapsed_ms;

    std::cout << "加速比（越大越好）:\n"
              << "  full alignas(64) vs no alignas: " << full_vs_no_align
              << "x\n"
              << "  control align only vs no alignas: " << control_vs_no_align
              << "x\n"
              << "checksums: full=" << aligned_checksum
              << ", control=" << control_aligned_checksum
              << ", none=" << no_align_checksum << std::endl;

    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "错误: " << ex.what() << std::endl;
    return 1;
  }
}