# Closure Escape and First-Class Storage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Allow by-reference/`this` closure returns with a lifetime warning, and make nominal closure types usable in aliases, struct fields, arrays, and ordinary parameters.

**Architecture:** Keep the existing closure struct ABI. Add `decltype(expr)` as a type-only query, infer `auto` array elements from their brace initializer, and lower closure calls through arbitrary lvalue/rvalue expressions. Auto-return instance methods become supported so a `this` closure can actually escape. No heap allocation, reference counting, type erasure, or by-value copy-semantics change.

**Tech Stack:** C++20, LLVM 22, CMake, GoogleTest/CTest, CDD parser/sema/codegen.

**Spec:** `docs/superpowers/specs/2026-11-12-closure-first-class-design.md`

## Global Constraints

- By-reference/`this` closure returns are allowed, with warning `W3006`; captured storage lifetime is the caller's responsibility.
- No hidden runtime allocation or cleanup.
- Closure types remain nominal; `decltype` only names an existing nominal type.
- `auto` arrays infer element type from the first brace initializer.
- Auto-return instance methods are supported; static auto methods are not.
- Public by-value closure escape and local by-ref closure calls keep working.
- Do not modify `thirdparty/`.
- Run `ctest --test-dir build -j8 --output-on-failure` after each implementation task.

## Review Focus

- **Escape checks removed but warning added:** no hard error for by-ref/`this`; W3006 appears once per escaping return.
- **`decltype` resolves without evaluating:** call expressions in `decltype` must not emit code or twice-analyze side effects.
- **Class method table updated after auto inference:** `c.get()` must see the closure return type, not `auto`.
- **Array element inference ordering:** element type must be known before `ArrayType` construction and initializer-list checking.
- **Closure expression callee lowering:** lvalues pass their address; rvalues get a temporary alloca, never a load of the struct value as a function pointer.
- **Existing diagnostics:** method prototype, static method, capture, dangling-return, and by-value closure tests stay green.

---

### Task 1: Add failing tests for the new surface

**Files:**
- Modify: `tests/e2e/test_lambda.cpp`
- Modify: `tests/sema/test_warnings.cpp`
- Modify: `tests/frontend/test_parser.cpp` (decltype parse coverage)

**Interfaces:**
- Consumes: existing `runSource`, `compileErrors`, `WarningAnalysisTest::analyze`.
- Produces: red tests for escapes, `decltype`, auto arrays, expression calls, auto methods.

- [x] **Step 1: Add positive escape tests**

Add to `tests/e2e/test_lambda.cpp`:

```cpp
TEST_F(LambdaE2E, ByReferenceClosureReturnAllowed) {
    EXPECT_EQ(runSource(R"(
        auto make_ref() {
            int32 x = 7;
            return [&x]() -> int32 { return x; };
        }
        int32 main() { auto f = make_ref(); return 0; }
    )", "auto_ret_ref_ok.cdd"), 0);
}

TEST_F(LambdaE2E, IndirectByReferenceClosureReturnAllowed) {
    EXPECT_EQ(runSource(R"(
        auto make_ref() {
            int32 x = 7;
            auto f = [&x]() -> int32 { return x; };
            return f;
        }
        int32 main() { auto f = make_ref(); return 0; }
    )", "auto_ret_ref_indirect_ok.cdd"), 0);
}

TEST_F(LambdaE2E, NestedByReferenceClosureReturnAllowed) {
    EXPECT_EQ(runSource(R"(
        auto make_ref() {
            int32 y = 7;
            auto inner = [&y]() -> int32 { return y; };
            auto outer = [inner]() -> int32 { return inner(); };
            return outer;
        }
        int32 main() { auto f = make_ref(); return 0; }
    )", "auto_ret_nested_ref_ok.cdd"), 0);
}
```

Remove the three old `*Rejected` tests for these cases.

- [x] **Step 2: Add this-escape and auto-method tests**

