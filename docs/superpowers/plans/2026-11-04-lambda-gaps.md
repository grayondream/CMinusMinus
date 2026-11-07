# Lambda 语言缺口补齐实施计划

> 状态：5 个任务全部完成（2026-11-04）。实现提交见 `git log --grep=PAR-11` / `--grep=CG-11`。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补齐 lambda 的 `[&]`、混排捕获、捕获初始化、`[this]` 隐式成员、未捕获变量诊断和返回类型推断，且不破坏既有 lambda 行为。

**Architecture:** 扩展 `LambdaExprAST::Capture` 保存捕获初始化表达式；parser 解析默认/覆盖/初始化；sema 用 lambda frame 栈统一处理捕获解析、`this` 脱糖和未捕获诊断；返回类型在 body 遍历中惰性推断；codegen 抽一个闭包构造 helper，立即调用改为 spill 返回的闭包值。

**Tech Stack:** C++ / LLVM / CMake / GoogleTest；构建 `cmake --build build -j$(nproc)`；测试 `ctest --test-dir build -j8`。

**Spec:** `docs/superpowers/specs/2026-11-04-lambda-gaps-design.md`

## Global Constraints

- 只改语言层 lambda；不碰 FFI、驱动链接、libclang。
- 语言无 `int/float/double`；测试用定宽类型，`int32 main()`。
- 既有 `[]`、`[=]`、`[a]`、`[&a]`、无捕获函数指针、`auto` 闭包、立即调用行为必须保持。
- 全局/函数内 static/thread_local/块级 extern 不进入闭包字段（c++ 语义；修当前 `[=]` 复制全局的 bug）。
- 按值捕获写入继续写闭包副本；本轮不引入 `mutable`，也不新增只读诊断。
- 有捕获闭包逃逸（函数返回、struct 字段、数组元素、普通函数参数）仍是非目标。
- 工作区已有未提交的容器迁移改动（`Progress.md`、`TODO.md`、`libs/std/{vec,deque,hashmap,hashset}.cmm` 等）；提交时只 `git add` 本计划涉及的文件，绝不 stage 这些既有改动。
- 每任务先写失败测试，RED→GREEN，再提交；提交信息引用 `PAR-11`。

## Review Focus

- **未捕获外层变量**：嵌套 lambda 内层 `[]` 引用外层局部/捕获变量时，必须 sema 报错，不能段错误。
- **全局/静态捕获**：`[=]` 不再把全局/static 拷进闭包；写全局必须在闭包外可见。
- **捕获初始化求值次数**：`[x = f()]` 在立即调用路径只求值一次。
- **`this` 隐式成员**：`[this]` 与 `[=]` 内直接写 `v` 必须解析为 `this->v`；`[]` 内不解糖。
- **返回推断顺序**：`return` 引用 body 内先声明的局部/`auto` 闭包时必须可用；`if`/`switch` 内 return 必须推断出类型。

---

### Task 1: AST + Parser 捕获语法

**Files:**
- Modify: `src/ast/Expr.h`（`LambdaExprAST::Capture`、`captureAllByRef`）
- Modify: `src/frontend/Parser.cpp`（`parseLambdaExpr`）
- Test: `tests/frontend/test_parser.cpp`

**Interfaces:**
- Produces:
  - `struct LambdaExprAST::Capture { std::string name; bool byRef; std::unique_ptr<ExprAST> init; }`
  - `bool LambdaExprAST::captureAllByRef`
  - parser 接受 `[&]`、`[=, &x]`、`[&, x]`、`[x = expr]`、`[&x = expr]`、`[this]`

- [x] **Step 1: 写失败 parser 测试**

在 `tests/frontend/test_parser.cpp` 的 PAR-11 区域添加：

