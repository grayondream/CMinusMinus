# CDD 1.0 — 定宽类型统一 + LEX-15 字面量类型消费端

- 状态：待评审
- 日期：2026-09-28
- 关联条目：LEX-15、LEX-05、TYP-04、TYP-22、PAR-18
- 分类：architectural

## 1. 背景与动机

当前类型系统同时存在「非定宽类型」与「定宽类型」两套重复表示：

- `TypeKind::Int`（→ LLVM i32）与 `TypeKind::Int32`（→ LLVM i32）重复；
- `TypeKind::Float`（→ LLVM float）与 `TypeKind::Float32` 重复；
- `TypeKind::Double`（→ LLVM double）与 `TypeKind::Float64` 重复。

同时 LEX-15 的 `LiteralKind` 只在词法层生成，未贯穿 AST/sema/codegen：

- `NumberExprAST(int)`、`FloatExprAST(double)` 不携带字面量 kind；
- 词法器把 64 位累加结果 `static_cast<int>` 后存入 `TokenValue`（`Lexer.cpp:431,514`），>int 字面量被截断；
- sema 把无后缀浮点赋成 `TypeKind::Float`（f32），而 LEX-15 规定默认 `float64`；
- `lexer` 已有 `u/l/ul/lu`、`f/f16/f32/f64/f128` 后缀识别，但下游完全忽略。

本设计把语言数值类型统一到定宽集合（删除 `int`/`float`/`double` 关键字与 `TypeKind::Int/Float/Double`），并在此基础上完成 LEX-15 消费端，使字面量后缀与默认类型真正生效。

## 2. 目标与非目标

### 目标

1. 语言数值类型仅保留定宽拼写；`int`/`float`/`double` 不再是语言关键字。
2. 删除 `TypeKind::Int`、`TypeKind::Float`、`TypeKind::Double` 及 `getInt()/getFloat()/getDouble()`。
3. 新增 `TypeKind::Float16`、`TypeKind::Float128` 与 `float16`/`float128` 关键字。
4. `LiteralKind` 贯穿 lexer → parser → sema → codegen；后缀与默认类型生效；修复 >int 字面量截断。
5. 全量测试（当前 715）保持通过，并补充上述行为的测试。

### 非目标

- 不改变隐含窄化策略（`checkAssignmentTypes` 对算术类型仍放行）。
- 不做 `half`/`fp128` 的软件兜底与跨目标差异（留给 TYP-04）。
- 不改变 `char`/`bool`/`void`/`isize`/`usize` 语义。
- 不改变数字后缀语法本身（`u/l/ul/lu`、`f/f16/f32/f64/f128`）。
- 不改动 C++ 实现代码自身的 `int`/`float`/`double`（那与语言类型无关）。

## 3. 最终类型集

- 整数：`int8/16/32/64/128`、`uint8/16/32/64/128`、`isize`、`usize`
- 浮点：`float16/32/64/128`
- 其它保留：`char`(8bit)、`bool`(1bit)、`void`、指针、数组、`Slice`、`Optional`、`Result`、`struct`、`class`、`union`、`enum`、`using` 别名/`TypeKind::Typedef`（内部表示）
- 删除：`TypeKind::Int`、`TypeKind::Float`、`TypeKind::Double`、`getInt()`、`getFloat()`、`getDouble()`、关键字 `int`/`float`/`double`

## 4. 迁移映射

仅用于改写既有语言源码（产品源、测试内语言源码字符串、文档示例）：

| 旧写法 | 新写法 |
|---|---|
| `int` | `int32` |
| `float` | `float32` |
| `double` | `float64` |

字面量默认类型映射（LEX-15）：

| `LiteralKind` | 基础类型 |
|---|---|
| `Int` | `int32` |
| `UInt` | `uint32` |
| `Long` | `int64` |
| `ULong` | `uint64` |
| `Float16` | `float16` |
| `Float32` | `float32` |
| `Float64` | `float64`（无后缀默认） |
| `Float128` | `float128` |

## 5. 阶段设计

### Phase 1 — 迁移语言写法（别名暂留，测试保持全绿）

只改写语言源码的拼写，不动关键字/类型系统，因此过渡期两种写法并存，全量测试必须保持通过。

- 产品源：`libs/std/core.cdd`、`libs/std/c.cdd`、`libs/std/io.cdd`、`resources/main.c`、`resources/main_min.cdd`
- 测试内语言源码字符串：约 242 行、228 处 `int main`
- 文档示例：`docs/spec/*`、`README`

验证：`ctest` 全绿（≥715）。允许漏改——漏改的写法会在 Phase 2 被精准暴露。

### Phase 2 — 删除别名与非定宽类型

