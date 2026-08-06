可以。下面不再按 GCC 版本罗列，而是按**优化器实际做了什么**分类。括号里的版本表示该能力首次加入或出现显著增强，不代表旧版本完全没有类似优化。

先区分两类：

* **运行性能优化**：让最终程序更快。
* **构建性能优化**：让 LTO、链接或增量编译更快，本身不一定改变程序运行速度。

---

# 一、跨文件与跨函数优化

## 1. LTO：链接时优化

**选项：**

```bash
-flto
-flto=auto
```

普通编译时，每个 `.cpp` 独立优化。LTO 会在链接阶段把多个翻译单元放在一起，执行：

* 跨文件内联
* 跨文件常量传播
* 跨文件去虚拟化
* 未使用函数删除
* 更精确的别名分析
* 跨文件参数优化

### 场景

`math.cpp`：

```cpp
double normalize(double x) {
    return x * 0.01;
}
```

`main.cpp`：

```cpp
extern double normalize(double);

double calculate(double x) {
    return normalize(x) + 1.0;
}
```

没有 LTO，`calculate()` 通常需要调用 `normalize()`。

使用 LTO 后，可能直接变成：

```cpp
double calculate(double x) {
    return x * 0.01 + 1.0;
}
```

### 适合

* 大型 C++ 工程
* 模板与小函数很多
* 计算引擎拆成多个静态库
* 虚函数调用较多
* 热路径跨多个 `.cpp`

### 注意

编译和链接阶段都应传入：

```bash
g++ -O3 -flto=auto -c a.cpp
g++ -O3 -flto=auto -c b.cpp
g++ -O3 -flto=auto a.o b.o -o app
```

LTO 能把静态库中的 GIMPLE 中间表示也纳入全局优化；`-flto=auto` 可利用 Make jobserver 或 CPU 核数并行执行。([GNU 编译器集合][1])

---

## 2. WHOPR：并行、分区式 LTO

WHOPR 可以理解为 GCC 的**可扩展 LTO 执行模式**：

1. 全程序分析。
2. 将程序划分成多个分区。
3. 多线程并行执行局部优化。

### 场景

一个包含数千个 `.cpp` 的大型服务，如果完全串行执行 LTO，链接可能非常慢。WHOPR 会将调用图划分后并行优化。

用户一般不需要直接操作 WHOPR，使用：

```bash
-flto=auto
```

即可。

---

## 3. Incremental LTO：增量链接时优化

**GCC 15：**

```bash
-flto-incremental=/tmp/gcc-lto-cache
```

它缓存 LTO 中间结果。当只修改一个小函数时，不必重新处理整个程序。

### 场景

一个大型 C++ 工程开启 LTO：

```text
修改一个算子
→ 重新编译一个 cpp
→ 但普通 LTO 仍可能重新分析大量代码
```

增量 LTO 会复用未变化翻译单元的优化结果。

> 这是主要改善**开发构建速度**的功能，不直接提高线上运行性能。

GCC 文档明确说明它可显著缩短小范围代码修改后的 LTO 编辑—编译周期。([GNU 编译器集合][2])

---

## 4. 函数内联

**相关选项：**

```bash
-finline-functions
-finline-small-functions
-fpartial-inlining
-findirect-inlining
```

GCC 10 开始，`-finline-functions` 默认进入 `-O2`。

### 场景

```cpp
inline double clamp_max(double x, double max_v) {
    return x > max_v ? max_v : x;
}

double process(double x) {
    return clamp_max(x, 10.0) * 2.0;
}
```

内联后：

```cpp
double process(double x) {
    return (x > 10.0 ? 10.0 : x) * 2.0;
}
```

收益不仅是减少一次 `call/ret`。更重要的是，内联后编译器可以继续做：

* 常量传播
* 分支消除
* SIMD 向量化
* 公共子表达式消除
* 循环外提

### 适合

* 小型 getter
* 算子包装函数
* 模板抽象层
* `std::span` 包装的计算函数
* 热路径中的策略函数

### 风险

过度内联可能：

* 增大代码体积
* 增加指令缓存压力
* 增加寄存器压力

因此 `always_inline` 不应该到处乱贴。GCC 会使用启发式成本模型决定普通函数是否值得内联，并支持只内联函数热区的 partial inlining。([GNU 编译器集合][3])

---

