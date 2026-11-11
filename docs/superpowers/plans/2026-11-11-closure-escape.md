# Capturing-Closure Escape Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow by-value capturing closures to escape a function by return through `auto` return-type deduction, while rejecting by-reference/`this` closure escape at compile time.

**Architecture:** Extend sema so a free runtime function declared with `auto` return type is analyzed body-first, infers its return type from the first value `return`, then registers its final signature. Closure capture metadata on `StructType` lets return checking reject by-reference/`this` captures. Existing struct-return codegen and closure-call codegen carry the by-value closure across frames.

**Tech Stack:** C++ compiler implementation, GoogleTest e2e tests, LLVM JIT harness, Cmm examples/spec.

**Spec:** `docs/superpowers/specs/2026-11-11-closure-escape-design.md`

## Global Constraints

- Execute natively in the current worktree; do not create a worktree and do not dispatch per-task subagents.
- No new language alias for `auto`; no GC, heap allocation, reference counting, or type erasure.
- `auto` return deduction is for free runtime functions only in v1. Methods, `compile_time`, and `compile_only` functions are rejected with targeted diagnostics.
- By-value capture closure returns are safe and must work. By-reference and `this` capture closure returns must be compile errors.
- Keep existing lambda behavior, no-capture function-pointer behavior, `auto` local variables, and module type visibility unchanged.
- Every task ends with a focused build/test run and a commit.
- Full-suite baseline before this plan: **1865/1865**. Final full suite must be green.
- Do not use `constexpr` in new Cmm code; use `compile_time` where applicable.

## Review Focus

- **Calling an `auto`-return function before its definition:** expected a clear unresolved-call error, never an `Auto` type in codegen.
- **Mixed or bare `return` in an `auto` function:** expected a diagnostic, never invalid LLVM IR.
- **Indirect by-reference closure escape (`auto f = [&x]...; return f;`):** expected the same rejection as returning the lambda directly.
- **By-value closure called after the creating frame returns:** expected captured values copied correctly and independent closure instances not sharing state.
- **Namespace-exported struct/class/template with methods, statics, and inheritance across modules:** expected visible and callable, with no type redefinition.

---

### Task 1: `auto` return-type deduction for free functions

**Files:**
- Modify: `src/sema/SemanticAnalyzer.h`
- Modify: `src/sema/SemanticAnalyzer.cpp`
- Test: `tests/e2e/test_lambda.cpp`

**Interfaces:**
- Consumes: existing `decayInferredReturnType(Type*)`, `typesCompatible(Type*, Type*)`, `mangleFunction`, `declareSymbol`, `definedFunctions`.
- Produces: `SemanticAnalyzer::m_autoReturnFunction`, `m_inferredAutoReturn`, `m_pendingAutoVoidReturns`; `visit(FunctionDeclAST&)` registers an `auto` function only after its body is analyzed.

- [ ] **Step 1: Write the failing tests**

Add these tests to `tests/e2e/test_lambda.cpp` after the existing lambda tests. Also add local `compileErrors` / `hasErrorContaining` helpers near the existing `runSource` helper:

