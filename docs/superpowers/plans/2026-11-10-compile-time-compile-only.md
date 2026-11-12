# Compile-Time / Compile-Only Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace language-level `constexpr` with `compile_time` (CT-evaluable and runtime-callable) and add `compile_only` (CT-only, no runtime symbol), including fixed-size array/POD aggregate results such as `DfaTable`.

**Architecture:** Keep the existing shared `CompileTimeEvaluator`/`CtValue` pipeline, extend it with an `AGGREGATE` value kind and lvalue paths, and split CT-vs-runtime reachability at sema. `compile_time` functions keep normal LLVM emission; `compile_only` functions are skipped by codegen and are only interpreted. `if compile_time` selects the then branch in CT and emits only the else branch at runtime.

**Tech Stack:** C++17, LLVM 21, CMake, GoogleTest, LLJIT, existing `cdd_lib` parser/sema/codegen/test harness.

**Spec:** `docs/superpowers/specs/2026-11-10-compile-time-compile-only-design.md`

## Global Constraints

- Language-level `constexpr` is removed; do not add an alias or compatibility mode.
- `compile_time` and `compile_only` are contextual keywords, not reserved lexer keywords.
- `compile_time` variables are implicitly read-only; set the read-only property on the declaration/symbol, never mutate a shared `Type::isConst`.
- `compile_time` functions remain runtime-callable; `compile_only` functions must leave no `define` or `declare` in LLVM IR.
- A `compile_time` function that calls `compile_only` must put the call inside `if compile_time`; direct calls outside it are errors.
- `compile_only` first version supports free functions only; no methods, templates, lambdas, virtuals, function pointers, fields, or closures.
- `compile_only` return values must be `void` or fixed-size, trivially copyable CT values with no pointers, references, owning `string`, union, bitfield, flexible array, or base-class slot.
- `compile_only` parameters may be CT scalars/aggregates or read-only `string`/`string_view`; no pointer/reference parameters.
- `if compile_time` is allowed only inside `compile_time`/`compile_only` functions.
- Parser-level array dimensions and template arguments accept named `compile_time` constants, not direct `compile_only` calls.
- Preserve the current baseline: **1829/1829** tests pass before this plan.
- Build: `cmake --build build -j$(nproc)`. Test: `ctest --test-dir build -j8`. Example: `build/bin/cdd examples/all_features.cdd -o /tmp/opencode/all_features`.
- The new design/plan files are under `docs/*`, which is git-ignored; force-add them when committing with `git add -f docs/superpowers/...`.
- Do not use subagents for per-task implementation; the user requested native execution.

## Review Focus

- Runtime reachability: a runtime `main -> make_table -> build_dfa` path must never silently call a missing `compile_only` symbol; tests must assert a full-chain diagnostic.
- Aggregate value semantics: nested `t.next[s][c] = ...` must mutate the intended aggregate, while passing/returning a `DfaTable` must deep-copy; tests must cover aliasing.
- No runtime symbol: IR tests must assert both `getFunction("build_dfa") == nullptr` and no stray `declare`; address/function-pointer uses must error.
- `compile_time` read-only: mutations, `++`, out-params, and mutable pointers must be rejected without making a shared `DfaTable` type globally const for unrelated variables.
- Migration completeness: all language-level `constexpr` in stdlib/tests/examples is gone; `constexpr` input gives exactly the targeted migration error without cascades.

---

### Task 1: Rename `constexpr` to `compile_time` and migrate existing language uses

**Files:**
- Modify: `src/frontend/Lexer.cpp`, `src/frontend/Token.h`, `src/support/Utils.cpp`
- Modify: `src/frontend/Parser.h`, `src/frontend/Parser.cpp`
- Modify: `src/ast/Decl.h`, `src/ast/Decl.cpp`
- Modify: `src/sema/SemanticAnalyzer.h`, `src/sema/SemanticAnalyzer.cpp`
- Modify: `src/sema/CompileTimeEvaluator.h`, `src/sema/CompileTimeEvaluator.cpp`
- Modify: `src/ast/Symbol.h`
- Modify: `libs/std/core.cdd`, `libs/std/math.cdd`, `libs/std/thread.cdd`, `libs/std/time.cdd`
- Modify: `tests/frontend/test_parser.cpp`, `tests/sema/test_semantic_analyzer.cpp`
- Modify: `tests/e2e/test_const_constexpr.cpp`, `tests/e2e/test_constexpr_function.cpp`, `tests/e2e/test_constexpr_containers.cpp`, `tests/e2e/test_compile_time.cpp`
- Modify: `tests/e2e/resources/test_const.cdd`
- Test: `tests/frontend/test_parser.cpp` (new `CompileTimeDeclaration` and `ConstexprRemoved` tests)

**Interfaces:**
- Consumes: existing `parseDeclarationAsType`, `parseVariableDeclList`, `parseFunctionDecl`, `VarDeclAST`, `FunctionDeclAST`.
- Produces: `VarDeclAST::isCompileTime` and `FunctionDeclAST::isCompileTime`; constructors accept a trailing `bool compileTime = false`; `Parser::parseVariableDeclList(Type*, const std::string&, bool isCompileTime = false)`; `SemanticAnalyzer::compileTimeValues`/`compileTimeFunctions`; `SemanticAnalyzer::getCompileTimeValues()`; `Symbol::isCompileTime`.

