# AGG-10 static 成员 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** class/struct 支持 `static` 成员函数（无 self）与 static 成员变量（类内初始化），仅经 `Class::member` 限定访问。

**Architecture:** 方案 A 类前缀全局符号去糖——声明侧（parser 收集 + sema 改名 `Class_member`）把 static 成员落为全局变量/全局函数；访问侧复用既有 `resolveNamespaceName` 限定名拍平（`Vec::create` → `Vec_create`），查找/mangle/codegen 零改动。

**Tech Stack:** C++20 / LLVM 22 / gtest / CTest

**Spec:** `docs/superpowers/specs/2026-10-05-static-members-design.md`（DS1–DS5 冻结）

## Global Constraints

- 基线 820/820（commit `cb31f10`）；每任务结束全量 `ctest` 必须绿。
- TDD：RED 先行，未亲见失败不写实现。
- sema 测试名前缀 `SM`（TypeContext 单例跨测试泄漏防护）。
- `docs/superpowers/` 与 `Progress.md` 被 gitignore，提交需 `git add -f`。
- 决策引用：DS1 仅类名限定 / DS2 类内初始化=全局定义 / DS3 `mangleNamespaceName` 去糖 / DS4 无 this / DS5 E2009 复用 DEC-01。
- 范围外（不得顺手实现）：实例路径访问 static、类内 unqualified 直呼、继承链 static 查找、顶层 static 函数、static 局部变量。

## Review Focus

1. **namespace 内类的 static 成员**：声明与访问两侧的 ns 前缀必须一致（类在 `ns` 内 → 符号 `ns_Vec_count`，访问写 `ns::Vec::count`）；半限定（`Vec::count`）按 namespace 现状拒绝——Task 1 测试钉。
2. **StructDeclAST::codegen 提前 return 分支**：类型已注册时（如前向声明后再定义）staticMembers 仍须生成全局变量，否则链接期 undefined——Task 2 e2e 钉。
3. **static 方法体内使用同类 private static 成员**：`currentClass`（SemanticAnalyzer.h:174）须对 static 方法同样生效，E2009 不误报——Task 2 e2e 钉。
4. **字段路径初始化器缺口**：实例字段本不支持 `= init`，static 变量的 `= init` 是新解析分支，须正确消费 `=` 后的完整表达式与 `;`——Task 1 sema 测试钉。
5. **去糖碰撞**：用户手写 free function `Vec_count` 与 `Vec::count` 去糖符号重名 → `declare` 重定义错误（可接受，非静默）——Task 1 sema 测试钉。

---

### Task 1: AST + Parser + Sema（收集、改名注册、访问控制）

**Files:**
- Modify: `src/ast/Decl.h`（FunctionDeclAST 加 `bool isStatic = false;`；StructDeclAST 加 `std::vector<std::unique_ptr<VarDeclAST>> staticMembers;`）
- Modify: `src/frontend/Parser.cpp`（`parseStructDecl` :2356 与 `parseClassDecl` :2388 成员循环）
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit(StructDeclAST)` :1743 起；`visit(VariableExprAST)` / `visit(CallExprAST)` 访问检查挂点）与 `SemanticAnalyzer.h`（`staticMemberIndex` 成员）
- Test: `tests/sema/test_semantic_analyzer.cpp`（文件末尾追加）

**Interfaces:**
- Consumes: `mangleNamespaceName(std::string)`（SemanticAnalyzer.cpp 内既有）；`visit(VarDeclAST)` 全局声明路径；`currentClass`（SemanticAnalyzer.h:174）；`ClassType::memberAccessLevel(name)`（Type.h:172）。
- Produces: `FunctionDeclAST::isStatic`（Task 2 codegen 依赖"static 方法名已被 sema 改写为去糖形式、无 this 参数"）；`StructDeclAST::staticMembers`（Task 2 codegen 消费）；sema 成员 `staticMemberIndex`（key → `{ClassType*, 原成员名}`，Task 2 复用）。

- [ ] **Step 1: 写 8 项 sema 测试（RED）**

```cpp
// AGG-10: static members desugar to class-prefixed global symbols.
// Spec: docs/superpowers/specs/2026-10-05-static-members-design.md

TEST(SliceSemTest, SMStaticMethodQualifiedCall) {
    EXPECT_TRUE(analyzeOk(
        "class SMBox { public: static int32 make() { return 7; } }; "
        "int32 main() { return SMBox::make(); }"));
}

