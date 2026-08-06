先纠正一下写法：GCC 是单横线：

```bash
-O3 -march=native
```

不是 `--march`。在 x86 上，`-march=native` 通常已经隐含对应的 `-mtune=native`，所以一般不必再重复写 `-mtune=native`。([GCC][1])

## 结论

在 `-O3 -march=native` 之外，真正值得额外考虑的主要是：

1. **LTO：`-flto=auto`**
2. **PGO：`-fprofile-generate` + `-fprofile-use`**
3. **放宽浮点语义：`-ffast-math` 或更细粒度的数学选项**
4. **动态库优化：`-fno-semantic-interposition`、`-fvisibility=hidden`、`-fno-plt`**
5. **根据 benchmark 决定的激进优化：`-funroll-loops`、预取、向量宽度控制**
6. **构建配置：`-DNDEBUG`，避免发布版仍运行断言**

其中，最通用、最值得加的是：

```bash
-O3 -march=native -flto=auto -DNDEBUG
```

追求极致性能时，再加 PGO。

---

# 一、强烈推荐额外开启

## 1. `-flto=auto`：跨文件链接时优化

```bash
-O3 -march=native -flto=auto
```

`-O3` 只能充分优化**当前翻译单元内可见的代码**。如果一个热函数实现在另一个 `.cpp` 或静态库中，没有 LTO，GCC 通常无法跨文件：

* 内联函数
* 传播常量
* 去虚拟化
* 删除未使用参数
* 分析函数副作用
* 删除不可达代码

开启 LTO 后，多个目标文件可以像一个大的翻译单元一样参与跨过程分析。GCC 官方要求编译和最终链接阶段都使用 `-flto`；`-flto=auto` 可以自动利用 Make jobserver 或 CPU 核数并行执行。([GCC][2])

### 场景

`operator.cpp`：

```cpp
double add_scale(double x) {
    return x * 0.01 + 1.0;
}
```

`engine.cpp`：

```cpp
extern double add_scale(double);

void calculate(const double* input, double* output, int n) {
    for (int i = 0; i < n; ++i)
        output[i] = add_scale(input[i]);
}
```

没有 LTO，循环里可能保留函数调用。

开启 LTO 后，`add_scale()` 可以被跨文件内联，进而让整个循环向量化。

### CMake

```cmake
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)

if(ipo_supported)
    set_property(TARGET my_target PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
```

或者直接：

```cmake
target_compile_options(my_target PRIVATE -O3 -march=native -flto=auto)
target_link_options(my_target PRIVATE -flto=auto)
```

### 静态库注意

使用 LTO 静态库时，推荐让构建系统使用：

```bash
gcc-ar
gcc-ranlib
gcc-nm
```

现代工具链通常能通过 linker plugin 自动处理，但老工具链可能无法把静态库内部的 LTO IR 纳入优化。([GCC][2])

---

## 2. PGO：依据真实运行情况优化

PGO 通常比继续堆一堆零碎 `-fxxx` 更有效。

第一阶段生成插桩程序：

```bash
g++ -O3 -march=native -flto=auto \
    -fprofile-generate=./profile \
    *.cpp -o app
```

使用有代表性的输入运行：

```bash
./app representative_input
```

第二阶段读取 profile：

```bash
g++ -O3 -march=native -flto=auto \
    -fprofile-use=./profile \
    *.cpp -o app
```

PGO 能告诉编译器：

* 哪些函数真正是热函数
* 哪条分支最常走
* 哪些间接调用目标最常出现
* 循环通常迭代多少次
* 哪些函数值得内联
* 哪些循环值得展开
* 哪些错误处理路径应该移到冷代码区

`-fprofile-use` 还会额外启用一批依赖 profile 才比较安全的优化，例如 `-funroll-loops`、tracer、value profiling、函数重排和更积极的热路径优化。([GCC][2])

### 特别适合你的场景

树模型或 DAG 计算中：

```cpp
switch (node->type) {
case Add:
    ...
case Multiply:
    ...
case Divide:
    ...
case Custom:
    ...
}
```

静态编译时，GCC 不知道哪个算子最常出现。

PGO 可能发现：

```text
Add       60%
Multiply  35%
其他       5%
```

于是它可以：

* 把 Add/Multiply 路径连续放置
* 把罕见路径放入冷区
* 更积极内联常见算子
* 优化间接调用目标
* 改善 I-cache 局部性