```cpp
static std::vector<std::string> compileErrors(const std::string& source,
                                              const std::string& filename) {
    Lexer lexer(filename, source);
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    std::vector<std::string> errors;
    for (auto& d : parser.getErrors()) errors.push_back(d.format());
    if (!ast) return errors;
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (auto& d : analyzer.getErrors()) errors.push_back(d.format());
    return errors;
}

static bool hasErrorContaining(const std::vector<std::string>& errors,
                               const std::string& needle) {
    for (auto& e : errors) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}

TEST_F(LambdaE2E, AutoReturnScalar) {
    EXPECT_EQ(runSource(R"(
        auto twice(int32 x) { return x * 2; }
        int32 main() { auto y = twice(21); return y - 42; }
    )", "auto_ret_scalar.cmm"), 0);
}

TEST_F(LambdaE2E, AutoReturnVoid) {
    EXPECT_EQ(runSource(R"(
        auto noop() { return; }
        int32 main() { noop(); return 0; }
    )", "auto_ret_void.cmm"), 0);
}

TEST_F(LambdaE2E, AutoReturnInconsistentTypesRejected) {
    auto errs = compileErrors(R"(
        auto bad(int32 x) {
            if (x > 0) return x;
            return "no";
        }
        int32 main() { return 0; }
    )", "auto_ret_mismatch.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "inconsistent return types"));
}

TEST_F(LambdaE2E, AutoReturnBareAfterValueRejected) {
    auto errs = compileErrors(R"(
        auto bad(int32 x) {
            if (x > 0) return x;
            return;
        }
        int32 main() { return 0; }
    )", "auto_ret_bare.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "must return a value"));
}

TEST_F(LambdaE2E, AutoReturnPrototypeRejected) {
    auto errs = compileErrors(R"(
        auto f(int32 x);
        int32 main() { return 0; }
    )", "auto_ret_proto.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "must have a body"));
}

TEST_F(LambdaE2E, AutoReturnMethodRejected) {
    auto errs = compileErrors(R"(
        class C { public: auto f() { return 1; } };
        int32 main() { return 0; }
    )", "auto_ret_method.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "not supported on methods"));
}

TEST_F(LambdaE2E, AutoReturnCompileTimeRejected) {
    auto errs = compileErrors(R"(
        compile_time auto f() { return 1; }
        int32 main() { return 0; }
    )", "auto_ret_ct.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "compile_time"));
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run:

```bash
ctest --test-dir build -R 'LambdaE2E\.AutoReturn' --output-on-failure
```

Expected: failures showing `expected 'auto'` return mismatch or missing diagnostics.

- [ ] **Step 3: Add auto-inference state to sema**

In `src/sema/SemanticAnalyzer.h`, next to `currentFunction`:

```cpp
// Auto-return inference for the free function currently being analyzed.
// Non-null only while visiting that function's body.
FunctionDeclAST* m_autoReturnFunction = nullptr;
Type* m_inferredAutoReturn = nullptr;
std::vector<ReturnStmtAST*> m_pendingAutoVoidReturns;
```

- [ ] **Step 4: Implement body-first inference in `visit(FunctionDeclAST&)`**

In `src/sema/SemanticAnalyzer.cpp`, after return/parameter types are resolved and after the existing `compile_time`/`compile_only` validation blocks, detect:

```cpp
Type* resolvedReturn = stripTypedef(node.returnType);
const bool isAutoReturn = resolvedReturn && resolvedReturn->kind == TypeKind::Auto;
```

For `isAutoReturn`:

1. Reject and return early with these exact messages:
   - no body: `"auto function '" + node.name + "' must have a body"`
   - `currentClass != nullptr || !node.className.empty()`:
     `"auto return type is not supported on methods"`
   - `node.isCompileTime || node.isCompileOnly`:
     `"auto return type is not supported on compile_time/compile_only functions"`
2. Do **not** register the signature yet.
3. Set `currentFunction = &node`, `enterScope()`, declare parameters as the existing body path does.
4. Save the `Auto` pointer, set `node.returnType = typeCtx->getVoid()`, set `m_autoReturnFunction = &node`, clear `m_inferredAutoReturn` and `m_pendingAutoVoidReturns`.
5. `visit(*node.body);` then `checkInitialization(node);`.
6. Set `node.returnType = m_inferredAutoReturn ? m_inferredAutoReturn : typeCtx->getVoid()`.
7. For every pending bare return when the final type is non-void, emit
   `"non-void function '<name>' must return a value"` at that pending node.
8. Clear `m_autoReturnFunction`, `m_inferredAutoReturn`, `m_pendingAutoVoidReturns`, exit scope, restore `currentFunction`.
9. Fall through to the existing signature-registration block using the final `node.returnType`.
10. Skip the existing body-analysis block for `isAutoReturn` (it already ran); keep it for all other functions.

Do not change the existing non-`auto` path.

- [ ] **Step 5: Implement the `ReturnStmtAST` auto branch**

In `src/sema/SemanticAnalyzer.cpp` `visit(ReturnStmtAST&)`, after the existing lambda-frame inference block and before the generic return checks:

```cpp
if (currentFunction && currentFunction == m_autoReturnFunction) {
    const bool bareReturn = !node.value;
    const bool voidExprReturn = strippedVal &&
                                strippedVal->kind == TypeKind::Void;
    auto nonVoidMessage = [&]() {
        return "non-void function '" + currentFunction->name +
               "' must return a value";
    };

    if (bareReturn || voidExprReturn) {
        if (m_inferredAutoReturn &&
            stripTypedef(m_inferredAutoReturn)->kind != TypeKind::Void) {
            emitError(nonVoidMessage(), node);
        } else {
            m_pendingAutoVoidReturns.push_back(&node);
        }
        return;
    }

    if (!m_inferredAutoReturn) {
        Type* inferred = decayInferredReturnType(retValType);
        m_inferredAutoReturn = inferred;
        currentFunction->returnType = inferred;
        Type* deduced = stripTypedef(inferred);
        if (deduced && deduced->kind != TypeKind::Void) {
            for (ReturnStmtAST* pending : m_pendingAutoVoidReturns) {
                emitError(nonVoidMessage(), *pending);
            }
        }
        m_pendingAutoVoidReturns.clear();
    } else if (!typesCompatible(m_inferredAutoReturn, retValType)) {
        emitError("auto function '" + currentFunction->name +
                      "' has inconsistent return types: '" +
                      typeToString(m_inferredAutoReturn) + "' vs '" +
                      typeToString(retValType) + "'",
                  node);
    }
    return;
}
```

Keep the lambda-frame block first so a lambda nested inside an auto function still uses lambda inference.

- [ ] **Step 6: Run the focused tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E\.AutoReturn' --output-on-failure
```

