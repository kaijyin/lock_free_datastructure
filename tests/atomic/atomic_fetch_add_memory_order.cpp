/**
 * atomic_fetch_add_memory_order.cpp
 *
 * 对比目标：观察 fetch_add 的 memory_order 在两个场景里的区别。
 *
 * 1. 只把 fetch_add 当计数器：
 *    所有 memory_order 都保证 counter 本身的原子递增，不会丢加法。
 *
 * 2. 把 fetch_add 当“发布信号”：
 *    fetch_add(relaxed) 只保证计数器原子递增，不会发布前面的普通写入。
 *    producer 需要 release，consumer 需要 acquire，才会为普通 payload 建立
 *    happens-before。
 *
 * 在 x86_64 上，fetch_add(relaxed/release/acquire/acq_rel/seq_cst) 通常都
 * 编译为 lock 指令，所以普通运行时错误版本也经常看起来正确。可以用
 * ThreadSanitizer 验证 relaxed 发布版本里的 data race：
 *
 *   clang++ -std=c++20 -Og -g -fsanitize=thread -fno-omit-frame-pointer \
 *       tests/atomic/atomic_fetch_add_memory_order.cpp -pthread \
 *       -o /tmp/atomic_fetch_add_memory_order_tsan
 *   /tmp/atomic_fetch_add_memory_order_tsan
 */

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kThreadCount = 4;
constexpr std::uint64_t kIncrementsPerThread = 500'000;
constexpr double kDoubleIncrement = 0.5;
constexpr int kPayloadValue = 42;

struct CounterSummary {
  std::string_view name;
  std::uint64_t expected = 0;
  std::uint64_t observed = 0;
  double ns_per_fetch_add = 0.0;
};

struct DoubleCounterSummary {
  std::string_view name;
  double expected = 0.0;
  double observed = 0.0;
  bool is_lock_free = false;
  double ns_per_fetch_add = 0.0;
};

struct PublishSummary {
  std::string_view name;
  int observed = 0;
  bool has_happens_before = false;
};

struct Message {
  int payload = 0;
  std::atomic<std::uint64_t> published{0};
};

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "check failed: " << message << "\n";
    std::abort();
  }
}