## 5. 去虚拟化 Devirtualization

**相关选项：**

```bash
-fdevirtualize
-fdevirtualize-speculatively
```

### 原始代码

```cpp
struct Operator {
    virtual double run(double x) const = 0;
};

struct AddOne final : Operator {
    double run(double x) const override {
        return x + 1.0;
    }
};

double execute(const Operator* op, double x) {
    return op->run(x);
}
```

正常虚调用需要：

1. 读取对象虚表。
2. 读取函数地址。
3. 间接跳转。

如果 GCC 能证明 `op` 实际上总是 `AddOne`，可以变成：

```cpp
return x + 1.0;
```

### 推测性去虚拟化

如果 99% 的对象是 `AddOne`，但理论上也可能是其他类型，可以生成：

```cpp
if (type_of(op) == AddOne) {
    return x + 1.0;      // 快路径
}
return op->run(x);       // 通用慢路径
```

### 适合

* 算子 DAG
* 虚函数策略模式
* AST 或执行计划节点
* 插件接口
* 大量 `Base*` 调用

GCC 5 显著增强了动态类型判断和推测性去虚拟化；GCC 16 又把类似推测扩展到一般间接函数调用，并支持同时推测多个目标。([GNU 编译器集合][4])

---

## 6. `-fno-semantic-interposition`

Linux 动态库默认允许符号被 `LD_PRELOAD` 或其他动态库替换，这叫 semantic interposition。

```cpp
// libfactor.so
double scale() {
    return 0.01;
}

double calculate(double x) {
    return x * scale();
}
```

在默认语义下，GCC 不能确信 `scale()` 一定是当前文件里的版本，因为运行时可能被替换。因此可能无法内联它。

使用：

```bash
-fno-semantic-interposition
```

后，GCC 可以更积极地：

* 内联共享库内函数
* 常量传播
* 分析函数副作用
* 去虚拟化

### 适合

你完全控制的内部 `.so`：

```bash
g++ -O3 -fPIC -fno-semantic-interposition -shared ...
```

### 不适合

依赖以下机制的库：

* `LD_PRELOAD` hook
* malloc hook
* 外部替换导出函数
* 某些调试或注入框架

该选项允许编译器假设符号即使被覆盖，其语义和副作用也不会改变。([GNU 编译器集合][1])

---

## 7. IPA-CP：跨函数常量传播

**选项：**

```bash
-fipa-cp
-fipa-cp-clone
```

### 场景

```cpp
double calculate(const double* x, int n, bool normalize) {
    for (int i = 0; i < n; ++i) {
        double v = x[i];
        if (normalize)
            v *= 0.01;
        // ...
    }
}
```

调用位置：

```cpp
calculate(data, 10000, true);
```

如果编译器跨函数知道 `normalize == true`，可以消除循环里的判断。

`-fipa-cp-clone` 还可能生成两个专用版本：

```cpp
calculate_normalized(...);
calculate_without_normalization(...);
```

### 适合

* 模板之外仍通过枚举或 bool 配置行为
* 算子模式固定
* 数据类型、维度、步长经常固定
* 执行计划初始化后参数不变

它可能提升性能，但函数克隆会增加代码体积。([GNU 编译器集合][1])

---

## 8. IPA Bit-CP 与 IPA-VRP

### Bit-CP：位级常量传播

```cpp
void process(unsigned flags);
process(MODE_FAST | MODE_ALIGNED);
```

如果 GCC 知道某些 bit 永远为 0 或 1，可以消除：

```cpp
if (flags & MODE_SAFE) {
    // 永远不会执行
}
```

它还能传播指针对齐信息，让向量器少生成一些对齐检查。

### VRP：取值范围传播

```cpp
void process(int depth) {
    if (depth > 16)
        slow_path();

    array[depth] = 1;
}
```

如果调用图分析证明：

```text
0 <= depth <= 7
```

则可以：

* 删除 `depth > 16`
* 优化边界检查
* 简化 switch
* 帮助循环展开
* 帮助函数克隆

GCC 7 增加了跨过程 bit 常量传播和 value-range propagation；它们在现代 GCC 的 `-O2` 中属于重要 IPA 优化。([GNU 编译器集合][5])

---

## 9. IPA-SRA：跨函数聚合体标量替换

### 场景

```cpp
struct Input {
    double price;
    double volume;
    double timestamp;
    double unused[20];
};

double calculate(Input input) {
    return input.price * input.volume;
}
```