### 注意

训练数据必须具有代表性。训练时完全没执行的函数，默认可能偏向按尺寸优化；如果训练覆盖不完整，可以考虑：

```bash
-fprofile-partial-training
```

它让未训练到的函数按普通非 PGO 模式优化，而不是激进地压缩尺寸。([GCC][2])

---

# 二、浮点计算需要额外设置

## 3. `-ffast-math`

`-O3` **不会自动开启** `-ffast-math`。

```bash
-O3 -march=native -ffast-math
```

它会放宽一系列 IEEE 浮点规则，包括：

* 假设不存在部分 NaN、Inf 情况
* 允许浮点表达式重结合
* 不维护数学函数的 `errno`
* 假设舍入模式不会动态变化
* 放宽 signed zero
* 更积极地向量化浮点 reduction
* 某些除法可转换成倒数乘法

GCC 文档显示，`-ffast-math` 是多个浮点选项的集合；`-Ofast` 则相当于 `-O3` 再加上 `-ffast-math` 等非严格标准优化。([GCC][2])

### 场景：浮点 reduction

```cpp
double sum(const double* x, std::size_t n) {
    double result = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        result += x[i];
    return result;
}
```

严格浮点语义下：

```text
(((x0 + x1) + x2) + x3)
```

不能随意改成：

```text
(x0 + x1) + (x2 + x3)
```

因为浮点加法不是严格结合的。这会限制 SIMD reduction。

`-ffast-math` 允许重结合，更容易生成并行累加：

```text
ymm0 += x[0..3]
ymm1 += x[4..7]
```

### 不适合

以下情况不要直接使用：

* 金融账务结果
* 依赖 NaN 传播
* 依赖 `+0.0` 和 `-0.0`
* 修改浮点舍入模式
* 要求逐 bit 可重复
* 精确判断浮点异常

---

## 4. 更保守的数学选项

不想一次开启完整 `-ffast-math`，可以只选择需要的部分。

### `-fno-math-errno`

```bash
-O3 -march=native -fno-math-errno
```

允许 GCC 假设 `sqrt`、`sin`、`exp` 等数学函数不需要设置全局 `errno`。

例如：

```cpp
double y = std::sqrt(x);
```

某些情况下可以直接生成硬件指令，而不是调用带 `errno` 语义的库函数。

这是相对容易接受的选项，特别适合内部数值计算库。

---

### `-fno-trapping-math`

```bash
-fno-trapping-math
```

告诉编译器不依赖浮点异常 trap，例如除零、overflow、invalid operation 的硬件异常行为。

它可以释放一些浮点重排机会，但仍然应该做数值正确性测试。

---

### `-fassociative-math`

允许浮点加法、乘法重结合：

```bash
-fassociative-math
```

它对 reduction 向量化很有帮助，但可能改变累计误差。

通常它还需要其他浮点选项配合，不建议脱离完整配置随便单独添加。

---

### 实际建议

比完整 `-ffast-math` 更保守的组合：

```bash
-O3 -march=native \
-fno-math-errno \
-fno-trapping-math
```

如果结果误差可以接受，再测试：

```bash
-O3 -march=native -ffast-math
```

不要直接假设它更快，要 benchmark，因为很多普通加减乘循环在 `-O3` 下已经能很好地 SIMD。

---

# 三、动态库场景需要额外设置

## 5. `-fno-semantic-interposition`

```bash
-O3 -fPIC -fno-semantic-interposition
```

Linux 共享库默认允许库内的全局符号在运行时被替换：

```cpp
double scale() {
    return 0.01;
}

double process(double x) {
    return x * scale();
}
```

即使 `scale()` 和 `process()` 在同一个 `.so` 中，默认情况下 GCC 也要考虑 `scale()` 被其他 DSO 或 `LD_PRELOAD` 替换的可能。

这会阻止：

* 内联
* 常量传播
* 函数副作用分析
* 某些跨函数优化

使用：

```bash
-fno-semantic-interposition
```

后，GCC 可以更积极优化库内部调用。([GCC][2])

### 适合

* 自己控制的内部 `.so`
* 计算引擎动态库
* 不需要外部 hook 的算法库
* 不依赖 `LD_PRELOAD` 替换内部函数

### 不适合

* 需要符号覆盖
* malloc hook
* profiler/instrumentation 依赖 interposition
* 插件通过替换全局符号工作

---

