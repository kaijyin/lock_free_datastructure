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

### PGO 主要做了什么

一句话概括：PGO 没有更换算法，而是利用训练阶段记录的执行次数，重新决定大段
代码是否内联、基本块如何排列、低频分支放在哪里，以及循环主路径应该重点优化
哪一种输入形状。

`-O3 -march=native` 主要负责生成 SIMD 指令，LTO 让编译器看见跨文件代码；PGO
在此基础上补充“哪些代码实际最常执行”的信息。这三类优化相互配合，但职责不同。

#### 1. 合并预热和计时阶段重复展开的大循环

`Measure()` 在两个位置调用同一个 `operation`：

```cpp
for (std::size_t i = 0; i < 20; ++i) operation();

for (std::size_t sample = 0; sample < samples; ++sample) {
  const auto start = Clock::now();
  for (std::size_t i = 0; i < iterations; ++i) operation();
  // 记录耗时。
}
```

非 PGO 版本把部分 lambda 的数组循环分别内联到预热和计时位置，相同的机器码会
在 `Run` 实例中出现两份。PGO 版本根据实际调用频率和函数大小调整内联决策：把
较大的 lambda 提取成一个独立函数，两个位置通过 `call` 复用同一份循环。

例如 `constant` 的手写 SIMD lambda #4 在 PGO 版本中被两个循环共同调用：

```asm
# 20 次预热
38f68:
    mov    %r15,%rdi
    call   39970 <main::{lambda()#4}::operator()() const>
    dec    %r12
    jne    38f68

# 正式计时
395e3:
    mov    %r15,%rdi
    inc    %r14
    call   39970 <main::{lambda()#4}::operator()() const>
    cmp    %r14,%rbp
    jne    395e3
```

一次 lambda 调用内部会处理 16,384 个元素，因此额外一次函数调用的固定成本很小；
只保留一份大循环则可以减轻指令缓存压力。

`positive` 的变化更加明显。PGO 将禁止向量化的 lambda #13、编译器自动向量化的
lambda #14 和手写 SIMD 的 lambda #16 都提取成了独立函数；xtensor 的 lambda #15
包装层仍被内联，`Run` 直接调用 `XUnary`。非 PGO 版本中没有这些独立 lambda 符号，
相应循环都位于 `Run` 内部。因为 #14 和 #16 比 `constant` 的填充循环更大，
`positive` 热 `Run` 的缩小比例也更高。

#### 2. 把低频边界处理移到冷区

训练的主要输入长度为 16,384，GCC 因而知道大数组 SIMD 主循环是高频路径，而
`size <= 3`、标量尾部等分支很少执行。PGO 没有删除这些正确性处理，而是把它们
拆分到 `.cold` 克隆：

```asm
cmp    $0x3,%rdi
jbe    <main::{lambda()#16}::operator()() const [clone .cold]>

# 紧接高频 AVX 主循环。
vmovupd      (...),%ymm0
vcmpge_oqpd  %ymm4,%ymm0,%ymm2
vcmpordpd    %ymm0,%ymm0,%ymm1
vandpd       %ymm2,%ymm1,%ymm1
vblendvpd    %ymm1,%ymm0,%ymm3,%ymm0
```

标量尾部位于较远的冷代码区域：

```asm
<main::{lambda()#16}::operator()() const [clone .cold]>:
    vmovsd   (...),%xmm0
    vucomisd %xmm0,%xmm0
    ...
```

这样热循环附近包含更少的低频跳转和边界代码，CPU 顺序取指和分支预测更容易命中。
整个二进制的 `.cold` 克隆从 116 个增加到 271 个，也验证了这种热冷拆分。

#### 3. 根据真实循环次数优化主循环形态

`constant` 的非 PGO 写循环使用较碎的 128-bit 写入和多条 lane 提取：

```asm
prefetchw     (%rax)
add           $0x40,%rax
vmovupd       %xmm3,-0x480(%rax)
vextractf128  $0x1,%ymm3,-0x470(%rax)
vextractf128  $0x1,%ymm3,-0x460(%rax)
vextractf128  $0x1,%ymm3,-0x450(%rax)
```

PGO 版本针对训练中反复出现的大数组路径，生成了更紧凑的 64 字节循环主体：

```asm
vbroadcastsd <constant>,%ymm0

<loop>:
    vmovupd   %ymm0,-0x440(%rdx)
    prefetchw (%rdx)
    add       $0x40,%rdx
    vmovupd   %ymm0,-0x460(%rdx)
    cmp       %r8,%rsi
    jb        <loop>
```

两次 256-bit store 完成一次 64 字节批量写入，主循环指令更少。这一变化与函数去重、
热冷布局共同作用，是 `constant` SIMD 明显加速的重要原因；不能只用函数尺寸变化
解释其性能收益。

#### 4. 创建更小的专用版本并重新排列基本块

PGO 与 LTO 一起生成了更多 `.constprop` 和 `.isra` 克隆。它们分别表示常量传播后的
专用函数，以及经过跨过程标量替换或参数简化的函数。符号数量因此增加，但通用大
函数和低频分支被拆小，最终 `.text` 反而减少 26.28%。

从整个二进制看，PGO 后：

- 跳转指令减少 36.01%，说明基本块布局和冷路径拆分变化明显；
- `call` 只减少 7.97%，说明 PGO 并不是简单地“尽量内联”；
- `.constprop`、`.isra` 和 `.cold` 克隆均明显增加，说明 GCC 更积极地生成适合实际
  workload 的专用版本。

#### 如何理解热 `Run` 缩小比例

44.19% 和 70.52% 只统计热 `Run` 符号本身，不包含被提取出的 lambda 和 `.cold`
代码。将局部相关代码重新加回来后，对比更接近如下口径：

| 实例 | 非 PGO `Run` + cold | PGO `Run` + lambda + cold | 局部缩小 |
|---|---:|---:|---:|
| `constant` | 4,509 B | 3,155 B | 30.03% |
| `positive` | 5,782 B | 3,386 B | 41.44% |

所以，热 `Run` 缩小不代表运行时少做了相同比例的计算。部分机器码只是从 `Run`
搬到可以复用的独立函数或冷区；真正删除的主要是重复展开、通用分支和可以由 profile
证明不必放在热路径附近的代码。

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