- [ ] **Step 1: Write the failing parser tests**

Add to `tests/frontend/test_parser.cpp`:

```cpp
TEST_F(ParserTest, CompileTimeFunctionAndVariableParse) {
    auto tu = parse("compile_time int32 f(int32 x) { return x + 1; }\n"
                    "compile_time int32 N = f(41);\n");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 2u);
    auto* fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->isCompileTime);
    auto* var = dynamic_cast<VarDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(var, nullptr);
    EXPECT_TRUE(var->isCompileTime);
}

TEST_F(ParserTest, ConstexprRemovedWithTargetedDiagnostic) {
    auto errs = parseErrors("constexpr int32 N = 1;");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("constexpr"), std::string::npos);
    EXPECT_NE(errs[0].message.find("compile_time"), std::string::npos);
    EXPECT_NE(errs[0].message.find("compile_only"), std::string::npos);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='ParserTest.CompileTimeFunctionAndVariableParse:ParserTest.ConstexprRemovedWithTargetedDiagnostic'`
Expected: FAIL — `isCompileTime` does not exist, and `constexpr` is still accepted.

- [ ] **Step 3: Implement the rename and migration**

- Rename `VarDeclAST::isConstexpr` to `isCompileTime`; constructor parameter `compileTime_`.
- Rename `FunctionDeclAST::isConstexpr` to `isCompileTime`; constructor parameter `compileTime_`.
- Rename Parser `isConstexpr` parameters to `isCompileTime`; all call sites.
- In `Parser::parseDeclarationAsType`, replace the `TOKEN_CONSTEXPR` branch:
  - If `TOKEN_CONSTEXPR`: emit `'constexpr' has been removed; use 'compile_time' for functions/variables, or 'compile_only' for functions that must not exist at runtime`, consume it, and continue with `isCompileTime = true` for recovery.
  - If the next token is identifier `compile_time` and the following token starts a declaration type (not `.`), consume it and set `isCompileTime = true`.
- In block-statement parsing, replace `check(TOKEN_CONSTEXPR)` with the same `compile_time` contextual check; keep the `constexpr` diagnostic path.
- Rename SemAnalyzer maps and methods: `constexprValues` → `compileTimeValues`, `constexprFunctions` → `compileTimeFunctions`, `getConstexprValues()` → `getCompileTimeValues()`, `isConstexprContainerExpr` can keep its internal name for now (container-specific), but update the `isCompileTime` field reads.
- Add `bool isCompileTime = false;` to `Symbol`; set it in `visit(VarDeclAST)` when the declaration is `compile_time`.
- Replace every language-level `constexpr` in `libs/std/*.cdd`, `tests/e2e/*.cpp` string sources, and `tests/e2e/resources/test_const.cdd` with `compile_time`. Do not change C++ `static constexpr` internals.
- Keep `TOKEN_CONSTEXPR` in `Token.h`/`Lexer.cpp`/`Utils.cpp` only for the migration diagnostic.

- [ ] **Step 4: Run the focused tests**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='ParserTest.*:ConstexprFunctionE2E.*:ConstexprContainersTest.*:CompileTimeE2E.*'`
Expected: PASS, except tests that still contain language-level `constexpr`; update them until the filter is green.

- [ ] **Step 5: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS with the same baseline count or more; no language-level `constexpr` errors.

- [ ] **Step 6: Commit**

```bash
git add src libs tests
git commit -m "refactor(ct): replace constexpr with compile_time"
```

---

### Task 2: Make `compile_time` variables read-only without mutating shared types

**Files:**
- Modify: `src/frontend/Parser.cpp` (`parseVariableDecl`)
- Modify: `src/sema/SemanticAnalyzer.h` (helper declaration)
- Modify: `src/sema/SemanticAnalyzer.cpp` (assignment/inc/dec/address checks)
- Modify: `src/ast/Symbol.h`
- Test: `tests/e2e/test_const_constexpr.cpp` (new read-only tests)

**Interfaces:**
- Consumes: `Symbol::isCompileTime`, `VarDeclAST::isCompileTime`, existing `isConstexprContainerExpr` root-variable pattern.
- Produces: `bool SemanticAnalyzer::isCompileTimeVariableExpr(const ExprAST* expr) const`; read-only enforcement for assignment, `++`/`--`, address-of, and by-value/pointer argument checks.

- [ ] **Step 1: Write the failing read-only tests**

Add to `tests/e2e/test_const_constexpr.cpp` (fixture `ConstConstexprE2E`):

```cpp
TEST_F(ConstConstexprE2E, CompileTimeVariableMutationRejected) {
    auto errs = compileErrors(R"(
compile_time int32 N = 1;
int32 main() { N = 2; return N; }
)", "ct_mutate.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "read-only"));
}
```

If this test file lacks `compileErrors`/`hasErrorContaining`, add small helpers modeled on `tests/e2e/test_constexpr_containers.cpp` (`analyzeSource` + error scan). The aggregate “does not poison the shared type” test belongs in Task 6, after aggregate values exist.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='ConstConstexprE2E.CompileTimeVariableMutationRejected'`
Expected: FAIL — mutation is accepted.