template <std::memory_order Order>
CounterSummary RunCounter(std::string_view name) {
  std::atomic<std::uint64_t> counter{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  const auto start = std::chrono::steady_clock::now();
  for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads.emplace_back([&] {
      for (std::uint64_t i = 0; i < kIncrementsPerThread; ++i) {
        counter.fetch_add(1, Order);
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }
  const auto stop = std::chrono::steady_clock::now();

  const std::uint64_t expected = kIncrementsPerThread * kThreadCount;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
  return CounterSummary{
      name,
      expected,
      counter.load(std::memory_order_relaxed),
      static_cast<double>(elapsed.count()) / static_cast<double>(expected),
  };
}

template <std::memory_order Order>
DoubleCounterSummary RunDoubleCounter(std::string_view name) {
  std::atomic<double> counter{0.0};
  const bool is_lock_free = counter.is_lock_free();
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  const auto start = std::chrono::steady_clock::now();
  for (int thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads.emplace_back([&] {
      for (std::uint64_t i = 0; i < kIncrementsPerThread; ++i) {
        counter.fetch_add(kDoubleIncrement, Order);
      }
    });
  }

  for (auto &thread : threads) {
    thread.join();
  }
  const auto stop = std::chrono::steady_clock::now();

  const auto operation_count = kIncrementsPerThread * kThreadCount;
  const double expected =
      static_cast<double>(operation_count) * kDoubleIncrement;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
  return DoubleCounterSummary{
      name,
      expected,
      counter.load(std::memory_order_relaxed),
      is_lock_free,
      static_cast<double>(elapsed.count()) /
          static_cast<double>(operation_count),
  };
}

template <std::memory_order AddOrder, std::memory_order LoadOrder>
PublishSummary RunPublish(std::string_view name, bool has_happens_before) {
  Message message;
  int observed = 0;

  std::thread producer([&] {
    message.payload = kPayloadValue;
    message.published.fetch_add(1, AddOrder);
  });

  std::thread consumer([&] {
    while (message.published.load(LoadOrder) == 0) {
      std::this_thread::yield();
    }
    observed = message.payload;
  });

  producer.join();
  consumer.join();
  return PublishSummary{name, observed, has_happens_before};
}

void PrintCounterSummary(const CounterSummary &summary) {
  std::cout << std::left << std::setw(12) << summary.name << " expected "
            << summary.expected << ", observed " << summary.observed
            << ", ns/fetch_add " << std::fixed << std::setprecision(2)
            << summary.ns_per_fetch_add << "\n";
}

void PrintDoubleCounterSummary(const DoubleCounterSummary &summary) {
  std::cout << std::left << std::setw(12) << summary.name << " expected "
            << std::fixed << std::setprecision(1) << summary.expected
            << ", observed " << summary.observed << ", lock_free "
            << (summary.is_lock_free ? "yes" : "no") << ", ns/fetch_add "
            << std::setprecision(2) << summary.ns_per_fetch_add << "\n";
}

void PrintPublishSummary(const PublishSummary &summary) {
  std::cout << std::left << std::setw(24) << summary.name << " observed "
            << summary.observed << ", "
            << (summary.has_happens_before ? "correct" : "wrong") << "\n";
}

} // namespace

int main() {
  const std::vector<CounterSummary> counters = {
      RunCounter<std::memory_order_relaxed>("relaxed"),
      RunCounter<std::memory_order_acquire>("acquire"),
      RunCounter<std::memory_order_release>("release"),
      RunCounter<std::memory_order_acq_rel>("acq_rel"),
      RunCounter<std::memory_order_seq_cst>("seq_cst"),
  };

  std::cout << "fetch_add as a counter\n";
  for (const CounterSummary &summary : counters) {
    PrintCounterSummary(summary);
    Check(summary.observed == summary.expected,
          "fetch_add must not lose increments");
  }

  const std::vector<DoubleCounterSummary> double_counters = {
      RunDoubleCounter<std::memory_order_relaxed>("relaxed"),
      RunDoubleCounter<std::memory_order_acquire>("acquire"),
      RunDoubleCounter<std::memory_order_release>("release"),
      RunDoubleCounter<std::memory_order_acq_rel>("acq_rel"),
      RunDoubleCounter<std::memory_order_seq_cst>("seq_cst"),
  };

  std::cout << "\natomic<double> fetch_add as a counter\n";
  for (const DoubleCounterSummary &summary : double_counters) {
    PrintDoubleCounterSummary(summary);
    Check(summary.observed == summary.expected,
          "atomic<double> fetch_add must not lose increments");
  }

  const std::vector<PublishSummary> publishes = {
      RunPublish<std::memory_order_relaxed, std::memory_order_relaxed>(
          "relaxed/relaxed", false),
      RunPublish<std::memory_order_release, std::memory_order_relaxed>(
          "release/relaxed", false),
      RunPublish<std::memory_order_relaxed, std::memory_order_acquire>(
          "relaxed/acquire", false),
      RunPublish<std::memory_order_release, std::memory_order_acquire>(
          "release/acquire", true),
      RunPublish<std::memory_order_acq_rel, std::memory_order_acquire>(
          "acq_rel/acquire", true),
      RunPublish<std::memory_order_seq_cst, std::memory_order_seq_cst>(
          "seq_cst/seq_cst", true),
  };

  std::cout << "\nfetch_add as a publish signal\n";
  for (const PublishSummary &summary : publishes) {
    PrintPublishSummary(summary);
    Check(summary.observed == kPayloadValue,
          "x86 often prints the expected value even for wrong variants");
  }

  return 0;
}
