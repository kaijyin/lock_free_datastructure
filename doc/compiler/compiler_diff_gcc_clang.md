大多数常用选项在 GCC 和 Clang 里**拼写相同**，但不能认为效果完全相同。两者优化 pass、成本模型、默认浮点语义和 LTO 实现都不同。

最值得注意的坑是：

> GCC 的 `-flto=auto` 是“自动并行 LTO”；Clang 的 `-flto=auto` 等价于 `-flto=full`，并不等价于 ThinLTO。

## 一、常用选项对照

| 目的                     | GCC                           | Clang                         | 主要区别                       |
| ---------------------- | ----------------------------- | ----------------------------- | -------------------------- |
| 高级优化                   | `-O3`                         | `-O3`                         | 拼写一样，但具体 pass 和成本模型不同      |
| 本机指令集                  | `-march=native`               | `-march=native`               | 基本相同                       |
| CPU 调优                 | `-mtune=native`               | `-mtune=native`               | 基本相同                       |
| LTO                    | `-flto=auto`                  | `-flto=thin` 或 `-flto=full`   | **差异最大**                   |
| PGO 生成                 | `-fprofile-generate`          | `-fprofile-generate`          | 文件格式和后处理流程不同               |
| PGO 使用                 | `-fprofile-use`               | `-fprofile-use=file.profdata` | Clang 需要 `llvm-profdata`   |
| 激进浮点                   | `-ffast-math`                 | `-ffast-math`                 | 大方向相同，内部选项组合略有区别           |
| `-Ofast`               | 仍支持                           | 已弃用                           | Clang 应写 `-O3 -ffast-math` |
| 循环展开                   | `-funroll-loops`              | `-funroll-loops`              | 启发式策略不同                    |
| SIMD 宽度偏好              | `-mprefer-vector-width=256`   | 同名                            | 两者都支持                      |
| 禁止 PLT                 | `-fno-plt`                    | 同名                            | ELF 平台基本类似                 |
| 符号隐藏                   | `-fvisibility=hidden`         | 同名                            | 基本类似                       |
| semantic interposition | `-fno-semantic-interposition` | 同名                            | 都支持，但实际默认行为和收益可能不同         |
| 异常/RTTI                | `-fno-exceptions -fno-rtti`   | 同名                            | 基本类似                       |

---

# 二、LTO：GCC 和 Clang 最大的区别

## GCC

推荐：

```bash
g++ -O3 -march=native -flto=auto ...
```

GCC 的：

```bash
-flto=auto
```

表示使用 GNU Make jobserver；没有 jobserver 时，自动根据 CPU 数量并行执行 GCC 的 LTO/WHOPR 后端。([GCC][1])

## Clang

Clang 明确区分两种模式：

```bash
-flto=full
-flto=thin
```

而且：

```bash
-flto
-flto=auto
-flto=jobserver
```

在当前 Clang 中都等价于：

```bash
-flto=full
```

不是自动选择 ThinLTO。([Clang][2])

### Clang Full LTO

```bash
clang++ -O3 -march=native \
  -flto=full \
  -fuse-ld=lld \
  ...
```

特点：

* 全局优化能力最强
* 将整个程序视为大型 LLVM IR
* 链接内存和时间较高
* 适合最终发布版本或中小型程序

### Clang ThinLTO

```bash
clang++ -O3 -march=native \
  -flto=thin \
  -fuse-ld=lld \
  ...
```

特点：

* 跨模块分析
* 后端并行
* 增量构建友好
* 链接时间和内存通常比 Full LTO 更可控

ThinLTO 在链接阶段先合并各模块摘要，再由并行后端执行函数导入和优化；默认会并行运行后端。([Clang][3])

### 实际推荐

大型 C++ 项目首先测试：

```bash
clang++ -O3 -march=native \
  -flto=thin \
  -fuse-ld=lld \
  -DNDEBUG
```

最终性能再比较：

```text
ThinLTO
Full LTO
无 LTO
```

ThinLTO 还支持链接缓存：

```bash
-Wl,--thinlto-cache-dir=/tmp/thinlto-cache
```

以及控制并行度：

```bash
-Wl,--thinlto-jobs=8
```

这些属于 Clang/LLVM ThinLTO 比较成熟的工程化能力。([Clang][3])

---

# 三、PGO：选项名字类似，工作流不同

## GCC PGO

```bash
# 生成插桩版本
g++ -O3 -march=native -flto=auto \
  -fprofile-generate=profile \
  *.cpp -o app

# 运行典型工作负载
./app

# 使用 profile
g++ -O3 -march=native -flto=auto \
  -fprofile-use=profile \
  *.cpp -o app
```

