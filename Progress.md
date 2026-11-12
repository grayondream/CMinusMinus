# 进展记录

## 背景

- 任务来源：用户要求「根据 TODO 继续完善」。
- 项目：CDD / CDD 1.0，基于 LLVM 的 C 风格编译器；`TODO.md` 为唯一需求与进度清单。
- 本次选择：TODO 中的 **LEX-13 + LEX-17** —— 词法诊断与错误恢复。诊断码 `E0001`（非法字符）、`E0002`（未闭合字面量）、`E0003`（整型溢出）此前已在 `src/sema/Diagnostic.cpp` 注册，但词法器从未产出。

## 目标

- 为词法器增加结构化诊断：非法字符（E0001）、未闭合字面量（E0002）、整型字面量超过 64 位（E0003）。
- 出错后能恢复继续扫描，不抛异常、不进入死循环。
- 驱动层（`CompilerDriver` / `ModuleLoader` / `StdPrelude`）上报诊断并使编译失败。
- 验收：新增测试先 RED 后 GREEN，且全量测试无回归。
- 所有模块都要有完整的自动化测试case

---

## 2026-09-27 21:35 完成 LEX-13 + LEX-17 词法诊断

- 改动摘要（文件路径）：
  - `src/frontend/Lexer.h` / `src/frontend/Lexer.cpp`：
    - 新增 `getDiagnostics()` 与 `report()`（替换原抛异常的 `error()`）。
    - **E0001**：未知字符报错后跳过该字符并继续扫描。
    - **E0002**：未闭合字符串 / 字符 / 原始字符串 / 块注释；同时修复了未闭合字符串、字符在 EOF 处**死循环**的缺陷。
    - **E0003**：二 / 八 / 十六 / 十进制均按 64 位无符号累加并检测溢出，超过 64 位报错一次。
    - 新增 `skipTrivia()` 统一跳过空白与注释，修复「行注释后的空行被误判为非法字符」的潜在缺陷。
  - `src/driver/CompilerDriver.cpp`、`src/driver/ModuleLoader.cpp`、`src/driver/StdPrelude.cpp`：上报词法诊断并中止编译。
  - 测试：`tests/frontend/test_lexer.cpp`（12 项）、`tests/driver/test_compiler_driver.cpp`（2 项）、`tests/sema/test_diagnostic_snapshot.cpp` 及快照 `tests/diagnostics/snapshots/lexical_invalid_character.txt`。
  - 文档：`TODO.md`（LEX-13 → `[x]`；LEX-17 更新为部分完成；进度快照更新为 698 测试）、`docs/spec/conversions.md §7`。
- 验证方式与结果：
  - 按 TDD：先写失败测试（6 项断言失败、2 项超时挂起），实现后全绿。
  - `ctest` 全量 **698** 通过（原 683 + 新增 15），0 失败。
  - 驱动诊断格式实测：`file:line:col: error[E0001]: invalid character '@'`。
- 遗留问题 / 下一步：
  - LEX-17 的**数字分隔符位置规则**（`1__2` / `_1` / `1_` 是否合法）尚未校验。
  - LEX-15（字面量默认类型与后缀映射）未做；当前 `TokenValue` 仅存 `int`，大于 `int` 的字面量仍会被截断，属独立条目。

---

## 2026-09-27 22:40 收尾词法：LEX-17 分隔符规则 + LEX-15 后缀/类型映射（词法层）

- 背景：上一轮 LEX-13/17 遗留「`_` 位置规则未校验」，且 LEX-15 未做。经 brainstorming 分类为 bounded，用户选择「仅词法层」范围并批准设计。
- 改动摘要（文件路径）：
  - `src/frontend/Token.h` / `Token.cpp`：新增 `enum class LiteralKind`（`Int/UInt/Long/ULong/Float16/Float32/Float64/Float128`）、`Token::literalKind` 字段与 `literalKindName()` 映射表。
  - `src/frontend/Lexer.h` / `Lexer.cpp`：
    - **LEX-17**：新增 `validateDigitSeparators()`，`_` 仅当左右均为当前进制数字时合法；违规报 **E0004** 并继续（恢复）。
    - **LEX-15**：`consumeIntegerSuffix()`（`u`/`l`/`ul`/`lu`，各进制通用）与 `consumeFloatSuffix()`（`f`/`F`、`f16`/`f32`/`f64`/`f128`，兼容 `l`/`L`）；无后缀默认整型 `int`、浮点 `float64`；重写 `scanNumber()` 以在计算值前剥离后缀。
  - `src/sema/Diagnostic.h` / `Diagnostic.cpp`：注册 `LexInvalidDigitSeparator` → `E0004`。
  - 测试：`tests/frontend/test_lexer.cpp` 新增 `LexerNumberSuffixTest.*`（5 项）与 `LexerSeparatorTest.*`（6 项）；`tests/sema/test_diagnostic_snapshot.cpp` + 快照 `lexical_digit_separator.txt`。
  - 文档：`docs/spec/grammar.ebnf`（分隔符产生式收紧、后缀去 `[plan]`、补裸 `f`）、`keywords.md`、`conversions.md §7`、`TODO.md`（LEX-17 → `[x]`，LEX-15 → `[~]` 并注明消费端待补）。
- 验证方式与结果：
  - TDD：先写测试观察 RED（9 失败），实现后全绿。
  - `ctest` 全量 **710** 通过（上轮 698 + 新增 12），0 失败；编译无警告。
  - 实测评测：`snapshot.c:2:19: error[E0004]: misplaced digit separator '_'`。
- 遗留问题 / 下一步：
  - **LEX-15 消费端**：AST/sema/codegen 仍按 `int`/`double` 处理字面量（`NumberExprAST::value` 为 `int`，codegen 固定 `APInt(32)`），大整数/无符号字面量的真实类型未生效，`TokenValue` 也未扩宽——应在后续条目（如 TYP-23 收口）实现。
  - 未做无效后缀诊断（避免与尚未实现的指数 `1e10` 冲突）；浮点指数仍未实现（LEX-05）。

---

## 2026-09-27 23:30 补齐浮点指数（LEX-05）

- 背景：上轮收尾词法时发现 `1e10` 会被切成 `1` + 标识符 `e10`；LEX-05（浮点字面量含指数）虽有规范但未实现。经 brainstorming 分类为 bounded，用户批准设计。
- 改动摘要（文件路径）：
  - `src/frontend/Lexer.cpp`（`scanNumber` 十进制分支）：新增指数扫描——`e`/`E` + 可选 `+`/`-` + 十进制数字（数字间允许 `_`）；`isfloat = 有小数点 || 有指数`；传给 `std::stod` 的字符串改为按位置截取数值段（后缀前）并剔除 `_`，从而支持 `1e10`/`1.5e-3`/`2E+4`。
  - 宽容策略（不新增诊断码）：`e` 后不足一个数字则不构成指数（但后跟 `_` 时会捕获以便报 E0004），故 `1e`→`NUMBER 1`+`IDENT e`、`1e+x` 保持切分；`0x1e10` 因 `e` 是十六进制数字不受影响。
  - 测试：`tests/frontend/test_lexer.cpp` 新增 `LexerFloatExponentTest`（5 项：无小数点指数、指数+后缀、十六进制 `0x1e10`、不完整指数宽容、指数内分隔符 `1e1_0` 合法 / `1e_10` 报 E0004）。
  - 文档：`TODO.md`（LEX-05 → `[~]` 并注明剩余“结尾点”形式；进度快照 715）、`docs/spec/grammar.ebnf`（补 LEX-05 `[impl]` 注释与已知差异）、`docs/spec/conversions.md §7`（指数规则）。
- 验证方式与结果：
  - TDD：先写测试观察 RED（3 项失败：两个 token 数不符、一个缺 E0004），实现后全绿。
  - `ctest` 全量 **715** 通过（上轮 710 + 新增 5），0 失败；编译无警告。
- 遗留问题 / 下一步：
  - 「结尾点」浮点 `1.` / `1.e3` 与十六进制浮点 `0x1p3` 仍未实现（已在 TODO LEX-05 注明）。
  - LEX-15 消费端（AST/sema/codegen 按字面量类型处理）仍待后续条目。

---

## 2026-09-28 定宽类型统一 + LEX-15 消费端（设计/计划阶段）

- 背景：推进 LEX-15 消费端时，用户决定把数值类型统一为定宽集合，删除 `int`/`float`/`double` 关键字与 `TypeKind::Int/Float/Double`，并新增 `Float16/Float128`。
- 产出：
  - 设计文档 `docs/spec/fixed-width-types.md`（已提交 `3e2119f`）。
  - 实施计划 `docs/superpowers/plans/2026-09-28-fixed-width-types.md`（本地，`docs/superpowers/` 被 gitignore）。
- 计划要点：Phase 1 迁移语言写法（别名暂留、测试全绿）→ Phase 2 分小步删除别名/非定宽类型（每步可编译可测）→ Phase 3 LEX-15 消费端（Token 扩宽→AST 带 kind→sema 映射→codegen 定宽），共 13 个 Task。
- 状态：等待用户选择执行方式（subagent-driven / native）后开始实现。
- 遗留问题 / 下一步：按计划执行；完成后追加 Progress 并更新 TODO/规范文档。

## 2026-09-28 22:40 Task 2 完成：测试内语言源码迁移到定宽写法

- 背景：定宽类型统一计划 Phase 1 第二任务，把测试 `.cpp` 中嵌入字符串（`"..."` / `R"(...)"`）的 CDD 源码 `int/float/double` 迁移为 `int32/float32/float64`，为 Task 7 删除旧关键字铺路。
- 完成事项（提交 `c444391`，33 个文件，592+/592-）：
  - 用只改字符串字面量内容的脚本批量迁移，并保留非语言源码字符串：`literalKindName(LiteralKind::Int), "int"`（Task 10）、`message.find("int"/"float")`（Task 6）、`conv.find("enum ↔ int")`（Task 3）。
  - 连带改动（测试全绿所需）：`test_parser.cpp` 的 `TypeKind::Int/Float/Double` 断言 → `Int32/Float32/Float64`；`test_lexer.cpp` 的 `TOKEN_INT`→`TOKEN_INT32` 及列号；`test_source_location.cpp` 列号；4 个诊断快照用 `CDD_UPDATE_SNAPSHOTS=1` 重新生成。
  - Phase 1 变通：类方法实参字面量改显式强转 `f.setX((int32)42)`（7 处，因 `resolveMethod` 指针相等匹配而字面量仍为 `int`），Task 5 后可回退。
- 验证方式与结果：
  - `cd build && cmake --build . -j$(nproc) && ctest` → **100% passed, 0 failed, 715/715**（2 个 ConstexprFunctionE2E 按既有基线 Skip）。
  - `grep -rn --include=*.cpp 'int main' tests` 无输出；字符串内旧写法无残留。
- 遗留问题 / 下一步：
  - Task 3 迁移文档示例；后续 Task 5/6 使字面量定宽后简化类方法调用变通。

## 2026-09-28 22:10 Task 1 完成：产品语言源迁移到定宽写法

- 背景：定宽类型统一计划（`docs/superpowers/plans/2026-09-28-fixed-width-types.md`）Phase 1 首任务。
- 完成事项：
  - 迁移 `libs/std/{core,c,io}.cdd`、`resources/{main,main_min}.c` 的语言源码：`int→int32`、`float→float32`、`double→float64`（提交 `91cd2fc`）。
  - 控制器裁定 R1：`src/sema/SemanticAnalyzer.cpp` `visit(FunctionDeclAST&)` 的 constexpr 字面量类型检查改用 `isIntegerType(t) || isFloatType(t)`（原硬编码 `TypeKind::Int/Float/Double/Char`）。
- 验证方式与结果：
  - `cmake --build build -j$(nproc)` 成功；`ctest` 全量 **715/715 通过，0 失败**。
  - R1 负例 `ConstexprFunctionWithNonLiteralReturnType/Parameter` 仍通过；`CoreMinMax/CoreClamp/CoreConstexprUse` 通过。
  - `grep -rnE '\b(int|float|double)\b' libs resources` 无输出。
- 遗留问题 / 下一步：
  - 环境修复：重配置 build（禁用系统 GTest find_package、清掉残留 `FETCHCONTENT_SOURCE_DIR_GOOGLETEST`）以恢复可构建状态，未改源码。
  - 后续 Task 2 迁移测试内语言源码。

## 2026-09-28 22:38 Task 3 完成：文档示例迁移到定宽写法

- 背景：定宽类型统一计划 Phase 1 第三任务，把 `docs/spec` 中作为 CDD 源码的 `int/float/double` 迁移为 `int32/float32/float64`。
- 完成事项（提交 `5c054db`，4 文件，37+/37-）：`abi.md`（标量表/名称修饰表）、`conversions.md`（§2/§3/§4/§5/§9）、`modules.md`（namespace 示例块）、`stdlib.md`（assert 示例 + std.core/std.io 签名表）。`semantics.md`/`compile_time.md`/`README.md` 无旧写法，未改。
- 保留项：`conversions.md` 的 `enum ↔ int` 与 §7（后者归 Task 8），`abi.md` 的 `enum E : uint8` 与 LLVM 类型列 `float`/`double`；未触碰 grammar.ebnf/keywords.md/fixed-width-types.md。
- 验证方式与结果：`cmake --build build -j$(nproc)` 成功；`ctest` → **715/715 通过，0 失败**（2 个既有 Skip）；`SpecConformance.*` 12/12 通过；`git status` 仅 4 个文档文件。
- 遗留问题 / 下一步：Task 8 处理 conversions.md §7、grammar.ebnf、keywords.md、TODO.md。

## 2026-09-28 22:50 Task 4 完成：新增 Float16/Float128 类型与 float16/float128 关键字

- 背景：fixed-width-types 计划 Task 4，新增定宽浮点 `float16`（half）/`float128`（fp128），纯增量，不删除 int/float/double 或 TypeKind::Int/Float/Double。
- 完成事项（提交 `068919e`，12 文件，71+/1-）：
  - `src/ast/Type.h`/`Type.cpp`：`TypeKind::Float16/Float128` + `TypeContext::getFloat16()/getFloat128()`。
  - `src/ast/Symbol.cpp`：`floatWidth` 加 `Float16→16`、`Float128→128`。
  - `src/codegen/CodegenContext.cpp`：`getLLVMType` 加 `getHalfTy`/`getFP128Ty`。
  - `src/sema/SemanticAnalyzer.cpp`：`typeToString` 加 `"float16"`/`"float128"`。
  - `src/frontend/Token.h`/`Lexer.cpp`/`Parser.cpp`：`TOKEN_FLOAT16/TOKEN_FLOAT128` + keywordMap + `isTypeStart`/`parseBaseType`。
  - `src/support/Utils.cpp`：token 名表补 `float16`/`float128`。
  - 测试：`test_codegen_context.cpp`（half/fp128）、`test_parser.cpp`（声明→TypeKind）、`test_lexer.cpp`（关键字）各 1 项。
- 验证方式与结果：
  - TDD：先写测试 RED（编译报 `getFloat16`/`Float16`/`TOKEN_FLOAT16` 不存在），实现后聚焦 3 项全绿。
  - `cd build && cmake --build . -j$(nproc) && ctest` → **100% passed, 0 failed, 718/718**（基线 715 + 新增 3；2 个 ConstexprFunctionE2E 既有 Skip）。
- 遗留问题 / 下一步：
  - `SemanticAnalyzer::isFloatType()` 未纳入 `Float16/Float128`（brief 未要求），sema 算术类型检查暂不识别这两类；建议在删除旧浮点类型/收口 LEX-15 消费端时一并更新。
  - 报告见 `.superpowers/sdd/2026-09-28-fixed-width-types/task-4-report.md`。

## 2026-09-28 22:55 Task 5 完成：src 引用改写为定宽类型（保留别名）

- 背景：fixed-width-types 计划 Task 5（R2 裁定扩大作用域）。让 `src/` 不再产出/依赖 `TypeKind::Int/Float/Double`，
  `int`/`float`/`double` 关键字现产出 `Int32/Float32/Float64`；保留枚举成员与三个 getter（未使用，Task 6 删除）。
- 完成事项（提交 `c834904`，17 文件，171+/179-）：
  - `src/frontend/Parser.cpp`：`parseBaseType` 关键字分支 → `getInt32()/getFloat32()/getFloat64()`。
  - `src/sema/SemanticAnalyzer.cpp`：getter 调用点改定宽；`isIntegerType` 去 `Int`；`isFloatType` 去 `Float/Double` 并补 `Float16/Float128`；`typesCompatible`/`typeToString` 去旧标签。
  - `src/ast/Symbol.cpp`：`arithmeticUnderlying`/`promoteArithmeticType`/`isUnsignedArithmeticType` 定宽化，删除 `integerWidth`/`floatWidth`/`integerRank` 旧标签。
  - `src/ast/Mangle.cpp`：删旧 `Int/Float/Double` 标签；`Enum` 修饰 `"int"`→`"int32"`（解决 Task 3 遗留 abi.md:86 漂移）。
  - `src/codegen/CodegenContext.cpp`：`getDIType`/`getLLVMType` 删旧标签。
  - `src/support/Utils.cpp`：`TypeToString` 补齐定宽整型/浮点（含 Float16/Float128）+ Bool/Enum。
  - `src/ast/PrintFormat.cpp`/`Expr.cpp`/`Decl.cpp`：删旧分组标签、`promoteVarArg` 保留 Float32 提升、constexpr 折叠构造定宽。
  - 测试：7 文件机械改写 getter/TypeKind 引用；移除 7 处 `(int32)` 方法实参强转（Task 2 carry-forward，字面量现为 Int32）。
- 验证方式与结果：
  - `cd build && cmake --build . -j$(nproc) && ctest` → **100% passed, 0 failed, 718/718**（2 个 ConstexprFunctionE2E 既有 Skip）。
  - `grep -rnE 'getInt\(\)|getFloat\(\)|getDouble\(\)' src` 仅命中保留的 6 行签名（Type.h 声明 + Type.cpp 定义），零调用点。
- 遗留问题 / 下一步：
  - Task 6 删除 `TypeKind::Int/Float/Double` 与三个 getter（届时该 grep 才会为空）。
  - 报告见 `.superpowers/sdd/2026-09-28-fixed-width-types/task-5-report.md`。

## 2026-09-28 23:00 Task 6 完成：删除旧类型枚举与 getter

- 背景：fixed-width-types 计划 Task 6。Task 5 已把 `src`/`tests` 全部调用点改写到定宽类型，旧
  `TypeKind::Int/Float/Double` 与 `getInt()/getFloat()/getDouble()` 已无调用点，本次做纯删除收口。
- 完成事项（提交 `bf5e293`，2 文件，27-）：
  - `src/ast/Type.h`：`TypeKind` 删除 `Int/Float/Double` 三成员；`TypeContext` 删除三个 getter 声明。
  - `src/ast/Type.cpp`：删除三个 getter 定义（含 `m_types[...]` 惰性构造体）。
  - `src/sema/SemanticAnalyzer.cpp`：`typeToString` 无旧 case（Task 5 已删），未改动。
- 验证方式与结果：
  - 改动前 `grep -rnE 'TypeKind::(Int|Float|Double)\b|getInt\(\)|getFloat\(\)|getDouble\(\)' src tests`
    仅命中 `Type.h`/`Type.cpp` 自身声明与定义；改动后同命令 exit 1（零匹配）。
  - `cd build && cmake --build . -j$(nproc) && ctest` → **100% passed, 0 failed, 718/718**（2 个既有 Skip）。
  - `git status --short` 仅两处预期修改。
- 遗留问题 / 下一步：无（旧类型已彻底移除）。
  - 报告见 `.superpowers/sdd/2026-09-28-fixed-width-types/task-6-report.md`。

## 2026-09-28 23:01 Task 9 完成：抽离 LiteralKind 到 support 层

- 背景：fixed-width-types 计划 Task 9。`LiteralKind` 与 `literalKindName()` 原定义在
  `frontend/Token.{h,cpp}`，需供 `ast/` 复用而不引入 `ast -> frontend` 依赖。
- 完成事项：
  - 新建 `src/support/LiteralKind.h`：迁入 `enum class LiteralKind : int32_t`（成员与底层类型不变）
    及 `const char* literalKindName(LiteralKind);` 声明。
  - `src/support/Utils.cpp`：迁入 `literalKindName` 定义（映射表逐字未改）。
  - `src/frontend/Token.h`：改 `#include "support/LiteralKind.h"`，删除本地枚举与声明。
  - `src/frontend/Token.cpp`：移除已迁出的定义。
  - `tests/frontend/test_lexer.cpp`：新增 `LexerLiteralKindTest.NameIsAvailableFromSupportHeader`。
- 验证方式与结果：
  - `cd build && cmake --build . -j$(nproc) && ctest` → **100% passed, 0 failed, 722/722**
    （2 个 `ConstexprFunctionE2E` 既有 Skip）。基线 721 + 新增 1。
  - 无行为变更：`literalKindName(LiteralKind::Int)` 仍为 `"int"`（Task 10 才改）。
- 遗留问题 / 下一步：Task 10 把 `Int` 映射改为 `"int32"`。
  - 报告见 `.superpowers/sdd/2026-09-28-fixed-width-types/task-9-report.md`。

## 2026-09-29 00:20 Task 7/8 完成：删除关键字 + 规范文档同步

- 改动摘要：
  - Task 7（提交 `8c14f8b`）：删除 `int`/`float`/`double` 关键字与 `TOKEN_INT`/`TOKEN_DOUBLE`（保留 `TOKEN_FLOAT` 作浮点字面量）；`src/frontend/{Lexer.cpp,Token.h,Parser.cpp}`、`src/support/Utils.cpp`。
  - 补齐前序遗漏的嵌入式语言源：`src/driver/StdPrelude.cpp`（内建 std.c 预置串）与 `tests/resources/test_input/*.cdd`、`tests/e2e/resources/test_const.cdd`。
  - 新增负例/正例：`tests/frontend/test_lexer.cpp`（`int float double` 为标识符）、`tests/frontend/test_parser.cpp`（`int x` 报错、`int32 int = 5` 合法）。
  - Task 8（提交 `841840b`）：`docs/spec/{keywords.md,grammar.ebnf,conversions.md}`、`TODO.md` 同步定宽类型集与默认字面量类型。
- 验证方式与结果：`cd build && cmake --build . -j$(nproc) && ctest` → **100% passed, 0 failed, 721/721**；
  `SpecConformance` 12/12；`grep` 无残留 `TOKEN_INT`/`TOKEN_DOUBLE`。
- 遗留问题 / 下一步：进入 Phase 3（LEX-15 消费端）。

## 2026-09-29 00:40 Task 10-13 完成：LEX-15 消费端（Token→AST→sema→codegen）

- 改动摘要：
  - Task 10（`cba2de1`）：`TokenValue` 整数改 `long long`；`Lexer.cpp` 存 64 位；`literalKindName(Int)`→`"int32"`；新增不截断测试。
  - Task 11（`9e182be`）：`NumberExprAST` 持 `long long value` + `literalKind`；`FloatExprAST` 持 `literalKind`；`Parser.cpp` 用 token 的 kind 构造；新增 parser 测试。
  - Task 12（`715a37d`）：`SemanticAnalyzer::typeForLiteralKind` 映射；`ConstValue::intVal` 改 `long long`；无后缀浮点默认 `float64`；新增 sema 测试。
  - Task 13（`6c618ab`）：`src/ast/Expr.cpp` 整数常量按宽度/符号、浮点按 APFloat 语义（half/single/double/quad）生成；新增 codegen 与 e2e 不截断测试；`conversions.md §7`、`TODO.md` 收尾。
  - 相关文件：`src/frontend/{Token.h,Lexer.cpp,Parser.cpp}`、`src/support/{LiteralKind.h,Utils.cpp}`、`src/ast/{Expr.h,Expr.cpp}`、`src/sema/{SemanticAnalyzer.h,SemanticAnalyzer.cpp}`。
- 验证方式与结果：`cd build && ctest` → **100% passed, 0 failed, 727/727**（2 个既有 Skip）；
  最终 review 结论 `MERGE: READY`、无新增 findings。
  - `grep -rnE '\b(int|float|double)\b' libs resources` → 空；
  - `grep -rnE 'TypeKind::(Int|Float|Double)\b|getInt\(\)|getFloat\(\)|getDouble\(\)' src tests` → 空。
- 遗留问题 / 下一步：
  - `tests/frontend/test_parser.cpp` 既有恒真断言（pre-existing，可接受）；
  - `EnumUnderlyingE2E.Int8EnumNegativeValue` 曾偶发一次 SEGFAULT（复跑 0/20，疑似 JIT flake）；
  - half/fp128 软件兜底与跨目标差异属 TYP-04，未纳入本轮。
  - 分支 `feature/fixed-width-types`（13 commits，HEAD `6c618ab`）待合并。

## 2026-09-30 遗留小项清理：ParserErrorTest 恒真断言

- 改动摘要：`tests/frontend/test_parser.cpp` 两处 `ASSERT_TRUE(errors.empty() || !errors.empty())`
  改为有意义断言：
  - `MissingEqualsInArrayInitReportsError`：改为 `ASSERT_FALSE(errors.empty())`
    （`int32 arr[10] {1,2,3}` 实证会报错，恢复路径报 `expected ';' after variable declaration` 等）。
  - `UnexpectedTokenInExpression`：改为断言错误信息含 `expected expression`
    （`return ;` 实证报 `unexpected token '', expected expression after 'return'`）。
- 行为边界实证：`void g() { return; }` 同样报错——当前解析器要求 `return` 后必有表达式，
  裸 `return;` 一律不支持（属 PAR-02/FUN-02 既有行为，本轮不改）。
- 验证方式与结果：聚焦测试 2/2 通过；全量 `ctest` → **727/727**；提交 `c50f993`。
- 遗留问题 / 下一步：裸 `return;` 是否应支持（void 函数）待在 PAR-02/FUN-02 条目下决策；
  继续推进 LEX-05 收尾（`1.`/`1.e3` 尾点形式、十六进制浮点规范决策）。

## 2026-09-30 LEX-05 收尾：结尾点浮点形式

- 改动摘要（提交 `55667f8`）：
  - `src/frontend/Lexer.cpp::scanNumber`：数字后紧跟 `.` 一律进入小数部分
    （原条件要求点后必有数字），解锁文法既有的 `1.` / `1.e3` / `1.f32` 形式；
    指数、后缀、LEX-17 分隔符校验逻辑复用，`1_.` / `1._5` 自动报 E0004。
  - `tests/frontend/test_lexer.cpp`：新增 `LexerTrailingDotFloatTest`（5 项：裸尾点、
    带指数、带后缀、`1.foo` 切分为 FLOAT+IDENT、分隔符负例）。
  - `tests/sema/test_semantic_analyzer.cpp`：新增 `TrailingDotFloatLiteralsAreAccepted`
    （lexer→parser→sema 冒烟）。
  - `docs/spec/grammar.ebnf`：删除"结尾点暂未接受"的已知差异注释，改为已实现说明
    （含 `1.foo` 切分语义）；`TODO.md` LEX-05 置 `[x]`，十六进制浮点 `0x1p3` 明确延后（YAGNI）。
