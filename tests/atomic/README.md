# Atomic 内存序示例说明

本目录中的示例用于观察 C++ atomic 内存序在 producer/consumer 场景下的行为。
重点文件是：

- `atomic_pc_publish_compare.cpp`：对比错误的 `relaxed` 发布和正确的
  `release/acquire` 发布。
- `atomic_memory_order_store_buffering.cpp`：对比相同代码形状下不同
  memory order 的 Store Buffering 结果。

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
both zero count: 16599

release/relaxed
iterations:      1000000
both zero count: 10544

relaxed/acquire
iterations:      1000000
both zero count: 16725

release/acquire
iterations:      1000000
both zero count: 17542

seq_cst
iterations:      1000000
both zero count: 0
```

这些计数每次运行都会变化，但现象稳定：

- `relaxed` 可能出现 both-zero。
- `release/relaxed` 可能出现 both-zero。
- `relaxed/acquire` 可能出现 both-zero。
- `release/acquire` 可能出现 both-zero。
- `seq_cst` 禁止 both-zero。

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

前四种在当前 x86_64 + GCC 下的核心汇编相同：

```asm
mov DWORD PTR x[rip], 1
mov eax, DWORD PTR y[rip]
mov DWORD PTR r1[rip], eax
ret
```

`seq_cst` 不同：

```asm
mov  eax, 1
xchg eax, DWORD PTR x[rip]
mov  eax, DWORD PTR y[rip]
mov  DWORD PTR r1[rip], eax
ret
```

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

在 x86 上，前四种核心汇编都是普通 `mov`。普通 store 可能先进入本核心的
store buffer，后续 load 读取的是另一个地址：

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

### 为什么 seq_cst 禁止 both-zero

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
语义。它比普通 `mov` 更强，能够阻止前四种 `mov + mov` 允许的 Store
Buffering 结果。