GCC 通常直接产生并读取 `.gcda` 等 GCC profile 文件。`-fprofile-use` 还会启用循环展开、tracer、函数克隆、value profiling 等一组依赖真实运行频率的优化。([GCC][1])

## Clang PGO

Clang 的插桩运行结果首先是：

```text
*.profraw
```

需要用 `llvm-profdata` 合并为：

```text
*.profdata
```

完整流程：

```bash
# 生成插桩程序
clang++ -O3 -march=native \
  -fprofile-generate=profile \
  *.cpp -o app

# 运行，可以用环境变量指定文件名
LLVM_PROFILE_FILE="profile/app-%p.profraw" ./app

# 转换 profile
llvm-profdata merge \
  -output=app.profdata \
  profile/

# 使用 profile
clang++ -O3 -march=native \
  -fprofile-use=app.profdata \
  *.cpp -o app
```

即使只有一个 `.profraw`，仍然需要执行 `llvm-profdata merge`，因为它不仅合并数据，还会转换文件格式。([Clang][4])

## Clang 特有的上下文敏感 PGO

Clang 还有：

```bash
-fcs-profile-generate
```

CS 表示 context-sensitive。它在内联之后插桩，可以区分同一函数从不同调用路径进入时的行为。

例如：

```cpp
process();  // 从行情线程调用
process();  // 从后台统计线程调用
```

普通 PGO 可能只看到 `process()` 的整体频率；上下文敏感 PGO 能更好地区分不同调用链。Clang 官方建议与 `-fprofile-use` 配合构建第二轮 profile。([Clang][4])

对于固定调用图、算子 DAG、大量公共小函数，这个能力值得测试。

---

# 四、`-Ofast` 的区别

## GCC

GCC 的 `-Ofast` 目前仍然是有效选项，而且不只是：

```bash
-O3 -ffast-math
```

它还会启用或改变：

* `-fallow-store-data-races`
* `-fno-semantic-interposition`
* `-fno-protect-parens`
* 部分 Fortran 选项

因此 GCC 的 `-Ofast` 比单独的 `-O3 -ffast-math` 更激进。([GCC][1])

## Clang

Clang 已经将 `-Ofast` 标为弃用，官方建议写成：

```bash
-O3 -ffast-math
```

或者只使用：

```bash
-O3
```

保持标准兼容优化。([Clang][2])

所以跨编译器配置不要写：

```cmake
-Ofast
```

更可控的写法是：

```cmake
-O3 -ffast-math
```

---

# 五、Clang 的浮点选项更细致

除了 GCC 兼容的：

```bash
-ffast-math
-fno-math-errno
-fno-trapping-math
-ffp-contract=fast
```

Clang 还有一个比较好用的总开关：

```bash
-ffp-model=precise
-ffp-model=strict
-ffp-model=fast
-ffp-model=aggressive
```

含义大致是：

| Clang 选项     | 含义                               |
| ------------ | -------------------------------- |
| `precise`    | 默认，保持数值安全，允许标准范围内的 FMA           |
| `strict`     | 严格浮点环境、异常和舍入语义                   |
| `fast`       | 放宽一部分浮点规则，但比完整 `-ffast-math` 稍保守 |
| `aggressive` | 基本等价于 `-ffast-math`              |

Clang 官方把 `aggressive` 定义为完整启用 `-ffast-math`，而 `fast` 是 `-funsafe-math-optimizations`、`-fno-math-errno` 和快速 FMA contraction 的组合。([Clang][4])

### 实际建议

比较保守：

```bash
clang++ -O3 -march=native \
  -ffp-model=fast
```

非常激进：

```bash
clang++ -O3 -march=native \
  -ffast-math
```

严格结果：

```bash
clang++ -O3 -march=native \
  -ffp-model=strict
```

---

# 六、一个容易忽略的默认差异：trapping math

Clang 默认行为相当于：

```bash
-fno-trapping-math
```

即假设普通浮点操作不会产生用户可见的 trap。([Clang][4])

GCC 当前默认则是：

```bash
-ftrapping-math
```

除非使用 `-Ofast` 或显式加入：

```bash
-fno-trapping-math
```

([GCC][1])

所以这组配置：

```bash
-O3 -fno-math-errno -fno-trapping-math
```

在 GCC 上可能比单独 `-O3` 释放更多优化空间；在 Clang 上，`-fno-trapping-math` 往往已经是默认值。

---

# 七、FMA contraction 默认也不完全相同

Clang 的默认是：

```bash
-ffp-contract=on
```

允许在同一个表达式内部形成 FMA，但默认不跨语句融合：

```cpp
double r = a * b + c;  // 可以形成 FMA
```

跨语句：

```cpp
double r = a * b;
r += c;
```

通常需要：