虽然传入了整个 `Input`，函数只使用两个字段。IPA-SRA 可能等价地改成：

```cpp
double calculate(double price, double volume);
```

甚至删除未使用的返回值字段：

```cpp
struct Result {
    double value;
    double diagnostics[10];
};
```

调用者只读取 `value` 时，GCC 可以避免计算或传递 `diagnostics`。

### 适合

* 大型配置结构体
* 算子参数对象
* Tuple 或 Result 包装
* 跨模块传递的聚合类型

GCC 10 重写了 IPA-SRA，使其能在 LTO 下工作，并能删除未使用返回值的计算。([GNU 编译器集合][3])

---

## 10. IPA-modref、pure/const 分析

GCC 分析函数究竟：

* 读取哪些内存
* 修改哪些内存
* 是否只有返回值依赖输入
* 是否具有可观察副作用

### 场景

```cpp
double get_scale();

for (int i = 0; i < n; ++i) {
    out[i] = in[i] * get_scale();
}
```

如果 GCC 证明 `get_scale()` 不修改内存，并且每次结果一样，就可以把它移出循环：

```cpp
double scale = get_scale();

for (int i = 0; i < n; ++i) {
    out[i] = in[i] * scale;
}
```

### 适合

* 数学函数
* 只读配置查询
* DAG 节点属性读取
* 纯计算工具函数

GCC 11 的 IPA-modref 会跨函数跟踪副作用，并提高 points-to 和别名分析精度。([GNU 编译器集合][6])

---

## 11. ICF：相同代码折叠

**选项：**

```bash
-fipa-icf
```

### 场景

模板实例化可能产生大量语义相同的函数：

```cpp
int foo_a(int x) { return x + 1; }
int foo_b(int x) { return x + 1; }
```

ICF 可以让它们共享同一份机器代码。

### 收益

主要是：

* 减小二进制
* 降低指令缓存压力
* 减少静态库模板膨胀
* 缩短动态加载时间

### 适合

* 模板密集项目
* 自动生成代码
* 多个相似算子类型
* 大量 wrapper

ICF 在 `-O2` 和 `-Os` 默认启用，配合 LTO 通常更有效。([GNU 编译器集合][1])

---

## 12. Allocation DCE：无效分配消除

**相关选项：**

```bash
-fallocation-dce
-fmalloc-dce
```

### 场景

```cpp
void test() {
    auto* p = new int[100];
    delete[] p;
}
```

如果分配对象没有任何可观察用途，整个 `new/delete` 可以被删除。

也可能优化：

```cpp
void* p = malloc(100);
free(p);
```

### 适合

* 临时 RAII 包装
* 内联后暴露出的无用分配
* 泛型代码中某条分支最终没有使用对象

GCC 10加入了无用 `new/delete` 对消；当前 GCC 还可以在满足约束时消除对应的 `malloc/free`。([GNU 编译器集合][3])

---

# 二、基于真实运行行为的优化

## 13. PGO：Profile-Guided Optimization

典型流程：

```bash
# 第一步：插桩
g++ -O3 -fprofile-generate *.cpp -o app

# 第二步：用典型负载运行
./app representative_data.bin

# 第三步：根据采样结果重新编译
g++ -O3 -fprofile-use *.cpp -o app
```

PGO 会告诉 GCC：

* 哪些函数最热
* 哪些分支最常走
* 哪些循环迭代次数多
* 间接调用最常指向哪个函数
* 哪些路径从未执行
* 哪些函数应当内联

### 场景

```cpp
if (unlikely_error) {
    complicated_error_handling();
} else {
    hot_calculation();
}
```

普通编译器只能猜测概率。PGO 可以根据真实数据把热路径放成 fall-through，并把冷路径移到单独区域。

PGO 会打开一批通常只有掌握真实运行频率才安全或划算的优化，包括循环展开、peeling、函数克隆、tracer、向量化以及更积极的 loop transformation。([GNU 编译器集合][1])

---

## 14. AutoFDO：基于 `perf` 采样的反馈优化

传统 PGO 需要重新编译插桩版本。AutoFDO 使用硬件采样数据，例如：

```bash
perf record -b ./app
```

再将 profile 转换给 GCC：

```bash
-fauto-profile=profile.afdo
```

### 适合

