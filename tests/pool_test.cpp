#include "memory_pool.h"
#include "object_pool.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

TEST(MemoryPool, AcquireWithinCapacityAndResetReuse) {
  MemoryPool pool(256);

  void *first = pool.acquire(64);
  void *second = pool.acquire(32);

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(pool.used(), 96u);

  pool.reset();

  void *after_reset = pool.acquire(64);
  ASSERT_NE(after_reset, nullptr);
  EXPECT_EQ(after_reset, first);
  EXPECT_EQ(pool.used(), 64u);
}

TEST(MemoryPool, AlignmentAndExhaustion) {
  MemoryPool pool(128);

  void *first = pool.acquire(8, 8);
  void *aligned = pool.acquire(16, 32);
  void *too_large = pool.acquire(256);

  ASSERT_NE(first, nullptr);
  ASSERT_NE(aligned, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(aligned) % 32, 0u);
  EXPECT_EQ(too_large, nullptr);
}

namespace {
struct Widget {
  inline static int live_count = 0;
  int id = 0;
  std::string name;

  Widget(int value, std::string text) : id(value), name(std::move(text)) {
    ++live_count;
  }

  ~Widget() { --live_count; }
};
} // namespace

TEST(ObjectPool, AcquireReleaseAndReuseSlot) {
  ObjectPool<Widget> pool(2);

  Widget *first = pool.acquire(1, "first");
  Widget *second = pool.acquire(2, "second");
  Widget *third = pool.acquire(3, "third");

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(third, nullptr);
  EXPECT_EQ(pool.available(), 0u);
  EXPECT_EQ(Widget::live_count, 2);

  ASSERT_TRUE(pool.release(first));
  EXPECT_EQ(pool.available(), 1u);
  EXPECT_EQ(Widget::live_count, 1);

  Widget *reused = pool.acquire(4, "reused");
  ASSERT_NE(reused, nullptr);
  EXPECT_EQ(reused, first);
  EXPECT_EQ(reused->id, 4);
  EXPECT_EQ(reused->name, "reused");

  EXPECT_TRUE(pool.release(second));
  EXPECT_TRUE(pool.release(reused));
  EXPECT_EQ(pool.available(), 2u);
  EXPECT_EQ(Widget::live_count, 0);
}

TEST(ObjectPool, RejectInvalidOrDoubleRelease) {
  ObjectPool<int> pool(1);

  int *value = pool.acquire(42);
  ASSERT_NE(value, nullptr);

  EXPECT_TRUE(pool.release(value));
  EXPECT_FALSE(pool.release(value));

  int external = 7;
  EXPECT_FALSE(pool.release(&external));
}