```cpp
TEST(ParserLambdaTest, ParsesCaptureAllByRef) {
    auto expr = parseExpr("[&](int32 x) { return x; }");
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    EXPECT_TRUE(lam->captureAll);
    EXPECT_TRUE(lam->captureAllByRef);
}

TEST(ParserLambdaTest, ParsesMixedCaptureDefaults) {
    auto byVal = dynamic_cast<LambdaExprAST*>(
        parseExpr("[=, &a, b](int32 x) { return x; }"));
    ASSERT_NE(byVal, nullptr);
    EXPECT_TRUE(byVal->captureAll);
    EXPECT_FALSE(byVal->captureAllByRef);
    ASSERT_EQ(byVal->captures.size(), 2u);
    EXPECT_TRUE(byVal->captures[0].byRef);
    EXPECT_FALSE(byVal->captures[1].byRef);

    auto byRef = dynamic_cast<LambdaExprAST*>(
        parseExpr("[&, a, &b](int32 x) { return x; }"));
    ASSERT_NE(byRef, nullptr);
    EXPECT_TRUE(byRef->captureAll);
    EXPECT_TRUE(byRef->captureAllByRef);
    ASSERT_EQ(byRef->captures.size(), 2u);
    EXPECT_FALSE(byRef->captures[0].byRef);
    EXPECT_TRUE(byRef->captures[1].byRef);
}

TEST(ParserLambdaTest, ParsesCaptureInitializers) {
    auto expr = parseExpr("[x = 1 + 2, &y = z](int32 a) { return a; }");
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    ASSERT_EQ(lam->captures.size(), 2u);
    EXPECT_EQ(lam->captures[0].name, "x");
    EXPECT_FALSE(lam->captures[0].byRef);
    ASSERT_NE(lam->captures[0].init, nullptr);
    EXPECT_EQ(lam->captures[1].name, "y");
    EXPECT_TRUE(lam->captures[1].byRef);
    ASSERT_NE(lam->captures[1].init, nullptr);
}

TEST(ParserLambdaTest, ParsesThisCapture) {
    auto expr = parseExpr("[this](int32 x) { return x; }");
    auto lam = dynamic_cast<LambdaExprAST*>(expr);
    ASSERT_NE(lam, nullptr);
    ASSERT_EQ(lam->captures.size(), 1u);
    EXPECT_EQ(lam->captures[0].name, "this");
    EXPECT_FALSE(lam->captures[0].byRef);
}
```

- [x] **Step 2: 运行测试确认 RED**

Run: `build/bin/compiler_tests --gtest_filter='ParserLambdaTest.*'`
Expected: 新测试失败（`[&]`/混排/初始化解析报错）。

- [x] **Step 3: 实现 AST 与 parser**

`src/ast/Expr.h` 中：

```cpp
struct Capture {
    std::string name;
    bool byRef = false;
    std::unique_ptr<ExprAST> init;  // 非空 = `[x = expr]` / `[&x = expr]`
};
...
bool captureAll = false;
bool captureAllByRef = false;
```

`parseLambdaExpr()` 的 capture 解析改为：

- `[` 后先看 `]`。
- `=` 开头：`captureAll=true`；若后面 `,` 则继续解析 override-list，否则要求 `]`。
- `&` 开头：若 `&` 后是 `]` 或 `,`，是 `[&]` 默认按引用；否则是显式 `&name`。
- 显式项：可选 `&`，必须 identifier；若匹配 `=`，`init = parseExpr(2)`（`minPrec=2` 防止吃掉逗号）。
- `this` 作为普通 identifier 捕获名；`[&this]` / `[this = expr]` 留给 sema 诊断。
- 保留 `captureAll` 字段供既有测试。

- [x] **Step 4: 运行 parser 测试确认 GREEN**

Run: `build/bin/compiler_tests --gtest_filter='ParserLambdaTest.*'`
Expected: PASS。

- [x] **Step 5: 提交**

```bash
git add src/ast/Expr.h src/frontend/Parser.cpp tests/frontend/test_parser.cpp
git commit -m "feat(PAR-11): parse lambda capture defaults, overrides, and initializers"
```

---

### Task 2: Sema 捕获模型、`this` 与未捕获诊断

