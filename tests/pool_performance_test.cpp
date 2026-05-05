#include "benchmark_utils.h"
#include "memory_pool.h"
#include "object_pool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Config {
  std::size_t total_ops = 4'000'000;
  std::size_t batch_size = 4096;
  std::size_t block_size = 64;
};

struct TestObject {
  std::uint64_t a;
  std::uint64_t b;
  std::uint64_t c;
  std::uint64_t d;

  explicit TestObject(std::uint64_t seed)
      : a(seed), b(seed * 3 + 1), c(seed * 7 + 3), d(seed * 11 + 5) {}

  std::uint64_t touch() const { return a ^ b ^ c ^ d; }
};

volatile std::uint64_t g_sink = 0;

void print_usage(const char *program) {
  std::cout << "用法: " << program
            << " [--ops N] [--batch N] [--block-size N]\n"
            << "  --ops N         总操作次数，默认 4000000\n"
            << "  --batch N       每批申请数量，默认 4096\n"
            << "  --block-size N  MemoryPool 单次申请字节数，默认 64\n";
}

std::size_t parse_size(const std::string &text, const char *name) {
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
      config.total_ops = parse_size(next_value("--ops"), "--ops");
      continue;
    }
    if (arg == "--batch") {
      config.batch_size = parse_size(next_value("--batch"), "--batch");
      continue;
    }
    if (arg == "--block-size") {
      config.block_size =
          parse_size(next_value("--block-size"), "--block-size");
      continue;
    }
    throw std::runtime_error("未知参数: " + arg);
  }

  config.batch_size = std::min(config.batch_size, config.total_ops);
  return config;
}

template <typename Func>
lf::BenchResult run_benchmark(const std::string &name, std::size_t total_ops,
                              Func &&func) {
  lf::BenchTimer timer;
  timer.start();
  const std::uint64_t checksum = func();
  g_sink ^= checksum;
  return lf::BenchResult{name, 1, total_ops, timer.elapsed_ms()};
}

lf::BenchResult bench_memory_pool(const Config &config) {
  return run_benchmark("MemoryPool acquire+reset", config.total_ops, [&] {
    MemoryPool pool(config.batch_size * config.block_size + 256);
    std::uint64_t checksum = 0;
    std::size_t completed = 0;

    while (completed < config.total_ops) {
      const std::size_t batch =
          std::min(config.batch_size, config.total_ops - completed);
      for (std::size_t i = 0; i < batch; ++i) {
        auto *memory = static_cast<std::uint64_t *>(
            pool.acquire(config.block_size, alignof(std::uint64_t)));
        if (memory == nullptr) {
          throw std::runtime_error("MemoryPool 容量不足");
        }
        memory[0] = static_cast<std::uint64_t>(completed + i);
        checksum += memory[0];
      }
      pool.reset();
      completed += batch;
    }

    return checksum;
  });
}

lf::BenchResult bench_operator_new_batch(const Config &config) {
  return run_benchmark("operator new/delete batch", config.total_ops, [&] {
    std::vector<void *> ptrs(config.batch_size, nullptr);
    std::uint64_t checksum = 0;
    std::size_t completed = 0;

    while (completed < config.total_ops) {
      const std::size_t batch =
          std::min(config.batch_size, config.total_ops - completed);
      for (std::size_t i = 0; i < batch; ++i) {
        ptrs[i] = ::operator new(config.block_size);
        auto *memory = static_cast<std::uint64_t *>(ptrs[i]);
        memory[0] = static_cast<std::uint64_t>(completed + i);
        checksum += memory[0];
      }
      for (std::size_t i = 0; i < batch; ++i) {
        ::operator delete(ptrs[i]);
      }
      completed += batch;
    }

    return checksum;
  });
}

lf::BenchResult bench_object_pool(const Config &config) {
  return run_benchmark("ObjectPool acquire/release", config.total_ops, [&] {
    ObjectPool<TestObject> pool(config.batch_size);
    std::vector<TestObject *> objects(config.batch_size, nullptr);
    std::uint64_t checksum = 0;
    std::size_t completed = 0;

    while (completed < config.total_ops) {
      const std::size_t batch =
          std::min(config.batch_size, config.total_ops - completed);
      for (std::size_t i = 0; i < batch; ++i) {
        objects[i] = pool.acquire(static_cast<std::uint64_t>(completed + i));
        if (objects[i] == nullptr) {
          throw std::runtime_error("ObjectPool 容量不足");
        }
        checksum += objects[i]->touch();
      }
      for (std::size_t i = 0; i < batch; ++i) {
        pool.release(objects[i]);
      }
      completed += batch;
    }

    return checksum;
  });
}

lf::BenchResult bench_object_new_delete(const Config &config) {
  return run_benchmark("new/delete object batch", config.total_ops, [&] {
    std::vector<TestObject *> objects(config.batch_size, nullptr);
    std::uint64_t checksum = 0;
    std::size_t completed = 0;

    while (completed < config.total_ops) {
      const std::size_t batch =
          std::min(config.batch_size, config.total_ops - completed);
      for (std::size_t i = 0; i < batch; ++i) {
        objects[i] = new TestObject(static_cast<std::uint64_t>(completed + i));
        checksum += objects[i]->touch();
      }
      for (std::size_t i = 0; i < batch; ++i) {
        delete objects[i];
      }
      completed += batch;
    }

    return checksum;
  });
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Config config = parse_args(argc, argv);

    std::cout << "==============================================\n";
    std::cout << "  预分配内存池 / 对象池 O3 性能测试\n";
    std::cout << "  total_ops=" << config.total_ops
              << ", batch_size=" << config.batch_size
              << ", block_size=" << config.block_size << " bytes\n";
    std::cout << "==============================================\n";

    std::vector<lf::BenchResult> results;
    results.push_back(bench_memory_pool(config));
    results.push_back(bench_operator_new_batch(config));
    results.push_back(bench_object_pool(config));
    results.push_back(bench_object_new_delete(config));

    lf::print_results(results);
    std::cout << "checksum sink = " << g_sink << std::endl;
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "错误: " << ex.what() << std::endl;
    return 1;
  }
}