- 决策：十六进制浮点不入本轮规范（用户批准）；`1.foo` 切分语义为有意行为（Rust 风格，
  与既有 `1.5.foo` 切分同类）。
- 验证方式与结果：TDD——6 个新用例先 RED（6/6 失败）后 GREEN；
  SpecConformance 12/12；全量 `ctest` → **733/733**。
- 遗留问题 / 下一步：裸 `return;`（void 函数）支持与否待 PAR-02/FUN-02 决策；
  接下来推进 TYP-04（浮点软件兜底）与 P1-01 剩余项的 brainstorm。

## 2026-09-30 TYP-04（部分）：print 全格式接入 f16/f128 + varargs 提升

- 探查结论：x86-64 上 f16/f128 的算术/比较/混合运算/转换、形参/返回、全局变量、
  struct 字段**均已可用**（LLVM 原生支持承担兜底）；真实缺口仅 print 格式化与 varargs 提升。
- 改动摘要（提交 `59c4bd3`）：
  - `src/ast/PrintFormat.cpp::builtinPrintKind`：`Float16`/`Float128` → `PrintArgKind::Float`。
  - `src/ast/Expr.cpp::promotePrintArg`：half → `FPExt` double（无损）；fp128 → `FPTrunc` double（精度截断）。
  - `src/ast/Expr.cpp::promoteVarArg`：新增 `Float16 → float64` 提升（C23）；`float128` 原样传递（C23 `__float128` 不提升）。
  - `docs/spec/conversions.md` §9 表格更新 + 新增 §9.1（print 浮点格式化与精度说明）。
  - `TODO.md`：TYP-04 置 `[~]` 并记录完成/延后边界；快照 735。
- 测试：`EndToEndTest.PrintFormatsHalfAndQuad`（f16/f128 × `{}`/`{:f}`/`{:.2f}`/`{:e}`）、
  `EndToEndTest.Float16VarargPromotesToDouble`（extern printf varargs）。
  实施中一次"失败"实为测试期望值笔误（第 4 参为 `1.5f128` 而非 2.5），归约排查后修正期望，非产品缺陷。
- 验证方式与结果：TDD 2 用例先 RED 后 GREEN；SpecConformance 12/12；全量 `ctest` → **735/735**。
- 遗留问题 / 下一步：跨目标（非 x86-64）软兜底验证挂靠 INF-05/TST-08；f128 精确十进制输出挂靠 FMT-13；
  接下来 brainstorm P1-01 剩余项（class 成员访问段 / union / Slice）。

## 2026-09-30 P1-01 之 C：union 补齐（AGG-17/AGG-03）

- 探查实证：union 声明/初始化/拷贝/布局已可用；真实缺口为匿名 union 成员不提升、
  `typedef struct/union` 别名成员访问失效（通用 bug，struct 同样命中）、聚合按值传参/返回整体未实现（struct 同样命中）。
- 改动摘要（提交 `28c6ecc`）：
  - `src/frontend/Parser.cpp`：`parseStructDecl`/`parseClassDecl`/union 体循环支持匿名
    struct/union 成员提升（C11 语义，parseType 内联定义路径已消费 `;`），名字冲突报错；
    新增 `isAnonymousAggregate`/`promoteAnonymousMembers` 助手。
  - `src/sema/SemanticAnalyzer.cpp` `visit(MemberAccessExprAST)`：`.`/`->` 均剥离
    `TypedefType` 后再判定聚合类型与查找成员。
  - `src/ast/Expr.cpp` `MemberAccessExprAST::codegen`：分派同样剥离 typedef（含指针 base）。
  - `tests/e2e/test_union.cpp`（新增 8 项）：初始化/拷贝/sizeof 最大成员/嵌套 union/
    数组成员/指针访问/tag-union/匿名提升（union+struct）/typedef 聚合。
- 实施注记：首次编辑的 else-if 落入 parseClassDecl（循环结构相同）而 parseStructDecl
  是简版循环——经 CLI 变体排查（匿名 union 在前时后续成员全丢）定位后补齐，union 体循环一并支持。
- 决策：union/struct 按值传参/返回归 MEM-14/TYP-27 单独项（非 union 特有）；
  文件作用域匿名 union 延后。
- 验证方式与结果：TDD——8 个新用例 3 个 RED（与探查缺口精确一致）后全 GREEN；
  SpecConformance 12/12；全量 `ctest` → **743/743**。
- 遗留问题 / 下一步：P1-01 剩余 B（class 访问段诊断/protected/static 成员）与
  A（Slice 完整实现）待 brainstorm。

## 2026-09-30 P1-01 之 B：class 访问段与访问控制（PAR-04/SEM-04/DEC-01）

- 探查实证（修正前轮误判）：`private 生效`是**假象**——真实 bug 是访问说明符截断类体
  （`public:`/`private:` 处 break，其后成员/方法全部丢失）；方法 + `this->field` 机制本身完好。
- 决策（用户批准）：**DEC-01 冻结——class 默认 private，struct 默认 public**；
  protected 本轮 ≡ private（继承落地前）；static 成员/嵌套类型延后（AGG-10/11）。
- 改动摘要（提交 `8f9d40a`）：
  - `src/ast/Type.h`：`enum class AccessLevel`；`ClassType::memberAccess` 平行表（fields 结构与 codegen 索引零影响）。
  - `src/ast/Decl.h`：`StructDeclAST::memberAccess` + `isClassDecl` 标记。
  - `src/frontend/Parser.cpp::parseClassDecl`：访问说明符分支（`protected` 为上下文关键字，需后随 `:` 消歧）；
    每成员记录级别（含匿名提升成员）；类体不再截断。
  - `src/sema/SemanticAnalyzer`：`isClass` 采纳 `isClassDecl`（修复无方法 class 被当 struct 的 bug）；
    `currentClass` 栈跟踪；类外访问 private/protected 字段/方法报 **E2009**（带 fix 建议）；
    未知成员仍报 "no member"。
  - `src/sema/Diagnostic.*`：注册 `SemPrivateMemberAccess` → **E2009**。
  - 快照：`tests/diagnostics/snapshots/private_member_access.txt`。
  - 既有测试迁移（不弱化断言）：8 处 class 补显式 `public:`（原意图=成员可访问，按新默认语义显式化）。
- 测试教训：单跑通过、合跑失败 → 全局 `TypeContext` 单例跨测试泄漏，新测试改用唯一类名（CA/CC/CD/SE）规避；
  首次 sed 迁移漏掉行尾带空格的 `class Math {`，全量回归抓出后手动补。
- 验证方式与结果：TDD——7 个新用例先 RED 后 GREEN；迁移后全量 `ctest` → **751/751**；SpecConformance 12/12。
- 遗留问题 / 下一步：protected 派生类放开（随 INH）；static 成员（AGG-10）、嵌套类型（AGG-11）；
  P1-01 之 A（Slice 完整实现）待 brainstorm。

## 2026-09-30 P1-01 之 A：TYP-12 Slice 完整实现

- 流程：brainstorming（architectural）→ spec（`docs/superpowers/specs/2026-09-30-slice-design.md`，`9b97c23`）→ writing-plans（`docs/superpowers/plans/2026-09-30-slice-implementation.md`，`9a6645e`）→ executing-plans 内联 7 任务。
- 冻结决策（用户批准）：D1 范围=下标读写+.len+退化（无范围切片/显式构造）；D2 越界不检查（UB）；D3 零初始化 `{null,0}`；D4 方案 A（内联转换点+规范 LLVM 类型）。
- 实现要点（`8f599b1`..`9d74a2f` 共 7 commits）：
  - `CodegenContext::getLLVMType` Slice 分支单例化（修复 "Slice"/"Slice.0" 类型分裂，Task 1 RED 实证 Verifier 拒绝）。
  - sema：`visit(ArrayAccessExprAST)`/`visit(MemberAccessExprAST)` 加 slice 分支；`conversionRank`/`typesCompatible`/`checkAssignmentTypes` 加 Array→Slice 退化；`typesEqual` 加 Slice 元素递归比较。
  - codegen：`CodegenContext::emitArrayToSliceDecay`（{ptr,N} 视图）；下标（load→extractvalue 0→GEP）；`.len`（extractvalue 1）；局部零初始化（store null）。
  - parser：`parseType` 的 `[]` 后缀循环化，支持 `int32[][]`。
- 测试：SliceE2ETest 6 项 + SliceSemTest 10 项 + 快照 1 项；全量 **768/768**（基线 751）。
- Rulings（执行中）：①MethodCall 传参退化不做（resolveMethod/mangle 以 arg->type 匹配，sema 不支持退化，单改 codegen 前后端不一致）→ TODO；②`int32 arr[2]` 形参语法不解析（TYP-11 缺口）且返回局部数组为悬垂 UB → 删该用例、退化分支保留；③typesCompatible 的 slice 分支须置于 kind==kind 早退之前；④全局变量 codegen 缺口（`int32 g;` 不生成符号，CLI 实证通用问题）→ 移除全局用例、挂 TODO。
- 终审：subagent 评审两次失败（R7/R7b 沿袭）→ 作者自审：无 Critical/Important；deferred minor 2 条（嵌套 slice codegen e2e 缺口、stripTypedefs 重复）。
- 遗留：MethodCall 退化、形参数组语法（TYP-11）、全局变量 codegen 缺口、范围切片/显式构造（YAGNI）——均挂 TODO。

## 2026-10-01 全局变量 codegen 缺口修复

- 根因：`VarDeclAST::codegen` 全局分支把 `InitVal=nullptr` 传给 `GlobalVariable`，LLVM 视为 external 声明（不分配存储）→ `int32 g;` 链接 undefined reference；有初始化器则正常（TYP-12 轮实证残留项）。
- 修复：`src/ast/Decl.cpp` 全局无初始化器时回退 `Constant::getNullValue`（零初始化定义，对齐 C tentative definition）；`extern` 变量语义范围外（语言无该标志，YAGNI）。
- 测试：新建 `tests/e2e/test_globals.cpp`（GlobalsE2E 4 项，含恢复的 TYP-12 全局 slice `{null,0}` 用例）；RED（3 FAIL/1 OK 精确预测）→ GREEN；全量 **772/772**。
- 验证：CLI 复现用例 `int32 g2;` 编译链接运行 exit=0。
- 遗留：MethodCall 传参退化、`return` 退化待 TYP-11 形参数组语法（见 TODO.md）。

## 2026-10-01 TYP-11 数组形参去糖 分支评审（subagent）

- 评审包：`.superpowers/sdd/2026-10-01-typ11-array-params/review-1639446..0c51cd0.diff`（4 commits，HEAD=0c51cd0，工作树一致）。
- 实证验证：e2e 4 项 + 快照绿；全量 ctest 计数 **783**；CLI 复核 `int32 a[]` 空括号去糖、多维诊断、悬垂拒绝（含全局数组同拒）均正确。
- 结论：实现本身无 Critical/Important 缺陷（D1–D4 一致，守卫 typedef 剥离正确，回归面干净）。
- **Important 1**：`FunctionPointerArrayParamDesugared` 空转通过——`int32 cb(int32 a[2])` 直接函数类型形参 parser 根本不支持（CLI 实证 `expected ')'`），analyzeOk 忽略 parse 错误 → AST 只剩 main → 恒真；Review Focus 4 未被真正钉住。真实共用路径（函数指针 `int32 (*cb)(int32 a[2])`，经 `parseFunctionPointerType`→`parseParamDecl`）去糖正确但无任何测试。
- **Important 2**：多维形参诊断文案（"multi-dimensional array parameters are not supported"）无测试断言，`MultiDimArrayParamRejected` 改前改后均绿，Review Focus 2 的差异化行为未被验证。
- Minor：诊断文案 spec/plan 不一致（"(dangling)" vs "(dangling view)"）且对全局数组同样报 "local array"；TODO.md P1-01（:511）未按 Task 3 Step 4 同步（"数组形参"已完成仍列为剩余）；`T name[]` 空括号去糖属未记录扩展（spec/测试未提）；spec §3.2 "node.type = nullptr" 与实现不符（ReturnStmtAST 无该字段）。
- 下一步：作者修复两个 Important（改显式管线 + 函数指针拼写；补文案断言）与 Minor 文档项。

## 2026-10-02 MethodCall 传参退化（TYP-12 遗留）

- 流程：brainstorming → spec（`docs/superpowers/specs/2026-10-02-methodcall-decay-design.md`，`4b346c7`，D1–D5）→ plan（`docs/superpowers/plans/2026-10-02-methodcall-decay.md`，`208d173`）→ Native 3 任务。
- 根因：①sema `resolveMethod` 按 Type* 指针同一性匹配（数组实参对 slice 形参必失配）；②codegen 按实参类型 mangle（定义点按声明类型）。
- 实现：`MethodCallExprAST::resolvedParamTypes`（含 this）；`resolveMethod` 两阶段（先精确后 conversionRank>=0，null argType 淘汰）；codegen 按 resolvedParamTypes mangle + `emitArrayToSliceDecay` 退化 + `castValue`。
- 测试：sema 5 项 + e2e 2 项（`tests/e2e/test_method_args.cpp`，MethodArgsE2E）；全量 **791/791**（基线 784）。
- 裁定：RED 表现为段错误（未实现通道 calleeFn 空解引用，既有缺陷）——按 crash=RED 继续；方法声明但未定义仍崩溃（现状即如此）挂遗留。
- 遗留：calleeFn 为 null 时无防护（既有）；重载最优匹配打分（YAGNI）。

## 2026-10-02 MethodCall 传参退化 分支评审（subagent）

- 评审包：`.superpowers/sdd/2026-10-02-methodcall-decay/review-4b346c7..7e5aa72.diff`（4 commits，HEAD=7e5aa72）。
- 实证验证：构建 + 全量 ctest **791/791**（3 连绿；首跑 1 次 `EnumStrongE2E.ExplicitCastIntToEnumAllowed` 偶发 SegFault，与本分支无关）；CLI 探针 5 项：null argType（`m.g(nosuchvar)`）正确拒绝不崩溃、跨类精确匹配仍胜过派生类 rank 候选、数组→指针形参新放行可用、继承链数组实参写穿透正确。
- 结论：实现无 Critical 缺陷；D1–D5 一致，resolveMethod 阶段 1 原样保留（精确等价），基类方法先拷入派生类方法表保证 phase 1 先于 phase 2 覆盖基类精确匹配。
- **Important 1**：D4 null-argType 守卫分支（SemanticAnalyzer.cpp:2140 `!argTypes[i]`）零测试覆盖——`MethodNullArgGuard` 实际只测 arity 失配（`m.g()` 时 argTypes 为空，循环体不执行）；Review Focus 5 的真实场景无防护。
- **Important 2**：Review Focus 3 的 codegen 主张（resolvedParamTypes 为基类声明形参 → mangle 命中基类定义）仅有 sema 级测试，test_method_args.cpp 无继承 e2e（plan 自身映射缺口）。
- Minor：phase 2 放行面为 conversionRank 全谱系（加宽/指针/null→指针），文档口径偏窄为"数组→slice"；spec D5"arg->type 非空时"括号限定未按字面实现（不可达，castValue 自身容错）；EnumStrongE2E 偶发 SegFault 建议另行跟踪。
- 下一步：作者补 null-argType sema 测试与继承 e2e 各 1 项；Minor 文档口径可选。

## 2026-10-03 多维数组本体（TYP-11 剩余之一）

- 流程：brainstorming → spec（docs/superpowers/specs/2026-10-03-multidim-arrays-design.md，6652f2f，D1–D5）→ plan（docs/superpowers/plans/2026-10-03-multidim-arrays.md，8bc8490）→ Native 3 任务。
- 冻结决策：D1 嵌套 ArrayType（[2 x [3 x i32]]）；D2 parser 维度链从右向左（parseVariableDecl + parseMemberArraySuffix，sema 零改动）；D3 首维推断；D4 codegen 零改动（六条递归通路复用）；D5 整体多维→slice 不放行/悬垂守卫自动覆盖。
- 实现外发现：全局数组带初始化器为既有缺口（一维亦崩、无测试覆盖）——ArrayDeclAST::codegen 补全局分支（镜像 VarDeclAST），一维全局顺带修复。
- 测试：sema 6 项 + e2e 4 项（tests/e2e/test_multidim.cpp，MultiDimE2E）；全量 **803/803**（基线 793）。
- 遗留：多维形参（语义待定：行 slice 的 slice / 扁平化）、VLA；行级初始化长度校验（范围外，1-D 亦无）。

## 2026-10-04 多维数组本体 分支评审（subagent）

- 评审包：`.superpowers/sdd/2026-10-03-multidim-arrays/review-6652f2f..5e1afda.diff`（4 commits，HEAD=5e1afda）。
- 实证验证：构建 + 全量 ctest **803/803**；IR 探针 3 项：`[2][3]` 建为 `[2 x [3 x i32]]`（维度顺序正确）、3-D `[2][3][2]` 初始化行主序且 `sizeof==48` 常量折叠正确、`a[2][]` 缺省内层维度被静默接受为 `[2 x [0 x i32]]`（OOB GEP）、`int32 g[2] = 5;` 全局被静默零初始化（`= 5` 丢弃，IR `zeroinitializer` 证实）。
- 结论：核心实现（D1–D5、维度右向左、1-D 等价、全局分支）无 Critical 缺陷；Review Focus 1–4 全部有布局敏感测试钉住。
- **Important 1**：非首维缺省维度被静默接受（`a[2][]`→`[2 x [0 x i32]]`，成员侧缺省为 1）——OOB 误码无诊断。
- **Important 2**：ArrayDeclAST::codegen 全局分支（Decl.cpp:407-415）静默丢弃非列表初始化器；sema :1741 以 elementType（而非数组类型）比对实参使 `g[2] = 5` 通过。
- **Important 3**：Review Focus 5 成员字段多维仅 sema 级验收，成员字段多维读写 codegen 无 e2e。
- Minor：全局 const 数组 ExternalLinkage 与 VarDeclAST const 标量 PrivateLinkage 不一致（镜像不完整）；parseVariableDecl 逆序循环 `i>=1` 依赖隐式保证；parseMemberArraySuffix 忽略 expect 失败（既有）；局部多维 sizeof / 3-D / 全局首维推断无回归钉；buildAggregateConstant 非常量元素静默补零（既有，VarDecl 同）。
- 下一步：作者补 1 个成员字段多维 e2e；Important 1/2 建议本分支或紧随轮修复（各 1 个拒绝用例 + 全局分支兜底）。
- 评审修复（dbcb685）：非首维缺省拒绝（防零长度行+越界 GEP）、数组标量初始化拒绝（防静默丢值）、成员字段多维 e2e。全量 806/806。

## 2026-10-04 多维数组形参（TYP-11 剩余之二）

- 流程：brainstorming（方案 A 行 slice，用户选定）→ spec（docs/superpowers/specs/2026-10-04-multidim-array-params-design.md，6f6acbe，D1'–D5'）→ plan（docs/superpowers/plans/2026-10-04-multidim-array-params.md，5c1c6e9）→ Native 执行。
- 语义：`T name[][3]` / `T name[2][3]` → `Slice<int32[3]>`；首维文档性、内维显式、行类型编译期严格比较；return 多维仍拒绝。
- 实现外发现：typesEqual 数组分支用恒 null 的 base 且不比长度——所有数组两两相等；修复为 size+elementType 比较（Symbol.cpp）。
- 测试：sema 5 项（MDP 前缀）+ 1 项旧 pin 改写（MultiDimParamParses）+ e2e 3 项；全量 **814/814**（基线 806）。
- 遗留：VLA（TYP-11 最后一项）；裸函数类型形参（parser 既有缺口，另立项）。

## 2026-10-04 多维数组形参——整分支评审（subagent）

- 评审包：`.superpowers/sdd/2026-10-04-multidim-array-params/review-6f6acbe..17bb427.diff`（4 commits，HEAD=17bb427）。
- 实证验证：构建 + 全量 ctest **814/814**（2 项 constexpr skip 属既有）；Review Focus 五条逐条核对均钉住（①写穿透 e2e ②行不匹配 sema ③1-D 既有 4 钉 + dims.size()==1 等价 ④内维缺省解析期拒 ⑤MethodCall e2e）。
- 裁定核查：a) typesEqual 数组分支修复成立——调用点仅 conversionRank/OverloadSet::resolve/Scope::declare（函数签名判重）与 SemanticAnalyzer 的 slice 元素比较（:250/:473），均无依赖旧"所有数组相等"语义的合法用例；结构化比较 + getSliceType 无缓存，无指针同一性陷阱。b) MultiDimParamParses 正向 pin 成立，语义接受由 MDPExplicitFirstDimAccepted 覆盖。
- **Important（既有，非本分支引入）**：数组-数组直接赋值 `a = b`（a: int32[2][3]，b: int32[2][4]）静默通过（SemanticAnalyzer.cpp:478 同 kind 兜底，不经 typesEqual），codegen 生成 `store ptr %decay, ptr %a`（把 b 的退化指针写进 a 的存储，覆盖 a[0][0..1]，无任何复制语义）——内存不安全。实测 IR 证实。建议另立项：sema 对 Array↔Array 赋值按形状相等拒绝。
- Minor：2-D 元素类型不匹配（float64[2][3]→int32[][3]）实测拒绝但无测试钉；3-D 形参无测试；`f(int32[][3])`/`f(int32[][4])` 现为两个可共存重载（typesEqual 修复的可见行为变化，spec 未规定）；MDPNonFirstDimRejected 未钉错误消息文本；`[4294967297]` 维度 int 截断（既有模式）；`m[2][0]` 零长度行接受（与声明侧 a[2][0] 一致）。
- 结论：核心去糖实现无 Critical 缺陷，裁定 a/b 均成立；数组赋值洞建议紧随轮修复。

## 2026-10-05 VLA 决策：明确不支持（TYP-11 完结）

- 流程：brainstorming → 用户质疑"不支持运行时长度是否更合理" → 技术论证支持 → 用户决策不支持 → 方案 1（专用诊断 + 关闭）。
- 论证：无界栈增长与安全定位冲突（CERT MSC34-C）；C23 已降为可选；Rust/Zig/Go/Swift 均无 VLA；动态长度由 Slice 承接（未来配合显式堆分配）。
- 实现：parseVariableDecl + parseParamDecl 两处 `[` 后非 NUMBER/`]` → "variable-length arrays are not supported: array size must be an integer literal"；pin 测试 2 项（VLA 前缀，钉消息文本）。816/816（基线 814）。
- TYP-11 完结（`[x]`）。遗留相关：数组-数组赋值缺陷（另立项，见 2026-10-04 条目）。

## 2026-10-05 数组赋值缺陷修复（bounded）

- 背景：2026-10-04 评审发现的既有缺陷——`checkAssignmentTypes` 同 kind 放行 + isPointerOrArray 混合放行，`a=b`/`a=p` 静默通过且 codegen 写退化指针进数组存储（内存破坏、无诊断）。
- 决策：C99 6.5.16 对齐——数组是不可修改左值，赋值左值为 Array 一律拒绝（含同形状）；不用"形状相等深拷贝"方案（YAGNI，复制走 memcpy/初始化列表/逐元素）。
- 实现：SemanticAnalyzer.cpp `checkAssignmentTypes` 单点，Array LHS → SemIncompatibleAssignment；`p=a`/`slice=a`/struct/union 拷贝不动。
- 测试：AASG 前缀 4 项（2 拒绝 + 1 退化回归 pin + 1 消息文本 pin）；RED 3 FAIL + 1 pin PASS 后转绿；全量 **820/820**（基线 816）。
- 遗留：无。下一候选：AGG-10 static 成员、AGG-11 嵌套类型。

## 2026-10-05 AGG-10 static 成员

- 流程：brainstorming（方案 A 类前缀全局符号去糖获批）→ spec（docs/superpowers/specs/2026-10-05-static-members-design.md，87c14ab，DS1-DS5）→ plan（docs/superpowers/plans/2026-10-05-static-members.md，641f7c4）→ Native 执行。
- 语义：`static T name[= init];` / `static T name(...)`；仅 `Class::member` 限定访问；类内初始化器=全局定义；不占布局；无 this；private 类外访问 E2009。
- 语义外发现：类体内 static 此前被 parser 静默丢弃；namespace 内类名被 parser 前缀化（与 sema 前缀双叠）——新增 StructDeclAST::bareName 修正。
- 时序要求：staticMembers 声明/生成必须先于方法体分析（sema 与 codegen 两侧同构），否则方法体内引用失败且编译器带病产出坏二进制。
- 测试：sema 10 项（SM 前缀）+ e2e 4 项（ClassCodegenE2E.SM*）；全量 **834/834**（基线 820）。
- 遗留：P1-01 仅剩 AGG-11 嵌套类型。

## 2026-10-05 AGG-10 评审修复

- 评审（subagent 整分支）：2 Critical + 2 Important + 6 Minor。
- C1（流程）：Task 2 的 sema 时序修复漏提交（commit 漏 add）→ HEAD 自带 e2e 必 FAIL；补提交 151e052。教训：commit 前 git status 核对；Ruling 声称两侧同构时两侧改动须同一 commit。
- C2：struct 仅含 static 成员时整个定义被解析器前向声明回退分支吞掉（泄漏为裸名全局、类型空注册、非常量初始化器可致编译器 SIGSEGV）——回退条件补 staticMembers/methods 判空；补 sema 2 项 + e2e 1 项。
- I1：同名 static/实例成员共享 memberAccess 裸名键后写覆盖 → E2009 误报——static 成员改独立键 "static:<name>"（parser 方法与变量两分支 + checkStaticMemberAccess）；补 2 项。
- I2：static 方法无法调用声明在其后的同类 static 方法——两遍处理：先统一改名+索引+declare 预注册（原型容忍），再统一分析体；补 1 项。
- 全量 **840/840**（834 + 6）。
- Deferred minors：static 数组成员诊断误导；半限定拼写拒绝未钉；protected static 经 Base:: 拒绝（已记 AGG-10 条目）；static+匿名聚合组合丢 static；Decl.cpp 注释与幂等事实不符；static 初始化器引用后声明成员（与全局一致）。

## 2026-10-05 16:00 — AGG-11 嵌套类型 + 前向声明 完成（R5 Native）
- 完成事项：类/struct/union 体内嵌套类型（enum/struct/class/union，任意深度）全链路——
  Parser 三路判定 + `bareName` 前缀压栈 + 解析期扁平键注册（`src/frontend/Parser.cpp`、
  `src/ast/Decl.h`）；sema `nestedTypes` 遍历 + `classPathPrefix` 去糖推广 + 嵌套枚举
  常量 `Outer::Red` + private 嵌套类型 E2009（var decl + cast 目标）
  （`src/sema/SemanticAnalyzer.cpp/.h`）；codegen 两分支递归生成（`src/ast/Decl.cpp`）。
