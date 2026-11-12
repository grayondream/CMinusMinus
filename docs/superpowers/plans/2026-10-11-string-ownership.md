# string 所有权/拷贝语义（I4 收口）+ string_view Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `string` 成为 C++ `std::string` 式拥有型值（拷贝即独立、新鲜值即移动），新增 `string_view`（原 `str` 硬改名），从根上消除 I4「值拷贝别名双 free」。

**Architecture:** 编译器为 `string` 类型特判三处值语义挂接（赋值/初始化/传参返回），合成 `stringCopy`/`stringAssign` 惰性 LLVM helper；「新鲜值」按表达式 kind 判定（rvalue 调用→移动，其余→深拷贝，默认安全）。`str`→`string_view` 是 Parser 类型名 + 全库机械迁移（TypeKind::Str 内部名不变）。

**Tech Stack:** LLVM 21（IRBuilder/惰性 helper 合成）、gtest/ctest、LLJIT e2e（runSource 骨架）。

**Spec:** `docs/superpowers/specs/2026-10-11-string-ownership-design.md`（已提交 529ae6a，用户已批 D2 拷贝模型 B / D3 硬改名 / D7 赋值自动释放）

## Global Constraints

- 拷贝/移动语义**严格限于 `string` 类型**，不推广到一般聚合（spec D5）。
- NG-04：不引入一般所有权/生命周期模型；`string_view` 悬垂仍是用户责任；无 use-after-move 诊断（spec §3.4）。
- 默认安全：无法判定 → **拷贝**（spec §4）。
- `str` 硬改名 `string_view`，`str` 成为未知标识符（spec D3）；`str_from_c` 函数名保留。
- 全量基线 1212 绿（2 个既有 GTEST_SKIP 不变）；每任务 TDD RED→GREEN；全量 `ctest --test-dir build` 绿才提交；一任务一提交。
- e2e 用 runSource 骨架（tests/e2e/test_string.cpp:36 模式，LLJIT 执行返回 main 值）。

## Review Focus

- 输入 `string b = a;` 后 `a` 继续使用、双双 `destroy()` —— 必须独立（Task 2 赋值 / Task 3 初始化钉住）。
- 输入 `return x.s;`（struct 字段 string 返回）—— 必须**拷贝**，不得移动字段损坏 `x`（Task 4 钉住）。
- 输入自赋值 `b = b;` —— 不得 free 旧缓冲（Task 2 钉住）。
- 输入三元 `cond ? a : b`（含活变量）—— 拷贝，`a`/`b` 保持有效（Task 3/4 钉住）。
- 输入 `f(string.new("x"))`（临时实参，形参 string）—— 移动，不泄漏（Task 4 钉住）。
- 输入聚合（struct 含 string 字段）赋值覆盖旧缓冲 —— 字段感知 free+copy（Task 5 钉住）。
- 输入 `str` 作类型名 —— 未知标识符诊断，不静默（Task 1 钉住）。

---

### Task 1: `str` → `string_view` 硬改名（D3）

