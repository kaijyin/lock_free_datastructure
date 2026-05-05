# 内存池与对象池 O3 性能测试

## 目标

本文档记录 [tests/pool_performance_test.cpp](../tests/pool_performance_test.cpp) 的性能测试结果，重点比较以下四种分配方式在 `-O3` 优化下的单线程吞吐量：

1. `MemoryPool acquire + reset`
2. `operator new/delete`
3. `ObjectPool acquire/release`
4. `new/delete object`

这两个池化方案都属于“提前申请”：

- **内存池**：预先申请一大块连续内存，运行时调用 `acquire()` 线性切分，任务结束后调用 `reset()` 整体复位。
- **对象池**：预先申请固定数量对象槽位，运行时通过 `acquire()` 构造对象，通过 `release()` 归还槽位复用。

## 相关文件

- 实现： [include/memory_pool.h](../include/memory_pool.h)
- 实现： [include/object_pool.h](../include/object_pool.h)
- 正确性测试： [tests/pool_test.cpp](../tests/pool_test.cpp)
- 性能测试： [tests/pool_performance_test.cpp](../tests/pool_performance_test.cpp)
- CMake 目标： [CMakeLists.txt](../CMakeLists.txt)

## 构建与运行

在仓库根目录执行：

```bash
cmake -S . -B build
cmake --build build --target pool_performance_test -j
./build/pool_performance_test
```

说明：

- `pool_performance_test` 在 CMake 中单独设置了 `-O3`。
- 本次测试为**单线程**基准，重点比较分配器/池化机制本身的开销。

## 程序参数

支持参数：

- `--ops N`：总操作次数，默认 `4000000`
- `--batch N`：每批申请数量，默认 `4096`
- `--block-size N`：`MemoryPool` 单次申请字节数，默认 `64`

含义：

- `MemoryPool` 测试里，每次申请 `block-size` 字节，累计一个 batch 后执行一次 `reset()`。
- `ObjectPool` 测试里，每次 `acquire()` 一个 `TestObject`，批量 `release()` 后进入下一轮。

## 测试环境

- CPU：AMD Ryzen 5 4500U
- 核心 / 线程：6C / 6T
- 操作系统：Linux
- 编译优化：`-O3`
- 总操作数：`4,000,000`

## 实测结果

### 汇总表

| 场景                            | MemoryPool 吞吐量 Mops | `operator new/delete` 吞吐量 Mops | MemoryPool 加速比 | ObjectPool 吞吐量 Mops | `new/delete object` 吞吐量 Mops | ObjectPool 加速比 |
| ------------------------------- | ---------------------: | --------------------------------: | ----------------: | ---------------------: | ------------------------------: | ----------------: |
| 默认：`batch=4096, block=64`    |                 898.69 |                             74.27 |            12.10x |                 288.15 |                           64.57 |             4.46x |
| 小块：`batch=4096, block=32`    |                 915.28 |                             75.27 |            12.16x |                 288.40 |                           64.33 |             4.48x |
| 大块：`batch=4096, block=256`   |                 278.10 |                             41.83 |             6.65x |                 287.72 |                           64.81 |             4.44x |
| 小批次：`batch=256, block=64`   |                 327.29 |                             73.65 |             4.44x |                 293.92 |                           65.16 |             4.51x |
| 大批次：`batch=16384, block=64` |                 325.54 |                             64.55 |             5.04x |                 279.87 |                           63.38 |             4.42x |

### 分场景展开

#### 1. 默认参数：`--ops 4000000 --batch 4096 --block-size 64`

| 测试项                     | 吞吐量 (Mops) | 延迟 (ns/op) | 总耗时 (ms) |
| -------------------------- | ------------: | -----------: | ----------: |
| MemoryPool acquire+reset   |        898.69 |         1.11 |        4.45 |
| operator new/delete batch  |         74.27 |        13.46 |       53.86 |
| ObjectPool acquire/release |        288.15 |         3.47 |       13.88 |
| new/delete object batch    |         64.57 |        15.49 |       61.95 |

#### 2. 小块申请：`--ops 4000000 --batch 4096 --block-size 32`

| 测试项                     | 吞吐量 (Mops) | 延迟 (ns/op) | 总耗时 (ms) |
| -------------------------- | ------------: | -----------: | ----------: |
| MemoryPool acquire+reset   |        915.28 |         1.09 |        4.37 |
| operator new/delete batch  |         75.27 |        13.29 |       53.14 |
| ObjectPool acquire/release |        288.40 |         3.47 |       13.87 |
| new/delete object batch    |         64.33 |        15.54 |       62.18 |

#### 3. 大块申请：`--ops 4000000 --batch 4096 --block-size 256`