```cpp
TEST_F(LambdaE2E, AutoReturnInstanceMethod) {
    EXPECT_EQ(runSource(R"(
        class C { public: auto twice(int32 x) { return x * 2; } };
        int32 main() { C c; return c.twice(21) - 42; }
    )", "auto_ret_instance_method.cdd"), 0);
}

TEST_F(LambdaE2E, ThisClosureReturnAllowed) {
    EXPECT_EQ(runSource(R"(
        class C {
        public:
            int32 x;
            auto get() { return [this]() -> int32 { return x; }; }
        };
        int32 main() {
            C c;
            c.x = 7;
            auto f = c.get();
            return f() - 7;
        }
    )", "auto_ret_this_ok.cdd"), 0);
}
```

Update `AutoReturnMethodRejected` to the positive test above. Change
`AutoReturnOutOfClassStaticRejected` to expect
`not supported on static methods`. Change `AutoReturnMethodPrototypeDiagnostic`
to expect `must have a body`.

- [x] **Step 3: Add decltype field/array tests**

```cpp
TEST_F(LambdaE2E, DecltypeClosureField) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        using Adder = decltype(make_adder(0));
        struct Box { Adder f; };
        int32 main() {
            Box b = { make_adder(40) };
            return b.f(2) - 42;
        }
    )", "closure_field_decltype.cdd"), 0);
}

TEST_F(LambdaE2E, AutoArrayOfClosures) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        int32 main() {
            auto a[2] = { make_adder(10), make_adder(20) };
            return a[0](1) + a[1](1) - 32;
        }
    )", "closure_array_auto.cdd"), 0);
}

TEST_F(LambdaE2E, AutoArrayOfClosuresUnsized) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        int32 main() {
            auto a[] = { make_adder(10), make_adder(20) };
            return a[0](1) + a[1](1) - 32;
        }
    )", "closure_array_auto_unsized.cdd"), 0);
}
```

- [x] **Step 4: Add rvalue/chained closure call coverage**

```cpp
TEST_F(LambdaE2E, ReturnedClosureImmediateCall) {
    EXPECT_EQ(runSource(R"(
        auto make_adder(int32 x) {
            return [x](int32 y) -> int32 { return x + y; };
        }
        int32 main() { return make_adder(40)(2) - 42; }
    )", "closure_rvalue_call.cdd"), 0);
}
```

- [x] **Step 5: Add W3006 test**

Add to `tests/sema/test_warnings.cpp`:

```cpp
TEST_F(WarningAnalysisTest, WarnsWhenByRefClosureEscapes) {
    auto r = analyze(R"(
        auto make_ref() {
            int32 x = 7;
            return [&x]() -> int32 { return x; };
        }
        int32 main() { return 0; }
    )");
    ASSERT_TRUE(r.errors.empty());
    EXPECT_EQ(r.count(DiagnosticCode::WarnClosureEscape), 1);
    EXPECT_TRUE(r.mentions(DiagnosticCode::WarnClosureEscape, "by-reference"));
}
```

- [x] **Step 6: Add decltype parser test**

Add to `tests/frontend/test_parser.cpp` a test that parses:

```cpp
using T = decltype(make(1));
```

and asserts no parser errors and the alias exists. Use the existing parser test
fixture patterns in that file.

