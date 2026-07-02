# Atomic 内存序示例说明

本目录中的示例用于观察 C++ atomic 内存序在 producer/consumer 场景下的行为。
重点文件是：

- `atomic_pc_publish_compare.cpp`：对比错误的 `relaxed` 发布和正确的
  `release/acquire` 发布。
- `atomic_memory_order_store_buffering.cpp`：对比相同代码形状下不同
  memory order 的 Store Buffering 结果。
- `atomic_fetch_add_memory_order.cpp`：对比 `fetch_add` 作为纯计数器和作为
  发布信号时，不同 memory order 的含义。
- `atomic_relaxed_reorder.cpp`：观察 GCC 对 atomic 访存、普通内存访存和
  无关计算的编译期重排。

## Producer/Consumer 发布普通数据

`atomic_pc_publish_compare.cpp` 中有两个函数：

```cpp
int RunWrongRelaxedPublish();
int RunCorrectReleaseAcquirePublish();
```

两者业务逻辑相同：producer 写 `payload`，再设置 `ready`；consumer 等到
`ready == true` 后读取 `payload`。

错误写法：

```cpp
message.payload = kPayloadValue;
message.ready.store(true, std::memory_order_relaxed);

while (!message.ready.load(std::memory_order_relaxed)) {
  std::this_thread::yield();
}
observed = message.payload;
```

正确写法：

```cpp
message.payload = kPayloadValue;
message.ready.store(true, std::memory_order_release);

while (!message.ready.load(std::memory_order_acquire)) {
  std::this_thread::yield();
}
observed = message.payload;
```

普通运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target atomic_pc_publish_compare
./build/atomic_pc_publish_compare
```

在当前 x86_64 + GCC 11.4 环境下，通常会看到：

```text
producer/consumer publish compare
wrong relaxed observed:          42
correct release/acquire observed: 42
```

这说明错误版本在 x86 上常常“看起来能跑对”。但这不代表它是正确的 C++
程序。

## 实际架构下的汇编结果

当前环境：

```text
Architecture: x86_64
Compiler: g++ 11.4.0
```

可以用下面命令生成精简汇编：

```bash
g++ -std=c++20 -O3 -march=native -S -masm=intel -x c++ \
  -o /tmp/pc_publish_core.s - <<'CPP'
#include <atomic>

struct Message {
  int payload;
  std::atomic<bool> ready;
};

extern "C" __attribute__((noinline)) void wrong_producer(Message* message) {
  message->payload = 42;
  message->ready.store(true, std::memory_order_relaxed);
}

extern "C" __attribute__((noinline)) int wrong_consumer(Message* message) {
  while (!message->ready.load(std::memory_order_relaxed)) {}
  return message->payload;
}

extern "C" __attribute__((noinline)) void correct_producer(Message* message) {
  message->payload = 42;
  message->ready.store(true, std::memory_order_release);
}

extern "C" __attribute__((noinline)) int correct_consumer(Message* message) {
  while (!message->ready.load(std::memory_order_acquire)) {}
  return message->payload;
}
CPP