* 插桩影响延迟，无法接受
* 线上服务
* 低延迟系统
* 使用真实生产流量优化
* 大型程序插桩成本太高

### 局限

采样精度通常不如完整插桩 PGO，而且需要合理的符号和 profile 转换流程。

GCC 5 引入 AutoFDO，允许使用低开销采样 profile，而不是仅依赖插桩计数。([GNU 编译器集合][4])

---

## 15. 分支概率、热冷路径分区

PGO 可以把：

```cpp
if (error) {
    log_error();
    recover();
}
fast_path();
```

布局成：

```text
fast_path 指令连续存放
error 路径放到冷代码区
```

收益包括：

* 减少热路径跳转
* 减少 I-cache 污染
* 提升指令预取效果
* 减少冷代码占据 cache line

x86 上热冷代码分区从 GCC 8 起在 `-O2` 默认启用；PGO 会进一步提供更准确的分支频率。([GNU 编译器集合][7])

---

## 16. 间接调用目标与值分析

### 间接调用

```cpp
using Fn = double (*)(double);
Fn fn = select_function(config);

for (...) {
    result += fn(x[i]);
}
```

Profile 发现 `fn` 绝大多数时候是 `fast_fn`，可能生成：

```cpp
if (fn == fast_fn) {
    // 内联或直接调用 fast_fn
} else {
    fn(...);
}
```

### Value Profile

```cpp
result = x / divisor;
```

如果 profile 显示 `divisor` 几乎总是 10，GCC 可以为常见值生成专用路径，使用更便宜的计算。

这类 profile 信息也会参与函数重排、间接调用优化和热路径识别。([GNU 编译器集合][6])

---

# 三、循环与 SIMD 优化

## 17. Loop Vectorization：循环向量化

```cpp
for (std::size_t i = 0; i < n; ++i) {
    out[i] = a[i] + b[i];
}
```

在 AVX2 下可能一次计算 4 个 `double`：

```text
load 4 doubles
load 4 doubles
vaddpd
store 4 doubles
```

### 适合

* 数组逐元素计算
* reduction
* 归一化
* clamp
* 加减乘除
* 连续内存扫描

GCC 12 开始，循环向量化在 `-O2` 默认启用，并使用较保守的 `very-cheap` 成本模型；`-O3` 使用更积极的动态成本模型。([GNU 编译器集合][8])

---

## 18. SLP：直线代码向量化

SLP 不要求存在显式循环。

```cpp
r0 = a0 + b0;
r1 = a1 + b1;
r2 = a2 + b2;
r3 = a3 + b3;
```

可能合并成一个向量加法。

### 场景

* 手工展开的代码
* 固定尺寸矩阵
* 同时计算多个树
* 多个独立特征归一化
* AoS 字段的并行计算

GCC 11 起，SLP 会在整个函数范围寻找机会，甚至跨 CFG 合流点和回边，而不再只局限于简单基本块。([GNU 编译器集合][6])

---

## 19. If-conversion：分支转无分支计算

```cpp
for (int i = 0; i < n; ++i) {
    if (x[i] > 0)
        y[i] = x[i];
    else
        y[i] = 0;
}
```

可能转换成：

```cpp
y[i] = max(x[i], 0);
```

或者 SIMD mask/blend。

### 适合

* clamp
* ReLU
* 条件赋值
* 树模型节点比较
* 简单二选一

### 不一定有利

如果分支高度可预测，并且另一条路径计算非常昂贵，无分支化可能反而做了更多工作。

GCC 的 loop if-conversion 主要目的是去除内层循环控制流，为后续向量化创造条件。([GNU 编译器集合][1])

---

## 20. Early-break 循环向量化

```cpp
for (int i = 0; i < n; ++i) {
    if (x[i] < 0)
        break;
    sum += x[i];
}
```

旧向量器通常很难处理 `break`，因为每个 SIMD lane 可能在不同位置触发退出。

新版本会：

1. 并行比较一组元素。
2. 形成退出 mask。
3. 找到第一个触发退出的位置。
4. 只累计退出前的元素。

GCC 15 增强了未知输入长度情况下的 early-exit 向量化；GCC 16 又改善了 early-break 代码生成。([GNU 编译器集合][2])

---

## 21. 未知循环次数与 uncounted loop 向量化

```cpp
while (*p != sentinel) {
    sum += *p;
    ++p;
}
```