Expected: all `LambdaE2E.AutoReturn*` tests pass.

- [ ] **Step 7: Commit**

```bash
git add src/sema/SemanticAnalyzer.h src/sema/SemanticAnalyzer.cpp tests/e2e/test_lambda.cpp
git commit -m "feat(sema): infer auto return types for free functions"
```

---

### Task 2: Reject by-reference and `this` closure escape

**Files:**
- Modify: `src/ast/Type.h`
- Modify: `src/sema/SemanticAnalyzer.cpp`
- Test: `tests/e2e/test_lambda.cpp`

**Interfaces:**
- Consumes: Task 1 `m_autoReturnFunction`, `m_inferredAutoReturn`; existing `LambdaExprAST::Capture`.
- Produces: `StructType::closureHasByRefCapture`, `StructType::closureCapturesThis`; return checking that rejects escaping closures carrying those flags.

- [ ] **Step 1: Write the failing tests**

Add to `tests/e2e/test_lambda.cpp`:

```cpp
TEST_F(LambdaE2E, ByReferenceClosureReturnRejected) {
    auto errs = compileErrors(R"(
        auto bad() {
            int32 x = 7;
            return [&x]() -> int32 { return x; };
        }
        int32 main() { return 0; }
    )", "auto_ret_ref.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "by-reference captures cannot escape by return"));
}

TEST_F(LambdaE2E, IndirectByReferenceClosureReturnRejected) {
    auto errs = compileErrors(R"(
        auto bad() {
            int32 x = 7;
            auto f = [&x]() -> int32 { return x; };
            return f;
        }
        int32 main() { return 0; }
    )", "auto_ret_ref_indirect.cmm");
    EXPECT_TRUE(hasErrorContaining(errs, "by-reference captures cannot escape by return"));
}

TEST_F(LambdaE2E, ByReferenceClosureLocalCallStillAllowed) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 7;
            auto f = [&x]() -> int32 { return x; };
            return f() - 7;
        }
    )", "lambda_ref_local_ok.cmm"), 0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
ctest --test-dir build -R 'LambdaE2E\.(ByReferenceClosure|IndirectByReferenceClosure)' --output-on-failure
```

Expected: the two negative tests fail because the escape is currently accepted (or no targeted diagnostic); the local-call test already passes.

- [ ] **Step 3: Add closure capture metadata**

In `src/ast/Type.h` inside `StructType`, after `closureReturnType`:

```cpp
// Capturing-closure escape safety: by-reference capture stores a pointer
// into the creating frame; `this` stores the receiver pointer. Returning
// such a closure would dangle, so sema rejects it.
bool closureHasByRefCapture = false;
bool closureCapturesThis = false;
```

- [ ] **Step 4: Populate the flags in lambda sema**

In `src/sema/SemanticAnalyzer.cpp` `visit(LambdaExprAST&)`, when setting `closure->isClosure` etc. (step 6 of that function), add:

```cpp
closure->closureHasByRefCapture =
    std::any_of(effective.begin(), effective.end(),
                [](const LambdaExprAST::Capture& c) { return c.byRef; });
closure->closureCapturesThis = capturesThis;
```

`effective` must still be alive at this point; if it was moved earlier, compute/store the two booleans before `node.captures = std::move(effective);`.

- [ ] **Step 5: Add and call the return-escape check**

