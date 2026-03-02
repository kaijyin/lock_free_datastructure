/**
 * correctness_test.cpp
 *
 * 使用 Google Test 验证所有数据结构的正确性：
 *   1. 单线程基本操作
 *   2. 单线程满/空边界条件
 *   3. 多线程 SPSC 数据完整性（生产者发送，消费者验证无缺失/重复）
 *   4. 多线程 MPMC 数据完整性
 *   5. MS 无界队列多线程正确性
 */
#include "mpmc_ring_buffer.h"
#include "ms_queue.h"
#include "mutex_containers.h"
#include "spsc_linked_queue.h"
#include "spsc_ring_buffer.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

using namespace lf;

// ============================================================
// SPSCRingBuffer 正确性测试
// ============================================================
TEST(SPSCRingBuffer, BasicPushPop) {
  SPSCRingBuffer<int, 8> rb;
  EXPECT_TRUE(rb.empty());

  EXPECT_TRUE(rb.push(1));
  EXPECT_TRUE(rb.push(2));
  EXPECT_TRUE(rb.push(3));

  int v;
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 2);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 3);
  EXPECT_TRUE(rb.empty());
  EXPECT_FALSE(rb.pop(v));
}

TEST(SPSCRingBuffer, BoundaryFull) {
  SPSCRingBuffer<int, 4> rb;
  EXPECT_TRUE(rb.push(10));
  EXPECT_TRUE(rb.push(20));
  EXPECT_TRUE(rb.push(30));
  EXPECT_TRUE(rb.push(40));
  EXPECT_FALSE(rb.push(50)); // 满了
  EXPECT_TRUE(rb.full());

  int v;
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 10);
  EXPECT_TRUE(rb.push(50)); // 弹出一个后可写入

  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 20);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 30);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 40);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 50);
  EXPECT_TRUE(rb.empty());
}