- [ ] **Step 3: Implement symbol-based read-only checks**

- In `Parser::parseVariableDecl`, stop doing `type->isConst = true` for non-scalar `compile_time` declarations. Do not mutate shared aggregate types. For scalars, either keep the existing copied const type or rely on the symbol flag; the symbol flag is authoritative.
- Add `SemanticAnalyzer::isCompileTimeVariableExpr(const ExprAST* expr) const`; return true when the root variable’s `Symbol::isCompileTime` is set. Reuse the same root-variable stripping logic as `isConstexprContainerExpr` (parentheses, member/index chains, unary deref if applicable).
- In `visit(AssignmentExprAST)`, reject a compile-time root with `"compile_time variable '<name>' is read-only"`.
- In unary `++`/`--` and postfix `++`/`--`, reject the same root.
- In unary `&`, reject taking a mutable address of a compile-time variable; allow a `const T*` result only if the existing const-pointer rules permit it.
- In call argument checks, reject passing a compile-time variable to a mutable by-value or mutable-pointer parameter.
- Propagate `isCompileTime` to every `VarDeclAST` symbol, including attached/static/global paths, in `visit(VarDeclAST)`.

- [ ] **Step 4: Run the focused tests**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='ConstConstexprE2E.CompileTimeVariableMutationRejected:*CompileTime*'`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(ct): enforce compile_time variable read-only semantics"
```

---

### Task 3: Add `compile_only` declaration, no runtime symbol, and scalar CT calls

**Files:**
- Modify: `src/frontend/Parser.h`, `src/frontend/Parser.cpp`
- Modify: `src/ast/Decl.h`, `src/ast/Decl.cpp`
- Modify: `src/sema/SemanticAnalyzer.h`, `src/sema/SemanticAnalyzer.cpp`
- Modify: `src/sema/CompileTimeEvaluator.cpp`
- Test: `tests/e2e/test_compile_only.cpp` (new)
- Test: `tests/frontend/test_parser.cpp`

**Interfaces:**
- Consumes: Task 1 `compileTimeFunctions`, Task 2 symbol read-only.
- Produces: `FunctionDeclAST::isCompileOnly`; `SemanticAnalyzer::compileOnlyFunctions`; `bool Parser::isCompileOnlyStart() const`; `FunctionDeclAST* findCtFunction(...)` also finds `compile_only`; codegen skips `isCompileOnly`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/frontend/test_parser.cpp`:

```cpp
TEST_F(ParserTest, CompileOnlyFunctionParses) {
    auto tu = parse("compile_only int32 twice(int32 x) { return x * 2; }");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto* fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->isCompileOnly);
}
```

Add `tests/e2e/test_compile_only.cpp`:

```cpp
TEST_F(CompileOnlyE2E, ScalarBuildsCompileTimeVariable) {
    EXPECT_EQ(runSource(R"(
compile_only int32 twice(int32 x) { return x * 2; }
compile_time int32 X = twice(21);
int32 main() { return X - 42; }
)", "compile_only_scalar.cdd"), 0);
}

TEST_F(CompileOnlyE2E, NoRuntimeSymbolOrDeclaration) {
    auto ctx = codegenSource(R"(
compile_only int32 helper(int32 x) { return x + 1; }
int32 main() { return 0; }
)", "compile_only_no_symbol.cdd");
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("helper"), nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("helper_int32"), nullptr);
}
```

`runSource`/`codegenSource` can be copied from `tests/e2e/test_constexpr_function.cpp`; `codegenSource` should stop before JIT and return the `CodegenContext` (or use a local lambda that returns `std::unique_ptr<CodegenContext>`).

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='ParserTest.CompileOnlyFunctionParses:CompileOnlyE2E.*'`
Expected: FAIL — `compile_only` is parsed as a type/identifier, and `helper` is emitted.

- [ ] **Step 3: Implement the declaration and no-symbol path**

- Add `bool isCompileOnly = false;` to `FunctionDeclAST`; leave the constructor default false and set it after parsing.
- Add `bool Parser::isCompileOnlyStart() const` (identifier lexeme `compile_only`, not followed by `.`). In `parseDeclarationImpl` and block declaration parsing, consume it, parse the following declaration, and:
  - if the result is a `FunctionDeclAST`, set `isCompileOnly = true`;
  - otherwise emit `compile_only is only valid on functions` and return the parsed declaration for recovery.