sed -n '1,140p' /tmp/pc_publish_core.s
```

核心汇编如下。

错误 producer：

```asm
mov DWORD PTR [rdi], 42
mov BYTE PTR 4[rdi], 1
ret
```

正确 producer：

```asm
mov DWORD PTR [rdi], 42
mov BYTE PTR 4[rdi], 1
ret
```

错误 consumer：

```asm
.L4:
movzx eax, BYTE PTR [rdx]
test  al, al
je    .L4
mov   eax, DWORD PTR [rdi]
ret
```

正确 consumer：

```asm
.L9:
movzx eax, BYTE PTR [rdx]
test  al, al
je    .L9
mov   eax, DWORD PTR [rdi]
ret
```

在 x86_64 上，`store(relaxed)` 和 `store(release)` 通常都编译为普通
`mov`，`load(relaxed)` 和 `load(acquire)` 也通常都编译为普通 `mov`。因此
这两个版本的核心机器码可能完全一样。

## 为什么机器码一样，结果却一个错一个对

C++ 内存模型关注的是语言层面的 happens-before 关系，而不只是某一次在某个
CPU 上生成了什么指令。

错误版本：

```cpp
ready.store(true, std::memory_order_relaxed);
ready.load(std::memory_order_relaxed);
```

`relaxed` 只保证 `ready` 本身的读写是原子的，不建立跨线程同步关系。因此：

```text
producer 写 payload
consumer 读 payload
```

两者之间没有 happens-before。`payload` 是普通 `int`，一个线程写、另一个线程
读，中间没有同步关系，所以这是 data race，属于未定义行为。

正确版本：

```cpp
ready.store(true, std::memory_order_release);
ready.load(std::memory_order_acquire);
```

当 consumer 的 acquire load 读到 producer 的 release store 写入的 `true`
时，二者建立 synchronizes-with，从而得到：

```text
producer 写 payload
happens-before
consumer 读 payload
```

所以 consumer 可以安全读取 `payload == 42`。

x86 的 TSO 内存模型比较强，release/acquire 经常不需要额外 CPU 指令。但
release/acquire 仍然很重要：

- 它给 C++ 编译器明确的同步语义。
- 它让 ThreadSanitizer 等工具知道这里有同步关系。
- 它让代码在 ARM、RISC-V 等弱内存序架构上仍然按 C++ 语义正确。

## 用 ThreadSanitizer 验证

普通运行不一定暴露错误，因为错误版本在 x86 上经常也会打印 `42`。可以用
ThreadSanitizer 检查 data race：

```bash
clang++ -std=c++20 -Og -g -fsanitize=thread -fno-omit-frame-pointer \
  tests/atomic/atomic_pc_publish_compare.cpp -pthread \
  -o /tmp/atomic_pc_publish_compare_tsan

/tmp/atomic_pc_publish_compare_tsan
```

预期现象：

- `RunWrongRelaxedPublish()` 中 `payload` 的读写会被报告为 data race。
- `RunCorrectReleaseAcquirePublish()` 不会因为 `payload` 报 data race。

示例报告中的关键行类似：

```text
WARNING: ThreadSanitizer: data race
Read ... atomic_pc_publish_compare.cpp
Previous write ... atomic_pc_publish_compare.cpp
```

这就是本示例想说明的重点：在 x86 上，错误写法和正确写法可能有相同汇编、
相同输出，但 C++ 内存模型下只有 release/acquire 版本是正确程序。

## `atomic_memory_order_store_buffering.cpp`

这个文件演示经典 Store Buffering 场景：两段代码形状完全一样，只替换
`memory_order`。

核心代码：

```cpp
template <std::memory_order StoreOrder, std::memory_order LoadOrder>
Summary RunStoreBuffering(std::string_view name, std::size_t iterations) {
  // thread 0
  x.store(1, StoreOrder);
  r1[i] = y.load(LoadOrder);

  // thread 1
  y.store(1, StoreOrder);
  r2[i] = x.load(LoadOrder);
}
```

测试统计的是这一种结果出现了多少次：

```cpp
r1[i] == 0 && r2[i] == 0
```

也就是：

```text
thread 0 写了 x=1，但读 y 时没看到 thread 1 的 y=1
thread 1 写了 y=1，但读 x 时没看到 thread 0 的 x=1
```

运行命令：

```bash
cmake --build build --target atomic_memory_order_store_buffering
./build/atomic_memory_order_store_buffering
```

当前 x86_64 + GCC 11.4 环境中的一次运行结果：

```text
Store Buffering: same code, different memory_order

relaxed
iterations:      1000000
both zero count: 10948

release/relaxed
iterations:      1000000
both zero count: 4891

relaxed/acquire
iterations:      1000000
both zero count: 6328

relaxed/seq_cst
iterations:      1000000
both zero count: 9602

seq_cst/relaxed
iterations:      1000000
both zero count: 0

release/acquire
iterations:      1000000
both zero count: 18425

seq_cst
iterations:      1000000
both zero count: 0
```

这些计数每次运行都会变化，但现象稳定：

- `relaxed` 可能出现 both-zero。
- `release/relaxed` 可能出现 both-zero。
- `relaxed/acquire` 可能出现 both-zero。
- `relaxed/seq_cst` 也可能出现 both-zero。它只有 load 是 seq_cst，两个 store
  不是 seq_cst。
- `seq_cst/relaxed` 在当前 x86_64 + GCC 下不会出现 both-zero，因为
  `seq_cst store` 编译成了更强的 `xchg`。
- `release/acquire` 可能出现 both-zero。
- `seq_cst/seq_cst` 禁止 both-zero。

### Store Buffering 的精简汇编

可以用下面命令查看核心两行在 x86_64 上的汇编：

```bash
g++ -std=c++20 -O3 -march=native -S -masm=intel -x c++ \
  -o /tmp/atomic_order_core.s - <<'CPP'