这种循环没有显式 `n`，旧版本很难提前确定 trip count。

GCC 16 的向量器开始支持：

* 无法确定迭代次数的循环
* uncounted loops
* 更复杂的 reduction 并行性
* 可变向量长度架构上的对齐 peeling

适合 sentinel 扫描、解析器、字符串或流式数据处理，但是否成功仍强烈依赖循环结构。([GNU 编译器集合][9])

---

## 22. Loop Interchange：循环交换

### 原始代码

```cpp
for (int j = 0; j < M; ++j) {
    for (int i = 0; i < N; ++i) {
        sum += matrix[i][j];
    }
}
```

如果是 C/C++ 行主序，内层访问跨行跳跃，缓存局部性差。

交换后：

```cpp
for (int i = 0; i < N; ++i) {
    for (int j = 0; j < M; ++j) {
        sum += matrix[i][j];
    }
}
```

现在内层连续访问。

### 适合

* 矩阵运算
* 多维数组
* 图像处理
* 批量特征数据
* `batch × feature` 布局转换

默认属于 `-O3` 优化，也可能由 PGO/AutoFDO 开启。([GNU 编译器集合][1])

---

## 23. Loop Distribution：循环拆分

### 原始代码

```cpp
for (int i = 0; i < n; ++i) {
    a[i] = b[i] + 1;
    c[i] = expensive_function(d[i]);
}
```

可能拆成：

```cpp
for (int i = 0; i < n; ++i)
    a[i] = b[i] + 1;

for (int i = 0; i < n; ++i)
    c[i] = expensive_function(d[i]);
```

### 收益

第一个循环变得非常规整，可能更容易：

* SIMD
* 并行化
* 预取
* 识别成库函数

### 风险

拆成两次遍历会增加内存流量，因此编译器需要成本判断。

Loop distribution 在 GCC 8 得到增强，并在 `-O3` 默认启用。([GNU 编译器集合][7])

---

## 24. Loop Pattern Recognition

```cpp
for (int i = 0; i < n; ++i) {
    data[i] = 0;
}
```

可能直接变成：

```cpp
memset(data, 0, n * sizeof(data[0]));
```

或者将一部分初始化循环拆出去，再识别成 `memset`。

### 适合

* 数组清零
* 内存复制
* 固定字节填充
* 初始化缓冲区

该 pattern distribution 在 `-O2` 及以上默认启用。([GNU 编译器集合][1])

---

## 25. Unroll-and-Jam

### 原始嵌套循环

```cpp
for (int i = 0; i < N; ++i) {
    for (int j = 0; j < M; ++j) {
        c[i][j] += a[i] * b[j];
    }
}
```

外层展开两次后，把两个内层循环融合：

```cpp
for (int i = 0; i < N; i += 2) {
    for (int j = 0; j < M; ++j) {
        c[i][j]     += a[i]     * b[j];
        c[i + 1][j] += a[i + 1] * b[j];
    }
}
```

### 收益

* `b[j]` 加载可被复用
* 增加指令级并行
* 方便 SIMD
* 减少内层循环控制开销

### 风险

* 寄存器压力增大
* 代码膨胀
* 可能出现 spill

默认属于 `-O3`。([GNU 编译器集合][7])

---

## 26. Loop Unrolling：循环展开

```cpp
for (int i = 0; i < 8; ++i)
    sum += x[i];
```

可能展开为八条加法，完全删除循环控制。

对于较长循环，也可能按 4 倍展开：

```cpp
for (; i + 3 < n; i += 4) {
    sum += x[i];
    sum += x[i + 1];
    sum += x[i + 2];
    sum += x[i + 3];
}
```

### 适合

* 固定小循环
* 循环体很小
* 分支和计数开销占比较大
* 需要暴露 ILP 或 SIMD 机会

### 风险

* 指令缓存压力
* 寄存器不够导致 spill
* 大循环体反而变慢

GCC 文档也明确提醒，`-funroll-loops` 会增大代码，并不保证一定更快；`-funroll-all-loops` 往往更加危险。([GNU 编译器集合][1])

---

## 27. Loop Peeling

将循环前几次迭代单独取出来：

```cpp
// 先单独处理若干元素，使 p 对齐
while (!aligned(p)) {
    process(*p++);
}

for (...) {
    vectorized_process(p);
}
```

