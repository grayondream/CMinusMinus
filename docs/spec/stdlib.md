# CDD 1.0 — 标准库与内建（stdlib）

本册描述**已实现**（`[impl]`）的标准库表面与语言内建。对应 `TODO.md` 的
P0-05（STD-01 / STD-12 / STD-23 / STD-27）与 DEC-21。

> 与 `print`/`assert`/`panic` 相关的能力是**语言内建**而非可导入的库：它们需要
> 调用点的编译期信息（格式类型、源码位置），而无预处理器（NG-02）意味着不能像 C
> 那样用宏实现。内建与用户同名声明共存时，用户声明优先（见 §1）。

## 1. 内建识别规则 `[impl]`

以下名字在**用户未声明同名函数**时作为内建处理（沿用 `print`/`println` 的策略，
见 `src/sema/SemanticAnalyzer.cpp:visit(CallExprAST)`）：

| 名字 | 形式 | 说明 |
|---|---|---|
| `print` | `print("fmt {}", a, b)` | 格式串字面量 + 若干参数；无换行 |
| `println` | `println("fmt {}", a, b)` | 同上，末尾追加 `\n` |
| `assert` | `assert(cond)` | `cond` 为标量；为假则报错并终止 |
| `panic` | `panic(msg)` | `msg` 为 C 字符串；总是报错并终止 |

命名空间限定名（如经 `namespace std` 导出的函数）**不会**被内建拦截：内建判定发生
在限定名解析之后，因此 `std::panic(...)` 调用的是模块里定义的真实函数。

## 2. `assert(cond)` `[impl]`

- `cond` 必须是**标量**（算术或指针/数组，见 `semantics.md`）；否则为错误。
- 求值 `cond`：为真则继续执行；为假则向 **stderr** 打印
  `"<file>:<line>: assertion failed"` 并调用 C 库 `abort()`。
- `file`/`line` 取**调用点**的位置，在编译期嵌入（INF-03/P0-06）。
- 无 `NDEBUG` 概念（无预处理器），断言**始终启用**。
- 类型为 `void`。

```cdd
int32 main() {
    int32 x = compute();
    assert(x > 0);   // 失败: foo.cdd:3: assertion failed
    return x;
}
```

## 3. `panic(msg)` `[impl]`

- `msg` 必须是 C 字符串（指针/数组）；否则为错误。
- 向 **stderr** 打印 `"<file>:<line>: panic: <msg>\n"`，随后 `abort()`。
- 类型为 `void`；控制流不会返回。

## 4. `abort` `[impl]`

`abort` 是 **C 绑定层**（§5）暴露的 libc 符号，全局可用；`panic`/`assert` 内部即调用
它。它**不**放在 `namespace std` 下：CDD 的非限定名解析优先匹配外层
namespace 前缀，`namespace std { void abort() { abort(); } }` 会自递归，故 `abort`
作为 C 绑定保留在全局作用域。

## 5. `std.c` 绑定层（`libs/std/c.cdd`）`[impl]`

无预处理器下访问 libc 的方式：`extern` 声明 + 链接期解析（MOD-09 / STD-23）。该文件
是权威来源，编译器内嵌同文本作为兜底；默认预置于每个编译单元，`--no-prelude` 关闭。

已声明（节选）：

- 输出/输入：`printf`、`puts`、`putchar`、`getchar`、`scanf`、`dprintf`
- 内存：`malloc`、`calloc`、`realloc`、`free`
- 字符串/内存：`strlen`、`strcmp`、`strncmp`、`strcpy`、`memcpy`、`memset`、`memmove`、`memcmp`
- 进程：`exit`、`abort`
- 文件 I/O（`FILE*` 以不透明 `void*` 传递）：`fopen`、`fclose`、`fread`、`fwrite`、
  `fgets`、`fputs`、`fgetc`、`fputc`、`fprintf`
- 数值：`abs`
- 回调（`MEM-10`）：`qsort`、`bsearch`、`atexit`——`extern` 声明支持函数指针形参，
  用户函数可直接作为回调传入