**Files:**
- Modify: `src/ast/Symbol.h`（`Symbol::isStaticStorage`）
- Modify: `src/ast/Expr.h`（`VariableExprAST::implicitThisAccess`）
- Modify: `src/sema/SemanticAnalyzer.h`（lambda frame、helper 声明）
- Modify: `src/sema/SemanticAnalyzer.cpp`（捕获解析、`this` 脱糖、未捕获检查、collectUsedNames 扩展）
- Test: `tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 1 的 `Capture::init`、`captureAllByRef`。
- Produces:
  - `Symbol::isStaticStorage`
  - `VariableExprAST::implicitThisAccess`
  - `SemanticAnalyzer::LambdaFrame` 栈（bodyScope、capturedNames、capturesThis、inferReturn、inferredReturn、pendingVoidReturns）
  - `Symbol* findCapturableLocal(const std::string&)`
  - `ClassType* findFieldOwner(ClassType*, const std::string&)`

- [x] **Step 1: 写失败 sema 测试**

在 `tests/sema/test_semantic_analyzer.cpp` 添加（复用现有 Lexer→Parser→SemanticAnalyzer 管线）：

```cpp
TEST(LambdaSemaGaps, RejectsUncapturedOuterLocal) {
    std::string source = R"(
        int32 main() {
            int32 x = 1;
            auto outer = [&x]() -> int32 {
                auto inner = []() -> int32 { return x; };
                return inner();
            };
            return outer();
        }
    )";
    // parse + analyze
    // EXPECT_FALSE(errors.empty());
    // EXPECT_NE(find "not captured", npos);
}

TEST(LambdaSemaGaps, CaptureInitializerInfersType) {
    std::string source = R"(
        int32 main() {
            auto f = [x = 41]() -> int32 { return x + 1; };
            return f() == 42 ? 0 : 1;
        }
    )";
    // EXPECT_TRUE(errors.empty());
}

TEST(LambdaSemaGaps, CaptureInitializerByRefRequiresLValue) {
    std::string source = R"(
        int32 main() {
            auto f = [&x = 1]() -> int32 { return x; };
            return 0;
        }
    )";
    // EXPECT_FALSE(errors.empty());
    // EXPECT_NE(find "lvalue" / "左值", npos);
}

TEST(LambdaSemaGaps, ThisImplicitMemberResolves) {
    std::string source = R"(
        class C {
        public:
            int32 v;
            int32 f() {
                auto g = [this]() -> int32 { return v; };
                return g();
            }
        };
    )";
    // EXPECT_TRUE(errors.empty());
}

TEST(LambdaSemaGaps, ThisCaptureOutsideMethodRejected) {
    std::string source = R"(
        int32 main() {
            auto f = [this]() -> int32 { return 1; };
            return 0;
        }
    )";
    // EXPECT_FALSE(errors.empty());
}

TEST(LambdaSemaGaps, CaptureAllDoesNotCaptureGlobals) {
    std::string source = R"(
        int32 g = 5;
        int32 main() {
            auto f = [=]() -> int32 { return g; };
            return f() == 5 ? 0 : 1;
        }
    )";
    // EXPECT_TRUE(errors.empty());
}
```

- [x] **Step 2: 运行测试确认 RED**

Run: `build/bin/compiler_tests --gtest_filter='LambdaSemaGaps.*'`
Expected: 未捕获用例可能段错误/通过错误；`[x = 41]` 解析或 sema 失败；`[this] + v` 报 undeclared。

- [x] **Step 3: 实现 Symbol 标记与捕获解析**

`Symbol` 加 `bool isStaticStorage = false;`。在 `visit(VarDeclAST&)` / `visit(ArrayDeclAST&)` 的 `declareSymbol` 之后设置：

```cpp
declaredVar->isStaticStorage =
    atGlobalLevel() || node.isStaticLocal || node.isThreadLocal || node.isExternLocal;