### 场景

* 处理数组头部，使后续地址 SIMD 对齐
* 小循环完全展开
* 首次迭代行为特殊
* PGO 显示循环通常只执行少数几次

`-fpeel-loops` 默认属于 `-O3`，也会由 PGO 和 AutoFDO 启用。([GNU 编译器集合][1])

---

## 28. Loop Splitting

```cpp
for (int i = 0; i < n; ++i) {
    if (i < threshold)
        a[i] = fast_a(i);
    else
        a[i] = fast_b(i);
}
```

可以拆成：

```cpp
for (int i = 0; i < threshold; ++i)
    a[i] = fast_a(i);

for (int i = threshold; i < n; ++i)
    a[i] = fast_b(i);
```

### 收益

循环内部不再每次判断，更容易向量化。

GCC 7 增加了专门的 loop splitting pass；现代 GCC 在 `-O3` 或 profile 驱动优化中使用它。([GNU 编译器集合][5])

---

## 29. Loop Unswitching

```cpp
for (int i = 0; i < n; ++i) {
    if (normalize)
        out[i] = in[i] * scale;
    else
        out[i] = in[i];
}
```

如果 `normalize` 在整个循环期间不变，可以变成：

```cpp
if (normalize) {
    for (int i = 0; i < n; ++i)
        out[i] = in[i] * scale;
} else {
    for (int i = 0; i < n; ++i)
        out[i] = in[i];
}
```

### 收益

* 循环内删除分支
* 两个循环都更容易 SIMD
* 热分支可进一步专用化

### 风险

复制循环会增大代码体积。

默认属于 `-O3`。([GNU 编译器集合][1])

---

## 30. Loop-Invariant Code Motion

```cpp
for (int i = 0; i < n; ++i) {
    out[i] = in[i] * std::sqrt(scale);
}
```

如果 `scale` 不变，可以变成：

```cpp
double s = std::sqrt(scale);

for (int i = 0; i < n; ++i) {
    out[i] = in[i] * s;
}
```

还包括 store motion：如果某个写入在循环中重复，但中间不可观察，可以只在循环结束后写一次。

### 适合

* 配置读取
* 固定数学计算
* 指针地址计算
* 不变条件
* 重复写状态变量

它依赖精确的副作用和别名分析；函数调用不透明时可能无法外提。([GNU 编译器集合][1])

---

## 31. Loop Versioning for Strides

```cpp
for (int i = 0; i < n; ++i) {
    out[i] = input[i * stride];
}
```

GCC 可以生成两个版本：

```cpp
if (stride == 1) {
    // 连续访问，可向量化
    for (int i = 0; i < n; ++i)
        out[i] = input[i];
} else {
    // 通用版本
    for (int i = 0; i < n; ++i)
        out[i] = input[i * stride];
}
```

### 适合

* `std::mdspan`
* 张量切片
* 列主序/行主序兼容接口
* 数据视图
* 动态 stride 数组

默认属于 `-O3`，也可由 PGO 或 AutoFDO 开启。([GNU 编译器集合][1])

---

# 四、标量、控制流与机器码级优化

## 32. Store Merging

```cpp
struct Header {
    unsigned char a;
    unsigned char b;
    unsigned char c;
    unsigned char d;
};

void init(Header& h) {
    h.a = 1;
    h.b = 2;
    h.c = 3;
    h.d = 4;
}
```

原本可能是四条 byte store，合并后可能成为一次更宽的 store。

### 适合

* 结构体初始化
* 协议头填写
* 小数组写常量
* 状态表初始化

GCC 7 增加了 store merging pass，当前在 `-O2` 及以上默认启用。([GNU 编译器集合][5])

---

## 33. Code Hoisting、PRE、FRE

```cpp
if (condition) {
    result = expensive(x) + a;
} else {
    result = expensive(x) + b;
}
```

可以变成：

```cpp
auto temp = expensive(x);

if (condition)
    result = temp + a;
else
    result = temp + b;
```

### PRE

某个表达式只在部分路径上已经计算过，编译器适当移动计算，使后面不必重复。

### FRE

表达式在所有到达路径上都已经计算过，可以直接复用结果。

### 场景

* 分支中重复计算地址
* 相同字段读取
* 重复数学表达式
* 多条控制流最终执行相同计算