#include <atomic>

std::atomic<int> x{0};
std::atomic<int> y{0};
int r1 = -1;

extern "C" __attribute__((noinline)) void relaxed_core() {
  x.store(1, std::memory_order_relaxed);
  r1 = y.load(std::memory_order_relaxed);
}

extern "C" __attribute__((noinline)) void release_relaxed_core() {
  x.store(1, std::memory_order_release);
  r1 = y.load(std::memory_order_relaxed);
}

extern "C" __attribute__((noinline)) void relaxed_acquire_core() {
  x.store(1, std::memory_order_relaxed);
  r1 = y.load(std::memory_order_acquire);
}

extern "C" __attribute__((noinline)) void relaxed_seq_cst_core() {
  x.store(1, std::memory_order_relaxed);
  r1 = y.load(std::memory_order_seq_cst);
}

extern "C" __attribute__((noinline)) void seq_cst_relaxed_core() {
  x.store(1, std::memory_order_seq_cst);
  r1 = y.load(std::memory_order_relaxed);
}

extern "C" __attribute__((noinline)) void release_acquire_core() {
  x.store(1, std::memory_order_release);
  r1 = y.load(std::memory_order_acquire);
}

extern "C" __attribute__((noinline)) void seq_cst_core() {
  x.store(1, std::memory_order_seq_cst);
  r1 = y.load(std::memory_order_seq_cst);
}
CPP

sed -n '1,180p' /tmp/atomic_order_core.s
```

对普通 lock-free `atomic<int>` 的 load/store，当前 x86_64 + GCC 下可以粗略
总结成：

```text
load(relaxed)   -> mov
load(acquire)   -> mov
load(seq_cst)   -> mov

store(relaxed)  -> mov
store(release)  -> mov
store(seq_cst)  -> xchg
```

也就是说，只有 `seq_cst store` 在这里明显变成了更强的指令。`fetch_add`、
`compare_exchange` 这类 read-modify-write 不属于这个表，它们通常会生成
`lock add`、`lock xadd`、`lock cmpxchg` 之类的 RMW 指令。

不含 `seq_cst store` 的前五种在当前 x86_64 + GCC 下核心汇编相同：

```asm
mov DWORD PTR x[rip], 1
mov eax, DWORD PTR y[rip]
mov DWORD PTR r1[rip], eax
ret
```

其中 `relaxed/seq_cst` 的实现是：

```asm
mov DWORD PTR x[rip], 1    ; x.store(1, relaxed)
mov eax, DWORD PTR y[rip]  ; y.load(seq_cst)
mov DWORD PTR r1[rip], eax
ret
```

这里 `seq_cst load` 仍然只是普通 `mov`。原因是 x86_64 的普通 load 已经足够
满足 acquire/seq_cst load 的硬件约束；这个 load 会参与 C++ 的 seq_cst 全局
顺序，但前面的 `relaxed store` 不参与 seq_cst 全局顺序，也不会因为后面有
seq_cst load 就被强制从 store buffer 刷到其他核心可见。因此
`relaxed/seq_cst` 仍然可能出现 both-zero。

`seq_cst/relaxed` 和 `seq_cst/seq_cst` 不同：

```asm
mov  eax, 1
xchg eax, DWORD PTR x[rip]
mov  eax, DWORD PTR y[rip]
mov  DWORD PTR r1[rip], eax
ret
```

其中 `seq_cst/relaxed` 的实现是：

```asm
mov  eax, 1
xchg eax, DWORD PTR x[rip] ; x.store(1, seq_cst)
mov  eax, DWORD PTR y[rip] ; y.load(relaxed)
mov  DWORD PTR r1[rip], eax
ret
```

这里变强的是 `seq_cst store`，不是 relaxed load。GCC 在 x86_64 上通常用
`xchg` 实现 seq_cst store；`xchg` 对内存操作有隐式锁语义，会让这个 store
在继续执行后续 load 前完成更强的全局可见性约束。于是当前机器上
`seq_cst/relaxed` 实测 both-zero 为 0。

### 为什么 release/acquire 仍然会出现 both-zero

`release/acquire` 建立同步关系有一个关键条件：acquire load 必须读到对方
release store 写入的值。

在 Store Buffering 中：

```text
thread 0: x.store(1, release); r1 = y.load(acquire);
thread 1: y.store(1, release); r2 = x.load(acquire);
```

如果结果是：

```text
r1 == 0
r2 == 0
```

那么两个 acquire load 都读到了初始值 `0`，没有读到对方 release store 写入的
`1`。因此没有建立 synchronizes-with，也就没有跨线程 happens-before。

在 x86 上，不含 `seq_cst store` 的五种核心汇编都是普通 `mov`。普通 store
可能先进入本核心的 store buffer，后续 load 读取的是另一个地址：

```text
core 0: x=1 暂存在 core 0 的 store buffer
core 1: y=1 暂存在 core 1 的 store buffer