- Reject `compile_only` on methods during sema (`currentClass != nullptr`), on templates (`TemplateDeclAST` function), and on lambdas (no parse path needed).
- Add `std::unordered_map<std::string, FunctionDeclAST*> compileOnlyFunctions;` to sema; register under bare and mangled names in `visit(FunctionDeclAST)`.
- Add `bool m_inCompileTimeInitializer = false;` to `SemanticAnalyzer`; set it around `getExprType`/`evaluateConstexpr` in `visit(VarDeclAST)` for `isCompileTime`, and around `evalCompileTime` for `compile_time.static_assert`/`compile_time.if` conditions. Restore on all exits.
- Extend `findCtFunction`, `evalConstexprCall`, and `evalConstexprCallCT` lookup to include `compileOnlyFunctions`.
- In `FunctionDeclAST::codegenPrototype` and `FunctionDeclAST::codegen`, return `nullptr` immediately when `isCompileOnly`; ensure `TranslationUnitAST` pre-scan and method/template paths do not create it elsewhere.
- In `visit(CallExprAST)`, after `checkFunctionCall`, if the resolved callee is `compile_only`, allow the call only when:
  - the enclosing context is a `compile_time` variable initializer (`SemanticAnalyzer::m_inCompileTimeInitializer`), or
  - `currentFunction && currentFunction->isCompileOnly`.
  Otherwise emit `compile_only function '<name>' cannot be called at runtime` (Task 5 refines the `if compile_time` exception and chain).
- Reject `&f`, function-pointer assignment, and value use of a `compile_only` function in `visit(VariableExprAST)` when `isFunctionRef` / address-of paths reach it.

- [ ] **Step 4: Run the focused tests**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='ParserTest.CompileOnlyFunctionParses:CompileOnlyE2E.*'`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(ct): add compile_only free functions with no runtime symbol"
```

---

### Task 4: Implement `if compile_time` dual-path statements

**Files:**
- Modify: `src/ast/Decl.h` (new `CompileTimeContextIfStmtAST`)
- Modify: `src/frontend/Parser.h`, `src/frontend/Parser.cpp`
- Modify: `src/sema/SemanticAnalyzer.h`, `src/sema/SemanticAnalyzer.cpp`
- Modify: `src/sema/CompileTimeEvaluator.h`, `src/sema/CompileTimeEvaluator.cpp`
- Test: `tests/e2e/test_compile_only.cpp`
- Test: `tests/frontend/test_parser.cpp`

**Interfaces:**
- Consumes: `FunctionDeclAST::isCompileTime`, `isCompileOnly`; existing `CompileTimeIfStmtAST` for `compile_time.if(cond)`.
- Produces: `CompileTimeContextIfStmtAST` with `thenStmt`, `elseStmt`; `Parser::parseCompileTimeContextIf()`; `SemanticAnalyzer::m_inCtOnlyBranch`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/e2e/test_compile_only.cpp`:

```cpp
TEST_F(CompileOnlyE2E, IfCompileTimeSelectsBothPaths) {
    EXPECT_EQ(runSource(R"(
compile_time int32 f(int32 x) {
    if compile_time {
        return x + 1;
    } else {
        return x - 1;
    }
}
compile_time int32 CT = f(5);
int32 main() {
    return f(5) * 10 + CT - 46;
}
)", "if_compile_time.cdd"), 0);
}

TEST_F(CompileOnlyE2E, IfCompileTimeRejectedInRuntimeFunction) {
    auto errs = compileErrors(R"(
int32 main() {
    if compile_time { return 1; } else { return 0; }
}
)", "if_compile_time_bad.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "if compile_time"));
}
```

Add to `tests/frontend/test_parser.cpp`:

```cpp
TEST_F(ParserTest, ParseIfCompileTime) {
    auto tu = parse("compile_time int32 f() { if compile_time { return 1; } else { return 0; } }");
    ASSERT_NE(tu, nullptr);
    auto* fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->body, nullptr);
    ASSERT_EQ(fn->body->stmts.size(), 1u);
    EXPECT_NE(dynamic_cast<CompileTimeContextIfStmtAST*>(fn->body->stmts[0].get()), nullptr);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='*IfCompileTime*:ParserTest.ParseIfCompileTime'`
Expected: FAIL — parser expects `(` after `if`, and the AST node does not exist.

- [ ] **Step 3: Implement the AST, parser, sema, codegen, and CT interpreter**

- Add `CompileTimeContextIfStmtAST` to `src/ast/Decl.h`:

```cpp
class CompileTimeContextIfStmtAST : public StmtAST {
public:
    std::unique_ptr<CompoundStmtAST> thenStmt;
    std::unique_ptr<CompoundStmtAST> elseStmt;
    llvm::Value* codegen(CodegenContext& ctx) override;
};
```

- In `Parser::parseIfStmt`, after consuming `if`, if the next token is identifier `compile_time` and the token after it is `{`, parse:
  - `thenStmt = parseCompoundStmt()`;
  - optional `else` followed by a `{` block into `elseStmt`; a non-block else emits `expected '{' after 'else' in if compile_time`.
  Return the new node. Otherwise continue the normal `(` path.
- In `Codegen`, emit only `elseStmt` (or nothing) for `CompileTimeContextIfStmtAST`; `compile_only` functions are never emitted anyway.
- In `SemanticAnalyzer::visit(CompileTimeContextIfStmtAST&)`:
  - if no `currentFunction` or neither `isCompileTime` nor `isCompileOnly`, emit `if compile_time is only allowed inside compile_time/compile_only functions`;
  - set `m_inCtOnlyBranch = true`, visit `thenStmt`, restore;
  - if current function is not `compile_only`, visit `elseStmt`.
- In `SemanticAnalyzer::evalConstexprStmt`, add a case for `CompileTimeContextIfStmtAST` that executes `thenStmt` unconditionally.
- Do not confuse this with `CompileTimeIfStmtAST`: keep the `compile_time.if(cond)` path unchanged.
- Add `bool m_inCtOnlyBranch = false;` to `SemanticAnalyzer` (Task 3 already adds `m_inCompileTimeInitializer`).

- [ ] **Step 4: Run the focused tests**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='*IfCompileTime*:ParserTest.ParseIfCompileTime'`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(ct): add if compile_time dual-path statements"
```

---

### Task 5: Enforce `compile_only` call contexts, body restrictions, and call-chain diagnostics

**Files:**
- Modify: `src/sema/SemanticAnalyzer.h`, `src/sema/SemanticAnalyzer.cpp`
- Modify: `src/sema/CompileTimeEvaluator.cpp`
- Test: `tests/e2e/test_compile_only.cpp`
- Test: `tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 3 `isCompileOnly`, Task 4 `m_inCtOnlyBranch`.
- Produces: `struct CallEdge`, `struct CompileOnlyViolation`, `reportCompileOnlyViolations()`; `checkCompileOnlyCallSite`; `validateCompileOnlyBody`; `m_callEdges`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/e2e/test_compile_only.cpp`:

```cpp
TEST_F(CompileOnlyE2E, DirectCallInCompileTimeFunctionRejected) {
    auto errs = compileErrors(R"(
compile_only int32 build(int32 x) { return x * 2; }
compile_time int32 make(int32 x) { return build(x); }
int32 main() { return make(1); }
)", "compile_only_direct.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "outside 'if compile_time'"));
}

TEST_F(CompileOnlyE2E, RuntimeCallChainReportsPath) {
    auto errs = compileErrors(R"(
compile_only int32 build(int32 x) { return x * 2; }
int32 make(int32 x) { return build(x); }
int32 main() { return make(1); }
)", "compile_only_runtime.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "main"));
    EXPECT_TRUE(hasErrorContaining(errs, "make"));
    EXPECT_TRUE(hasErrorContaining(errs, "build"));
}