## 6. `-fvisibility=hidden`

```bash
-fvisibility=hidden
```

默认隐藏符号，只把真正的公共 API 显式导出：

```cpp
#define API __attribute__((visibility("default")))

API void run_engine();
```

收益主要是：

* 减少动态符号表
* 减少 PLT/GOT 间接访问
* 告诉编译器更多函数不会被外部替换
* 减少动态链接和加载工作
* 有利于内部调用优化

GCC 文档也建议结合显式 visibility 属性，使外部声明避免不必要地通过 PLT 调用。([GCC][3])

典型组合：

```bash
-O3 -march=native -flto=auto \
-fPIC \
-fvisibility=hidden \
-fno-semantic-interposition
```

---

## 7. `-fno-plt`

```bash
-fno-plt
```

针对外部动态函数调用，避免通过 PLT stub：

```cpp
memcpy(dst, src, n);
```

传统 PIC 调用大致经过：

```text
caller → PLT stub → GOT → memcpy
```

`-fno-plt` 改为从 GOT 加载地址后直接间接调用，可能减少 PLT 跳转，并让 GOT load 暴露给优化器。

代价是：

* 外部符号在程序加载时解析
* 不能使用正常的 PLT lazy binding
* 对整体性能的影响通常很小
* 可能改善高频外部函数调用，也可能没区别

GCC 官方说明该选项通过避免 PLT stub 生成更直接的调用代码，但会使外部符号在加载时完成解析。([GCC][3])

适合 benchmark：

```bash
-O3 -march=native -fno-plt
```

但优先级明显低于 LTO 和 PGO。

---

# 四、`-O3` 没有默认完全开启的激进循环优化

## 8. `-funroll-loops`

```bash
-O3 -march=native -funroll-loops
```

当前 GCC 的 `-O3` 已经包含：

* `-fpeel-loops`
* `-floop-unroll-and-jam`
* `-fsplit-loops`
* `-funswitch-loops`

但不等于无条件开启完整的 `-funroll-loops`。`-fprofile-use` 会额外开启它。([GCC][2])

### 场景

```cpp
for (int i = 0; i < n; ++i) {
    sum += a[i] * b[i];
}
```

展开四倍后类似：

```cpp
for (; i + 3 < n; i += 4) {
    sum0 += a[i]     * b[i];
    sum1 += a[i + 1] * b[i + 1];
    sum2 += a[i + 2] * b[i + 2];
    sum3 += a[i + 3] * b[i + 3];
}
```

可能增加：

* 指令级并行
* load 并发
* SIMD 机会
* 减少循环跳转

但也可能导致：

* 代码体积膨胀
* I-cache miss
* 寄存器压力增加
* spill 到栈
* 前端取指压力

GCC 官方明确说，该选项可能变快，也可能不变甚至变慢。([GCC][2])

### 建议

不要把它当成固定配置。建立一个独立 benchmark 版本：

```bash
BASE="-O3 -march=native"
TEST="-O3 -march=native -funroll-loops"
```

特别是你手工 SIMD、手工展开或模板展开很多时，再开一次很容易过度展开。

不要使用：

```bash
-funroll-all-loops
```

GCC 官方直接指出它通常会让程序变慢。([GCC][2])

---

## 9. `-fprefetch-loop-arrays`

```bash
-fprefetch-loop-arrays
```

让 GCC 在某些数组循环中生成预取指令。

### 可能适合

```cpp
for (std::size_t i = 0; i < n; ++i) {
    sum += large_array[i];
}
```

或者固定 stride 的访问：

```cpp
for (std::size_t i = 0; i < n; ++i) {
    sum += data[i * 8];
}
```

### 通常不适合

* 数据已在 L1/L2
* 随机 gather
* 访问地址依赖前一次结果
* 工作集很小
* 硬件预取器已经能识别
* 多线程导致内存带宽已经饱和

GCC 文档明确说明它可能生成更好或更差的代码，效果高度依赖循环结构。([GCC][2])

现代 x86 硬件预取器已经很强，所以这个 flag 通常只在确定存在内存访问瓶颈后测试。

---

# 五、SIMD 宽度需要额外调节

## 10. `-mprefer-vector-width=256`

假设 CPU 支持 AVX-512，`-march=native` 允许 GCC 使用 512-bit 指令，但不表示 512-bit 对你的负载一定最快。

可以测试：