core 0: load y，暂时看不到 core 1 的 y=1，读到 0
core 1: load x，暂时看不到 core 0 的 x=1，读到 0
```

所以 both-zero 是允许结果。

### Store Buffer、流水线和 L1 的关系

Store Buffering 不是简单的“数据已经在 L1，只是还没刷新到 L2”。更准确地说，
store buffer 是每个 CPU 核心内部的一个硬件队列，位于执行流水线和缓存一致性
系统之间。

可以粗略理解为：

```text
CPU 执行流水线
    |
    | store 指令执行
    v
store buffer        <- 本核心临时保存尚未全局可见的 store
    |
    | 获取 cache line 独占权限，推进一致性状态
    v
L1 cache / cache coherence
```

普通 store 指令执行时，如果 CPU 必须等待下面这些事情全部完成：

```text
获取 cache line 独占权限
写入 L1 cache
让其他核心的对应 cache line 失效或更新
完成缓存一致性协议状态转换
```

流水线就会频繁停顿。store buffer 的作用是让 store 可以先进入本核心的临时
队列，然后后续指令继续执行。这个 store 对本核心来说已经排入队列，但对其他
核心可能还没有全局可见。

store buffer 中的 store 没有一个固定的保存时间。CPU 会在满足下面条件后尽快
把它提交到 L1/cache coherence：

```text
目标 cache line 已在本核，或者本核已经拿到独占/可修改权限；
更早的 store 已经按架构要求处理完成；
L1/cache pipeline 和一致性事务资源可用。
```

如果目标 cache line 已经在本核的 Modified/Exclusive 状态，提交可能很快。
如果这条 cache line 正在别的核心手里，CPU 需要先通过缓存一致性协议让其他
核心失效或交出所有权，这个 store 在其他核心看来就会晚一些才可见。

因此 Store Buffering 场景里会出现：

```text
core 0:
  mov [x], 1    -> x=1 进入 core 0 的 store buffer
  mov eax, [y]  -> 读取另一个地址 y

core 1:
  mov [y], 1    -> y=1 进入 core 1 的 store buffer
  mov eax, [x]  -> 读取另一个地址 x
```

`core 0` 的 load `y` 看不到 `core 1` store buffer 里的 `y=1`，`core 1`
的 load `x` 也看不到 `core 0` store buffer 里的 `x=1`，于是两边都可能读到
旧值 `0`。

如果 load 的是本核心刚 store 的同一个地址，例如：

```asm
mov [x], 1
mov eax, [x]
```

CPU 通常可以通过 store-to-load forwarding 从自己的 store buffer 读到 `1`。
Store Buffering 的问题出在：每个核心读的是对方刚写的另一个地址，而不是自己
刚写的地址。

所以这里的关键不是“有没有刷新到 L2”，而是：

```text
store 还在本核心 store buffer 中，或者尚未完成让其他核心可见的缓存一致性流程；
后续 load 可以先读取另一个地址；
其他核心暂时看不到这个 store。
```

### 为什么 seq_cst/seq_cst 禁止 both-zero

`seq_cst` 要求所有 `seq_cst` 原子操作能排成一个所有线程都同意的全局顺序。

如果 `r1 == 0 && r2 == 0`：

- `thread 0` 读到 `y == 0`，说明它的 load 在全局顺序中必须早于
  `thread 1` 的 `y.store(1)`。
- `thread 1` 读到 `x == 0`，说明它的 load 在全局顺序中必须早于
  `thread 0` 的 `x.store(1)`。
- 但每个线程内部又要求自己的 store 早于自己的 load。

这些约束合在一起会形成环，无法排出一个合法的全局顺序。因此 C++ 标准禁止
`seq_cst` 版本出现 both-zero。

在当前 x86_64 + GCC 下，`seq_cst store` 也被编译成更强的 `xchg`，带隐式锁
语义。它比普通 `mov` 更强，能够阻止不含 `seq_cst store` 的 `mov + mov`
组合允许的 Store Buffering 结果。

## `atomic_relaxed_reorder.cpp`

这个文件不是做并发运行结果统计，而是专门观察编译器生成的汇编：同一个函数
里混合 atomic load/store、普通全局变量或指针参数上的 load/store、以及与访存
无关的整数计算时，GCC 会怎样调度指令。

运行和查看汇编：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target atomic_relaxed_reorder
./build/atomic_relaxed_reorder

g++ -std=c++20 -O3 -DNDEBUG -march=native -S -masm=intel \
  tests/atomic/atomic_relaxed_reorder.cpp \
  -o build/atomic_relaxed_reorder.s

grep -A60 'PlainReadModifyWritesAroundSeqCstStore' \
  build/atomic_relaxed_reorder.s
```

