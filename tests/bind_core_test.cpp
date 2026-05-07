#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t kDefaultBufferBytes = 8ULL * 1024ULL * 1024ULL;
constexpr int kDefaultPasses = 8;
constexpr int kDefaultWarmupRounds = 2;
constexpr int kDefaultMeasureRounds = 8;
constexpr std::size_t kStride = 8191U;

struct Stats {
  double mean = 0.0;
  double stddev = 0.0;
  double min = 0.0;
  double max = 0.0;
};

struct BenchmarkResult {
  Stats stats;
  double checksum = 0.0;
};

struct Config {
  int thread_count = 0;
  std::size_t buffer_bytes = kDefaultBufferBytes;
  int passes = kDefaultPasses;
  int warmup_rounds = kDefaultWarmupRounds;
  int measure_rounds = kDefaultMeasureRounds;
};

int online_cpu_count() {
  const long count = sysconf(_SC_NPROCESSORS_ONLN);
  return count > 0 ? static_cast<int>(count) : 1;
}

void print_usage(const char *program) {
  std::cout << "用法: " << program << " [options]\n"
            << "  --threads N         线程数，默认等于在线 CPU 数\n"
            << "  --buffer-size SIZE  每线程工作集大小，支持 K/M/G 后缀，如 "
               "512K、8M、64M\n"
            << "  --passes N          每轮重复扫描次数，默认 8\n"
            << "  --warmup N          预热轮数，默认 2\n"
            << "  --rounds N          统计轮数，默认 8\n"
            << "  --help              显示帮助\n";
}

[[noreturn]] void fail_argument(const std::string &message) {
  std::cerr << "参数错误: " << message << std::endl;
  std::exit(1);
}

int parse_int_arg(const std::string &text, const std::string &name) {
  try {
    const int value = std::stoi(text);
    if (value <= 0) {
      fail_argument(name + " 必须大于 0");
    }
    return value;
  } catch (const std::exception &) {
    fail_argument(name + " 不是合法整数: " + text);
  }
}

std::size_t parse_size_arg(const std::string &text) {
  if (text.empty()) {
    fail_argument("buffer-size 不能为空");
  }

  std::string lowered = text;
  std::transform(
      lowered.begin(), lowered.end(), lowered.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  std::size_t numeric_end = 0;
  while (numeric_end < lowered.size() &&
         (std::isdigit(static_cast<unsigned char>(lowered[numeric_end])) ||
          lowered[numeric_end] == '.')) {
    ++numeric_end;
  }

  if (numeric_end == 0) {
    fail_argument("buffer-size 缺少数值部分: " + text);
  }

  const double value = std::stod(lowered.substr(0, numeric_end));
  const std::string suffix = lowered.substr(numeric_end);

  std::size_t multiplier = 1;
  if (suffix.empty() || suffix == "b") {
    multiplier = 1;
  } else if (suffix == "k" || suffix == "kb" || suffix == "kib") {
    multiplier = 1024ULL;
  } else if (suffix == "m" || suffix == "mb" || suffix == "mib") {
    multiplier = 1024ULL * 1024ULL;
  } else if (suffix == "g" || suffix == "gb" || suffix == "gib") {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
  } else {
    fail_argument("不支持的 buffer-size 后缀: " + text);
  }

  const double bytes = value * static_cast<double>(multiplier);
  if (bytes < static_cast<double>(sizeof(double))) {
    fail_argument("buffer-size 至少需要容纳一个 double");
  }
  return static_cast<std::size_t>(bytes);
}

Config parse_args(int argc, char **argv) {
  Config config;
  config.thread_count = online_cpu_count();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string &option) -> std::string {
      if (i + 1 >= argc) {
        fail_argument(option + " 缺少参数值");
      }
      return argv[++i];
    };

    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    }
    if (arg == "--threads") {
      config.thread_count = parse_int_arg(require_value(arg), "threads");
      continue;
    }
    if (arg == "--buffer-size") {
      config.buffer_bytes = parse_size_arg(require_value(arg));
      continue;
    }
    if (arg == "--passes") {
      config.passes = parse_int_arg(require_value(arg), "passes");
      continue;
    }
    if (arg == "--warmup") {
      config.warmup_rounds = parse_int_arg(require_value(arg), "warmup");
      continue;
    }
    if (arg == "--rounds") {
      config.measure_rounds = parse_int_arg(require_value(arg), "rounds");
      continue;
    }

    fail_argument("未知参数: " + arg);
  }

  return config;
}