`dprintf(fd, ...)` 用于访问 stderr（fd = 2），从而**无需暴露 C 的 `FILE` 类型**。

## 6. `std.core` `[impl]`

`import std.core;`，经 `export namespace std` 导出：

| 名字 | 签名 | 说明 |
|---|---|---|
| `std::min` | `compile_time int32 min(int32, int32)` | 较小值 |
| `std::max` | `compile_time int32 max(int32, int32)` | 较大值 |
| `std::clamp` | `int32 clamp(int32 v, int32 lo, int32 hi)` | 区间截断 |

`optional<T>`/`result<T,E>`（STD-02，2026-10-06）由**语言内建**承载：伪字段
访问器 `.valid`/`.value`（optional）、`.ok`/`.value`/`.error`（result）可自由
读写。条件位置直接使用 `if (opt)` / `if (!res)`（真值分别为 `valid` / `ok`）；
另有 `is_some`/`is_none`/`unwrap`/`value_or`（optional）与
`is_ok`/`is_err`/`unwrap`/`value_or`（result）。仍无 `?` 传播（NG-05）。

## 7. `std.io` `[impl]`

`import std.io;`，经 `export namespace std` 导出：

| 组 | 名字 | 签名 |
|---|---|---|
| stdout | `std::print_int` / `std::print_char` / `std::print_str` / `std::print_bool` | `void (int32)` / `void (char)` / `void (const char*)` / `void (int32)` |
| stderr | `std::print_err_str` / `std::print_err_int` | `void (const char*)` / `void (int32)` |
| stdin | `std::read_char` / `std::read_int` / `std::read_line` | `int32 ()` / `int32 ()` / `int32 (char*, int32)` |
| 文件 | `std::file_open` / `std::file_close` | `void* (const char*, const char*)` / `int32 (void*)` |
| 文件 | `std::file_read` / `std::file_write` | `usize (void*, void*, usize)` |
| 文件 | `std::file_read_line` / `std::file_read_char` / `std::file_write_char` / `std::file_write_str` | 见 `libs/std/io.cdd` |

- `read_int` 返回解析值，输入非法时返回 `0`（类似 `atoi`；无异常通道）。
- `read_line`/`file_read_line` 读入缓冲区并保证 NUL 结尾，不含换行。
- 文件句柄是不透明 `void*`（底层 `FILE*`）。

## 8. DEC-21：`panic` 的语义 `[impl]`（已冻结）

- `panic`/`assert` 失败**不可恢复**、**不可捕获**：语言无异常（NG-05）、无栈展开；
  直接调用 `abort()` 终止进程。
- 诊断输出到 stderr，格式固定为 `file:line: panic: <msg>` / `file:line: assertion failed`。
- 与 `[[nonnull]]` 等边界检查属性（MEM-*）的关系：二者都属"安全失败即终止"策略；
  `[[nonnull]]` 违规的默认处理将复用本节的内建终止路径（待实现）。

## 9. `std.string`（P1-09 / STD-10）`[impl]`

**分层**：原语（内建，无需 import）+ 组合（`libs/std/string.cdd`，`import std.string;`）。
原语之所以是内建：`.cdd` 层无法触达 `string_view` 内部字段、也无法从 `ptr+len` 构造切片值。
`string_view`/字符串字面量只隐式转换为 `const char*`；可变 `char*` 需显式转换。

### 9.1 内建原语 `[impl]`
| API | 语义 | 所有权 |
|---|---|---|
| `s.find(needle: string_view\|string) -> isize` | 首现字节下标；无 → `-1`；空 needle → `0` | 视图 |
| `s.rfind(needle) -> isize` | 末现字节下标；无 → `-1`；空 needle → `len` | 视图 |
| `s.sub(begin, end) -> string_view` | 字节区间零拷贝视图 `{ptr+begin, end-begin}` | 视图（见下） |
| `s[begin:end]` | 范围切片语法；对 `string_view`/`string` 得 `string_view`，对数组/切片得 `T[]`；越界 panic | 视图 |
| `s.len` / `s.char_count` | 属性形式；`len()`/`char_count()` 保留为别名 | — |
| `string.data` | 拥有型缓冲裸指针（可写，保证 `data[len] == 0`，生命周期同 `string`） | 借用 |
| `string_view.data` | 只读视图指针（`const char*`） | 借用 |
| `split(s, sep) -> string_view[]` | 贪心非重叠分割；元素为源数据视图；无分隔符 → 全串 1 段 | 数组 malloc（见下） |
| `split_destroy(parts)` | 释放 split 的段数组（不触碰元素） | — |