Code hoisting 和 PRE 在 `-O2` 开始启用，更激进的 partial PRE 属于 `-O3`。([GNU 编译器集合][5])

---

## 34. Jump Threading

```cpp
if (x > 0) {
    // ...
}

if (x <= 0) {
    slow_path();
}
```

进入第二个判断时，控制流已经知道 `x > 0`，因此第二个判断结果已确定，可以直接跳过。

### 适合

* 多层条件判断
* 状态机
* 解析器
* switch 展开后的控制流
* 大量 guard checks

Jump threading 在 `-O1` 及以上启用。([GNU 编译器集合][1])

---

## 35. Switch Conversion 与 Bit-test

```cpp
bool is_space(int c) {
    return c == 9 || c == 10 || c == 12 ||
           c == 13 || c == 32;
}
```

GCC 可能转换为：

* switch
* bit mask test
* jump table
* 决策树

线性 switch：

```cpp
switch (x) {
case 2: return 205;
case 3: return 305;
case 4: return 405;
}
```

甚至可能被转换成类似：

```cpp
return 100 * x + 5;
```

### 适合

* 状态枚举
* 字符分类
* opcode dispatch
* 协议解析
* 小整数集合判断

GCC 9 改善了 switch 策略选择；GCC 11 增强了连续比较到 switch/bit-test 的转换。([GNU 编译器集合][10])

---

## 36. LRA Rematerialization

当寄存器不够时，一般会把值 spill 到栈上：

```text
store register → stack
...
load stack → register
```

如果该值很容易重新计算，例如：

```cpp
ptr = base + index * 8;
```

编译器可能选择重新计算，而不是访问栈：

```text
lea ...
```

### 适合

* 寄存器压力高
* 大量地址表达式
* 手工循环展开
* SIMD kernel
* 内联后函数很大

`-flra-remat` 会在重新加载 spill 值和重新计算之间做成本判断，从 GCC 5 起得到明显增强，目前在 `-O2` 及以上启用。([GNU 编译器集合][4])

---

## 37. IPA Register Allocation

假设调用者有一个值保存在 caller-saved 寄存器里。正常情况下调用函数前后可能需要保存恢复。

如果 GCC 知道被调函数根本不使用该寄存器，就可以避免：

```text
push/save
call
pop/restore
```

### 场景

* 小函数调用频繁
* 同一编译单元内的计算函数
* LTO 让调用者和被调者同时可见
* 热循环里存在无法内联的调用

`-fipa-ra` 在能够准确知道被调函数寄存器使用情况时减少调用附近的保存恢复。([GNU 编译器集合][1])

---

## 38. Shrink Wrapping

### 原始概念

传统函数一进入，就执行完整序言：

```text
保存寄存器
分配栈空间
```

但可能只有错误路径需要大量栈空间：

```cpp
int process(int x) {
    if (x >= 0)
        return x + 1;           // 热路径，非常简单

    LargeObject temporary;     // 冷路径
    return slow_process(x, temporary);
}
```

Shrink wrapping 会尽量让热路径不执行不需要的：

* 寄存器保存
* 栈帧构建
* 冷路径资源准备

GCC 还可以分别移动序言和尾声的不同组成部分，但需要目标架构支持。([GNU 编译器集合][5])

---

## 39. 指令调度

如果代码存在加载延迟：

```text
load A
立即使用 A
load B
立即使用 B
```

调度器可能改成：

```text
load A
load B
执行其他独立指令
使用 A
使用 B
```

目的是：

* 隐藏 load latency
* 隐藏浮点运算延迟
* 增加流水线并行
* 降低数据依赖造成的 stall

不过是否启用、收益大小高度依赖架构。例如 x86 本身有强大的乱序执行，而 AArch64 上编译期调度可能更重要。GCC 的 `-fschedule-insns` 和第二阶段调度会根据目标后端决定是否启用。([GNU 编译器集合][1])

---

## 40. `-march` 与 `-mtune`

这不是中端优化 pass，但对最终性能非常重要。

```bash
-march=native
```

允许使用当前 CPU 支持的：

* AVX2
* FMA
* BMI
* AVX-512
* VNNI
* 其他 ISA 扩展

```bash
-mtune=native
```

不改变程序允许使用的指令集，但针对当前 CPU 调整：

* 指令选择
* 展开成本
* 调度模型
* 对齐策略
* 指令延迟和吞吐估计

