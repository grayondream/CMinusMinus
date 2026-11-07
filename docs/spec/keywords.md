# Cmm 1.0 — 关键字 / 保留字 / 运算符总表

> 对应 TODO：**INF-11**、**LEX-14**、**DEC-18**
> 状态图例：`[impl]` 已实现（见 `src/frontend/Lexer.cpp` 关键字表）｜`[plan]` 规划新增｜`[removed]` 已移除（P0-01）｜`[remove]` 计划移除/废弃｜`[decide]` 待决策
> 本表是词法层唯一权威关键字清单；`docs/spec/grammar.ebnf` 的终结符须与本表一致。

## 0. 现状说明

- 关键字目前通过 `src/frontend/Lexer.cpp` 中的静态 map 识别；`src/frontend/Token.h` 定义 token 枚举。
- `using`、`protected` 当前**不是**关键字，而是把标识符文本当作上下文关键字处理（见 `Parser::parseDeclaration`）——本表统一登记为“上下文关键字”。
- `namespace`、`template`、`typename`、`this`、`thread_local`、`static_cast`、`reinterpret_cast` **尚无**对应 token，属规划项。

## 1. 数据类型关键字

| 关键字 | 状态 | 说明 |
|---|---|---|
| `void` | `[impl]` | |
| `bool` | `[impl]` | |
| `char` | `[impl]` | token `TOKEN_CHAR_KW` |
| `int` / `float` / `double` | `[removed]` | 已移除：仅保留定宽拼写；`int`→`int32`、`float`→`float32`、`double`→`float64` |
| `int8` `int16` `int32` `int64` `int128` | `[impl]` | 有符号定宽 |
| `uint8` `uint16` `uint32` `uint64` `uint128` | `[impl]` | 无符号定宽 |
| `isize` / `usize` | `[impl]` | 平台指针宽度（TYP-03） |
| `float16` / `float32` / `float64` / `float128` | `[impl]` | 定宽浮点 |
| `f16` / `f32` / `f64` / `f128` | `[impl]` | TYP-04：与 `float16`/`float32`/`float64`/`float128` 等价的类型关键字；同时是字面量后缀（LEX-15） |
| `char8` / `char16` / `char32` | `[impl]` | TYP-05：uint8/16/32 别名（非独立名义类型） |
| `str` / `String` | 非关键字 | `str` 已硬改名 `string_view`（DEC-18）；标准库类型不在词法层保留 |

### 1a. 字面量前缀

| 前缀 | 状态 | 说明 |
|---|---|---|
| `u8` / `u16` / `u32` | `[impl]` | TYP-05：字符串字面量前缀，类型分别为 `char8[]` / `char16[]` / `char32[]`（逐字节存储，不转码）；无前缀 `"..."` 仍为 `string_view` |
| `r` / `"""` | `[impl]` | 原始字符串 / 多行字符串（LEX-06 / LEX-16） |

## 2. 存储类 / 限定符
| 关键字 | 状态 | 说明 |
|---|---|---|
| `const` | `[impl]` | |
| `auto` | `[impl]` | 仅用于变量声明的类型推导，必须有初始化器（Review 2.6） |
| `constexpr` | `[impl]` | 与 `compile_time` 关系见 DEC-05 |
| `static` | `[impl]` | |
| `extern` | `[impl]` | C 链接（MOD-09） |
| `volatile` | `[impl]` | |
| `restrict` | `[impl]` | 指针后缀限定符（`T* restrict`，PAR-06）；仅语法接受，别名/优化语义见 MEM-04 |
| `inline` | `[impl]` | |
| `register` | `[remove]` | 无实际语义；DEC-18 已裁决移除 token |
| `thread_local` | `[impl]` | 存储类（变量），CON-04；完整存储模型见 CON-10 |
| `atomic` | `[impl]` | 类型限定符（整型标量），CON-01；完整并发模型见 CON-02/09 |

## 3. 控制流

| 关键字 | 状态 | 说明 |
|---|---|---|
| `if` `else` | `[impl]` | |
| `for` `while` `do` | `[impl]` | |
| `switch` `case` `default` | `[impl]` | 允许贯穿 |
| `break` `continue` | `[impl]` | |
| `return` | `[impl]` | `return;` 与 `return expr;` 均支持 |
| `defer` | `[impl]` | 语义待冻结，见 SEM-18 / FUN-10 |
| `goto` | `[removed]` | 已连同标签语句移除（NG-01/P0-01）：不再有 token、AST、解析或代码生成 |

## 4. 类型构造 / 声明

| 关键字 | 状态 | 说明 |
|---|---|---|
| `struct` `union` `enum` `class` | `[impl]` | |
| `typedef` | `[removed]` | PAR-24：由 `using` 取代；出现即报错并提示改写 |
| `operator` | `[impl]` | 可重载集合见 SEM-17 |
| `template` | `[impl]` | GEN-01 / PAR-21（函数/class/别名模板） |
| `typename` | `[impl]` | GEN-01 |
| `this` | `[impl]` | CRTP 必需，PAR-17 / INH-05 |
| `using` | `[impl]` 上下文 | 唯一类型别名形式 `using X = type-id;`；顶层/namespace/块/聚合成员体（块内词法作用域，成员受访问控制） |
| `type` | `[removed]` | PAR-24：distinct 别名方案取消，由 `using` 取代 |
| `generic` | `[remove]` | 未使用的旧泛型关键字；token 已移除（LEX-02 / DEC-18） |