```

`findCapturableLocal(name)` 从 `currentScope` 向上查，遇到 `globalScope`/`activeModuleScope` 停止；返回第一个非 `isStaticStorage` 的 Symbol。

重写 `visit(LambdaExprAST&)` 的捕获阶段：

- `captureAll`：
  - `collectUsedNamesStmt` 扫描 body；排序后逐个处理。
  - 跳过参数名；只捕获 `findCapturableLocal` 找到的符号。
  - `byRef = node.captureAllByRef`。
  - 方法内若 used 含 `this`，或某个 used 名字不是任何符号但 `findFieldOwner(currentClass, name)` 命中，追加按值 `this` 捕获。
- 显式捕获：
  - `init` 非空：在当前外层作用域 `getExprType(*init)`；byRef 要求 `init->isLValue`，字段类型 `T*`；否则字段类型 `T`。
  - 无 `init`：`findCapturableLocal`；`this` 特殊处理。
  - `this`：`currentClass` 必须非空；byRef → 诊断；init → 诊断；字段类型为 `this` 符号类型（`Class*`）。
  - 重复捕获名 → 诊断。
- 闭包字段与 effective captures 同步构建；无效项不加入字段。
- `node.captures = std::move(effective)`；`node.closureType` / `closureName` 按现有逻辑。
- 保存 `capturesThis` 供 body 分析。

扩展 `collectUsedNames` 对 `LambdaExprAST` 的处理：除了 body，还扫描每个 capture 的 `name`（`this` 保留）和 `init` 表达式。

- [x] **Step 4: 实现 body 声明、`this` 脱糖与未捕获检查**

- 进入 lambda body scope 后，按 effective captures 声明名字：
  - `byRef`（非 `this`）声明 pointee 类型；`this` 声明 `Class*`；`byValue` 声明字段类型。
  - 与参数重名 → 诊断，跳过。
- 推入 `LambdaFrame{bodyScope, capturedNames, capturesThis, ...}`；body 分析后弹出。
- `visit(VariableExprAST&)`：
  - 先按现有逻辑查符号。
  - 若未找到且最内层 frame `capturesThis` 且 `currentClass` 有该字段：
    - 构造 `MemberAccessExprAST(Arrow, VariableExprAST("this"), name)`；
    - `visit(*member)`；把结果存入 `node.implicitThisAccess`；
    - 复制 `type`/`isLValue` 并返回。
  - 若找到符号，判断它是否在当前 lambda body scope 内：
    - 在 body scope 或子块 → 允许；
    - 在 global/module 或 `isStaticStorage` → 允许；
    - 否则必须在该 frame 的 `capturedNames` 中；缺则 `emitError("variable 'x' is not captured by lambda", node)`。
  - 嵌套 lambda 从最内层 frame 向外检查；内层未捕获即报错。
- `findFieldOwner` 沿 `ClassType::base` 链查找字段（深度上限 64），命中返回定义类；`this` 脱糖走 `visit(MemberAccessExprAST)`，访问控制复用现有逻辑。

- [x] **Step 5: 运行 sema 测试确认 GREEN**

Run: `build/bin/compiler_tests --gtest_filter='LambdaSemaGaps.*:ParserLambdaTest.*'`
Expected: PASS（未捕获用例是干净诊断，不崩溃）。

- [x] **Step 6: 提交**

```bash
git add src/ast/Symbol.h src/ast/Expr.h src/sema/SemanticAnalyzer.h src/sema/SemanticAnalyzer.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(PAR-11): lambda capture semantics, this capture, and uncaptured diagnostics"
```

---

### Task 3: Sema 返回类型惰性推断

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit(ReturnStmtAST&)`、`visit(LambdaExprAST&)`）
- Test: `tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 2 的 `LambdaFrame.inferReturn/inferredReturn/pendingVoidReturns`。
- Produces: 省略返回类型的 lambda 在 body 遍历中推断；`return` 可引用先声明的局部/`auto` 闭包。

- [x] **Step 1: 写失败 sema 测试**

```cpp
TEST(LambdaSemaGaps, ReturnInferenceSeesBodyLocal) {
    std::string source = R"(
        int32 main() {
            auto f = []() {
                int32 y = 1;
                return y;
            };
            return f() == 1 ? 0 : 1;
        }
    )";
    // EXPECT_TRUE(errors.empty());
}