- 词法：`keywordMap`（`Lexer.cpp:33-36`）删除 `int/float/double`；`TokenType` 删除 `TOKEN_INT`、`TOKEN_DOUBLE`；`TOKEN_FLOAT` 语义收窄为「浮点字面量」（当前它同时被关键字与字面量使用，删关键字后歧义消除）；新增 `TOKEN_FLOAT16`、`TOKEN_FLOAT128` 及对应 `keywordMap` 项。
- 语法：`Parser::isTypeStart`（1337-1382）与 `parseBaseType`（1434-）删除三关键字分支，新增 `float16`/`float128`；`Parser.cpp:199-201`、`Utils.cpp:13-15` 的 token 名表同步。
- 类型系统：`Type.h` 删除 `Int/Float/Double`，新增 `Float16/Float128`；`TypeContext` 删除 `getInt/getFloat/getDouble`，新增 `getFloat16/getFloat128`。
- 语义/后端：
  - `Symbol.cpp`：`integerWidth`（53）、`floatWidth`（75）、`isUnsignedKind`（85）、`integerRank`（100）、`arithmeticUnderlying`（121）、`conversionRank`（195）去除三 kind；`getInt()` 默认改为 `getInt32()`；`floatWidth` 新增 16/128。
  - `CodegenContext::getLLVMType`（371）：删除三 case，新增 `half`、`fp128`。
  - `SemanticAnalyzer::typeToString`（271）删除三 case、新增两 case；所有 `getInt()/getFloat()/getDouble()` 调用点改为对应定宽 getter。
- 测试：155 处 `TypeKind::Int/Float/Double`、`getInt()/getFloat()/getDouble()` 断言更新。
- 新增负例：`int x;` 应报语法错误（`int` 成为标识符）。

### Phase 3 — LEX-15 字面量类型消费端

- 词法/Token：
  - `TokenValue` 变体 `int` → `long long`（其余不变）；读点 `Parser.cpp:516,2202,2367`、`Utils.cpp:126` 与 7 处测试同步。
  - `scanNumber` 存 `static_cast<long long>(value)`，保留 64 位位模式；有/无符号由 `literalKind` 决定。
  - `literalKindName(Int)` 改为 `"int32"`。
- AST：
  - `NumberExprAST`：`int value` → `long long value`，新增 `LiteralKind literalKind{Int}`；保留 `NumberExprAST(int)` 旧构造，另加带 kind 的构造。
  - `FloatExprAST`：新增 `LiteralKind literalKind{Float64}`；构造加默认参数，旧用法不变。
  - `parser`（`Parser.cpp:514-525`）用 `token->literalKind` 填充节点。
- sema：
  - 新增 `Type* typeForLiteralKind(LiteralKind)`；`visit(NumberExprAST&)`/`visit(FloatExprAST&)` 改用它（`None` 回退 `int32`/`float64`）。
  - `ConstValue::intVal` 由 `int` 提升为 `long long`。
- codegen（`ast/Expr.cpp:99-105`）：
  - `NumberExprAST::codegen` 按 `node.type` 取 LLVM 整型宽度（缺省 i32），用 `isUnsignedIntegerType` 定 signedness。
  - `FloatExprAST::codegen` 按 `node.type` 选 APFloat 语义（half/float/double/fp128，缺省 double）。

## 6. 测试策略（TDD）

- Phase 1/2：以「全量 720+ 绿」与新增「`int` 被拒绝」用例驱动；补充 `typeToString`、`getLLVMType`、常规算术的类型单测。
- Phase 3：先写失败用例：
  - parser：`42u/42l/42ul/1.5f/1.5f16` 的 AST `literalKind` 正确；
  - sema：`getExprType` 对各字面量返回期望 `TypeKind`（含 `Float16/Float128`）；
  - codegen：`1l`→i64、`1ul`→i64（无符号位模式）、`1.5f`→float、`1.5f16`→half、`1.5f128`→fp128、默认 `1`→i32、`1.5`→double；
  - e2e：`4000000000ul` 不再截断（JIT 执行）。

## 7. 风险与缓解

| 风险 | 缓解 |
|---|---|
| Phase 1 批量改写误伤测试里的 C++ `int`（非语言源码） | 按字符串模式定向改写；Phase 2 的编译/测试失败精确定位漏改与误改 |
| `TOKEN_FLOAT` 语义收窄是接口变更 | 全仓核对引用（`Parser.cpp:521,1341,1443`、`Utils.cpp:14`） |
| 默认浮点 f32→f64 改变部分 IR | e2e 断言运行结果，预期可通过；如有 IR 字符串断言一并修正 |
| `half`/`fp128` 的 JIT 执行在个别平台不稳 | f16/f128 测试以类型与常量种类为主，不强行全执行 |
| 删除 `TypeKind::Int/Float/Double` 引发大量编译错误 | Phase 2 一次性切换，编译器错误引导逐点修复 |

## 8. 验收标准

1. 语言中 `int`/`float`/`double` 不再可作类型关键字；`int32/float32/float64/float16/float128` 可用。
2. `TypeKind::Int/Float/Double` 与 `getInt/getFloat/getDouble` 已删除。
3. `LiteralKind` 从 lexer 贯穿到 codegen，后缀与默认类型生效，`4000000000ul` 不被截断。
4. 全量测试通过（≥715 且 0 失败），新增用例覆盖第 3 条。
5. `docs/spec/grammar.ebnf`、`docs/spec/conversions.md`、`docs/spec/keywords.md`、`TODO.md` 同步更新。

## 9. 假设

- `char`/`bool`/`void` 保留（其宽度固定：8/1/—）。
- `int`/`float`/`double` 关键字彻底删除，不保留任何别名。
- `float16`/`float128` 新增为类型关键字。