如果本机缺少 GTest，顶层 CMake 可能在配置阶段失败；这个示例本身不依赖
GTest，可以直接用上面的 `g++` 命令单独编译。

### 主要实验形状

第一类：只看 atomic 访存之间是否被重排。

```cpp
g_store_a.store(1, std::memory_order_relaxed);
g_store_b.store(2, std::memory_order_relaxed);
g_store_c.store(3, std::memory_order_relaxed);

const int a = g_load_a.load(std::memory_order_relaxed);
const int b = g_load_b.load(std::memory_order_relaxed);
const int c = g_load_c.load(std::memory_order_relaxed);
```

文件中还包含 load 后 store、store/load 交错、同一个 atomic 重复访问、通过
指针参数访问 atomic 等版本。

第二类：在 atomic 访存之间加入只在寄存器里完成的整数计算。

```cpp
g_store_a.store(1, std::memory_order_relaxed);
const int first = MixIntegers(x, y);
g_store_b.store(2, std::memory_order_relaxed);
const int second = MixIntegers(static_cast<std::uint32_t>(first), x + 17);
g_store_c.store(3, std::memory_order_relaxed);
```

第三类：在 release/acquire/seq_cst 周围加入普通内存访问。

```cpp
const int before_a = g_plain_a;
const int before_b = g_plain_b;
const int first = MixIntegers(before_a + x, before_b + y);
g_plain_c = first;
g_plain_d = before_a + before_b;

g_store_a.store(1, std::memory_order_release);

const int after_e = g_plain_e;
const int after_f = g_plain_f;
const int second = MixIntegers(after_e + y, after_f + x);
g_plain_g = second;
g_plain_h = after_e + after_f;
```

这个形状也有 acquire load 和 seq_cst store/load 版本。

### 当前 GCC/x86_64 观察结论

当前环境：

```text
Compiler: g++ 12.2.0
Architecture: x86_64
Flags: -O3 -DNDEBUG -march=native
```

观察结果：

- `memory_order_relaxed` 的 atomic load/store 编译成普通 `mov`。
- `memory_order_release` store 和 `memory_order_acquire` load 也编译成普通
  `mov`。
- `memory_order_seq_cst` store 编译成 `xchg`。
- `memory_order_seq_cst` load 仍编译成普通 `mov`。
- 没观察到 GCC 把 atomic 访存和其他 atomic 访存互相重排。
- 同形普通全局变量读改写测试中，没观察到普通 `g_plain_*` 访存跨过
  relaxed atomic store。
- 没观察到普通全局变量或指针参数上的 load/store 跨过 release/acquire。
- 没观察到普通全局变量 load/store 跨过 seq_cst store/load。
- 与访存无关、只在寄存器里完成的计算会被大幅调度。GCC 会把它拆开，
  安排到 atomic 操作前后。
- 普通 `g_plain_*` 访问在 release/acquire/seq_cst 边界的同一侧内部仍可能
  重排，例如同在 release 前的两个普通 store 可以调换顺序。
- `ComplexRelaxedPlainComputeMix()` 这类复杂 relaxed 混合测试中，普通
  `g_plain_*` 写入和计算也会被调度，但没有观察到 atomic relaxed 访存之间
  互相重排。

例如 `ComputeBetweenStoresRelaxed()` 的源码顺序是：