- `sub` 越界（`begin < 0 || end < begin || end > len`）→ 运行时 panic `string_view.sub: out of bounds`。
- `split` 空 sep：**字面量**在编译期拒绝（E2022 `split: empty separator`）；**动态**运行时 panic 同文案。
- **视图生存期（用户责任）**：`sub`/`split` 的元素引用源数据字节；源 `string` 先 `destroy()` 则视图悬挂
  （与 `string_view` = `string` 视图的所有权模型一致；I4 已收口——`string` 为拥有型值，`string_view`
  非拥有，悬垂仍是用户责任）。`split_destroy` 只释放段数组本身。

### 9.2 组合库 `[impl]`（`import std.string;`）
| API | 语义 | 所有权 |
|---|---|---|
| `std::contains(s, sub) -> bool` | `find >= 0` | — |
| `std::starts_with(s, prefix) -> bool` | `find == 0` | — |
| `std::ends_with(s, suffix) -> bool` | 尾对齐比较 | — |
| `std::trim_left/right/trim(s) -> string_view` | `isspace` 扫描 + `sub` | 视图 |
| `std::join(parts: string_view[], sep) -> string` | 空数组 → 空 string（cap≥1） | **新分配，调用方 destroy** |
| `std::concat(a, b) -> string` | 新分配 | 同上 |
| `std::repeat(s, n) -> string` | `n=0` → 空 string | 同上 |
| `std::utf8_sub(s, start_cp, len_cp) -> string_view` | 码点步进（`char_len_at`）+ `sub` | 视图 |

**字符串构建**：`string` 自身即 builder（`string.new("")` + `append`/`push`/`len`，P1-06）。
**string 拥有型值（I4 收口，2026-10-11）**：赋值/初始化/传参/返回深拷贝活值、移动新鲜值
（`string.new`/函数调用 rvalue）、赋值自动释放旧缓冲——`string` 是 C++ `std::string` 式拥有值，
`string_view`（原 `str`）是非拥有视图。见 `docs/superpowers/specs/2026-10-11-string-ownership-design.md`。
独立 `StrBuilder` 类型暂不提供；`string` 自身即 builder，构造路径统一为
`string.new("")` + `append`/`push`。模块级 `export struct/class/template` 已可用
（容器库即建立在此之上），因此如需独立 builder 可直接在库层实现，无需改编译器。
不做（spec §8）：字典序 `<`/`>`、`replace`；ASCII 大小写转换见 §16 `std.ascii`。

## 10. `format` 与 FMT-08 规格集（P1-09 / STD-11）`[impl]`

`format(fmt: str, args...) -> string` 为内建（user-defined 同名函数优先），
与 `print`/`println` 共享同一规格解析器（`PrintSpec`，`src/ast/PrintFormat.cpp`）。
`format` 返回**新分配 string**（单缓冲一次 `malloc`，无增长路径——长度可预知，
FMT-12 以此满足），调用方 `destroy()`。

### 10.1 规格文法（`{:...}`）`[impl]`
```
spec := [fill] align? sign? '0'? width? ('.' precision)? type?
fill := 除 `{` `}` `:` 数字外的任意字符（仅当后随 align 时生效）
align := '<' | '>' | '^'        sign := '+' | '-' | ' '
width := 1..200                 precision := 0..200
type  := 'x' | 'X' | 'o' | 'b' | 'f' | 'e' | 's'
```
- 语义：`'<'` 左对齐、`'>'` 右对齐、`'^'` 居中（或自定义 fill）走渲染路径；
  `'0'` 补零（仅数值、仅右对齐）；`'+'`/`' '` 符号 flag；`{:.2}` 浮点无 type 推断为 `f`；
  空 `{}` 保持旧行为（FMT-06）：整数 `%d`、浮点 `%g`、str `%.*s`、bool `true/false`。
