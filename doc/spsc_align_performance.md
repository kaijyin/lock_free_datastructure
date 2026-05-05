# SPSC `alignas(64)` 性能测试

## 目标

本文档记录 [tests/spsc_align_performance_test.cpp](../tests/spsc_align_performance_test.cpp) 的实测结果，用来观察 `alignas(64)` 对 SPSC 无锁环形队列的性能影响。

测试重点不是比较不同算法，而是比较**同一套 SPSC 算法**在不同内存布局下的差异：

1. `SPSC full alignas(64)`
   - producer 控制块 `alignas(64)`
   - consumer 控制块 `alignas(64)`
   - buffer 也 `alignas(64)`
2. `SPSC control align only`
   - 只有 producer / consumer 控制块 `alignas(64)`
   - buffer 不额外对齐
3. `SPSC no alignas`
   - 完全不做 `alignas(64)`

## 相关文件

- 原始 SPSC 实现： [include/spsc_ring_buffer.h](../include/spsc_ring_buffer.h)
- 性能测试： [tests/spsc_align_performance_test.cpp](../tests/spsc_align_performance_test.cpp)
- 构建配置： [CMakeLists.txt](../CMakeLists.txt)

## 构建与运行

在仓库根目录执行：

```bash
cmake -S . -B build
cmake --build build --target spsc_align_performance_test -j

# 绑核测试
./build/spsc_align_performance_test

# 不绑核测试
./build/spsc_align_performance_test --no-bind
```

说明：

- 该目标单独使用 `-O3` 编译。
- 默认会把 producer 绑定到 core 0，consumer 绑定到 core 1。
- 使用 `--no-bind` 时，线程交由系统调度。

## 参数

- `--ops N`：每轮 SPSC push/pop 对数，默认 `20000000`
- `--rounds N`：重复轮数，默认 `5`
- `--no-bind`：关闭绑核

## 测试环境

- CPU：AMD Ryzen 5 4500U
- 核心 / 线程：6C / 6T
- 操作系统：Linux
- 编译优化：`-O3`
- 队列容量：`65536`
- 默认测试参数：`total_ops=20000000`，`rounds=5`

## 重新测试结果

### 1. 绑核模式

命令：

```bash
./build/spsc_align_performance_test
```

| 版本                    | 吞吐量 (Mops) | 延迟 (ns/op) | 平均耗时 (ms) | 相对 `no alignas` |
| ----------------------- | ------------: | -----------: | ------------: | ----------------: |
| SPSC full alignas(64)   |         69.12 |        14.47 |        289.36 |             1.20x |
| SPSC control align only |         70.78 |        14.13 |        282.58 |             1.23x |
| SPSC no alignas         |         57.58 |        17.37 |        347.33 |             1.00x |

### 2. 不绑核模式

命令：

```bash
./build/spsc_align_performance_test --no-bind
```

| 版本                    | 吞吐量 (Mops) | 延迟 (ns/op) | 平均耗时 (ms) | 相对 `no alignas` |
| ----------------------- | ------------: | -----------: | ------------: | ----------------: |
| SPSC full alignas(64)   |         13.65 |        73.24 |       1464.80 |             0.59x |
| SPSC control align only |         12.76 |        78.39 |       1567.72 |             0.55x |
| SPSC no alignas         |         23.14 |        43.22 |        864.32 |             1.00x |

## 结果分析

### 1. 绑核时，控制块分离带来稳定收益

在绑定到不同核心后：

- `full alignas(64)` 比 `no alignas` 快约 `20%`
- `control align only` 比 `no alignas` 快约 `23%`

这说明主要收益来自：

- producer 的 `pos/cached_read`
- consumer 的 `pos/cached_write`

被放到不同 cache line 后，false sharing 明显减少。

换句话说，SPSC 队列里最敏感的不是 buffer 本身，而是**高频读写的控制状态**。

### 2. 绑核本身就是一个明显优化

从测试结果看，是否绑核对整体吞吐影响非常大：