```text
store_a(relaxed)
计算 first
store_b(relaxed)
计算 second
store_c(relaxed)
```

但汇编中 GCC 会把若干普通计算移开，形成类似：

```asm
imul edx, edi, 1664525
mov  DWORD PTR g_store_a[rip], 1
mov  DWORD PTR g_store_b[rip], 2
mov  DWORD PTR g_store_c[rip], 3
imul esi, esi, 1013904223
...
```

这里不是 atomic store 之间被重排；`store_a -> store_b -> store_c` 的顺序仍然
保持。被移动的是与访存无关的普通整数计算。

再看普通内存访问和 release：

```cpp
g_plain_a = 101;
g_plain_b = 202;
g_store_a.store(1, std::memory_order_release);
g_plain_c = 303;
```

当前 GCC 输出仍保持普通 store 不跨过 release：

```asm
mov DWORD PTR g_plain_a[rip], 101
mov DWORD PTR g_plain_b[rip], 202
mov DWORD PTR g_store_a[rip], 1
mov DWORD PTR g_plain_c[rip], 303
```

seq_cst store 版本里，atomic store 会变成 `xchg`：

```asm
mov  esi, 1
xchg esi, DWORD PTR g_store_a[rip]
```

### 注意边界

这个文件记录的是当前 GCC、当前优化参数、当前 x86_64 目标下的实现现象，不是
C++ 标准给出的可移植承诺。C++ 内存模型规定的是语义保证；不同编译器、版本、
优化参数或目标架构都可能生成不同代码。

另外，普通 store 如果后续没有可观察读取，可能被 GCC 删除或折叠成常量。
`ReadManyPlainGlobals()` 的作用就是让“多个普通全局 store”测试不会退化成死写。

## `atomic_fetch_add_memory_order.cpp`

这个文件演示 `fetch_add` 的两个常见用法。

第一种：只把 `fetch_add` 当作计数器。

```cpp
counter.fetch_add(1, order);
```

这个场景下，`memory_order` 不影响计数器本身的原子性。多个线程同时加同一个
atomic 变量，不管使用 `relaxed`、`acquire`、`release`、`acq_rel` 还是
`seq_cst`，最终计数都不会丢。

第二种：把 `fetch_add` 当作发布信号。

```cpp
message.payload = 42;
message.published.fetch_add(1, add_order);

while (message.published.load(load_order) == 0) {
  std::this_thread::yield();
}
observed = message.payload;
```

这时 `memory_order` 会影响普通数据 `payload` 是否被正确发布：

- `relaxed/relaxed`：错误。只保证 `published` 的加法原子，不发布 `payload`。
- `release/relaxed`：错误。producer 做了 release，但 consumer 没有 acquire。
- `relaxed/acquire`：错误。consumer 做了 acquire，但没有读到 release 写入。
- `release/acquire`：正确。consumer 的 acquire load 读到 producer 的
  release `fetch_add` 结果时，建立 synchronizes-with。
- `acq_rel/acquire`：正确。`fetch_add(acq_rel)` 包含 release 语义。
- `seq_cst/seq_cst`：正确，并且所有 seq_cst 操作还参与全局顺序。

运行命令：

```bash
cmake --build build --target atomic_fetch_add_memory_order
./build/atomic_fetch_add_memory_order
```

当前 x86_64 + GCC 11.4 环境中的一次运行结果类似：

```text
fetch_add as a counter
relaxed      expected 2000000, observed 2000000, ns/fetch_add 10.63
acquire      expected 2000000, observed 2000000, ns/fetch_add 9.28
release      expected 2000000, observed 2000000, ns/fetch_add 13.40
acq_rel      expected 2000000, observed 2000000, ns/fetch_add 15.14
seq_cst      expected 2000000, observed 2000000, ns/fetch_add 14.44

atomic<double> fetch_add as a counter
relaxed      expected 1000000.0, observed 1000000.0, lock_free yes, ns/fetch_add 16.06
acquire      expected 1000000.0, observed 1000000.0, lock_free yes, ns/fetch_add 40.20
release      expected 1000000.0, observed 1000000.0, lock_free yes, ns/fetch_add 22.25
acq_rel      expected 1000000.0, observed 1000000.0, lock_free yes, ns/fetch_add 14.64
seq_cst      expected 1000000.0, observed 1000000.0, lock_free yes, ns/fetch_add 44.36

fetch_add as a publish signal
relaxed/relaxed          observed 42, wrong
release/relaxed          observed 42, wrong
relaxed/acquire          observed 42, wrong
release/acquire          observed 42, correct
acq_rel/acquire          observed 42, correct
seq_cst/seq_cst          observed 42, correct
```