- [x] **Step 7: Build and confirm failures**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E|WarningAnalysisTest|ParserTest' --output-on-failure
```

Expected: new tests fail on parse/sema/codegen; old rejection tests removed or
updated.

---

### Task 2: Implement `decltype` and auto array inference

**Files:**
- Modify: `src/frontend/Token.h`
- Modify: `src/frontend/Lexer.cpp`
- Modify: `src/ast/Type.h`
- Modify: `src/frontend/Parser.cpp`
- Modify: `src/frontend/Parser.h` if a helper declaration is needed.
- Modify: `src/sema/SemanticAnalyzer.cpp`
- Modify: `src/sema/TemplateInstantiator.cpp` (reject or preserve `Decltype`)
- Modify: `src/sema/Diagnostic.h`
- Modify: `src/sema/Diagnostic.cpp`
- Modify: `src/sema/SemanticAnalyzer.h` if a guard member/helper is needed.

**Interfaces:**
- Produces: `TypeKind::Decltype`, `DecltypeType`, `TOKEN_DECLTYPE`.
- Produces: auto-array element inference in `visit(ArrayDeclAST)`.
- Produces: `WarnClosureEscape` diagnostic code.

- [x] **Step 1: Add token and keyword**

Add `TOKEN_DECLTYPE` beside `TOKEN_AUTO`; map `"decltype"` in `keywordMap`;
add it to `Parser::isTypeStart()`.

- [x] **Step 2: Add the type node**

```cpp
class DecltypeType : public Type {
public:
    std::shared_ptr<ExprAST> expr;
    explicit DecltypeType(std::shared_ptr<ExprAST> e)
        : Type(TypeKind::Decltype), expr(std::move(e)) {}
};
```

Add `Decltype` to `TypeKind`. Include `<memory>` is already present.

- [x] **Step 3: Parse `decltype(expr)`**

In `Parser::parseBaseType`:

```cpp
case TokenType::TOKEN_DECLTYPE: {
    advance();
    if (!expect(TokenType::TOKEN_LPAREN, "expected '(' after decltype")) return nullptr;
    auto expr = parseExpr();
    if (!expr) {
        errorUnexpected("expression in decltype");
        return nullptr;
    }
    if (!expect(TokenType::TOKEN_RPAREN, "expected ')' after decltype")) return nullptr;
    return new DecltypeType(std::shared_ptr<ExprAST>(std::move(expr)));
}
```

This must work in `use` aliases, fields, arrays, and parameters because they all
route through `parseType()`/`parseTypeId()`.

- [x] **Step 4: Resolve `decltype` in sema**

Add a `std::unordered_set<DecltypeType*> m_decltypeResolving;` member. In
`resolveTypeInstance` add:

```cpp
case TypeKind::Decltype: {
    auto* dt = static_cast<DecltypeType*>(t);
    if (!dt->expr) return nullptr;
    if (!m_decltypeResolving.insert(dt).second) {
        emitError("recursive decltype in type definition", at);
        return nullptr;
    }
    Type* resolved = getExprType(*dt->expr);
    m_decltypeResolving.erase(dt);
    if (!resolved) {
        emitError("cannot deduce a type from decltype expression", at);
        return nullptr;
    }
    return resolved;
}
```

Ensure `visit(UsingDeclAST)` resolves its aliased type before registering it so
a later field/local use sees the concrete type. Do not evaluate the expression
in codegen.

- [x] **Step 5: Add warning code**

Add `WarnClosureEscape` to `DiagnosticCode` after `WarnNullNonnull`; register
`W3006` with description `"closure captures storage that may not outlive it"`.

- [x] **Step 6: Infer auto array element types**

In `visit(ArrayDeclAST)`, before `checkNestedTypeAccess` / `ArrayType`
construction:

```cpp
if (node.elementType && stripTypedef(node.elementType)->kind == TypeKind::Auto) {
    auto* initList = dynamic_cast<InitializerListExprAST*>(node.initExpr.get());
    if (!initList || initList->initializers.empty()) {
        emitError("auto array '" + node.name +
                      "' requires a brace-enclosed initializer", node);
        node.elementType = typeCtx->getInt32();
    } else {
        Type* elem = getExprType(*initList->initializers[0]);
        if (!elem) {
            emitError("cannot infer auto array element type", node);
            elem = typeCtx->getInt32();
        }
        for (size_t i = 1; i < initList->initializers.size(); ++i) {
            Type* other = getExprType(*initList->initializers[i]);
            if (other && !typesCompatible(elem, other)) {
                emitError("auto array '" + node.name +
                              "' has incompatible element types: '" +
                              typeToString(elem) + "' vs '" + typeToString(other) + "'",
                          node);
            }
        }
        node.elementType = elem;
    }
}
```

Then continue existing size inference and `ArrayType` construction. Ensure the
initializer list is not analyzed twice in a way that mints a second closure type
(the lambda node caches its type).

- [x] **Step 7: Handle `Decltype` in type utilities**

Add `typeToString` case (`"<decltype>"` is acceptable if resolution normally
happens first). In `TemplateInstantiator::rewrite`, keep `Decltype` unchanged
and add a sema diagnostic when it survives template instantiation:
`decltype is not supported in template bodies`. This avoids silently evaluating
an expression against unbound type parameters.

- [x] **Step 8: Build and run focused tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E.Decltype|LambdaE2E.AutoArray|ParserTest' --output-on-failure
```