TEST(SPSCRingBuffer, MultiThread_DataIntegrity) {
  constexpr size_t CAP = 1024;
  constexpr size_t N = 1'000'000;
  SPSCRingBuffer<uint64_t, CAP> rb;

  std::atomic<uint64_t> sum_produced{0}, sum_consumed{0};

  std::thread producer([&] {
    for (uint64_t i = 0; i < N; ++i) {
      while (!rb.push(i)) {
        // 队满，自旋
      }
      sum_produced.fetch_add(i, std::memory_order_relaxed);
    }
  });

  std::thread consumer([&] {
    uint64_t v;
    uint64_t count = 0;
    while (count < N) {
      if (rb.pop(v)) {
        sum_consumed.fetch_add(v, std::memory_order_relaxed);
        ++count;
      }
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(sum_produced.load(), sum_consumed.load());
  // 验证 0+1+...+(N-1) = N*(N-1)/2
  uint64_t expected = (uint64_t)N * (N - 1) / 2;
  EXPECT_EQ(sum_consumed.load(), expected);
}

// ============================================================
// MPMCRingBuffer 正确性测试
// ============================================================
TEST(MPMCRingBuffer, BasicPushPop) {
  MPMCRingBuffer<int, 8> rb;
  EXPECT_TRUE(rb.empty());

  for (int i = 0; i < 8; ++i)
    EXPECT_TRUE(rb.push(i));
  EXPECT_FALSE(rb.push(99)); // 满

  int v;
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(rb.pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(rb.pop(v));
}

TEST(MPMCRingBuffer, MultiThread_4P4C) {
  constexpr size_t CAP = 1024;
  constexpr size_t PRODUCERS = 4;
  constexpr size_t CONSUMERS = 4;
  constexpr size_t N_PER_THREAD = 250'000;
  constexpr size_t TOTAL = PRODUCERS * N_PER_THREAD;

  MPMCRingBuffer<uint64_t, CAP> rb;

  std::atomic<uint64_t> sum_produced{0}, sum_consumed{0};
  std::atomic<size_t> consumed_count{0};

  std::vector<std::thread> producers, consumers;

  for (size_t p = 0; p < PRODUCERS; ++p) {
    producers.emplace_back([&, p] {
      uint64_t base = p * N_PER_THREAD;
      for (uint64_t i = base; i < base + N_PER_THREAD; ++i) {
        while (!rb.push(i)) { /* 自旋 */
        }
        sum_produced.fetch_add(i, std::memory_order_relaxed);
      }
    });
  }

  for (size_t c = 0; c < CONSUMERS; ++c) {
    consumers.emplace_back([&] {
      uint64_t v;
      while (consumed_count.load(std::memory_order_relaxed) < TOTAL) {
        if (rb.pop(v)) {
          sum_consumed.fetch_add(v, std::memory_order_relaxed);
          consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto &t : producers)
    t.join();
  for (auto &t : consumers)
    t.join();

  EXPECT_EQ(sum_produced.load(), sum_consumed.load());
}

TEST(MPMCRingBuffer, MultiThread_DataNoDuplicate) {
  // 验证无重复、无丢失：每个值只被消费一次
  constexpr size_t CAP = 512;
  constexpr size_t N = 10000;

  MPMCRingBuffer<int, CAP> rb;
  std::vector<std::atomic<int>> received(N);
  for (auto &a : received)
    a.store(0);

  std::atomic<size_t> consumed{0};

  std::thread prod([&] {
    for (int i = 0; i < (int)N; ++i)
      while (!rb.push(i)) { /* 自旋 */
      }
  });

  std::vector<std::thread> cons_threads;
  for (int c = 0; c < 4; ++c) {
    cons_threads.emplace_back([&] {
      int v;
      while (consumed.load(std::memory_order_relaxed) < N) {
        if (rb.pop(v)) {
          received[v].fetch_add(1);
          consumed.fetch_add(1);
        }
      }
    });
  }

  prod.join();
  for (auto &t : cons_threads)
    t.join();

  for (size_t i = 0; i < N; ++i) {
    EXPECT_EQ(received[i].load(), 1)
        << "Value " << i << " received " << received[i].load() << " times";
  }
}

// ============================================================
// MSQueue 正确性测试
// ============================================================
TEST(MSQueue, BasicPushPop) {
  MSQueue<int> q;
  EXPECT_TRUE(q.empty());

  q.push(1);
  q.push(2);
  q.push(3);

  int v;
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 2);
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 3);
  EXPECT_FALSE(q.pop(v));
  EXPECT_TRUE(q.empty());
}

TEST(MSQueue, MultiThread_4P4C) {
  constexpr size_t PRODUCERS = 4;
  constexpr size_t CONSUMERS = 4;
  constexpr size_t N_PER_THREAD = 50'000;
  constexpr size_t TOTAL = PRODUCERS * N_PER_THREAD;

  // 池大小需足够大：TOTAL + 1（哑节点）
  MSQueue<uint64_t, (1 << 18)> q;

  std::atomic<uint64_t> sum_produced{0}, sum_consumed{0};
  std::atomic<size_t> consumed_count{0};

  std::vector<std::thread> producers, consumers;

  for (size_t p = 0; p < PRODUCERS; ++p) {
    producers.emplace_back([&, p] {
      uint64_t base = p * N_PER_THREAD;
      for (uint64_t i = base; i < base + N_PER_THREAD; ++i) {
        while (!q.push(i)) { /* 池满，极少发生 */
        }
        sum_produced.fetch_add(i, std::memory_order_relaxed);
      }
    });
  }

  for (size_t c = 0; c < CONSUMERS; ++c) {
    consumers.emplace_back([&] {
      uint64_t v;
      while (consumed_count.load(std::memory_order_relaxed) < TOTAL) {
        if (q.pop(v)) {
          sum_consumed.fetch_add(v, std::memory_order_relaxed);
          consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto &t : producers)
    t.join();
  for (auto &t : consumers)
    t.join();

  EXPECT_EQ(sum_produced.load(), sum_consumed.load());
}

// ============================================================
// MutexQueue 正确性测试
// ============================================================
TEST(MutexQueue, BasicPushPop) {
  MutexQueue<int> q;
  q.push(1);
  q.push(2);
  q.push(3);
  int v;
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 2);
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 3);
  EXPECT_FALSE(q.pop(v));
}

// ============================================================
// SPSCLinkedQueue 正确性测试
// ============================================================
TEST(SPSCLinkedQueue, BasicPushPop) {
  SPSCLinkedQueue<int, 64> q;
  EXPECT_TRUE(q.empty());

  EXPECT_TRUE(q.push(1));
  EXPECT_TRUE(q.push(2));
  EXPECT_TRUE(q.push(3));

  int v;
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 2);
  EXPECT_TRUE(q.pop(v));
  EXPECT_EQ(v, 3);
  EXPECT_FALSE(q.pop(v));
  EXPECT_TRUE(q.empty());
}

TEST(SPSCLinkedQueue, NodeReuse) {
  // 验证节点复用：用小池（16 个节点）完成大量 push/pop 而不耗尽
  // 队列深度最大为 1，但总操作次数远超池大小
  SPSCLinkedQueue<int, 16> q;
  int v;
  for (int i = 0; i < 10000; ++i) {
    ASSERT_TRUE(q.push(i)) << "push failed at i=" << i;
    ASSERT_TRUE(q.pop(v)) << "pop failed at i=" << i;
    EXPECT_EQ(v, i);
  }
  EXPECT_TRUE(q.empty());
}

TEST(SPSCLinkedQueue, MultiThread_DataIntegrity) {
  constexpr size_t N = 1'000'000;
  SPSCLinkedQueue<uint64_t, 1 << 17> q;

  std::atomic<uint64_t> sum_produced{0}, sum_consumed{0};

  std::thread producer([&] {
    for (uint64_t i = 0; i < N; ++i) {
      while (!q.push(i)) { /* 池满，极少发生 */
      }
      sum_produced.fetch_add(i, std::memory_order_relaxed);
    }
  });

  std::thread consumer([&] {
    uint64_t v;
    uint64_t count = 0;
    while (count < N) {
      if (q.pop(v)) {
        sum_consumed.fetch_add(v, std::memory_order_relaxed);
        ++count;
      }
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(sum_produced.load(), sum_consumed.load());
  uint64_t expected = (uint64_t)N * (N - 1) / 2;
  EXPECT_EQ(sum_consumed.load(), expected);
}

// ============================================================
// MutexRingBuffer 正确性测试
// ============================================================
TEST(MutexRingBuffer, BasicPushPop) {
  MutexRingBuffer<int, 4> rb;
  EXPECT_TRUE(rb.push(1));
  EXPECT_TRUE(rb.push(2));
  EXPECT_TRUE(rb.push(3));
  EXPECT_TRUE(rb.push(4));
  EXPECT_FALSE(rb.push(5)); // 满

  int v;
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 2);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 3);
  EXPECT_TRUE(rb.pop(v));
  EXPECT_EQ(v, 4);
  EXPECT_FALSE(rb.pop(v));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