```bash
-O3 -march=native -mprefer-vector-width=256
```

可选值包括：

```bash
-mprefer-vector-width=128
-mprefer-vector-width=256
-mprefer-vector-width=512
```

它只是向自动向量器表达宽度偏好。([GCC][1])

### 256 位可能更快的场景

* AVX-512 导致较明显降频
* 前端或 load/store 吞吐是瓶颈
* 代码有大量 mask、shuffle
* 512 位导致寄存器压力过高
* 循环很短，尾部处理成本高

### 512 位可能更快的场景

* 大规模连续数组运算
* 计算密集
* 内存带宽尚未饱和
* CPU 的 AVX-512 频率影响较小
* mask 操作明显受益

建议至少比较：

```text
默认
-mprefer-vector-width=256
-mprefer-vector-width=512
```

看吞吐、频率和 IPC，而不是只看汇编里有没有 `zmm`。

---

# 六、较新的实验性优化

## 11. `-favoid-store-forwarding`

当前 GCC 文档中，这个优化默认关闭：

```bash
-favoid-store-forwarding
```

它尝试避免这种可能导致 store-forwarding stall 的模式：

```cpp
std::uint32_t x;
auto* p = reinterpret_cast<std::uint8_t*>(&x);

p[0] = a;
p[1] = b;
p[2] = c;
p[3] = d;

return x;
```

CPU 先执行多个窄 store，马上执行一个更宽 load，且 store/load 的尺寸或边界不匹配，可能无法高效完成 store-to-load forwarding。

这个 pass 会尝试重组代码规避停顿。它不是通用必开选项，只适合：

* 序列化
* 协议编码
* 位打包
* 写小字段后立刻宽读取
* perf 明确显示 store forwarding stall

GCC 将其标记为默认关闭。([GCC][2])

---

# 七、优化二进制大小和 I-cache

## 12. `-ffunction-sections -fdata-sections` 与 `--gc-sections`

```bash
-ffunction-sections -fdata-sections
-Wl,--gc-sections
```

每个函数、变量放入独立 section，链接器删除最终没有引用的 section。

### 可能收益

* 减小可执行文件
* 减少冷代码
* 改善 I-cache 和加载时间
* 静态链接大型第三方库时尤其明显

### 代价

* `.o` 文件增大
* 编译、汇编和链接变慢
* 某些相对地址优化受到限制
* 不保证运行速度提升

GCC 文档明确说应该在确有收益时使用，因为它可能妨碍编译器和汇编器利用同一翻译单元内的相对位置。([GCC][2])

如果已经开启 LTO，很多未使用代码会在 LTO 阶段被删除，因此额外收益可能没有想象中大。

---

# 八、这些经常被添加，但 `-O3` 已经包括

## 不需要重复添加

### 向量化

```bash
-ftree-vectorize
-ftree-loop-vectorize
-ftree-slp-vectorize
```

现代 GCC 在 `-O2` 就已经开启 loop vectorization 和 SLP；`-O3` 使用更积极的动态成本模型。([GCC][2])

---

### 省略 frame pointer

```bash
-fomit-frame-pointer
```

`-O1` 及以上已经默认开启。([GCC][2])

不过低延迟线上系统为了 `perf` 调用栈质量，有时反而会主动使用：

```bash
-fno-omit-frame-pointer
```

它可能牺牲一点寄存器和函数序言性能，但让线上 profiling 更可靠。这是可观测性与极限性能之间的取舍。

---

### 这些循环优化

以下已经由 `-O3` 开启：

```bash
-floop-interchange
-floop-unroll-and-jam
-fpeel-loops
-fsplit-loops
-ftree-loop-distribution
-funswitch-loops
-fversion-loops-for-strides
-fipa-cp-clone
```

不需要重复写。([GCC][2])

---

### `-mtune=native`

如果已经使用：

```bash
-march=native
```

在普通 x86 CPU 目标上通常已经隐含相应的 `-mtune`。([GCC][1])

但如果你使用的是通用 ISA 等级：

```bash
-march=x86-64-v3
```

它采用通用 tuning，此时可以针对实际部署 CPU 再指定：

```bash
-march=x86-64-v3 -mtune=znver4
```

这样程序仍能运行在所有 x86-64-v3 CPU 上，但代码调度针对 Zen 4 优化。

---

# 九、`-fno-exceptions`、`-fno-rtti` 是否能提速

## `-fno-rtti`