---

### Task 3: Allow by-ref/`this` escape and auto-return instance methods

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`
- Modify: `src/sema/SemanticAnalyzer.h` (signature rename if desired)
- Modify: `src/ast/Type.h` comments only if needed.
- Modify: `src/sema/Diagnostic.*` already in Task 2.

**Interfaces:**
- Consumes: `WarnClosureEscape`.
- Produces: warning instead of error for by-ref/`this` closure returns.
- Produces: auto-return instance methods and updated class method table.

- [x] **Step 1: Replace the hard escape error with a warning**

Rename `checkClosureReturnEscape` to `warnClosureReturnEscape` (or keep the
name but change its body). Emit:

```cpp
if (closure->closureCapturesThis) {
    emitWarning(DiagnosticCode::WarnClosureEscape,
                "returned closure captures 'this'; the object must outlive the closure", at);
    return;
}
if (closure->closureHasByRefCapture) {
    emitWarning(DiagnosticCode::WarnClosureEscape,
                "returned closure contains by-reference captures; captured storage must outlive the closure",
                at);
}
```

Keep the call in `visit(ReturnStmtAST)` before inference so every value return is
covered.

- [x] **Step 2: Permit instance-method auto returns**

In `visit(FunctionDeclAST)` replace the method rejection:

```cpp
if (isMethodDecl && node.isStatic) {
    emitError("auto return type is not supported on static methods", node);
    return;
}
if (!node.body) { ... } // existing body check
```

Do not reject non-static methods. Keep `compile_time`/`compile_only` rejection.

- [x] **Step 3: Update the class method table after inference**

Save `ClassType* autoMethodClass = currentClass;` before the body-first path.
After `node.returnType` is finalized and before/after symbol registration,
update the class's method entry:

```cpp
if (autoMethodClass) {
    if (FunctionType* ft = autoMethodClass->getMethod(node.name)) {
        ft->returnType = node.returnType;
    }
}
```

Use the bare method name at that point. Ensure out-of-class definitions bind the
same class pointer before inference. Static methods remain rejected.

- [x] **Step 4: Update diagnostics/tests**

- Keep `AutoReturnOutOfClassStaticRejected` but expect
  `not supported on static methods`.
- Change `AutoReturnMethodPrototypeDiagnostic` to expect `must have a body`.
- Ensure `ThisClosureReturnAllowed` runs without errors; warnings do not fail
  `runSource`.

- [x] **Step 5: Build and run focused tests**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E.Auto|LambdaE2E.This|LambdaE2E.ByReference|WarningAnalysisTest' --output-on-failure
```

---