- 验证：全量 `ctest` 863/863（基线 840 → 863，新增 sema 16 项 + e2e 7 项）。
- 提交：spec `4720650`+`4f2033d`，plan `a62bc30`，Task1 `25bc204`，Task2 `a0dcfb1`，Task3 `b4747c0`。
- 文档：abi.md §4 第 5 条（嵌套类型扁平编码）；TODO AGG-11/PAR-03/PAR-04/P1-01/AGG-07 勾记。
- 遗留：self-type 缺口（类体内裸/限定自引用类型，顶层类同样存在，是否立项待定）；
  EnumUnderlyingE2E.Int8EnumNegativeValue 偶发 SEGFAULT（LLJIT flaky 家族，约 2/6 全量频次）。

## 2026-10-05 17:30 — INH 继承链完成（R5 Native）
- 完成事项：单继承收口——Parser struct/class 继承子句 + INH-02 多继承诊断 +
  非公有继承诊断（`src/frontend/Parser.cpp`）；方案乙：删基类方法表复制，
  resolveMethod 沿 base 链查找 + definingClass 附带，方法 E2009 归属精确到
  定义类，私有基方法洞闭合（`src/sema/SemanticAnalyzer.cpp/.h`）；
  StructType 补 baseClass/base/isComplete（`src/ast/Type.h`）；codegen
  emitClassFieldGEP 泛化 + Struct 布局基类首字段（`src/ast/Expr.cpp`、
  `src/codegen/CodegenContext.cpp`）；`src/ast/Decl.h` 加 isForwardDecl。
- 验证：全量 ctest 890/890（基线 868 → 890，sema +14、e2e +8）。
- 提交：spec `b7a5287`，plan `d48f97e`，Task1 `265244a`，Task2 `951b4d0`，Task3 `ae6107c`。
- 裁决要点：缺省继承一律 public（class 亦然，偏离 C++ 记文档）；访问级别
  判定式零改动（protected 放宽未做，另立项候选）；struct 带基类复用 parse 期
  StructType 对象（变量 Type* 同址）；派生自不完整基类诊断（isComplete）。
- 遗留：INH-05 CRTP 随 GEN；protected 放宽另立项候选；LLJIT flaky 家族。

## 2026-10-05 18:40 — Redef 类型重复定义诊断收口（bounded）
- 完成事项：struct/class/union/enum 四类重复定义诊断（此前全静默，是 INH
  评审 C1 的根因缺口）——sema 定义路径检查 `isTypeRedefined`（四张注册表
  按 name 查 complete，跨种类同报）+ E2004 SemRedefinition 复用（func 重
  定义同码）；`UnionType/EnumType` 补 isComplete、`UnionDeclAST/
  EnumDeclAST` 补 isForwardDecl（parser 前向分支打标）；union visit 改复用
  既有注册（镜像 struct 分支）。
- 保持合法（pin）：fwd+def、fwd×2、不同外层同名嵌套 inner。
- 落点：`src/ast/Type.h`、`src/ast/Decl.h`、`src/frontend/Parser.cpp`、
  `src/sema/SemanticAnalyzer.cpp/.h`；测试 sema `Redef*` 8 项 + e2e 1 项。
- 验证：RED 5 红（四类 + 跨种类）3 绿（pin）；GREEN 后全量 ctest 902/902。
- 裁决：e2e fixture runSource 对任何 sema 错误 ADD_FAILURE（正向专用），
  错误路径按 INHMultiInheritDiagE2E 先例走显式管线；内存 Diagnostic::
  format() 不含码位（E2004 由驱动打印时附加），断言用消息文本。
- 遗留：无（INH 评审 C1 根因就此闭合）。

## 2026-10-06 — P1-02 Optional/Result（TYP-13/14、STD-02、DEC-03）
- 完成事项：Optional/Result 内建魔术类型端到端——parser 类型位置尖括号
  （`Optional<T>`/`Result<T,E>`，`>>` 原地拆分，isTypeStart 同步）；类型
  相等严格化（typesEqual/compatible/checkAssignmentTypes 按实参）；布局
  `{i1,T}`/`{i1,T,E}`（valid/ok 在前，修 SliceType 误转换，具名结构体幂
  等）；伪字段 `.valid/.ok/.value/.error`（可写）；聚合初始化校验 + 
  codegen（GEP/ExtractValue/常量）；e2e 7 项。spec + plan 在 docs/
  superpowers/。
- 关键裁决：getLLVMType 幂等（getTypeByName 先查，防实例漂移）；flag 字
  段走 bool 赋值通道（true/false 是 int32 字面量）、value/error 字段 
  typesEqual；`{5, true}` 类型层不可拒（C 语义合法）；嵌套 {} 跳过 sema
  校验（struct 先例）；`T??` 双后缀不承诺。
- 落点：`src/frontend/Parser.cpp`、`src/ast/Symbol.cpp`、
  `src/ast/Expr.cpp`、`src/ast/Decl.cpp`、`src/codegen/CodegenContext.cpp`、
  `src/sema/SemanticAnalyzer.cpp`、`docs/spec/{abi,conversions,stdlib}.md`、
  `TODO.md`；测试 sema +16、layout +3、e2e +7。
- 验证：全量 ctest 927/927（902 → 927，每任务 TDD RED→GREEN）。
- 遗留：`T??`/return 位置 {} 推断未做（后者为既有全局行为，struct 同）；
  最终评审待做。

## 2026-10-06 — P1-02 Optional/Result 最终评审（独立评审人）
- 完成事项：对 b5e80a0..db00f7d 全量评审——Review Focus 5 项均有测试且
  通过（复核确认，未重复报告）；聚焦测试未覆盖区，实机构造 20+ 复现
  用例（/tmp/opencode/t*.cdd，本机 cdd -S 编译验证）。
- 发现（详见评审报告）：**Critical ×1**——LLVM 具名结构体以
  `typeToMangled` 作布局身份，而 enum 一律 mangle 成 "int32"
  （Mangle.cpp:43）且 `_` 分隔可被 struct 名内下划线消歧失败：
  `Optional<enum:u8>` 与 `Optional<enum:u64>` 共享 `{i1,i8}`，对 2 字节
  alloca 发 8 字节 store（栈腐坏，t2 复现）；`Result<A_B,C>` 与
  `Result<A,B_C>` 同名（t3c）；用户 struct 名 `Optional_int32` 与内建撞
  名（t15b OOB GEP）。Important ×5：比较/逻辑算子对 Optional 静默放行
  → ICmp 断言崩溃（SemanticAnalyzer.cpp:445/452，isStructOrUnionType
  未含新 kind）；typedef 实参/typedef 初值被 init-list 校验误拒
  （:1872 typesEqual 不剥 typedef）；typedef 返回类型 + 调用点取伪字段
  → `load %Struct, %Struct %v` 非法 IR 崩溃（Expr.cpp:988 band-aid）；
  三元分支不校验（getCommonType 恒取左）→ phi 断言/地址当值存（根因
  既有，标量同样中招 t4）；rvalue 伪字段写 → 非法 store 崩溃
  （sema isLValue 无条件 true，struct 同病）。Minor ×6：init-list 双
  重诊断、间接调用无参检、`Optional <` 遮蔽变量、`.ok/.valid` codegen
  混映射、return 位 `{}` 不可用（已裁决遗留）、`int32??` 诊断差。
- 验证：全量 ctest 927/927 复跑通过（2 项 skip 为既有）；每个 Critical/
  Important 均附最小复现与建议测试。
- 结论：**fix-first**——C1（布局身份碰撞）与 I1（比较算子崩溃）须先修。
- 遗留：无（评审交付，修复另立任务）。

## 2026-10-06（续）— P1-02 最终评审与修复
- 评审：subagent（glm-5.3）全分支评审，verdict fix-first——C1（布局身份键
  非单射 → 静默栈腐坏）、I1（比较/逻辑算子崩编译器）、I2（typedef 误拒）、
  I3（typedef 返回 rvalue 成员崩后端）、I4（三元聚合分支）、I5（rvalue 赋
  值）单轮修复，测试 +13，全量 940/940。
- 遗留（评审 minors，挂账待裁决）：M1 双重诊断、M2 函数指针无参检（既有）、
  M3 `Optional <` 歧义、M4 codegen 混映射、M5 return 位 {}、M6 T?? 文案；
  三元标量根因独立缺陷轮。

## 2026-10-06（续）— 三元根因缺陷轮
- 完成事项：TernaryExprAST codegen 分支取值（lvalue loadValue，数组除外）
  + getCommonType 统一 cast + phi 前驱记实际终结块；sema 撤销 I4 拒绝并
  新增分支兼容性诊断；顺带修解析后缀优先级（parseUnaryImpl 一元分支补
  parsePostfix，-a.v/-arr[i]/++a.v 曾误解析为 (-a).v/(-arr)[i]）。
- 落点：src/ast/Expr.cpp、src/sema/SemanticAnalyzer.cpp、
  src/frontend/Parser.cpp；测试 tests/e2e/test_ternary.cpp（11）+ sema 1。
- 验证：全量 ctest 952/952（940 → 952，TDD RED→GREEN）。
- 遗留：数组作三元分支（退化语义）未处理；Optional/Result 全局零初始化
  已绿；M1-M6 及 LLJIT flaky、P1-03 泛型待后续轮。

## 2026-10-06（续2）— P1-03 泛型 + CRTP
- 完成事项：函数/类/别名模板 + 非类型参数（仅整数）+ 显式/推导实例化 +
  编译期单态化 + this 表达式 + CRTP 全链路。模板定义只 parse 存 AST；
  使用点惰性触发 TemplateInstantiator（AST 深克隆 + TypeVar 重写），
  TemplateRegistry 键去重/状态机/深度上限 64；实例以普通声明走既有
  sema/codegen；诊断附 `in instantiation of template`。
- 落点：src/frontend/{Token,Lexer,Parser}.h/.cpp、src/ast/{Type,Decl,Expr,Mangle}、
  src/sema/{TemplateRegistry,TemplateInstantiator,SemanticAnalyzer}、
  docs/superpowers/specs/2026-10-06-generics-crtp-design.md、plan 同目录 plans/。
- 验证：全量 ctest 992/992（952 → 992，每任务 TDD RED→GREEN）。
  测试 +40：parser 6、registry 6、sema 13、e2e 15。
- 遗留：模板与模块 import 组合未测；别名模板嵌套展开缓存为进程级；
  struct 模板方法（struct 无方法语义）不支持；`compile_time` 语境的
  非类型参数（GEN-02 后半）随 P1-04；M1-M6 minors 挂账不变。

## 2026-10-06 21:40 — P1-04 compile_time 核心纵向切片完成

- 完成事项：CT-01（解析层前瞻特判）、CT-02（static_assert 顶层+函数体）、CT-03（compile_time.if 条件编译+死分支类型毒化）、CT-04/05（target/build 查询）、CT-06（CompileTimeEvaluator 求值器：ConstValue+STR、运算、size_of/align_of/offset_of 布局查询）、CT-12（ctInt/ctFloat/ctHandled → llvm::Constant 零指令）、CT-14/DEC-05（并存+共享内核）、PAR-15/SEM-07（部分/完成）。TODO.md 收口 14 条。
- 关键文件：`src/sema/CompileTimeEvaluator.{h,cpp}`（新）、`src/sema/SemanticAnalyzer.{h,cpp}`（钩子/守卫/毒化）、`src/ast/{Expr.h,Expr.cpp,Decl.h,Decl.cpp}`、`src/frontend/Parser.{h,cpp}`、`src/driver/CompilerDriver.cpp`、`tests/{frontend,sema,e2e}/test_compile_time*.cpp`（新 3 文件 32 用例）。
- 验证：ctest 1032/1032（基线 999 + 33）；每任务 TDD RED→GREEN，全量绿后提交。
- 修复：filter-branch 重写 7 个未推送提交以清除误提交的 in-source CMake 产物（138k 行→1.9k 行）；.gitignore 补防复发规则。
- 遗留：反射 CT-07/08/13 另轮（依赖 P1-06 str）；两分支同名类型变体选择不支持；函数参数/返回值位置未检查毒化类型；OptionalBranchExec 偶发 SEGFAULT（判定 LLJIT 压力 flaky）。

## 2026-10-06 22:20 — P1-04 终审与修复 pass

- 终审（fresh reviewer subagent）：0 Critical / 5 Important / 4 Minor。
- 修复（每项 RED→GREEN）：I1 constexpr CT 实参互递归（新增 `evalConstexprCallCT`）；I2 CHAR 误判 STR 逃逸；I3 毒化覆盖（typedef/指针/数组/字段/参数/cast）；I4 同名变量消歧（作用域感知）；I5/M3 测试补 pin 与断言强化。
- Ruling：M1（size_of→usize）实测引发 E2007 重载匹配回归，回退挂账。
- 验证：ctest 1037/1037；新增测试 38 个（parse 4 + sema 21 + e2e 13）。
- 遗留：CT-07/08/13 反射另轮；M1/M2/M4 deferred；OptionalBranchExec 偶发 SEGFAULT（LLJIT flaky）。

## 2026-10-07 22:19 P1-05 注解系统（ANN）代码评审（子代理会话）

- 评审范围：`review-592049c..29935c2.diff`（P1-05 六提交）、spec `docs/superpowers/specs/2026-10-07-annotations-design.md`、plan `docs/superpowers/plans/2026-10-07-annotations.md`。
- 方法：通读 diff 与相关源码（LayoutBuilder、SemanticAnalyzer、Parser、CodegenContext、CompileTimeEvaluator），并用基线 worktree（592049c）+ 现有构建实证复现可疑路径；验证后已删除 worktree、恢复全部临时插桩（工作区 clean，`git diff` 为空）。
- 主要结论（详见评审输出）：
  1. **Critical（已复现）**：`[[packed]]`/`[[align]]` + 带基类 struct + `compile_time.size_of` → `report_fatal_error`（self-check failed），编译器崩溃；根因 `src/ast/LayoutBuilder.cpp:55-104` 物化时基类伪字段进 `padded` 但 cursor 从 0 起算，且 106-118 行又显式推一次基类（双基类）。
  2. **Critical（已复现，回归）**：无注解 `struct S { int32 x; void m() {} }` 在基线 592049c 编译通过，HEAD 上 SIGTRAP（`DataLayout::getPrefTypeAlign`）——LayoutBuilder 对所有聚合急切计算 DataLayout 布局，layout 非法字段（struct 成员位置的 `void m()` 被解析为 void 字段）从"惰性容忍"变为崩溃。
  3. **Important（已复现）**：带基类的注解 struct 走 codegen 路径时 `StructDeclAST::codegen` 的 `isClass = !methods.empty() || !baseClass.empty()` 误路由到 `getClass`，取不到 StructType → 回退旧路径，packed/align **静默丢失**（IR `{ %B, i16 }` 非 packed），与 CT 查询结果互相矛盾。
  4. **Important**：类型级 `[[align(N)]]` 未传导到该类型变量的对齐（`A l;` alloca align 8，`align_of(A)==64`）——违反 spec §3 与 §4 硬约束。
  5. **Important**：W3004 键为裸名且 `visit(MethodCallExprAST)` 无检查 → `this->method()`/`obj.method()` 弃用方法漏报（spec §5 明列 MethodCall）；裸名键导致跨命名空间/遮蔽误报；deprecated 类型用于字段/参数/数组未检查（visitStructDeclImpl 字段环只查 ctDeadBranch）。
  6. 其余 Minor：W3005 只查 NumberExprAST 字面 0（未走 evaluateConstexpr）；offset_of 死代码双环；TU 层 module 特判 rewind 可能造成诊断重复；`new ArrayType` padding 类型泄漏（与代码库风格一致）；using/type 声明注解不校验。
- 验证方式：`build/bin/cdd` 对 8 个探针源文件编译/运行/IR 检查；gdb 定位崩溃帧；基线 worktree 对照构建；annotation 相关 gtest 40/40 通过。
- 遗留问题：上述 Critical/Important 需修复后复审；struct 成员位置方法解析语义（是否允许 struct 带方法）需裁决。

## 2026-10-07 — P1-05 注解系统完成

- 完成事项：ANN-01（解析/AST 挂载，六挂载点）、ANN-06（E2010~E2014）、ANN-02/03（repr(C)/packed/align，LayoutBuilder 单源化）、ANN-04/05（inline/cold/nonnull LLVM 属性 + deprecated W3004 + nonnull W3005）、LEX-10/PAR-07；TODO 收口 14 条。
- 关键文件：`src/ast/{Annotation.h,LayoutBuilder.{h,cpp},Type.h,Decl.h,Decl.cpp,Expr.cpp}`、`src/frontend/Parser.{h,cpp}`、`src/sema/{SemanticAnalyzer.{h,cpp},Diagnostic.{h,cpp},CompileTimeEvaluator.cpp,TemplateInstantiator.cpp}`、`src/codegen/CodegenContext.cpp`、新测试 4 文件 46 用例。
- 终审（fresh reviewer）：2C/5I/7M 全部 Critical/Important 修复（每项 RED→GREEN）；途中发现并修复跨 kind 静态转换读垃圾值（C1 同款）与 evaluator 缺 Array case。
- 验证：ctest 1083/1083（基线 1037 + 46）。
- 遗留：ANN-07/08、方法 nonnull this 偏移、裸名键误报/漏报、offset_of 继承字段、M2/M3/M4/M5/M6/M7 minors。

## 2026-10-08 22:45 — P1-06 str/String 核心纵向完成
- 完成 T1~T6（Native 内联，master 直接开发）：
  - T1 `optional`/`result` 小写化（`d6366a9`）
  - T2 str/string 类型骨架 + 字面量重定型（`9db7cb0`）
  - T3 str 方法 + str_from_c（`1b3e3b4`）
  - T4 string 内存与方法（`c32d123`）
  - T5 UTF-8 验证 E2015（`68a328b`）
  - T6 硬化边界（`7347a34`）
  - T7 TODO 收口（本次）
- 验证：全量 ctest 1123/1123 绿（基线 1077 + 新增 46）；每任务 RED→GREEN 全程亲见失败。
- 涉及：`src/ast/{Type,Expr,Symbol,PrintFormat}.{h,cpp}`、`src/frontend/Parser.cpp`、`src/sema/{SemanticAnalyzer,CompileTimeEvaluator,Diagnostic}.{h,cpp}`、`src/codegen/CodegenContext.{h,cpp}`、`src/support/Utf8.{h,cpp}`（新）、`tests/e2e/test_string.cpp`（新）。
- 遗留：format（FMT-06~09）与 STD-10 另轮；终审待派。

## 2026-10-09 20:41 — 终审修复轮复审（commit 3f17eb7）
- 复审四个修复（只读验证，工作区 HEAD = 3f17eb7，构建最新）：
  - C1（typesCompatible 单向 str↔char*）：已修。`str s = p;` 与 `return p;` 被拒，`char* p = s;` 仍过；函数参数路径经 conversionRank 未动。测试 CharPointerToStrStillExplicit / LiteralToCharPointer / StrFromCAcceptsCharPointer 3/3 绿。
  - I1（str ==/!= 条件 memcmp）：已修。condBr + PHI 结构正确（lenEq→cmpBB memcmp→doneBB；不等→doneBB 常量 false；Ne 取反），Eq/Ne 均正确。
  - I2（F5..F7 拒绝）：已修。实证：F5 80 80 80 00 缓冲经 str_from_c → "panic: invalid UTF-8 sequence" + abort（exit 134）；F0 9F 8E 89（🎉）→ exit 0。IR 中 f4bound = !is4 || lead<=0xF4 就位。
  - I3（append 自别名）：已修（覆盖语言可表达的别名形态）。谓词 sptr∈[ptr,ptr+cap)（UGE/ULT GEP），自别名后重投影 ptr2 + memmove；非别名仍走 sptr。测试 AppendSelfAlias/AppendExec/AppendGrowthExec/AppendAcceptsString 4/4 绿。残余：若未来引入带偏移的 str 子视图（s[k..]），重投影会丢偏移——当前语言无此构造，不算缺陷。
- 验证：目标 gtest 7/7 绿；全量 ctest -j4：1123/1125，2 个 ModuleVisibility 失败为并行 LLJIT 抖动（隔离重跑均过，与已知 flaky 同类）；ConstexprFunctionE2E 2 个为源码内 GTEST_SKIP（既有遗留）。
- 结论：通过。新引入问题：无阻塞项；轻微不对称——三元分支 `cond ? str : char*` 现被拒（`? char* : str` 可过），系单向规则的连带效应，可接受。

## 2026-10-10 P1-09 字符串库 + format 体系（本轮主体）

- **完成**：FMT-06~11 全落地、FMT-12 单缓冲、STD-10 组合库、STD-11 核心。
  - PrintSpec 解析器（`src/ast/PrintFormat.{h,cpp}`），print/println/format 三者共享；E2020~E2022 稳定诊断码。
  - `format()` 内建：sema 全检（字面量）+ codegen 字面量直拼 / 动态 `cdd.format.dyn` 扫描（单缓冲一次 malloc）。
  - str 原语内建：`find`/`rfind`/`sub`/`split`/`split_destroy`（零拷贝视图 / malloc 段数组）。
  - `libs/std/string.cdd`：contains/starts_with/ends_with/trim*/join/concat/repeat/utf8_sub。
- **顺带修复既有编译器 bug**（`e878bdb`）：`&&`/`||` 按位降级 → C 语义短路 + 判零真值；派生→基类赋值整结构 store 溢出 → castValue 沿继承链截取基子对象。
- **挂账**：StrBuilder 类跨模块导出缺口（语言层，parse 期类型名注册先于 import）；`replace`/字典序/to_lower|upper 不做（spec §8）；std.mem/math/collections 另轮；终审 I4 仍在账。
- **文档**：`docs/spec/stdlib.md` 新增 §9 std.string / §10 std.format；`docs/spec/abi.md` 补 str/string 布局行（终审 M5 清账）；TODO.md FMT-05~12/STD-09~11/P1-09 收口。
- **验证**：全量 `ctest` 1180/1180 绿（含新增 PrintFormat 32 用例、format e2e 20 用例、split/split_destroy、StdStringLib 7 用例、&& 短路 e2e）；3 个已知 flaky 隔离重跑通过。
- **留痕**：spec `docs/superpowers/specs/2026-10-09-p109-string-format-design.md`（853bc1e），plan `docs/superpowers/plans/2026-10-09-p109-string-format.md`（66ff5c5），执行 ledger `.superpowers/sdd/2026-10-09-p109-string-format/`。

## 2026-10-10 20:13 — P1-09 终审（whole-branch review, 66ff5c5..efb2149）
- 复审范围：diff 全文 + 实机验证（build/bin/cdd 逐例编译/运行）。
- **Critical 2 项**（均实证）：
  1. `src/ast/Expr.cpp:378` 短路 `&&`/`||` 的 PHI incoming 块硬编码 `rhsBB`——RHS 自身产生基本块（嵌套 `&&`/`||`、str `==`/`!=`、三元）时 PHI incoming 指向非前驱块，IR 非法，`-c`/JIT 段错误。复现：`a && s == "x"`、`a || s != "S"`、`a && (b || c)`。修法：RHS 求值后以 `GetInsertBlock()` 作 incoming 块。无测试覆盖（既有 e2e 仅调用型 RHS）。
  2. `src/ast/Expr.cpp:819`（`renderScalar`，动态 format 路径）chunk 长度取 snprintf「本应长度」（SExt）而缓冲仅 64 字节——实参超长（如 to_string > 63 字节）时 memcpy 越界读栈 + 结果混入垃圾字节。复现：76 字符 to_string → len=76 仅 63 字节有效。同族：字面量路径 `:891`（bufSize=344+）、print 渲染槽 `:184`（bufSize=512+）。修法：长度 clamp 至 bufSize-1。
- **Important 2 项**：
  3. `src/sema/SemanticAnalyzer.cpp:1909` `split_destroy` 接受 `[]string`（split 永不产生）→ 栈数组 decay 后传入即 `free` 栈指针（复现 abort）。应只接受 `[]str`。
  4. 基类截取修复（e878bdb）无任何回归测试钉死；且 `src/codegen/CodegenContext.cpp:399` 链上找不到目标时静默 fall-through 回通用 castValue（旧溢出路径）。实机验证单继承/三级链/含 string 基类均正确，多继承被 sema 拒绝——当前可达路径安全，但缺测试与防御性失败。
- Minor：print 渲染槽 str 含内嵌 NUL 截断（format 路径字节保真，不一致）；"unterminated '{'"/"single '}'" 误归类 E2020；`{:#x}`/`{:F}` 等旧合法规格收窄未在计划文档化（`{:d}`/`{:g}` 已文档化）；`emitPaddedValue` 大 str 居中即全量栈 alloca；`renderScalar` 对 snprintf 负返回值未 clamp（与 pad 路径不一致）；`getSplitFn` 硬编码 16 字节 stride。
- 核实为正确：coerceToBool（bool/int/float NaN 真值/指针）；castValue 同型赋值在走查前短路（string→string、无关 struct 不受影响）；memcpy 返回值不链式（三处合成 helper 均显式 GEP 偏移）；getStrFindFn/RFind 边界（空 needle 0/len、过界 -1）；getSplitFn 贪心非重叠计数与填充一致；getFormatDynFn 单 malloc 上界、块支配关系正确；print 旧行为回归（空 spec 表逐字一致、`%%`、`%.*s`、错误文案）。
- Review Focus 1~5：1（%b 宿主可用、依赖已文档化）、2（FormatDynSpecIgnored 钉死）、3（FormatStrNulPreserved，print 渲染槽除外→Minor）、4（unit+e2e 全绿）、5（文档化不测试）——全部按计划处置。
- 结论：**不通过（阻塞：Critical 1/2）**。修复面小（两处 codegen 数行 + sema 一处），建议修复后补 3 个 e2e（嵌套逻辑 RHS、长 to_string 动态 format、`B b = d;` 值赋值）再复验。

## 2026-10-11 11:00 — 环境恢复 + 全量构建验证 + LEX-12 测试期望修正