- 绑核时，三个版本都落在 `57 ~ 71 Mops`
- 不绑核时，三个版本只剩 `12 ~ 23 Mops`

原因不是“绑核自动加速算法”，而是它让测试更接近一个**稳定、可重复的执行环境**：

1. **减少线程迁移**
   - producer 和 consumer 固定在不同核心上运行。
   - 避免线程在多个核心之间来回迁移导致 L1/L2 缓存热数据失效。

2. **减少 cache line ownership 抖动**
   - SPSC 中最热的数据是 `pos`、`cached_read`、`cached_write`。
   - 绑核后，这些 cache line 主要只在两个固定核心之间传递。
   - 不绑核时，cache line 可能因为线程迁移在更多核心之间 bouncing，coherence 成本更高。

3. **减少时间片竞争与调度噪声**
   - 不绑核时，OS 可能暂时把 producer / consumer 放到不理想的位置，甚至让两者经历更频繁的抢占与调度。
   - 这会放大自旋等待开销，显著拉低吞吐。

4. **让 `alignas(64)` 的收益更容易被观测到**
   - 如果调度器噪声太大，false sharing 优化带来的收益会被淹没。
   - 绑核后，测试更像是在测“数据结构设计差异”，而不是在测“调度器今天怎么安排线程”。

所以这里可以把“绑核”本身看作一种**测试条件优化**，也是一种常见的**实际部署优化手段**：

- 对超低延迟 SPSC 管道，固定 producer / consumer 到独立核心通常是合理做法。
- 这能让缓存局部性、同步行为和时延分布更稳定。

### 3. 只对控制块对齐，已经基本拿到主要收益

从绑核结果看：

- `SPSC full alignas(64)`：`69.12 Mops`
- `SPSC control align only`：`70.78 Mops`

两者几乎一样，且 `control align only` 还略快。

这说明：

- 把 buffer 再做 `alignas(64)`，在这个测试里并没有带来额外明显收益
- 关键优化点在控制块分离，而不是 buffer 起始地址本身

### 4. 不绑核时，结果受调度噪声显著影响

不绑核时出现了相反结果：

- 未对齐版本反而最快
- 对齐版本明显变慢

这并不表示 `alignas(64)` 本身有害，而更可能说明：

- 线程迁移
- CPU 频率波动
- 调度器将两个线程放到不同/相同核心上的不确定性
- 缓存热度和抢占

会掩盖甚至反转纯粹的 cache line 优化收益。

因此，如果要研究 `alignas(64)` 对 false sharing 的真实影响，**绑核结果更可信**。

### 5. 这个测试验证了 `alignas(64)` 的核心价值

对这种 SPSC 队列，`alignas(64)` 的价值不是“玄学优化”，而是很具体的：

- 减少 producer / consumer 对同一 cache line 的争用
- 减少 cache coherence 流量
- 让 `memory_order_acquire/release` 的同步开销更少受到伪共享影响

## 结论

在当前机器和 O3 条件下：

1. **如果 SPSC 运行在两个固定核心上，控制块 `alignas(64)` 是有价值的**
   - 实测带来约 `20% ~ 23%` 的性能提升

2. **如果追求稳定低延迟，绑核本身也值得做**
   - 它能显著减少迁核、缓存抖动和调度噪声
   - 在这个测试中，绑核与不绑核之间的总吞吐差距远大于单独的 buffer 对齐差距

3. **对齐收益主要来自控制块分离**
   - buffer 是否额外 `alignas(64)`，影响远小于控制块本身

4. **不绑核时不要轻易根据结果判断对齐是否有效**
   - 调度噪声足以淹没甚至反转 cache line 优化收益

## 建议的后续实验

- 固定更多轮次，例如 `--rounds 10`，减少抖动
- 将 producer / consumer 分别绑定到不同 CCX 或不同 NUMA 节点的机器上测试
- 加入 `perf stat` 观察：
  - `cache-misses`
  - `LLC-load-misses`
  - `context-switches`
- 把当前测试扩展到：
  - 更小容量队列
  - 更大容量队列
  - 不同 payload 大小
