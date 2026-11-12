# P1-09 设计：字符串库（STD-10）与 format 体系（STD-11 / FMT-06~11）

日期：2026-10-09
状态：已批准（brainstorming 四节设计用户逐节批准；四问裁决见 §0）
上游：P1-06（str/string 内建类型，spec `2026-10-08-str-string-design.md`）

## §0 已批准的决策

| 决策 | 结论 |
|------|------|
| D1 范围 | 一轮做完 STD-10 + STD-11（FMT-06~09 核心 + FMT-10/11；FMT-12 以「单缓冲增长」最低限度满足） |
| D2 format 架构 | 内建 + 统一 spec 解析器（print/println/format 三者共享），非纯库 varargs |
| D3 split 返回 | `[]str` 零拷贝视图切片 + `split_destroy` 显式释放（string.new/destroy 同一所有权模式） |
| D4 可见性 | `format()` 与 print/println 同为无 import 内建；`import std.format` 只引入附加工具与文档（FMT-06 的「纳入体系」钉死为文档归属 + spec 共享，非门控） |
| D5 分层 | str 内部字段不可从 .cdd 触及 → 内建只加 `find`/`sub` 两原语，其余组合进 `.cdd` |

## §1 术语与不变式

- 所有新 API 以 `str`（不拥有）为输入优先；产生新内存的 API 返回 `string` 或要求显式释放（DEC-20 所有权模型不变）。
- 所有字节下标为**字节偏移**（非码点下标）；UTF-8 语义仅经码点步进 API 暴露。
- 运行时无格式错误（全部编译期拒绝，§4）；运行时唯一 abort 语义沿用 panic 既有路径（str_from_c 的 UTF-8 验证，不在本轮改动）。
- `format`/`print`/`println` 内建在用户以同名声明函数时让位（与 print 既有 user-defined 优先规则一致）。

## §2 format 内建（STD-11 / FMT-06~11）

### §2.1 签名与可见性

```
format(fmt: str, args...) -> string
```

- 内建函数，无 import 直用（D4）；用户同名声明优先（沿用 assert/panic/print 的 user-defined 查找先例）。
- 返回 `string`：调用者拥有，负责 `destroy()`（DEC-20）。**不返回 str**（无静态缓冲可回）。

### §2.2 规格说明语法（FMT-08，钉死集合）

```
placeholder   = '{{' | '}}' | '{' [ format-spec ] '}'
format-spec   = [ fill-align ] [ sign ] [ '0' ] width [ '.' precision ] type
fill-align    = (任意非 ':' 单字符 + '>' | '<' | '^')     ;; 仅支持 '>' '<' 两种对齐 + '^' 居中
width         = 1..DIGIT
precision     = 1..DIGIT
type          = '' | 'x' | 'X' | 'o' | 'b' | 'f' | 'e' | 's'
```

- `{}` 等价 `{:s}`；`{:02}` = 零填充宽度 2；`{:.2f}` = 精度 2 定点；`{:>8}` = 右对齐宽 8（整数默认右对齐，字符串默认左对齐）。
- type 省略时按实参类型推断（整数→d 十进制，浮点→f，bool→true/false，char→字符，str→字符串，指针→十六进制）。
- `{:b}` 二进制、`{:o}` 八进制、`{:x}`/`{:X}` 十六进制（仅整数实参合法，浮点/字符串实参 → 编译期错误）。
- `{:f}`/`{:e}` 仅浮点实参合法（整数实参 → 编译期错误；如需浮点先显式 cast）。
- `{^N}` 居中对齐：本轮支持（snprintf 无原生居中，codegen 生成 pad 循环或经 `%*s` + 补丁——实现时取更简者，结果语义钉死为居中）。
- `{{`/`}}` 转义为字面 `{`/`}`。
- 规格集合之外的一切（`{:d}` 显式 d、`{:n}`、动态宽度 `{:*}`、`{0}` 位置参数、`{:?}` debug）→ 编译期错误（明确不在本轮）。

### §2.3 编译期检查（FMT-09 / SEM-08，E 码族）

sema 在 visit(CallExprAST) 的 format 内建分支：