- **背景**：工程从 `/media/nvme0n1/...` 迁到 `/media/nvm1/workspace/My_LLVM_C`，旧磁盘已不存在。in-source 残留（根目录 `CMakeCache.txt`/`CMakeFiles`/`Makefile`/`cmake_install.cmake`/`CTestTestfile.cmake`/`Testing`/`src|tests/Makefile`，以及 `src/CMakeFiles`、`tests/CMakeFiles`）与 `build/` 缓存全部指向失效路径，`scripts/build.sh` 直接报 "CMakeCache.txt directory ... is different"；同时 LLVM 21 开发包缺失。
- **环境（用户安装）**：`llvm-dev`（→ `llvm-21-dev` 21.1.8 + 无版本 `llvm-config`）、`libcxxopts-dev 3.3.1`、`libgtest-dev 1.17`；`libspdlog-dev 1.15`、gcc 15.2、cmake 4.2.3、`ld`/`ld.lld` 原已具备。实测 `find_package(LLVM/GTest/spdlog/cxxopts)` 四项全部命中系统包，构建不再走 FetchContent 联网下载。
- **清理与重建**：删除全部失效 in-source 残留与 `build/`，`bash scripts/build.sh` 全量重建 → **BUILD_EXIT=0**（`build/bin/cdd`、`build/bin/compiler_tests`）。
- **唯一失败项定位**：`tests/frontend/test_lexer.cpp::AsmKeyword.LexesAsmVolatileTemplate`（未提交 WIP，对应 **LEX-12**）。gtest 打印 `<69-00 00-00>` 是**十六进制** = 0x69 = 105 = `TOKEN_LPAREN`，易误读成十进制 69（`TOKEN_PERCENT`）。独立复现（直接编译 `Lexer.cpp`+`Diagnostic.cpp` 打印 token 流）证实 lexer 正确：`asm volatile ( "nop" ) ;` → `[0]ASM [1]VOLATILE [2]LPAREN [3]STRING [4]RPAREN [5]SEMICOLON`，`diagnostics=0`。
- **根因**：测试期望写错两处 ——（1）漏算 `(` ，字符串实际在 `tokens[3]` 而非 `tokens[2]`；（2）假设字符串 lexeme 不含引号，实际**含两端引号**（既有约定，`src/frontend/Parser.cpp:575` 用 `lex.front()=='"' && lex.back()=='"'` 剥除后 `decodeEscapes`）。
- **修复**：仅修正测试期望（`ASSERT_EQ(size,6)` + 补 `LPAREN/RPAREN/SEMICOLON` + lexeme `\"nop\"`），**未改 lexer/Token.h**；同时清掉 `src/CMakeFiles`、`tests/CMakeFiles` 两处 451M 的 in-source 旧对象目录。
- **验证**：`ctest -j8` → **100% tests passed, 0 tests failed out of 1184**（CTEST_EXIT=0）；2 个未运行项为既有 `GTEST_SKIP`（`ConstexprFunctionE2E.ConstexprForLoop/NestedCall`，源码注明 constexpr for/嵌套调用会挂起）。期间 `ModuleVisibilityTest.NonExportedFunctionIsHidden` 曾在 `-j12` 下失败一次，为**已知并行 LLJIT flaky**（同 2026-10-09 条目记录），隔离重跑通过。
- **遗留**：根目录 `bin/`（137M）、`lib/`（85M）、`a.out` 仍为旧路径构建产物，未删除（非 git 跟踪，见 `.gitignore`）；**LEX-12** 在 `TODO.md` 仍为 `[ ]`，本轮只覆盖词法 token 层，parser/codegen 的 `asm` 尚未实现。

## 2026-10-11 — P1-08 内联 asm 全链路完成（LEX-12/PAR-16/CG-10/DEC-09）

- **流程**：brainstorming → spec（`docs/superpowers/specs/2026-10-10-p108-inline-asm-design.md`，70382f4）→ plan（`docs/superpowers/plans/2026-10-10-p108-inline-asm.md`，971586f）→ executing-plans Native 内联 5 任务（master 直接工作，与 P1-06/P1-09 同惯例）。
- **Task 1（词法）**：`TOKEN_ASM` + keywordMap `{"asm", ...}`；提交 `9fe660e`。Ruling：plan 自带测试片段与实际 token 流不符（`asm volatile ( "nop" ) ;` 共 6 个，字符串在 `tokens[3]` 且 lexeme 含引号），已同步修正 plan 与 brief。
- **Task 2（AST+解析）**：`AsmOperand`/`AsmExprAST` + `parseAsmExpr()`（至多三段操作数、模板/约束须字符串字面量、`decodeEscapes` 去引号）；提交 `943527f`。Rulings：纯虚 codegen 需占位定义才能链接（Task 4 替换）；`AsmTemplateMustBeLiteral` 改断言 `getErrors()` 非空（`Parser::parse()` 永不返回 nullptr）。
- **Task 3（sema）**：E2023（操作数/约束）/E2024（模板转义）+ `visit(AsmExprAST&)`；提交 `582e21c`；变异验证 4 负向用例恰好转红。Ruling：`TEST` → `TEST_F`（gtest 禁同 suite 混用）。
- **Task 4（codegen+e2e）**：提交 `9d55994`；e2e 6 用例 LLJIT 执行通过。**实证 LLVM 21 与 GCC 三处关键差异并适配**（llc 21.1.8 + LLVM 源码验证）：
  1. 输出必须映射 call 返回值（单值/`{T,T}` struct 逐位回写）——plan 原案「`=r` 传左值地址」被 LLVM 拒绝（"number of input constraints does not match"）。
  2. LLVM 21 无 `+` 前缀（`Parse` 只认 `~`/`=`/`!`）——`+r` 翻译为 `=r` + 追加在全部普通输入之后的 tied 数字约束（`"=r,r,0"`），实证 GCC `%N` 编号与 LLVM `$N` 完全对齐（ReadWriteOperand `add %1,%0` → 5+3=8）。
  3. 模板转义方言不同（LLVM 只认 `$N`/`$$`）——GCC 模板翻译 `$`→`$$`、`%%`→`%`、`%N`→`$N`。
- **顺带修复既有潜在 UAF**：`CodegenContext::takeContext()` 把 context 移给 LLJIT（编译后经 `ThreadSafeContext::State` 销毁）后，IRBuilder 的 `CurDebugLocation`（TrackingMDRef）仍引用其中 DILocation → 析构 `MetadataTracking::untrack` 读已释放内存，间歇 SIGSEGV。证据：gdb（关 ASLR）下 `InlineAsmE2E.ReadWriteOperand` 15/15 崩、其余 asm 用例 0/15；4 用例 DebugLoc MD 内存全是释放块填充；`LLVMContext` 析构经 `ThreadSafeContext` 触发；占位 codegen 时该用例也崩（非本次引入）。修复：`takeContext()` 移出前 `builder.SetCurrentDebugLocation(llvm::DebugLoc())`，修复后 gdb 20/20 不崩；32 处测试 harness 共用该 API 一并受益。
- **Task 5（文档收口）**：`keywords.md`（asm `[plan]`→`[impl]`）、`grammar.ebnf`（asm 产生式 `[impl]`）、`TODO.md`（LEX-12/PAR-16/CG-10/DEC-09 勾选、P1-08 `[x]`、进度快照）、`Progress.md`（本条）。
- **验证**：每任务 TDD RED→GREEN；Task 4 全量 `ctest -j8` → **1203/1203 绿**（1197 + 新增 6；2 个既有 GTEST_SKIP 不变）。
- **遗留**：顶层未知语句被静默吞掉（`asm volatile("nop");` 放函数体外的顶层语句不报错，既有缺陷，建议单独立项）；`%=`（GCC 唯一标签转义）未支持（sema E2024 拒绝）；根目录旧路径 `bin/`/`lib/`/`a.out` 待用户决定删除。

## 2026-10-11（续）— P1-08 最终 whole-branch review 与修复轮

- **评审**（fresh reviewer subagent，`971586f..386c860`）结论：核心实现与三处偏差映射（输出→返回值、`+r`→`=r`+tied、模板 GCC→LLVM 翻译）独立验证正确，UAF 修复精准；发现 **C1（Critical）+ I1/I2/I3（Important）+ M1-M4（Minor）**。
- **C1（Critical，实证）**：内存类输出约束（`=m`/`+m`/`=o`/`=V`/`=Um`）SEGV 编译器（rc=139，`SelectionDAGBuilder::visitInlineAsm`）——LLVM 21 对返回-值输出模型的内存约束走崩溃路径。修复：sema 拒绝输出约束含内存类字母 `m`/`o`/`V`（E2023，`hasMemoryClassOutputConstraint`），CLI 实证 `=m` 输出干净报错不再 SEGV；`=p`/`=g`/`=X`/`=v` 由后端干净报错不拒绝。
- **I1**：e2e 补 6 用例钉偏差路径（`=r`+`+r` 混用=40、多 `+r`=84、多输入=7、`&` 早破坏=5、`m` 输入=5、多位 `%N`=66）。
- **I2**：`m` 输入由 LLVM spill 副本（写经 `m` 输入写到副本，非原变量）——spec §5 + codegen 注释记录。
- **I3**：零输出 asm 隐式 volatile（GCC 语义，`HasSideEffects = isVolatile || outputs.empty()`），IR 层测试 `ZeroOutputAsmIsImplicitlyVolatile` 钉住，spec §2 同步。
- **验证**：每项 RED→GREEN；修复后全量 `ctest -j8` → **1212/1212 绿**（1203 + 9）。
- **遗留**：M1（sema 不查 `%N` 越界，D2 后端兜底）、M2（`%=` 拒绝未在 spec 注明）、M3（asm 前缀位置可解析、sema 以 void 拒绝）、M4（模板 `\0` 得内嵌 NUL）——deferred minors。

## 2026-10-11 — P1-09 终审 I4 收口：string 拥有型值语义 + string_view 硬改名

- **流程**：brainstorming → spec（`docs/superpowers/specs/2026-10-11-string-ownership-design.md`，529ae6a）→ plan（`docs/superpowers/plans/2026-10-11-string-ownership.md`，a6a233b）→ executing-plans Native 内联 6 任务（master 直接工作，与 P1-06/P1-08/P1-09 同惯例）。
- **Task 1（`str`→`string_view` 硬改名，D3）** `f081340`：Parser `isTypeStart`/`parseBaseType` 认 `string_view`（`str` 成未知标识符）；sema 显示名 + 全部用户文案（**实际 10 处**而非 pre-flight 估算 6 处——漏 4 处：`string.new takes exactly one str`/`requires a str`/`no member named ... in str`/`append() requires ... str`）→ `string_view`；libs/std/string.cdd + 全库测试迁移。Rulings：①负向测试 `str v2;` 顶层被 P1-08 遗留「顶层未知语句静默吞」吞掉——改函数体内载体；②`cmake --build build` 默认只重链 `--target` 指定的目标，CLI 旧二进制曾误导排查。DEC-18 测试补 `int32 string_view = 1;` 覆盖。
- **Task 2（赋值 D7）** `858ba34`：`getStringCopyFn`/`getStringAssignFn` 惰性合成（`cdd.string.copy`/`cdd.string.assign`）+ `isFreshStringValue`（rvalue 返回 string 的调用）。赋值挂接：string LHS → fresh 移动（free 旧+store）/ 活值 `getStringAssignFn`。Rulings：①自赋值守卫 `dst.ptr==src.ptr` 用「什么都不做」而非计划原文「仅拷贝跳过 free」（后者泄漏旧缓冲）；②helper 嵌套调用须 save/restore 共享 builder；③`CreateInsertValue` 的 Constant* 重载坑。e2e 4 测试。
- **Task 3（初始化 D2）** `027e89f`：VarDecl 局部 string 初始化——活值 `getStringCopyFn` 深拷贝 / fresh 直接 store。e2e 4 测试（含三元初始化拷贝钉）。
- **Task 4（传参/返回 D2/D6）** `c51551f`：三处实参组装 string 形参活值深拷贝（`emitStringArgCopy` 临时）；`string_view` 形参经 castValue 既有投影；返回——fresh 直接 ret / 局部标识符（AllocaInst）移动+源置零 / 字段/三元/全局拷贝。Rulings：①`true` 字面量→bool 形参重载解析失败是 pre-existing 缺陷（bool 变量可行、`true`→int32 可行），测试用 bool 变量规避、建议单独立项；②`-fsyntax-only` 只解析不跑 sema，误导排查。e2e 5 测试。
- **Task 5（聚合 §3.1/§5）** `c5c9d50`：`aggregateHasStringField`/`emitAggregateStringCopy`（递归字段感知：string 字段深拷贝、其余原样、嵌套聚合递归、基类子对象 slot 0）；挂接聚合初始化/brace-init string 字段/聚合赋值。Rulings：未初始化局部不零初始化（仅 slice 特判）——fresh 赋值 free 守卫依赖 `oldPtr==null`，未初始化 struct 字段是垃圾可能 free(garbage)，建议后续扩展 slice 零初始化先例（本任务测试均用初始化值）；已知限制：数组/optional/result 内含 string 视为不敏感（整段浅拷贝）。e2e 3 测试。
- **Task 6（文档收口）**：`2026-10-08-str-string-design.md` §1.2 标注 string 例外、`stdlib.md` §9 str→string_view + 所有权行、`semantics.md` NG-04 旁注、`TODO.md`（P1-09 I4 已收口 + 进度快照 1229）、`Progress.md`（本条）。
- **验证**：每任务 TDD RED→GREEN（`free(): double free detected` abort → 全绿）；Task 6 全量 `ctest -j8` → **1229/1229 绿**（1212 + 17 新增：parser/sema 迁移期净 +1、ownership e2e 16；2 个既有 GTEST_SKIP 不变）。
- **遗留/found issues**：①`true`/`false` 字面量 → bool 形参重载解析失败（pre-existing，建议单独立项）；②未初始化 string / 含 string 聚合局部不零初始化——fresh 赋值 free 守卫依赖 null（建议扩展 slice 零初始化先例）；③数组/optional/result 内含 string 的聚合拷贝不字段感知；④字符串字面量 → `string` 初始化仍须显式 `string.new`（既有）。

## 2026-10-12 — I4 收口轮 found issues 处理（3 项全部修复）

- **背景**：I4 收口轮（2026-10-11）遗留 3 项 found issue，用户选择本轮处理。每项 TDD RED→GREEN，一 issue 一提交。
- **Issue 1（`true`/`false` 应为 bool 字面量）** `628aa61`：根因——Parser 把 `true`/`false` 解析为 `NumberExprAST(1)/(0)`（int32），违反 spec keywords.md「布尔字面量」；`conversionRank(int32→bool)=-1` → 重载解析匹配不上 bool 形参（`true`→int32 形参却可行，因加宽）。修复——新增 `BoolLiteralExprAST`（Expr.h/.cpp codegen i1 常量）、Parser 两处、sema `visit` + 分发 + `evaluateConstexpr`、CompileTimeEvaluator。**零 ripple**：`isIntegerType` 含 Bool → 算术/初始化/赋值/返回全按整数族照旧，仅重载解析区分 bool。e2e `ConditionalReturnCopiesOperands` 还原为 `pick(true)` 钉住。**顺带发现**：optional/result 的嵌套 brace-init（struct 字段 `{ { true, ... } }`）在 codegen 报 `unknown variable: string`——pre-existing，另记。
- **Issue 2（未初始化 string/含 string 聚合局部零初始化）** `23b18c1`：根因——Decl.cpp 仅 slice 有零初始化特判（TYP-12 D3）；`string s;` 是垃圾 → D7 fresh 赋值 free 守卫可能 free(garbage)（CLI 实证没崩只是栈页侥幸为零）。修复——扩展 TYP-12 D3 到 String 与 `aggregateHasStringField` 的 Struct/Class（`{null,0,0}`）。**Ruling（测试载体）**：OS 给新栈页清零 → `string s;` 在 -O0 下碰巧为 null，「不崩溃」语言级测试修复前后都过，非有效 RED——改 IR 级 codegen 测试断言 alloca 有零初始化 store（确定性）。另补 e2e 安全钉 `UninitializedStringLocalAssignSafe`。
- **Issue 3（数组/optional/result 内含 string 的聚合拷贝不字段感知）** `835ca66`：根因——`aggregateHasStringField` 对 Array/Optional/Result 返回 false → 整段浅拷贝 → 元素共享缓冲双 free（数组实测 double free、optional/result 悬垂读段错误）。修复——`aggregateHasStringField` 递归 Array elementType / Optional elementType / Result success+error；`emitAggregateStringCopy` 重构为 switch 分发 + 新 `emitStringValueCopy` helper（string→helper、聚合→递归、其余→store）；Array 逐元素、Optional valid 守卫拷贝 value、Result ok 守卫拷贝 value/error。**Ruling（builder 插入点）**：Optional/Result 分支创建基本块后不能外层 `restoreIP`（会退回旧块致 doneBB 缺 terminator）——改为「builder 停在收尾块，调用方从其继续」（嵌套递归也正确）。已知限制：optional/result valid→invalid 赋值时旧缓冲不 free（泄漏非崩溃，未处理）。
- **验证**：每项 RED→GREEN（数组 double free / optional·result 段错误 → 全绿）；全量 `ctest -j8` → **1236/1236 绿**（2 个既有 GTEST_SKIP 不变）。基线 1229 → 1236（净 +7：sema bool 测试 +1、codegen 零初始化 IR 测试 +1、e2e +5）。
- **新遗留/found**：①optional/result 嵌套 brace-init 的 codegen 缺陷（`unknown variable: string`）；②optional/result valid→invalid 聚合赋值旧缓冲泄漏（边缘，未处理）。

## 2026-10-12（续）— 新遗留项处理：嵌套 brace-init 缺陷修复

- **背景**：found-issues 轮遗留 ①optional/result 嵌套 brace-init codegen 缺陷、②valid→invalid 赋值旧缓冲泄漏（边缘）。用户选定先修 ①。
- **根因（systematic-debugging）**：`visit(InitializerListExprAST)`（SemanticAnalyzer.cpp:3015）只 sema 元素[0]，不递归其余元素。`struct T x = { { true, string.new("data") } };` 的外层 VarDecl sema 对嵌套列表调 getExprType → 只访问 `true`；`string.new("data")` 的 MethodCallExprAST 从未触发 `tryAnalyzeStringStaticCall` → `builtinMethod` 未设 → codegen（Expr.cpp:1229）回退 `object->codegen`（VariableExprAST("string")）→ `unknown variable: string`。对照：变量级 `optional<string> o = {true, string.new("data")};` 正常（VarDecl 对每个元素 getExprType，:3215）。
- **修复** `a0b1887`：`visit(InitializerListExprAST)` 递归所有元素（node.type 保留首元素启发）。sema 通过但 codegen 失败时验证链：runSource 无 ADD_FAILURE、程序返回 2（`true` 正常存储、`string.new` 失败留零值 → `v!="data"`）——RED 精确命中根因。
- **范围确认**：仅此一处。VarDecl/ArrayDecl/UnionDecl 外层列表已逐元素；嵌套列表唯一入口即此 visit。非 string 表达式（如嵌套 struct `{ {1,2} }`）元素[1] 虽未 sema 也碰巧 codegen 正常（NumberExprAST type=null 默认 int32）——非有效 RED，未加测试。
- **验证**：2 新 e2e（`NestedBraceInitOptionalStringField`/`NestedBraceInitResultStringField`）RED→GREEN；CLI bug.c 干净编译运行；全量 ctest → **1238/1238 绿**。
- **剩余遗留**：仅 ②optional/result valid→invalid 聚合赋值旧缓冲泄漏（边缘，未处理）。

## 2026-10-12（续）— 新遗留项收尾：optional/result valid→invalid 赋值旧缓冲泄漏修复

- **背景**：嵌套 brace-init 缺陷修复后，剩余遗留仅 ②optional/result valid→invalid 聚合赋值旧缓冲泄漏（边缘，未处理）。
- **根因（systematic-debugging）**：`emitAggregateStringCopy`（CodegenContext.cpp）的 Optional/Result 分支只按**源**有效位分支——`br srcValid ? body : done`（srcOk ? ok : err）。string 字段的 assign helper 只释放它**写入**的槽（新活跃字段）；目标中"变为失效"的槽位从不触碰：optional valid→invalid（body 跳过，dstVSlot 保留旧缓冲）、result ok→err（okBB 跳过，旧 value 缓冲残留）、err→ok（errBB 跳过，旧 error 缓冲残留）。可观测点：失效后读伪字段 `.value`/`.error`（TYP-13/14 自由读写）→ 残留旧内容（len>0）；且违反"valid⟹value 含缓冲"不变量。非崩溃泄漏，故此前无测试。
- **RED 有效性（TDD）**：CLI 实证六文件全部 exit=2（读失效槽 len!=0），runSource 返回 2 精确命中根因。5 项 e2e（OptionalAssignValidToInvalidReleasesOld / ReleasesStructField / ReleasesArrayField、ResultAssignOkToErrReleasesOldValue / ErrToOkReleasesOldError）+ 1 项再转换安全（OptionalInvalidateThenRevalidateSafe）。⚠ 约束实证：赋值 RHS 不能是 brace-init 列表（`o = {...}` 报 E2003，列表类型按首元素 bool）→ 走 `x = y` 变量赋值；`optional<string[2]>` 非法（模板实参不能数组类型）→ 数组 case 经 `optional<struct S{string arr[2]}>` 间接到达。
- **修复** `42da409`：①Optional 分支读旧 valid 位 → `br srcValid ? body : staleBB`，staleBB 按 oldValid 决定是否 `freeBB`；②Result 分支读旧 ok 位 → okBB 尾 `br oldOk ? done : freeErrBB`、errBB 尾 `br oldOk ? freeValBB : done`；③新增 `emitStringValueRelease`/`emitAggregateStringRelease`（镜像拷贝递归，Struct/Class/Array/Optional/Result 全支持）：释放槽位内全部 string 缓冲（free null 守卫）+ 置零，维持"失效⟹槽位零化"不变量——否则再转换时 assign helper 对悬垂指针二次 free。非 string 承载 → 无操作。
- **验证**：6 新 e2e RED（exit=2）→ GREEN；CLI 六实证 exit=0 + r7 再转换 exit=0；全量 `ctest -j8` → **1244/1244 绿**（1238 + 6；2 个既有 GTEST_SKIP 不变）。
- **新遗留**：**已清空**。相关已知限制（不在本轮范围）：失效侧 RHS 临时值被丢弃（如 `{false, string.new("world")}` 的 "world" 缓冲）——语言无 RAII，未消费的 rvalue 临时一律丢弃（pre-existing）；可选后续：Issue-3 的 optional/result 测试仍用字段赋值构造（当初为规避本 bug），现可换自然嵌套 brace-init 语法。

## 2026-10-15 — P1-07 位域完成（PAR-10/SEM-12/AGG-06/CG-09/DEC-08）

- 流程：plan（docs/superpowers/plans/2026-10-13-bitfields.md）→ executing-plans native 6 任务 TDD（每任务 RED→GREEN→提交）。提交线：`2ba8de7`(T1 语法) → `9671a24`(T2 sema E2025/26/27) → `d9e5851`(T3 System V 布局物化) → `78537b8`(T4 RMW codegen) → `ccdcc45`(T5 全局/static 常量位域感知) → `a973db6`(T6 union/packed/class)。
- 布局：非 packed 按 System V / GCC 单元分配（无当前单元/基类型尺寸不同/放不下 → 新单元；零宽结束单元；bool → i8 单元）。packed 用 GCC 连续位流"区域"模型：相邻位域共享单一 `[N x i8]` LLVM 伪字段、位偏移为区域内绝对位、零宽按声明类型自然字节边界取整（不结束区域）、普通字段结束区域——`int a:3;int b:3` packed → 1B、`a:3;:0;b:3` → 5B 等尺寸与 gcc 实测逐一核对一致。
- 关键机制：`BitFieldLayout {unitIndex,bitOffset,bitWidth,unitBits}`；`semanticFields` 物化前快照；RMW 助手 `loadBitFieldValue`/`storeBitFieldValue`（unitBits 驱动加载/掩码，packed 区域单元可宽于基类型）；`buildAggregateConstant` OR 常量进存储单元（packed 区域拆字节数组）；局部花括号初始化按语义字段序消费。
- Rulings（执行中）：①正向测试改显式 parser 管线断言 bitWidth（runSource 无法捕获静默丢弃）；②命名零宽 parse 期拒绝（FieldInfo 编码无法表示）；③AsArgAndReturn 去聚合返回调用（MEM-14 既有限制）；④NestedChain 测试缺陷（signed 3 位存 5 读 -3 是 C 语义）；⑤static 局部不支持（Parser 仅类成员处理 TOKEN_STATIC）——Task 5 StaticLocalInit 测试原靠 parse 错误恢复静默丢弃 `static` 才碰巧通过，改为诚实的 LocalBraceInitTruncation；⑥packed 布局按 GCC 区域模型（上）；⑦无名成员不消费花括号初始值（独立 initIdx，修 `{5,6}` 的 6 被 `int:0` 吞掉）；⑧sema 成员解析物化后优先 semanticFields（修 size_of 后访问失败）。
- 整分支评审（subagent）：C1 union 位域被当普通字段（Parser 注册丢 bitWidth + sema 重复成员 → 无 RMW/E2027 绕过）；C2 union codegen unitBits 未设（IntNTy(0)）；I1 派生聚合物化 GEP 基类 +1 重复；M1 位宽溢出 cast 回绕绕过 E2026。修复提交 `26d432a` + 5 回归测试（UnionBitFieldTruncates/AddrOfRejected/GlobalInit、DerivedNormalAndBitField、WidthOverflowRejected）。
- 验证：每任务提交后全量 ctest 全绿；最终 **1283/1283**（1244 基线 + 34 BitFieldE2E + 5 评审回归）。CLI 与 gcc 实测核对 packed 尺寸与位域值语义（signed sext / unsigned zext / 截断回绕）。
- 已知限制（非本轮范围）：static 局部变量不支持（DEC 存储类）；packed 位域 RMW 用整数自然对齐（x86-64 容忍，严格对齐目标需 Align(1)，M2）；位域 struct 整体按值传参/返回（MEM-14 聚合 ABI）；readLValue 三份拷贝（计划已接受）。

## 2026-10-15 — 全特性演示 examples/all_features.cdd + 3 个编译器 bug 修复

- 产出：`examples/all_features.cdd`（单文件 20 节全特性演示，编译+运行通过，`ALL FEATURES OK`）。编译运行：`./bin/cdd -c examples/all_features.cdd -o /tmp/a.o && cc /tmp/a.o -o /tmp/a && /tmp/a`。
- 建设过程中用 systematic-debugging 定位并修复 3 个编译器 bug（提交 `15bbbb6`，各配回归测试 `tests/e2e/test_demo_regressions.cpp`）：
  - **A** `CompileTimeEvaluator::toLLVMType` 对 Class 用 `static_cast<StructType*>` 读 isPacked——ClassType 非 StructType 子类，跨类读偏移 UB，读垃圾值 → 布局损坏，`size_of(派生类)` 非确定性 `Invalid size request on a scalable vector` 崩溃。按 kind 分支读取 name/isPacked。
  - **B** 无后缀整型字面量恒 int32 → `int64 v = 4000000000` 以有符号 int32 构造 APInt（Debug 断言崩溃 / Release 静默截断）。sema 按值域提升 int32→int64→int128。
  - **C** `ForStmtAST::codegen` 循环变量无独立作用域 → 兄弟 for 循环复用同名 `i` 时第二个循环绑定首个循环的 i（=11），循环体不执行。pushScope/popScope 包住循环。