std::size_t bytes_to_elements(std::size_t buffer_bytes) {
  return std::max<std::size_t>(1, buffer_bytes / sizeof(double));
}

bool is_power_of_two(std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

std::size_t advance_index(std::size_t index, std::size_t step,
                          std::size_t capacity) {
  if (capacity == 0) {
    return 0;
  }
  index += step;
  if (is_power_of_two(capacity)) {
    return index & (capacity - 1);
  }
  return index % capacity;
}

// 绑定当前线程到指定核心。
void bind_to_core(int core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  pthread_t current_thread = pthread_self();
  if (pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset) != 0) {
    std::cerr << "Error binding to core " << core_id << std::endl;
  }
}

int bound_core_id(int thread_id, int cpu_count) {
  return cpu_count > 0 ? thread_id % cpu_count : -1;
}

Stats calc_stats(const std::vector<double> &samples) {
  Stats stats;
  if (samples.empty()) {
    return stats;
  }

  stats.mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
               static_cast<double>(samples.size());
  stats.min = *std::min_element(samples.begin(), samples.end());
  stats.max = *std::max_element(samples.begin(), samples.end());

  double variance = 0.0;
  for (double sample : samples) {
    const double diff = sample - stats.mean;
    variance += diff * diff;
  }
  variance /= static_cast<double>(samples.size());
  stats.stddev = std::sqrt(variance);
  return stats;
}

// 每个线程独占自己的 buffer，重点观察线程迁移对私有缓存局部性的影响。
void private_memory_task(int thread_id, std::size_t buffer_elements, int passes,
                         int bind_core, double &result) {
  if (bind_core >= 0) {
    bind_to_core(bind_core);
  }

  std::vector<double> buffer(buffer_elements);
  for (std::size_t i = 0; i < buffer_elements; ++i) {
    buffer[i] = static_cast<double>((i + 1) * (thread_id + 1)) * 0.25;
  }

  std::size_t index = static_cast<std::size_t>(thread_id) % buffer_elements;
  double local_sum = 0.0;

  for (int pass = 0; pass < passes; ++pass) {
    for (std::size_t i = 0; i < buffer_elements; ++i) {
      index = advance_index(index, kStride, buffer_elements);
      double value = buffer[index];
      value = value * 1.0000001192092896 + static_cast<double>(pass + 1) * 1e-6;
      buffer[index] = value;
      local_sum += value;
    }
  }

  result = local_sum + buffer[(index + 1) % buffer_elements];
}

// 所有线程共享一块大数组，并以交错方式写入相邻元素，制造共享 cache line 压力。
void shared_memory_task(int thread_id, std::vector<double> &shared_buffer,
                        int thread_count, int passes, int bind_core,
                        double &result) {
  if (bind_core >= 0) {
    bind_to_core(bind_core);
  }

  const std::size_t total_elements = shared_buffer.size();
  double local_sum = 0.0;

  for (int pass = 0; pass < passes; ++pass) {
    for (std::size_t index = static_cast<std::size_t>(thread_id);
         index < total_elements;
         index += static_cast<std::size_t>(thread_count)) {
      double value = shared_buffer[index];
      value = value * 1.0000001192092896 + static_cast<double>(pass + 1) * 1e-6;
      shared_buffer[index] = value;
      local_sum += value;
    }
  }

  result = local_sum +
           shared_buffer[static_cast<std::size_t>(thread_id) % total_elements];
}

template <typename Launcher>
BenchmarkResult run_benchmark(int warmup_rounds, int measure_rounds,
                              Launcher &&launcher) {
  std::vector<double> elapsed_seconds;
  elapsed_seconds.reserve(measure_rounds);
  double checksum = 0.0;

  for (int round = 0; round < warmup_rounds + measure_rounds; ++round) {
    auto start = std::chrono::high_resolution_clock::now();
    const double round_checksum = launcher();
    auto end = std::chrono::high_resolution_clock::now();

    checksum += round_checksum;
    if (round >= warmup_rounds) {
      std::chrono::duration<double> diff = end - start;
      elapsed_seconds.push_back(diff.count());
    }
  }

  return {calc_stats(elapsed_seconds), checksum};
}

