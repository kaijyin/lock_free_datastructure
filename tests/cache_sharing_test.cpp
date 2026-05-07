/**
 * cache_sharing_test.cpp
 *
 * 两线程内存访问模式对比测试，五个场景使用相同的「顺序扫描数组」访问模式，
 * 唯一变量是两线程之间的内存共享关系，确保结果可横向对比：
 *
 *  1. shared_readonly     — 两线程只读同一数组（MESI S 状态，无 invalidate）
 *  2. private_rw          — 两线程各自扫描独立数组（无任何共享，理想基准）
 *  3. false_sharing       — 两线程写同一 cache line 内不同变量（伪共享）
 *  4. false_sharing_fixed — 填充隔离，消除伪共享（对照场景3）
 *  5. true_sharing        — 两线程写同一变量（真共享，最强竞争）
 *
 * 所有场景每次迭代都是「读数组[i] → 写数组[i] → 累加 sum」，
 * 避免单变量循环依赖链干扰，让 cache 访问延迟成为主导瓶颈。
 *
 * 编译示例：
 *   g++ -O2 -std=c++17 -pthread cache_sharing_test.cpp -o cache_sharing_test
 *   运行示例（指定绑核）：
 *   ./cache_sharing_test 0 1
 */

#include <algorithm>
#include <chrono>
#include <cmath>
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

// ─────────────────── 编译期常量 ───────────────────
constexpr std::size_t kCacheLineSize = 64;       // 典型 cache line 大小
constexpr long long kIterations = 100'000'000LL; // 每线程操作次数
constexpr int kWarmupRounds = 2;
constexpr int kMeasureRounds = 6;

// ─────────────────── 工具结构 ───────────────────
struct Stats {
  double mean = 0.0;
  double stddev = 0.0;
  double min = 0.0;
  double max = 0.0;
};

Stats calc_stats(const std::vector<double> &samples) {
  Stats s;
  if (samples.empty())
    return s;
  s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
           static_cast<double>(samples.size());
  s.min = *std::min_element(samples.begin(), samples.end());
  s.max = *std::max_element(samples.begin(), samples.end());
  double var = 0.0;
  for (double v : samples) {
    double d = v - s.mean;
    var += d * d;
  }
  s.stddev = std::sqrt(var / static_cast<double>(samples.size()));
  return s;
}