- 调试方法：非确定性内存类 bug，gdb/valgrind/ASAN 均因堆布局改变不触发；`MALLOC_PERTURB_`（释放内存填 0xA5）使崩溃确定化（90%+），Debug 版 gdb 抓到精确行号，确认 `StructLayout::getElementOffset` 返回 scalable TypeSize（DataLayout 缓存被垃圾 isPacked 损坏的 Class 布局污染）。
- 演示期间还发现（未修，记录）：数组 typedef 不支持下标/slice，`sizeof` 按指针退化（4）；演示已按实际行为调整断言。
- 全量 ctest **1286/1286** 绿。

## 2026-10-16 — 演示覆盖补齐（§21 算术转换/const·constexpr/全局/方法数组）

- 按 tests/e2e 清单逐项对照，发现并补齐行为级缺口：算术转换语义（混合符号比较→无符号、无符号除/模、逻辑移 vs 算术移、uint8 零扩展、char 提升、uint→int64 零扩展、uint→float、float/int 公共类型提升）、const/constexpr 变量、全局零初始化/全局切片、方法数组成员写入。
- examples/all_features.cdd 现 21 节；实测 8/8 编译干净、8/8 运行 `ALL FEATURES OK` exit 0（提交 `5afeeef`）。

## 2026-10-16 — 基座能力边界探测 + 5 个 codegen/语义 bug 修复

- 方法：按 TODO 第 0 节 BASE-01..10（初步能力）逐域写边界压力探测（`/tmp/probe/p_*.c`，check 助手累加失败），覆盖控制流/类型与字面量/表达式与语法/聚合/字符串与格式/函数指针/模板/namespace/optional·result/运算符重载/数组·切片/constexpr·compile_time/驱动 flag。每域全绿后收口，共修复 5 个真 bug（各配回归测试，提交 `9a06963`、`f4aa8a9`、`7e26546`）：
  - 指针后缀 `++`/`--` 用 `CreateAdd`（opaque ptr 字节算术）→ `int32* p++` 只前进 1 字节，解引用读错位值（20→20<<24）。镜像前缀路径改 `CreateGEP` 按元素步进。
  - 复合赋值恒走整数指令 → `fd *= 2.0` 按整数位模式乘法得 0（1.5 低 32 位为 0）。按操作数浮点/无符号分派 F 系 / U 系。
  - 无符号复合赋值 `>>=` 恒 `AShr`（0x80000000>>4 得 0xF8000000 而非 0x08000000）、`/=`/`%=` 恒有符号。按 lhs 无符号性选 `LShr`/`UDiv`/`URem`。
  - 运算符重载解析成功时表达式类型被设成左操作数类型（非 operator 函数返回类型）→ `int32 s = p + q` 报 got 'struct V2'、`p + q == 10` 报 V2 与 int32 不可比。从 resolved Symbol 的 FunctionType 取 returnType（镜像普通调用）。
  - **裸 `return;`（void 函数早退）**：sema/codegen 早已支持（void 检查 + CreateRetVoid），仅解析器硬性要求表达式被拒（Progress 两轮遗留）。两处 return 解析放行。更新 `ParserErrorTest.UnexpectedTokenInExpression`（原用 `return ;` 当非法载体，改用 `return )`），新增 `BareReturnParses`。
- 验证：全部探测文件绿；演示 `ALL FEATURES OK` exit 0；全量 ctest **1292/1292**（1286 + 6 新回归）。
- 已知缺口（未修，另项评估）：指针相减 `ptr - ptr` 不支持；函数返回函数指针语法不支持；`&fn` 取函数地址不支持；int128 字面量 >64 位被 E0003 拒；string_view 下标 `s[0]` 不支持（用 char_at/sub）；数组 typedef 下标/slice/sizeof 按指针退化。

## 2026-10-16 — 补齐基座已知缺口（5 项特性 + int128 字面量）

- 按用户要求补齐上轮记录的全部已知缺口（提交 `bfdecdc`，各配回归测试）：
  - **`&fn` 取函数地址**：函数设计符 decay 后是 Pointer(Function) 且非 lvalue，AddressOf 误报"cannot take address"；C 语义 &fn==fn，取址无操作。
  - **string_view 下标 `s[i]`**：sema/codegen 加 Str 分支（解包 {ptr,len} 后 GEP i8），等价 char_at(i)。
  - **指针相减 `p - q`**：sema 允许 Pointer-Pointer → isize；codegen 用 LLVM CreatePtrDiff 按元素计数。
  - **数组 typedef**（根因是 parser 丢维度）：`typedef int32 IntArr[4]` 此前只注册 int32（`[4]` 被丢），下标/slice/sizeof 全坏；parseTypedefDecl 处理名字后 `[N]` 后缀（右到左嵌套 ArrayType）+ 全链路剥 typedef。
  - **函数指针 typedef + 函数返回函数指针**：parseTypedefDecl 复用 parseFunctionPointerDeclarator（`typedef int32 (*BinOp)(int32,int32)`）；间接调用按 typedef 变量解析。原始声明形式 `int32 (*f(p))(params)` 仍不支持（罕见，typedef 惯用法可替代）。
  - **int128/uint128 字面量（>64 位）**：TokenValue 整数值统一 unsigned __int128，lexer 累加到 128 位（E0003 移到超 2^128-1）；NumberExprAST 加 valueHi；sema 映射 int128/uint128；codegen APInt 两字构造；constexpr 对 128 位字面量明确拒绝（64 位 ConstValue 无损承载）。
- 验证：全量 ctest **1299/1299**（1292 + 6 新回归 + 1 新 lexer 正例）；20 探测全绿；演示 `ALL FEATURES OK`（新增数组 typedef/指针差/int128 用例）。
- 遗留（已记）：原始函数返回函数指针声明语法不支持（typedef 可用）；128 位字面量在常量表达式中被拒（运行时值可用）。

## 2026-10-16 — P1-07 指定初始化器 + 柔性数组完成（PAR-13/AGG-04/05）

- 流程：brainstorming → spec（docs/superpowers/specs/2026-10-16-designated-init-flexarray-design.md）→ plan（docs/superpowers/plans/2026-10-16-designated-init-flexarray.md，5 任务 TDD）→ executing-plans Native 内联（master 直接提交，项目惯例）。
- 提交线：`82ff6d6`(T1 解析) → `da0c144`(T2 sema) → `eb42bca`(T3 局部 codegen) → `9afa55b`(T4 全局常量) → `6e89e71`(T5 FAM e2e) → `4bbab6f`(终审修复)。
- **Task 1（解析）**：`Designator{kind, field, index, indexExpr}`（Ruling：计划原 `int64 index` 不足以承载常量表达式设计符如 `[Red]`——parser 无求值能力，增 `indexExpr` 节点，sema 折叠回写 `index`）；`InitializerListExprAST::designators`（与 initializers 平行，空=位置式）；`FieldInfo::flexibleArray`；`parseMemberArraySuffix` 末维 `[]` → `ArrayType(T,0)` 置标记。
- **Task 2（sema）**：`resolveDesignators` 逐元素展开语义目标链（`resolvedTargets`）——位置式游标（跳过 `name==""` 伪字段，AGG-06 规则）/设计符逐链节（`.name` 查 semanticFields 或 fields，class 沿 base 链 + E2009、union 命中即槽 0、`[i]` 仅数组、链节间须聚合、重复/乱序不报错、Optional/Result 伪字段明确拒绝）；数组大小推断 = C99 `max(位置式计数位置, 最大 Index+1)`；FAM 约束（仅 struct 尾、非匿名/位域、前有具名成员、class/union/花括号初始化拒绝）。Rulings：①`sizeof(局部数组变量)` 在 compile_time.static_assert 不折叠 → 推断测试改手建 AST 检查 `node.size==4`；②Parser/sema 共 6+3 处 `addField/addMember` 拷贝丢 flexibleArray → 逐处补标记；③Designator 含 unique_ptr 不可拷贝 → `push_back(std::move)`+resize 而非 assign。
- **Task 3（局部 codegen）**：整块补零（`CreateMemSet`，未指定补零 `Point p={1}` → `p.y==0`）+ 逐元素按 `resolvedTargets` 链写（`emitDesignatedInitializer`/`resolveFieldGEP`：semanticFields 物化索引绝对、基类下降 slot 0、位域 `storeBitFieldValue`、union 槽 0 指名成员类型、嵌套列表递归、string 深拷贝路径复用）。Ruling：基类下降 GEP 用 slot 0 而非 base 偏移（base 只用于自身字段）。
- **Task 4（全局 codegen）**：`buildDesignatedConstant` 数组下标/struct-class 字段（含 base 偏移）/位域 bfAcc 掩码 OR/union 槽 0；顶层 hasResolved dispatch。Rulings：①裸结构体名 `Point g` 顶层声明不识别（既有缺口）→ 全局测试改 `struct Point g` C 风格；②dispatch 使 union 位置式列表进入 buildDesignatedConstant 但原无 Union 分支 → 回退零初始化致 `BitFieldE2E.UnionBitFieldGlobalInit` 失败 → 补 Union 分支。
- **Task 5（FAM）**：关键发现——FAM 功能经 T1/T2（解析标记+语义约束）+ 既有 LayoutBuilder 零长数组（`[0 x T]` allocSize=0）**天然正确，零源码改动**，仅补测试锁定。gcc 基线核对：默认 sizeof/offsetof `1 1 | 4 4 | 1 1`（packed）；align(16) sizeof=16 但 offsetof(data)=4（FAM 偏移取自然对齐，align 只抬 size）；位域+FAM sizeof=固定部分。Rulings：①JIT e2e harness 需前置 std prelude（parseStdCPrelude+prependDeclarations），否则 malloc/free undeclared（CLI 自动注入而 JIT 不注入）；②offset_of 字段名须字符串字面量。
- **终审修复（`4bbab6f`）**：C1 物化路径字段/位域 GEP 双计数 base 伪字段（baseOffset+unitIndex，base 在槽 0 后索引即绝对）——`emitAggregateInitializer`/`resolveFieldGEP` 去 baseOffset；C2 全局常量丢基类字段/多链——`buildDesignatedConstant` 重写为递归 `placeDesignatorValue`（累积常量：后写覆盖、位域掩码 OR 保留已有位、基类子对象下降、`buildAPIntFromByteArray` 逆还原 packed 位域单元）；I1 全局 union 多设计符后写覆盖（原先写者赢）；I2 VarDeclAST 局部数组（含 typedef 数组）设计符初始化补零——零填充守卫加 Array kind；I3 顶层数组大小推断接受多链 `[1][2]=5`（首节 Index 即外层维度位置）。
- 测试：ParserDesignatedInitTest 3 + SemaDesignatedInitTest 5 + SemaFlexArrayTest 3 + DesignatedInitE2E 10（含 5 终审回归）+ FlexArrayE2E 4。
- 验证：每任务 TDD RED→GREEN；终审后全量 ctest **1326/1326** 绿（2 个既有 GTEST_SKIP 不变）。工作区 clean。
- 遗留/P1-07 剩余：匿名类型定义（PAR-12，匿名成员提升 AGG-03 已做）；lambda（PAR-11/CG-11/DEC-10）。相关既有缺口：裸结构体名顶层变量声明不识别；`compile_time` 表达式内 `sizeof(变量)` 不折叠。

## 2026-10-17 — P1-07 匿名类型定义完成（PAR-12）

- 流程：brainstorming（用户选定「逐个执行」P1-07 剩余）→ spec（docs/superpowers/specs/2026-10-17-anonymous-types-design.md）→ plan（docs/superpowers/plans/2026-10-17-anonymous-types.md，3 任务 TDD）→ executing-plans Native 内联（master 直接提交）。
- 提交线：`ccf9a76`(T1 合成名身份) → `8730e27`(T2 匿名 class 注册) → `1635135`(T3 名义身份强制)。
- **探查实证**：匿名 struct/union 定义+变量（全局/局部）已基本可用；匿名 **class** 不可用（`no member named 'v' in class ''`——parseDeclarationImpl class 分支丢弃 classDecl AST，sema 永不注册字段/方法，占位 ClassType 由 parseClassDecl 预注册故字段也不进）；类型身份错误（两匿名 struct `a=b` 通过）；附带发现 pre-existing 洞：`struct S {...}; S s;` 全局裸类型名变量把 `s` 当类型名（`@S`，另立项）、命名 struct/class 交叉赋值同 kind 放行（spec 要求按名字）、局部命名 struct/class 定义+变量不支持。
- **Task 1（合成名身份）**：`TypeContext::nextAnonId()`（进程内单调 `__anon_<N>`）；parseType inline struct/union 路径与 parseStructDecl/parseClassDecl/parseUnionDecl 在无 tag+`{` 时分配；`qualifyTypeDeclName` 对 `__anon_` 前缀不再加 namespace 前缀（否则 `S___anon_0` 失配 AGG-03 判定）；`isAnonymousAggregate` 扩为「空名或 `__anon_` 前缀」；struct/class/union 声明分支改 `parseVariableDeclList`（支持 `, v2`）。
- **Task 2（匿名 class 注册）**：根因同上；`StructDeclAST::attachedVars`（unique_ptr<DeclAST>）挂载内联变量，class 分支返回聚合 AST 而非丢弃；sema `visit(StructDeclAST)` 尾处理 attachedVars（analyzeTopLevelDecl 分派）；codegen `StructDeclAST::codegen` 尾 emit；局部 `class { ... } c;` 走 parseDeclaration（isTypeStart 不含 class）。附带修复命名 class 内联变量 `class C { ... } c;`（p47 同病）。
- **Task 3（名义身份强制）**：`checkAssignmentTypes`/`typesCompatible` 同 kind 兜底对 Struct/Class/Union 收紧——`typesEqual`（同名）或派生→基（新 `isDerivedFrom` helper，沿 baseClass 链 walk，Struct/Class 双形态，保 INH-04 值切片）；否则报错。两次独立匿名定义赋值/初始化/实参/返回/比较互斥（C 语义）；同声明多变量（`struct { ... } a, b;`）共享类型可互赋；typedef 命名后同型可互赋。顺带修复命名 struct/class 交叉赋值 pre-existing 洞。
- 测试：ParserAnonTypeTest 3 + AnonClassE2E 4 + AnonTypeE2E 7。
- 验证：每任务 TDD RED→GREEN；最终全量 ctest **1340/1340** 绿（2 个既有 GTEST_SKIP 不变）。工作区 clean。
- 遗留/P1-07 剩余：**lambda（PAR-11/CG-11/DEC-10）**。相关既有缺口：裸类型名全局变量（`S s;`）、局部命名 struct/class 定义+变量、struct-with-method 内联变量（struct 方法路径不完整）、匿名类型函数签名跨声明点（须 typedef 命名，C 同）。

## 2026-10-17 — P1-07 lambda 完成（PAR-11/CG-11/DEC-10）

- 流程：brainstorming（用户选定「逐个执行」P1-07 剩余）→ spec（docs/superpowers/specs/2026-10-17-lambda-design.md）→ plan（docs/superpowers/plans/2026-10-17-lambda.md，4 任务 TDD）→ executing-plans Native 内联（master 直接提交）。
- 提交线：`fbf86ba`(T1 解析) → `bc59bbc`(T2+T3 sema+codegen) → 文档轮（本次）。
- **探查**：无 lambda token/AST/codegen；`[` 仅下标（后缀）与声明注解——表达式起始 `[` 无下标含义，可安全识别为 lambda；语言无 `auto` → 有捕获闭包类型不可命名（DEC-10 裁决：实际形态为立即调用）。
- **Task 1（解析）**：`LambdaExprAST`（captures{captureAll,name,byRef}/params/returnType/hasExplicitReturnType/body/closureName/closureType）；前向声明 ParamDeclAST/CompoundStmtAST（Decl.h→Stmt.h→Expr.h 循环）；析构 out-of-line 于 Expr.cpp；`parseLambdaExpr()`——capture list（`]`/`[=]`/`[a,&b]`）、`(params)` 复用 parseParamDecl、可选 `-> Ret`、`{body}`；`parsePrimaryImpl` 遇 `[` → lambda。后缀 `(` 作用于 lambda → `LambdaCallExprAST`（立即调用）。
- **Task 2（sema）**：参数/捕获解析（`[=]` 语句感知扫描 `collectUsedNamesStmt/Decl`）；返回类型推断（body 作用域内取首个 return 表达式类型，多 return 不兼容未处理）；闭包 struct（`__lambda_<N>`，字段=按值拷贝/按引用指针）或函数指针类型（无捕获）；body 分析（入作用域、声明参数与捕获、伪 FunctionDeclAST 承载返回类型检查——注意其构造会 move params/body，须传局部空容器）。
- **Task 3（codegen）**：生成静态函数——有捕获 `__lambda_<N>__call(Closure*, params...)`（body 捕获名声明：按值=字段拷贝 alloca，按引用=把闭包字段指针直接作为捕获名地址，**写穿原对象** `a=9` 生效）；无捕获 `__lambda_<N>(params...)`（表达式 bitcast 函数指针，可赋/回调/直接调用）；`LambdaCallExprAST` 立即调用（栈上构造闭包→call）；`saveIP/restoreIP` 保护调用方插入点（嵌套函数生成不串位）。
- 测试：ParserLambdaTest 4 + LambdaE2E 7（无捕获函数指针/立即调用/按值捕获/按引用写穿/`[=]`/qsort 回调/返回类型推断）。
- 验证：每任务 TDD RED→GREEN；最终全量 ctest **1351/1351** 绿（2 个既有 GTEST_SKIP 不变）。工作区 clean。
- 遗留/P1-07 相关：lambda 多 return 不兼容推断（只取首个）；嵌套 lambda 捕获内层未深度处理；有捕获闭包不可命名（无 auto，DEC-10 裁决）。**P1-07 全部完成**。下一里程碑：P1-09 剩余（std.mem/std.math/std.collections，STD-05/06/07/13/14）与 P2。

## 2026-10-17 — P1-09 std.mem 纯函数层完成（STD-05 范围 a）

- 流程：brainstorming（用户选定 std.mem，范围 a：纯函数层）→ spec（docs/superpowers/specs/2026-10-17-stdmem-design.md）→ plan（docs/superpowers/plans/2026-10-17-stdmem.md，3 任务 TDD）→ executing-plans Native 内联（master 直接提交）。
- 提交线：`840b287`（Task 1+2 合并：mem.cdd + std.c posix_memalign + 7 e2e）。
- **探查实证（关键约束）**：函数可跨模块导出；**struct/class 类型不可**——parse 期类型名注册先于 import 处理（`std::Point` 报 `expected ';'`）；模块内 `template<typename T>` 亦不可见（`tprobe::tmax<int32>` 报 undeclared）。此即 stdlib.md:149 已挂账的「class 无法跨模块导出」（StrBuilder 同因）。→ Allocator/Arena/Pool 需类型，本轮**挂账写草案**。
- **Task 1+2（mem.cdd + std.c）**：`libs/std/mem.cdd`（模块自包含 extern，io.cdd 先例）——`alloc`（malloc）/`alloc_zeroed`（calloc）/`resize`（realloc，避 extern 重名）/`dealloc`（free，避重名）/`align_up(n,a)=(n+a-1)&~(a-1)`/`align_down=n&~(a-1)`/`is_aligned(p,a)`（2 的幂位运算，非 2 的幂行为未定义文档注明）/`aligned_alloc`（posix_memalign 包装：成功返回 ptr、失败返回 null；align 须 2 的幂且 ≥ sizeof(void*)）/`aligned_free`（=free）。`libs/std/c.cdd` 与 `src/driver/StdPrelude.cpp` 内嵌兜底同步补 `posix_memalign` 声明。
- 测试：`tests/e2e/test_std_mem.cpp`（runWithImports harness）7 项——alloc 非空/alloc_zeroed 置零/resize 保数据/align_up_down/is_aligned/aligned_alloc 64 对齐/aligned_alloc 非 2 幂对齐失败 null。测试修正两处：①is_aligned 用栈变量地址测对齐不可靠（地址不保证），改 aligned_alloc 已知对齐指针；②断言 p 非 128 对齐错误（posix_memalign 保证至少 64 对齐，128 字节块可能更高对齐），改 16 对齐 + 偏移 1 字节验证。
- 文档：stdlib.md §11 std.mem 函数表 + §11.1 Allocator/Arena/Pool 接口草案（含 `struct Allocator`/`Arena`/`Pool` 蓝本，标注阻塞原因）；TODO STD-05 `[x]`、MEM-06 `[~]`（包装已落 std.mem）、MEM-07 草案挂账、P1-09 快照 1358。
- 验证：每任务 TDD RED→GREEN；最终全量 ctest **1358/1358** 绿（2 个既有 GTEST_SKIP 不变）。工作区 clean。
- 遗留/P1-09 剩余：**std.math/std.bit（STD-13/14）**（纯函数，不受模块导出类型阻塞，可本轮）；**std.collections（STD-06~08）**（泛型容器，阻塞于「模块导出类型」语言缺口）；std.mem 的 Allocator/Arena/Pool（随语言层缺口一并）。

## 2026-10-17 — P1-09 std.bit + std.math 完成（STD-14/13）

- 流程：brainstorming → spec（docs/superpowers/specs/2026-10-17-stdmath-bit-design.md）→ plan（docs/superpowers/plans/2026-10-17-stdmath-bit.md，3 任务 TDD）→ executing-plans Native 内联（master 直接提交）。
- 提交线：`df03a73`（Task 1 std.bit）→ `f6dd3be`（Task 2 std.math + C 符号修复）→ 文档轮（本次）。
- **Task 1（std.bit）**：`libs/std/bit.cdd` 纯语言位工具——SWAR popcount8/16/32/64（64 位掩码用 `ul` 字面量 + 移位构造，**无 `ull` 后缀**）；二分 clz32/64、循环 ctz32/64（**对 0 返回位宽，冻结**）；rotl32/64、rotr32/64（n 按位宽取模）；bitcast 包装（f32/f64 ↔ u32/u64/i32/i64，reinterpret_cast）。7 e2e（tests/e2e/test_std_bit.cpp）。
- **Task 2（std.math）**：`libs/std/math.cdd`——18 个 libc 绑定（sqrt/sqrtf/fabs/fabsf/floor/floorf/ceil/ceilf/round/roundf/trunc/truncf/pow/powf/cos/cosf/sin/sinf，**`-lm` 链接**）+ `constexpr float64 PI/E` + 位级 isnan/isinf/is_finite（IEEE754：指数段全 1 判断，运行时构造掩码绕开超 int64 常量折叠限制）。5 e2e（tests/e2e/test_std_math.cpp）。
- **根因修复（sema，配套编译器改动）**：namespace 内 C-linkage extern 函数被 `scopedName` 前缀化为 `std_sqrt`，`mangleFunction` 因前缀不再命中 `isCName`（`markCName` 注册裸名 `sqrt`）→ 生成 `std_sqrt_float64`，JIT 找不到 libm 符号（`ConstantsPIAndE`/`IsNanIsInfIsFinite` 通过、3 个调 extern 的测试失败，精确命中）。修复（SemanticAnalyzer.cpp）：
  - `visit(FunctionDeclAST)`：`isCName(node.name)` 时跳过 `scopedName`（保持 C 裸符号名）；
  - `namespaceCandidates`：`ns::f` 限定名在限定键后追加裸 C 名回退候选（仅当裸名为 C 名）。
  - 验证：`cdd -c` + `cc -lm` 编译链接运行 exit=0；5 e2e JIT 全过。
- **Ruling（namespace 内 C 名函数语义）**：namespace 内的 extern C 函数符号名保持 C 裸名（链接 libc/libm），namespace 前缀仅作访问语法——若错，`ns::f` 限定名会意外落到全局裸名 f（仅在裸名为 C 名时回退，风险面小）。
- 文档：stdlib.md §12 std.bit / §13 std.math 函数表 + C 符号名说明 + 前置约束；TODO STD-13/14 `[x]`、P1-09 收口、进度快照 1370。
- 验证：每任务 TDD RED→GREEN（Task 2 RED：临时移走 math.cdd 全 5 FAIL；GREEN 后全绿）；最终全量 ctest **1370/1370** 绿（1365 + 5；2 个既有 GTEST_SKIP 不变）。
- 遗留/P1-09 剩余：**仅 std.collections（STD-06~08）**，阻塞于「模块导出类型」语言缺口；std.mem 的 Allocator/Arena/Pool 同因挂账。

## 2026-10-19 — 语法遗漏收口完成（LEX-16/18、PAR-14、TYP-05/24、CON-01/04、FUN-09、CT-07/13、INH-06）

