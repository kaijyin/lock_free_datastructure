# C++ 原子变量与内存序详解

## 目录

- [为什么需要内存序](#为什么需要内存序)
- [六种内存序速览](#六种内存序速览)
- [三种核心内存序详解](#三种核心内存序详解)
  - [memory_order_relaxed](#1-memory_order_relaxed)
  - [memory_order_release](#2-memory_order_release)
  - [memory_order_acquire](#3-memory_order_acquire)
  - [release-acquire 配对](#4-release--acquire-配对同步原语)
- [memory_order_seq_cst（默认值）](#5-memory_order_seq_cst默认值)
- [常见使用模式](#常见使用模式)
  - [单生产者-单消费者标志位](#模式一单生产者-单消费者标志位)
  - [引用计数](#模式二引用计数)
  - [Treiber 无锁栈](#模式三treiber-无锁栈)
- [本项目中的应用](#本项目中的应用)
- [选择内存序的原则](#选择内存序的原则)
- [常见错误](#常见错误)

---

## 为什么需要内存序

现代 CPU 和编译器都会对指令进行**重排序**以提升性能。例如：

```cpp
// 线程 A
data = 42;          // ①
ready.store(true);  // ②
```

编译器或 CPU 可能将 ② 排在 ① 前面执行。此时线程 B 看到 `ready == true` 时，`data` 可能还是 0。

```cpp
// 线程 B
while (!ready.load()) {}
use(data);  // 可能读到 0！
```

**内存序（memory order）** 是程序员告诉编译器和 CPU"哪些重排序是允许的"的机制。
不同的内存序对应不同的约束强度，越强越安全，但性能开销也越大。

---

## 六种内存序速览

| 内存序    | 强度         | 常见用途                                                     |
| --------- | ------------ | ------------------------------------------------------------ |
| `relaxed` | 最弱         | 只保证原子性，无顺序约束；适合计数器、统计                   |
| `consume` | 弱（已废弃） | 数据依赖顺序，实际实现等同 acquire，不建议使用               |
| `acquire` | 中           | **读**操作，与 release 配对；确保后续读写不被提前            |
| `release` | 中           | **写**操作，与 acquire 配对；确保之前读写不被延后            |
| `acq_rel` | 中强         | 读-改-写操作（CAS/fetch_add），同时具备 acquire+release 语义 |
| `seq_cst` | 最强         | 全局顺序一致，所有线程看到相同的操作顺序；默认值             |

---

## 三种核心内存序详解

### 1. `memory_order_relaxed`

#### 保证内容

- **原子性**：该操作本身是不可分割的（不会读到中间值）
- **无顺序约束**：编译器和 CPU 可以把该操作与其他操作任意重排

#### 不保证内容

- 不与其他线程的任何操作建立 happens-before 关系
- 其他线程看到此操作的时机不确定

#### 典型代码

```cpp
std::atomic<int> counter{0};

// 多线程安全的计数累加（但最终读取需要更强的序）
void increment() {
    counter.fetch_add(1, std::memory_order_relaxed);
}

// 读取最终结果时，需要等所有线程结束后再读（或用 seq_cst）
int get() {
    return counter.load(std::memory_order_relaxed);
}
```

#### 使用场景

- 统计计数器（只关心最终累计值，不关心中间顺序）
- 原子标志的"读自己之前写入的值"（同一线程内）
- 与 release/acquire 配合时，被保护数据之外的辅助操作

---

### 2. `memory_order_release`

#### 保证内容

**在 release store 之前的所有内存操作，不会被重排到 release store 之后。**

换句话说：release store 就像一道"向上的屏障"——屏障之上的写操作，在屏障执行后对其他线程可见。

```
[写 data]      ← 这些操作一定先于 release store 完成
[写 data2]     ←
release store(flag)  ← 屏障
[后续操作]     ← 这些可能被提前（relaxed），但不影响上面
```

#### 典型代码

```cpp
std::atomic<bool> ready{false};
int data = 0;

// 生产者线程
void producer() {
    data = 42;                              // ① 普通写
    ready.store(true, memory_order_release); // ② release store
    // 保证：① 一定在 ② 之前完成（对消费者可见）
}
```

#### 适用操作

`store`、`exchange`、`fetch_add` 等写类操作。
（纯读操作用 release 无意义，编译器通常会警告或忽略）

---

### 3. `memory_order_acquire`

#### 保证内容

**在 acquire load 之后的所有内存操作，不会被重排到 acquire load 之前。**

acquire load 就像一道"向下的屏障"——屏障之下的读写操作，一定发生在屏障之后。

```
[之前操作]     ← 这些可能被延后（relaxed），但不影响下面
acquire load(flag)   ← 屏障
[读 data]      ← 这些操作一定在 acquire load 之后
[读 data2]     ←
```

#### 典型代码

```cpp
// 消费者线程（与上面的 producer 配对）
void consumer() {
    while (!ready.load(memory_order_acquire)) {} // acquire load
    // 保证：下面读到的 data 一定是 producer 写入后的值
    assert(data == 42);  // ✅ 安全
}
```

#### 适用操作

`load`、`exchange`、`compare_exchange_*` 等含读语义的操作。

---

### 4. release + acquire 配对：同步原语

release 和 acquire 必须**配对使用**才能建立 happens-before 关系：

```
线程 A                          线程 B
──────────────────────          ──────────────────────
data = 42;          ─┐          while (!flag.load(     ─┐
flag.store(true,     │            memory_order_acquire))  │ happens-before
  memory_order_      │          {}                        │
  release);         ─┘          assert(data == 42);   ─┘
```

**规则**：若线程 B 的 acquire load 读到了线程 A 的 release store 写入的值，
则线程 A 在 release store 之前的所有操作，对线程 B 在 acquire load 之后的操作均可见。

> ⚠️ 只有"B 的 acquire load 读到了 A 的 release store 写的那个值"这一条件满足时，
> happens-before 才成立。若 B 读到的是更早的值（如初始值），则不建立同步关系。

#### 性能对比（x86_64）

在 x86 架构上，release store 和 acquire load 编译为普通的 `MOV` 指令（x86 TSO 模型天然保证了大部分顺序），性能与 relaxed 几乎相同。在 ARM/POWER 等弱序架构上，才需要插入实际的内存屏障指令（`dmb`、`lwsync` 等）。

---

### 5. `memory_order_seq_cst`（默认值）

#### 保证内容

在 release/acquire 的基础上，额外保证：**所有标注 seq_cst 的操作，在所有线程中都看到相同的全局顺序。**

#### 何时必须使用 seq_cst

release/acquire 只能在**一对**生产者-消费者之间建立顺序。当需要多个原子变量之间的全局顺序时，必须用 seq_cst：

```cpp
// 经典例子：Peterson 互斥算法
std::atomic<bool> want0{false}, want1{false};

// 线程 0
want0.store(true, seq_cst);   // ①
if (!want1.load(seq_cst)) {   // ②
    // 进入临界区
}

// 线程 1
want1.store(true, seq_cst);   // ③
if (!want0.load(seq_cst)) {   // ④
    // 进入临界区
}
// 若 ① 和 ③ 用 release，② 和 ④ 用 acquire，
// 则两个线程可能都看到对方的 want 为 false → 同时进入临界区！
// seq_cst 保证 ①③ 有全局顺序，避免此问题。
```

#### 性能

x86：seq_cst store 需要 `MFENCE` 或 `LOCK XCHG`，比 release store 慢约 **5–10×**。
ARM：需要 `dmb ish`，开销也显著高于 acquire/release。

---

## 常见使用模式

### 模式一：单生产者-单消费者标志位

```cpp
std::atomic<bool> ready{false};
int shared_data = 0;

// 生产者
shared_data = 100;
ready.store(true, std::memory_order_release);

// 消费者
while (!ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
}
// 此处可以安全读取 shared_data == 100
```

### 模式二：引用计数

```cpp
std::atomic<int> ref_count{1};

void add_ref() {
    // 只需原子性，无顺序要求
    ref_count.fetch_add(1, std::memory_order_relaxed);
}

void release() {
    // release：确保本线程对对象的所有修改，对最后一个调用 release() 的线程可见
    if (ref_count.fetch_sub(1, std::memory_order_release) == 1) {
        // acquire：确保能看到所有 release 之前的写操作（其他线程的修改）
        std::atomic_thread_fence(std::memory_order_acquire);
        delete this;
    }
}
```

### 模式三：Treiber 无锁栈

```cpp
struct Node { int val; Node* next; };
std::atomic<Node*> top{nullptr};

void push(int val) {
    Node* node = new Node{val};
    node->next = top.load(std::memory_order_relaxed);
    // acq_rel：读旧 top（acquire）+ 写新 top（release）
    while (!top.compare_exchange_weak(
        node->next, node,
        std::memory_order_release,   // 成功时：release，发布新节点
        std::memory_order_relaxed))  // 失败时：只需重读 top，relaxed 即可
    {}
}

Node* pop() {
    Node* node = top.load(std::memory_order_acquire); // acquire：能看到 push 写的数据
    while (node && !top.compare_exchange_weak(
        node, node->next,
        std::memory_order_acquire,   // 成功时：acquire，能安全读 node->val
        std::memory_order_acquire))  // 失败时：仍需 acquire 刷新 node
    {}
    return node;
}
```

---

## 本项目中的应用

### SPSCRingBuffer

```cpp
// push（生产者）
buffer_[wp & MASK] = item;                    // 普通写：数据写入
prod_.pos.store(wp + 1, memory_order_release); // release：发布"槽已填充"

// pop（消费者）
cached_write = prod_.pos.load(memory_order_acquire); // acquire：读到 release 发布的 pos
item = buffer_[rp & MASK];                            // 普通读：此时数据已可见
cons_.pos.store(rp + 1, memory_order_release);        // release：发布"槽已消费"
```

```
生产者写 buffer[wp]  ──┐
                        │  release store(prod_.pos)
                        └──────────────────────────→  acquire load(prod_.pos)
                                                       读 buffer[rp]  ← 可见 ✅
```

缓存对端位置时使用 `relaxed`：
```cpp
// 生产者私有缓存，只有自己读写，relaxed 足够
cached_read = cons_.pos.load(memory_order_acquire); // 此处用 acquire 而非 relaxed
// 注意：这里必须用 acquire，因为需要与消费者的 release store 配对
// cached_read 只是一个优化：避免每次 push 都做 acquire load
```

### SPSCLinkedQueue

```cpp
// push：发布链表节点
node->next.store(nullptr, memory_order_relaxed); // relaxed：无人观察这个 nullptr
tail->next.store(node, memory_order_release);    // release：发布新节点（含 data）

// pop：读取链表节点
next = head->next.load(memory_order_acquire);    // acquire：与 push release 配对
item = next->data;                               // 普通读：data 此时已可见

// return_pub 归还通道
return_pub.store(local_ret, memory_order_release);  // release：发布归还链
return_pub.exchange(nullptr, memory_order_acquire); // acquire：取走归还链
```

### MPMCRingBuffer

```cpp
// 生产者写入数据，发布序列号
cell->data = data;
cell->sequence.store(pos + 1, memory_order_release); // release

// 消费者读取数据，通过序列号同步
seq = cell->sequence.load(memory_order_acquire);     // acquire
data = cell->data;                                   // 普通读，安全
cell->sequence.store(pos + capacity, memory_order_release); // release 归还槽
```

---

## 选择内存序的原则

```
需要跨线程传递数据（"A 写完，B 能读到"）？
  ├─ 是 → 写端用 release，读端用 acquire
  │         两者操作的是同一个原子变量
  │
  └─ 否，只需要原子性（如计数器）？
        └─ 用 relaxed
           （如果 relaxed 的读取结果会用于"是否继续"判断，
             可能需要 acquire 防止 load 被重排到后续读操作之前）

需要多个原子变量之间保证全局顺序？
  └─ 用 seq_cst（代价高，能用 release/acquire 解决的尽量不用）

CAS / fetch_add / exchange 等读-改-写操作？
  ├─ 操作结果需要发布数据 → 成功时用 release（或 acq_rel）
  └─ 只是重试循环，不发布数据 → 失败时用 relaxed
```

---

## 常见错误

### ❌ 错误 1：release 写，relaxed 读

```cpp
// 生产者
data = 42;
flag.store(true, memory_order_release);

// 消费者（错误！）
while (!flag.load(memory_order_relaxed)) {} // relaxed 不与 release 配对
assert(data == 42);  // ❌ 未定义行为，可能读到旧值
```

**修正**：消费者改用 `memory_order_acquire`。

---

### ❌ 错误 2：两个原子变量，只用 release/acquire

```cpp
std::atomic<bool> a{false}, b{false};

// 线程 1
a.store(true, release);

// 线程 2
b.store(true, release);

// 线程 3
if (a.load(acquire) && b.load(acquire)) {
    // ❌ 不能保证线程 3 同时看到两者为 true
    // release/acquire 只建立 1-to-1 的配对同步，无法保证全局一致性
}
```

**修正**：改用 `seq_cst`，或重新设计为单原子变量控制。

---

### ❌ 错误 3：循环中 relaxed load 导致死循环（在 ARM 上）

```cpp
// 消费者（在 ARM 等弱序架构上可能死循环）
while (!flag.load(memory_order_relaxed)) {}
// relaxed load 可能被 CPU 缓存，永远看不到最新值
```

**修正**：改用 `memory_order_acquire`（或至少 `memory_order_consume`）。

---

### ❌ 错误 4：对同一原子变量，release store 之后再 relaxed store

```cpp
flag.store(true, memory_order_release);   // 发布数据
flag.store(false, memory_order_relaxed);  // ❌ 覆盖了 release，但无 release 语义
// 消费者可能看到 false，而 data 是未初始化的旧值
```

**规则**：如果一个原子变量承担"发布"职责，对它的所有写操作都应保持 release 语义。