In `SemanticAnalyzer.h`, declare:

```cpp
void checkClosureReturnEscape(Type* returnValueType, const ASTNode& at);
```

In `SemanticAnalyzer.cpp`, implement:

```cpp
void SemanticAnalyzer::checkClosureReturnEscape(Type* returnValueType,
                                                const ASTNode& at) {
    Type* t = stripTypedef(returnValueType);
    if (!t || t->kind != TypeKind::Struct) return;
    auto* closure = static_cast<StructType*>(t);
    if (!closure->isClosure) return;
    if (closure->closureCapturesThis) {
        emitError("capturing closure with a 'this' capture cannot escape by return", at);
        return;
    }
    if (closure->closureHasByRefCapture) {
        emitError("capturing closure with by-reference captures cannot escape by return", at);
    }
}
```

Call it in `visit(ReturnStmtAST&)` for every value return, before type matching:
- in the Task 1 auto branch, after `retValType` is known and before storing `m_inferredAutoReturn`;
- in the generic `if (node.value)` path, before `typesCompatible`.
Do not call it for bare `return;`.

- [ ] **Step 6: Run the focused tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E\.(ByReferenceClosure|IndirectByReferenceClosure)' --output-on-failure
```

Expected: all three tests pass; local by-reference closure calls still work.

- [ ] **Step 7: Commit**

```bash
git add src/ast/Type.h src/sema/SemanticAnalyzer.h src/sema/SemanticAnalyzer.cpp tests/e2e/test_lambda.cpp
git commit -m "feat(sema): reject by-reference closure escape by return"
```

---

### Task 3: Return and call by-value capturing closures

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp` (only if the new tests expose a bug)
- Modify: `src/ast/Decl.cpp` / `src/ast/Stmt.cpp` (only if the new tests expose a codegen bug)
- Test: `tests/e2e/test_lambda.cpp`

**Interfaces:**
- Consumes: Task 1 auto return inference; Task 2 capture flags; existing closure `StructType` call path and normal struct return ABI.
- Produces: working `auto make(int32 x) { return [x](...) ...; }` escape and `auto f = make(...); f(...);` calls.

- [ ] **Step 1: Write the failing tests**

Add to `tests/e2e/test_lambda.cpp`:

```cpp
TEST_F(LambdaE2E, ByValueClosureReturn) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        int32 main() {
            auto f = make_adder(40);
            return f(2) - 42;
        }
    )", "auto_ret_closure.cmm"), 0);
}

TEST_F(LambdaE2E, ReturnedClosureIsIndependentCopy) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        int32 main() {
            auto add40 = make_adder(40);
            auto add2 = make_adder(2);
            return add40(2) + add2(2) - 46;
        }
    )", "auto_ret_closure_copy.cmm"), 0);
}

TEST_F(LambdaE2E, ReturnedClosureCalledRepeatedly) {
    EXPECT_EQ(runSource(R"(
        auto make_counter(int32 start) {
            return [start]() -> int32 { return start + 1; };
        }
        int32 main() {
            auto f = make_counter(10);
            if (f() != 11) return 1;
            if (f() != 11) return 2;
            return 0;
        }
    )", "auto_ret_closure_repeat.cmm"), 0);
}

TEST_F(LambdaE2E, ReturnedClosureFromBranch) {
    EXPECT_EQ(runSource(R"(
        auto make(int32 x) {
            auto f = [x](int32 y) -> int32 { return x + y; };
            if (x > 0) return f;
            return f;
        }
        int32 main() {
            auto plus = make(10);
            auto minus = make(-10);
            return plus(3) + minus(3) - 6;
        }
    )", "auto_ret_closure_branch.cmm"), 0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E\.(ByValueClosureReturn|ReturnedClosure)' --output-on-failure
```

Expected: fail before the codegen/type path is complete; the failure should be a wrong result or verifier/return mismatch, not a parser error.

- [ ] **Step 3: Fix only what the failures show**

Likely fixes, in order:
- If sema says no matching closure call, ensure the inferred function return type is the closure `StructType` and that `auto f = make(...)` stores it unchanged.
- If LLVM verifier rejects the return, make `ReturnStmtAST` use the closure struct as the function return type and let the existing `castValue` handle the by-value struct copy.
- If the closure call after return cannot find `__lambda_N__call`, ensure it is emitted in the module before the caller's codegen (the existing lambda expression codegen owns this).

