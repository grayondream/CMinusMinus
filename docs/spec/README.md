# CDD 1.0 语言规范（Spec）

本目录是语言的**规范性定义**，对应 `TODO.md` 的 P0-07「语言规范骨架」。

| 文件 | 内容 | TODO |
|---|---|---|
| [`grammar.ebnf`](./grammar.ebnf) | 规范性 EBNF 文法 + 运算符优先级/结合性表 | INF-06 / INF-09 / INF-10 |
| [`keywords.md`](./keywords.md) | 关键字 / 保留字 / 运算符总表 | INF-11 / LEX-14 |
| [`abi.md`](./abi.md) | 类型布局、传参/返回 ABI、名称修饰、端序对齐 | TYP-27 / MEM-14~16 / MOD-15 |
| [`conversions.md`](./conversions.md) | 隐式/显式转换矩阵、整数提升、重载转换等级 | TYP-19 ~ TYP-24 |
| [`semantics.md`](./semantics.md) | 求值顺序、`defer`、重载解析、静态检查清单 | SEM-13 ~ SEM-18 |
| [`modules.md`](./modules.md) | 模块/文件映射、`namespace`、导入可见性、名称解析 | MOD-01 ~ MOD-15 |
| [`compile_time.md`](./compile_time.md) | `compile_time` 语法、求值器、反射 API、沙箱 | CT-01 ~ CT-22 |
| [`stdlib.md`](./stdlib.md) | 内建 `print`/`assert`/`panic`、`std.c` 绑定层、`std.core`/`std.io` | P0-05 / STD-01 / STD-12 / STD-23 / DEC-21 |

> 各分册均为**草案**：`[impl]` 部分已与实现对齐，`[plan]` 部分待对应 DEC 裁决后冻结。
>
> 文档与实现的一致性由 `tests/spec/test_spec_conformance.cpp` 约束（P0-07）：验证
> 规范性优先级表覆盖全部中缀运算符、文法中不再含 `goto`/label/预处理产生式、
> 关键字表标记了移除项、模块分册使用 `-M/--module-path`。

## 状态标记

- `[impl]`：当前实现已支持，以 `src/frontend` 为准。
- `[plan]`：TODO 规划，尚未实现；实现前可能随对应 DEC 决策调整。
- `[ng]`：明确不做（Non-goals）。

## 约定

- 终结符使用单引号 `'if'`；`{ X }` 重复 0+；`[ X ]` 可选；`(* … *)` 注释。
- 文法与 `src/frontend/Lexer.cpp`、`Token.h`、`Parser.cpp` 必须保持一致；实现变更需同步本目录。
- 未决项一律引用 `TODO.md` 的 DEC 编号，不在此处自行拍板。

## 维护

1. 新增语言特性时，先在本目录给出文法/语义，再实现。
2. 每个 `[plan]` 转 `[impl]` 时，同步更新 `keywords.md` 与 `grammar.ebnf` 的状态标记。