- 字符串带 width 时**默认左对齐**；数值默认右对齐。
- `{:b}` 依赖 C23 `%b`（glibc ≥ 2.35 基线）。

### 10.2 拒绝矩阵（编译期）`[impl]`
| 诊断 | 条件 |
|---|---|
| E2020 参数计数不匹配 | 占位符数 ≠ 实参数 |
| E2021 规格与实参不符 | `x/X/o/b` 用于非整数；`f/e` 用于非浮点；`s` 用于非字符串类；precision/`0` 用于不支持者；Char/`char*`/指针带任何规格 |
| E2022 规格非法 | 未知 type、`{:d}`（十进制只用 `{}`）、`0` 与 `<`/`^` 冲突、`0` 无 width、align 无 width、width/precision > 200、文法错误 |

### 10.3 实参降级（FMT-10）`[impl]`
无内建转换的类型走 `to_string`（类方法/自由函数两条路径，P1-06 已有）；
仍无可行转换 → 编译期错误 `cannot format value of type ...`。

### 10.4 动态格式串契约（非字面量 fmt）`[impl]`
编译期**只检查实参可格式化**，不解析规格。运行时（`cdd.format.dyn`）：
`{{`/`}}` 转义；`{...}` 消费下一实参并按其静态类型**默认转换**（规格文本被忽略）；
占位符多于实参 → 多余忽略；少于 → 缺位读空串即停；未终止 `{` → 扫描即止。
以上动态行为**不做检查不 panic**，格式串自担（stdlib 边界，spec §2.3.3）。

## 11. `std.mem`（P1-09 / STD-05）`[impl]`

`import std.mem;` 后用 `std::alloc(...)` 等。分配失败返回 `null`（C 语义，调用方
检查），不抛异常不 abort。**后端固定为 mimalloc**（`thirdparty/mimalloc`，静态链接，
`MI_OVERRIDE=ON`）；`std::backend_name()` 返回 `"mimalloc"`。模块自包含 extern 声明。

| 函数 | 语义 |
|---|---|
| `void* alloc(usize size)` | `mi_malloc` |
| `void* alloc_zeroed(usize count, usize size)` | `mi_calloc`（零初始化） |
| `void* resize(void* p, usize size)` | `mi_realloc`（保内容，失败 null 原指针有效） |
| `void  dealloc(void* p)` | `mi_free` |
| `void* bytes_copy/move/set/compare(...)` | libc 字节操作（STD-03，不调元素析构） |
| `usize align_up(usize n, usize align)` | 向上取整到 2 的幂倍数 |
| `usize align_down(usize n, usize align)` | 向下取整到 2 的幂倍数 |
| `bool is_aligned(void* p, usize align)` | 指针按 2 的幂对齐检测 |
| `void* aligned_alloc(usize align, usize size)` | `mi_malloc_aligned`（失败 null） |
| `void  aligned_free(void* p)` | `mi_free` |

`align_up/down/is_aligned` 的 `align` 须为 2 的幂（非 2 的幂行为未定义）；
`aligned_alloc` 的 `align` 必须为 2 的幂（违反返回 null）。

### 11.1 Allocator / Arena / Pool `[impl]`

`libs/std/mem.cdd` 导出以下类型（模块导出类型已可用）：