Do not add a new ABI, heap allocation, or closure type name.

- [ ] **Step 4: Run the focused tests**

```bash
ctest --test-dir build -R 'LambdaE2E\.(ByValueClosureReturn|ReturnedClosure)' --output-on-failure
```

Expected: all four pass.

- [ ] **Step 5: Commit**

```bash
git add src/sema/SemanticAnalyzer.cpp src/ast/Decl.cpp src/ast/Stmt.cpp tests/e2e/test_lambda.cpp
git commit -m "feat(codegen): support returning by-value capturing closures"
```

(If no production file changed, commit only the test file with the same message.)

---

### Task 4: Module type-export regression coverage

**Files:**
- Test: `tests/driver/test_module_loader.cpp`

**Interfaces:**
- Consumes: existing `preloadImports`, `Parser`, `SemanticAnalyzer`, `jitRun`.
- Produces: regression proof that namespace-exported struct/class/template types with methods, statics, and inheritance remain usable across modules.

- [ ] **Step 1: Write the regression test**

Add after `ModuleLoaderTest.ExportedTypesVisibleBeforeParsingImporter`:

```cpp
TEST_F(ModuleLoaderTest, ExportedNamespaceTypesWithMethodsTemplatesAndInheritance) {
    const fs::path dir = fs::temp_directory_path() / "cmm_module_namespace_types_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "api_types.cmm");
        out << "module api_types;\n"
               "export namespace api {\n"
               "    struct Point {\n"
               "        int32 x; int32 y;\n"
               "        int32 sum() { return this->x + this->y; }\n"
               "    };\n"
               "    class Box {\n"
               "    public:\n"
               "        int32 v;\n"
               "        int32 get() { return this->v; }\n"
               "        static int32 twice(int32 x) { return x * 2; }\n"
               "    };\n"
               "    template<typename T> struct Wrap {\n"
               "        T v;\n"
               "        T get() { return this->v; }\n"
               "    };\n"
               "    class Base { public: int32 b; int32 basev() { return this->b; } };\n"
               "    class Derived : public Base {\n"
               "    public:\n"
               "        int32 d;\n"
               "        int32 total() { return this->b + this->d; }\n"
               "    };\n"
               "}\n";
    }

    const std::string mainSource = R"(
        module main;
        import api_types;
        int32 main() {
            api::Point p; p.x = 1; p.y = 2;
            api::Box b; b.v = 4;
            api::Wrap<int32> w; w.v = 5;
            api::Derived d; d.b = 6; d.d = 7;
            int32 r = p.sum() + b.get() + api::Box::twice(3) + w.get() + d.total();
            return r - 31;
        }
    )";
    const std::string mainPath = (dir / "main.cmm").string();
    Lexer lexer(mainPath, mainSource);
    auto tokens = lexer.tokenize();
    ASSERT_TRUE(lexer.getDiagnostics().empty());

    std::set<std::string> loaded;
    std::vector<std::unique_ptr<DeclAST>> imported;
    std::vector<std::string> errors;
    const auto specs = cmm::collectImportSpecifiers(tokens);
    ASSERT_EQ(specs.size(), 1u);
    cmm::preloadImports(specs, dir.string(), cmm::ModuleSearchPaths{{dir.string()}},
                        loaded, imported, errors);
    for (const auto& e : errors) ADD_FAILURE() << e;
    ASSERT_TRUE(errors.empty());

    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_NE(ast, nullptr);
    for (const auto& d : parser.getErrors()) ADD_FAILURE() << d.formatWithSeverity();
    ASSERT_TRUE(parser.getErrors().empty());

    ast->declarations.insert(ast->declarations.begin(),
                             std::make_move_iterator(imported.begin()),
                             std::make_move_iterator(imported.end()));
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    for (const auto& e : analyzer.getErrors()) ADD_FAILURE() << e.formatWithSeverity();
    ASSERT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
    fs::remove_all(dir);
}
```