## 5. 模块 / 可见性

| 关键字 | 状态 | 说明 |
|---|---|---|
| `module` | `[impl]` | |
| `import` | `[impl]` | 访问语法见 DEC-02 / MOD-11 |
| `export` | `[impl]` | |
| `public` | `[impl]` | |
| `private` | `[impl]` | |
| `protected` | `[impl]` | 上下文关键字；派生类内经 `this->` 访问基类 protected 成员放行（INH-06） |
| `namespace` | `[impl]` | 已实现（函数/变量/限定类型名）；见 PAR-22 / MOD-12 |

## 6. 编译期 / 反射

| 关键字 | 状态 | 说明 |
|---|---|---|
| `constexpr` | `[impl]` | 见 §2 |
| `true` / `false` | `[impl]` | 布尔字面量 |
| `null` | `[impl]` | 唯一空指针字面量；`nullptr` 不再是关键字（Review 2.3） |
| `comptime` | `[remove]` | 统一并入 `compile_time`；token 已移除（CT-01 / NG-12 / DEC-18） |
| `compile_time` | `[impl]` | 编译期命名空间（非保留字，按标识符处理即可） |
| `static_assert` | `[impl]` | 作为 `compile_time.static_assert`，**不设独立关键字**（NG-12） |
| `sizeof` | `[impl]` | 一元运算符 |
| `alignof` / `offsetof` | `[impl]` | 独立关键字形式已实现（PAR-14） |
| `typeof` | `[remove]` | DEC-18 裁决移除；类型查询改由 `compile_time` 反射（CT-07）承担 |
| `cast` | `[remove]` | DEC-18 裁决移除；显式转换用 `static_cast`/`reinterpret_cast`（LEX-11） |
| `type_info` | `[remove]` | 由 `compile_time` 反射取代（NG-12） |

## 7. 泛型 / 转换 / 汇编

| 关键字 | 状态 | 说明 |
|---|---|---|
| `static_cast` | `[impl]` | LEX-11 / PAR-18 |
| `reinterpret_cast` | `[impl]` | LEX-11 / PAR-18 |
| `asm` | `[impl]` | P1-08（PAR-16 / CG-10 / DEC-09）；模板/约束透传 LLVM，方言 AD_ATT |

## 8. 运算符与标点（token）

| 记号 | 含义 | 状态 |
|---|---|---|
| `+ - * / %` | 算术 / 一元 / 指针 | `[impl]` |
| `= += -= *= /= %=` | 赋值 | `[impl]` |
| `== != < > <= >=` | 比较 | `[impl]` |
| `&& \|\| !` | 逻辑 | `[impl]` |
| `& \| ^ ~` | 位运算 / 取地址 | `[impl]` |
| `<< >>` | 移位 | `[impl]` |
| `&= \|= ^= <<= >>=` | 复合赋值 | `[impl]` |
| `++ --` | 自增自减 | `[impl]` |
| `? :` | 条件运算符 | `[impl]` |
| `::` | 限定名 | `[impl]` token `TOKEN_COLON_COLON`（PAR-22） |
| `. ->` | 成员访问 | `[impl]` |
| `( ) [ ] { }` | 分组 / 下标 / 初始化 | `[impl]` |
| `, ;` | 分隔 | `[impl]` |
| `...` | 可变参数 | `[impl]` |
| `#` | 非运算符 | `[removed]` 无预处理器（NG-02/P0-01）；`#` 仅词法识别为 `TOKEN_HASH`，解析器立即报错“不支持预处理指令” |
| `[[` `]]` | 注解定界 | `[plan]` ANN-01 |

## 9. 保留策略

1. 本表为**唯一权威**；新增关键字需同步更新 `Lexer.cpp`、`Token.h` 与本文件。
2. 不以关键字形式复用的标识符：`str`、`String`、`Array` 等标准库类型名保持普通标识符。
3. 上下文关键字（`using`/`type`/`protected`/`compile_time`）在引入二义性前不改为一等关键字，见 DEC-16。
4. **DEC-18 已裁决**：移除 `register`/`cast`/`typeof`/`comptime`/`generic` 的 token（已从 `Token.h`、`Lexer.cpp`、`Utils.cpp` 删除）。显式转换改用 `static_cast`/`reinterpret_cast`（LEX-11），类型查询改用 `compile_time` 反射（CT-07）。
5. `print`/`println`/`assert`/`panic` 是**内建标识符**，不是关键字：它们仍是普通标识符，仅在用户未声明同名函数时由语义分析按内建处理（见 `stdlib.md`）。
6. `static_cast`/`reinterpret_cast` 是**上下文关键字**：作为普通标识符，仅在紧跟 `<类型>` 时按转换运算符解析（LEX-11 / PAR-18 / DEC-18，见 `conversions.md` §5）。