```cdd
struct Allocator {
    void* (*alloc_fn)(void* ctx, usize size);
    void* (*resize_fn)(void* ctx, void* p, usize size);
    void  (*free_fn)(void* ctx, void* p);
    void* ctx;
};
Allocator allocator_default();
void* allocator_alloc(Allocator*, usize);
void* allocator_resize(Allocator*, void*, usize);
void  allocator_free(Allocator*, void*);

struct Arena { void* base; usize cap; usize used; };
void  arena_init(Arena*, void* base, usize cap);
void* arena_alloc(Arena*, usize size, usize align); // OOB -> null
usize arena_remaining(Arena*);
void  arena_reset(Arena*);
Arena arena_create(usize cap);   // mimalloc-backed
void  arena_destroy(Arena*);

struct Pool { void* free_list; usize block_size; usize block_align; };
void  pool_init(Pool*, usize block_size, usize align);
void* pool_alloc(Pool*);
void  pool_free(Pool*, void* block);
void  pool_destroy(Pool*);       // outstanding blocks must be returned first
```

`Arena` 是调用方提供连续存储的 bump allocator；`arena_create` 便捷地使用
mimalloc 分配底层区。`Pool` 是固定大小自由链表，`pool_destroy` 只释放当前
自由链表上的块——未归还的块由调用方负责。

`std.mem` 在模块级使用 `compile_time.static_assert` 校验
`size_of(usize) == target.pointer_width / 8`。

## 12. `std.bit`（P1-09 / STD-14）`[impl]`

`import std.bit;` 后用 `std::popcount32(...)` 等。**纯语言实现**（无编译器
内建/asm）：SWAR popcount、二分 clz、循环 ctz、移位+或 rotl/rotr、
`reinterpret_cast` bitcast 包装。LLVM 优化可折叠 SWAR 为单条 `popcnt`。

| 函数 | 语义 |
|---|---|
| `int32 popcount8/16/32/64(...)` | 置位个数（SWAR） |
| `int32 clz32(uint32)` / `int32 clz64(uint64)` | 前导零；**对 0 返回位宽**（冻结） |
| `int32 ctz32(uint32)` / `int32 ctz64(uint64)` | 尾随零；**对 0 返回位宽**（冻结） |
| `uint32 rotl32(uint32,int32)` / `uint64 rotl64(...)` | 左循环移位，n 按位宽取模 |
| `uint32 rotr32(uint32,int32)` / `uint64 rotr64(...)` | 右循环移位，n 按位宽取模 |
| `uint32 f32_to_bits(float32)` / `uint64 f64_to_bits(float64)` | 位重解释为无符号整型 |
| `float32 bits_to_f32(uint32)` / `float64 bits_to_f64(uint64)` | 位重解释回浮点 |
| `uint32 i32_to_u32(int32)` / `int32 u32_to_i32(uint32)` | 整型位宽内重解释 |

**前置约束**：语言无 `ull` 后缀（Lexer 只认 `u`/`l`/`ul`/`lu`）；超 int64 范围
的字面量在常量折叠受限，库内 64 位高位掩码经 `ul` 字面量 + 运行时移位构造。

## 13. `std.math`（P1-09 / STD-13）`[impl]`

`import std.math;` 后用 `std::sqrt(...)` 等。libc 绑定（驱动自动追加 `-lm`；
JIT 经进程符号解析）+ `compile_time` 常量 + 位级浮点分类。

| 函数/常量 | 语义 |
|---|---|
| `float64 sqrt(float64)` / `float32 sqrtf(float32)` | 平方根 |
| `float64 fabs(float64)` / `float32 fabsf(float32)` | 绝对值 |
| `float64 floor/ceil/round/trunc(float64)`（`float32` 对应 `floorf/ceilf/roundf/truncf`） | 取整族 |
| `float64 pow(float64,float64)` / `float32 powf(float32,float32)` | 幂 |
| `float64 cos/cosf`、`sin/sinf` | 三角函数 |
| `compile_time float64 PI` / `compile_time float64 E` | 圆周率 / 自然常数 |
| `bool isnan(float64)` / `bool isinf(float64)` / `bool is_finite(float64)` | IEEE754 位级分类（纯语言，不依赖 libc 分类宏） |