1. 第一实参必须是 `str` 类型（含字面量；非 str → 现有类型不匹配诊断）。
2. 字面量格式串 → **立即全量解析**（复用扩展后的 buildPrintFormat）：占位符数与实参数不符 → 错误；type 与实参类型不符（§2.2 矩阵）→ 错误；未知/非法 spec → 错误；非法转义 → 错误。
3. 非字面量格式串（运行时 str）：本轮**不做编译期检查也不做运行时检查**——`format(dynamic_str, args...)` 合法，规格在 codegen 期按实参静态类型逐一转译，spec 错误语义 = 未定义（文档明示「动态格式串规格自担」）；占位符数不匹配的运行时行为 = 多余实参忽略、缺失占位符读到空串即停（钉死为良性，不 panic）。
4. 新诊断码：`E2020` 格式串与实参数不符；`E2021` 规格与实参类型不符；`E2022` 非法/未知格式规格；（码位若与现有冲突则在实现时顺延，注册时核对）。

### §2.4 codegen 组装

- 本地 string：`malloc(INIT_CAP)`（INIT_CAP = 格式串字面长度 ×2 + 64，下限 64）。
- 字面量片段：memcpy 追加；占位符：按 spec 生成 C 格式串 → `snprintf` 进栈缓冲（如 `i32` + `{:02x}` → `snprintf(buf, 64, "%02x", v)`）→ 追加；`str` 实参直接 memcpy 字节（不经 NUL）；`string` 实参投影视图后同 str。
- 全程单一缓冲，满则 `cap' = max(cap*2, need)` realloc（复用 FMT-02 增长策略）。
- 返回 `{ptr, len, cap}` string 值。
- print/println 改造：走同一 spec 解析器输出 C 格式串（现状 `%.*s` 等机制保留），print 的 `{}` 旧行为不变（回归保障），新增规格自动生效（FMT-06）。

### §2.5 FMT-10 to_string 降级

- format/print 实参为 struct/class 且该位置需要字符串化 → 查找 `to_string`：自由函数 `to_string(T)` 或方法 `T.to_string()`（沿用 print 的 lowerToString 两条路径）；找不到 → 编译期错误「cannot format T」。
- 约定文档化进 STD-09（`to_string`/`to_hash`/`equals` 扩展点；to_hash/equals 仅文档，实现随 STD-07）。

## §3 字符串库（STD-10）

### §3.1 内建原语（sema + codegen，MethodCall 机制）

| 方法 | 签名 | 语义 |
|------|------|------|
| `find` | `str.find(needle: str) -> isize` | 首次出现的**字节**下标；无 → -1（libc memmem 或手写扫描；空 needle → 0） |
| `rfind` | `str.rfind(needle: str) -> isize` | 末次出现字节下标；无 → -1 |
| `sub` | `str.sub(begin: usize, end: usize) -> str` | 零拷贝子视图 `{ptr+begin, end-begin}`；`begin > end || end > len` → **panic**（越界是程序错误，DEC-06 不做静默钳制） |

- `find`/`sub` 加进 `BuiltinMethod` 枚举，sema 方法表 + codegen 直落；`rfind` 随实现顺带（扫描方向不同，成本极低）。

### §3.2 组合层 `libs/std/string.cdd`

`export namespace std { ... }`，自包含 libc extern（io.cdd 先例）：

| API | 形式 | 说明 |
|-----|------|------|
| `contains(s, sub) -> bool` | 自由函数 | `s.find(sub) >= 0` |
| `starts_with(s, prefix) -> bool` | 自由函数 | `len`/`char_at` 或 find(0)==0 + 长度比较（实现取稳者） |
| `ends_with(s, suffix) -> bool` | 自由函数 | rfind 到末尾判定 |
| `trim_left(s) -> str` | 自由函数 | isspace 扫描 + `sub`，零拷贝视图 |
| `trim_right(s) -> str` | 自由函数 | 同上反向 |
| `trim(s) -> str` | 自由函数 | 两者串联 |
| `split(s, sep) -> []str` | 自由函数 | 视图数组：`str* arr = static_cast<str*>(malloc(n * sizeof(str)))`，逐段 `sub` 写入；返回 `{arr, n}` 切片；sep 为空 → 编译期拒绝（无意义）；无分隔符 → 单段全串（n=1，非空数组） |
| `split_destroy(parts: []str) -> void` | 自由函数 | `free(parts.ptr)`；不触碰元素（视图不拥有） |
| `join(parts: []str, sep: str) -> string` | 自由函数 | `string.new(first)` + 逐段 append(sep)+append(part)；返回 string，调用者 destroy |
| `concat(a: str, b: str) -> string` | 自由函数 | `string.new(a).append(b)` 便捷式 |
| `repeat(s: str, n: usize) -> string` | 自由函数 | n=0 → 空 string（len 0, cap 1） |
| `StrBuilder` | class | 包装 string：`new()`/`append(str)`/`push(char)`/`len()`/`build() -> str`（对内部 string 的视图，builder 存活期有效）/`destroy()`（释放内部 string + 自身 malloc） |
| `utf8_sub(s, start_cp, len_cp) -> str` | 自由函数 | 码点步进（`char_len_at` 累加）+ `sub`；越界码点下标 → panic（与 sub 一致） |