TEST(LambdaSemaGaps, ReturnInferenceCoversIfBranch) {
    std::string source = R"(
        int32 main() {
            int32 c = 1;
            auto f = [c](int32 x) {
                if (x > 0) { return c + x; }
                return c;
            };
            return f(1) == 2 ? 0 : 1;
        }
    )";
    // EXPECT_TRUE(errors.empty());
}

TEST(LambdaSemaGaps, ReturnInferenceCoversOnlyBranch) {
    std::string source = R"(
        int32 main() {
            auto f = [](int32 x) {
                if (x > 0) { return 1; }
            };
            return f(1) == 1 ? 0 : 1;
        }
    )";
    // EXPECT_TRUE(errors.empty());
}

TEST(LambdaSemaGaps, ReturnInferenceVoidWithoutValueReturn) {
    std::string source = R"(
        int32 main() {
            auto f = []() { int32 x = 1; };
            f();
            return 0;
        }
    )";
    // EXPECT_TRUE(errors.empty());
}
```

- [x] **Step 2: 运行测试确认 RED**

Run: `build/bin/compiler_tests --gtest_filter='LambdaSemaGaps.ReturnInference*'`
Expected: 至少 `ReturnInferenceSeesBodyLocal` / `OnlyBranch` 失败。

- [x] **Step 3: 实现惰性推断**

- `visit(LambdaExprAST&)`：
  - 不再预先扫描 return；`retType = node.returnType`（显式）或 `nullptr`。
  - 创建 `tmpFn` 时若无显式返回类型，先给 `void`。
  - 进入 body scope、声明参数/捕获后，推入/更新 frame：`inferReturn = !hasExplicitReturnType`、`inferredReturn = node.returnType`、`pendingVoidReturns.clear()`。
  - `visit(*body)` 后：若 `inferReturn`，`retType = frame.inferredReturn ? frame.inferredReturn : void`；写回 `node.returnType`。
- `visit(ReturnStmtAST&)`：
  - 若最内层 frame `inferReturn`：
    - 带值 return：`retValType = getExprType(*node.value)`；若 `inferredReturn == nullptr`，设为 `retValType`，更新 `currentFunction->returnType`；若 `pendingVoidReturns` 非空且推导类型非 void，对这些先前的无值 return 逐个报“非 void lambda 不能无值 return”，然后清空列表。
    - 无值 return：若 `inferredReturn == nullptr`，把该节点加入 `pendingVoidReturns`；否则沿用现有非 void 检查报错。
  - 之后继续现有 dangling/兼容性检查；`currentFunction->returnType` 已更新，所以检查使用最终类型。
- 嵌套 lambda 各自 frame；外层 return 不受内层影响。

- [x] **Step 4: 运行测试确认 GREEN**

Run: `build/bin/compiler_tests --gtest_filter='LambdaSemaGaps.*:ParserLambdaTest.*'`
Expected: PASS。

- [x] **Step 5: 提交**

```bash
git add src/sema/SemanticAnalyzer.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(PAR-11): infer lambda return types during body analysis"
```

---

### Task 4: Codegen 捕获初始化、`this` 脱糖、立即调用去重

**Files:**
- Modify: `src/ast/Expr.cpp`（`LambdaExprAST::codegen`、`LambdaCallExprAST::codegen`、`VariableExprAST::codegen`）
- Test: `tests/e2e/test_lambda.cpp`

**Interfaces:**
- Consumes: Task 2/3 的 `Capture::init`、`implicitThisAccess`、推断后的 `returnType`。
- Produces: 闭包构造 helper 同时处理普通捕获、`this`、捕获初始化；立即调用只求值一次捕获初始化。

- [x] **Step 1: 写失败 e2e 测试**

在 `tests/e2e/test_lambda.cpp` 添加：

```cpp
TEST_F(LambdaE2E, CaptureAllByReferenceWrites) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [&]() { a = 10; b = 20; };
            f();
            return (a == 10 && b == 20) ? 0 : 1;
        }
    )", "lam_refall.c"), 0);
}