void bind_to_core(int core_id) {
  if (core_id < 0)
    return;
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(core_id, &cs);
  pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

// ─────────────────── 场景数据布局 ───────────────────

// 工作数组大小：适中，能装入 L2/L3 但大于 L1，体现 cache line 传输代价
// 每线程 256KB = 32K doubles
constexpr std::size_t kWorkArrayElems = 32ULL * 1024ULL;

// 伪共享：两个变量挤在同一 cache line
struct FalseShared {
  // 用数组对齐到同一 cache line，a 在前 32 字节，b 紧随其后（同一行）
  alignas(kCacheLineSize) double arr[kCacheLineSize / sizeof(double)];
  // arr[0..3] 给线程0，arr[4..7] 给线程1，同属一个 cache line
};

// 伪共享消除：两变量各占独立 cache line
struct FalseSharedFixed {
  alignas(
      kCacheLineSize) double a_arr[kCacheLineSize / sizeof(double)]; // 线程0
  alignas(
      kCacheLineSize) double b_arr[kCacheLineSize / sizeof(double)]; // 线程1
};

// 真共享：两线程读写同一数组（完全重叠）
// 直接传同一个 vector 指针即可

// ─────────────────── 统一访问内核：顺序扫描数组，读-改-写，无循环依赖
// ───────────────── 每次迭代：读 arr[i]，乘以扰动系数写回，累加 sum
// 不同场景的区别仅在于 arr 指针指向的内存与另一线程的关系
void scan_rw_thread(double *arr, std::size_t n, long long iters, int core,
                    double &out) {
  bind_to_core(core);
  double sum = 0.0;
  std::size_t idx = 0;
  for (long long i = 0; i < iters; ++i) {
    double v = arr[idx] * 1.0000001 + 1e-9;
    arr[idx] = v;
    sum += v;
    if (++idx >= n)
      idx = 0;
  }
  out = sum;
}

// 场景1 只读：不写回，仅读取
void scan_ro_thread(const double *arr, std::size_t n, long long iters, int core,
                    double &out) {
  bind_to_core(core);
  double sum = 0.0;
  std::size_t idx = 0;
  for (long long i = 0; i < iters; ++i) {
    sum += arr[idx];
    if (++idx >= n)
      idx = 0;
  }
  out = sum;
}

// ─────────────────── 场景1：共享只读 ───────────────────
double run_shared_readonly(int core0, int core1) {
  std::vector<double> data(kWorkArrayElems);
  for (std::size_t i = 0; i < kWorkArrayElems; ++i)
    data[i] = static_cast<double>(i) * 0.001 + 1.0;

  double r0 = 0.0, r1 = 0.0;
  // 两线程读同一数组，同一批 cache line 可在两核间共享（MESI S 状态）
  auto t0 = std::thread(scan_ro_thread, data.data(), kWorkArrayElems,
                        kIterations, core0, std::ref(r0));
  auto t1 = std::thread(scan_ro_thread, data.data(), kWorkArrayElems,
                        kIterations, core1, std::ref(r1));
  t0.join();
  t1.join();
  return r0 + r1;
}

// ─────────────────── 场景2：私有读写（理想基准）───────────────────
double run_private_rw(int core0, int core1) {
  // 两线程各有独立数组，cache line 完全隔离，无任何跨核通信
  std::vector<double> buf0(kWorkArrayElems, 1.0);
  std::vector<double> buf1(kWorkArrayElems, 1.0);

  double r0 = 0.0, r1 = 0.0;
  auto t0 = std::thread(scan_rw_thread, buf0.data(), kWorkArrayElems,
                        kIterations, core0, std::ref(r0));
  auto t1 = std::thread(scan_rw_thread, buf1.data(), kWorkArrayElems,
                        kIterations, core1, std::ref(r1));
  t0.join();
  t1.join();
  return r0 + r1;
}

// ─────────────────── 场景3：伪共享（同 cache
// line，不同位置）──────────────────
double run_false_sharing(int core0, int core1) {
  FalseShared fs{};
  for (auto &v : fs.arr)
    v = 1.0;

  double r0 = 0.0, r1 = 0.0;
  // 线程0 写 arr[0..3]，线程1 写 arr[4..7]，不同变量但同一 cache line
  // 每次写都会触发对方核心的 invalidate
  auto t0 = std::thread(scan_rw_thread, fs.arr + 0, 4, kIterations, core0,
                        std::ref(r0));
  auto t1 = std::thread(scan_rw_thread, fs.arr + 4, 4, kIterations, core1,
                        std::ref(r1));
  t0.join();
  t1.join();
  return r0 + r1;
}

// ───────────────────
// 场景4：伪共享消除（填充隔离）─────────────────────────────
double run_false_sharing_fixed(int core0, int core1) {
  FalseSharedFixed fs{};
  for (auto &v : fs.a_arr)
    v = 1.0;
  for (auto &v : fs.b_arr)
    v = 1.0;

  double r0 = 0.0, r1 = 0.0;
  // 线程0 写 a_arr，线程1 写 b_arr，各占独立 cache line，无 invalidate
  auto t0 =
      std::thread(scan_rw_thread, fs.a_arr, kCacheLineSize / sizeof(double),
                  kIterations, core0, std::ref(r0));
  auto t1 =
      std::thread(scan_rw_thread, fs.b_arr, kCacheLineSize / sizeof(double),
                  kIterations, core1, std::ref(r1));
  t0.join();
  t1.join();
  return r0 + r1;
}

// ─────────────────── 场景5：真共享（极小数组，强制持续争用同一批 cache
// line）──
//
// 关键：将数组缩小到 4 个 cache line（32 个 double），两线程各自循环扫描这
// 同一块内存。由于数组远小于 L1，每次写入都必然命中对方刚 invalidate 掉的
// 同一 cache line，产生持续的 RFO（Request For Ownership）争用。
//
// 对比场景5 vs 场景3（伪共享）的区别：
//   场景3：两线程写 *同一 cache line 内不同位置*，每写一次触发一次 invalidate
//   场景5：两线程写 *完全相同的每一个 cache line*，争用密度是场景3的 N 倍

static void true_sharing_thread(volatile double *arr, std::size_t n,
                                long long iters, int core, double &out) {
  bind_to_core(core);
  double sum = 0.0;
  std::size_t idx = 0;
  for (long long i = 0; i < iters; ++i) {
    arr[idx] = arr[idx] * 1.0000001 + 1.0;
    sum += arr[idx];
    if (++idx >= n)
      idx = 0;
  }
  out = sum;
}

double run_true_sharing(int core0, int core1) {
  // 4 个 cache line = 32 个 double，两线程反复在这 256 字节上争用
  constexpr std::size_t kTinyElems = 4 * kCacheLineSize / sizeof(double);
  alignas(kCacheLineSize) static volatile double shared[kTinyElems];
  for (std::size_t i = 0; i < kTinyElems; ++i)
    shared[i] = 1.0;

  double r0 = 0.0, r1 = 0.0;
  auto t0 = std::thread(true_sharing_thread, shared, kTinyElems, kIterations,
                        core0, std::ref(r0));
  auto t1 = std::thread(true_sharing_thread, shared, kTinyElems, kIterations,
                        core1, std::ref(r1));
  t0.join();
  t1.join();
  return r0 + r1;
}

// ─────────────────── 通用 benchmark 驱动 ───────────────────
template <typename Fn> Stats benchmark(Fn &&fn) {
  std::vector<double> elapsed;
  elapsed.reserve(kMeasureRounds);
  for (int r = 0; r < kWarmupRounds + kMeasureRounds; ++r) {
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile double cs = fn();
    (void)cs;
    auto t1 = std::chrono::high_resolution_clock::now();
    if (r >= kWarmupRounds)
      elapsed.push_back(std::chrono::duration<double>(t1 - t0).count());
  }
  return calc_stats(elapsed);
}

void print_stats(const std::string &label, const Stats &s,
                 double ops_per_iter) {
  const double total_ops = static_cast<double>(kIterations) * 2.0; // 2 线程
  const double mops = total_ops * ops_per_iter / s.mean / 1e6;
  std::cout << std::left << std::setw(38) << label << std::right << std::fixed
            << std::setprecision(3) << "  mean=" << std::setw(8)
            << s.mean * 1000.0 << " ms"
            << "  stddev=" << std::setw(7) << s.stddev * 1000.0 << " ms"
            << "  min=" << std::setw(8) << s.min * 1000.0 << " ms"
            << "  max=" << std::setw(8) << s.max * 1000.0 << " ms"
            << "  MOps/s=" << std::setw(8) << mops << '\n';
}

} // namespace