```bash
-ffp-contract=fast
```

才能更加积极地融合。([Clang][4])

GCC 当前在多数非严格 C 模式以及 C++ 模式下，默认是：

```bash
-ffp-contract=fast
```

而严格标准 C 模式下默认可能是 `off`。([GCC][1])

这意味着即使两边都使用：

```bash
-O3 -march=native
```

FMA 的形成位置也可能不完全一致。

想让结果更可比，可以显式指定：

```bash
-ffp-contract=fast
```

或者：

```bash
-ffp-contract=off
```

---

# 八、Clang 特有或更好用的 SIMD 选项

## 1. 禁止自动生成 gather/scatter

Clang 提供：

```bash
-mno-gather
-mno-scatter
```

它们禁止**自动向量化器**生成 gather/scatter 指令。([Clang][2])

例如：

```bash
clang++ -O3 -march=native -mno-gather ...
```

这对你之前那种 gather 占比很高的树模型代码很有价值，可以比较：

```text
默认自动 gather
-mno-gather 后保持标量加载
手工重构数据布局
```

但它**不会删除你显式写的 intrinsic**：

```cpp
_mm256_i32gather_pd(...)
```

因为那是你明确要求生成的指令，不属于自动向量化。

---

## 2. 向量数学函数库

Clang 支持：

```bash
-fveclib=libmvec
-fveclib=SLEEF
-fveclib=SVML
-fveclib=AMDLIBM
```

这允许向量器将循环中的：

```cpp
std::sin(x[i]);
std::exp(x[i]);
std::log(x[i]);
```

映射到向量数学库，而不只是生成逐元素标量函数调用。当前 Clang 文档列出了 libmvec、SLEEF、SVML、ArmPL、AMDLIBM 等后端。([Clang][2])

例如 Linux/glibc：

```bash
clang++ -O3 -march=native \
  -ffast-math \
  -fveclib=libmvec
```

普通加减乘除循环用不到这个选项；包含 `sin/exp/log/pow` 的循环才可能明显受益。

---

## 3. Clang 循环 pragma 更丰富

```cpp
#pragma clang loop vectorize(enable)
#pragma clang loop vectorize_width(4)
#pragma clang loop interleave_count(2)
for (std::size_t i = 0; i < n; ++i) {
    out[i] = a[i] + b[i];
}
```

还可以控制：

* vectorize
* interleave
* unroll
* vector width
* interleave count

LLVM 文档明确支持通过 `#pragma clang loop` 控制向量化和交错因子。([LLVM][5])

不过 pragma 仍然只是强提示；如果存在不可处理的数据依赖，编译器不一定能安全生成 SIMD。

---

# 九、优化报告选项完全不同

## GCC

常用：

```bash
-fopt-info-vec-optimized
-fopt-info-vec-missed
-fopt-info-inline-optimized-missed
```

例如：

```bash
g++ -O3 -march=native \
  -fopt-info-vec-optimized \
  -fopt-info-vec-missed \
  test.cpp
```

GCC 的 `-fopt-info` 可以按 `vec`、`loop`、`inline`、`ipa` 等优化组筛选成功、失败和详细信息。([GCC][6])

## Clang

对应写法：

```bash
-Rpass=loop-vectorize
-Rpass-missed=loop-vectorize
-Rpass-analysis=loop-vectorize
```

例如：

```bash
clang++ -O3 -march=native \
  -Rpass=loop-vectorize \
  -Rpass-missed=loop-vectorize \
  -Rpass-analysis=loop-vectorize \
  test.cpp
```

含义：

```text
-Rpass              成功做了什么
-Rpass-missed       哪些优化失败
-Rpass-analysis     为什么失败
```

([LLVM][5])

Clang 还可以输出结构化 YAML：

```bash
-fsave-optimization-record
```

生成类似：

```text
test.opt.yaml
```

其中包含内联、向量化、循环优化等决策。([Clang][4])

对于分析 SIMD，Clang 的这一套通常非常顺手。

---

# 十、Clang 的 C++ 虚函数优化选项

## `-fwhole-program-vtables`

```bash
-flto=thin -fwhole-program-vtables
```

或者：

```bash
-flto=full -fwhole-program-vtables
```

它让 Clang 利用“程序中所有相关虚表已经可见”的假设做：

* 去虚拟化
* 虚表优化
* 类型判断优化
* 删除不可能的虚调用目标

该选项要求 LTO。([Clang][2])

对于算子 DAG：

```cpp
struct Operator {
    virtual void compute(...) = 0;
};
```

如果所有派生类都在当前程序中，Clang 能更积极地把：

```cpp
op->compute(...)
```

转换成直接调用甚至内联。

## `-fvirtual-function-elimination`