- 流程：用户暂停标准库（std.collections 不做）→ 语法层面审计（实测确认 11 项遗漏 + 1 项静默丢弃缺陷 + 多文档滞后）→ brainstorm → spec（docs/superpowers/specs/2026-10-18-syntax-gaps-design.md，builder-check 8 项裁定）→ plan（docs/superpowers/plans/2026-10-18-syntax-gaps.md，9 任务 TDD）→ executing-plans Native 内联（master 直接提交）。
- 提交线：Task1 `feat(LEX-16/18)` → Task2 `feat(TYP-05/CON-04)` → Task3 `feat(CON-01)` → Task4 `feat(FUN-09)` → Task5 `fix E2031` → Task6 `feat(CT-07/13)` → Task7 `feat(INH-06)` → Task8 `feat(PAR-14/TYP-24)` → Task9 `docs`。
- **Task 1（LEX-16/18）**：多行字符串 `"""..."""`（scanMultilineString + Token::isMultilineString + parser 3 引号剥离）+ Unicode 标识符（高位字节放宽 scanToken/scanIdentifier）。转义全集本已实现（decodeEscapes）。
- **Task 2（TYP-05/CON-04）**：char8/16/32 → uint8/16/32 别名（parseBaseType + isTypeStart）；thread_local 存储类（VarDeclAST::isThreadLocal + codegen setThreadLocal + 函数拒绝 E2028）。IR 实证 `@x = thread_local global i32 5`。
- **Task 3（CON-01）**：atomic 限定符——**克隆类型不污染共享单例**（`atomic int32` ≠ `int32`，typesEqual/mangle 纳入 isAtomic）；原子 load/store（seq_cst）+ 复合赋值/`++`/`--` 经 atomicrmw（add/sub/and/or/xor）；`* / % << >>` 与浮点/指针拒绝（E2029/E2030，cmpxchg 属 CON-02 边界）。IR 实证 `atomicrmw add ... seq_cst`。
- **Task 4（FUN-09）**：默认参数 `T name = const-expr`（仅尾随、须常量）——ParamDeclAST::hasDefault/defaultValue + Symbol::decl 回指针 + FunctionDeclAST::defaultArgCount；**OverloadSet::resolve 区间化**（[minArgs,paramCount]）+ 并列偏好少默认；checkFunctionCall/resolveMethod 同区间化；调用点折叠暂存（DefaultArgValue）+ codegen 补参（CallExprAST/MethodCallExprAST）。
- **Task 5（缺陷修复）**：未知类型名声明静默丢弃——`f16 x;`/`char8 c;` 零诊断消失；parseDeclarationAsType 报 E2031 unknown type + 双报防护（模板实参失败不补报）。
- **Task 6（CT-07/13）**：compile_time 反射——ReflectionRegistry（sema→codegen 交接）+ FieldInfo/Attribute 内建类型 + parser ctMember 白名单扩展 + CompileTimeEvaluator::evalReflectFields（LayoutBuilder 偏移/大小）+ codegenReflection（全局常量数组 + slice 视图）。`name_of→string_view`、`fields_of→Slice<FieldInfo>`、`attributes_of→Slice<Attribute>`。
- **Task 7（INH-06）**：protected 派生访问放宽——canAccessMember(definingClass, level)：Public 放行、定义类放行、protected+派生类放行、**private 仅定义类**。4 检查点（static 成员/字段/方法/指定初始化器）。
- **Task 8（PAR-14/TYP-24）**：`alignof(T)`/`offsetof(T, member)` 独立关键字（SizeofExprAST::isAlignOf + OffsetOfExprAST，LayoutBuilder 偏移）+ `nullptr`（词法映射 TOKEN_NULL，≡ null）。
- **Task 9（文档）**：grammar.ebnf/keywords.md/conversions.md/compile_time.md/semantics.md/abi.md 同步（`str`→`string_view`、裸 return、原始字符串、转义全集、反射签名等滞后修正）；TODO 勾记 9 条目 + 进度快照 1418；Progress 本记录。
- **关键 Ruling**：①atomic 克隆类型（共享单例污染隐患——builder-check #3）；②默认参数少默认优先（C++ 语义，builder-check #2）；③`f(int,int)` 与 `f(int,int=5)` 是 C++ 非法重载（测试用例修正）；④name_of 返回 typeToString（`struct S` 非 `S`）；⑤反射 ctHandled 先于 ctConstantOrNull 判断；⑥E2031 双报防护（诊断计数）。
- 验证：每任务 TDD RED→GREEN（编译错误/断言失败/CLI 复现三种 RED 形态）；最终全量 ctest **1418/1418** 绿（2 个既有 GTEST_SKIP 不变）。工作区 clean。
- 遗留/P1-09 剩余：**仅 std.collections（STD-06~08）**，阻塞于「模块导出类型」语言缺口（parse 期类型名注册先于 import）；std.mem 的 Allocator/Arena/Pool 同因挂账。CON-02/03/09/10（atomic 完整并发模型）、LEX-16 多行字符串缩进策略等另轮。

## 2026-10-20 — 语法缺口第二轮收口（TYP-04、PAR-06、TYP-05、PAR-08、PAR-03、SEM-17、GEN-02）

- 流程：用户「补全语法层面仍缺失的部分」→ brainstorm 范围裁定（虚函数标 Non-goal；可变参数模板维持 GEN-10 Non-goal；其余全做）→ spec `docs/superpowers/specs/2026-10-20-syntax-gaps-r2-design.md`（builder-check 6 项裁定）→ plan `docs/superpowers/plans/2026-10-20-syntax-gaps-r2.md`（11 任务）→ executing-plans Native 内联（master 直接提交）。
- 提交线：Task1 `feat(TYP-04)` → Task2 `feat(PAR-06)` → Task3 `feat(TYP-05)` → Task4 `feat(PAR-08)` → Task5 `feat(PAR-08)` → Task6 `feat(PAR-03)` → Task7 `feat(SEM-17)` → Task8 `feat(SEM-17)` → Task9 `feat(SEM-17)` → Task10 `feat(GEN-02)` → Task11 `docs`。
- **Task 1（TYP-04）**：`f16/f32/f64/f128` 注册为类型关键字（keywordMap 4 别名）；字面量后缀在 scanNumber 内最大匹配，不受影响。
- **Task 2（PAR-06）**：`Type::isRestrict` + parseType 指针后缀循环接受 `restrict`（`T* const restrict` 组合可用）；TemplateInstantiator Pointer rewrite 同步拷贝；语义（别名分析）仍挂 MEM-04。
- **Task 3（TYP-05）**：`u8/u16/u32"..."` 前缀字面量——`StringLiteralPrefix`（support 层）+ token 字段 + scanPrefixedString + parser 剥离 + sema `ArrayType(char8/16/32, n)` + codegen 定宽全局数组（逐字节存储，不转码）。
- **Task 4（PAR-08）**：C99 复合字面量 `(T){...}`——`CompoundLiteralExprAST`（内嵌 InitializerListExprAST，复用设计符解析）；parseType 增抽象 `[N]` 维（未命名数组形参显式拒绝，保住 TYP-11 pin）；块内 alloca/文件级常量；直接转义（取址/返回数组或 slice）拒绝，by-value 到全局允许；聚合初始化 helper 经 Decl.h 暴露。
- **Task 5（PAR-08）**：slice 字面量 `T[] s = {1,2,3}`——sema 元素类型校验 + codegen 栈匿名数组 + `emitArrayToSliceDecay`；`m_sliceLiteralVars` 跟踪并拒绝返回悬垂视图。
- **Task 6（PAR-03）**：类外方法定义 `RetType Class::method(...) {...}`——`FunctionDeclAST::className` + parser `IDENTIFIER :: IDENTIFIER (` 形态 + sema 校验类/方法/签名、插入隐式 `this`、`currentClass` 作用域（private 可访问）。
- **Task 7（SEM-17）**：一元重载调用点——`UnaryExprAST/PostfixIncDecExprAST::mangledCallee` + sema 查找 + codegen CreateCall；前缀 `operator++(V)`、后缀 `operator++(V, int32)`；补 `operator!` 声明（parser 缺 `!` 映射）；`!aggregate` 无重载改为诊断（原为 codegen 崩溃）。
- **Task 8（SEM-17）**：二元集合收敛——补 `% && || & | ^ << >>` 到 `getOperatorMangledName` 与调用点 switch；重载 `&&`/`||` 不短路（mangledCallee 分支先于短路 lowering）。
- **Task 9（SEM-17）**：`operator[]` / `operator()`——parser 括号对符号 + `ArrayAccessExprAST::mangledCallee` / `CallExprAST::calleeExpr`；sema 聚合对象改写为 operator 调用；普通字符串 callee 路径不动。
- **Task 10（GEN-02）**：非类型模板参数作值——TemplateInstantiator cloneExpr 把值参数名替换为 `NumberExprAST`；局部变量/形参遮蔽值参数在克隆期记录、sema 诊断。
- **Task 11（文档）**：grammar.ebnf（f16 关键字、前缀字面量、类外方法、运算符声明、复合字面量、slice 初始化）/ keywords.md（f16 系列、restrict、字面量前缀）/ semantics.md（§4 运算符集合 + §7.6 字面量与模板值存储语义）/ TODO（7 条目勾记 + INH-07/GEN-10 Non-goal 注记 + 进度快照 1470）/ 本记录。
- **关键 Ruling**：①`(T[]){...}` 是 slice 复合字面量、`(T[N]){...}` 是定长数组（本语言 `T[]` = slice）；②禁转义只查直接转义、不追踪副本、by-value 拷贝到全局允许；③`foo<N>()`（值参数作模板实参）延后；④operator 沿用 raw-type mangle、`operator()` 窄路径；⑤一元/`[]`/`()` 触发类型含 Optional/Result；⑥本语言 struct 无方法（方法须用 class）；⑦`int32[] s = {1.0, 2}` 合法（窄化兼容，TYP-23），不匹配测试改用 struct 元素；⑧`restrict` 仅语法接受。
- 验证：每任务 TDD RED→GREEN；最终全量 ctest **1470/1470** 绿（2 个既有 GTEST_SKIP 不变）。工作区 clean。
- 遗留/P1-09 剩余：**仅 std.collections（STD-06~08）**，仍阻塞于「模块导出类型」语言缺口；MEM-04 的 restrict 完整语义、`foo<N>()` 显式模板实参折叠另轮。

## 2026-10-22 — 顶层用户类型声明修复（PAR-05 补丁；typedef 暂保留）

- 背景：语法审计发现「文件作用域识别不出用户定义类型」——`struct S {...}; S g;`、`S* p;`、`V f()`、`class C f()`、`B<int32> g;`、`using W = S;` 全部被误解析为变量声明。用户裁定：先修此问题，`typedef` 暂不删除。
- 根因：`parseStructDecl` / `parseClassDecl` / `parseUnionDecl` 在无 attached declarator 时会消费声明末尾的 `;`，但 `parseDeclarationImpl` 随后仍无条件检查「下一个 token 是否 IDENTIFIER」并当作 attached declarator——于是下一条声明开头的类型名被吞掉。
- 修复（`src/frontend/Parser.cpp` 四个 record 分支）：按记录声明的三种形态分派——`;`=完整声明（无声明符）、`}`=定义 + attached declarator、无 body=前向声明（下一个 `;`）或声明符内的类型引用（回退 `parseDeclarationAsType`）。前向 struct/union 显式注册占位类型；enum 分支补消费自己的 `;`。
- 测试：新增 `tests/e2e/test_toplevel_user_types.cpp` 11 个 e2e（struct/union/class 变量与指针、裸 tag 返回、`struct V f()`、模板实例变量/返回、`using` 别名、`V operator+`、attached 变量回归），RED（8 失败）→ GREEN；全量 ctest **1493/1493** 通过（+11）。
- 顺带确认的既有缺口（未在本轮修复，已记 TODO）：①`f().field`（struct/class 返回值的成员访问）对 call 结果直接 GEP → 非法 IR；②struct/union 前向声明的占位类型与后续定义不合并（`struct S; S* p; struct S {...}; p->x` 报无成员）。
- 用户裁定：`typedef` 删除延后；`using` 的完整替代能力（函数指针别名、前向别名、块/类体内使用）另行评估。

## 2026-10-23 — using 别名收口（删除 typedef / type，PAR-24 / DEC-16）

- 流程：用户裁定「删除 typedef、只留 using」→ 追问确认 typedef 硬错误、type 一并删除 → spec `docs/superpowers/specs/2026-10-23-using-alias-design.md`（builder-check 5 项决议）→ plan `docs/superpowers/plans/2026-10-23-using-alias.md`（6 任务 TDD）→ executing-plans Native 内联（master 直接提交）。
- 提交线：Task1 `88e6e4a`（数组 type-id）→ Task2 `2a5632d`（函数指针 type-id）→ Task3 `6e0aea3`（内联 enum/注解）→ Task4 `f7332d7`（迁移测试/示例）→ Task5 `141b590`（硬错误 + 删 AST 节点）→ Task6 本文档提交。
- **Task 1**：`Parser::parseTypeId()`——`parseType()` 之后补固定数组后缀（`T[2][3]`，从右到左建 `ArrayType`，维度限整数字面量）。`using A = int32[4];` 现在下标/`sizeof`/slice 退化/设计符补零全通；slice 别名 `using Sl = int32[]` 加回归守卫。
- **Task 2**：复用 `parseFunctionPointerDeclarator(..., requireName=false)` 解析抽象函数指针；`using Fn = int32 (*)(int32,int32);` 可赋值/调用/作形参/作返回值，不再静默截断成 `int32`。TDD 修正：Task1 曾提前写入该分支，已移除并重看 3 个测试失败后恢复。
- **Task 3**：`visit(UsingDeclAST)` 对齐 typedef 语义——`validateAnnotations(..., "using")`（`repr` 报 “not valid on using”）、`deprecated` 折叠、内联 enum 枚举量写入 `enumConstants`。Ruling：弃用标志同时落到解析期与 sema 期两份 `TypedefType` 包装（已解析声明持有解析期包装）。
- **Task 4**：迁移 9 个 e2e + 6 个单元测试文件 + `examples/all_features.cdd`；`using S = struct { ... };` / `using E = enum : uint16 { ... };` / 数组/函数指针别名逐一替换。移除 `ReprCOnTypedefE2011`（由 `ReprCOnUsingE2011` 覆盖），全量 1504。示例编译运行输出 `ALL FEATURES OK`。
- **Task 5**：`typedef`/`type` 在顶层、namespace、块、struct/class/union 成员体统一硬错误（`'typedef' has been removed; use 'using Name = Type;' instead`），块/成员体 `using` 报 “only allowed at namespace scope”；删除 `parseTypedefDecl`、`TypedefDeclAST`、`TypeDeclAST` 及 sema/codegen/TemplateInstantiator 分支，保留内部 `TypedefType`。8 个错误诊断测试。
- **Task 6**：`grammar.ebnf`（唯一 `type-alias` + `type-id` 产生式）、`keywords.md`（typedef/type 标 `[removed]`，using 为唯一形式）、`semantics.md` §5.5、`abi.md`/`conversions.md`/`fixed-width-types.md` 措辞；TODO PAR-24/DEC-16 `[x]` + 进度快照 1512；本记录。
- 验证：每任务 TDD RED→GREEN；最终全量 ctest **1512/1512** 通过（2 个既有 `GTEST_SKIP` 不变）；语言源 `typedef`/`type` 扫描与 `TypedefDeclAST`/`TypeDeclAST` 扫描均为空；工作区 clean。
- 遗留：块/类/结构体内 `using`（需局部类型作用域）、前向别名身份（`using N = struct Node;` 后定义仍无成员）、`*p` 对指针别名不解引用（typedef/using 既有）、`resources/main.c` 非构建源未迁移。

## 2026-10-24 — 类型解析三缺口收口

- 流程：brainstorming → spec `docs/superpowers/specs/2026-10-24-type-resolution-gaps-design.md` → builder-check（4 点裁决：局部词法键、分层 delta 栈、模板别名风险不成立、块别名不 declare）→ plan `docs/superpowers/plans/2026-10-24-type-resolution-gaps.md` → executing-plans Native。
- 提交线：Task1 `045bccc`（`*p` 穿透别名）→ Task2 `9775a95`（前向占位合并）→ Task3 `6ff650d`（块作用域 using）→ Task4 `8cef859`（成员体 using + 访问控制）→ Task5 本文档提交。
- **Task 1**：sema `UnaryOp::Deref` 与 codegen 先剥 `TypedefType`；`using P=int32*; *p` 读写、`(*p).x`、`**pp` 全通。
- **Task 2**：parser 在 `parseType()` 内联定义与 `parseDeclarationImpl()` 注册处复用已有 struct/union 占位（清空字段重填），不再 `new` 覆盖；`using N=struct Node;`、`struct S; S* p;` 定义后可见成员；parser 不置 `isComplete`，重定义判定仍归 sema。
- **Task 3**：TypeContext 新增局部别名栈（`push/pop/add/getLocalTypedef`，`addLocalTypedef` 返回解析期包装）；`parseCompoundStmt`/sema `visit(CompoundStmtAST)` 配平 push/pop；块 `using` 走 `parseUsingAliasDecl(false)`，非限定名先查局部栈，限定名跳过；块别名不 `declare`、弃用标志落到 `aliasWrapper` + 新包装。
- **Task 4**：`parseUsingAliasDecl(true)` 用扁平键登记成员别名并入 `nestedTypes`；class 记录 `type:<bare>` 访问级别；sema `visitNestedTypeDecls/visitNestedDecl` 识别 `UsingDeclAST`，`checkNestedTypeAccess` 先走 typedef 链，字段/union 成员/变量/转换目标均受 E2009 约束；TemplateInstantiator 复制 `bareName/isBlockScope`。
- **Task 5**：`grammar.ebnf`（`declaration-statement`、`class-member`、struct/union 体加 `type-alias`）、`semantics.md` §5.5（作用域/前向/解引用）、`keywords.md`、`examples/all_features.cdd`（块别名、成员别名、前向别名、别名解引用）；TODO MOD-03 `[x]` + PAR-24 补丁 + 2026-10-24 快照。
- 验证：每任务 TDD RED→GREEN；最终全量 **1534/1534** 通过（2 个既有 `GTEST_SKIP` 不变）；示例输出 `ALL FEATURES OK`。
- 遗留：前向类型按值使用（`N n;` 在定义前）未诊断；无花括号单语句体的 `using` 作用域延伸到外层块；成员别名不继承到派生类；同作用域重复别名不诊断。
- **复核修复**：`checkNestedTypeAccess` 在指针/数组每一层先检查别名链，堵住 `C::T*` 绕过 E2009 的洞；新增 `PrivateMemberAliasPointerDeniedOutside`。最终全量 **1535/1535**。

## 2026-10-25 — full_feature 验收示例

- 新增 `examples/full_feature.cdd`（908 行，25 节）：在单文件内覆盖当前实现的定宽类型/字面量、`using`（顶层/块作用域/成员体/前向别名/指针别名链）、struct/union/enum/位域、class（单继承/访问控制/protected/类外方法）、指针与函数指针、数组/切片、string_view/string/format/split、u8/u16/u32/raw/multiline 字符串、optional/result、模板与别名模板、namespace、函数与运算符重载、lambda、控制流（含 do-while）、显式转换、constexpr/compile_time/alignof/offsetof、模块（`module full_feature;` + `import std.math;` + `export`）、内联 asm、libc、thread_local/static/atomic/volatile/inline。
- 编译运行：`./build/bin/cdd examples/full_feature.cdd -o /tmp/full_feature && /tmp/full_feature`，输出 `FULL FEATURE OK`，退出码 0。
- 说明：`examples/all_features.cdd` 保持原样；full_feature 以模块单元形式演示 module/import/export。

## 2026-10-25 — 比较结果作 bool 实参

- 根因：`conversionRank`（`src/ast/Symbol.cpp`）只放行拓宽/衰变，比较表达式类型为 `int32`，`int32 → bool` 无 rank；而赋值/返回走 `checkAssignmentTypes` 的算术转换，两条路径不一致。
- 修复：`conversionRank` 增加整数 → `bool`（rank 2，低于拓宽，`int32` 重载仍优先）；`bool → 整数` 保持 rank 1；浮点 → `bool` 仍仅显式。
- 测试：`OverloadingE2E.ComparisonResultAsBoolArgument`（比较/整数实参）、`ComparisonResultPrefersInt32Overload`；全量 **1537/1537** 通过。
- `examples/all_features.cdd` 与 `examples/full_feature.cdd` 的 `check` 帮手改为 `bool` 形参，直接接收比较结果；两者分别输出 `ALL FEATURES OK` / `FULL FEATURE OK`。
- 文档：`docs/spec/conversions.md` §3 记录实参路径的整数 → `bool` 转换。

## 2026-10-26 — 先修两个缺陷，再收口语法缺口 1–6

### 先修缺陷
- **CG-05 rvalue 聚合成员**：`make().x` 原先在聚合值上直接 GEP（非法 IR/后端崩溃）；sema 将 rvalue 聚合字段标为 xvalue，codegen 先 spill 到临时 alloca 再走继承/位域 GEP，最终按值读取。`make().x = 5` 现在被拒。提交 `a805740`。
- **MOD-03 前向类型按值使用**：解析期为每个变量快照 `typeIncompleteAtParse`（`parsedDefinition`，不受后续定义追溯），sema 对未定义 struct/union/class 的按值变量/数组报 incomplete；指针/切片仍放行。提交同上。

### 1. 模块导出类型
- `ModuleLoader::collectImportSpecifiers` + `preloadImports`：在解析导入者前先加载模块，类型名进入 `TypeContext`；`export struct/class/using/template/函数` 均可跨模块使用。新增 `ModuleLoaderTest.ExportedTypesVisibleBeforeParsingImporter`。提交 `53768f1`。
- 已知边界：非导出类型在 parse 期仍可能被导入者看到（TypeContext 全局、无模块作用域），留待类型可见性轮次。

### 2. 模块头约束
- 解析器强制 `module` 必须是首个顶层声明、不可重复、必须有名字；驱动校验入口文件 module 名 ↔ 文件路径（点分模块按目录匹配）。提交 `3d88e2b`。

### 3. 局部存储类与局部记录
- 函数内 `static T x` / `thread_local T x` / `static thread_local T x`：codegen 提升为内部链接全局（可 TLS），初始化器要求编译期常量；未初始化零初始化。`static` 局部跨调用持久。
- 函数内 `struct/union/enum` 定义经 `parseDeclaration` 进入 `DeclStmtAST`，sema/codegen 复用既有记录路径；局部 `class` 原路径不变。提交 `c2d509b`。

### 4. 通用声明符
- `parseFunctionPointerDeclarator` 重写为通用括号声明符：`(*name[suffix])[suffix]`，支持 `int32 (*get(int32))(int32)`、`using F = int32(int32)` + `F*`、`int32 (*p)[3]`、`int32 (*arr[2])(int32)`。
- 新增 `parseAbstractFunctionSuffix`；`CallExprAST::isExprCallee` 支持 `arr[0](1)` / `get()(1)` 表达式 callee 间接调用。函数类型别名经 typedef 剥层后可间接调用。提交 `4d77701`。

### 5. 类与成员运算符
- `CodegenContext::getLLVMType(Pointer)` 不再递归解析 pointee（opaque pointer），修复 `class Node { Node* next; }` 爆栈。
- 类内成员运算符（二元/一元）调用点接入：sema 在自由函数失败后 `resolveMethod`，codegen 传 this；类外 `V::operator+` 解析与签名绑定打通；类体运算符符号改为 token 映射（此前 `"operator"` 丢符号）。提交 `8dd88ca`。

### 6. 编译期/内省
- 新增 `CompileTimeIfStmtAST`：函数体 `compile_time.if (cond) { ... } else { ... }` 按语句解析，sema 只分析选中分支，codegen 只出选中分支。
- `offsetof` 支持成员链与数组下标：`offsetof(T, a.b)`、`offsetof(T, arr[i])`，按 LayoutBuilder 逐层累加偏移。提交 `2597f62`。
- 未做（规范明确挂账）：自定义注解注册制（ANN-08，未知注解仍 E2010）、CT-08 类型作为值。

### 验证
- 每项 TDD（RED→GREEN）后提交；全量 `ctest -j8` **1559/1559**（2 个既有 GTEST_SKIP）。
- `examples/all_features.cdd` → `ALL FEATURES OK`；`examples/full_feature.cdd` → `FULL FEATURE OK`。

## 2026-10-26 — 基础 C 声明/语句语法收口（9 项）

- 流程：用户指定 9 项语法缺口 → 计划 `docs/superpowers/plans/2026-10-26-syntax-gaps-r3.md`（`docs/superpowers` 被 .gitignore，未入库）→ 新增 `tests/e2e/test_syntax_gaps.cpp`，11 个 e2e 先 RED → 逐项 GREEN。
- 提交线：`0bc3be2`（空语句）→ `88fca71`（聚合成员声明符/函数指针字段）→ `b07dc1f`（sizeof/alignof type-id）→ `42d3ed1`（后置限定符）→ `dfcb9cc`（块内 extern）→ `607ff32`（record attached 声明符）→ `dad042d`（多声明符复杂后继）→ `31a68fb`（带方法 struct/ClassType 注解修复）→ `27a1242`（attached 指针函数测试）。

### 实现要点
- **空语句**：`parseStmt()` 遇单独 `;` 直接返回 `NullStmtAST`（此前被当表达式起始报错）。
- **聚合成员**：新增 `Parser::parseMemberFieldDecl()`，统一处理 `name`、前导 `*`、`(*f)(params)`、`(*p)[N]`、数组后缀和位域；struct/class/union 三个成员循环改为“首声明符 + 逗号循环”，后继声明符从 `stripDeclaratorType()` 的基础类型重新开始。`stripDeclaratorType` 增加 Function 层剥离，支持 `int (*f)(int), (*g)(int)`。
- **直接函数指针字段**：`struct S { int32 (*f)(int32); }` 现在是普通字段；`s.f(1)` 在 sema 走字段函数指针分支（`MethodCallExprAST::fieldCallee`），codegen 加载字段并以无 `this` 的间接调用签名调用。class 字段同样支持并检查访问级别。
- **指向数组的指针字段**：`int32 (*p)[3]` 复用 `parseFunctionPointerDeclarator`，codegen 走既有指针/数组访问路径。
- **sizeof/alignof**：类型分支从 `parseType()` 改为 `parseTypeId()`，支持固定数组维度和抽象函数指针；`sizeof((T[N]){...})` 的复合字面量仍走表达式分支。
- **后置限定符**：`parseType()` 在 `parseBaseType()` 后再次消费 `const/volatile/atomic`，`int32 const` 与 `const int32` 同型；`atomic` 克隆逻辑保持在指针层之前。
- **块内 extern**：块存储类分支接受 `extern`，`VarDeclAST::isExternLocal`；codegen 复用/声明外部全局而非分配 alloca；不参与 static-local 常量初始化检查。
- **record attached 声明符**：新增 `Parser::parseDeclarationFromBaseType()`，处理 `*p`、`f()`、`*f()`、`(*fp)(...)`、`(*p)[N]`；struct/union/enum 返回声明节点，class 放入 `attachedVars` 以保留方法/访问段注册。
- **多声明符复杂后继**：`parseVariableDeclList` 的逗号循环遇 `(` 调 `parseFunctionPointerDeclarator(baseType, ...)`，支持 `int32 (*f)(int), (*g)(int);` 与 `int32 (*p)[3], (*q)[3];`。
- **struct 方法兼容**：为不破坏 `AnnotationLayoutE2E.PackedOnMethodStructOk`，`parseStructDecl` 保留成员函数到 `StructDeclAST::methods`；带方法的 struct 解析期注册为 `ClassType`（字段仍默认 public）。同时修复 sema 布局注解把 `ClassType*` 强转 `StructType*` 的 UB：按 `isClass` 分派，并在注解前 `getOrCreateClass`。

### 验证
- 新增 `SyntaxGapsE2E` 12 个用例（空语句、struct/class/union 多字段、直接函数指针字段、指向数组字段、sizeof/alignof type-id、后置限定符、块 extern、attached 指针/函数/函数指针/返回指针函数、多复杂声明符）。
- 全量 `ctest --test-dir build -j8` **1572/1572** 通过（2 个既有 GTEST_SKIP 不变）。
- `examples/all_features.cdd` → `ALL FEATURES OK`；`examples/full_feature.cdd` → `FULL FEATURE OK`。

## 2026-10-27 — 语法缺口审计（第三轮后）