TEST_F(LambdaE2E, MixedCaptureDefaultValueAndRefOverride) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [=, &b]() -> int32 { b = 20; return a + b; };
            int32 r = f();
            return (r == 21 && a == 1 && b == 20) ? 0 : 1;
        }
    )", "lam_mixed1.c"), 0);
}

TEST_F(LambdaE2E, MixedCaptureDefaultRefAndValueOverride) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 a = 1; int32 b = 2;
            auto f = [&, a]() -> int32 { b = 20; return a + b; };
            int32 r = f();
            return (r == 21 && a == 1 && b == 20) ? 0 : 1;
        }
    )", "lam_mixed2.c"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerEvaluatedOnce) {
    EXPECT_EQ(runSource(R"(
        int32 calls = 0;
        int32 next() { calls = calls + 1; return calls; }
        int32 main() {
            int32 r = [x = next()]() -> int32 { return x; }();
            return (r == 1 && calls == 1) ? 0 : 1;
        }
    )", "lam_init_once.c"), 0);
}

TEST_F(LambdaE2E, CaptureInitializerByReference) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 n = 1;
            auto f = [&x = n]() -> int32 { x = x + 1; return x; };
            int32 r = f();
            return (r == 2 && n == 2) ? 0 : 1;
        }
    )", "lam_init_ref.c"), 0);
}

TEST_F(LambdaE2E, ThisImplicitMember) {
    EXPECT_EQ(runSource(R"(
        class C {
        public:
            int32 v;
            int32 f() {
                auto g = [this]() -> int32 { return v; };
                return g();
            }
            int32 h() {
                auto g = [=]() -> int32 { return v + 1; };
                return g();
            }
        };
        int32 main() { C c; c.v = 7; return (c.f() == 7 && c.h() == 8) ? 0 : 1; }
    )", "lam_this.c"), 0);
}

TEST_F(LambdaE2E, NestedLambdaCapturesOuter) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 x = 1;
            auto outer = [&x]() -> int32 {
                auto inner = [=]() -> int32 { return x + 1; };
                return inner();
            };
            return outer() == 2 ? 0 : 1;
        }
    )", "lam_nested.c"), 0);
}

TEST_F(LambdaE2E, ReturnInferenceSeesBodyLocal) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            auto f = []() {
                int32 y = 1;
                return y;
            };
            return f() == 1 ? 0 : 1;
        }
    )", "lam_retlocal.c"), 0);
}
```

- [x] **Step 2: 运行测试确认 RED**

Run: `build/bin/compiler_tests --gtest_filter='LambdaE2E.*'`
Expected: 新用例失败；`CaptureInitializerEvaluatedOnce` 可能因双重构造返回 2。

- [x] **Step 3: 实现 codegen**

`src/ast/Expr.cpp`：

- 新增静态 helper：

```cpp
static bool emitClosureCaptures(LambdaExprAST& lambda,
                                CodegenContext& ctx,
                                llvm::Value* closureAlloca);