### Task 4: Support closure calls through fields, array elements, and rvalues

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`
- Modify: `src/ast/Expr.cpp`

**Interfaces:**
- Consumes: expression callee ASTs and closure `StructType` metadata.
- Produces: `isClosureCall` for `isExprCallee` paths; codegen address lowering.

- [x] **Step 1: Factor closure-call checking**

Extract the existing stored-closure validation (arg count, compatibility,
`mangledCallee`, `resolvedParamTypes`, return type) into a helper used by both
the named-variable path and the expression-callee path. For expression callees:

```cpp
if (node.isExprCallee && node.calleeExpr) {
    Type* ct = stripTypedef(getExprType(*node.calleeExpr));
    if (ct && ct->kind == TypeKind::Struct &&
        static_cast<StructType*>(ct)->isClosure) {
        // validate args, set isClosureCall, mangledCallee, resolvedParamTypes, type
        return;
    }
    // existing function-pointer path
}
```

Keep the function-pointer diagnostic for non-closure aggregates.

- [x] **Step 2: Lower expression closure calls in codegen**

In `CallExprAST::codegen`, change the `isClosureCall` branch:

```cpp
llvm::Value* closureAddr = nullptr;
if (isExprCallee && calleeExpr) {
    if (calleeExpr->isLValue) {
        closureAddr = calleeExpr->codegen(ctx);
    } else {
        llvm::Value* v = calleeExpr->codegen(ctx);
        if (!v) return nullptr;
        auto& b = ctx.getBuilder();
        auto* tmp = b.CreateAlloca(ctx.getLLVMType(calleeExpr->type), nullptr,
                                   "closure.rvalue");
        b.CreateStore(v, tmp);
        closureAddr = tmp;
    }
} else {
    closureAddr = ctx.lookupVariableAddr(callee);
}
```

Then pass `closureAddr` as the hidden first argument. Do not run the
function-pointer load path for closure calls.

- [x] **Step 3: Verify field/array/rvalue cases**

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -R 'LambdaE2E.Decltype|LambdaE2E.AutoArray|LambdaE2E.ReturnedClosureImmediateCall' --output-on-failure
```

Expected: all pass, including `b.f(2)`, `a[0](1)`, and
`make_adder(40)(2)`.

---

### Task 5: Documentation and full verification

**Files:**
- Modify: `README.md`
- Modify: `docs/spec/semantics.md`
- Modify: `docs/spec/keywords.md`
- Modify: `docs/spec/grammar.ebnf`
- Modify: `TODO.md`
- Modify: `Progress.md`
- Add: this design/plan under `docs/superpowers/`.

**Interfaces:**
- Consumes: Tasks 1–4.
- Produces: current docs and progress snapshot.

- [x] **Step 1: Update README limitations**

Replace the current “by-reference/`this` rejected; anonymous closure types
stored in fields/arrays unsupported” paragraph with:

- by-value closure escape copies the struct;
- by-reference/`this` escape is allowed with W3006 and caller-managed lifetime;
- `decltype` names closure types for fields/arrays/aliases/parameters;
- `auto` arrays infer element types;
- auto-return instance methods supported.

- [x] **Step 2: Update semantics and keywords**

In `docs/spec/semantics.md` §7.5:
- change escape rules and warning text;
- add `decltype` and `auto` array rules;
- add expression closure-call rules;
- update auto-return method limitation to static-only.

In `docs/spec/keywords.md`, add `decltype` as a type query keyword and note
that the old `typeof` spelling remains removed by DEC-18. Update grammar if it
enumerates type specifiers.

- [x] **Step 3: Add TODO/Progress snapshot**

Record:
- W3006 and allowed by-ref/`this` escape;
- auto-return instance methods;
- `decltype` + auto arrays + expression closure calls;
- test count and feature-tour result;
- design/plan paths.

- [x] **Step 4: Full verification**

```bash
ctest --test-dir build -j8 --output-on-failure
build/bin/cdd examples/all_features.cdd -o /tmp/all_features
/tmp/all_features
git status --short
```

Expected: full suite green; `ALL FEATURES OK`; clean tree after commit.

- [x] **Step 5: Commit**

```bash
git add -A
git add -f docs/superpowers/specs/2026-11-12-closure-first-class-design.md \
           docs/superpowers/plans/2026-11-12-closure-first-class.md
git commit -m "feat(closure): allow escaping captures and first-class storage"
```

## Self-Review Notes

- `decltype` is deliberately type-only; no runtime expression evaluation.
- The escape change does not make dangling references safe; it makes the
  lifetime rule explicit through W3006 rather than a hard error.
- Auto arrays are homogeneous: all elements must have the same nominal closure
  type. Different lambda literals remain different types.
- The existing closure struct ABI is unchanged, so by-value escape and
  cross-function calls do not need new runtime support.
