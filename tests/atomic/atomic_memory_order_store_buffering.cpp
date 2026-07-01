/**
 * atomic_memory_order_store_buffering.cpp
 *
 * 对比目标：代码形状完全一样，只替换 memory_order，观察结果差异。
 *
 * 经典 Store Buffering 场景：
 *
 *   thread 0: x.store(1, store_order); r1 = y.load(load_order);
 *   thread 1: y.store(1, store_order); r2 = x.load(load_order);
 *
 * 若 order 是 relaxed，r1 == 0 && r2 == 0 是允许结果：两个线程都可以先看
 * 到对方变量的旧值。
 *
 * 只用 release store、只用 acquire load，或 release/acquire 成对使用时，
 * r1 == 0 && r2 == 0 仍然允许：因为两个 acquire load 都没有读到对方的
 * release store，所以没有建立 synchronizes-with。
 *
 * 若 order 是 seq_cst，r1 == 0 && r2 == 0 被禁止：所有 seq_cst 操作必须能
 * 排成一个全局一致顺序，而这个结果无法满足该全局顺序。
 */

#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kIterations = 1'000'000;

struct Summary {
  std::string_view name;
  std::size_t iterations = 0;
  std::size_t both_zero_count = 0;
};

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "check failed: " << message << "\n";
    std::abort();
  }
}

template <std::memory_order StoreOrder, std::memory_order LoadOrder>
Summary RunStoreBuffering(std::string_view name, std::size_t iterations) {
  std::atomic<int> x{0};
  std::atomic<int> y{0};
  std::vector<int> r1(iterations, -1);
  std::vector<int> r2(iterations, -1);

  std::barrier round_start(3);
  std::barrier round_done(3);

  std::thread t0([&] {
    for (std::size_t i = 0; i < iterations; ++i) {
      round_start.arrive_and_wait();

      x.store(1, StoreOrder);
      r1[i] = y.load(LoadOrder);

      round_done.arrive_and_wait();
    }
  });

  std::thread t1([&] {
    for (std::size_t i = 0; i < iterations; ++i) {
      round_start.arrive_and_wait();

      y.store(1, StoreOrder);
      r2[i] = x.load(LoadOrder);

      round_done.arrive_and_wait();
    }
  });

  Summary summary{name, iterations, 0};
  for (std::size_t i = 0; i < iterations; ++i) {
    x.store(0, std::memory_order_relaxed);
    y.store(0, std::memory_order_relaxed);

    round_start.arrive_and_wait();
    round_done.arrive_and_wait();

    if (r1[i] == 0 && r2[i] == 0) {
      ++summary.both_zero_count;
    }
  }

  t0.join();
  t1.join();
  return summary;
}

void PrintSummary(const Summary &summary) {
  std::cout << summary.name << "\n";
  std::cout << "iterations:      " << summary.iterations << "\n";
  std::cout << "both zero count: " << summary.both_zero_count << "\n";
}

}  // namespace

int main() {
  const Summary relaxed =
      RunStoreBuffering<std::memory_order_relaxed,
                        std::memory_order_relaxed>("relaxed", kIterations);
  const Summary release_relaxed =
      RunStoreBuffering<std::memory_order_release,
                        std::memory_order_relaxed>("release/relaxed",
                                                   kIterations);
  const Summary relaxed_acquire =
      RunStoreBuffering<std::memory_order_relaxed,
                        std::memory_order_acquire>("relaxed/acquire",
                                                   kIterations);
  const Summary release_acquire =
      RunStoreBuffering<std::memory_order_release,
                        std::memory_order_acquire>("release/acquire",
                                                   kIterations);
  const Summary seq_cst =
      RunStoreBuffering<std::memory_order_seq_cst,
                        std::memory_order_seq_cst>("seq_cst", kIterations);

  std::cout << "Store Buffering: same code, different memory_order\n\n";
  PrintSummary(relaxed);
  std::cout << "\n";
  PrintSummary(release_relaxed);
  std::cout << "\n";
  PrintSummary(relaxed_acquire);
  std::cout << "\n";
  PrintSummary(release_acquire);
  std::cout << "\n";
  PrintSummary(seq_cst);

  Check(relaxed.both_zero_count > 0,
        "relaxed should observe the both-zero outcome on this machine");
  Check(release_relaxed.both_zero_count > 0,
        "release store alone can still observe both-zero in this pattern");
  Check(relaxed_acquire.both_zero_count > 0,
        "acquire load alone can still observe both-zero in this pattern");
  Check(release_acquire.both_zero_count > 0,
        "release/acquire can still observe both-zero in this pattern");
  Check(seq_cst.both_zero_count == 0,
        "seq_cst must forbid the both-zero outcome");
  return 0;
}