| 测试项                     | 吞吐量 (Mops) | 延迟 (ns/op) | 总耗时 (ms) |
| -------------------------- | ------------: | -----------: | ----------: |
| MemoryPool acquire+reset   |        278.10 |         3.60 |       14.38 |
| operator new/delete batch  |         41.83 |        23.91 |       95.63 |
| ObjectPool acquire/release |        287.72 |         3.48 |       13.90 |
| new/delete object batch    |         64.81 |        15.43 |       61.72 |

#### 4. 小批次：`--ops 4000000 --batch 256 --block-size 64`

| 测试项                     | 吞吐量 (Mops) | 延迟 (ns/op) | 总耗时 (ms) |
| -------------------------- | ------------: | -----------: | ----------: |
| MemoryPool acquire+reset   |        327.29 |         3.06 |       12.22 |
| operator new/delete batch  |         73.65 |        13.58 |       54.31 |
| ObjectPool acquire/release |        293.92 |         3.40 |       13.61 |
| new/delete object batch    |         65.16 |        15.35 |       61.39 |

#### 5. 大批次：`--ops 4000000 --batch 16384 --block-size 64`

| 测试项                     | 吞吐量 (Mops) | 延迟 (ns/op) | 总耗时 (ms) |
| -------------------------- | ------------: | -----------: | ----------: |
| MemoryPool acquire+reset   |        325.54 |         3.07 |       12.29 |
| operator new/delete batch  |         64.55 |        15.49 |       61.97 |
| ObjectPool acquire/release |        279.87 |         3.57 |       14.29 |
| new/delete object batch    |         63.38 |        15.78 |       63.11 |

## 结果分析

### 1. MemoryPool 的优势最大

在默认参数下：

- `MemoryPool`：`898.69 Mops`
- `operator new/delete`：`74.27 Mops`

即 `MemoryPool` 大约快 **12.10 倍**。

原因：

- 不需要频繁调用通用分配器
- `acquire()` 本质上只是线性推进 offset
- 一批任务完成后只需一次 `reset()`，几乎没有回收成本

这类模式非常适合：

- 临时 scratch buffer
- request / frame 级别分配
- 批处理任务中的生命周期一致对象

### 2. ObjectPool 的提升稳定

在几组参数下，`ObjectPool` 相对 `new/delete object` 基本稳定在 **4.4x ~ 4.5x**。

原因：

- 避免频繁向系统申请/释放堆对象
- 槽位已预分配，`acquire()` / `release()` 只是在空闲索引表中取回与归还
- 对象构造和析构仍然存在，因此它不会像 `MemoryPool` 那样极端快

这类模式适合：

- 需要显式对象生命周期管理
- 对象可反复复用
- 希望保留构造/析构语义，而不是仅拿裸内存

### 3. MemoryPool 对 batch 更敏感

`MemoryPool` 在 `batch=4096` 时最佳，而：

- `batch=256` 时降到 `327.29 Mops`
- `batch=16384` 时降到 `325.54 Mops`

说明：

- batch 太小：`reset()` 更频繁，循环管理开销占比上升
- batch 太大：单轮工作集变大，缓存局部性和写入带宽更容易成为瓶颈

当前机器上，`4096 × 64B = 256 KiB` 左右的单批体量较合适。

### 4. block-size 增大后，MemoryPool 优势缩小

当 `block-size=256`：

- `MemoryPool` 从约 `900 Mops` 降到 `278 Mops`
- 但仍明显快于 `operator new/delete` 的 `41.83 Mops`

原因：

- 大块申请下，真正的数据写入成本占比更高
- 纯“分配器路径”优化的收益被内存读写本身稀释

也就是说：

- 小对象/小块内存：池化收益通常更夸张
- 大对象/大块内存：池化依然有用，但不会像小块那样夸张

## 结论

在当前机器和 O3 条件下：

1. **如果任务生命周期整齐，可以整批复位，优先使用 `MemoryPool`**
   - 吞吐量最高
   - 分配路径最短
   - 最适合批量临时内存

2. **如果需要 `acquire/release` 形式的对象复用，优先使用 `ObjectPool`**
   - 语义清晰
   - 性能稳定
   - 明显优于频繁 `new/delete`

3. **提前申请确实显著提升性能**
   - `MemoryPool`：约 `4x ~ 12x`
   - `ObjectPool`：约 `4.4x ~ 4.5x`

## 建议的后续实验

- 增加多线程版本，比较：
  - 线程私有 `MemoryPool`
  - 线程私有 `ObjectPool`
  - 带锁共享池
- 增加不同对象大小测试，例如 `32B / 64B / 128B / 512B`
- 加入命中率与缓存行为分析，例如结合 `perf stat`
- 增加“构造很重”的对象类型，观察对象池在复杂对象上的收益变化