```bash
-fno-rtti
```

主要收益是减少：

* `type_info`
* RTTI 元数据
* 部分二进制尺寸

如果根本没有使用 `dynamic_cast`、`typeid`，通常不会让热循环明显变快。GCC 也主要将其描述为节省空间。([GCC][4])

---

## `-fno-exceptions`

```bash
-fno-exceptions
```

主要收益通常是：

* 减少异常处理元数据
* 减小二进制
* 简化部分代码生成
* 强制项目不依赖 throw/catch

它不是“打开以后普通代码自然快很多”的 flag。

C++ 异常的正常路径本来通常采用 zero-cost 模型；真正抛异常时才非常昂贵。低延迟项目禁用异常，更大的价值往往是：

* 避免热路径意外 throw
* 控制尾延迟
* 简化错误处理策略
* 减少不可预测控制流

不要仅为几个百分点的想象性能收益就贸然关闭，因为第三方库可能仍然抛异常。

---

# 十、推荐的几套配置

## 1. 通用高性能发布版

```bash
-O3 \
-march=native \
-flto=auto \
-DNDEBUG
```

这是比较稳妥的默认起点。

---

## 2. 低延迟内部动态库

```bash
-O3 \
-march=native \
-flto=auto \
-DNDEBUG \
-fvisibility=hidden \
-fno-semantic-interposition
```

可额外 benchmark：

```bash
-fno-plt
```

---

## 3. 数值计算，允许放宽部分语义

比较保守：

```bash
-O3 \
-march=native \
-flto=auto \
-DNDEBUG \
-fno-math-errno \
-fno-trapping-math
```

更激进：

```bash
-Ofast \
-march=native \
-flto=auto \
-DNDEBUG
```

必须执行误差、NaN、Inf、边界输入和结果一致性测试。

---

## 4. 追求峰值性能

第一阶段：

```bash
g++ -O3 -march=native -flto=auto \
    -fprofile-generate=./profile \
    *.cpp -o app
```

运行有代表性的 workload。

第二阶段：

```bash
g++ -O3 -march=native -flto=auto \
    -fprofile-use=./profile \
    *.cpp -o app
```

再分别 benchmark：

```text
+ -ffast-math
+ -funroll-loops
+ -mprefer-vector-width=256
+ -fno-semantic-interposition
```

---

# 最终优先级

按投入产出排序：

| 优先级 | 额外选项                          | 建议                 |
| --- | ----------------------------- | ------------------ |
| 1   | `-flto=auto`                  | 大多数大型 C++ 项目值得开启   |
| 2   | PGO                           | 固定工作负载通常很有价值       |
| 3   | `-DNDEBUG`                    | 确保发布版关闭断言          |
| 4   | `-fno-math-errno`             | 数值计算中相对保守          |
| 5   | `-ffast-math` / `-Ofast`      | 仅在允许改变浮点语义时        |
| 6   | `-fno-semantic-interposition` | 自有 `.so` 很值得测试     |
| 7   | `-fvisibility=hidden`         | 动态库推荐合理控制导出 API    |
| 8   | `-mprefer-vector-width=256`   | AVX-512 平台按 CPU 测试 |
| 9   | `-funroll-loops`              | 必须 benchmark       |
| 10  | `-fprefetch-loop-arrays`      | 只在明确内存瓶颈时测试        |

对于你的计算引擎或 SIMD kernel，最推荐先比较这四组：

```bash
# A
-O3 -march=native

# B
-O3 -march=native -flto=auto

# C
-O3 -march=native -flto=auto -fprofile-use

# D
-O3 -march=native -flto=auto -fprofile-use -ffast-math
```

通常 **LTO + PGO** 比手动堆 `-funroll-loops -fpeel-loops -funroll-all-loops` 更靠谱，因为编译器获得了更多真实信息，而不是被强迫对所有代码采用同一种激进策略。

[1]: https://gcc.gnu.org/onlinedocs/gcc/x86-Options.html "x86 Options (Using the GNU Compiler Collection (GCC))"
[2]: https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html "Optimize Options (Using the GNU Compiler Collection (GCC))"
[3]: https://gcc.gnu.org/onlinedocs/gcc/Code-Gen-Options.html "Code Gen Options (Using the GNU Compiler Collection (GCC))"
[4]: https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Dialect-Options.html "C++ Dialect Options (Using the GNU Compiler Collection (GCC))"
