# `op_kernel_simd_bench` PGO 对比报告

## 结论

`op_kernel_simd_bench` 可以使用 GCC PGO 编译，而且 profile 已确认被 GCC 消费。
PGO 对机器码的影响很明显：最终 `.text` 缩小 **26.28%**，反汇编指令数减少
**29.32%**，跳转指令减少 **36.01%**。

性能结果则不是全面提升：

- 对训练尺寸 `16384`，手写 SIMD 各算子的相对收益几何平均为 **+1.22%**，
  但依次执行全部 31 个 SIMD 算子的合计耗时为 **-1.80%**。
- 对带 SIMD 尾部的尺寸 `16387`，手写 SIMD 相对收益几何平均为 **+1.77%**，
  合计耗时为 **+1.29%**。
- xtensor 路径在两个尺寸下均明显回退；训练尺寸的合计耗时回退 **6.08%**。

因此，当前 profile 能显著改善代码布局和代码尺寸，但性能收益较小且依赖算子与
输入形状。现阶段不建议把该 PGO profile 直接设为默认发布配置。

## 构建改动

`benchmark/CMakeLists.txt` 新增两个缓存变量：

- `OP_KERNEL_SIMD_BENCH_PGO=OFF|GENERATE|USE`
- `OP_KERNEL_SIMD_BENCH_PGO_DIR=<profile directory>`

PGO 只应用于 `op_kernel_simd_bench`，不改变其他测试、benchmark 或第三方依赖。
当前实现针对 GCC；`USE` 阶段启用 `-Werror=missing-profile`，缺失 profile 时构建
会失败，而不是静默退化成普通 Release 构建。

实际参数如下：

```text
公共基线:
  -O3 -DNDEBUG -march=native -mprefer-vector-width=256
  -flto=auto -fno-fat-lto-objects

PGO 采集:
  -fprofile-generate=<profile-dir>

PGO 使用:
  -fprofile-use=<profile-dir> -fprofile-correction
  -Werror=missing-profile
```

## 实验环境与方法

| 项目 | 值 |
|---|---|
| 源码 | `8c47225` 加当前 LTO/PGO 工作区改动 |
| CPU | AMD EPYC Processor |
| 编译器 | GCC/G++ 12.2.0 |
| 构建 | Release、C++20、LTO 开启 |
| CPU 亲和性 | `taskset -c 0` |
| PGO 训练 | `16384 200 1` |
| 训练 profile | 219,464 B |
| 正式测量 | `16384 300 3`，基线与 PGO 交替运行三轮 |
| 尾部复测 | `16387 300 3`，基线与 PGO 交替运行三轮 |

每个程序轮次中，benchmark 对每条路径先预热 20 次，再对 3 个样本取中位数；
报告再对三个独立程序轮次取中位数。正数表示 PGO 更快。

## 性能汇总

### 训练尺寸：16384

“合计收益”使用 31 个算子的中位 `ns/element` 之和，近似表示依次执行每个算子
一次的总耗时。“几何平均”让每个算子的相对改善权重相同。

| 实现 | 基线合计 | PGO 合计 | 合计收益 | 相对收益几何平均 | 变快/变慢算子 |
|---|---:|---:|---:|---:|---:|
| no-vectorization | 84.701 | 85.553 | -1.00% | -0.05% | 14 / 17 |
| compiler optimized | 76.707 | 75.963 | +0.98% | +0.61% | 20 / 11 |
| xtensor | 77.333 | 82.339 | -6.08% | -16.09% | 10 / 20 |
| hand-written SIMD | 28.098 | 28.614 | -1.80% | +1.22% | 15 / 16 |

### 尾部尺寸：16387

| 实现 | 基线合计 | PGO 合计 | 合计收益 | 相对收益几何平均 | 变快/变慢算子 |
|---|---:|---:|---:|---:|---:|
| no-vectorization | 85.140 | 85.601 | -0.54% | -0.16% | 13 / 18 |
| compiler optimized | 76.968 | 75.987 | +1.29% | +0.31% | 15 / 14 |
| xtensor | 77.152 | 82.735 | -6.75% | -16.50% | 8 / 22 |
| hand-written SIMD | 28.804 | 28.438 | +1.29% | +1.77% | 21 / 8 |

尾部输入没有让 PGO 失效，但两个尺寸的合计收益方向不同，说明 1%～2% 量级的
变化仍容易受到代码布局、缓存状态和测量噪声影响。

## 手写 SIMD 逐算子结果

以下为训练尺寸 `16384`，单位是 `ns/element`：

