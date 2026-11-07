# 语法遗漏收口 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 11 项语法/类型特性 + 修复「未知类型名声明静默丢弃」缺陷 + 同步滞后文档。

**Architecture:** 全部特性在既有 Parser/Lexer/Sema/Codegen 四层内实现，不引入新子系统（反射的 sema→codegen 交接经进程级注册表，见 Task 6）。每特性 TDD RED→GREEN，独立提交。

**Tech Stack:** C++17、LLVM（>= 18）、GTest/CTest（`build/bin/compiler_tests` 全量 `ctest`）。

**Spec:** `docs/superpowers/specs/2026-10-18-syntax-gaps-design.md`（本文档是计划的权威来源；执行者两者都读）

## Global Constraints

- 项目约定：commit 直接在 master（Native inline 执行，前几轮已确立）；测试二进制经 `file(GLOB_RECURSE TEST_SOURCES ...)` 自动收集到 `tests/CMakeLists.txt`，新增测试文件后需重新 configure。
- 构建：`cmake --build build -j$(nproc)`；全量验证：`cd build && ctest -j8`。基线 1373/1373（2 个既有 `GTEST_SKIP`：ConstexprFunctionE2E.ConstexprForLoop / ConstexprNestedCall）。
- TDD：每任务先写失败测试（RED）→ 实现（GREEN）→ 提交；commit message 引用 TODO ID（如 `feat(LEX-16): ...`）。
- 语言源码内嵌字符串中类型名用定宽写法（int32/float64 等，无 `int`/`float`/`double`）。
- 任务开始/结束按 AGENTS.md 更新 `TODO.md`/`Progress.md`（开始读、结束写）。
- CLI 探测：`./build/bin/cmm -S file.c`（`-S` 输出到 stdout）；`-fsyntax-only` 已知不跑 sema，探测用 `-S`。
- 诊断格式：`file:line:col: error[E####]: msg`；新 E 码须注册进 `src/sema/Diagnostic.cpp` 注册表。

## Review Focus

1. **未知类型名不再静默**：`f16 x;`/`char8 c;`/`nosuchtype int32 y;` 报 `unknown type`，且**不双报**——模板实参解析失败（`Foo<int, bar>`）只报一次、名字取声明起始 token。→ Task 5 测试。
2. **默认参数不破坏既有重载**：`f(int32)`+`f(int32,int32=5)` 实参 `f(1)`/`f(1,2)` 各选正确候选，`f(1,2,3)` 报无匹配；并列时偏好少默认参数者。区间判定不引入歧义误报。→ Task 4 测试。
3. **atomic 限定符不污染全局类型**：`atomic int32 a;` 后 `int32 b;` 是普通类型；`atomic int32* p` 报错；`atomic` 的 `* / % << >>` 复合赋值报编译期拒绝。→ Task 3 测试。
4. **反射返回值在运行时可用**：`compile_time.fields_of(S)` 返回 `Slice<FieldInfo>`，元素 `.name`（string_view）/`.offset`/`.size` 可读，数组生命周期覆盖读取点（全局常量）。→ Task 6 测试。
5. **protected 放宽不放开类外访问**：派生类方法内经 `this->` 访问基类 protected 字段/方法放行；类外对象访问 protected 仍 E2009。→ Task 7 测试。
6. **多行字符串与转义互不干扰**：`"""a\nb"""` 内转义照常解码；普通字符串行为不变。→ Task 1 测试。
7. **char8/16/32 与 thread_local/atomic 关键字不再静默吞声明**：三关键字注册后 `char8 c;` 是真 uint8 变量（有符号/可赋值），`thread_local int32 x;` 是 threadlocal 全局（LLVM IR `thread_local`）。→ Task 2/3 测试。

---

### Task 1: 词法层 — 多行字符串 + Unicode 标识符（LEX-16/LEX-18）

**Files:**
- Modify: `src/frontend/Lexer.h`（新增 `peekNext2()`、多行字符串标记）
- Modify: `src/frontend/Lexer.cpp`（scanToken 起始判定、scanString 三分支、scanIdentifier）
- Modify: `src/frontend/Token.h` / `Token.cpp`（`bool isMultilineString` 标记）
- Modify: `src/frontend/Parser.cpp`（字符串字面量 3 引号剥离，:556/:730/:745 附近）
- Test: `tests/frontend/test_lexer.cpp`、`tests/e2e/test_string_lib.cpp`（或新 `tests/e2e/test_multiline.cpp`）

**Interfaces:**
- Produces: `Token::isMultilineString`（多行字符串 token 置 true）；`Lexer::peekNext2()`；lexer 对 `r"` / `"""` / `"` 三分支识别。

- [ ] **Step 1: 写失败测试（lexer）**