```

对每个 `captures[i]`：

- `init != nullptr`：
  - `llvm::Value* v = init->codegen(ctx);`
  - `byRef`：要求 `init->isLValue`；`builder.CreateStore(v, fieldPtr);`
  - 按值：若 `init->isLValue` 先 `readLValue`；`ctx.castValue(v, init->type, ctx.getLLVMType(fieldType))`；store。
- `init == nullptr`：
  - `byRef` / `this` 按现有 `lookupVariableAddr` 逻辑；
  - 按值 load/store。
- 错误返回 `false`。

`LambdaExprAST::codegen` 用 helper 替换现有捕获构造循环。
`LambdaCallExprAST::codegen`：

- 调用 `lambda->codegen(ctx)` 得到 `lambdaVal`。
- 有闭包时创建栈槽并 `builder.CreateStore(lambdaVal, closureAlloca)`，把 `closureAlloca` 作为首参；**删除重新构造捕获的循环**。

`VariableExprAST::codegen`：

```cpp
if (implicitThisAccess) return implicitThisAccess->codegen(ctx);
```

放在 enum/function ref 检查之前或之后均可，但必须在 `lookupVariableAddr` 之前。

- [x] **Step 4: 运行 e2e 测试确认 GREEN**

Run: `build/bin/compiler_tests --gtest_filter='LambdaE2E.*:ParserLambdaTest.*:LambdaSemaGaps.*'`
Expected: PASS。

- [x] **Step 5: 提交**

```bash
git add src/ast/Expr.cpp tests/e2e/test_lambda.cpp
git commit -m "feat(CG-11): lambda capture initializers, implicit this, and single-eval calls"
```

---

### Task 5: 文档与全量验证

**Files:**
- Modify: `docs/spec/grammar.ebnf`
- Modify: `docs/spec/semantics.md`
- Modify: `docs/superpowers/specs/2026-11-04-lambda-gaps-design.md`（如实现中调整决策）
- Modify: `TODO.md`、`Progress.md`（只追加本轮记录；不要 stage 既有容器迁移改动）
- Test: 全量

- [x] **Step 1: 更新 grammar**

`docs/spec/grammar.ebnf` 的 `lambda-capture` 改为：

```ebnf
lambda-capture      = '[' [ capture-spec ] ']' ;
capture-spec        = '=' [ ',' override-list ]
                    | '&' [ ',' override-list ]
                    | capture-list ;
capture-list        = capture-item , { ',' , capture-item } ;
override-list       = capture-item , { ',' , capture-item } ;
capture-item        = [ '&' ] identifier [ '=' assignment-expr ] ;
```

- [x] **Step 2: 更新 semantics §7.5**

写清 `[&]`、混排覆盖、捕获初始化、`[this]` 隐式成员、未捕获诊断、返回惰性推断、全局/static 不捕获，以及闭包逃逸仍是非目标。

- [x] **Step 3: 更新 TODO/Progress**

在 `TODO.md` 的 lambda/PAR-11 相关条目追加本轮完成项；在 `Progress.md` 追加“2026-11-04 lambda 语言缺口”小节。**只追加，不重排既有未提交改动。**

- [x] **Step 4: 全量构建与测试**

Run:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build -j8
build/bin/cmm examples/all_features.cmm -o /tmp/all_features && /tmp/all_features
```

Expected: 全量测试 0 失败（当前仓库无 `GTEST_SKIP`；历史计划中的 2 个已在 2026-11-01 constexpr 容器收口时移除）；示例输出 `ALL FEATURES OK`。

- [x] **Step 5: 提交文档**

只 stage 本轮明确修改的文档；`TODO.md`/`Progress.md` 若混有既有容器迁移改动，则**不要提交**，留在工作区：

```bash
git add docs/spec/grammar.ebnf docs/spec/semantics.md docs/superpowers/specs/2026-11-04-lambda-gaps-design.md docs/superpowers/plans/2026-11-04-lambda-gaps.md
git commit -m "docs(PAR-11): document lambda capture and inference gaps"
```

---

## Self-Review

- Spec coverage：D1 parser Task 1；D2/D3/D4 sema Task 2；D5 sema Task 3；D6 codegen Task 4；文档 Task 5。
- 类型一致性：`Capture::init`、`captureAllByRef`、`implicitThisAccess`、`LambdaFrame` 字段在任务间同名。
- Review Focus 每一项都有对应测试：未捕获（Task 2）、全局/static（Task 2）、初始化一次（Task 4）、`this`（Task 2/4）、返回推断（Task 3/4）。
- 工作区保护：每个提交步骤只 `git add` 明确文件，不 stage 既有容器迁移改动。
