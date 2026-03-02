# 无锁数据结构：实现原理、测试方法与性能结果

## 目录

- [项目结构](#项目结构)
- [环境与构建](#环境与构建)
- [数据结构实现原理](#数据结构实现原理)
  - [1. SPSCRingBuffer](#1-spscringbuffer)
  - [2. SPSCLinkedQueue](#2-spsclinkedqueue)
  - [3. MPMCRingBuffer](#3-mpmcringbuffer)
  - [4. MSQueue](#4-msqueue)
  - [5. MutexRingBuffer / MutexQueue（对照组）](#5-mutexringbuffer--mutexqueue对照组)
- [测试方法](#测试方法)
  - [正确性测试](#正确性测试)
  - [性能测试](#性能测试)
- [性能测试结果](#性能测试结果)
- [综合结论](#综合结论)
- [选型指南](#选型指南)

---

## 项目结构

```
lock_free_datastructure/
├── include/
│   ├── spsc_ring_buffer.h      # SPSC 无锁环形缓冲区
│   ├── spsc_linked_queue.h     # SPSC 无锁链表队列
│   ├── mpmc_ring_buffer.h      # MPMC 无锁环形缓冲区（Vyukov 算法）
│   ├── ms_queue.h              # Michael-Scott MPMC 无锁链表队列
│   ├── mutex_containers.h      # Mutex 队列/环形缓冲（对照组）
│   └── benchmark_utils.h       # 计时器、格式化输出工具
├── tests/
│   ├── correctness_test.cpp    # 正确性验证（Google Test，13 项）
│   └── performance_test.cpp    # 四场景性能对比
├── doc/
│   └── README.md               # 本文档
├── CMakeLists.txt
└── build.sh
```

---

## 环境与构建

| 项目       | 版本             |
| ---------- | ---------------- |
| 编译器     | GCC 11.4（g++）  |
| C++ 标准   | C++17            |
| 构建系统   | CMake 3.14+      |
| 测试框架   | Google Test 1.11 |
| 操作系统   | Ubuntu 22.04     |
| CPU 核心数 | 12               |

```bash
# Release 构建（默认）
bash build.sh --release

# 运行正确性测试
bash build.sh --release --test

# 运行性能测试
bash build.sh --release --perf

# Debug 模式（开启 ThreadSanitizer 检测数据竞争）
bash build.sh --debug --test
```

---

## 数据结构实现原理

### 1. SPSCRingBuffer

**文件**：`include/spsc_ring_buffer.h`
**适用场景**：严格 1 个生产者线程 + 1 个消费者线程
**有界/无界**：有界（`Capacity` 必须是 2 的幂）

#### 核心数据布局

```
ProducerSide  (alignas(64)，独占一条 cache line)
  ├── pos          atomic<size_t>   生产者写入，消费者 acquire-load
  └── cached_read  size_t           生产者私有缓存，减少跨线程 atomic load

ConsumerSide  (alignas(64)，独占一条 cache line)
  ├── pos          atomic<size_t>   消费者写入，生产者 acquire-load
  └── cached_write size_t           消费者私有缓存

buffer_[Capacity]  (独立 cache line 对齐的数组)
```

#### 算法步骤

**push（仅生产者调用）**

```
1. wp = prod_.pos.load(relaxed)
2. if wp - cached_read >= Capacity:
       cached_read = cons_.pos.load(acquire)  // 仅"可能满"时才做跨线程 acquire
       if 仍满 → return false
3. buffer_[wp & MASK] = item
4. prod_.pos.store(wp+1, release)             // release 确保 buffer 写先于 pos 更新可见
```

**pop（仅消费者调用）**

```
1. rp = cons_.pos.load(relaxed)
2. if rp == cached_write:
       cached_write = prod_.pos.load(acquire)
       if 仍空 → return false
3. item = buffer_[rp & MASK]
4. cons_.pos.store(rp+1, release)
```

#### 关键优化

| 优化技术            | 说明                                                                                                        |
| ------------------- | ----------------------------------------------------------------------------------------------------------- |
| **Cache line 隔离** | `ProducerSide`/`ConsumerSide` 各自 `alignas(64)`，消除 false sharing                                        |
| **缓存对端指针**    | `cached_read`/`cached_write` 是普通变量，仅在必要时才触发跨线程 acquire load，大幅减少 cache coherence 流量 |
| **无 CAS**          | push/pop 均只用 `store(release)` + `load(acquire)`，无原子 RMW 指令                                         |
| **位掩码索引**      | `pos & MASK` 替代取模，零开销                                                                               |

#### 限制

- **严格 1P+1C**：`push` 对 `pos` 直接 `store`，多生产者同时读到相同 `wp` 时会写同一槽位，产生数据竞争
- `Capacity` 必须为 2 的幂

---

### 2. SPSCLinkedQueue

**文件**：`include/spsc_linked_queue.h`
**适用场景**：严格 1P+1C，但需要**无界**（不能因容量满而丢失数据）
**有界/无界**：逻辑无界（节点来自预分配池，可复用）

#### 与 SPSCRingBuffer 的根本区别

|            | SPSCRingBuffer  | SPSCLinkedQueue   |
| ---------- | --------------- | ----------------- |
| 存储方式   | 连续数组        | 链表节点          |
| 容量       | 固定（2 的幂）  | 逻辑无界          |
| Cache 友好 | ✅ 极佳          | ❌ pointer chasing |
| 节点分配   | 无需            | 三层节点池管理    |
| 队满行为   | push 返回 false | 不会队满          |

#### 算法：Dummy Node 单链表

```
head → [dummy] → [node1] → [node2] → ... → [nodeN] ← tail
```

初始状态：`head = tail = pool_[0]`（dummy 节点）

**push（仅生产者，无 CAS）**

```
1. node = alloc_node()
2. node->data = item
3. node->next.store(nullptr, relaxed)
4. tail->next.store(node, release)    // release 确保 data 先于指针可见
5. tail = node
```

**pop（仅消费者，无 CAS）**

```
1. next = head->next.load(acquire)    // 与 push release 配对
2. if next == nullptr → return false
3. item = next->data
4. return_node(head)                  // 归还旧 dummy
5. head = next                        // next 成为新 dummy
```

#### 节点内存管理（三层，零堆分配）

```
Producer 侧（无竞争）                  Consumer 侧（无竞争写）
────────────────────                   ────────────────────────
prod_.free_head                        cons_.local_ret   ← 纯私有链，非 atomic
  （私有空闲链表，非 atomic）              （积累待归还节点）
prod_.alloc_idx                        cons_.return_pub  ← 单向共享通道（atomic）
  （线性分配游标）                          （整批发布给 producer）
```

**分配优先级（仅生产者）**：
1. `free_head` 私有链（零原子操作，最快）
2. `return_pub.exchange(nullptr, acquire)` 批量取回消费者归还的节点
3. `pool_[alloc_idx++]` 线性分配

**归还协议（仅消费者）**：

```cpp
void return_node(Node* node) {
    node->next = local_ret;            // 前插私有链，无原子操作
    local_ret = node;
    if (!return_pub.load(relaxed)) {   // 通道空时才整批发布
        return_pub.store(local_ret, release);
        local_ret = nullptr;
    }
}
```

> ⚠️ **关键设计**：`return_node` 绝不读 `return_pub` 的旧值来拼链。
> 若先 `load(return_pub)` 取得 `chain_X`，再令 `node->next = chain_X`，
> producer 可能已经 `exchange` 走了 `chain_X` 并将其重新入队，
> 导致同一节点同时存在于队列和归还链（double-use）——链表断裂，消费者死循环。

---

### 3. MPMCRingBuffer

**文件**：`include/mpmc_ring_buffer.h`
**适用场景**：任意数量生产者 + 消费者线程
**有界/无界**：有界（`Capacity` 必须是 2 的幂）
**算法来源**：Dmitry Vyukov，[1024cores.net](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue)

#### 核心数据布局

```
buffer_[Capacity]：每个 Slot 独占一条 64B cache line
  Slot {
    atomic<size_t> sequence   // 编码 slot 状态的序列号
    T data
  }

enqueue_pos_  atomic<size_t>  （独立 cache line）
dequeue_pos_  atomic<size_t>  （独立 cache line）
```

初始化：`slot[i].seq = i`

#### 序列号状态机

| `seq - pos` 差值     | 含义                              |
| -------------------- | --------------------------------- |
| `== 0`               | slot 空闲，当前生产者可写入       |
| `== 1`（消费者视角） | slot 已填充，当前消费者可读取     |
| `< 0`                | 队满（生产者）或队空（消费者）    |
| `> 0`（且 ≠ 期望值） | 本线程 pos 落后，重新 load 后重试 |

**push 成功后**：`slot.seq = pos + 1`（发布数据给消费者）
**pop 成功后**：`slot.seq = pos + Capacity`（归还槽位给下一轮生产者）

#### 关键性质

- 每个 slot 独占 cache line，多线程写不同 slot 时无 false sharing
- CAS 只在 `enqueue_pos_`/`dequeue_pos_` 上竞争，slot 数据写入是单线程独占的
- 序列号单调递增，天然避免 ABA 问题

---

### 4. MSQueue

**文件**：`include/ms_queue.h`
**适用场景**：MPMC，无界
**有界/无界**：逻辑无界（受 `PoolSize` 限制，节点可复用）
**算法来源**：Michael & Scott，1996，《Simple, Fast, and Practical Non-Blocking and Blocking Concurrent Queue Algorithms》

#### 结构

```
head  atomic<uint64_t>  —— 始终指向 dummy 节点
tail  atomic<uint64_t>  —— 始终指向最后一个节点

Node {
    T data
    atomic<uint64_t> next   // 带版本计数的标记指针
}
```

#### ABA 问题与标记指针

原始 MS 算法用裸指针会遭遇 ABA 问题（指针值相同但中间状态已变化，CAS 误判通过）。
本实现将指针打包为 `uint64_t`：

```
bits [31: 0] = 节点池索引
bits [63:32] = 版本计数器（每次 CAS 成功 +1）
```

版本号随每次成功 CAS 递增，使"相同池索引 + 不同版本"可被区分，彻底消除 ABA。

#### 节点池

- 预分配 `PoolSize` 个节点，`pool_[0]` 作为初始 dummy
- **分配**：先从 Treiber 自由链表（`free_head_`）弹出，否则原子计数器线性分配
- **回收**：`pop` 将旧 dummy 归还到 Treiber 自由链表

#### 算法步骤（简化）

**push**

```
loop:
  读 tail 及 tail->next
  if tail->next 为空:
      CAS(tail->next, null, new_node) 成功 → 再 CAS 推进 tail → return
  else:
      帮助推进落后的 tail（另一线程 push 后未及时更新的情况）
```

**pop**

```
loop:
  读 head, tail, head->next
  if 队列为空（head == tail 且 head->next 为空） → return false
  if tail 落后（head == tail 但 head->next 非空） → 帮助推进 tail
  else:
      读 head->next.data
      CAS(head, head, head->next) 成功 → 归还旧 dummy → return true
```

#### MPMC 链表 vs MPMC 环形缓冲

|            | MSQueue                    | MPMCRingBuffer                          |
| ---------- | -------------------------- | --------------------------------------- |
| CAS 热点   | `head`/`tail` 两个全局指针 | `enqueue_pos_`/`dequeue_pos_` + slot 级 |
| 内存局部性 | 差（pointer chasing）      | 好（数组，顺序访问）                    |
| 无界性     | ✅                          | ❌                                       |
| 高竞争吞吐 | 明显劣于环形缓冲           | 较好                                    |

---

### 5. MutexRingBuffer / MutexQueue（对照组）

**文件**：`include/mutex_containers.h`

两者均用 `std::mutex` + `std::lock_guard` 保护所有操作，是标准教科书实现：

```cpp
// MutexQueue（无界，std::deque 底层）
bool push(T item) {
    std::lock_guard<std::mutex> lk(mutex_);
    queue_.push_back(std::move(item));
    return true;
}

// MutexRingBuffer（有界，数组）
bool push(T item) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (size_ == Capacity) return false;
    buffer_[tail_] = std::move(item);
    tail_ = (tail_ + 1) % Capacity;
    ++size_;
    return true;
}
```

Mutex 主要开销来源：

| 场景                               | 典型延迟    |
| ---------------------------------- | ----------- |
| 无竞争（用户态 futex 快路径）      | ~20–40 ns   |
| 有竞争（线程挂起 + 唤醒，syscall） | ~200–300 ns |

---

## 测试方法

### 正确性测试

**文件**：`tests/correctness_test.cpp`
**框架**：Google Test
**共 13 个测试用例**

| 测试套件          | 测试用例                      | 验证内容                                              |
| ----------------- | ----------------------------- | ----------------------------------------------------- |
| `SPSCRingBuffer`  | `BasicPushPop`                | FIFO 顺序、空队列 pop 返回 false                      |
| `SPSCRingBuffer`  | `BoundaryFull`                | 满时 push 返回 false、弹出后可继续写入                |
| `SPSCRingBuffer`  | `MultiThread_DataIntegrity`   | 1P+1C，100 万次，校验 `sum_produced == sum_consumed`  |
| `MPMCRingBuffer`  | `BasicPushPop`                | FIFO 顺序、满时拒绝                                   |
| `MPMCRingBuffer`  | `MultiThread_4P4C`            | 4P+4C，100 万次，校验 sum 总和                        |
| `MPMCRingBuffer`  | `MultiThread_DataNoDuplicate` | 4P+4C，每个值只被消费一次（无重复/无丢失）            |
| `MSQueue`         | `BasicPushPop`                | FIFO 顺序、空队列行为                                 |
| `MSQueue`         | `MultiThread_4P4C`            | 4P+4C，20 万次，校验 sum 总和                         |
| `MutexQueue`      | `BasicPushPop`                | FIFO 基本行为                                         |
| `SPSCLinkedQueue` | `BasicPushPop`                | FIFO 顺序、`empty()` 语义                             |
| `SPSCLinkedQueue` | `NodeReuse`                   | 16 节点小池完成 10000 次 push/pop，验证节点复用不崩溃 |
| `SPSCLinkedQueue` | `MultiThread_DataIntegrity`   | 1P+1C，100 万次，校验 sum 及等差数列完整性            |
| `MutexRingBuffer` | `BasicPushPop`                | FIFO 顺序、满时拒绝                                   |

**运行**：

```bash
./build/correctness_test --gtest_color=yes
# [  PASSED  ] 13 tests.
```

---

### 性能测试

**文件**：`tests/performance_test.cpp`

#### 场景 A：单线程吞吐量

- **目的**：测量数据结构的原始内存访问开销上限，无任何线程竞争干扰
- **方法**：同一线程交替 `push(i)` / `pop(v)`，共 1000 万次
- **意义**：反映数据结构在最理想条件下的速度上界

#### 场景 B：SPSC 吞吐量（1 生产者 + 1 消费者）

- **目的**：测量最常见管道模式下的跨线程传输速率
- **方法**：生产者自旋 push，消费者自旋 pop，共 1000 万次；用 `StartLatch` 保证两线程同时开始

#### 场景 C：MPMC 吞吐量（N=2/4/8 生产者 + N 消费者）

- **目的**：测量高竞争下多线程吞吐的可扩展性
- **方法**：N 个生产者各负责 `total_ops/N` 次 push，N 个消费者竞争消费，共 500 万次
- **注意**：SPSC 类结构不参与此场景（算法约束，多线程调用会产生数据竞争）

#### 场景 D：Ping-Pong 延迟

- **目的**：测量单次跨线程传递的端到端延迟
- **方法**：

```
Pinger：push(i) → q1，阻塞等待 pop(v) ← q2
Ponger：pop(v) ← q1，立即 push(v) → q2

单程延迟 = 总耗时 / (2 × rounds)
```

- **意义**：与吞吐量（流水线批量）不同，Ping-Pong 是严格串行往返，
  暴露 CPU cache coherence 协议的核间传播延迟（~30–60 ns）加各数据结构的额外开销

---

## 性能测试结果

**测试环境**：Ubuntu 22.04，12 核 CPU，GCC 11.4，`-O3 -march=native`

### 场景 A：单线程吞吐量（10M ops）

| 数据结构            | 吞吐（Mops/s） | 延迟（ns/op） |
| ------------------- | -------------- | ------------- |
| **SPSC_RingBuffer** | **993**        | **1.01**      |
| SPSC_LinkedQueue    | 438            | 2.28          |
| MPMC_RingBuffer     | 197            | 5.09          |
| MS_Queue            | 110            | 9.07          |
| Mutex_RingBuffer    | 90             | 11.11         |
| Mutex_Queue         | 88             | 11.39         |

**分析**：
- `SPSC_RingBuffer` 约 1 ns/op，接近 L1 cache 存取理论极限
- `SPSC_LinkedQueue` 慢约 2.3×，来自节点分配/归还时的 `exchange` 原子操作
- `MPMC_RingBuffer` 慢约 5×，序列号方案含额外 CAS
- Mutex 系列无竞争约 11 ns，锁操作本身占约 10 ns

### 场景 B：SPSC 吞吐量（1P+1C，10M ops）

| 数据结构            | 吞吐（Mops/s） | 延迟（ns/op） |
| ------------------- | -------------- | ------------- |
| **SPSC_RingBuffer** | **367**        | **2.72**      |
| MPMC_RingBuffer     | 162            | 6.17          |
| SPSC_LinkedQueue    | 70             | 14.27         |
| Mutex_Queue         | 23             | 43.47         |
| Mutex_RingBuffer    | 18             | 54.21         |
| MS_Queue            | 9              | 111.72        |

**分析**：
- `SPSC_RingBuffer` 比 Mutex 快 **16–20×**，完全无 CAS 是关键
- `SPSC_LinkedQueue` 慢于 `SPSC_RingBuffer` 约 5×：链表 pointer chasing 引发 cache miss；`return_pub` 通道的 acquire/release 产生核间 cache coherence 流量
- `MS_Queue` 在 SPSC 场景最慢（9 Mops），`head`/`tail` 两个原子 CAS 在两线程间形成严重 cache line 争用

### 场景 C：MPMC 吞吐量（5M ops）

**4P+4C（8 线程）**

| 数据结构            | 吞吐（Mops/s） | 延迟（ns/op） |
| ------------------- | -------------- | ------------- |
| **MPMC_RingBuffer** | **11.7**       | **85.7**      |
| Mutex_RingBuffer    | 10.4           | 96.2          |
| Mutex_Queue         | 10.0           | 100.3         |
| MS_Queue            | 3.9            | 253.0         |

**8P+8C（16 线程）**

| 数据结构            | 吞吐（Mops/s） | 延迟（ns/op） |
| ------------------- | -------------- | ------------- |
| **MPMC_RingBuffer** | **10.6**       | **94.1**      |
| Mutex_RingBuffer    | 8.7            | 114.6         |
| Mutex_Queue         | 8.5            | 117.5         |
| MS_Queue            | 3.4            | 290.8         |

**分析**：
- 低竞争（2P+2C）时，Mutex 吞吐有时反而高于 `MPMC_RingBuffer`：无锁自旋持续占用总线带宽（频繁 cache invalidation），Mutex 让等待线程睡眠释放资源
- 随线程数增加，`MPMC_RingBuffer` 吞吐下降平缓（~10 Mops），扩展性最好
- `MS_Queue` 在所有 MPMC 场景均最慢，`head`/`tail` 是不可分散的全局热点 CAS

### 场景 D：Ping-Pong 延迟（500K rounds）

| 数据结构            | 单程延迟（ns） |
| ------------------- | -------------- |
| **MPMC_RingBuffer** | **46.0**       |
| **SPSC_RingBuffer** | **56.5**       |
| SPSC_LinkedQueue    | 78.2           |
| MS_Queue            | 108.0          |
| Mutex_Queue         | 289.0          |
| Mutex_RingBuffer    | 295.8          |

**分析**：
- `MPMC_RingBuffer` 延迟最低（46 ns），序列号方案将 slot 数据与控制变量（`pos`）分离，减少核间 cache line 抢占
- Mutex 系列约 290 ns，其中约 **200 ns 来自 futex 系统调用**（线程挂起/唤醒），是无锁结构的 5–6 倍
- `SPSC_LinkedQueue` 比 `SPSC_RingBuffer` 慢约 38%，来自链表节点的间接内存访问

---

## 综合结论

### 1. 无锁 ≠ 无条件更快

高竞争（多线程争用同一数据结构）时，无锁自旋持续占用 CPU 总线，产生大量 cache invalidation。此时 Mutex 让等待线程睡眠释放资源，吞吐可与无锁持平甚至反超（2P+2C 时 Mutex ≈ MPMC）。

### 2. SPSC 是无锁最优场景

`SPSC_RingBuffer` 在 SPSC 场景下比 Mutex 快 **16–20×**，完全依靠 `store(release)` + `load(acquire)` 实现跨线程同步，没有任何 CAS。

### 3. 链表队列的代价

链表结构（`SPSCLinkedQueue`、`MSQueue`）比数组结构（`SPSCRingBuffer`、`MPMCRingBuffer`）慢 2–10×，来源：
- **Pointer chasing**：节点地址不连续，频繁引发 L1/L2 cache miss
- **节点管理**：即使是无动态堆分配的池方案，也需要原子操作维护归还通道

### 4. 延迟 vs 吞吐

- 吞吐（场景 A/B/C）反映流水线峰值速率，受益于批量化和 CPU 预取
- 延迟（场景 D）暴露单次操作的真实成本，Mutex 因系统调用固定损耗约 200 ns，无法规避

---

## 选型指南

| 使用场景                                 | 推荐结构            | 理由                                  |
| ---------------------------------------- | ------------------- | ------------------------------------- |
| 严格 1P+1C，对延迟/吞吐极度敏感          | **SPSCRingBuffer**  | 最快，无 CAS，Ping-Pong 延迟 ~57 ns   |
| 严格 1P+1C，但队列深度不可预知（需无界） | **SPSCLinkedQueue** | 逻辑无界，节点复用，比 Mutex 快 3–4×  |
| 多生产者 + 多消费者，追求低延迟          | **MPMCRingBuffer**  | Ping-Pong 延迟最低（46 ns），扩展性好 |
| 多生产者 + 多消费者，代码简单优先        | **MutexQueue**      | 高竞争下与 MPMC 吞吐持平，实现最简洁  |
| 需要 MPMC + 无界 + 强无锁保证            | **MSQueue**         | 学术价值高，实测吞吐不如上述方案      |