在 `tests/frontend/test_lexer.cpp` 新增 `LexerMultilineStringTest`：
```cpp
TEST_F(LexerTest, MultilineStringLexesSingleToken) {
    // source: int32 main() { string_view s = """a
    // b"""; return 0; }
    // 断言：TOKEN_STRING 恰一个，lexeme 以 """ 开头，isMultilineString == true
}
TEST_F(LexerTest, MultilineStringUnterminatedReportsE0002) {
    // source: str s = """abc   （无闭合）
    // 断言：E0002 unterminated string literal
}
TEST_F(LexerTest, UnicodeIdentifierAccepted) {
    // source: int32 变量 = 42;
    // 断言：TOKEN_IDENTIFIER 名含 UTF-8 字节（"变量"）
}
```

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='LexerMultilineStringTest.*:LexerTest.UnicodeIdentifierAccepted'`
Expected: 多行字符串相关失败（`"""` 被切成 3 个 STRING/非法字符）；Unicode 标识符失败（`变量` 报非法字符/被切成多 token）。

- [ ] **Step 3: 实现多行字符串词法**

`Lexer.h` 加 `char peekNext2() const;`（返回 `m_source[m_pos+2]`，越界返回 `'\0'`）。
`scanToken()`：起始判定改为——`ch=='"' && peekNext()=='"' && peekNext2()=='"'` → 调 `scanMultilineString()`；`ch=='r' && peekNext()=='"'` → scanString（原始，既有）；`ch=='"'` → scanString（普通）。
新增 `scanMultilineString()`：消耗 `"""`，扫描到下一个连续 `"""` 终止（复用 scanString 的 EOF 守卫报 E0002）；`makeToken(TOKEN_STRING, lexeme())` 后置 `token.isMultilineString = true`。
`Token` 加 `bool isMultilineString{}`。

- [ ] **Step 4: 实现 Unicode 标识符词法**

`scanToken()`：起始判定 `isalpha(ch) or ch=='_' or (ch & 0x80)!=0` → scanIdentifier。
`scanIdentifier()`：续字符判定 `isalnum(ch) or ch=='_' or (ch & 0x80)!=0`（原 `isalnum` 保留 ASCII）。
不做 NFC/码点校验（记文档注释）。

- [ ] **Step 5: parser 3 引号剥离**

Parser 三处字符串字面量消费点（:556、:730、:745）统一处理：若 token `isMultilineString`，剥离**前 3 后 3** 字符再 `decodeEscapes`；否则剥离前 1 后 1（现状）。可加 helper `std::string stripStringQuotes(const Token&)`。

- [ ] **Step 6: 跑测试确认 GREEN**

Run: 聚焦 lexer 测试 + 既有 `LexerTest.*` 全绿
Expected: 新增 3 项 + 既有 lexer 全部 PASS。

- [ ] **Step 7: 写 e2e（多行字符串运行时行为）**

`tests/e2e/test_multiline.cpp`：
```cpp
TEST(MultilineE2E, MultiLineContentPreserved) {
    // string_view s = """hello
    // world"""; s.len() == 11（含换行）
}
TEST(MultilineE2E, EscapesStillDecoded) {
    // string_view s = """a\tb"""; s[1] == '\t'
}
TEST(MultilineE2E, UnicodeIdentifierRuns) {
    // int32 数据 = 7; int32 main() { return 数据 - 7; }  → 0
}
```
（e2e fixture 沿用 `tests/e2e/test_std_math.cpp` 的 `runWithImports` 或更简的 `runSource`——查既有 e2e 通用 fixture，沿用其形态）

- [ ] **Step 8: 跑 e2e + 全量**

Run: `cd build && ctest -j8`
Expected: 全部通过（基线 1373 + 新增）。

- [ ] **Step 9: Commit**

```bash
git add src/frontend/Lexer.h src/frontend/Lexer.cpp src/frontend/Token.h src/frontend/Token.cpp src/frontend/Parser.cpp tests/frontend/test_lexer.cpp tests/e2e/test_multiline.cpp
git commit -m "feat(LEX-16/18): multiline strings + Unicode identifiers"
```

---

### Task 2: 类型关键字 — char8/16/32 + thread_local 存储类（TYP-05/CON-04）

**Files:**
- Modify: `src/frontend/Token.h` / `Lexer.cpp`（3 token + thread_local token）
- Modify: `src/frontend/Parser.cpp`（parseBaseType 映射 char8/16/32；parseDeclarationImpl 存储类分支 thread_local）
- Modify: `src/ast/Type.h` / `Type.cpp`（若 thread_local 需要标志——方案：复用 VarDeclAST 侧标志，Type 不加）
- Modify: `src/ast/Decl.h` / `Decl.cpp`（VarDeclAST 加 `bool isThreadLocal`；codegen 全局/局部 thread_local）
- Modify: `src/sema/SemanticAnalyzer.cpp`（thread_local 约束检查：仅变量；E 码注册）
- Modify: `src/support/Utils.cpp`（token 名表）
- Test: `tests/frontend/test_lexer.cpp`、`tests/frontend/test_parser.cpp`、`tests/e2e/test_types.cpp`（或新文件）

**Interfaces:**
- Consumes: Token 扩展（Task 1 同层）。
- Produces: `VarDeclAST::isThreadLocal`；`char8→getUInt8()/char16→getUInt16()/char32→getUInt32()` 类型映射；`SemInvalidThreadLocalOnFunction` E 码。

- [ ] **Step 1: 写失败测试**

`tests/frontend/test_lexer.cpp`：
```cpp
TEST_F(LexerTest, Char8Char16Char32Keywords) { /* TOKEN_CHAR8/16/32 */ }
TEST_F(LexerTest, ThreadLocalKeyword) { /* TOKEN_THREAD_LOCAL */ }
```
`tests/frontend/test_parser.cpp`：
```cpp
TEST_F(ParserTest, Char8DeclaresUInt8) { /* char8 c; → getUInt8() */ }
```
`tests/e2e/test_types.cpp`（或新 `test_storage.cpp`）：
```cpp
TEST(StorageE2E, ThreadLocalGlobalIsThreadLocalInIR) {
    // 编译含 thread_local int32 x = 5; 的源，断言 IR 含 thread_local 标记
    // （e2e 若只执行运行时不看 IR，则改为运行时行为：主线程读写 x）
}
TEST(StorageE2E, Char8Assignable) { /* char8 c = 65; return c == 65 ? 0 : 1; */ }
TEST(StorageE2E, ThreadLocalOnFunctionRejected) {
    // thread_local int32 f(); → sema 错误含 "thread_local"
}
```

- [ ] **Step 2: 运行确认 RED**

Run: 聚焦以上测试
Expected: 关键字测试失败（token 未注册→解析错位）；thread_local 声明仍被静默吞或报错；char8 声明被静默吞。

- [ ] **Step 3: 注册 token**

`Token.h` 加 `TOKEN_CHAR8/TOKEN_CHAR16/TOKEN_CHAR32/TOKEN_THREAD_LOCAL`；`Lexer.cpp` keywordMap 加 `{"char8",...}` 等 4 项；`src/support/Utils.cpp` token 名表加 4 项。

- [ ] **Step 4: parseBaseType 映射 char8/16/32**

`Parser::parseBaseType` 三个新 case → `TypeContext::getUInt8()/getUInt16()/getUInt32()`（别名，非独立类型——文档决策）。

- [ ] **Step 5: parseDeclarationImpl thread_local 分支**

在 `parseDeclarationImpl`（:2402）开头 extern 分支旁加：
```cpp
if (check(TokenType::TOKEN_THREAD_LOCAL)) {
    advance();
    auto decl = parseDeclaration();
    if (auto* var = dynamic_cast<VarDeclAST*>(decl.get())) {
        var->isThreadLocal = true;
        return decl;
    }
    if (dynamic_cast<FunctionDeclAST*>(decl.get())) {
        emitError(DiagnosticCode::SemInvalidThreadLocalOnFunction,
                  "thread_local cannot be applied to a function", ...);
        return nullptr;
    }
    return decl;
}
```
（若 parseDeclaration 返回的是 MultiVarDeclAST，需遍历子变量逐个置位——查 `MultiVarDeclAST` 形态后处理。）

- [ ] **Step 6: VarDeclAST + codegen thread_local**

`Decl.h` `VarDeclAST` 加 `bool isThreadLocal{};`。
`Decl.cpp` 全局变量 codegen：`var->isThreadLocal` 时 `llvm::GlobalVariable` 构造后 `gv->setThreadLocal(true)`（其余初始化逻辑复用）。函数内 static 局部 thread_local：查既有 static 局部路径，镜像处理（若不存在 static 局部，本轮仅全局；记文档）。

- [ ] **Step 7: sema 约束**

`sema` 变量声明处允许 `isThreadLocal`（顶层/命名空间）；确认局部非 static thread_local 的处理（拒绝或忽略——记文档，倾向拒绝：局部 thread_local 无 static 语义易混淆）。注册 `SemInvalidThreadLocalOnFunction` → E 码（查既有注册表选未用号）。

- [ ] **Step 8: 跑测试确认 GREEN + 全量**

Run: `cd build && ctest -j8`
Expected: 全绿。

- [ ] **Step 9: Commit**

```bash
git add src/frontend/Token.h src/frontend/Lexer.cpp src/frontend/Parser.cpp src/ast/Decl.h src/ast/Decl.cpp src/sema/SemanticAnalyzer.cpp src/sema/Diagnostic.cpp src/support/Utils.cpp tests/frontend/test_lexer.cpp tests/frontend/test_parser.cpp tests/e2e/test_types.cpp
git commit -m "feat(TYP-05/CON-04): char8/16/32 types + thread_local storage class"
```

---

### Task 3: atomic 类型限定符（CON-01）

**Files:**
- Modify: `src/frontend/Token.h` / `Lexer.cpp`（TOKEN_ATOMIC）
- Modify: `src/frontend/Parser.cpp`（parseType 限定符前缀循环；克隆类型）
- Modify: `src/ast/Type.h` / `Type.cpp`（`bool isAtomic` + 克隆 helper）
- Modify: `src/ast/Symbol.cpp`（typesEqual/mangle 纳入 isAtomic）
- Modify: `src/ast/Expr.cpp`（load/store 原子化、atomicrmw）
- Modify: `src/sema/SemanticAnalyzer.cpp`（atomic 约束：仅整型标量、复合赋值操作集；E 码）
- Test: `tests/frontend/test_parser.cpp`、`tests/sema/`、`tests/e2e/test_atomic.cpp`

**Interfaces:**
- Consumes: Token 扩展。
- Produces: `Type::isAtomic`；`Type* cloneWithAtomic(Type*)`；atomic 类型在 `typesEqual`/mangle 中区分；`SemAtomicNonScalar`/`SemAtomicBadRMWOp` E 码。

- [ ] **Step 1: 写失败测试**

`tests/frontend/test_lexer.cpp`：`TEST_F(LexerTest, AtomicKeyword)`。
`tests/frontend/test_parser.cpp`：
```cpp
TEST_F(ParserTest, AtomicQualifierClonesType) {
    // atomic int32 → 类型 isAtomic==true 且 != getInt32()（新对象）
    // 随后 int32 → 仍 getInt32()（未被污染，isAtomic==false）
}
```
`tests/e2e/test_atomic.cpp`：
```cpp
TEST(AtomicE2E, AtomicLoadStoreWorks) {
    // atomic int32 a = 0; a = a + 1; 两次，return a == 2 ? 0 : 1
}
TEST(AtomicE2E, AtomicIncrement) {
    // atomic int32 c = 0; c++; ++c; return c == 2 ? 0 : 1
}
TEST(AtomicE2E, AtomicPointerRejected) {
    // atomic int32* p; → sema 错误
}
TEST(AtomicE2E, AtomicMultiplyRejected) {
    // atomic int32 a = 2; a *= 3; → 编译期拒绝（cmpxchg 未实现）
}
```

- [ ] **Step 2: 运行确认 RED**

Expected: lexer/parser 失败；e2e 中 `atomic` 被静默吞（`a` 未声明）或解析错位。

- [ ] **Step 3: 注册 token**

`TOKEN_ATOMIC` + keywordMap + Utils token 名表。

- [ ] **Step 4: parseType 限定符克隆**

`parseType`（:2252）限定符循环（现 const/volatile）加 `atomic`。关键：**克隆**——`atomic int32` 时 `baseType = new Type(baseType->kind, baseType->base)` 副本并置 `isAtomic=true`，不触碰共享单例 `getInt32()`。（`cloneAtomicQualified(Type*)` helper：浅拷贝 Type 标量层；数组/指针元素上的 atomic 本轮拒绝——仅在基础类型层接受。）

- [ ] **Step 5: Type::isAtomic + 相等/mangle**

`Type` 加 `bool isAtomic{};`。`typesEqual`（Symbol.cpp）纳入 `isAtomic`（`atomic int32` ≠ `int32`）。mangle（Mangle.cpp）对 `isAtomic` 追加后缀（如 `_atomic`），保证符号隔离。

- [ ] **Step 6: sema 约束**

变量声明：类型 `isAtomic` 时校验——基础类型须为整型标量（`isIntegerType` 且非数组/指针/slice/聚合/浮点）；否则 `SemAtomicNonScalar`。复合赋值/`++`/`--` 左值 atomic 时校验操作符 ∈ {`+= -= &= |= ^=`}（及 `++/--`），`* / % << >>` 报 `SemAtomicBadRMWOp`。

- [ ] **Step 7: codegen 原子访问**

`Expr.cpp`：对 `isAtomic` 左值的 load/store 用 `LoadInst/StoreInst` 设 `setAtomic(AtomicOrdering::SequentiallyConsistent)`；复合赋值/`++`/`--` 用 `BinaryOperator::CreateAtomicRMW`（Add/Sub/And/Or/Xor，`xchg` 用于 `=`？——`=` 是普通 store 原子化即可）。赋值 `a = expr` 走原子 store。读 `a` 走原子 load。

- [ ] **Step 8: 跑测试 GREEN + 全量**

Run: `cd build && ctest -j8`
Expected: 全绿（既有 atomic 相关无——确认无冲突）。

- [ ] **Step 9: Commit**

```bash
git add src/frontend/Token.h src/frontend/Lexer.cpp src/frontend/Parser.cpp src/ast/Type.h src/ast/Symbol.cpp src/ast/Mangle.cpp src/ast/Expr.cpp src/sema/SemanticAnalyzer.cpp src/sema/Diagnostic.cpp src/support/Utils.cpp tests/frontend/test_lexer.cpp tests/frontend/test_parser.cpp tests/e2e/test_atomic.cpp
git commit -m "feat(CON-01): atomic type qualifier (integer scalars, atomicrmw subset)"
```

---

### Task 4: 默认参数（FUN-09）

**Files:**
- Modify: `src/ast/Decl.h`（`ParamDeclAST` 加 defaultValue；`FunctionDeclAST`/方法加 defaultArgCount）
- Modify: `src/frontend/Parser.cpp`（parseParamDecl 的 `=` 分支）
- Modify: `src/ast/Symbol.h` / `Symbol.cpp`（`Symbol::decl` 回指针；`OverloadSet::resolve` 区间化 + 少默认优先）
- Modify: `src/sema/SemanticAnalyzer.cpp`（checkFunctionCall 区间化、resolveMethod 区间化、默认值折叠、CallExprAST 暂存）
- Modify: `src/ast/Expr.h` / `Expr.cpp`（`CallExprAST::defaultArgValues`；codegen 补参）
- Test: `tests/frontend/test_parser.cpp`、`tests/sema/test_semantic_analyzer.cpp`、`tests/e2e/test_default_args.cpp`

**Interfaces:**
- Consumes: `OverloadSet::resolve(argTypes)` 现状（Symbol.cpp:286）。
- Produces: `Symbol::decl`（可空 `FunctionDeclAST*`）；`FunctionDeclAST::defaultArgCount`；`ParamDeclAST::hasDefault` + `defaultValue`（ExprAST）；`CallExprAST::defaultArgValues`（vector<ExprAST*> 折叠后的常量）；`OverloadSet::resolve` 签名不变但语义区间化。

- [ ] **Step 1: 写失败测试**

`tests/frontend/test_parser.cpp`：
```cpp
TEST_F(ParserTest, DefaultArgParses) {
    // int32 f(int32 x = 5, int32 y); → parse 错误（中部默认值拒绝）
    // int32 f(int32 x, int32 y = 5); → 合法，params[1].hasDefault
}
```
`tests/sema/test_semantic_analyzer.cpp`：
```cpp
TEST(SemanticAnalyzerTest, DefaultArgMiddleRejected) { /* 中部默认值 E 码 */ }
```
`tests/e2e/test_default_args.cpp`：
```cpp
TEST(DefaultArgsE2E, FillsDefault) {
    // int32 f(int32 x, int32 y = 10) { return x + y; }
    // main: f(5) == 15 && f(5, 2) == 7
}
TEST(DefaultArgsE2E, OverloadPrefersFewerDefaults) {
    // f(int32) vs f(int32, int32=5)：f(1) → f(int32)
    // f(int32, int32) vs f(int32, int32=5)：f(1,2) → 两参版（少默认优先）
}
TEST(DefaultArgsE2E, TooManyArgsRejected) {
    // f(int32, int32=5)：f(1,2,3) → no matching function
}
TEST(DefaultArgsE2E, DefaultValueMustBeConstexpr) {
    // int32 g(int32 x) { return x; }  int32 f(int32 a = g(1)); → 拒绝
}
```

- [ ] **Step 2: 运行确认 RED**

Expected: parser 对 `=` 报 `expected ')'`；sema/e2e 全部失败。

- [ ] **Step 3: ParamDeclAST + parser**

`Decl.h` `ParamDeclAST` 加 `std::unique_ptr<ExprAST> defaultValue; bool hasDefault{};`。
`Parser::parseParamDecl`：参数类型+名后遇 `TOKEN_ASSIGN` → `parseExpr()` 存 defaultValue、置 hasDefault。约束：**仅尾随参数可有默认值**——sema 层校验（见 Step 5），parser 只解析。

- [ ] **Step 4: Symbol::decl 回指针 + OverloadSet 区间化**

`Symbol` 加 `FunctionDeclAST* decl{};`（或 void* 避免头依赖——用前向声明 `class FunctionDeclAST;` + `FunctionDeclAST* decl;`）。
`Scope::declare` 无 FunctionDeclAST 上下文（:146-155）——**默认值不从 declare 走**：sema 在 `visit(FunctionDeclAST)` 声明函数时另行登记（见 Step 5）。故 `Symbol::decl` 由 sema 在函数声明完成后设置。
`OverloadSet::resolve`（Symbol.cpp:286）：
```cpp
// 每个候选：
//   minArgs = paramCount - defaultArgCountOf(sym->decl)
//   viable iff minArgs <= argTypes.size() <= paramCount（vararg：>= minArgs）
//   参数转换循环 i < min(argTypes.size(), paramCount) 防越界
// 并列：偏好 defaultArgCount 更小者（少默认优先）；仍并列 → ambiguous
```
`defaultArgCountOf`：经 `sym->decl`（null 时 0）读取；若 decl 为空但函数有默认值（如方法），sema 须保证 decl 已设置。

- [ ] **Step 5: sema — checkFunctionCall + resolveMethod 区间化 + 默认值折叠**

`checkFunctionCall`（:709）：
- `:734` 与 `:763` 的 `!=` 硬等号改区间判定（`minArgs <= args.size() <= paramCount`，vararg 为 `>= minArgs`）。
- `:738` 转换循环 `i < fixedCount` → `i < min(argTypes.size(), fixedCount)`。
- `:763` 的报错分支仅当 `args.size() > paramCount`（超参）或 `< minArgs`（不足）触发。
- resolve 成功后：`args.size() < paramCount` 时，把缺的默认值**折叠为常量**暂存到 CallExprAST（见 Expr.h Step 6）。
`resolveMethod`（:5377）：同样区间化 `paramTypes.size()-1` 两侧（`minArgs` 同样经方法 decl 的默认数），转换循环按实参个数限界；阶段 1（精确）与阶段 2（decay）都要处理。
`visit(FunctionDeclAST)`：函数声明完成后对每个有默认值的参数登记 `Symbol::decl` 回指针 + `FunctionDeclAST::defaultArgCount`；方法经 `ClassType::methods` 存 `MethodDeclAST*` 回指针（Type.h:269 方法表扩展，或并行 map）。
默认值 constexpr 校验：`defaultValue` 经 `CompileTimeEvaluator` 求值为常量（复用 CT-06 通路）；非常量报错（新 E 码 `SemDefaultArgNotConstexpr`）。

- [ ] **Step 6: Expr — CallExprAST 暂存 + codegen 补参**

`Expr.h` `CallExprAST` 加 `std::vector<ConstValue> defaultArgValues;`（或 `std::vector<ExprAST*> defaultedExprs`——采用折叠后的常量更稳：`std::vector<long long> defaultIntValues;` 等——具体形态按 CT-06 ConstValue 载体定，计划按「折叠后暂存」）。
`Expr.cpp` `CallExprAST::codegen`：`resolvedParamTypes.size() > args.size()` 时，对缺位用 `defaultArgValues[i]` 生成常量实参（按形参类型 `ConstantInt/ConstantFP`）。

- [ ] **Step 7: 方法默认参数**

`resolveMethod` 的默认值折叠路径：调用点经 `MethodCallExprAST` 同样暂存默认值；codegen 补参（镜像 CallExprAST）。若本轮方法默认值风险过高（方法表无 decl 回指针），可在 Step 5 记文档「方法默认参数不支持」并让 parser 在方法参数列表拒绝 `=`——**倾向：方法支持**（用户要求全做），按上述回指针方案。

- [ ] **Step 8: 跑测试 GREEN + 全量**

Run: `cd build && ctest -j8`
Expected: 全绿（既有重载测试不回归——重点核对 OverloadSet 既有用例）。

- [ ] **Step 9: Commit**

```bash
git add src/ast/Decl.h src/frontend/Parser.cpp src/ast/Symbol.h src/ast/Symbol.cpp src/sema/SemanticAnalyzer.cpp src/sema/Diagnostic.cpp src/ast/Expr.h src/ast/Expr.cpp tests/frontend/test_parser.cpp tests/sema/test_semantic_analyzer.cpp tests/e2e/test_default_args.cpp
git commit -m "feat(FUN-09): default arguments with overload interval resolution"
```

---

### Task 5: 未知类型名声明静默丢弃修复

**Files:**
- Modify: `src/frontend/Parser.cpp`（parseDeclarationAsType 未知类型诊断 + 双报防护）
- Modify: `src/sema/Diagnostic.cpp`（`SemUnknownType` E 码）
- Test: `tests/frontend/test_parser.cpp`、`tests/e2e/test_diagnostics.cpp`（或既有诊断快照设施）

**Interfaces:**
- Consumes: `parseDeclarationAsType`（:2667）。
- Produces: `SemUnknownType` E 码；parser 对未知类型名声明报错（不静默）。

- [ ] **Step 1: 写失败测试**

`tests/frontend/test_parser.cpp`：
```cpp
TEST_F(ParserTest, UnknownTypeDeclDiagnosed) {
    // nosuchtype int32 y = 5; → 报错，消息含 "unknown type" 与 "nosuchtype"
}
TEST_F(ParserTest, UnknownTypeNotDoubleReported) {
    // template<typename T> struct Box { T v; };
    // Box<int32, nosuchtype> b; → 模板实参失败只报一次（不额外报 unknown type nosuchtype）
}
```
诊断快照：`tests/diagnostics/snapshots/unknown_type_decl.txt`（若设施支持自动生成，用 `CMM_UPDATE_SNAPSHOTS=1`）。

- [ ] **Step 2: 运行确认 RED**

Expected: `nosuchtype int32 y;` 现编译通过（静默吞）→ 测试失败；模板用例现行为待观察（记录基线）。

- [ ] **Step 3: parseDeclarationAsType 诊断**

`parseDeclarationAsType`（:2667）：`parseType()` 返回 nullptr 时，若当前 token 是 `TOKEN_IDENTIFIER`（或类型起始关键字）→ 记录**本次声明尝试起始 token**（进入函数时的 `peek()`），报 `SemUnknownType`：`unknown type 'NAME'`（NAME = 起始 token lexeme）。非标识符起始的失败维持现状（其他诊断已报）。
**双报防护**：进入 `parseDeclarationAsType` 前记录 `getErrors().size()`（或 parser 侧诊断计数），仅当本次尝试未新增诊断才报 unknown type——模板实参失败（已有诊断）不再补报。

- [ ] **Step 4: 注册 E 码**

`Diagnostic.cpp` 注册 `SemUnknownType` → 未用 E 码（查注册表）。

- [ ] **Step 5: 跑测试 GREEN + 全量**

Run: `cd build && ctest -j8`
Expected: 全绿——重点：既有合法程序零回归（合法程序不会以未知类型名开头）；模板既有诊断测试不双报。

- [ ] **Step 6: Commit**

```bash
git add src/frontend/Parser.cpp src/sema/Diagnostic.cpp tests/frontend/test_parser.cpp tests/e2e/test_diagnostics.cpp tests/diagnostics/snapshots/unknown_type_decl.txt
git commit -m "fix: diagnose unknown-type declarations instead of silently dropping (E####)"
```

---

### Task 6: compile_time 反射 — name_of / fields_of / attributes_of（CT-07/13）

**Files:**
- Create: `src/sema/ReflectionRegistry.h` / `ReflectionRegistry.cpp`（sema→codegen 交接）
- Modify: `src/sema/SemanticAnalyzer.cpp` / `.h`（compile_time 钩子特判 name_of/fields_of/attributes_of；FieldInfo/Attribute 内建类型）
- Modify: `src/frontend/Parser.cpp`（若需要内建类型名特判：`FieldInfo`/`Attribute` 仿 string_view 特判）
- Modify: `src/ast/Expr.h` / `Expr.cpp`（`ReflectionExprAST` 专用节点 + codegen 物化全局数组）
- Modify: `src/ast/Type.h` / `Type.cpp`（FieldInfo/Attribute 聚合类型构造，或复用 StructType）
- Modify: `src/codegen/CodegenContext.cpp`（注册表查询、全局数组生成）
- Test: `tests/e2e/test_reflection.cpp`、`tests/sema/`

**Interfaces:**
- Consumes: `CompileTimeEvaluator::evalLayoutQuery` 模式（CompileTimeEvaluator.cpp:488）；`getStrType()`/`getSliceType()`（TypeContext）；`LayoutBuilder::buildAggregate`（字段偏移）。
- Produces: `ReflectionRegistry`（进程级单例：`registerName(Type*, string)` / `registerFields(Type*, vector<FieldInfoData>)` / `registerAttributes(DeclAST*, vector<AttributeData>)` + 查询）；`ReflectionExprAST{ kind, Type* targetType, string key }`；`FieldInfo`/`Attribute` 内建类型。

- [ ] **Step 1: 写失败测试**

`tests/e2e/test_reflection.cpp`：
```cpp
TEST(ReflectionE2E, NameOfReturnsTypeName) {
    // struct S { int32 a; };  compile_time.name_of(S) == "S"（string_view 比较）
}
TEST(ReflectionE2E, FieldsOfReturnsFieldTable) {
    // struct S { int32 a; float64 b; };
    // auto fs = compile_time.fields_of(S);   ← 无 auto！语言无 auto
    // 用显式类型：Slice<FieldInfo> fs = compile_time.fields_of(S);
    // fs.len == 2; fs[0].name == "a"; fs[0].offset == 0; fs[1].name == "b"
}
TEST(ReflectionE2E, AttributesOfReturnsAnnotations) {
    // [[deprecated("old")]] struct S { int32 a; };
    // Slice<Attribute> attrs = compile_time.attributes_of(S);
    // attrs.len == 1; attrs[0].name == "deprecated"
}
TEST(ReflectionE2E, FieldsOfNonAggregateRejected) {
    // compile_time.fields_of(int32) → 错误
}
```
（`Slice<FieldInfo>` 显式声明语法——查 Slice 类型写法 `Slice<T>` 是否可用；若 parser 的 slice 语法是 `T[]` 后缀，用 `FieldInfo[] fs = ...`。计划按 **`FieldInfo[]`（slice 后缀）** 为准，执行时以既有 slice 声明语法为准。）

- [ ] **Step 2: 运行确认 RED**

Expected: `compile_time.name_of` 报 unknown member / undeclared；`FieldInfo` 类型未定义。

- [ ] **Step 3: 内建类型 FieldInfo/Attribute**

Parser `parseType` 标识符特判（仿 string_view :2137）加 `"FieldInfo"` → 构造 StructType（字段 `string_view name; usize offset; usize size;`）注册 TypeContext；`"Attribute"` → StructType（`string_view name;`）。sema 侧同样识别。

- [ ] **Step 4: ReflectionRegistry**

`ReflectionRegistry.h`：进程级单例（或挂在 TranslationUnitAST）。API：
```cpp
struct FieldInfoData { std::string name; size_t offset; size_t size; };
struct AttributeData { std::string name; };
class ReflectionRegistry {
  static ReflectionRegistry& instance();
  void registerFields(const std::string& key, std::vector<FieldInfoData>);
  const std::vector<FieldInfoData>* getFields(const std::string& key) const;
  void registerName(const std::string& key, std::string name);
  std::string getName(const std::string& key) const;
  void registerAttributes(const std::string& key, std::vector<AttributeData>);
  const std::vector<AttributeData>* getAttributes(const std::string& key) const;
};
```
key 约定：`typeToMangled(targetType)`（mangle 名，稳定）。

- [ ] **Step 5: sema — compile_time 钩子 + 数据登记**

`SemanticAnalyzer` 的 compile_time 成员处理（仿 `evalLayoutQuery`）：识别 `name_of`/`fields_of`/`attributes_of`。
- 实参 = 类型（SizeofExprAST 包装或标识符，仿 evalLayoutQuery 的 queriedType 解析）。
- `name_of`：`typeToString(targetType)` 登记到 registry（key = mangled），返回 `string_view` 类型（node.type = getStrType()）。
- `fields_of`：仅 struct/class/union——`LayoutBuilder::buildAggregate` 拿偏移，字段名/偏移/大小填入 FieldInfoData 登记；返回 `Slice<FieldInfo>`（`getSliceType(fieldInfoType)`）。非聚合 → 错误（新 E 码 `SemReflectNonAggregate`）。
- `attributes_of`：目标声明（targetType 对应的 DeclAST——需从类型反查声明，或把注解登记在类型注册时）的注解列表登记；返回 `Slice<Attribute>`。
- 表达式节点：构造 `ReflectionExprAST{ kind, targetType, key }` 挂到调用点 AST。

- [ ] **Step 6: codegen — ReflectionExprAST 物化全局数组**

`Expr.cpp` `ReflectionExprAST::codegen`：
- `name_of`：查 registry 拿字符串 → 生成全局字符串常量 + `{ptr, len}` slice（string_view 布局）。
- `fields_of`/`attributes_of`：查 registry 拿数据 → 生成全局 `[N x <FieldInfo/Attribute>]` 常量数组（`ConstantStruct`/`ConstantArray`，字符串字段内联）→ `{ptr(数组), len}` slice。
- 数组全局链接可见、不优化掉（`linkonce_odr` 或 `internal` + 引用），生命周期 = 程序全程（Review Focus 4）。

- [ ] **Step 7: 跑测试 GREEN + 全量**

Run: `cd build && ctest -j8`
Expected: 全绿。

- [ ] **Step 8: Commit**

```bash
git add src/sema/ReflectionRegistry.h src/sema/ReflectionRegistry.cpp src/sema/SemanticAnalyzer.cpp src/sema/SemanticAnalyzer.h src/frontend/Parser.cpp src/ast/Expr.h src/ast/Expr.cpp src/ast/Type.h src/ast/Type.cpp src/codegen/CodegenContext.cpp src/sema/Diagnostic.cpp tests/e2e/test_reflection.cpp
git commit -m "feat(CT-07/13): compile_time reflection — name_of/fields_of/attributes_of"
```

---

### Task 7: protected 派生类访问放宽（INH-06）

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（4 处检查点判定式放宽：checkStaticMemberAccess :202、字段 :3021、方法 :3119、指定初始化器 :3600）
- Test: `tests/sema/test_semantic_analyzer.cpp`、`tests/e2e/test_inheritance.cpp`（或既有 INH e2e 文件追加）

**Interfaces:**
- Consumes: `isDerivedFrom`（:5354，沿 base 链）。
- Produces: 判定式 `currentClass == definingClass || isDerivedFrom(currentClass, definingClass)`。

- [ ] **Step 1: 写失败测试**

`tests/sema/test_semantic_analyzer.cpp`：
```cpp
TEST(SemanticAnalyzerTest, ProtectedBaseFieldAccessibleFromDerivedMethod) {
    // class B { protected: int32 x; };
    // class D : public B { int32 get() { return this->x; } };  → 无错误
}
TEST(SemanticAnalyzerTest, ProtectedBaseMethodAccessibleFromDerivedMethod) {
    // class B { protected: int32 f() { return 1; } };
    // class D : public B { int32 g() { return this->f(); } };  → 无错误
}
TEST(SemanticAnalyzerTest, ProtectedStillPrivateOutside) {
    // class B { protected: int32 x; };  main: B b; b.x; → E2009 仍报
}
TEST(SemanticAnalyzerTest, PrivateBaseMemberStillRejected) {
    // class B { private: int32 x; };  class D : public B { int32 get() { return this->x; } }; → 仍 E2009
}
```

- [ ] **Step 2: 运行确认 RED**

Expected: 前两项现报 E2009（失败）；后两项现通过（基线确认）。

- [ ] **Step 3: 判定式放宽（4 处）**

`SemanticAnalyzer.cpp` 4 处 `currentClass != definingClass` 类判定（或等价谓词）改为：
```cpp
auto protectedOk = [&]() {
    return currentClass == definingClass ||
           (currentClass && isDerivedFrom(currentClass, definingClass));
};
```
（`currentClass`/`definingClass` 的精确变量名按各处代码核对；`isDerivedFrom(derived, base)` 沿 `baseClass` 链。）

- [ ] **Step 4: 跑测试 GREEN + 全量**

Run: `cd build && ctest -j8`
Expected: 全绿（既有 INH-06 pin 测试若有"派生访问 protected 被拒"的旧断言需更新为新语义——核对后修改）。

- [ ] **Step 5: Commit**

```bash
git add src/sema/SemanticAnalyzer.cpp tests/sema/test_semantic_analyzer.cpp tests/e2e/test_inheritance.cpp
git commit -m "feat(INH-06): allow derived-class access to protected base members"
```

---

### Task 8: 表达式字面量 — alignof/offsetof 独立关键字 + nullptr（PAR-14/TYP-24）

**Files:**
- Modify: `src/frontend/Lexer.cpp`（`{"nullptr", TOKEN_NULL}`）
- Modify: `src/frontend/Parser.cpp`（prefix-expr 的 alignof、primary 的 offsetof 分支）
- Modify: `src/sema/SemanticAnalyzer.cpp` / `CompileTimeEvaluator.cpp`（alignof/offsetof 走布局查询；必要时）
- Test: `tests/frontend/test_parser.cpp`、`tests/e2e/test_expression.cpp`（或新 `test_expr_literals.cpp`）

**Interfaces:**
- Consumes: `TOKEN_ALIGNOF/TOKEN_OFFSETOF`（token 已在，lexer 已识别）；`TOKEN_NULL`（既有）；`CompileTimeEvaluator::evalLayoutQuery`（size_of/align_of/offset_of）。
- Produces: `alignof(T)`→usize（常量折叠）；`offsetof(T, member)`→usize；`nullptr` ≡ `null`。

- [ ] **Step 1: 写失败测试**

`tests/frontend/test_parser.cpp`：
```cpp
TEST_F(ParserTest, AlignofKeywordExpr) {
    // int32 main() { return alignof(int32); }  → parse 成功
}
TEST_F(ParserTest, OffsetofKeywordExpr) {
    // struct S { int32 a; int32 b; };
    // int32 main() { return offsetof(S, b); }  → parse 成功
}
```
`tests/e2e/test_expr_literals.cpp`：
```cpp
TEST(ExprLiteralsE2E, AlignofMatchesCompileTime) {
    // alignof(int32) == compile_time.align_of(int32) → 0
}
TEST(ExprLiteralsE2E, OffsetofSecondField) {
    // struct S { int32 a; int32 b; }; offsetof(S, b) == 4 → 0
}
TEST(ExprLiteralsE2E, NullptrIsNullPointerConstant) {
    // int32* p = nullptr; return p == null ? 0 : 1
}
TEST(ExprLiteralsE2E, NullptrInBoolContext) {
    // int32* p = nullptr; return !p ? 0 : 1
}
```

- [ ] **Step 2: 运行确认 RED**

Expected: `alignof`/`offsetof` 现报 `unexpected token`；`nullptr` 现报 `undeclared identifier`。

- [ ] **Step 3: nullptr → TOKEN_NULL**

`Lexer.cpp` keywordMap 加 `{"nullptr", TokenType::TOKEN_NULL}`。parser 无改动（`null` 已有完整空指针字面量路径）。

- [ ] **Step 4: alignof 前缀表达式**

`Parser` 前缀表达式分支（unary-expr，`sizeof` 旁）加 `TOKEN_ALIGNOF`：`alignof '(' type ')'` → 构造 SizeofExprAST 包装类型（复用现有 sizeof 类型实参 AST 形态），sema 映射到布局查询 align_of。

- [ ] **Step 5: offsetof 主表达式**

`Parser` primary-expr 加 `TOKEN_OFFSETOF`：`offsetof '(' type ',' identifier ')'` → 构造带 `{Type*, string fieldName}` 的表达式节点（可复用或新增小节点），sema 映射到 `compile_time.offset_of(type, "field")` 布局查询。

- [ ] **Step 6: sema 接线**

在 `visit`（offsetof 节点）与 alignof（SizeofExprAST 语义已有通路）接布局查询：结果 `usize` 常量折叠（`node.type = getUSize()`；`node.ctInt` 置偏移/对齐值——查 CT-12 常量集成位）。

- [ ] **Step 7: 跑测试 GREEN + 全量**

Run: `cd build && ctest -j8`
Expected: 全绿。

- [ ] **Step 8: Commit**

```bash
git add src/frontend/Lexer.cpp src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp src/sema/CompileTimeEvaluator.cpp tests/frontend/test_parser.cpp tests/e2e/test_expr_literals.cpp
git commit -m "feat(PAR-14/TYP-24): alignof/offsetof keywords + nullptr literal"
```

---

### Task 9: 文档同步（grammar.ebnf / keywords.md / conversions.md / compile_time.md / semantics.md / TODO.md / Progress.md）

**Files:**
- Modify: `docs/spec/grammar.ebnf`、`docs/spec/keywords.md`、`docs/spec/conversions.md`、`docs/spec/compile_time.md`、`docs/spec/semantics.md`、`docs/spec/abi.md`（如涉 thread_local/atomic 布局）
- Modify: `TODO.md`、`Progress.md`

**Interfaces:**
- Consumes: 各 Task 完成后状态。

- [ ] **Step 1: grammar.ebnf 同步**

- §1：multiline-string `[plan]`→`[impl]`；identifier Unicode 范围 `[plan]`→`[impl]`；`'str'`→`'string_view'`（primitive-type）。
- §3：storage-class 加 `'thread_local'` `[impl]`；parameter 加 `[ '=' const-expr ]` `[impl]`（默认参数）。
- §4：type 加 `'atomic'` 限定符 `[impl]`；primitive-type 加 `'char8'|'char16'|'char32'`。
- §5/§6：prefix-expr 加 `'alignof' '(' type ')'`、primary-expr 加 `'offsetof' '(' type ',' identifier ')'`；`'nullptr'` 并入 null-literal。
- §8：reflection-function 补 `'name_of' | 'fields_of' | 'attributes_of'`。
- 各 `[plan]` 状态按实际实现置 `[impl]`。

- [ ] **Step 2: keywords.md 同步**

- `f16/f32/f64/f128` 类型关键字：若 Task 2/3 未加（本轮 char8/16/32 + atomic + thread_local 已加），保持 `[plan]`（f16 类型关键字**不在本轮**——文档记「仅字面量后缀，类型关键字挂 TYP-04」）。
- `thread_local` → `[impl]`；`atomic` → `[impl]`（限定符，范围记 CON-01 最小）。
- `char8/char16/char32` → 新增行 `[impl]`。
- `nullptr` → `[impl]`；`alignof/offsetof` → `[impl]`。
- 修正：`str`/`String` 行（str 已改名 string_view）；裸 `return;` 行；原始字符串已实现。
- `template/typename/this/static_cast/reinterpret_cast` → `[impl]`（前几轮已落地，本表滞后——顺带修正）。

- [ ] **Step 3: conversions.md / compile_time.md / semantics.md / abi.md**

- conversions.md：`nullptr` `[plan]`→`[impl]`（§4/§7 处）；atomic/thread_local 简述（若 §2 有表）。
- compile_time.md：反射函数表补 name_of/fields_of/attributes_of（含 FieldInfo/Attribute 返回类型）。
- semantics.md：重载解析节补默认参数区间规则与少默认优先；访问控制节补 protected 派生放宽。
- abi.md：若 thread_local/atomic 需一句（大小/对齐不变；atomic 仅整型标量）。

- [ ] **Step 4: TODO.md 勾记**

- `LEX-16` → `[x]`（多行字符串+转义全集，含 Unicode 标识符 LEX-18 → `[x]`）。
- `PAR-14` → `[x]`（alignof/offsetof 独立关键字）；`TYP-24` → `[~]`（nullptr 完成，余 NULL 宏）或 `[x]`。
- `TYP-05` → `[~]`（char8/16/32 类型完成，UTF 字面量族未做）或 `[x]`。
- `CON-01` → `[x]`（atomic 限定符最小）；`CON-04` → `[x]`（thread_local 语法+基础存储）。
- `FUN-09` → `[x]`（默认参数）。
- `CT-07/CT-13` → `[x]`（反射全做）。
- `INH-06` → 更新（protected 放宽完成）。
- 新增/更新进度快照行。

- [ ] **Step 5: Progress.md 追加**

按 AGENTS.md 格式追加本轮记录：背景/目标/各 Task 完成情况/验证（全量测试数）/遗留。

- [ ] **Step 6: 全量验证 + Commit**

Run: `cd build && ctest -j8`（确认文档改动无测试影响）
```bash
git add docs/spec/grammar.ebnf docs/spec/keywords.md docs/spec/conversions.md docs/spec/compile_time.md docs/spec/semantics.md docs/spec/abi.md TODO.md Progress.md
git commit -m "docs: syntax-gaps completion — grammar/keywords/conversions/compile_time/semantics + TODO/Progress"
```