- 比较操作不重复提供：`==`/`!=` 内建已有（FMT-05），`<`/`>` 字典序本轮**不提供**（挂账：需要 memcmp 语义裁决，符号/无符号与码点序不一致）。

### §3.3 所有权与生命周期钉死

- `split` 返回的 `[]str` 元素是视图：底层数据随源 `str` 生存期；视图数组本身由 `split_destroy` 释放。源是字面量 → 数据永久有效；源是 `string` → 数组必须在源 destroy 之前 destroy（顺序不检查，文档明示用户责任，与 I4 裁决同模型）。
- `join`/`concat`/`repeat`/`StrBuilder.build` 产生的内存全部经显式 destroy/free 释放。

## §4 错误处理汇总（FMT-11）

| 情形 | 时机 | 形态 |
|------|------|------|
| 占位符数 ≠ 实参数（字面量 fmt） | 编译期 | E2020 |
| spec 与实参类型不符 | 编译期 | E2021 |
| 未知/非法 spec、非法转义 | 编译期 | E2022 |
| `sub` 越界 | 运行时 | panic |
| `utf8_sub` 越界 | 运行时 | panic |
| `split` 空 sep | 编译期 | E2021（归类「规格与实参不符」）或专用文案（实现时定，注册核对） |
| 动态 fmt 的 spec 错误 | 运行时 | 未定义（文档明示，不检查不 panic） |

## §5 文档与收口

- `docs/spec/stdlib.md`：新增 §std.string 与 §std.format（API 表 + 所有权模型 + 规格语法）。
- `docs/spec/abi.md`：布局表补 `str`/`string` 两行（终审 M5 顺手清账）。
- TODO.md：STD-09（部分：to_string 文档）/STD-10/STD-11 落定；FMT-06~12 逐项标定；P1-09 主体 `[~]`（STD-05~08/12 剩余另轮）。
- `docs/spec/grammar.ebnf`：无语法变更（无新 token/产生式）。

## §6 测试与验收

- 分层：`tests/frontend/`（spec 解析单元：合法/非法 spec、转义）、`tests/sema/`（E2020~22、to_string 降级、user-defined 优先）、`tests/e2e/`（format 运行时值正确性、print 规格回归、string.cdd 全 API、split/join/builder 所有权流）。
- 每任务 TDD（RED 亲见）+ 全量 ctest 绿 + 一提交；全局约束沿用 P1-06（git add 精确清单、`-f` 仅限 superpowers 文档）。
- 验收基线：全量 1123+ 绿不回退；format/print 规格行为以 e2e 值断言钉死。

## §7 任务骨架（供 writing-plans 展开）

- T1 spec 解析器扩展（PrintFormat：FMT-08 集合 + 转义 + 对齐/填充；print/println 接入回归）
- T2 format() 内建（sema 检查 E2020~22 + codegen snprintf 组装 + FMT-11）
- T3 to_string 降级统一（FMT-10，print/format 共享路径核对）
- T4 find/rfind/sub 内建原语
- T5 string.cdd 组合库（split/split_destroy/join/concat/repeat/trim*/contains/starts/ends/utf8_sub/StrBuilder）
- T6 文档/诊断注册核对/TODO 收口（含 Progress.md）

## §8 明确不做（本轮边界）

- STD-05~08（Allocator/Vec/HashMap）、STD-12 之外 io 扩展、std.math。
- `<`/`>` 字典序比较；动态格式串的编译期/运行时规格检查；`{0}` 位置参数与动态宽度。
- format 的 locale/Unicode 大小写折叠（to_lower/to_upper 不做——码点大小写映射表超轮）。
- print/println 迁出内建（保持直用）。