| 算子 | 基线 | PGO | PGO 收益 |
|---|---:|---:|---:|
| constant | 0.157 | 0.098 | +60.20% |
| null | 0.193 | 0.185 | +4.32% |
| inverse | 0.358 | 0.358 | 0.00% |
| positive | 0.233 | 0.213 | +9.39% |
| negative | 0.229 | 0.213 | +7.51% |
| relu | 0.210 | 0.199 | +5.53% |
| abs | 0.197 | 0.188 | +4.79% |
| power2 | 0.170 | 0.214 | -20.56% |
| sqrt | 0.697 | 0.696 | +0.14% |
| if | 0.362 | 0.370 | -2.16% |
| greater | 0.255 | 0.256 | -0.39% |
| less | 0.263 | 0.258 | +1.94% |
| add | 0.256 | 0.261 | -1.92% |
| subtract | 0.257 | 0.255 | +0.78% |
| multiply | 0.259 | 0.260 | -0.38% |
| imbalance | 0.373 | 0.376 | -0.80% |
| divide | 0.358 | 0.367 | -2.45% |
| cs_zscore | 0.604 | 0.618 | -2.27% |
| cs_demean | 0.345 | 0.338 | +2.07% |
| cs_mean | 0.339 | 0.326 | +3.99% |
| cs_std | 0.441 | 0.450 | -2.00% |
| cs_sum | 0.307 | 0.326 | -5.83% |
| ts_ret | 0.522 | 0.502 | +3.98% |
| ts_sum | 0.671 | 0.656 | +2.29% |
| ts_mean | 1.065 | 1.040 | +2.40% |
| ts_demean | 1.067 | 1.070 | -0.28% |
| ts_std | 2.611 | 2.705 | -3.48% |
| ts_skew | 3.258 | 3.377 | -3.52% |
| ts_kurt | 4.084 | 4.337 | -5.83% |
| ts_zscore | 2.900 | 2.878 | +0.76% |
| ts_corr | 5.057 | 5.224 | -3.20% |

`constant` 的相对收益很大，但绝对时间很短。相反，`ts_kurt`、`ts_corr` 等较重
算子的较小回退会主导合计耗时，所以几何平均为正而合计收益为负并不矛盾。

## 汇编和二进制变化

| 指标 | Release + LTO | Release + LTO + PGO | 变化 |
|---|---:|---:|---:|
| 文件大小 | 640,056 B | 612,248 B | -4.34% |
| `.text` | 525,500 B | 387,382 B | -26.28% |
| 反汇编指令数 | 118,344 | 83,651 | -29.32% |
| `call` 指令 | 4,267 | 3,927 | -7.97% |
| 跳转指令 | 14,465 | 9,256 | -36.01% |
| 代码符号 | 423 | 886 | +109.46% |
| `.cold` 克隆 | 116 | 271 | +133.62% |
| `.constprop` 克隆 | 52 | 179 | +244.23% |
| `.isra` 克隆 | 27 | 63 | +133.33% |

代码符号增加而 `.text` 减少，是因为 GCC 根据真实执行频率创建了更多小型的热、
冷和常量传播版本，同时缩小了原先的大型模板实例：

- `main`：21,485 B → 21,972 B，增加 2.27%。
- `constant` 的 `Run<lambda #1...#4>` 热主体：4,361 B → 2,434 B，减少
  44.19%。
- `positive` 的 `Run<lambda #13...#16>` 热主体：5,634 B → 1,661 B，减少
  70.52%。

例如 PGO 版本将 `constant` 的手写 SIMD lambda 独立出来，并把训练中极少执行的
小数组路径放入冷克隆：

```asm
cmp    $0x3,%rdi
jbe    <main::{lambda()#4}::operator()() const [clone .cold]>
vbroadcastsd <constant>,%ymm0
...
prefetchw (%rdx)
vmovupd %ymm0,...
```

基线版本也生成了 AVX 写循环，但它被嵌在约 4.3 KB 的 `Run` 实例内部，低频分支
仍跳到同一个大型函数的后部。PGO 版本把热循环、冷尾部和计时/输出路径重新布局，
改善指令缓存局部性，并减少热路径附近的跳转。

PGO 并没有改变算法，也没有简单地把所有函数内联。它根据训练频率重新决定哪些
函数内联、克隆、常量传播或移到冷路径；这也是部分算子加速、部分算子回退的原因。

## 复现流程

PGO 的 `GENERATE` 和 `USE` 必须复用同一个构建目录，否则 GCC 生成的 profile
文件名与对象路径不匹配。

```bash
cmake -S . -B build_pgo \
  -DCMAKE_BUILD_TYPE=Release \
  -DOP_KERNEL_SIMD_BENCH_PGO=GENERATE \
  -DOP_KERNEL_SIMD_BENCH_PGO_DIR="$PWD/build_pgo/profile"
cmake --build build_pgo --target op_kernel_simd_bench --parallel

# 训练，不采信插桩版本的耗时
build_pgo/benchmark/op_kernel_simd_bench 16384 200 1

cmake -S . -B build_pgo \
  -DOP_KERNEL_SIMD_BENCH_PGO=USE
cmake --build build_pgo --target op_kernel_simd_bench --parallel

taskset -c 0 \
  build_pgo/benchmark/op_kernel_simd_bench 16384 300 3
```

源码或编译参数改变后，应删除旧 profile 并重新执行 `GENERATE`；不要把不匹配的
profile 用于新二进制。

## 建议

这个 benchmark 同时比较四种实现。对整个 benchmark 做 PGO，会同时优化计时
框架、标量实现、xtensor 和手写 SIMD，因此它更适合研究“整个程序在指定 workload
下的 PGO 效果”，不适合作为完全中性的微基准对比。

如果目标是优化生产环境中的 `factor_compute`，更可靠的做法是用真实 executor
workload 采集 profile，再对生产二进制做 PGO；同时保留未使用 PGO 编译的独立
benchmark 作为测量工具。