**C 符号名说明**：namespace 内 extern C 函数保持 C 裸符号名（`std::sqrt` 链接
到 libm 的 `sqrt`，非 `std_sqrt_float64`）——namespace 前缀仅作访问语法。

## 14. 容器库 `std.vec` / `std.deque` / `std.hashmap` / `std.hashset`（STD-06~08）`[impl]`

容器全部实现在 `libs/std/{vec,deque,hashmap,hashset}.cdd`，**不在编译器内**；
每个容器是独立模块，按需导入：

- `import std.vec;` → `std::Vec<T>`
- `import std.deque;` → `std::Deque<T>`
- `import std.hashmap;` → `std::HashMap<K,V>`
- `import std.hashset;` → `std::HashSet<K>`

使用 C 语义：显式 `init()`/`destroy()`，容器不自动析构元素。元素存储按字节搬迁，
因此元素类型须可重定位（无内部自指针）；拥有型元素（如 `string`）的清理由调用方负责。

| 类型 | 主要 API |
|---|---|
| `Vec<T>` | `init/destroy/reserve/push/pop/get/get_ptr/set/clear/insert/remove/swap/contains/index_of/is_empty/capacity` |
| `Deque<T>` | `init/destroy/push_front/push_back/pop_front/pop_back/get/get_ptr/set/clear/is_empty/capacity` |
| `HashMap<K,V>` | `init(hash,eq)/destroy/put/get/get_ptr/contains/remove/clear/size/is_empty/bucket_count` |
| `HashSet<K>` | `init(hash,eq)/destroy/insert/contains/remove/clear/size/is_empty` |

- `Vec`/`Deque` 容量按 8 起步翻倍；`reserve` 失败保留旧缓冲并返回 `false`。
- `HashMap` 使用开放寻址（0 空/1 活/2 墓碑），负载因子 0.7；扩容重新插入。
- 键/值/元素不自动销毁：`string` 等拥有型元素须在 `destroy()` 前自行清理。
- 哈希/相等通过函数指针参数注入（无 trait）；`std.hash` 提供整数与 `string_view` 适配器。
- 各容器模块只依赖 `std.mem`；`uint8` 状态数组的字节宽度由类型本身保证。
- 四种容器可在 `compile_time` 求值器中执行；`compile_time` 容器变量由 codegen 物化为
  只读常量数组 + 常量结构体，运行时可经正常 `get`/`contains`/`size` 读取。详见
  [`compile_time.md`](./compile_time.md) §7.5。

## 15. `std.hash` / `std.sort`（STD-16/17）`[impl]`

`std.hash`：FNV-1a（`hash_bytes`/`hash_string`）、splitmix64（`hash_u64`/`hash_i64`/
`hash_i32`/`hash_u32`）、`hash_combine`，以及 `hash_key_*`/`equal_*` 回调适配器。

`std.sort`：`sort<T>(T* data, usize len, int32 (*cmp)(T,T))`（稳定插入排序，原地、
无分配）、`sort_slice<T>(T[], cmp)`、`is_sorted`、`binary_search`（返回索引或 -1）。
比较器约定与 C `qsort` 一致：`<0/0/>0`。

## 16. `std.parse` / `std.ascii` / `std.path`（STD-15/18）`[impl]`

`std.parse`：`parse_i64/parse_u64`（2..36 进制、可选 `0x/0b/0o` 前缀）、
`parse_i32`、`parse_bool`、`parse_float64`、`to_string_i64/u64/i32`、
`to_string_float64`。解析器用 out 参数 + `bool` 成功标志，不抛异常。

`std.ascii`：`is_alpha/is_digit/is_alnum/is_space/is_upper/is_lower/is_hex_digit`、
`to_lower/to_upper/to_digit`。仅 ASCII；Unicode 文本走 `std.string` 的 UTF-8 原语。

`std.path`：`separator/basename/dirname/extension/stem/is_absolute/join`。
纯词法操作，`join` 返回新分配的 `string`。分隔符/绝对路径判断通过
`compile_time.if (compile_time.target.os == "windows")` 在编译期选择分支。