int main(int argc, char **argv) {
  // 默认绑核：线程0→核0，线程1→核1
  int core0 = 0, core1 = 1;
  if (argc >= 3) {
    core0 = std::stoi(argv[1]);
    core1 = std::stoi(argv[2]);
  }

  const int cpu_count = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
  std::cout << "===== 两线程内存访问模式对比测试 =====\n"
            << "在线 CPU 数 : " << cpu_count << "\n"
            << "线程绑核    : core" << core0 << " / core" << core1 << "\n"
            << "每线程迭代  : " << kIterations / 1'000'000 << " M 次\n"
            << "预热/统计轮 : " << kWarmupRounds << " / " << kMeasureRounds
            << "\n\n";

  const std::size_t work_kb = kWorkArrayElems * sizeof(double) / 1024;
  std::cout << "每线程工作集 : " << work_kb << " KB (" << kWorkArrayElems
            << " doubles)\n";
  std::cout << "访问模式     : 顺序扫描读-改-写，无单变量依赖链\n\n";
  std::cout << std::string(115, '-') << '\n';

  // ── 场景1：共享只读 ──
  {
    auto s = benchmark([&] { return run_shared_readonly(core0, core1); });
    print_stats("1. 共享只读   (shared read-only)", s, 1.0);
  }

  // ── 场景2：私有读写（理想基准）──
  {
    auto s = benchmark([&] { return run_private_rw(core0, core1); });
    print_stats("2. 私有读写   (private RW, baseline)", s, 1.0);
  }

  // ── 场景3：伪共享 ──
  {
    auto s = benchmark([&] { return run_false_sharing(core0, core1); });
    print_stats("3. 伪共享     (false sharing, same line)", s, 1.0);
  }

  // ── 场景4：伪共享消除 ──
  {
    auto s = benchmark([&] { return run_false_sharing_fixed(core0, core1); });
    print_stats("4. 伪共享消除 (false sharing fixed, padded)", s, 1.0);
  }

  // ── 场景5：真共享 ──
  {
    auto s = benchmark([&] { return run_true_sharing(core0, core1); });
    print_stats("5. 真共享     (true sharing, same array)", s, 1.0);
  }

  std::cout
      << std::string(115, '-') << '\n'
      << "说明（所有场景均使用相同的顺序扫描读-改-"
         "写访问模式，唯一变量是共享关系）：\n"
      << "  场景1 共享只读    — 只读同一数组，cache line 在两核间以 S "
         "状态共享，无 invalidate\n"
      << "  场景2 私有读写    — 各自独立数组，零跨核通信，性能上界基准\n"
      << "  场景3 伪共享      — 写同一 cache line "
         "的不同位置，每次写触发对方 invalidate\n"
      << "  场景4 伪共享消除  — 填充隔离到不同 cache line，消除 "
         "invalidate（应接近场景2）\n"
      << "  场景5 真共享      — 两线程反复写同一极小数组(4 cache line)，\n"
         "                      两核持续争夺同一批 cache line 的所有权(RFO)，\n"
         "                      是伪共享的极限放大版，性能应显著差于场景3\n";
  return 0;
}