- [ ] **Step 2: Run the test**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'ModuleLoaderTest\.ExportedNamespaceTypesWithMethodsTemplatesAndInheritance' --output-on-failure
```

Expected: PASS. This test is a regression lock for behavior already present at HEAD; if it fails, stop and fix the module visibility/type registration path exposed by the failure before continuing.

- [ ] **Step 3: Commit**

```bash
git add tests/driver/test_module_loader.cpp
git commit -m "test(modules): lock namespace type export with methods and templates"
```

---

### Task 5: Documentation, example, and full verification

**Files:**
- Modify: `README.md`
- Modify: `docs/spec/semantics.md`
- Modify: `docs/spec/keywords.md`
- Modify: `docs/superpowers/specs/2026-11-11-closure-escape-design.md` (only if implementation differs)
- Modify: `TODO.md`
- Modify: `Progress.md`
- Optionally modify: `examples/all_features.cmm`

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: accurate documented behavior; final green suite.

- [ ] **Step 1: Update the README limitation**

Replace the `capturing-closure escape` phrase in `README.md` with an accurate current statement:

```text
By-value capturing closures can escape by returning them from an `auto`-return
function. Returning a closure with by-reference or `this` captures is rejected
at compile time. Storing anonymous closure types in struct fields/arrays remains
unsupported.
```

Keep the rest of the limitations sentence intact.

- [ ] **Step 2: Update the language spec**

In `docs/spec/semantics.md` §lambda:
- Document `auto` return deduction for free functions: first value `return` wins, arrays decay, bare/`return;` rules, source-order requirement, no recursion/mutual recursion.
- Change the “有捕获闭包逃逸不保证” paragraph to: by-value closure return is safe and copies the closure struct; by-reference/`this` closure return is a compile error. Struct fields/arrays/ordinary non-template parameters remain unsupported because the closure type is anonymous.

In `docs/spec/keywords.md`, update the `auto` row to include free-function return-type deduction.

If `docs/spec/grammar.ebnf` already accepts `auto` as a type, add only an `[impl]` note; do not change grammar productions.

- [ ] **Step 3: Update TODO and Progress**

Add a dated `2026-11-11` snapshot at the top of `TODO.md` stating:
- `auto` return-type deduction for free functions is implemented.
- By-value capturing closure return works.
- By-reference/`this` closure return is rejected.
- Module namespace type export was verified and regression-locked; `std.mem` `Allocator`/`Arena`/`Pool` and `std.vec` imported use compile/run.
- Baseline and final test counts.

Append a matching chronological entry to `Progress.md` with the design/plan paths, files changed, diagnostics, tests, and verification commands.

- [ ] **Step 4: Optionally extend the feature tour**

If the change fits without disrupting the existing output, add a small closure-return section to `examples/all_features.cmm` that prints a known value and keeps the final line `ALL FEATURES OK`. If it risks the tour, skip this step and note why in `Progress.md`.

- [ ] **Step 5: Build and run the focused suite**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E\.(AutoReturn|ByValueClosureReturn|ReturnedClosure|ByReferenceClosure|IndirectByReferenceClosure)' --output-on-failure
ctest --test-dir build -R 'ModuleLoaderTest\.ExportedNamespaceTypesWithMethodsTemplatesAndInheritance' --output-on-failure
```

Expected: all pass.

- [ ] **Step 6: Run the full suite**

```bash
ctest --test-dir build -j8 --output-on-failure
```

Expected: **1865 + new tests** all pass, 0 failures.

- [ ] **Step 7: Run the feature tour**

```bash
build/bin/cmm examples/all_features.cmm -o /tmp/all_features
/tmp/all_features
```

Expected last line: `ALL FEATURES OK`.

- [ ] **Step 8: Commit**

```bash
git add README.md docs/spec/semantics.md docs/spec/keywords.md TODO.md Progress.md
git add examples/all_features.cmm 2>/dev/null || true
git add docs/superpowers/specs/2026-11-11-closure-escape-design.md
git commit -m "docs: document capturing-closure escape and auto return deduction"
```

- [ ] **Step 9: Final verification**

Run `git status --short` and confirm only ignored build artifacts or nothing is left. Report the exact full-suite count and feature-tour output.

---

## Self-Review Notes

- Spec coverage: `auto` return deduction (Task 1), by-value escape (Task 3), by-reference/`this` rejection (Task 2), module regression (Task 4), docs/verification (Task 5).
- The `this` flag is implemented and checked even though v1’s free-function-only restriction makes a direct `this`-escape test syntax-unreachable; the by-reference test exercises the same return check.
- Closure types are nominal: a `return` branch test must return the same closure variable, not two independently created lambda literals (they carry different call functions).
- Deliberately out of scope: struct-field/array closure storage, ordinary function parameters of anonymous closure type, recursive `auto` functions, and auto-return methods/template bodies/CT functions.