## 17. `std.fs` / `std.time` / `std.os` / `std.testing`（STD-19~22）`[impl]`

- `std.fs`：`exists/is_file/file_size/read_file/write_file/append_file/remove_file/
  rename_path/create_dir/current_dir`。路径 `string_view` 内部转为 NUL 终止的
  `string` 再交给 libc；`read_file` 返回拥有型 `string`。
- `std.time`：`unix_seconds/unix_millis/process_ticks/process_seconds/sleep_ms`。
- `std.os`：`env_get/env_set/env_unset/run/pid`。`run` 直接把命令交给 `system`，
  不做 shell 转义；调用方负责命令字符串安全。
- `std.testing`：`fail/expect/expect_eq_i64/expect_eq_str`。失败写 stderr 后 `abort`
  （与语言 `assert`/`panic` 同一不可恢复策略）。

## 18. `std.atomic`（STD-24）`[impl]`

`import std.atomic;` 后用 `std::Atomic<T>` 或按序自由函数。模块名含关键字
`atomic`，驱动已支持关键字作为模块路径分量。

### 18.1 内存序与 API

```cdd
enum MemoryOrder : int32 {
    MemoryOrderRelaxed, MemoryOrderConsume, MemoryOrderAcquire,
    MemoryOrderRelease, MemoryOrderAcqRel, MemoryOrderSeqCst
};
```

- `Atomic<T>` 的**无 order 方法**固定为 seq_cst：`load/store/exchange/
  compare_exchange_weak/compare_exchange_strong/fetch_add/sub/and/or/xor`。
- `_order` 方法在库层按运行时 order 分派：`load_order/store_order/
  exchange_order/fetch_*_order`。非法组合（load 用 Release/AcqRel、store 用
  Acquire/Consume/AcqRel）运行时 `panic`。
- 显式序自由函数按名字固定编译期 order，例如 `atomic_load_acquire`、
  `atomic_store_release`、`atomic_exchange_acq_rel`、`atomic_fetch_add_relaxed`、
  `atomic_compare_exchange_strong_acq_rel`。每族覆盖 relaxed/acquire/release/
  acq_rel/seq_cst（load 另有 consume；store 只允许 relaxed/release/seq_cst）。
- CAS 的任意 success/failure 组合用
  `atomic_compare_exchange_{weak,strong}_explicit<T, Success, Failure>`。
  模板实参须显式给出；v1 用户源码用数值 `0..5`（跨模块 enum 常量不进入主源
  parser 的常量表），常用组合优先用命名包装。
- Fence：`atomic_thread_fence<Order>()`、`atomic_signal_fence<Order>()`，以及
  `*_seq_cst()` 无参便捷函数。

### 18.2 类型与布局

- 支持 1/2/4/8 字节整数、`bool`、指针、`float16/32/64`、底层为 1/2/4/8 字节的
  enum。
- `Atomic<T>` 只有一个 `T` 字段，大小/对齐与 `T` 相同；不加锁字段。
- `bool` 的原子访问经 i8 carrier（LLVM 不接受 `atomic i1`）；float/pointer/enum
  经同宽整数 carrier + bitcast。
- `fetch_add/sub` 支持整数和指针（指针按元素字节数步进）；`fetch_and/or/xor`
  支持非 bool 整数。bool/enum/float 的 RMW 方法是文档化误用：模板方法仍可
  实例化，调用时 `panic`。
- 不支持 16 字节/`int128`/`float128`/聚合；`is_lock_free()`/`is_always_lock_free()`
  在 1/2/4/8 字节返回 `true`，不支持的类型触发 `compile_time.static_assert`。
- 现有 `atomic T` 限定符保持不变（整数标量、seq_cst load/store 与
  atomicrmw 子集）；本模块提供显式序和 CAS/fence。

```cdd
import std.atomic;
std::Atomic<int64> counter = {0};
int32 main() {
    counter.store(1);
    counter.fetch_add(2);                         // seq_cst
    counter.fetch_add_order(3, std::MemoryOrderRelaxed);
    int64 expected = 6;
    return counter.compare_exchange_strong(&expected, 7) ? 0 : 1;
}
```