- 对基础 C 声明/语句 9 项收口后的剩余语法缺口做规范 + CLI 复核；详细清单写入 `TODO.md` **§24 语法缺口审计**。
- 分类：§24.1 规范已承诺未收口（SG-01 模板常量实参、SG-02 块内 extern 函数）；§24.2 C 兼容/声明符简化（SG-03 ~ SG-07）；§24.3 规范未覆盖扩展（SG-08 ~ SG-11）；§24.4 非语法边界（SG-B01 ~ SG-B06）；§24.5 Non-goals。
- 审计基线：全量 **1572/1572**（2 个既有 `GTEST_SKIP`）；`examples/all_features.cdd` → `ALL FEATURES OK`，`examples/full_feature.cdd` → `FULL FEATURE OK`。
- 本轮只改文档，未改代码；后续按 §24.6 顺序逐项 TDD 收口。

## 2026-10-27 — 语法缺口补充审计（第二轮）

- 在 `TODO.md` **§24** 之后新增 **§25 语法缺口补充审计**；本轮仍只改文档，未改代码。
- §25.1 规范已承诺未贯通：**SG-12** 数组维度只接受整数字面量、**SG-13** 位域宽度不能解析 enum/const 标识符、**SG-14** `static_cast`/`reinterpret_cast` 目标类型仍不走完整 type-id。
- §25.2 C 兼容/声明符：**SG-15** 静态数据成员类外定义、**SG-16** 括号声明符与多函数声明符、**SG-17** 函数类型形参未按 C decay、**SG-18** C 字面量子集、**SG-19** asm 连续冒号/命名操作数。
- §25.3 需先裁决的扩展：**SG-20** 模块扩展、**SG-21** 控制流扩展、**SG-22** struct/class 成员扩展、**SG-23** 模板类静态成员与类外模板成员定义。
- §25.4 非语法边界：**SG-B07** 类外静态成员函数 sema 未绑定、**SG-B08** 成员 `operator[]`/`operator()` 调用绑定、**SG-B09** 全局 `string_view` 字面量 codegen 段错误。
- 复核方式：`build/bin/cdd -fsyntax-only` 复现解析失败，`-c` 复现 sema/codegen 边界；对照通过项包括 `int32 (*p)[3]`、`sizeof(int32(*)(int32))`、`struct U { int32 c : 1 + 1; }`、`extern "C"`。
- 基线沿用 **1572/1572**（2 个既有 `GTEST_SKIP`）；两个示例仍为 `ALL FEATURES OK` / `FULL FEATURE OK`。
- 建议顺序并入 §24.6：**SG-01 → SG-02 → SG-12 → SG-04/SG-14 → SG-03**，随后 SG-16/SG-17 与 P2 扩展。

## 2026-10-27 — SG-14 显式转换完整 type-id

- 修复 §25 **SG-14**：`static_cast` / `reinterpret_cast` 的类型分支从 `parseType()` 改为 `parseTypeId()`，现在支持抽象函数指针 `Ret(*)(...)` 与指向数组的指针 `T(*)[N]`。
- 新增 `CastOperatorsE2E.StaticCastToFunctionPointerTypeId`、`CastOperatorsE2E.ReinterpretCastToPointerToArrayTypeId`；先 RED（解析失败）后 GREEN。
- 全量 `ctest --test-dir build -j8` **1574/1574** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-27 — SG-12/SG-13 数组常量维度与位域常量

- 修复 §25 **SG-12**：新增解析期常量表（enum 常量、`const`/`constexpr` 整型变量）与 `parseArrayDimension()`；变量、聚合成员、数组形参、`type-id` 的 `[const-expr]` 现在接受字面量表达式/枚举/const 整型。VLA 仍是 Non-goal。
- 修复 §25 **SG-13**：`evalIntConstExpr()` 增加标识符查找回调，枚举值解析时登记，`parseBitFieldWidth()` 复用同一查找；`int32 a : EnumN`、`: ConstN`、`: 1 + 1` 均通过。
- 新增 `SyntaxGapsE2E.ArrayDimensionsAcceptConstExpressions`、`TypeIdArrayDimensionsAcceptConstExpressions`、`BitFieldWidthsAcceptEnumAndConst`；先 RED（三例均失败）后 GREEN。
- 全量 `ctest --test-dir build -j8` **1577/1577** 通过（2 个既有 `GTEST_SKIP`）；两个示例仍输出 `ALL FEATURES OK` / `FULL FEATURE OK`。

## 2026-10-27 — SG-B09 全局 string_view 字面量崩溃

- 修复 §25 **SG-B09**：全局 `string_view s = "abc";` 在 `-c` 时段错误。根因是全局初始化阶段 IRBuilder 无 insertion block，`CreateGlobalString` 未传 Module 导致空指针解引用。
- 改为显式传 `&ctx.getModule()`；新增 `SyntaxGapsE2E.GlobalStringViewLiteralDoesNotCrash`，先 RED（测试进程 SIGSEGV）后 GREEN。
- 全量测试与示例在后续批次统一复跑。

## 2026-10-27 — SG-16 括号声明符（部分）

- `parseFunctionPointerDeclarator` 的 `*` 改为可选；`int32 (x);`、`int32 (f)(void);`、`int32 (a)[3];` 现在按 C 声明符语义解析。
- 新增 `SyntaxGapsE2E.ParenthesizedDeclarators`，先 RED（unknown type）后 GREEN。
- `int32 f(), g();` 多条函数声明符仍挂账（需函数声明列表/MultiVarDecl 扩展）。
- 全量 `ctest --test-dir build -j8` **1579/1579** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-27 — SG-17 函数类型形参 decay

- 修复 §25 **SG-17**：`parseParamDecl()` 在参数名后遇到 `(` 时解析抽象函数类型，并按 C 规则退化为函数指针；`int32 f(int32 cb(int32))` 与 `int32 f(int32 (*cb)(int32))` 等价。
- 新增 `SyntaxGapsE2E.FunctionTypeParameterDecaysToPointer`，先 RED 后 GREEN。
- 全量 `ctest --test-dir build -j8` **1580/1580** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-27 — SG-B07 类外静态成员函数定义

- 修复 §25 **SG-B07**：sema 在处理 `Class::method` 类外定义时，先查 `staticMemberIndex`；命中静态方法后绑定到脱糖后的全局符号，不再报 `method ... is not declared`，也不插入隐式 `this`。
- 新增 `SyntaxGapsE2E.OutOfClassStaticMethodDefinition`，先 RED（sema 报错）后 GREEN。
- 全量 `ctest --test-dir build -j8` **1581/1581** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-27 — SG-16 多函数声明符

- 补齐 `int32 f(), g();`：`parseFunctionDecl` 增加 `allowComma`，普通函数声明分支收集多个 `FunctionDeclAST` 并包成 `MultiVarDeclAST`；sema 的 `MultiVarDeclAST` 分派补 `FunctionDeclAST`。
- 新增 `SyntaxGapsE2E.MultipleFunctionDeclarators`，先 RED（`expected '{' or ';'`）后 GREEN。
- 全量 `ctest --test-dir build -j8` **1582/1582** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-28 — SG-15 静态数据成员类外定义

- 修复 §25 **SG-15**：parser 识别 `type Class::name [= init];`；sema 用 `staticDataDecls` 把初始化器挂到类内 `staticMembers` 的 VarDeclAST 上，避免生成重复全局。
- 新增 `SyntaxGapsE2E.OutOfClassStaticDataDefinition`，先 RED（unknown type 'x'）后 GREEN。
- 全量 `ctest --test-dir build -j8` **1583/1583** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-28 — SG-19 asm 连续冒号（部分）

- `parseAsmExpr()` 兼容 `TOKEN_COLON_COLON`：`::` 计作两个空段分隔符，`asm("nop" ::: "memory")` 不再因 lexer 折叠 `::` 而失败。
- 新增 `SyntaxGapsE2E.AsmAdjacentEmptyOperandSections`，先 RED 后 GREEN。
- `[name]` 命名操作数及绑定仍挂账。
- 全量 `ctest --test-dir build -j8` **1584/1584** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-28 — SG-18 前导小数点与 LL/ULL（部分）

- lexer 支持 C 的前导小数点浮点 `.5`；`consumeIntegerSuffix` 改为消费 `u` + 最多两个 `l`，支持 `LL`/`ULL`（映射到 int64/uint64）。
- 新增 `SyntaxGapsE2E.LeadingDotFloatAndLongLongSuffix`，先 RED 后 GREEN。
- 仍挂账：字符/字符串前缀、raw delimiter、十六进制浮点。
- 全量 `ctest --test-dir build -j8` **1585/1585** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-28 — SG-B08 成员运算符调用绑定

- sema 的 `operator[]` / `operator()` 解析在自由函数之后增加类方法表查找；命中成员运算符时以对象地址作为隐式 `this`，codegen 相应传地址。
- 新增 `SyntaxGapsE2E.MemberSubscriptOperatorRead`、`SyntaxGapsE2E.MemberCallOperator`，先 RED（no matching operator）后 GREEN。
- 全量 `ctest --test-dir build -j8` **1587/1587** 通过（2 个既有 `GTEST_SKIP`）；两个示例输出 `ALL FEATURES OK` / `FULL FEATURE OK`。
- 写穿仍受无引用返回类型限制（SG-B02），不属于调用绑定缺口。

## 2026-10-28 — SG-18 C 字面量兼容子集收口

- lexer 新增：前导小数点 `.5`、`LL`/`ULL`、`L'a'`/`u'a'`/`U'a'`/`u8'a'` 字符前缀、`L"..."`/`u"..."`/`U"..."` 字符串前缀、`R"(...)"` / `R"delim(...)delim"` 原始字符串、十六进制浮点 `0x1p3` / `0x1.8p1` / `0x.8p1`。
- `L` 字符串映射到 32 位 code unit；本语言无独立 `wchar_t`，已在 TODO 记录该兼容取舍。
- 新增 `SyntaxGapsE2E.CCharLiteralPrefixes`、`CStringLiteralPrefixesAndRaw`、`HexFloatLiteral` 与既有 `LeadingDotFloatAndLongLongSuffix`，先 RED 后 GREEN。
- `docs/spec/grammar.ebnf` 同步字面量产生式；全量 `ctest --test-dir build -j8` **1591/1591** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-28 — SG-19 asm 命名操作数

- parser 在 output/input 约束前接受 `[name]`；`AsmOperand` 记录名字。
- sema 收集 outputs→inputs 顺序的命名表，拒绝重名与模板中的未知名，并允许 `%[name]`。
- codegen 把模板 `%[name]` 翻译为 LLVM `$N` 操作数引用，数字引用与 `%%` 转义保持原行为。
- 新增 `SyntaxGapsE2E.NamedAsmOperand`（先 RED 后 GREEN）；`docs/spec/grammar.ebnf` 同步 asm 产生式。
- 全量 `ctest --test-dir build -j8` **1591/1591** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-28 — §25 本轮收尾

- SG-18（C 字面量兼容子集）与 SG-19（asm 连续冒号/命名操作数）已全部修复并分别提交。
- 验证：全量 `ctest --test-dir build -j8` **1591/1591** 通过（2 个既有 `GTEST_SKIP`）；`examples/all_features.cdd` → `ALL FEATURES OK`，`examples/full_feature.cdd` → `FULL FEATURE OK`。
- 用户裁决：SG-20 ~ SG-23 属于规范未覆盖的扩展，本轮暂不实现，保留为后续产品决策项。

## 2026-10-29 — all_features.cdd 扩展到 26 节

- 在 `examples/all_features.cdd` 中加入 `module all_features;` 与 `import std.math;`，并补 22 ~ 26 节：lambda、运算符重载、存储类/限定符、模块、声明符/静态成员/成员运算符。
- 第 1 节补 SG-18 字面量：`.5`、`LL`/`ULL`、字符/字符串前缀、`R"(...)"`、十六进制浮点；第 15 节补 `if/else` 与 `do-while`；第 19 节补命名 asm 操作数与 `:::`。
- 第 26 节覆盖近期修复：常量数组维度、enum 位域宽度、多函数声明符、括号声明符、函数类型形参 decay、类外静态数据/方法、成员 `operator[]`/`operator()`、全局 `string_view`。
- 验证：`build/bin/cdd examples/all_features.cdd` + 运行输出 `ALL FEATURES OK`；全量 `ctest --test-dir build -j8` **1591/1591** 通过（2 个既有 `GTEST_SKIP`）；`examples/full_feature.cdd` 仍输出 `FULL FEATURE OK`。

## 2026-10-29 — 语言易用性缺陷收口（Review 2.2–2.6）

- **2.2 序列访问统一**：数组/切片/`string_view`/`string` 统一 `.len` 属性（方法形式保留为别名）；`string` 增加 `.capacity`/`.char_count`/`.data`；新增 `s[a:b]` 范围切片（字符串 → `string_view`，数组/切片 → `T[]`，运行时越界 panic）。
- **2.3 空值字面量**：保留 `null` 为唯一拼写，移除 `nullptr` 关键字；文档明确 null 只隐式转指针，不转 optional/result。
- **2.4 Optional/Result**：条件与 `!`/`&&`/`||`/三元条件按 `valid`/`ok` 取真值；新增 `is_some`/`is_none`/`unwrap`/`value_or` 与 `is_ok`/`is_err`/`unwrap`/`value_or`。
- **2.5 字符串边界**：`string_view`/字面量只隐式转 `const char*`，可变 `char*` 需显式 cast；新增 `.data`；本地拥有型 `string` 在作用域退出时自动释放（显式 `destroy()` 仍幂等）。
- **2.6 可存储闭包**：新增 `auto` 变量类型推导；有捕获 lambda 的闭包 struct 记录调用符号，`auto f = [..]{..}; f(args)` 可反复调用（按值/按引用捕获均验证）。
- **附带修复**：parser 限定符不再污染共享标量单例（`const char*` 曾使全局 `char`/`int32` 变 const）；class 成员路径接受前导 `const/volatile/atomic`；本地 optional/result 未初始化时零初始化；C 绑定字符串参数改为 `const char*`。
- **验证**：`examples/all_features.cdd` 输出 `ALL FEATURES OK`；`examples/full_feature.cdd` 输出 `FULL FEATURE OK`；全量 `ctest --test-dir build -j8` **1604/1604** 通过（2 个既有 `GTEST_SKIP`）。

## 2026-10-31 — P1/P2 语法缺口收口（SG-01 ~ SG-07）

- **SG-01 模板常量实参**：类型位置与调用位置接受整型常量表达式（`A<1+2>`、`g<1+1>()`）；模板体内依赖值参数表达式（`N`、`N + 1`）通过 `TypeInstanceType::valueArgExprs` / `CallExprAST::explicitTemplateValueExprs` 记录，在单态化时用绑定值求值。新增模板嵌套实例字段的布局物化（`ensureInstanceVisited`）。
- **SG-02 块内 extern 函数**：`extern int32 f(...);` 在块内解析为函数原型，sema 块作用域注册，codegen 复用模块级声明。
- **SG-03 offsetof 常量下标**：`offsetof(S, a[1+1])`、枚举常量下标均可用；parser 保存 `indexExpr`，sema 用 `evaluateConstexpr` 折叠。
- **SG-04 C 风格 cast 完整 type-id**：cast 类型分支改走 `parseTypeId()`，支持 `(int32 (*)(int32))p`。
- **SG-05 深层声明符**：`parseFunctionPointerDeclarator` 改为递归构造器列表，支持 `int32 (*(*g)(int32))(int32)` 及任意嵌套括号/指针/后缀。
- **SG-06 C99 数组形参限定符**：`int32 a[static 3]`、`const int32 a[const 3]` 均可解析并去糖为 slice。
- **SG-07 相邻字符串拼接**：parser 字符串主表达式拼接相邻 token（含 raw/多行），`"ab" "cd"` → `"abcd"`。
- **示例**：`all_features.cdd`、`full_feature.cdd` 新增第 27 节覆盖上述 7 项。
- **验证**：新增 7 个 `SyntaxGapsE2E` 回归测试；全量 `ctest --test-dir build -j8` **1611/1611** 通过（2 个既有 `GTEST_SKIP`）；两个示例输出 `ALL FEATURES OK` / `FULL FEATURE OK`。

## 2026-11-01 — compile_time 开发体验（CTX-01，CT-15 ~ CT-21）

- 背景：按已确认的 1 → 2 → 3 计划，为 `compile_time` 补充开发期便利设施；所有新增标量查询必须停留在编译期，不能引入运行时调用或运行时循环。
- 完成事项：
  - **第一批（CT-15/16/17/18）**：`file/line/column/function`；`fail/warn/debug`；`assert_eq/ne/size/align/offset`；`is_same`/`is_*` 类型谓词；`int_min/max`、`uint_max`、`bit_width`、`float_epsilon/max/digits`；`has_field`/`field_*`；`has_attribute`/`attribute_*`；`target.endian/pointer_width/triple/has_feature/is`；`build.mode/compiler/host_*`、`version.major/minor/patch/at_least`。
  - **第二批（CT-17/19）**：`size_of(expr)`/`align_of(expr)`、`offset_of(T,"a.b")`、裸字段名、`bit_size_of`、`padding_of`、`is_packed`、`type_name_of`/`type_kind_of`、`enum_*`、`method_*`/`base_name_of`/`is_base_of`、`layout_size_of/align_of`、`format`/`to_string`。
  - **第三批（CT-20/21）**：`type_of/array_of/pointer_to/const_of` 编译期类型值；`is_convertible/is_assignable`；`template_name_of/template_arg_count/template_arg_name`；`compile_time.repeat(N) { ... }` 编译期展开（上限 1024，零运行时循环）。
  - 性能：`CompileTimeEvaluator` 新增按 `Type*` 的 `LayoutBuilder` 缓存；标量查询继续走 `ctInt/ctFloat` 的 LLVM 常量路径；仅 `fields_of/attributes_of` 保留运行时只读表。
- 关键文件：`src/frontend/Parser.{h,cpp}`（`CTArgPolicy`、repeat 解析）、`src/sema/CompileTimeEvaluator.{h,cpp}`（查询分派/布局缓存/字段/枚举/类/格式化/类型值）、`src/sema/SemanticAnalyzer.{h,cpp}`（诊断/断言/布尔与无符号映射/repeat）、`src/ast/Type.h`（字段注解保留、`Type::annotations`）、`src/ast/Decl.{h,cpp}`（`CompileTimeRepeatStmtAST`）、`src/sema/CompileTimeEvaluator.h`（`ConstValue::TYPE`）。
- 测试：新增 `tests/sema/test_compile_time_ergonomics.cpp`（36 个 case）、`tests/frontend/test_compile_time_parse.cpp` 的 `IsSameWrapsBothTypeArgs`/`AssertSizeFirstTypeRestExpr`/`FailTakesExpressionArg`/`RepeatParses`、`tests/e2e/test_compile_time.cpp` 的 `ErgonomicsScalarQueries`/`ErgonomicsReflectionQueries`/`ErgonomicsBatch2Queries`/`RepeatUnrollsAtCompileTime`。
- 验证：`ctest --test-dir build -j8` → **1658/1658** 通过，2 个既有 `GTEST_SKIP` 不变。
- 文档：`docs/spec/compile_time.md`（§4/§5/§7/§9 扩展）、`docs/spec/grammar.ebnf`（compile-time-member/repeat）、`TODO.md`（CT-15 ~ CT-22）、本文件。
- 遗留：**CT-22** `compile_time.for_fields(T, f)` / `for_enum_values(E, v)` 未实现——直接复用同一 body AST 无法为每个字段/枚举值生成不同的运行时绑定，需要 AST 克隆或 codegen 特化；不以运行时循环冒充编译期展开。**CT-09** 哈希缓存仍未做（当前仅 Type* 布局缓存）。

## 2026-11-01 — 标准库补全 + mimalloc 后端

- 背景：用户要求完善标准库；能用 `compile_time` 的场合使用编译期求值；内存库切换已引入的 mimalloc；语法不再改动；容器等 std 不得实现在编译器内，已内嵌/不合适的部分迁出。
- 架构决策：
  - 容器/算法/解析/路径/文件/时间/环境/测试辅助全部放在 `libs/std/*.cdd`，编译器只保留语言类型（string/optional/result/slice）与需要调用点信息的 `print/format/assert/panic` 内建。
  - `std.c` 删除编译器内嵌副本，改为只从 `<std>/c.cdd` 载入；缺失安装时驱动显式报错（`--no-prelude` 仍可关闭）。
  - mimalloc 以静态库 + `MI_OVERRIDE=ON` 接入编译器、测试与生成程序；Debug 构建关闭 mimalloc 的 guard/stats/profile 以保持性能。
- 完成事项：
  - **构建/驱动**：`CMakeLists.txt` 接入 `thirdparty/mimalloc`（`libmimalloc.a`）；`CompilerDriver` 支持 `-M`/安装目录/可执行文件相对路径查找标准库；`-l/-L` 真正传入 `Linker`，自动追加 `-lm` 与 `-lmimalloc`；Linker 做 shell quoting。
  - **std.mem**：`mi_malloc/mi_calloc/mi_realloc/mi_free/mi_malloc_aligned`；`bytes_copy/move/set/compare`（STD-03）；`Allocator` 接口、`Arena`（bump + create/destroy/reset）、`Pool`（free list）；`backend_name()=="mimalloc"`；模块级 `compile_time.static_assert` 校验 usize/指针宽度。
  - **std.collections**：`Vec<T>`、`Deque<T>`、`HashMap<K,V>`、`HashSet<K>`；显式 `init/destroy`、容量增长、开放寻址 + 墓碑 + 0.7 负载；哈希/相等通过函数指针回调注入。
  - **新增模块**：`std.hash`（FNV-1a + splitmix64 + combine + 容器适配器）、`std.sort`（泛型稳定插入排序/二分查找）、`std.parse`（2..36 进制整数、浮点、bool、整数渲染）、`std.ascii`、`std.path`（编译期选择平台分隔符）、`std.fs`、`std.time`、`std.os`、`std.testing`。
  - **编译器缺陷修复（阻塞 stdlib 的部分）**：
    1. `TemplateInstantiator::cloneExpr` 漏掉 `BoolLiteralExprAST`，模板方法中的 `return true/false` 被当成裸 return，非 void 方法误报。
    2. 模板注册表未按 Parser 的扁平命名空间键登记限定模板，`std::Box<T>`/`std::id<T>` 无法实例化；`instanceName` 统一 canonicalName，避免 `std::Box` 与命名空间内 `Box` 生成两个实例。
    3. `string` 缓冲区不写 NUL，`string.data` 传给 libc 不是合法 C 字符串；`string.new/append/push`、string copy helper、format/动态 format 全部维护终止符。
- 测试：
  - 新增 `tests/e2e/test_std_collections.cpp`（4）、`test_std_text.cpp`（4）、`test_std_system.cpp`（2）、`test_std_mem.cpp` 扩展（Allocator/Arena/Pool/backend）、`test_string.cpp` NUL 回归、`test_generics.cpp` 模板布尔/命名空间查找回归、Linker/StdPrelude 回归。
  - 新测试 harness 先 `preloadImports` 再 parse，确保模块导出类型/模板在 parse 期可见。
- 验证：
  - `ctest --test-dir build -j8` → **1679/1679** 通过，2 个既有 `GTEST_SKIP` 不变。
  - `std.io::read_line`/`file_read_line` 对 `cap<=0` 增加空指针/容量守卫（避免 OOB 写）。
  - `examples/all_features.cdd` → `ALL FEATURES OK`；`examples/full_feature.cdd` → `FULL FEATURE OK`。
- 文档：`docs/spec/stdlib.md` 更新 mem/collections/hash/sort/parse/ascii/path/fs/time/os/testing 与内建边界；`TODO.md` 更新 STD-03/05/06/07/08/09/15~22、P1-09、P2-02 与进度快照。
- 遗留：placement 分配、`List`/`BTree`、`std.unicode`、完整目录遍历/stat、argv/进程控制、`cdd test` runner；CT-22 仍待 AST 克隆/codegen 特化。


## 2026-11-01 — 示例收敛

- 删除 `examples/full_feature.cdd`，仅保留 `examples/all_features.cdd` 作为唯一验收示例。
- 原因：两份示例覆盖高度重叠，同步维护成本高；`all_features.cdd` 已覆盖当前实现的全部公开特性并通过 `ALL FEATURES OK`。
- 验证：`build/bin/cdd examples/all_features.cdd` 运行输出 `ALL FEATURES OK`；全量 `ctest --test-dir build -j8` **1679/1679** 通过（2 个既有 `GTEST_SKIP`）。


## 2026-11-01 — 移除 libsafec

- 删除 `src/libsafec/`（`safec.h`、`malloc.cpp`、`printf.cpp`、`string.cpp`）与 `tests/libsafec/`（malloc/printf/string 测试）。
- 移除 `src/CMakeLists.txt` 中的 `safec` 库目标与源文件排除规则，移除 `tests/CMakeLists.txt` 对 `safec` 的链接。
- 依据：全仓库只有 `tests/libsafec` 引用 `safec::`；编译器、JIT、生成程序与 `libs/std` 均不依赖。它是早期 1 MB bump allocator + 精简 libc 包装，已被 mimalloc + `std.mem`/`std.string` 取代。
- 验证：全量 `ctest --test-dir build -j8` **1657/1657** 通过（2 个既有 `GTEST_SKIP`；减少的 22 个测试全部来自被删除的 `libsafec` 自身测试）；`examples/all_features.cdd` 仍输出 `ALL FEATURES OK`。

## 2026-11-01 — std.collections 容器拆分

- 将 `libs/std/collections.cdd` 按容器拆为独立模块：
  - `libs/std/collections/vec.cdd` → `module std.collections.vec;`
  - `libs/std/collections/deque.cdd` → `module std.collections.deque;`
  - `libs/std/collections/hashmap.cdd` → `module std.collections.hashmap;`
  - `libs/std/collections/hashset.cdd` → `module std.collections.hashset;`
- `libs/std/collections.cdd` 保留为汇总模块，仅 `import` 上述四个模块并保留 `uint8` 宽度静态断言；`import std.collections;` 的既有用法不变。
- 新增 `StdCollectionsTest.DirectContainerModuleImports`，验证四个容器模块可脱离汇总模块直接导入并使用。
- 文档：`docs/spec/stdlib.md` §14 更新模块布局与导入方式。
- 验证：`ctest --test-dir build -j8` → **1658/1658** 通过（2 个既有 `GTEST_SKIP`，新增 1 个测试）；`examples/all_features.cdd` → `ALL FEATURES OK`。

## 2026-11-01 — constexpr 容器与常量表