### 区别

```text
-march：决定“能使用什么指令”
-mtune：决定“这些指令如何安排更划算”
```

`-march` 生成的程序可能无法运行在更旧的 CPU 上，而 `-mtune` 通常保持兼容性。([GNU 编译器集合][11])

---

# 五、`-Ofast`：放宽数学语义换性能

```bash
-Ofast
```

它不仅是“比 `-O3` 更高一级”，而是允许违反部分严格语言和 IEEE 浮点语义。

例如：

```cpp
double f(double a, double b, double c) {
    return (a + b) + c;
}
```

严格浮点下不能随意变成：

```cpp
a + (b + c);
```

因为浮点加法不满足严格结合律。

`-Ofast` 允许：

* 浮点重结合
* 假设没有 NaN/Inf
* 忽略 signed zero 差异
* 使用倒数乘法替代除法
* 更积极地向量化 reduction
* 不维护部分数学函数的 `errno`

### 适合

* 机器学习推理
* 图像处理
* 允许少量误差的数值计算
* 统计计算
* 某些量化因子批量计算

### 谨慎使用

* 金融账务结果
* 依赖 NaN 传播
* 依赖 `+0.0/-0.0`
* 修改浮点舍入模式
* 要求逐 bit 可复现
* 对累计误差敏感

GCC 文档明确指出 `-Ofast` 会启用不适用于所有标准兼容程序的优化，包括 `-ffast-math`、关闭 semantic interposition，以及放宽部分并发和浮点假设。([GNU 编译器集合][1])

---

# 六、对高性能 C++ 工程最有实际价值的组合

## 通用发布版本

```bash
-O3 -march=native -mtune=native
```

适合本机部署，不要求运行在其他旧 CPU 上。

## 加入跨文件优化

```bash
-O3 -march=native -flto=auto
```

对以下代码尤其有效：

* 大量小函数
* 多个静态库
* 虚函数
* 模板抽象
* 算子调用链

## 内部共享库

```bash
-O3 -march=native -flto=auto -fno-semantic-interposition
```

前提是不依赖动态符号替换。

## 数值语义允许放宽

```bash
-Ofast -march=native -flto=auto
```

必须先做正确性误差测试。

## 追求最终峰值性能

```bash
-O3 -march=native -flto=auto -fprofile-generate
# 运行真实负载
-O3 -march=native -flto=auto -fprofile-use
```

对于执行引擎、树模型推理或固定工作负载，通常优先级可以排成：

```text
数据布局
  > 算法与缓存局部性
  > 去除 gather / 随机访问
  > PGO
  > LTO 与去虚拟化
  > 自动向量化
  > 单个细粒度编译选项
```

也就是说，升级 GCC 能帮助优化已有结构，但不会神奇地把随机 gather、严重 cache miss 或错误的数据布局变成高吞吐代码。

[1]: https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html "Optimize Options (Using the GNU Compiler Collection (GCC))"
[2]: https://gcc.gnu.org/gcc-15/changes.html "GCC 15 Release Series — Changes, New Features, and Fixes - GNU Project"
[3]: https://gcc.gnu.org/gcc-10/changes.html "GCC 10 Release Series — Changes, New Features, and Fixes - GNU Project"
[4]: https://gcc.gnu.org/gcc-5/changes.html "GCC 5 Release Series — Changes, New Features, and Fixes - GNU Project"
[5]: https://gcc.gnu.org/gcc-7/changes.html "GCC 7 Release Series — Changes, New Features, and Fixes - GNU Project"
[6]: https://gcc.gnu.org/gcc-11/changes.html "GCC 11 Release Series — Changes, New Features, and Fixes - GNU Project"
[7]: https://gcc.gnu.org/gcc-8/changes.html "GCC 8 Release Series — Changes, New Features, and Fixes - GNU Project"
[8]: https://gcc.gnu.org/gcc-12/changes.html "GCC 12 Release Series — Changes, New Features, and Fixes - GNU Project"
[9]: https://gcc.gnu.org/gcc-16/changes.html "GCC 16 Release Series — Changes, New Features, and Fixes - GNU Project"
[10]: https://gcc.gnu.org/gcc-9/changes.html "GCC 9 Release Series — Changes, New Features, and Fixes - GNU Project"
[11]: https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/x86-Options.html?utm_source=chatgpt.com "x86 Options"