## 19. `std.thread`（STD-25）`[impl]`

`import std.thread;`。v1 后端固定为 Linux x86-64 pthread；驱动自动追加
`-l pthread`，编译器/JIT 进程由 CMake `Threads::Threads` 链接。

| 名字 | 签名 | 说明 |
|---|---|---|
| `std::Thread` | `struct { usize handle; bool active; bool detached; int32 last_error; }` | 线程所有权对象 |
| `init()` | `void` | 归零状态 |
| `start(fn, arg)` | `bool start(void* (*)(void*), void*)` | 创建线程；失败返回 false |
| `join()` | `bool` | 等待并回收；非 active/已 detach 返回 false |
| `detach()` | `bool` | 分离；成功后再 `join` 返回 false |
| `joinable()` | `bool` | `active && !detached` |
| `get_id()` | `usize` | `pthread_t` 的 usize 载体 |
| `error()` | `int32` | 最近一次 pthread/内部错误码 |
| `std::hardware_concurrency()` | `uint32` | `sysconf(_SC_NPROCESSORS_ONLN)`；失败 0 |
| `std::this_thread_get_id()` | `usize` | `pthread_self` |
| `std::this_thread_yield()` | `void` | `sched_yield` |
| `std::this_thread_sleep_ms(ms)` | `void` | `usleep` 分块，避免乘法溢出 |
| `std::thread_equal(a,b)` | `bool` | `pthread_equal` |

状态机：`init` → `started` → `joined` 或 `detached`。重复 `start`、重复 `join`、
detach 后 `join` 均返回 `false` 并写 `last_error`。`Thread` 无析构函数；离开作用域
前必须 `join` 或 `detach`。`Thread` 拷贝是普通结构体拷贝，不转移所有权。

`thread_local` 在 v1 按每线程副本工作（已由 `StdThreadTest.ThreadLocalIsPerThread`
覆盖）。不提供 mutex/condition_variable/jthread/线程取消；这些是后续条目。
线程入口只接受 `void* (*)(void*)` + 单个 `void*` 参数；错误用 `bool`/
`last_error`，不使用异常。

## 20. 内建与标准库的边界 `[impl]`

- `print`/`println`/`format`/`assert`/`panic` 仍是**语言内建**：它们需要调用点
  的格式串/源码位置，且无预处理器；用户同名声明优先。
- `string`/`string_view`/`optional`/`result`/`slice` 是**语言类型**，其底层表示与
  所有权规则由编译器维护；高层组合 API 在 `libs/std/string.cdd`。
- `std.c` 已完全移至 `libs/std/c.cdd`，编译器**不再内嵌** std.c 文本；缺失标准库
  安装时驱动会显式报错（`--no-prelude` 可关闭注入）。
- 容器、哈希、排序、解析、路径、文件、时间、环境、测试辅助全部在 `libs/std/`。

## 21. 待办 `[plan]`

- `std.mem`：placement 分配、`AlignedAllocator`/可失败分配策略细化。
- `std.vec` / `std.deque` / `std.hashmap` / `std.hashset`：`List`/`BTree`、`Vec::shrink_to_fit`、元素析构/清理辅助。
- `std.unicode`、完整 `std.string` 字典序/`replace`、`std.path::normalize`。
- `std.fs`：目录遍历、`stat` 元数据、原子写入；`std.os`：argv/进程控制。
- `std.testing` + `cdd test` runner（TOOL-01）。
- `std.atomic`：16 字节/`int128`/`float128`、聚合原子、`atomic_ref`/`atomic_flag`、
  compile_time atomic 语义；任意 CAS 组合的可读模板常量写法。
- `std.thread`：mutex/condition_variable、跨平台线程后端、`jthread`/取消、
  多参数/C 捕获 lambda 入口。
- `assert` 的 Release 释放模式若需要，随 DEC-06/07 裁决；当前**始终启用**。