- **求值器**：`CtValue`/`CtContainer` 共享值模型（`src/ast/CtValue.*`）；`constexpr` 解释器支持 block/if/while/do/for/break/continue/return、赋值/复合赋值、`++`/`--`、`&local` 出参、字符串字面量与 `string_view.len()/char_at()`。
- **容器执行**：`Vec`/`Deque`/`HashMap`/`HashSet` 的 `constexpr` 语义在 `SemanticAnalyzer` 中按运行时 API 执行；HashMap/HashSet 复刻开放寻址/墓碑/0.7 负载，并通过解释器调用 hash/equality 回调。支持标量、enum、`string_view` 元素。
- **常量表逃逸**：`constexpr std::Vec/Deque/HashMap/HashSet` 变量在 codegen 中物化为私有只读常量数组 + 与运行时 ABI 一致的常量结构体（`src/ast/Decl.cpp`）；支持全局、static 与局部 constexpr 容器。物化表只读：修改方法/字段赋值/可变拷贝均诊断拒绝。
- **限制**：求值步数上限 1,000,000；回调须可编译期求值；暂不支持任意指针、嵌套容器、`string` 元素、128 位整数常量。
- **顺带修复**：`typeToMangled` 补 `Str`/`String`（此前 `Vec<string_view>` 实例化为 `std_Vec$unknown`）；`TemplateInstantiator::rewrite` 递归替换函数类型内的 `TypeVar`（`bool (*)(T,T)` 的回调调用不再产生 IR 签名不匹配）。
- **测试**：新增 `tests/e2e/test_constexpr_containers.cpp`（constexpr Vec/Deque/HashMap/HashSet、string_view、出参、步数上限、只读检查、Vec/Deque/HashMap/HashSet/string_view 常量表运行时读取）；`tests/ast/test_type_nodes.cpp` 增加字符串 mangling 回归；重启用 `ConstexprForLoop`/`ConstexprNestedCall`。
- **终审修复**：
  - `get_ptr`/`.data` 指针逃逸：constexpr 表禁止 `get_ptr`，禁止把 `.data`/指针算术结果装入可变指针；按值传给可变形参也诊断拒绝。
  - 只读检查改用 `Symbol::isConstexprContainer` 身份而非变量名，修复同名局部变量的误报；`T.len`/`T.data[i]` 等标量读取恢复允许。
  - 赋值/局部初始化/形参绑定统一做声明类型转换（`ctBindValue`），修复 `int8 x = 300` 的折叠值不截断。
  - `constexpr` 重载按 mangled 签名登记与查找，修复同名重载折叠到最后一份实现。
  - 修复 `INT64_MIN / -1` 在 Parser 常量求值与 `CtValue` 中的有符号溢出崩溃。
  - 普通 constexpr 辅助函数的 `&out` 实现 copy-in/copy-out（`*p = v` 写回调用者局部）。
  - 循环体失败立即中止，避免误报为步数超限。
- **验证**：全量 `ctest --test-dir build -j8` **1699/1699** 通过（2 个既有 `GTEST_SKIP`）；`examples/all_features.cdd` → `ALL FEATURES OK`。
- 文档：`docs/spec/compile_time.md` §7.5、`docs/spec/stdlib.md` §14。

## 2026-11-01 — 容器模块拍平

- 用户将容器文件从 `libs/std/collections/` 移至 `libs/std/`，并删除汇总入口 `libs/std/collections.cdd`。
- 模块声明同步改为 `std.vec`、`std.deque`、`std.hashmap`、`std.hashset`（点号导入映射到 `libs/std/<name>.cdd`）。
- 更新测试导入：
  - `tests/e2e/test_std_collections.cpp` 各容器测试改为按需导入对应模块；直接导入测试改用扁平模块名。
  - `tests/e2e/test_constexpr_containers.cpp` 全部改为四个扁平模块导入。
- 更新文档：`docs/spec/stdlib.md` §14 与 `docs/spec/compile_time.md` §7.5；`libs/std/hash.cdd` 注释同步。
- 验证：`ctest --test-dir build -R 'StdCollectionsTest|ConstexprContainersTest' -j8` **30/30** 通过；全量 `ctest --test-dir build -j8` **1699/1699** 通过（2 个既有 `GTEST_SKIP`）；`examples/all_features.cdd` 输出 `ALL FEATURES OK`。

## 2026-11-01 — std.atomic / std.thread v1

- 背景：在**不改语言语法**的前提下补齐 C++ atomics/thread 能力。高层 API 全部放 `libs/std/`，编译器只新增内部 `__cdd_atomic_*` 内建；v1 目标 Linux x86-64 + pthread，支持 1/2/4/8 字节标量/指针/float16/32/64/enum，不做 16 字节/聚合/mutex/condvar。
- 实现：
  - `libs/std/atomic.cdd`：`MemoryOrder*`、`Atomic<T>`（seq_cst 简写 + `_order` 分派）、per-order 自由函数族、显式 `<T, Success, Failure>` CAS、fence；RMW 对 bool/enum/float 在方法内 `compile_time.if` 守卫，非法调用 panic；`is_lock_free` 用 `compile_time.static_assert` 拒绝超尺寸。
  - `libs/std/thread.cdd`：pthread `Thread` 状态机（init/start/join/detach/joinable/get_id/error）、`hardware_concurrency`、`this_thread_*`、`thread_equal`；驱动自动 `-l pthread`，CMake 链接 `Threads::Threads`。
  - 编译器：`AtomicBuiltin` AST 内建 + sema/order 校验 + LLVM 整数 carrier codegen（bool i8、float/pointer/enum bitcast、自然对齐）；修复模板 `atomic T` 的 TypeVar 名称丢失、`TemplateInstantiator` 丢失 `isAtomic`、i64 字段 align 4、atomic bool i1。
  - 前端：`import std.atomic;` 允许关键字作为模块路径分量；显式模板实参的限定常量识别；namespace 方法调用按定义侧 `std_init` 名字固化，修复调用点 `init` 与定义 `std_init` 不一致。
- 测试：
  - `tests/e2e/test_atomic.cpp`：模板字段、bool i8、i64 align 8、内部 builtin 全操作/CAS/fence、指针/float/bool/enum、坏序/坏类型诊断。
  - `tests/e2e/test_std_atomic.cpp`：seq_cst、`_order`、per-order、显式 CAS、float/bool/enum/pointer、lock-free、`Atomic<int128>` 拒绝、RMW 运行时序。
  - `tests/e2e/test_std_thread.cpp`：start/join/参数、状态机/ID、重复 lifecycle、detach 完成轮询、hardware_concurrency、yield/sleep、TLS 每线程、4×10000 原子计数压力。
  - 顺带把 `tests/driver/test_module_visibility.cpp` 的固定临时目录改为 pid 唯一，修复 `ctest -j8` 的并行 flake。
- 文档：`docs/spec/stdlib.md` §18/§19（API/限制/线程生命周期）、`docs/spec/abi.md` 原子 ABI 行与自然对齐说明、`TODO.md` CON-02/03/05/06/09、STD-24/25、P2-01。
- 验收：全量 `ctest --test-dir build -j8` **1726/1726** 通过（2 个既有 `GTEST_SKIP`）；`examples/all_features.cdd` 第 28 节运行输出 `ALL FEATURES OK`；生成程序自动链接 pthread 实测退出 0。
- 评审收口（2026-11-02）：relaxed fence 不再发出非法 `fence monotonic`；`_order` 的 exchange/fetch 把 `Consume` 分派到 acquire；``atomic int128` 与模板 `atomic T` 的指针/enum/聚合/float/int128 以及 union/typedef 字段解析改为编译期拒绝（不再静默丢原子性或退化为普通访问）；新增失败 CAS 回写、1/2 字节原子、Consume、relaxed fence、未 start 的 join/detach、驱动 pthread 链接测试。
- 已知限制：显式 CAS 的跨模块 enum 常量模板实参 v1 用数值 0..5；bool/enum/float RMW 是运行时 panic；`Thread` 无析构/所有权转移；无 mutex/condvar、跨平台线程后端、16 字节原子。

## 2026-11-04 — lambda 语言缺口（PAR-11 扩展）

- 背景：补齐常见 C++ 风格 lambda 写法，修复嵌套 lambda 未捕获变量导致 codegen 崩溃、`[=]` 误拷贝全局变量等缺口；不扩大到有捕获闭包逃逸。
- 语法：`[&]` 全按引用；`[=, &x]` / `[&, x]` 默认模式 + 显式覆盖；`[x = expr]` / `[&x = expr]` 捕获初始化；`[this]` 显式捕获。文法见 `docs/spec/grammar.ebnf`。
- sema：捕获初始化在外层作用域分析、创建点求值一次；`byRef` 初始化要求左值；`[this]` 在非静态方法内捕获 `Class*`，body 裸字段脱糖为 `this->field`，`[=]`/`[&]` 自动按值捕获 `this`；未捕获外层自动局部（含嵌套 lambda、调用被调者）报 `variable 'x' is not captured by lambda`；全局/模块作用域与 `static`/`thread_local`/块级 `extern` 不进入闭包字段；返回类型在 body 遍历中惰性推断（覆盖 `if`/`switch`、可见 body 局部/`auto` 闭包，先无值后有值时逐个补报）。
- codegen：闭包构造抽成单一 helper，`LambdaExprAST` 与立即调用共用，捕获初始化只求值一次；按值数组捕获初始化走整聚合拷贝；文件作用域有捕获 lambda 由 sema 直接诊断，不再进入 codegen。
- 非目标：有捕获闭包作为函数返回、struct 字段、数组元素、普通函数参数仍不支持；无 `mutable`，按值捕获写入写闭包副本且不新增只读诊断。
- 文档：`docs/spec/grammar.ebnf`、`docs/spec/semantics.md` §7.5、`docs/superpowers/specs/2026-11-04-lambda-gaps-design.md`、`docs/superpowers/plans/2026-11-04-lambda-gaps.md`、`TODO.md`。
- 评审收口（2026-11-05）：终审发现的 void 捕获初始化崩溃、数组返回类型推断非法 IR、body 局部遮蔽静默误编译、Switch/Defer/compile_time/multi-var 捕获发现遗漏、嵌套 init-capture 过捕获、块级 static/enum 与字段优先级、捕获变量未标记 used（W3002）全部修复；`CompoundStmt` codegen 增加作用域压栈；文档同步。
- 验证：`cmake --build build -j$(nproc)` 成功；`ctest --test-dir build -j8` **1796/1796** 通过（当前仓库无 `GTEST_SKIP`；旧计划提到的 2 个 skip 已在 constexpr 容器收口时移除）；`examples/all_features.cdd` 输出 `ALL FEATURES OK`。

## 2026-11-07 — 历史身份硬改名（已由 2026-11-12 统一改名归一化）

> 注：以下条目是历史身份改名记录。2026-11-12 的统一改名已将产品身份、后缀、CLI、项目名、namespace、文档全部切换为 CDD；为满足“全仓库无旧身份”，本条目中的旧名/旧后缀已机械归一化，具体日期与当时旧值以 git 历史为准。

- 背景：语言正式命名 **CDD（C dot dot）**，源文件后缀统一 `.cdd`；对外标准库命名空间/模块保持 `std`，编译器内部 C++ 命名空间改为 `cdd`。
- 构建身份：CMake 项目 `CDD`，库目标 `cdd_lib`，可执行文件 `build/bin/cdd`，LLVM module `cdd`，安装目录 `share/cdd/libs`，环境变量/宏 `CDD_STD_DIR`、`CDD_INSTALL_*`，`compile_time.build.compiler` 返回 `"cdd"`。
- 源文件迁移：22 个标准库源文件统一为 `.cdd`；`examples/all_features.cdd`、`resources/main_min.cdd`、测试 fixture 同步改名；模块加载器、预置层、测试合成文件名和 CTest 输入全部改用 `.cdd`。
- 兼容：硬切换，不保留 `.smc` 别名；命令行 `.smc` 输入和 `.smc` 导入均报明确的迁移错误。
- 内部命名：C++ 内部命名空间、UTF-8 辅助命名空间、内部 builtin 与 helper 统一为 `cdd` / `cdd_utf8` / `__cdd_*` / `cdd.*`；快照环境变量统一为 `CDD_UPDATE_SNAPSHOTS`。
- 文档：新增根 `README.md`（英文主文 + 中文简介）；`docs/spec/` 全部标题统一为 `CDD 1.0`，路径/代码围栏/工具命令同步；历史 TODO/Progress/设计计划中的旧产品名机械归一化。
- 提交：`46803c1`（构建身份）、`0074103`（源文件后缀迁移与 `.smc` 拒绝）、`ca7f098`（内部命名）、`51ba350`（文档/README）、`c511006`（终审修正）。
- 验证：`cmake -S . -B build && cmake --build build -j$(nproc)` 成功；`ctest --test-dir build -j8` **1802/1802** 通过；`build/bin/cdd examples/all_features.cdd` 输出 `ALL FEATURES OK`；安装布局包含 `bin/cdd` 与 `share/cdd/libs/std/c.cdd`。
- 评审修复（2026-11-08）：命令行输入名为 `.smc` 的隐藏文件时改按后缀拒绝（`path::extension()` 对其返回空）；嵌套 `.smc` 导入诊断补上导入者路径与 specifier；裸文件名输入现在按当前目录搜索同级导入；恢复 static-member 测试 fixture 的原名；统一两个 spec 分册标题；模块后缀长度改为显式常量并清理过期注释。新增 3 个回归测试（隐藏 `.smc`、嵌套诊断上下文、裸文件名同级导入）。

## 2026-11-09 — module/namespace 职责分离 + struct/class record 文法统一

- 背景：冻结 1.0 的两项语法身份裁定：`module` 只负责文件/导入，`namespace` 只负责 API 名字；`struct`/`class` 保留但共用一套 record 文法。设计见 `docs/superpowers/specs/2026-11-09-module-namespace-record-design.md`，计划 `docs/superpowers/plans/2026-11-09-module-namespace-record.md`。
- module/namespace：
  - `module`/`import` 只接受点分路径；`module a::b`、`import a::b` 给出明确诊断并恢复，不级联。
  - `namespace` 声明与访问只使用 `::`；`namespace A.B` 诊断 `namespace names use '::'; '.' is reserved for module paths`；仓库内 3 处 `namespace A.B` 迁移为 `A::B`。
  - import 只控制可见性：模块根部 `export` 保持非限定访问；namespace 成员必须限定访问。`import std.vec;` 的 API 是 `std::Vec`，无 `Vec` 或 `std.vec::Vec`。
  - 新增 parser 拼写测试 3 项、module visibility 测试 3 项；`ModuleVisibilityTest` 新增 preload-import helper 以匹配驱动顺序。
- record：
  - `parseStructDecl`/`parseClassDecl` 合并为 `parseRecordDecl(bool isClass)`；两者共用访问段、字段、方法、运算符、静态成员、嵌套类型、`using`、匿名成员和 attached declarator 解析。
  - `struct` 默认 public，`class` 默认 private；`StructDeclAST` 新增 `hasAccessSections`；带方法/访问段的 struct 注册为 `ClassType`，纯数据 struct 仍为 `StructType`；`union` 模型不变。
  - 修复 struct + attached variable 丢方法/字段的问题；class 默认/前向声明与 `repr(C)` 规则保持。
  - 新增 parser 测试 4 项、module/driver 测试 4 项、sema 访问测试 6 项、e2e 11 项（访问段、仅访问段、attached variable、self 类型、elaborated struct、前向+定义、交叉继承、struct/class 布局一致、compile_time 死分支）。
- 文档：`docs/spec/grammar.ebnf`（module-name、record-decl/record-member/access-section）、`docs/spec/modules.md`（职责、拼写、DEC-02 根导出/namespace 限定）、`TODO.md`（PAR-05/PAR-22/TYP-07/TYP-08/MOD-04/05/11/12/13/DEC-02/17 + 快照）。
- 提交：`259a355`（module/namespace 拼写）、`754ab70`（record parser 统一）、`019057f`（record 语义路由/访问控制）。
- 终审补丁（review fix pass）：
  - **Critical**：`struct S` 的 elaborated 引用现在优先查 `ClassType`（不再被前向 `StructType` 占位遮蔽）；前向声明不再 codegen 空 LLVM 类型；`StructDeclAST::codegen` 的既有类型路径补发 attachedVars，且 class-like struct 优先用 `ClassType` 建布局。
  - **Important**：`collectImportSpecifiers` 不再把 `import a::b` 预加载成部分模块 `a`；`compile_time.if` 死分支用统一 `isClassLikeRecord` 毒化/移除 class-like struct；struct 解析前按方法/访问段预注册 self 类型占位；`struct`/`class` 交叉继承（含字段查找/访问级别）已支持。
  - **Deferred**：局部作用域内带方法的 record（`int32 main(){ struct S{...} s; }`）仍会崩溃；class 等价写法同样崩溃，属既有独立缺陷，不在本轮 record 文法范围内。
- 验证：`ctest --test-dir build -j8` **1829/1829** 通过；`build/bin/cdd examples/all_features.cdd -o /tmp/opencode/all_features` 编译成功（仅既有 W3001/W3002 警告），运行输出包含 `ALL FEATURES OK`，退出码 0。

## 2026-11-10 — compile_time / compile_only 设计草案

- 背景：用户要求删除 `constexpr`，改为 `compile_time`（编译期可求值、运行期可调用）+ `compile_only`（仅编译期、无运行期符号），并完整支持 `DfaTable` 这类定长数组/POD 聚合返回。
- 已确认决策：`compile_time` 变量隐式只读；`compile_time` 函数内直接调用 `compile_only`（不在 `if compile_time` 内）报错；聚合返回必须进第一版（选项 A）。
- 新增设计：`docs/superpowers/specs/2026-11-10-compile-time-compile-only-design.md`。
  - 语法：`compile_time` 函数/变量、`compile_only` 自由函数、`if compile_time` 双路径；`constexpr` 硬删除并给迁移诊断。
  - 语义：CT 上下文表、`compile_only` 调用链诊断、无运行期符号、资源上限。
  - 值模型：`CtValue::AGGREGATE` + `CtAggregate`，支持嵌套定长数组/POD record、lvalue 路径、递归物化。
  - 迁移/测试/实现阶段已列出。
- 当前状态：仅设计，未改代码；待用户批准后写实施计划。基线仍为 **1829/1829**。

## 2026-11-10 — compile_time / compile_only 实施计划

- 用户批准设计及 v1 边界：`compile_only` 返回值通过命名 `compile_time` 常量进入 parser 级数组维度/模板实参；直接调用不纳入第一版。
- 新增计划：`docs/superpowers/plans/2026-11-10-compile-time-compile-only.md`，拆为 8 个 TDD 任务：改名迁移、变量只读、`compile_only` 无符号、`if compile_time`、调用链/函数体限制、聚合值模型、聚合求值+DfaTable、文档/示例/全量验证。
- 执行方式：按用户既有约束使用 native（当前会话逐任务实现），不使用 per-task subagent。
- 当前仍未改代码；基线 **1829/1829**。

## 2026-11-10 — compile_time / compile_only 实现完成

- 语言层：`constexpr` 删除；`compile_time` 函数/变量、变量隐式只读、`compile_only` 自由函数、`if compile_time` 双路径。
- 求值器：`CtAggregate` 定长数组/POD record、聚合 lvalue 路径（字段/下标）、聚合参数/返回深拷贝、递归 LLVM 常量物化。
- 安全边界：`compile_only` 无 `define`/`declare`；运行期可达链、`compile_time` 函数直接调用、地址/值使用、运行期函数调用、可变全局、非法返回类型均诊断。
- 资源：depth 128、steps 1,000,000、aggregate elements 1,000,000。
- 迁移：`libs/std/*.cdd`、e2e/sema/driver 测试、`examples/all_features.cdd`、`docs/spec/{compile_time,grammar,keywords,semantics,stdlib,conversions}.md`。
- 新增 e2e：`tests/e2e/test_compile_only.cpp`（10 项），覆盖 DfaTable、IR 无 `build_dfa`、聚合深拷贝、step limit、调用链。
- 验证：全量 `ctest --test-dir build -j8` **1851/1851** 通过；`build/bin/cdd examples/all_features.cdd` 输出 `ALL FEATURES OK`，退出码 0。

## 2026-11-10 — 终审修复（compile_time / compile_only）

- 修复 Critical：
  - 拒绝 `compile_only` 模板/方法/仅声明；模板实例化不再泄漏运行期函数体。
  - 拒绝 `compile_only` 运算符重载，避免运行期调用静默生成 `unknown operator`。
  - `ctConvertValue` 做聚合逐字段/逐元素类型转换；`Pair { int32, float64 }` 初始化不再生成非法 IR；拒绝 `compile_time` 聚合中的指针/union/位域/柔性数组。
- 修复 Important：
  - `compile_time`/`compile_only` 上下文关键字只在下一个 token 确实是类型/存储类时生效；`compile_time` 作为普通变量仍可用。
  - 嵌套 `{{...}}` 聚合初始化支持。
  - 命名 `compile_time` 常量可作为 parser 级数组维度；VLA 仍拒绝。
  - `compile_time` + storage class 定向报错；位域/指针聚合变量诊断。
  - 全局初始化器运行期链显示 `global initializer`。
  - 去掉 `&compile_only` 重复地址诊断；step-limit 不再叠加泛化初始化错误。
- 新增 e2e：模板/方法/仅声明/运算符拒绝、聚合类型转换、嵌套初始化、上下文关键字兼容、延迟数组维度、storage class、位域变量、global initializer 链。
- 验证：全量 **1865/1865** 通过；示例 `ALL FEATURES OK`。

## 2026-11-11 — capturing-closure escape（按值返回）与模块类型导出复核

- 背景：README 将 capturing-closure escape 列为当前限制；用户同时反馈模块系统无法跨模块导出 struct/class/template。复核当前 HEAD：namespace 内 `struct`/`class`/`template`（含方法、静态方法、继承）已可跨模块使用；`std.mem` 的 `Allocator`/`Arena`/`Pool`、`std.vec`/`std.deque`/`std.hashmap`/`std.hashset` 导入用例可编译运行。模块部分不改实现，新增回归测试锁定。
- 设计/计划：`docs/superpowers/specs/2026-11-11-closure-escape-design.md`、`docs/superpowers/plans/2026-11-11-closure-escape.md`。用户裁决：按值捕获闭包允许返回；按引用捕获（含 `this`）返回编译期拒绝。
- sema：自由运行期函数支持 `auto` 返回类型推导；首个带值 `return` 定类型，数组 decay，后续 return 兼容，裸 return/无 return 为 `void`。签名在 body 分析后注册，因此必须先定义后调用；递归/互递归/后置 auto 调用不支持。方法、`compile_time`、`compile_only` 上的 `auto` 返回定向拒绝；无 body 原型拒绝。
- 闭包安全：`StructType` 增加 `closureHasByRefCapture`/`closureCapturesThis`；返回带引用捕获或 `this` 捕获的闭包（含通过 `auto` 变量间接返回）报 `capturing closure with by-reference captures cannot escape by return` / `capturing closure with a 'this' capture cannot escape by return`。按值闭包返回复制闭包 struct，调用方持有独立副本。
- 名义类型：每个 lambda 表达式生成独立匿名闭包类型（调用函数不同）；两个 return 分支必须返回同一个闭包变量，不能各自新建 lambda 字面量。
- 测试：`tests/e2e/test_lambda.cpp` 新增 22 项（auto 返回/诊断、引用逃逸、按值闭包返回、嵌套逃逸、悬垂返回、顺序/递归）；`tests/driver/test_module_loader.cpp` 新增 1 项 namespace 类型导出回归。模块回归覆盖 struct/class/template 方法、静态方法、继承及继承方法调用。
- 文档：`README.md`、`docs/spec/semantics.md`、`docs/spec/keywords.md`、`docs/spec/grammar.ebnf`、`TODO.md` 同步；`examples/all_features.cdd` 未改（避免影响既有 tour 输出）。
- 终审修复（Critical/Important）：
  - `auto` 返回分支此前绕过 PAR-08/TYP-11 悬垂检查；抽出 `checkDanglingReturnValue` 并在所有值返回、lambda/auto 推断前调用。新增复合字面量与 slice literal 负向测试。
  - 闭包逃逸标志按捕获类型递归传播；按值捕获的嵌套引用闭包不再漏检。新增嵌套引用闭包返回负向测试。
  - 类外静态方法定义在清除 `className` 前记录 `isMethodDecl`，`auto` 返回仍按方法拒绝。新增 OOC static 测试。
  - 递归 `auto` 函数在返回表达式已失败时不再追加 `<unknown>` 类型噪声；补顺序、递归、先裸返回后带值返回、方法原型诊断测试。
  - 模块回归加入 `d.basev()`，覆盖跨模块继承方法分派。
- 验证：全量 `ctest --test-dir build -j8` **1888/1888** 通过；`build/bin/cdd examples/all_features.cdd` 输出 `ALL FEATURES OK`；模块/闭包聚焦测试通过。

## 2026-11-12 — CDD 身份统一改名

- 背景：用户要求语言身份全面改为 **CDD（C dot dot）**，源文件后缀改为 **`.cdd`**，并覆盖代码、构建、CLI、标准库、测试、示例和文档。设计/计划：`docs/superpowers/specs/2026-11-12-cdd-identity-design.md`、`docs/superpowers/plans/2026-11-12-cdd-rename.md`。
- 构建身份：CMake `project(CDD)`，库目标 `cdd_lib`，可执行文件 `build/bin/cdd`，安装目录 `share/cdd/libs`，安装匹配 `*.cdd`；`vcpkg.json` 包名/描述改为 `cdd`/CDD。
- CLI/模块：`Usage: cdd ...`、cxxopts 程序名/描述、标准库缺失提示、`CDD_STD_DIR`/`CDD_INSTALL_*`、模块加载器 `kCddExtension = ".cdd"`、预置层 `std/c.cdd` 全部同步。
- 源文件：30 个含旧身份的 tracked 路径用 `git mv` 改名，包括 22 个 `libs/std/*`、`examples/all_features.cdd`、`resources/main_min.cdd`、4 个测试 fixture、2 个历史改名设计/计划；内容中旧身份/旧后缀统一替换。
- 内部命名：C++ namespace `cdd`、`cdd_utf8`、`cdd.*` runtime helper、`__cdd_*` builtin、LLVM module/调试名、`compile_time.build.compiler = "cdd"` 全部同步。
- 文档：README 标题/首段/Hello/Why/布局/命名说明改为 CDD；`docs/spec/*` 标题和正文统一；历史 2026-11-07 改名条目及设计/计划加注“已归一化”；工作区目录 `My_LLVM_C` 与公开 `std` namespace 保持不变。
- 兼容性：不保留旧身份/旧后缀别名；标准库模块发现只使用 `.cdd`。直接编译输入的扩展名仍不参与语言语义。
- 验证：`cmake -S . -B build` 成功；`cmake --build build -j$(nproc)` 成功；全量 `ctest --test-dir build -j8` **1888/1888** 通过；`build/bin/cdd examples/all_features.cdd` 输出 `ALL FEATURES OK`；`build/bin/cdd --help` 显示 `Usage: cdd ...`；旧身份在 tracked 文本/路径/旧后缀中均无命中。