TEST_F(CompileOnlyE2E, CompileOnlyBodyCannotCallRuntimeFunction) {
    auto errs = compileErrors(R"(
int32 runtime_helper(int32 x) { return x; }
compile_only int32 build(int32 x) { return runtime_helper(x); }
int32 main() { return 0; }
)", "compile_only_runtime_body.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "cannot call runtime function"));
}

TEST_F(CompileOnlyE2E, AddressOfCompileOnlyRejected) {
    auto errs = compileErrors(R"(
compile_only int32 build(int32 x) { return x; }
int32 main() { int32 (*p)(int32) = &build; return p(1); }
)", "compile_only_addr.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "address"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='CompileOnlyE2E.*'`
Expected: FAIL — direct/body/address cases are not diagnosed or lack chain text.

- [ ] **Step 3: Implement call contexts, body checks, and reachability**

- Add to `SemanticAnalyzer`:
  - `struct CallEdge { FunctionDeclAST* from; FunctionDeclAST* to; bool ctOnly; const ASTNode* site; };`
  - `struct CompileOnlyViolation { FunctionDeclAST* from; FunctionDeclAST* callee; const ASTNode* site; bool directInCompileTime; };`
  - `std::vector<CallEdge> m_callEdges;`
  - `std::vector<CompileOnlyViolation> m_compileOnlyViolations;`
  - `std::vector<std::string> m_callStack;` (or derive paths from edges).
- In `visit(CallExprAST)`, after callee resolution, find the `FunctionDeclAST*` (use `findCtFunction` with `mangleFunction` and fallback). Record an edge from `currentFunction` to the callee with `ctOnly = m_inCtOnlyBranch || (currentFunction && currentFunction->isCompileOnly) || m_inCompileTimeInitializer`.
- `checkCompileOnlyCallSite(node, callee)`:
  - allowed when `m_inCompileTimeInitializer`, `m_inCtOnlyBranch`, or `currentFunction->isCompileOnly`;
  - if `currentFunction->isCompileTime` and not in a CT-only branch, emit `compile_only function '<name>' cannot be called from compile_time function '<fn>' outside 'if compile_time'` and record a violation;
  - otherwise emit `compile_only function '<name>' cannot be called at runtime` and record a violation.
- Body checks when `currentFunction && currentFunction->isCompileOnly`:
  - `visit(CallExprAST)`: reject any callee that is not `compile_time`/`compile_only` with `cannot call runtime function`.
  - `visit(VariableExprAST)`: reject mutable global/namespace variables, `static` locals, `thread_local`; allow locals/params/enum/`compile_time` variables. Use `Symbol::isStaticStorage && !Symbol::isCompileTime` for the static-storage rejection; locals/params are not static-storage.
  - Reject `volatile`/`atomic` declarations/expressions and `AsmExprAST` in the body.
  - Reject `defer` and `compile_time.repeat` inside `compile_only`.
  - Reject `compile_only` calls used as values/addresses/fields/captures; keep direct call expressions valid.
- At the end of `SemanticAnalyzer::analyze`, call `reportCompileOnlyViolations()`:
  - Build adjacency from `m_callEdges` (runtime edges only for reachability; CT-only edges may be shown as CT path).
  - Runtime roots: `main`, exported functions, functions whose address is taken, and runtime global initializers.
  - BFS to produce a path `root -> ... -> from -> callee`; if none, use `from -> callee`.
  - Emit the full path in the diagnostic, with the hint from the spec.
  - For `directInCompileTime`, always emit the direct-call diagnostic; include the runtime path if one exists.
- De-duplicate repeated violations at the same site.

- [ ] **Step 4: Run the focused tests**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='CompileOnlyE2E.*:SemanticAnalyzerTest.*CompileOnly*'`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(ct): enforce compile_only reachability and body restrictions"
```

---

### Task 6: Add `CtAggregate` and recursive constant materialization

**Files:**
- Modify: `src/ast/CtValue.h`, `src/ast/CtValue.cpp`
- Modify: `src/ast/Decl.h`, `src/ast/Decl.cpp`
- Modify: `src/sema/SemanticAnalyzer.cpp` (`ctBindValue`, `VarDeclAST` folding)
- Test: `tests/e2e/test_compile_only.cpp`
- Test: `tests/ast/test_type_nodes.cpp` or a new `tests/ast/test_ct_aggregate.cpp` (CMake glob; rerun configure)

**Interfaces:**
- Consumes: existing `CtValue`, `ctDefaultValue`, `ctClone`, `ctValueToConstant` (currently scalar/container only).
- Produces: `CtValue::AGGREGATE`; `std::shared_ptr<CtAggregate> CtValue::aggregate`; recursive `ctClone`/`ctDefaultValue`/`ctValueToConstant`; `VarDeclAST::foldedAggregate`.

- [ ] **Step 1: Write the failing aggregate materialization test**

Add to `tests/e2e/test_compile_only.cpp`:

```cpp
TEST_F(CompileOnlyE2E, CompileTimeAggregateInitializerMaterializes) {
    EXPECT_EQ(runSource(R"(
struct Pair { int32 a; int32 b; };
compile_time Pair P = { 20, 21 };
int32 main() { return P.a + P.b - 41; }
)", "ct_pair.cdd"), 0);
}

TEST_F(CompileOnlyE2E, CompileTimeAggregateDoesNotPoisonType) {
    EXPECT_EQ(runSource(R"(
struct S { int32 x; };
compile_time S A = { 1 };
int32 main() {
    S b;
    b.x = 2;
    return A.x + b.x - 3;
}
)", "ct_no_type_poison.cdd"), 0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='CompileOnlyE2E.CompileTimeAggregateInitializerMaterializes:CompileOnlyE2E.CompileTimeAggregateDoesNotPoisonType'`
Expected: FAIL — `InitializerListExprAST` folds to integer 0 / no aggregate constant, and/or `S` is globally const.

- [ ] **Step 3: Implement the aggregate value model and materialization**

- Add to `CtValue.h`:

```cpp
struct CtAggregate;
struct CtValue {
    enum Type { /* existing */, AGGREGATE };
    std::shared_ptr<CtAggregate> aggregate;
};
struct CtAggregate {
    ::Type* type = nullptr;
    std::vector<CtValue> elements;
    std::size_t totalElements = 0;
};
```

- `ctClone` deep-copies aggregates.
- `ctDefaultValue` handles `ArrayType` (recursive element defaults) and `StructType`/`ClassType` (recursive field defaults). Use `fields` in declaration order; do not include base-class slots in v1.
- `ctBindValue` (in `SemanticAnalyzer.cpp`) accepts aggregate values when target is a matching aggregate/array; element-wise bind scalar fields; reject length/type mismatch.
- `ctValueToConstant` in `Decl.cpp` recurses:
  - array → `llvm::ConstantArray`;
  - struct/class → `llvm::ConstantStruct` using `ctx.getLLVMType`;
  - reject pointers/strings/unions/bitfields/flexible arrays with a sema diagnostic (the sema signature check in Task 7 is the primary gate).
- Add `std::shared_ptr<CtAggregate> foldedAggregate;` to `VarDeclAST`.
- In `visit(VarDeclAST)`, when a `compile_time` initializer folds to `AGGREGATE`, store it in `foldedAggregate` and also in `compileTimeValues`.
- In `VarDeclAST::codegen`, add branches before the scalar/container paths:
  - global: `isCompileTime && foldedAggregate` → `ctValueToConstant`, private constant global;
  - static local: same constant initializer;
  - block local: `alloca` + `store` constant.
- Implement `InitializerListExprAST` folding for aggregates/arrays in `CompileTimeEvaluator::eval`: use the sema-assigned `type`, fill positionally, zero-fill missing elements. If designated initializers are not practical, diagnose them in CT aggregate context and document the v1 limitation.

- [ ] **Step 4: Run the focused tests**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='CompileOnlyE2E.CompileTimeAggregateInitializerMaterializes:CompileOnlyE2E.CompileTimeAggregateDoesNotPoisonType:*CtAggregate*'`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(ct): add compile-time aggregate values and materialization"
```

---

### Task 7: Add aggregate lvalues/returns, string builtins, and the `DfaTable` end-to-end case

**Files:**
- Modify: `src/sema/CompileTimeEvaluator.cpp`
- Modify: `src/sema/SemanticAnalyzer.cpp` (`evalConstexprStmt`, function signature validation, call binding)
- Modify: `src/ast/CtValue.h`, `src/ast/CtValue.cpp` if aggregate helpers are needed
- Modify: `src/ast/Decl.h` only if materialization needs a new field
- Test: `tests/e2e/test_compile_only.cpp`
- Test: `tests/e2e/test_constexpr_containers.cpp` (no regressions)

**Interfaces:**
- Consumes: Task 6 `CtAggregate`, `ctBindValue`, `ctDefaultValue`, `ctValueToConstant`.
- Produces: CT evaluator support for aggregate member/index reads/writes, aggregate parameters/returns, `string`/`string_view` `len()`/`char_at()`, `validateCompileOnlySignature`, and DfaTable e2e.

- [ ] **Step 1: Write the failing aggregate/DfaTable tests**

Add to `tests/e2e/test_compile_only.cpp`:

```cpp
TEST_F(CompileOnlyE2E, AggregateReturnAndMemberWrites) {
    EXPECT_EQ(runSource(R"(
struct Pair { int32 a; int32 b; };
compile_only Pair makePair(int32 x) {
    Pair p;
    p.a = x;
    p.b = x + 1;
    return p;
}
compile_time Pair P = makePair(20);
int32 main() { return P.a + P.b - 41; }
)", "ct_pair_return.cdd"), 0);
}

TEST_F(CompileOnlyE2E, AggregateParameterIsDeepCopied) {
    EXPECT_EQ(runSource(R"(
struct Pair { int32 a; int32 b; };
compile_only Pair makePair(int32 x) {
    Pair p;
    p.a = x;
    p.b = x + 1;
    return p;
}
compile_only int32 mutate(Pair p) {
    p.a = 99;
    return p.a;
}
compile_time Pair P = makePair(20);
compile_time int32 X = mutate(P);
int32 main() { return P.a + X - 119; }
)", "ct_pair_copy.cdd"), 0);
}

TEST_F(CompileOnlyE2E, DfaTableBuildsAtCompileTimeWithoutSymbol) {
    auto ctx = codegenSource(R"(
struct DfaTable {
    int32 next[4][8];
    int32 initial;
};
compile_only DfaTable build_dfa(string_view pattern) {
    DfaTable t;
    for (usize s = 0; s < 4; s = s + 1) {
        for (usize c = 0; c < 8; c = c + 1) {
            t.next[s][c] = 0;
        }
    }
    t.initial = 0;
    for (usize i = 0; i < pattern.len(); i = i + 1) {
        t.next[0][i] = pattern.char_at(i);
    }
    return t;
}
compile_time DfaTable EMAIL_DFA = build_dfa("abc");
int32 main() { return EMAIL_DFA.next[0][1] - 'b'; }
)", "ct_dfa.cdd");
    ASSERT_NE(ctx, nullptr);
    EXPECT_EQ(ctx->getModule().getFunction("build_dfa"), nullptr);
    EXPECT_NE(ctx->getModule().getGlobalVariable("EMAIL_DFA"), nullptr);
}

TEST_F(CompileOnlyE2E, CompileOnlyStepLimitDiagnosed) {
    auto errs = compileErrors(R"(
compile_only int32 spin(int32 x) {
    while (x > 0) { x = x; }
    return x;
}
compile_time int32 N = spin(1);
int32 main() { return 0; }
)", "ct_step_limit.cdd");
    EXPECT_TRUE(hasErrorContaining(errs, "limit"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='CompileOnlyE2E.*'`
Expected: FAIL — member/index assignment, aggregate return, string builtins, or step budget are missing.

- [ ] **Step 3: Implement aggregate evaluation and signature checks**

- In `CompileTimeEvaluator::eval`, add:
  - `InitializerListExprAST` (if not already completed in Task 6);
  - `MemberAccessExprAST` for aggregate field reads (non-`compile_time` roots);
  - `ArrayAccessExprAST` for array/aggregate index reads, with bounds checks;
  - `MethodCallExprAST` for `string`/`string_view` `len()` and `char_at()` only;
  - aggregate value reads clone (value semantics).
- In `SemanticAnalyzer::evalConstexprStmt`, add:
  - local aggregate declarations using `ctDefaultValue`;
  - `AssignmentExprAST`/compound assignment/`++`/`--` for lvalue paths `variable`, `member`, `index` (nested);
  - `return` of aggregate values.
- Add a CT lvalue helper (variable name + path of field/index indices) and write through the aggregate’s `shared_ptr`; do not return raw runtime pointers.
- In `ctBindValue`, bind aggregate parameters by deep clone; match target field/array types.
- Extend `validateCompileOnlySignature`:
  - return `void` or a CT value type: scalar, enum, fixed array, POD record; reject pointer/reference/string/string_view/union/bitfield/flexible array/base-class slot;
  - params: CT scalar/enum/aggregate, read-only `string`/`string_view`; no pointers/references;
  - methods/templates/lambdas rejected.
- Extend `validateCompileTimeSignature` with the same CT value types plus pointer-to-CT-value out-params; reject owning `string`.
- Ensure `compile_time` variable of aggregate type uses `foldedAggregate` and does not set `Type::isConst`.
- Add a step/element budget check in aggregate construction and lvalue writes; emit `compile_time evaluation limit exceeded ... (steps|elements)` and set `m_ctFailed`.

- [ ] **Step 4: Run the focused tests**

Run: `cmake --build build -j$(nproc) && build/compiler_tests --gtest_filter='CompileOnlyE2E.*:ConstexprContainersTest.*'`
Expected: PASS.

- [ ] **Step 5: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(ct): support compile-time aggregate construction and DfaTable"
```

---

### Task 8: Finish migration, docs, example, and full verification

**Files:**
- Modify: `docs/spec/compile_time.md`
- Modify: `docs/spec/grammar.ebnf`
- Modify: `docs/spec/keywords.md`
- Modify: `docs/spec/semantics.md`
- Modify: `docs/spec/stdlib.md`
- Modify: `examples/all_features.cdd`
- Modify: `TODO.md`, `Progress.md`
- Test: full suite + example + IR checks

**Interfaces:**
- Consumes: all previous tasks.
- Produces: final docs/example consistent with the implemented syntax; no language-level `constexpr`.

- [ ] **Step 1: Update the spec documents**

- `compile_time.md`: replace DEC-05 “coexist” text with `compile_time`/`compile_only`; add syntax, CT contexts, `if compile_time`, aggregate escape, read-only rules, `DfaTable` example.
- `grammar.ebnf`: add `compile_time`/`compile_only` declaration modifiers; add `if compile_time` statement; keep `compile_time.if(cond)`.
- `keywords.md`: mark `constexpr` as `[removed]`; list `compile_time`/`compile_only` as contextual.
- `semantics.md`: add read-only symbol rules, no-runtime-symbol rules, CT call contexts.
- `stdlib.md`: replace language-level `constexpr` with `compile_time`.
- `TODO.md`: mark CT-23..CT-27 complete when done; update DEC-05; add final progress snapshot.
- `Progress.md`: append the implementation record with test counts and example output.

- [ ] **Step 2: Add the all-features example section**

Add a small section to `examples/all_features.cdd`:

```cdd
struct DfaTable {
    int32 next[4][8];
    int32 initial;
};

compile_only DfaTable build_dfa(string_view pattern) {
    DfaTable t;
    for (usize s = 0; s < 4; s = s + 1) {
        for (usize c = 0; c < 8; c = c + 1) {
            t.next[s][c] = 0;
        }
    }
    t.initial = 0;
    for (usize i = 0; i < pattern.len(); i = i + 1) {
        t.next[0][i] = pattern.char_at(i);
    }
    return t;
}

compile_time DfaTable EMAIL_DFA = build_dfa("abc");

compile_time int32 table_at(int32 state, int32 c) {
    if compile_time {
        return EMAIL_DFA.next[state][c];
    } else {
        return EMAIL_DFA.next[state][c];
    }
}
```

Add an assertion/print that exercises `table_at(0, 1) == 'b'`.

- [ ] **Step 3: Run focused checks**

Run:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'CompileOnlyE2E|CompileTime|ParserTest|SemanticAnalyzerTest' -j8
build/bin/cdd examples/all_features.cdd -o /tmp/opencode/all_features
/tmp/opencode/all_features
```

Expected: focused tests pass; example prints `ALL FEATURES OK`.

- [ ] **Step 4: Run the full suite**

Run: `ctest --test-dir build -j8`
Expected: PASS; record the exact count.

- [ ] **Step 5: Verify migration completeness**

Run:

```bash
grep -R "constexpr" src libs tests examples --include='*.cpp' --include='*.h' --include='*.cdd' | grep -v 'static constexpr' || true
```

Expected: only C++ `static constexpr` internals and the migration-diagnostic strings; no language-level source uses.

- [ ] **Step 6: Commit**

```bash
git add src libs tests examples docs/spec TODO.md Progress.md
git add -f docs/superpowers/specs/2026-11-10-compile-time-compile-only-design.md docs/superpowers/plans/2026-11-10-compile-time-compile-only.md
git commit -m "docs(ct): finalize compile_time/compile_only migration"
```

---

## Self-Review Checklist

- [ ] Spec §1–§5: syntax, read-only variables, compile_time functions, compile_only contexts, aggregate returns, `if compile_time`, diagnostics → Tasks 1–7.
- [ ] Spec §6: `CtAggregate`, lvalue paths, recursive materialization → Tasks 6–7.
- [ ] Spec §7: no shared `Type::isConst` pollution → Task 2.
- [ ] Spec §8: stdlib/tests/docs migration → Tasks 1 and 8.
- [ ] Spec §9: parser/sema/evaluator/e2e/IR tests → Tasks 1–8.
- [ ] Review Focus lines each have a test in the owning task.
- [ ] No task relies on a type/function not defined by an earlier task.
- [ ] Plan length is acceptable: tasks give interfaces and tests, not full implementations.