```bash
-flto=full \
-fwhole-program-vtables \
-fvirtual-function-elimination
```

它用于删除最终不可达的虚函数实现，但要求：

```bash
-flto=full
```

不能只依赖 ThinLTO。([Clang][2])

## `-fstrict-vtable-pointers`

```bash
-fstrict-vtable-pointers
```

允许 Clang 根据 C++ 对多态对象生命周期和 vptr 修改的严格规则优化重复虚表读取。([Clang][2])

但如果代码里有很野的：

* placement new 覆盖现有多态对象
* 手工修改对象内存
* 违反对象生命周期规则
* 依赖 UB 的对象池实现

这个选项可能暴露问题。

---

# 十一、不要把 GCC 的细粒度 pass 选项直接复制给 Clang

GCC 常见：

```bash
-fipa-cp-clone
-fipa-icf
-ftree-loop-distribution
-floop-unroll-and-jam
-fversion-loops-for-strides
-fgcse-after-reload
-fpredictive-commoning
```

这些名称来自 GCC 的 GIMPLE/RTL 优化管线。

Clang 使用 LLVM IR 和 Machine IR，内部 pass 名称完全不同。即使 Clang 为兼容性接受某个 GCC 选项，也可能：

* 没有效果
* 只是别名
* 语义不完全相同
* 被忽略并给出 warning
* 只控制 LLVM 中近似的某个 pass

迁移到 Clang 时，建议保留高层选项：

```bash
-O3
-march=native
-flto=thin
-fprofile-use
-ffast-math
-funroll-loops
-mprefer-vector-width=256
```

不要整包复制几十个 GCC `-fipa-*`、`-ftree-*` 选项。

---

# 十二、推荐配置对照

## GCC 通用高性能版

```bash
g++ \
  -O3 \
  -march=native \
  -flto=auto \
  -DNDEBUG \
  ...
```

## Clang 通用高性能版

```bash
clang++ \
  -O3 \
  -march=native \
  -flto=thin \
  -fuse-ld=lld \
  -DNDEBUG \
  ...
```

## Clang 最终发布、追求完整 LTO

```bash
clang++ \
  -O3 \
  -march=native \
  -flto=full \
  -fuse-ld=lld \
  -DNDEBUG \
  ...
```

## 多态算子/DAG 场景

```bash
clang++ \
  -O3 \
  -march=native \
  -flto=thin \
  -fuse-ld=lld \
  -fwhole-program-vtables \
  -fstrict-vtable-pointers \
  -DNDEBUG \
  ...
```

## 允许放宽浮点语义

```bash
clang++ \
  -O3 \
  -march=native \
  -flto=thin \
  -fuse-ld=lld \
  -ffast-math \
  -DNDEBUG \
  ...
```

或者稍保守：

```bash
clang++ \
  -O3 \
  -march=native \
  -flto=thin \
  -fuse-ld=lld \
  -ffp-model=fast \
  -DNDEBUG \
  ...
```

## 针对 gather 瓶颈做实验

```bash
clang++ \
  -O3 \
  -march=native \
  -flto=thin \
  -mno-gather \
  -Rpass=loop-vectorize \
  -Rpass-missed=loop-vectorize \
  ...
```

最值得同时 benchmark 的组合是：

```text
GCC  -O3 -march=native
GCC  -O3 -march=native -flto=auto
Clang -O3 -march=native
Clang -O3 -march=native -flto=thin -fuse-ld=lld
Clang -O3 -march=native -flto=full -fuse-ld=lld
```

不要预设 Clang 或 GCC 一定更快。对树模型、gather、复杂分支、虚函数 DAG 来说，两者的向量化成本模型、去虚拟化决策、循环展开程度和寄存器分配很可能不同，最后还是以同一份 workload 的延迟分位数、吞吐量、二进制尺寸和 `perf` 数据为准。

[1]: https://gcc.gnu.org/onlinedocs/gcc-15.2.0/gcc/Optimize-Options.html "Optimize Options (Using the GNU Compiler Collection (GCC))"
[2]: https://clang.llvm.org/docs/ClangCommandLineReference.html "Clang command line argument reference — Clang 24.0.0git documentation"
[3]: https://clang.llvm.org/docs/ThinLTO.html "ThinLTO — Clang 24.0.0git documentation"
[4]: https://clang.llvm.org/docs/UsersManual.html "Clang Compiler User’s Manual — Clang 24.0.0git documentation"
[5]: https://llvm.org/docs/Vectorizers.html "Auto-Vectorization in LLVM - LLVM"
[6]: https://gcc.gnu.org/onlinedocs/gcc/Developer-Options.html?utm_source=chatgpt.com "Developer Options (Using the GNU Compiler Collection (GCC))"
