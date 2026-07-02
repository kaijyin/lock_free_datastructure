/**
 * atomic_relaxed_reorder.cpp
 *
 * 观察同一个函数里，多个不同 atomic 变量的 relaxed store/load，以及与访存
 * 无关的整数计算，在编译器生成汇编时是否会被重排。
 *
 * 建议用下面命令看核心函数汇编：
 *
 *   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build --target atomic_relaxed_reorder
 *   objdump -d -M intel --demangle build/atomic_relaxed_reorder \
 *       | grep -A20 'Relaxed'
 */

#include <atomic>
#include <cstdint>
#include <iostream>

namespace {

#if defined(__GNUC__) && !defined(__clang__)
#define ATOMIC_REORDER_NOINLINE __attribute__((noipa))
#elif defined(__GNUC__) || defined(__clang__)
#define ATOMIC_REORDER_NOINLINE __attribute__((noinline))
#else
#define ATOMIC_REORDER_NOINLINE
#endif

std::atomic<int> g_store_a{0};
std::atomic<int> g_store_b{0};
std::atomic<int> g_store_c{0};
std::atomic<int> g_load_a{11};
std::atomic<int> g_load_b{22};
std::atomic<int> g_load_c{33};
int g_plain_a = 101;
int g_plain_b = 202;
int g_plain_c = 303;
int g_plain_d = 404;
int g_plain_e = 505;
int g_plain_f = 606;
int g_plain_g = 707;
int g_plain_h = 808;

inline int MixIntegers(std::uint32_t x, std::uint32_t y) {
  std::uint32_t z = x * 1'664'525u + y * 1'013'904'223u;
  z ^= z >> 16;
  z *= 2'246'822'519u;
  z ^= z >> 13;
  return static_cast<int>(z);
}

extern "C" ATOMIC_REORDER_NOINLINE int StoreThenLoadRelaxed() {
  g_store_a.store(1, std::memory_order_relaxed);
  g_store_b.store(2, std::memory_order_relaxed);
  g_store_c.store(3, std::memory_order_relaxed);

  const int a = g_load_a.load(std::memory_order_relaxed);
  const int b = g_load_b.load(std::memory_order_relaxed);
  const int c = g_load_c.load(std::memory_order_relaxed);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int LoadThenStoreRelaxed() {
  const int a = g_load_a.load(std::memory_order_relaxed);
  const int b = g_load_b.load(std::memory_order_relaxed);
  const int c = g_load_c.load(std::memory_order_relaxed);

  g_store_a.store(1, std::memory_order_relaxed);
  g_store_b.store(2, std::memory_order_relaxed);
  g_store_c.store(3, std::memory_order_relaxed);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int InterleavedRelaxed() {
  g_store_a.store(1, std::memory_order_relaxed);
  const int a = g_load_a.load(std::memory_order_relaxed);
  g_store_b.store(2, std::memory_order_relaxed);
  const int b = g_load_b.load(std::memory_order_relaxed);
  g_store_c.store(3, std::memory_order_relaxed);
  const int c = g_load_c.load(std::memory_order_relaxed);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int SameAtomicRelaxed() {
  g_store_a.store(1, std::memory_order_relaxed);
  const int a = g_store_a.load(std::memory_order_relaxed);
  g_store_a.store(2, std::memory_order_relaxed);
  const int b = g_store_a.load(std::memory_order_relaxed);

  return a + b;
}

extern "C" ATOMIC_REORDER_NOINLINE int RepeatedSameAtomicRelaxed() {
  const int a = g_load_a.load(std::memory_order_relaxed);
  const int b = g_load_a.load(std::memory_order_relaxed);
  g_store_a.store(1, std::memory_order_relaxed);
  g_store_a.store(2, std::memory_order_relaxed);

  return a + b;
}

extern "C" ATOMIC_REORDER_NOINLINE int StoreThenLoadRelaxedByPointer(
    std::atomic<int> *store_a, std::atomic<int> *store_b,
    std::atomic<int> *store_c, std::atomic<int> *load_a,
    std::atomic<int> *load_b, std::atomic<int> *load_c) {
  store_a->store(1, std::memory_order_relaxed);
  store_b->store(2, std::memory_order_relaxed);
  store_c->store(3, std::memory_order_relaxed);

  const int a = load_a->load(std::memory_order_relaxed);
  const int b = load_b->load(std::memory_order_relaxed);
  const int c = load_c->load(std::memory_order_relaxed);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int LocalAtomicsRelaxed() {
  std::atomic<int> store_a{0};
  std::atomic<int> store_b{0};
  std::atomic<int> store_c{0};
  std::atomic<int> load_a{11};
  std::atomic<int> load_b{22};
  std::atomic<int> load_c{33};

  store_a.store(1, std::memory_order_relaxed);
  store_b.store(2, std::memory_order_relaxed);
  store_c.store(3, std::memory_order_relaxed);

  const int a = load_a.load(std::memory_order_relaxed);
  const int b = load_b.load(std::memory_order_relaxed);
  const int c = load_c.load(std::memory_order_relaxed);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeBeforeStoreThenLoadRelaxed(std::uint32_t x, std::uint32_t y) {
  const int computed = MixIntegers(x, y);

  g_store_a.store(1, std::memory_order_relaxed);
  g_store_b.store(2, std::memory_order_relaxed);
  g_store_c.store(3, std::memory_order_relaxed);

  const int a = g_load_a.load(std::memory_order_relaxed);
  const int b = g_load_b.load(std::memory_order_relaxed);
  const int c = g_load_c.load(std::memory_order_relaxed);

  return computed + a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeBetweenStoresRelaxed(std::uint32_t x, std::uint32_t y) {
  g_store_a.store(1, std::memory_order_relaxed);
  const int first = MixIntegers(x, y);
  g_store_b.store(2, std::memory_order_relaxed);
  const int second = MixIntegers(static_cast<std::uint32_t>(first), x + 17);
  g_store_c.store(3, std::memory_order_relaxed);

  return first + second;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeBetweenStoreAndLoadRelaxed(std::uint32_t x, std::uint32_t y) {
  g_store_a.store(1, std::memory_order_relaxed);
  g_store_b.store(2, std::memory_order_relaxed);
  g_store_c.store(3, std::memory_order_relaxed);

  const int computed = MixIntegers(x, y);

  const int a = g_load_a.load(std::memory_order_relaxed);
  const int b = g_load_b.load(std::memory_order_relaxed);
  const int c = g_load_c.load(std::memory_order_relaxed);

  return computed + a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeAfterLoadThenStoreRelaxed(std::uint32_t x, std::uint32_t y) {
  const int a = g_load_a.load(std::memory_order_relaxed);
  const int b = g_load_b.load(std::memory_order_relaxed);
  const int c = g_load_c.load(std::memory_order_relaxed);

  const int computed = MixIntegers(x, y);

  g_store_a.store(1, std::memory_order_relaxed);
  g_store_b.store(2, std::memory_order_relaxed);
  g_store_c.store(3, std::memory_order_relaxed);

  return computed + a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
IndependentComputeAroundRelaxed(std::uint32_t x, std::uint32_t y) {
  const int first = MixIntegers(x, y);
  g_store_a.store(1, std::memory_order_relaxed);
  const int second = MixIntegers(x + 1, y + 2);
  const int a = g_load_a.load(std::memory_order_relaxed);
  const int third = MixIntegers(x + 3, y + 4);
  g_store_b.store(2, std::memory_order_relaxed);
  const int b = g_load_b.load(std::memory_order_relaxed);

  return first + second + third + a + b;
}

extern "C" ATOMIC_REORDER_NOINLINE int
DependentComputeAfterLoadRelaxed(std::uint32_t x, std::uint32_t y) {
  g_store_a.store(1, std::memory_order_relaxed);
  const int a = g_load_a.load(std::memory_order_relaxed);
  const int first = MixIntegers(static_cast<std::uint32_t>(a) + x, y);
  const int b = g_load_b.load(std::memory_order_relaxed);
  const int second = MixIntegers(static_cast<std::uint32_t>(b) + y, x);
  g_store_b.store(2, std::memory_order_relaxed);

  return first + second;
}

extern "C" ATOMIC_REORDER_NOINLINE int StoreThenLoadReleaseAcquire() {
  g_store_a.store(1, std::memory_order_release);
  g_store_b.store(2, std::memory_order_release);
  g_store_c.store(3, std::memory_order_release);

  const int a = g_load_a.load(std::memory_order_acquire);
  const int b = g_load_b.load(std::memory_order_acquire);
  const int c = g_load_c.load(std::memory_order_acquire);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int LoadThenStoreAcquireRelease() {
  const int a = g_load_a.load(std::memory_order_acquire);
  const int b = g_load_b.load(std::memory_order_acquire);
  const int c = g_load_c.load(std::memory_order_acquire);

  g_store_a.store(1, std::memory_order_release);
  g_store_b.store(2, std::memory_order_release);
  g_store_c.store(3, std::memory_order_release);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int InterleavedReleaseAcquire() {
  g_store_a.store(1, std::memory_order_release);
  const int a = g_load_a.load(std::memory_order_acquire);
  g_store_b.store(2, std::memory_order_release);
  const int b = g_load_b.load(std::memory_order_acquire);
  g_store_c.store(3, std::memory_order_release);
  const int c = g_load_c.load(std::memory_order_acquire);

  return a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeBeforeStoreThenLoadReleaseAcquire(std::uint32_t x, std::uint32_t y) {
  const int computed = MixIntegers(x, y);

  g_store_a.store(1, std::memory_order_release);
  g_store_b.store(2, std::memory_order_release);
  g_store_c.store(3, std::memory_order_release);

  const int a = g_load_a.load(std::memory_order_acquire);
  const int b = g_load_b.load(std::memory_order_acquire);
  const int c = g_load_c.load(std::memory_order_acquire);

  return computed + a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeBetweenStoresRelease(std::uint32_t x, std::uint32_t y) {
  g_store_a.store(1, std::memory_order_release);
  const int first = MixIntegers(x, y);
  g_store_b.store(2, std::memory_order_release);
  const int second = MixIntegers(static_cast<std::uint32_t>(first), x + 17);
  g_store_c.store(3, std::memory_order_release);

  return first + second;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeBetweenStoreAndLoadReleaseAcquire(std::uint32_t x, std::uint32_t y) {
  g_store_a.store(1, std::memory_order_release);
  g_store_b.store(2, std::memory_order_release);
  g_store_c.store(3, std::memory_order_release);

  const int computed = MixIntegers(x, y);

  const int a = g_load_a.load(std::memory_order_acquire);
  const int b = g_load_b.load(std::memory_order_acquire);
  const int c = g_load_c.load(std::memory_order_acquire);

  return computed + a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComputeAfterLoadThenStoreAcquireRelease(std::uint32_t x, std::uint32_t y) {
  const int a = g_load_a.load(std::memory_order_acquire);
  const int b = g_load_b.load(std::memory_order_acquire);
  const int c = g_load_c.load(std::memory_order_acquire);

  const int computed = MixIntegers(x, y);

  g_store_a.store(1, std::memory_order_release);
  g_store_b.store(2, std::memory_order_release);
  g_store_c.store(3, std::memory_order_release);

  return computed + a + b + c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
IndependentComputeAroundReleaseAcquire(std::uint32_t x, std::uint32_t y) {
  const int first = MixIntegers(x, y);
  g_store_a.store(1, std::memory_order_release);
  const int second = MixIntegers(x + 1, y + 2);
  const int a = g_load_a.load(std::memory_order_acquire);
  const int third = MixIntegers(x + 3, y + 4);
  g_store_b.store(2, std::memory_order_release);
  const int b = g_load_b.load(std::memory_order_acquire);

  return first + second + third + a + b;
}

extern "C" ATOMIC_REORDER_NOINLINE int
DependentComputeAfterLoadAcquireRelease(std::uint32_t x, std::uint32_t y) {
  g_store_a.store(1, std::memory_order_release);
  const int a = g_load_a.load(std::memory_order_acquire);
  const int first = MixIntegers(static_cast<std::uint32_t>(a) + x, y);
  const int b = g_load_b.load(std::memory_order_acquire);
  const int second = MixIntegers(static_cast<std::uint32_t>(b) + y, x);
  g_store_b.store(2, std::memory_order_release);

  return first + second;
}

extern "C" ATOMIC_REORDER_NOINLINE int GlobalPlainStoresAroundRelease() {
  g_plain_a = 101;
  g_plain_b = 202;
  g_store_a.store(1, std::memory_order_release);
  g_plain_c = 303;

  return g_plain_a + g_plain_b + g_plain_c;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ManyGlobalPlainStoresAroundReleaseWithCompute(std::uint32_t x,
                                              std::uint32_t y) {
  g_plain_a = 101;
  const int first = MixIntegers(x, y);
  g_plain_b = 202;
  g_plain_c = 303;
  const int second = MixIntegers(x + 1, y + 2);

  g_store_a.store(1, std::memory_order_release);

  g_plain_d = 404;
  const int third = MixIntegers(x + 3, y + 4);
  g_plain_e = 505;
  g_plain_f = 606;
  const int fourth = MixIntegers(x + 5, y + 6);
  g_plain_g = 707;
  g_plain_h = 808;

  return first + second + third + fourth + g_plain_a + g_plain_b + g_plain_c +
         g_plain_d + g_plain_e + g_plain_f + g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int ReadManyPlainGlobals() {
  return g_plain_a + g_plain_b + g_plain_c + g_plain_d + g_plain_e + g_plain_f +
         g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int
PlainReadModifyWritesAroundRelease(std::uint32_t x, std::uint32_t y) {
  const int before_a = g_plain_a;
  const int before_b = g_plain_b;
  const int first = MixIntegers(static_cast<std::uint32_t>(before_a) + x,
                                static_cast<std::uint32_t>(before_b) + y);
  g_plain_c = first;
  g_plain_d = before_a + before_b;

  g_store_a.store(1, std::memory_order_release);

  const int after_e = g_plain_e;
  const int after_f = g_plain_f;
  const int second = MixIntegers(static_cast<std::uint32_t>(after_e) + y,
                                 static_cast<std::uint32_t>(after_f) + x);
  g_plain_g = second;
  g_plain_h = after_e + after_f;

  return first + second + g_plain_c + g_plain_d + g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int
PlainReadModifyWritesAroundRelaxedStore(std::uint32_t x, std::uint32_t y) {
  const int before_a = g_plain_a;
  const int before_b = g_plain_b;
  const int first = MixIntegers(static_cast<std::uint32_t>(before_a) + x,
                                static_cast<std::uint32_t>(before_b) + y);
  g_plain_c = first;
  g_plain_d = before_a + before_b;

  g_store_a.store(1, std::memory_order_relaxed);

  const int after_e = g_plain_e;
  const int after_f = g_plain_f;
  const int second = MixIntegers(static_cast<std::uint32_t>(after_e) + y,
                                 static_cast<std::uint32_t>(after_f) + x);
  g_plain_g = second;
  g_plain_h = after_e + after_f;

  return first + second + g_plain_c + g_plain_d + g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int
PlainReadModifyWritesAroundAcquire(std::uint32_t x, std::uint32_t y) {
  const int before_a = g_plain_a;
  const int before_b = g_plain_b;
  const int first = MixIntegers(static_cast<std::uint32_t>(before_a) + x,
                                static_cast<std::uint32_t>(before_b) + y);
  g_plain_c = first;
  g_plain_d = before_a + before_b;

  const int flag = g_store_a.load(std::memory_order_acquire);

  const int after_e = g_plain_e;
  const int after_f = g_plain_f;
  const int second = MixIntegers(static_cast<std::uint32_t>(after_e) + y,
                                 static_cast<std::uint32_t>(after_f) + x);
  g_plain_g = second;
  g_plain_h = after_e + after_f;

  return flag + first + second + g_plain_c + g_plain_d + g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int
PlainReadModifyWritesAroundSeqCstStore(std::uint32_t x, std::uint32_t y) {
  const int before_a = g_plain_a;
  const int before_b = g_plain_b;
  const int first = MixIntegers(static_cast<std::uint32_t>(before_a) + x,
                                static_cast<std::uint32_t>(before_b) + y);
  g_plain_c = first;
  g_plain_d = before_a + before_b;

  g_store_a.store(1, std::memory_order_seq_cst);

  const int after_e = g_plain_e;
  const int after_f = g_plain_f;
  const int second = MixIntegers(static_cast<std::uint32_t>(after_e) + y,
                                 static_cast<std::uint32_t>(after_f) + x);
  g_plain_g = second;
  g_plain_h = after_e + after_f;

  return first + second + g_plain_c + g_plain_d + g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int
PlainReadModifyWritesAroundSeqCstLoad(std::uint32_t x, std::uint32_t y) {
  const int before_a = g_plain_a;
  const int before_b = g_plain_b;
  const int first = MixIntegers(static_cast<std::uint32_t>(before_a) + x,
                                static_cast<std::uint32_t>(before_b) + y);
  g_plain_c = first;
  g_plain_d = before_a + before_b;

  const int flag = g_store_a.load(std::memory_order_seq_cst);

  const int after_e = g_plain_e;
  const int after_f = g_plain_f;
  const int second = MixIntegers(static_cast<std::uint32_t>(after_e) + y,
                                 static_cast<std::uint32_t>(after_f) + x);
  g_plain_g = second;
  g_plain_h = after_e + after_f;

  return flag + first + second + g_plain_c + g_plain_d + g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int
ComplexRelaxedPlainComputeMix(std::uint32_t x, std::uint32_t y) {
  const int plain_a = g_plain_a;
  const int plain_b = g_plain_b;
  const int independent_1 = MixIntegers(x, y);

  g_store_a.store(plain_a + independent_1, std::memory_order_relaxed);

  const int load_a = g_load_a.load(std::memory_order_relaxed);
  const int dependent_1 = MixIntegers(static_cast<std::uint32_t>(load_a) + x,
                                      static_cast<std::uint32_t>(plain_b) + y);
  g_plain_c = dependent_1;

  const int independent_2 = MixIntegers(x + 11, y + 13);
  g_store_b.store(independent_2, std::memory_order_relaxed);

  const int plain_e = g_plain_e;
  const int load_b = g_load_b.load(std::memory_order_relaxed);
  const int dependent_2 = MixIntegers(static_cast<std::uint32_t>(load_b) +
                                          static_cast<std::uint32_t>(plain_e),
                                      static_cast<std::uint32_t>(dependent_1));
  g_plain_d = plain_a + plain_b + plain_e;

  g_store_c.store(dependent_2, std::memory_order_relaxed);

  const int load_c = g_load_c.load(std::memory_order_relaxed);
  const int plain_f = g_plain_f;
  const int dependent_3 = MixIntegers(static_cast<std::uint32_t>(load_c) +
                                          static_cast<std::uint32_t>(plain_f),
                                      static_cast<std::uint32_t>(independent_1 +
                                                                 independent_2));
  g_plain_g = dependent_3;

  const int independent_3 = MixIntegers(x + 17, y + 19);
  g_plain_h = independent_3 + dependent_2;

  g_store_a.store(dependent_3 + independent_3, std::memory_order_relaxed);
  const int reload_a = g_store_a.load(std::memory_order_relaxed);

  return independent_1 + independent_2 + independent_3 + dependent_1 +
         dependent_2 + dependent_3 + reload_a + g_plain_c + g_plain_d +
         g_plain_g + g_plain_h;
}

extern "C" ATOMIC_REORDER_NOINLINE int GlobalPlainLoadsAroundAcquire() {
  const int before = g_plain_a;
  const int flag = g_store_a.load(std::memory_order_acquire);
  const int after_b = g_plain_b;
  const int after_c = g_plain_c;

  return before + flag + after_b + after_c;
}

extern "C" ATOMIC_REORDER_NOINLINE int GlobalPlainStoresBeforeRelease(int a,
                                                                      int b) {
  g_plain_a = a;
  g_plain_b = b;
  g_store_a.store(1, std::memory_order_release);

  return g_plain_a + g_plain_b;
}

extern "C" ATOMIC_REORDER_NOINLINE int GlobalPlainLoadsAfterAcquire() {
  const int flag = g_store_a.load(std::memory_order_acquire);
  const int a = g_plain_a;
  const int b = g_plain_b;

  return flag + a + b;
}

extern "C" ATOMIC_REORDER_NOINLINE int
PointerPlainStoresAroundRelease(int *before_a, int *before_b, int *after,
                                std::atomic<int> *flag) {
  *before_a = 101;
  *before_b = 202;
  flag->store(1, std::memory_order_release);
  *after = 303;

  return *before_a + *before_b + *after;
}

extern "C" ATOMIC_REORDER_NOINLINE int
PointerPlainLoadsAroundAcquire(const int *before, const int *after_a,
                               const int *after_b, std::atomic<int> *flag) {
  const int before_value = *before;
  const int flag_value = flag->load(std::memory_order_acquire);
  const int after_value_a = *after_a;
  const int after_value_b = *after_b;

  return before_value + flag_value + after_value_a + after_value_b;
}

}  // namespace

// 本文件在 GCC 12.2.0 + x86_64 下用以下参数观察汇编：
//   -O3 -DNDEBUG -march=native
//
// 1. 在当前平台上，`memory_order_relaxed` 的 atomic load/store 会编译成
//    普通 `mov` 指令。本文件这些测试里，没有观察到 GCC 把 relaxed atomic
//    访存和其他 atomic 访存互相重排；同形普通全局变量读改写测试里，也没有
//    观察到普通 `g_plain_*` 访存跨过 relaxed atomic store。
// 2. 在当前平台上，`memory_order_release` store 和
//    `memory_order_acquire` load 也会编译成普通 `mov` 指令。但在这些测试
//    里，它们仍然对普通内存访问形成编译器层面的顺序约束：普通全局变量或
//    指针参数上的 load/store 没有跨过 release/acquire 操作。
// 3. `memory_order_seq_cst` store 在当前 GCC/x86_64 下编译成 `xchg`；
//    `memory_order_seq_cst` load 仍编译成普通 `mov`。普通内存访问没有跨过
//    seq_cst store/load。
// 4. 与访存无关、只在寄存器里完成的整数计算，不会被 relaxed、release、
//    acquire 或 seq_cst 固定在源码位置。GCC 会把这些计算拆开并调度到
//    atomic 操作前后，以改善指令调度。
// 5. 普通 `g_plain_*` 访问在 release/acquire/seq_cst 边界的同一侧内部仍
//    可能重排。例如 release 前的普通 store 可以和 release 前的其他 store
//    调换顺序；acquire 后的普通 store 也可以和 acquire 后的计算交错。
//    在 `ComplexRelaxedPlainComputeMix()` 这类复杂 relaxed 混合测试里，
//    普通 `g_plain_*` 写入和计算也会被调度，但没有观察到 atomic relaxed
//    访存之间互相重排。
// 6. 这些只是当前编译器、优化参数和 CPU 目标下的实现观察，不是可移植承诺。
//    C++ 内存模型规定的是语义保证；不同编译器、版本、优化参数和目标架构
//    都可能生成不同代码。

int main() {
  const std::uint32_t seed_x =
      static_cast<std::uint32_t>(g_load_a.load(std::memory_order_relaxed));
  const std::uint32_t seed_y =
      static_cast<std::uint32_t>(g_load_b.load(std::memory_order_relaxed));

  std::cout << "StoreThenLoadRelaxed: " << StoreThenLoadRelaxed() << "\n";
  std::cout << "LoadThenStoreRelaxed: " << LoadThenStoreRelaxed() << "\n";
  std::cout << "InterleavedRelaxed: " << InterleavedRelaxed() << "\n";
  std::cout << "SameAtomicRelaxed: " << SameAtomicRelaxed() << "\n";
  std::cout << "RepeatedSameAtomicRelaxed: " << RepeatedSameAtomicRelaxed()
            << "\n";
  std::cout << "StoreThenLoadRelaxedByPointer: "
            << StoreThenLoadRelaxedByPointer(&g_store_a, &g_store_b, &g_store_c,
                                             &g_load_a, &g_load_b, &g_load_c)
            << "\n";
  std::cout << "LocalAtomicsRelaxed: " << LocalAtomicsRelaxed() << "\n";
  std::cout << "ComputeBeforeStoreThenLoadRelaxed: "
            << ComputeBeforeStoreThenLoadRelaxed(seed_x, seed_y) << "\n";
  std::cout << "ComputeBetweenStoresRelaxed: "
            << ComputeBetweenStoresRelaxed(seed_x, seed_y) << "\n";
  std::cout << "ComputeBetweenStoreAndLoadRelaxed: "
            << ComputeBetweenStoreAndLoadRelaxed(seed_x, seed_y) << "\n";
  std::cout << "ComputeAfterLoadThenStoreRelaxed: "
            << ComputeAfterLoadThenStoreRelaxed(seed_x, seed_y) << "\n";
  std::cout << "IndependentComputeAroundRelaxed: "
            << IndependentComputeAroundRelaxed(seed_x, seed_y) << "\n";
  std::cout << "DependentComputeAfterLoadRelaxed: "
            << DependentComputeAfterLoadRelaxed(seed_x, seed_y) << "\n";
  std::cout << "StoreThenLoadReleaseAcquire: " << StoreThenLoadReleaseAcquire()
            << "\n";
  std::cout << "LoadThenStoreAcquireRelease: " << LoadThenStoreAcquireRelease()
            << "\n";
  std::cout << "InterleavedReleaseAcquire: " << InterleavedReleaseAcquire()
            << "\n";
  std::cout << "ComputeBeforeStoreThenLoadReleaseAcquire: "
            << ComputeBeforeStoreThenLoadReleaseAcquire(seed_x, seed_y) << "\n";
  std::cout << "ComputeBetweenStoresRelease: "
            << ComputeBetweenStoresRelease(seed_x, seed_y) << "\n";
  std::cout << "ComputeBetweenStoreAndLoadReleaseAcquire: "
            << ComputeBetweenStoreAndLoadReleaseAcquire(seed_x, seed_y) << "\n";
  std::cout << "ComputeAfterLoadThenStoreAcquireRelease: "
            << ComputeAfterLoadThenStoreAcquireRelease(seed_x, seed_y) << "\n";
  std::cout << "IndependentComputeAroundReleaseAcquire: "
            << IndependentComputeAroundReleaseAcquire(seed_x, seed_y) << "\n";
  std::cout << "DependentComputeAfterLoadAcquireRelease: "
            << DependentComputeAfterLoadAcquireRelease(seed_x, seed_y) << "\n";
  std::cout << "GlobalPlainStoresAroundRelease: "
            << GlobalPlainStoresAroundRelease() << "\n";
  std::cout << "ManyGlobalPlainStoresAroundReleaseWithCompute: "
            << ManyGlobalPlainStoresAroundReleaseWithCompute(seed_x, seed_y)
            << "\n";
  std::cout << "ReadManyPlainGlobals: " << ReadManyPlainGlobals() << "\n";
  std::cout << "PlainReadModifyWritesAroundRelease: "
            << PlainReadModifyWritesAroundRelease(seed_x, seed_y) << "\n";
  std::cout << "PlainReadModifyWritesAroundRelaxedStore: "
            << PlainReadModifyWritesAroundRelaxedStore(seed_x, seed_y) << "\n";
  std::cout << "PlainReadModifyWritesAroundAcquire: "
            << PlainReadModifyWritesAroundAcquire(seed_x, seed_y) << "\n";
  std::cout << "PlainReadModifyWritesAroundSeqCstStore: "
            << PlainReadModifyWritesAroundSeqCstStore(seed_x, seed_y) << "\n";
  std::cout << "PlainReadModifyWritesAroundSeqCstLoad: "
            << PlainReadModifyWritesAroundSeqCstLoad(seed_x, seed_y) << "\n";
  std::cout << "ComplexRelaxedPlainComputeMix: "
            << ComplexRelaxedPlainComputeMix(seed_x, seed_y) << "\n";
  std::cout << "GlobalPlainLoadsAroundAcquire: "
            << GlobalPlainLoadsAroundAcquire() << "\n";
  std::cout << "GlobalPlainStoresBeforeRelease: "
            << GlobalPlainStoresBeforeRelease(404, 505) << "\n";
  std::cout << "GlobalPlainLoadsAfterAcquire: "
            << GlobalPlainLoadsAfterAcquire() << "\n";
  std::cout << "PointerPlainStoresAroundRelease: "
            << PointerPlainStoresAroundRelease(&g_plain_a, &g_plain_b,
                                               &g_plain_c, &g_store_a)
            << "\n";
  std::cout << "PointerPlainLoadsAroundAcquire: "
            << PointerPlainLoadsAroundAcquire(&g_plain_a, &g_plain_b,
                                              &g_plain_c, &g_store_a)
            << "\n";
  return 0;
}

#undef ATOMIC_REORDER_NOINLINE