TEST(SliceSemTest, SMStaticVarQualifiedAccess) {
    EXPECT_TRUE(analyzeOk(
        "class SMCtr { public: static int32 count = 0; }; "
        "int32 main() { SMCtr::count = 5; return SMCtr::count; }"));
}

TEST(SliceSemTest, SMPrivateStaticExternalRejected) {
    // DS5: E2009 (member access), not a parse error — pin the message.
    Lexer lexer("test.c",
        "class SMP { private: static int32 secret = 1; }; "
        "int32 main() { return SMP::secret; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool hasE2009 = false;
    for (auto& e : analyzer.getErrors())
        if (e.message.find("private") != std::string::npos) hasE2009 = true;
    EXPECT_TRUE(hasE2009);
}

TEST(SliceSemTest, SMInstancePathRejected) {
    // DS1: static members are not instance members — pin the "no matching
    // method" diagnostic (a parse-level rejection would be wrong here).
    Lexer lexer("test.c",
        "class SMP2 { public: static int32 make() { return 1; } }; "
        "int32 main() { SMP2 obj; return obj.make(); }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    ASSERT_FALSE(analyzer.getErrors().empty());
    bool hasNoMethod = false;
    for (auto& e : analyzer.getErrors())
        if (e.message.find("no matching method") != std::string::npos) hasNoMethod = true;
    EXPECT_TRUE(hasNoMethod);
}

TEST(SliceSemTest, SMStaticInstanceSameName) {
    EXPECT_TRUE(analyzeOk(
        "class SMDual { "
        "public: int32 f(int32 x) { return x; } "
        "static int32 f() { return 42; } }; "
        "int32 main() { SMDual obj; return obj.f(1) + SMDual::f(); }"));
}

TEST(SliceSemTest, SMStaticOverload) {
    EXPECT_TRUE(analyzeOk(
        "class SMOvl { public: static int32 g(int32 x) { return x; } "
        "static int32 g(int32 x, int32 y) { return x + y; } }; "
        "int32 main() { return SMOvl::g(1) + SMOvl::g(1, 2); }"));
}

TEST(SliceSemTest, SMStaticVarNoInitZero) {
    EXPECT_TRUE(analyzeOk(
        "class SMZ { public: static int32 z; }; "
        "int32 main() { return SMZ::z; }"));
}

TEST(SliceSemTest, SMStaticNotInLayout) {
    // DS2: static variables do not occupy object layout.
    EXPECT_TRUE(analyzeOk(
        "class SML { public: static int32 big = 0; int32 a; }; "
        "int32 main() { return sizeof(SML) == sizeof(int32) ? 0 : 1; }"));
}

TEST(SliceSemTest, SMStaticCollisionRejected) {
    // Review Focus 5: hand-written Vec_count collides with the desugared
    // Vec::count — a redefinition error, never silent shadowing.
    EXPECT_FALSE(analyzeOk(
        "class SMC { public: static int32 v = 0; }; "
        "int32 SMC_v = 1; int32 main() { return 0; }"));
}
```

（namespace 交互钉——Review Focus 1，一并加入：）

```cpp
TEST(SliceSemTest, SMStaticInNamespace) {
    EXPECT_TRUE(analyzeOk(
        "namespace smns { class SMN { public: static int32 v = 3; }; } "
        "int32 main() { return smns::SMN::v; }"));
}
```

- [ ] **Step 2: 跑测试验证失败形态**

Run: `./bin/compiler_tests --gtest_filter='SliceSemTest.SM*'`
Expected: 全部 FAIL（现状类体内 `static` 未处理 → 解析错误，正向测试 analyzeOk=false、消息钉不匹配）。SMStaticCollisionRejected 为反向 pin 预期 PASS（重定义本就拒绝）。

- [ ] **Step 3: 实现**

1. **Decl.h**：`FunctionDeclAST` 加公有字段 `bool isStatic = false;`；`StructDeclAST` 加 `std::vector<std::unique_ptr<VarDeclAST>> staticMembers;`。
2. **Parser**（两个成员循环同样处理）：循环体开头
   `bool memberIsStatic = false; if (check(TokenType::TOKEN_STATIC)) { advance(); memberIsStatic = true; }`
   - 方法分支（`check(TOKEN_LPAREN)`）：`func->isStatic = memberIsStatic;`（照常进 methods，进 memberAccess）。
   - 字段分支：`memberIsStatic` 时改为——`match(TOKEN_ASSIGN)` 则 `parseExpr(2)` 为初始化器；构造 `auto vd = std::make_unique<VarDeclAST>(memberName, declType, std::move(init));` 进 `staticMembers`；`memberAccess[memberName] = currentAccess;`；`expect(TOKEN_SEMICOLON, ...)`；`continue;`（不进 fields，保证不占布局）。
   - struct 循环（parseStructDecl）同样支持（无访问段，currentAccess 恒 public 语义——struct 走 fields 分支外的新 static 路径，memberAccess 不记录，DS5 检查按 Public）。
3. **Sema `visit(StructDeclAST)`**：class 分支中——
   - static 方法：跳过 this 插入与 `classType->addMethod`；`method->name = mangleNamespaceName(classType->name + "::" + method->name);` 然后 `staticMemberIndex[method->name] = {classType, 原名}`；`visit(*method)` 照常（currentClass 包裹不变——DS5 类内上下文）。
   - 实例方法路径不变。
   - staticMembers：`vd->name = mangleNamespaceName(classType->name + "::" + vd->name); staticMemberIndex[vd->name] = {classType, 原名}; visit(*vd);`
   - struct 分支（StructType）：staticMembers 名字改写为 `structName + "::" + name` 去糖后 `visit(*vd)`（无 memberAccess，跳过索引登记或登记为 Public）。
4. **Sema 访问检查（DS5）**：`visit(CallExprAST)`（callee resolve 之后、overload 解析处）与 `visit(VariableExprAST)`（symbol 解析成功处）各加：
   ```cpp
   if (auto it = staticMemberIndex.find(resolvedKey); it != staticMemberIndex.end()) {
       auto* definingClass = it->second.first;
       AccessLevel level = definingClass->memberAccessLevel(it->second.second);
       if (level != AccessLevel::Public && currentClass != definingClass) {
           emitError(DiagnosticCode::SemPrivateMemberAccess, /*与 :1461 同文案*/, node);
       }
   }
   ```
   注意：去糖符号与实例成员名不冲突（key 含类前缀），误报面为零。
5. namespace 场景一致性：声明侧 `visit(*vd)`/`visit(*method)` 走既有 `scopedName`（ns 前缀）；访问侧 `smns::SMN::v` 经 `resolveNamespaceName` 拍平为 `smns_SMN_v`——两侧同构，零额外改动（行为由 Step 1 末尾测试钉住）。

- [ ] **Step 4: 跑测试验证通过**

Run: `./bin/compiler_tests --gtest_filter='SliceSemTest.SM*'`
Expected: 10/10 PASS。

- [ ] **Step 5: 全量回归 + 提交**

Run: `ctest`
Expected: 830/830（820 + 10）。

```bash
git add src/ast/Decl.h src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp src/sema/SemanticAnalyzer.h tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(AGG-10): static 成员——parser 收集 + sema 去糖注册（DS1-DS5）"
```

### Task 2: Codegen + e2e

**Files:**
- Modify: `src/ast/Decl.cpp`（`StructDeclAST::codegen` :458 起）
- Test: `tests/e2e/test_agg.cpp`（文件末尾追加；若该 fixture 不存在则用 `tests/e2e/test_classes.cpp` 既有 fixture——执行时以实际文件为准）

**Interfaces:**
- Consumes: Task 1 的 `staticMembers`（名字已去糖、已过 sema）、static 方法（名字已去糖、无 this 参数、在 `node.methods` 中）；`VarDeclAST::codegen` 的 `ctx.isGlobalScope()` 全局分支（零改动复用）。
- Produces: 无下游消费（叶子任务）。

- [ ] **Step 1: 写 4 项 e2e 测试（RED）**

```cpp
// AGG-10: static members — global storage, class-qualified access.
TEST_F(<既有类 fixture>, SMCounterPersist) {
    EXPECT_EQ(runSource(R"(
        class SMCtr2 {
            private: static int32 count = 0;
            public: static int32 next() { count = count + 1; return count; }
        };
        int32 main() {
            SMCtr2::next(); SMCtr2::next();
            return SMCtr2::next() == 3 ? 0 : 1;
        }
    )", "test_sm_counter.c"), 0);
}

TEST_F(<既有类 fixture>, SMStaticFactory) {
    // 跨类返回类型，避开 self-type 返回的未验证面（Review Focus 范围外）。
    EXPECT_EQ(runSource(R"(
        class SMPoint { public: int32 x; int32 y; };
        class SMRegistry {
            public: static SMPoint origin() { SMPoint p; p.x = 0; p.y = 0; return p; }
        };
        int32 main() {
            SMPoint p = SMRegistry::origin();
            return (p.x == 0 && p.y == 0) ? 0 : 1;
        }
    )", "test_sm_factory.c"), 0);
}

TEST_F(<既有类 fixture>, SMPrivateInternalUse) {
    EXPECT_EQ(runSource(R"(
        class SMAcc {
            private: static int32 secret = 11;
            public: int32 reveal() { return SMAcc::secret; }
        };
        int32 main() {
            SMAcc obj;
            return obj.reveal() == 11 ? 0 : 1;
        }
    )", "test_sm_internal.c"), 0);
}

TEST_F(<既有类 fixture>, SMStaticAfterForwardDecl) {
    // Review Focus 2: forward declaration registers the type first; the
    // later definition's staticMembers must still emit the global.
    EXPECT_EQ(runSource(R"(
        class SMFwd;
        class SMFwd { public: static int32 v = 9; };
        int32 main() { return SMFwd::v == 9 ? 0 : 1; }
    )", "test_sm_fwd.c"), 0);
}
```

- [ ] **Step 2: 跑测试验证失败形态**

Run: `./bin/compiler_tests --gtest_filter='*SMCounterPersist*:*SMStaticFactory*:*SMPrivateInternalUse*:*SMStaticAfterForwardDecl*'`
Expected: 预判 FAIL（staticMembers 未生成 → JIT/运行期符号缺失或值错误）。SMPrivateInternalUse 可能直接 PASS（sema 改名后 static 方法已随 methods 生成）——若 PASS 则记录偏差为"方法侧已通，仅变量侧缺口"，不改测试。

- [ ] **Step 3: 实现 `StructDeclAST::codegen`**

在"类型已存在"提前 return 分支**与**正常路径末尾都加：

```cpp
for (auto& vd : staticMembers) {
    if (vd) vd->codegen(ctx);   // ctx.isGlobalScope() → 全局定义（幂等：LLVM getGlobal 复用）
}
```

static 方法零改动：sema 已改写名字，`FunctionDeclAST::codegen` 按去糖名生成（无 this 参数，与普通函数一致）。

- [ ] **Step 4: 跑测试验证通过**

Run: 同 Step 2
Expected: 4/4 PASS。

- [ ] **Step 5: 全量回归 + 提交**

Run: `ctest`
Expected: 834/834（830 + 4）。

```bash
git add src/ast/Decl.cpp tests/e2e/test_agg.cpp
git commit -m "feat(AGG-10): static 成员 codegen——staticMembers 全局定义（含提前 return 分支）"
```

### Task 3: 规范 + 文档收尾 + 评审

**Files:**
- Modify: `docs/spec/abi.md`（MOD-15 节补 static 成员编码）
- Modify: `TODO.md`（AGG-10 → `[x]`；PAR-04 待补列表更新；P1-01 剩余项更新为仅 AGG-11）
- Modify: `Progress.md`（追加；`git add -f`）

- [ ] **Step 1: abi.md 补编码规范**（MOD-15 节，照现有条目格式）：`Class_member`（static 成员变量，全局定义）；`Class_method_<params>`（static 成员函数，无 this 位）。

- [ ] **Step 2: TODO.md 勾记**：AGG-10 标 `[x]` 附实现摘要与 spec 路径；PAR-04 "待补"列表去掉 static 成员；P1-01 剩余改"嵌套类型（AGG-11）"。

- [ ] **Step 3: Progress.md 追加 + 提交**（记录含最终计数）

```bash
git add docs/spec/abi.md TODO.md && git add -f Progress.md
git commit -m "docs: AGG-10 static 成员完成记录（834/834）"
```

- [ ] **Step 4: 整分支评审 + TDD 修复**

review-package（spec-commit `87c14ab` 起）→ subagent 评审（Focus：DS1–DS5 逐条、Review Focus 五条、namespace 前缀一致性、提前 return 幂等）→ Critical/Important 修复。

- [ ] **Step 5: 评审修复提交 + Rulings 汇报**