注意这里的 `wrong` 不是说这次普通运行一定会打印错误值。在 x86 上它们经常也
打印 `42`。`wrong` 的意思是：C++ 内存模型下没有建立 `payload` 的
happens-before，普通 `int payload` 的跨线程读写是 data race，程序已经是未
定义行为。

可以用 ThreadSanitizer 看这个差异：

```bash
clang++ -std=c++20 -Og -g -fsanitize=thread -fno-omit-frame-pointer \
  tests/atomic/atomic_fetch_add_memory_order.cpp -pthread \
  -o /tmp/atomic_fetch_add_memory_order_tsan

/tmp/atomic_fetch_add_memory_order_tsan
```

预期现象：

- `relaxed/relaxed`、`release/relaxed`、`relaxed/acquire` 这些发布版本会让
  `payload` 的读写缺少同步，TSan 可以报告 data race。
- `release/acquire`、`acq_rel/acquire`、`seq_cst/seq_cst` 是正确发布。

### `fetch_add` 的精简汇编

可以用下面命令查看 x86_64 上的核心指令：

```bash
g++ -std=c++20 -O3 -march=native -S -masm=intel -x c++ \
  -o /tmp/fetch_add_order_core.s - <<'CPP'
#include <atomic>

std::atomic<unsigned long long> counter{0};

extern "C" __attribute__((noinline)) void relaxed_add() {
  counter.fetch_add(1, std::memory_order_relaxed);
}

extern "C" __attribute__((noinline)) void acquire_add() {
  counter.fetch_add(1, std::memory_order_acquire);
}

extern "C" __attribute__((noinline)) void release_add() {
  counter.fetch_add(1, std::memory_order_release);
}

extern "C" __attribute__((noinline)) void acq_rel_add() {
  counter.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" __attribute__((noinline)) void seq_cst_add() {
  counter.fetch_add(1, std::memory_order_seq_cst);
}
CPP

sed -n '1,140p' /tmp/fetch_add_order_core.s
```

当前 x86_64 + GCC 下，这几种通常都会生成同类核心指令：

```asm
lock add QWORD PTR counter[rip], 1
ret
```

原因是 read-modify-write 操作必须原子地完成“读旧值 + 写新值”。在 x86 上，
跨核心共享 atomic 变量的 RMW 通常需要 `lock` 前缀来保证这个操作不可被其他
核心打断。

`atomic<double>::fetch_add` 也是 RMW，但 x86 没有“一条指令完成 double 加法并
原子写回”的通用指令。当前 GCC 12.2 + x86_64 下，`atomic<double>` 在本机
`is_lock_free()` 为 true，但 `fetch_add` 核心实现是 compare-exchange 循环：

```asm
mov          rax, QWORD PTR [rdx]
vmovq        xmm0, rax
vaddsd       xmm0, xmm0, xmm1
vmovq        r8, xmm0
lock cmpxchg QWORD PTR [rdx], r8
jne          retry
```

如果 `cmpxchg` 失败，说明另一个线程已经改了这个 double 值；循环会用新读到的
值重新做浮点加法再尝试写回。因此 `atomic<double>::fetch_add` 也不会丢加法，
但在竞争下通常比整数 `fetch_add` 更贵。

所以在 x86 上，`fetch_add(relaxed)` 和 `fetch_add(seq_cst)` 的核心汇编可能
看起来一样，性能也可能接近。但它们在 C++ 语义上不同：

- `relaxed`：只保证这个 atomic 变量自身的 RMW 原子性。
- `release`：额外保证本线程之前的普通写入不会跑到这个发布操作之后。
- `acquire`：额外保证本线程之后的普通读取不会跑到这个获取操作之前。
- `acq_rel`：同时具有 acquire 和 release 语义。
- `seq_cst`：还加入所有 seq_cst 原子操作共享的全局顺序。

结论是：如果只是统计次数，用 `fetch_add(relaxed)` 通常就够；如果这个计数值
还表示“前面的数据已经可读”，producer 侧至少需要 release，consumer 侧需要
acquire。