**Files:**
- Modify: `src/frontend/Parser.cpp`（`isTypeStart`/`parseBaseType` 类型名特判，:1631 与 :1914 附近，`"str"` → `"string_view"`）
- Modify: `src/sema/SemanticAnalyzer.cpp:1903,1910`（`"[]str value"` → `"[]string_view value"`）
- Modify: `libs/std/string.cdd`（`str` 类型用法 → `string_view`，约 12 处：contains/starts_with/ends_with/trim*/utf8_sub 签名与局部变量）
- Modify: `tests/`（全库 `str` 类型用法 → `string_view`；`src/` 内 `TypeKind::Str`/`StrType`/`getStrType` 内部名不动）
- Test: `tests/frontend/test_parser.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Produces: 源码层类型名 `string_view`（`{ptr,len}`，内部仍 `TypeKind::Str`/`getStrType()`，签名不变）；`str` 成为未知标识符。
- Consumes: 既有 Parser 类型名识别机制（P1-06 小写化同路径）、DEC-18 先例。

- [x] **Step 1: 写失败测试**（tests/frontend/test_parser.cpp 追加）：`string_view v = "hi";` 解析为 str 类型表达式；`str v2;` 报未知类型诊断（用既有未知类型机制，错误码取现有类型名诊断）。
- [x] **Step 2: 跑测试确认 RED**。Run: `ctest --test-dir build -R "ParserTest|LexerTest" --output-on-failure`。Expected: 新测试失败；且全库既有 `str` 类型用法全部失败（未知类型）。
- [x] **Step 3: Parser 改类型名**：`"str"` → `"string_view"`（:1631、:1914）；sema 文案 `[]str` → `[]string_view`（:1903、:1910）。
- [x] **Step 4: 迁移**：`libs/std/string.cdd` 与 `tests/` 全部 `str` 类型用法改 `string_view`（仅类型名；注释/字符串里的 `str` 字样不动）。Run: `grep -rn "\bstr\b" tests libs --include="*.cpp" --include="*.cdd" | grep -v string_view` 逐个核对残留。
- [x] **Step 5: 全量回归**。Run: `cmake --build build -j12 && ctest --test-dir build -j8`。Expected: 100% tests passed（1212 基线 + 新测试，2 个 GTEST_SKIP 不变）。
- [x] **Step 6: 提交**。`git add src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp libs/std/string.cdd tests/` → `git commit -m "refactor(parser): rename str type to string_view (DEC-18)"`

### Task 2: string 拷贝/赋值 helper + 赋值语义（D7）

**Files:**
- Modify: `src/codegen/CodegenContext.h` / `src/codegen/CodegenContext.cpp`（惰性合成 `getStringCopyFn`、`getStringAssignFn`，仿 `getStrFindFn`/`declareLibcFn` 模式）
- Modify: `src/ast/Expr.cpp`（`AssignmentExprAST::codegen`，:1334 区域——string LHS 分支）
- Create: `tests/e2e/test_string_ownership.cpp`（runSource 骨架仿 test_string.cpp:36）

**Interfaces:**
- Produces:
  - `llvm::Function* CodegenContext::getStringCopyFn()`：`void({ptr,len,cap}* dst, {ptr,len,cap} src)` — `cap = max(len,1)`；`malloc(cap)` + `memcpy`；写 dst 三字段。
  - `llvm::Function* CodegenContext::getStringAssignFn()`：同签名 — 自赋值守卫（`dst.ptr == src.ptr` 则仅拷贝跳过 free）；否则 `dst.ptr != null` 时 `free(dst.ptr)`；再拷贝。
  - 判定 helper：`static bool isFreshStringValue(ExprAST&)`（rvalue 且为 CallExprAST/MethodCallExprAST 且类型 string → true），放 `Expr.cpp` 匿名命名空间，Task 3/4 复用。
- Consumes: `ctx.getLLVMType(StringType)`、既有 `declareLibcFn`（Expr.cpp:2024）、`builtinObjectAddr`/`builtinStrArg` 模式。

- [x] **Step 1: 写失败 e2e**（tests/e2e/test_string_ownership.cpp）：`AssignCopyIndependent`——`string a = string.new("y"); string b = string.new("x"); b = a; a.destroy(); b.destroy(); return 0;` Expected 0（当前浅拷贝 → abort 双 free）。
- [x] **Step 2: 跑测试确认 RED**。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: `AssignCopyIndependent` abort（free double free）。
- [x] **Step 3: 实现 helper**：`getStringCopyFn`/`getStringAssignFn` 惰性合成于 CodegenContext.cpp（声明加进 .h private 区）。
- [x] **Step 4: 赋值挂接**：`AssignmentExprAST::codegen` 中，LHS 类型为 string（剥 Typedef）且 `op == Assign` 时：RHS 为新鲜值 → `free 旧缓冲（守卫）+ CreateStore(result, lhsVal)`；否则 → `CreateCall(getStringAssignFn(), {lhsVal, result})` 替代 :1334 的裸 store。
- [x] **Step 5: 补钉测试**：`SelfAssignSafe`（`b = b;` 不崩）、`AssignFreesOldBuffer`（`b = a;` 后 `b` 值正确）。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: 3/3 PASS。
- [x] **Step 6: 全量回归**。Run: `cmake --build build -j12 && ctest --test-dir build -j8`。Expected: 100% passed。
- [x] **Step 7: 提交**。`git add src/codegen/CodegenContext.h src/codegen/CodegenContext.cpp src/ast/Expr.cpp tests/e2e/test_string_ownership.cpp` → `git commit -m "feat(codegen): string deep-copy assignment with auto-free (I4/D7)"`

### Task 3: 初始化拷贝/移动（fresh 值检测，D2）

**Files:**
- Modify: `src/ast/Decl.cpp`（`VarDeclAST::codegen` init 路径，:346-364 区域）
- Test: `tests/e2e/test_string_ownership.cpp`

**Interfaces:**
- Consumes: Task 2 的 `getStringCopyFn`、`isFreshStringValue`。
- Produces: 初始化规则——源为新鲜值（`string.new(...)`/函数调用 rvalue）→ 直接 store（移动，零拷贝）；源为其他（变量/字段/三元等）→ `stringCopy(dst, srcVal)` 后 store 拷贝。

- [x] **Step 1: 写失败 e2e**：`InitCopyIndependent`——`string s = string.new("x"); string t = s; s.destroy(); t.destroy(); return 0;`（I4 路径 1 的初始化形态；当前浅拷贝 → 双 free abort）。
- [x] **Step 2: 跑测试确认 RED**。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: `InitCopyIndependent` abort。
- [x] **Step 3: 实现**：`VarDeclAST::codegen` 非 initList 分支，当类型为 string 且非 constexpr 时：`isFreshStringValue(*initExpr)` → 直接 store `initVal`；否则先 `CreateCall(getStringCopyFn(), {alloca, initVal})` 再跳过裸 store（或等价：copy 后不 store）。
- [x] **Step 4: 补钉测试**：`InitMoveFromNew`（`string s = string.new("x"); s.destroy(); return 0;` 行为不变且不崩）、`InitFromCall`（`string s = std::concat(a, b);` 值正确）。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: 全 PASS。
- [x] **Step 5: 全量回归**。Run: `cmake --build build -j12 && ctest --test-dir build -j8`。Expected: 100% passed。
- [x] **Step 6: 提交**。`git add src/ast/Decl.cpp tests/e2e/test_string_ownership.cpp` → `git commit -m "feat(codegen): string init deep-copies lvalues, moves fresh values (D2)"`

### Task 4: 传参/返回 拷贝/移动（D2/D6）

**Files:**
- Modify: `src/ast/Expr.cpp`（`CallExprAST::codegen`/`MethodCallExprAST::codegen` 实参组装，:1260-1282 区域）
- Modify: `src/ast/Stmt.cpp`（`ReturnStmtAST::codegen`，:51-82 区域）
- Test: `tests/e2e/test_string_ownership.cpp`

**Interfaces:**
- Consumes: Task 2 的 `getStringCopyFn`、`isFreshStringValue`。
- Produces: 实参规则——形参 `string`：实参新鲜值 → 直接传（移动）；实参活值 → 先 `stringCopy` 到临时再传（深拷贝）。形参 `string_view`：`string` 实参投影借用（`builtinStrArg` 模式，:2005，零拷贝）。返回规则——返回表达式为新鲜调用 → 直接 `CreateRet(v)`；为**局部变量标识符**（sema 查作用域为函数局部/形参）→ 移动（load v 后把源存储清零再 `CreateRet(v)`）；其余（字段访问/三元/全局）→ 先 `stringCopy` 到临时再返回。

- [x] **Step 1: 写失败 e2e**：`PassByValueIndependent`——`void kill(string s) { s.destroy(); } int32 main() { string a = string.new("data"); kill(a); a.destroy(); return 0; }`（I4 路径 2；当前 callee destroy 释放共享缓冲 → 调用方 a 双 free abort）。
- [x] **Step 2: 跑测试确认 RED**。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: `PassByValueIndependent` abort。
- [x] **Step 3: 实参挂接**：实参组装处，当 `resolvedParamTypes[i+1]` 为 string 且实参非新鲜值时，`tmp = alloca; stringCopy(tmp, argVal); argVal = load(tmp)`；形参为 string_view 时投影（复用 `builtinStrArg` 逻辑）。
- [x] **Step 4: 返回挂接**：`ReturnStmtAST::codegen` 在 `CreateRet(v)` 前，当返回类型为 string 时按规则处理（局部标识符 → load 后清零源存储；其余 → stringCopy 临时）。
- [x] **Step 5: 补钉测试**：`ReturnMovesLocal`（`string f() { string s = string.new("x"); return s; }` 返回值正确、双 destroy 安全）、`ReturnFieldCopies`（`return x.s;` 字段 string 返回——拷贝，`x` 仍可 destroy）、`ConditionalReturnCopiesOperands`（`return cond ? a : b;` 后 `a`/`b` 仍可 destroy）、`TempArgMove`（`f(string.new("x"))` 形参 string 值正确）。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: 全 PASS。
- [x] **Step 6: 全量回归**。Run: `cmake --build build -j12 && ctest --test-dir build -j8`。Expected: 100% passed。
- [x] **Step 7: 提交**。`git add src/ast/Expr.cpp src/ast/Stmt.cpp tests/e2e/test_string_ownership.cpp` → `git commit -m "feat(codegen): string by-value args deep-copy, returns move locals (D2/D6)"`

### Task 5: 聚合字段感知拷贝（spec §3.1/§5）

**Files:**
- Modify: `src/codegen/CodegenContext.h` / `.cpp`（聚合拷贝 helper：递归，遇 string 字段用 `getStringCopyFn`/`getStringAssignFn`）
- Modify: `src/ast/Decl.cpp`（`emitAggregateInitializer`，:47 区域）
- Modify: `src/ast/Expr.cpp`（`AssignmentExprAST::codegen` 聚合赋值分支）
- Test: `tests/e2e/test_string_ownership.cpp`

**Interfaces:**
- Consumes: Task 2 的 `getStringCopyFn`/`getStringAssignFn`。
- Produces: `emitAggregateStringCopy(dst, srcStructVal, StructType*)`（字段感知：string 字段 `stringCopy`、其余整段拷贝）——Task 5 内部用，覆盖聚合初始化/赋值/传参/返回路径。

- [x] **Step 1: 写失败 e2e**：`StructFieldCopyIndependent`——`struct S { string s; }; ... struct S y = x; x.s.destroy(); y.s.destroy(); return 0;`（I4 路径 3；当前字段浅拷贝 → 双 free abort）。
- [x] **Step 2: 跑测试确认 RED**。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: `StructFieldCopyIndependent` abort。
- [x] **Step 3: 实现聚合拷贝**：字段感知 helper（string 字段深拷贝，其余字段原样）；挂接聚合初始化（brace init 含 string 字段时，对 string 字段走 copy）与聚合赋值（struct 赋值含 string 字段时，逐字段 copy，含旧缓冲 free 守卫）。
- [x] **Step 4: 补钉测试**：`StructWithStringAssignFreesOld`（聚合赋值覆盖旧 string 字段不泄漏、值正确）。Run: `ctest --test-dir build -R "StringOwnership" --output-on-failure`。Expected: 全 PASS。
- [x] **Step 5: 全量回归**。Run: `cmake --build build -j12 && ctest --test-dir build -j8`。Expected: 100% passed。
- [x] **Step 6: 提交**。`git add src/codegen/CodegenContext.h src/codegen/CodegenContext.cpp src/ast/Decl.cpp src/ast/Expr.cpp tests/e2e/test_string_ownership.cpp` → `git commit -m "feat(codegen): field-aware aggregate copies for string members (I4)"`

### Task 6: 文档收口 + 全量回归

**Files:**
- Modify: `docs/superpowers/specs/2026-10-08-str-string-design.md`（§1.2「无隐式堆分配」→ 标注 string 例外，指新 spec）
- Modify: `docs/spec/stdlib.md`（§9 str→string_view、所有权行更新）
- Modify: `docs/spec/semantics.md`（NG-04 旁注：string 单类型特例，不构成所有权模型）
- Modify: `TODO.md`（P1-09 挂账「终审 I4 仍在账」→ 已收口；进度快照）、`Progress.md`（本轮记录）
- Test: 全量

- [x] **Step 1: 更新文档**（上述四处；措辞对齐 spec 决策表 D1-D7）。
- [x] **Step 2: 全量回归**。Run: `cmake --build build -j12 && ctest --test-dir build -j8`。Expected: 100% tests passed。
- [x] **Step 3: 提交**。`git add -f docs/superpowers/specs/2026-10-08-str-string-design.md docs/spec/stdlib.md docs/spec/semantics.md TODO.md Progress.md` → `git commit -m "docs: close I4, string ownership semantics and string_view rename"`