void print_benchmark_result(const std::string &label,
                            const BenchmarkResult &baseline,
                            const BenchmarkResult &affinity) {
  const double speedup = baseline.stats.mean > 0.0
                             ? (baseline.stats.mean - affinity.stats.mean) /
                                   baseline.stats.mean * 100.0
                             : 0.0;

  std::cout << label << '\n'
            << "  [未绑核] 平均: " << std::fixed << std::setprecision(6)
            << baseline.stats.mean << " 秒, stddev=" << baseline.stats.stddev
            << ", min=" << baseline.stats.min << ", max=" << baseline.stats.max
            << ", checksum=" << baseline.checksum << '\n'
            << "  [绑核]   平均: " << affinity.stats.mean
            << " 秒, stddev=" << affinity.stats.stddev
            << ", min=" << affinity.stats.min << ", max=" << affinity.stats.max
            << ", checksum=" << affinity.checksum << '\n'
            << "  [差异]   绑核相对变化: " << speedup << "%" << std::endl;
}

void run_private_memory_benchmark(const Config &config, int cpu_count) {
  const std::size_t buffer_elements = bytes_to_elements(config.buffer_bytes);
  auto launch = [&](bool use_affinity) {
    return run_benchmark(config.warmup_rounds, config.measure_rounds, [&]() {
      std::vector<double> results(config.thread_count, 0.0);
      std::vector<std::thread> threads;
      threads.reserve(config.thread_count);

      for (int i = 0; i < config.thread_count; ++i) {
        threads.emplace_back(private_memory_task, i, buffer_elements,
                             config.passes,
                             use_affinity ? bound_core_id(i, cpu_count) : -1,
                             std::ref(results[i]));
      }

      for (auto &thread : threads) {
        thread.join();
      }
      return std::accumulate(results.begin(), results.end(), 0.0);
    });
  };

  std::cout << "\n=== 私有内存测试 ===\n";
  std::cout << "每线程工作集: " << config.buffer_bytes / (1024.0 * 1024.0)
            << " MiB, 总工作集约: "
            << config.buffer_bytes * config.thread_count / (1024.0 * 1024.0)
            << " MiB" << std::endl;
  print_benchmark_result("私有 buffer + 随机步长访问", launch(false),
                         launch(true));
}

void run_shared_memory_benchmark(const Config &config, int cpu_count) {
  const std::size_t per_thread_elements =
      bytes_to_elements(config.buffer_bytes);
  const std::size_t total_elements =
      per_thread_elements * static_cast<std::size_t>(config.thread_count);

  auto launch = [&](bool use_affinity) {
    return run_benchmark(config.warmup_rounds, config.measure_rounds, [&]() {
      std::vector<double> shared_buffer(total_elements);
      for (std::size_t i = 0; i < total_elements; ++i) {
        shared_buffer[i] = static_cast<double>(i + 1) * 0.125;
      }

      std::vector<double> results(config.thread_count, 0.0);
      std::vector<std::thread> threads;
      threads.reserve(config.thread_count);

      for (int i = 0; i < config.thread_count; ++i) {
        threads.emplace_back(shared_memory_task, i, std::ref(shared_buffer),
                             config.thread_count, config.passes,
                             use_affinity ? bound_core_id(i, cpu_count) : -1,
                             std::ref(results[i]));
      }

      for (auto &thread : threads) {
        thread.join();
      }
      return std::accumulate(results.begin(), results.end(), 0.0);
    });
  };

  std::cout << "\n=== 共享内存测试 ===\n";
  std::cout << "共享数组总大小: "
            << (config.buffer_bytes * config.thread_count) / (1024.0 * 1024.0)
            << " MiB（按线程交错写入，制造共享 cache line 压力）" << std::endl;
  print_benchmark_result("shared buffer + interleaved writes", launch(false),
                         launch(true));
}

} // namespace

int main(int argc, char **argv) {
  const Config config = parse_args(argc, argv);
  const int cpu_count = online_cpu_count();

  std::cout << "开始线程绑核对比测试..." << std::endl;
  std::cout << "在线 CPU 数: " << cpu_count << "\n"
            << "线程数: " << config.thread_count << "\n"
            << "每线程工作集: " << config.buffer_bytes / (1024.0 * 1024.0)
            << " MiB\n"
            << "passes=" << config.passes
            << ", 预热轮数=" << config.warmup_rounds
            << ", 统计轮数=" << config.measure_rounds << std::endl;

  if (config.thread_count > cpu_count) {
    std::cout << "注意: 线程数超过在线 CPU 数，绑核时会按 CPU 数取模复用核心。"
              << std::endl;
  }

  run_private_memory_benchmark(config, cpu_count);
  run_shared_memory_benchmark(config, cpu_count);
  return 0;
}