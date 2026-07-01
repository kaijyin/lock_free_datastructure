/**
 * atomic_pc_publish_compare.cpp
 *
 * producer/consumer 发布普通数据的错误写法和正确写法对比。
 *
 * 错误写法在 x86 上常常“看起来能跑对”，但 C++ 内存模型下没有建立
 * happens-before，payload 的读写是数据竞争。可用 Clang/TSan 验证：
 *
 *   clang++ -std=c++20 -Og -g -fsanitize=thread -fno-omit-frame-pointer \
 *       tests/atomic/atomic_pc_publish_compare.cpp -pthread \
 *       -o /tmp/atomic_pc_publish_compare_tsan
 *   /tmp/atomic_pc_publish_compare_tsan
 *
 * ThreadSanitizer 应报告 RunWrongRelaxedPublish() 中 payload 的 data race；
 * RunCorrectReleaseAcquirePublish() 不应报告 data race。
 */

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

constexpr int kPayloadValue = 42;

struct Message {
  int payload = 0;
  std::atomic<bool> ready{false};
};

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "check failed: " << message << "\n";
    std::abort();
  }
}

int RunWrongRelaxedPublish() {
  Message message;
  int observed = 0;

  std::thread producer([&] {
    message.payload = kPayloadValue;
    message.ready.store(true, std::memory_order_relaxed);
  });

  std::thread consumer([&] {
    while (!message.ready.load(std::memory_order_relaxed)) {
      std::this_thread::yield();
    }
    observed = message.payload;
  });

  producer.join();
  consumer.join();
  return observed;
}

int RunCorrectReleaseAcquirePublish() {
  Message message;
  int observed = 0;

  std::thread producer([&] {
    message.payload = kPayloadValue;
    message.ready.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    while (!message.ready.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    observed = message.payload;
  });

  producer.join();
  consumer.join();
  return observed;
}

}  // namespace

int main() {
  const int wrong = RunWrongRelaxedPublish();
  const int correct = RunCorrectReleaseAcquirePublish();

  std::cout << "producer/consumer publish compare\n";
  std::cout << "wrong relaxed observed:          " << wrong << "\n";
  std::cout << "correct release/acquire observed: " << correct << "\n";

  Check(wrong == kPayloadValue,
        "wrong version often passes on x86 but is still a data race");
  Check(correct == kPayloadValue,
        "correct version should observe the published payload");
  return 0;
}
