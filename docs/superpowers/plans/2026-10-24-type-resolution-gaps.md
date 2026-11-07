# 类型解析三缺口收口 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `using` 在块/聚合成员体可用、让同名 struct/union 的前向占位与定义合并、让 `*p` 穿透指针别名。

**Architecture:** 三块独立改动：①sema/codegen 在解引用前剥 `TypedefType`；②parser 在 struct/union 定义注册处复用已有占位对象并清空重填；③TypeContext 增加分层局部别名栈，parser/sema 在复合语句 push/pop，成员别名复用扁平键 + `nestedTypeAccess`。

**Tech Stack:** C++17、LLVM、GoogleTest/CTest、手写递归下降 parser + sema。

**Spec:** `docs/superpowers/specs/2026-10-24-type-resolution-gaps-design.md`

## Global Constraints

- 当前全量基线 **1514** 通过；每任务结束 `cd build && ctest -j8` 全绿，提交后才进下一任务。
- 不新增语言关键字；`using` 仍是上下文标识符。
- `typedef`/`type` 的硬错误与提示不变。
- 保留顶层/namespace/模板别名行为；`template<typename T> using` 必须继续工作。
- 块别名只进局部栈，不写全局 `m_typedefs`、不 `declare` 值符号。
- 成员别名用扁平键（`qualifyTypeDeclName`），不继承到派生类。
- 每任务一个提交；提交前确认 `git status` 只含本任务文件。

## Review Focus

- 块别名在内层块遮蔽外层、块尾失效；`C::T` 不受块别名影响。
- 类私有成员别名在类外使用报 E2009，类内方法体可用。
- 前向占位合并后，早先绑定的别名/指针看到定义后的字段（`p->x`）。
- `*p` 对多层指针别名读写、`(*p).x`、`**pp` 都正确。
- 模板别名跨函数使用不受局部栈影响。

---

### Task 1: `*p` 穿透指针别名

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（`UnaryOp::Deref` 分支，~1991）
- Modify: `src/ast/Expr.cpp`（`UnaryExprAST::codegen` 的 `UnaryOp::Deref`，~710）
- Test: `tests/e2e/test_using_alias.cpp`

**Interfaces:**
- Consumes: 现有 `stripTypedef(Type*)`（sema 文件顶部）与 `stripTypedefsT12(Type*)`（Expr.cpp）。
- Produces: 无新 API；`UnaryExprAST::type` 的 `Deref` 结果现在是剥别名后的 `Pointer::base`。

- [ ] **Step 1: 写失败测试**（追加到 `tests/e2e/test_using_alias.cpp`）

```cpp
TEST_F(UsingAliasE2E, DerefPointerAliasReadWrite) {
    EXPECT_EQ(runSource(R"(
        using P = int32*;
        int32 main() { int32 x = 7; P p = &x; *p = 9; return *p - 9; }
    )", "deref_alias.c"), 0);
}

TEST_F(UsingAliasE2E, DerefStructPointerAliasMember) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; };
        using PS = struct S*;
        int32 main() { S s; s.x = 1; PS p = &s; (*p).x = 5; return p->x - 5; }
    )", "deref_struct_alias.c"), 0);
}

TEST_F(UsingAliasE2E, DerefAliasChain) {
    EXPECT_EQ(runSource(R"(
        using P = int32*;
        using PP = P*;
        int32 main() { int32 x = 3; P p = &x; PP pp = &p; return **pp - 3; }
    )", "deref_alias_chain.c"), 0);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "UsingAliasE2E.Deref" --output-on-failure`
Expected: 3 个测试 FAIL，报 `cannot dereference non-pointer type 'P'`（或 `PS`）。

- [ ] **Step 3: sema 剥别名**

在 `SemanticAnalyzer::visit(UnaryExprAST&)` 的 `case UnaryOp::Deref:` 中，先把 `operandType` 剥成 `Type* pointee = stripTypedef(operandType);`，再判 `pointee && pointee->kind == TypeKind::Pointer`，命中时 `node.type = pointee->base;`；否则保持原错误文案（`typeToString(operandType)` 仍显示别名拼写）。

- [ ] **Step 4: codegen 剥别名**

在 `UnaryExprAST::codegen` 的 `UnaryOp::Deref` 中，令 `Type* opType = stripTypedefsT12(operand->type);`；`opType && opType->kind == TypeKind::Pointer` 时直接返回 `ptrVal`；回退分支的 pointee 取 `opType ? opType->base : nullptr`。

- [ ] **Step 5: 确认 GREEN + 全量**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "UsingAliasE2E.Deref" && ctest -j8`
Expected: 3/3 通过；全量 1517/1517 通过。

- [ ] **Step 6: 提交**

```bash
git add src/sema/SemanticAnalyzer.cpp src/ast/Expr.cpp tests/e2e/test_using_alias.cpp
git commit -m "fix(SEM-02): dereference through pointer aliases"
```

---

### Task 2: 前向类型占位与定义合并

**Files:**
- Modify: `src/frontend/Parser.cpp`（`parseType()` struct ~2141 / union ~2215；`parseDeclarationImpl()` 注册 ~2828 / ~2929）
- Test: `tests/e2e/test_toplevel_user_types.cpp`

**Interfaces:**
- Consumes: `TypeContext::getStruct/getUnion/addStruct/addUnion`、`StructType::fields`、`UnionType::members`。
- Produces: 同名 struct/union 的占位与定义是同一对象；不新增 API。

- [ ] **Step 1: 写失败测试**（追加到 `tests/e2e/test_toplevel_user_types.cpp`）

```cpp
TEST_F(TopLevelUserTypeE2E, UsingAliasBeforeStructDefinition) {
    EXPECT_EQ(runSource(R"(
        using N = struct Node;
        struct Node { int32 x; };
        int32 main() { N n; n.x = 5; return n.x - 5; }
    )", "fwd_alias_struct.c"), 0);
}

TEST_F(TopLevelUserTypeE2E, ForwardDeclThenAliasBeforeDefinition) {
    EXPECT_EQ(runSource(R"(
        struct Node;
        using N = Node;
        struct Node { int32 x; };
        int32 main() { N n; n.x = 6; return n.x - 6; }
    )", "fwd_decl_alias.c"), 0);
}

TEST_F(TopLevelUserTypeE2E, UsingAliasBeforeUnionDefinition) {
    EXPECT_EQ(runSource(R"(
        using U = union Value;
        union Value { int32 i; float32 f; };
        int32 main() { U u; u.i = 7; return u.i - 7; }
    )", "fwd_alias_union.c"), 0);
}

TEST_F(TopLevelUserTypeE2E, ForwardPointerMemberAfterDefinition) {
    EXPECT_EQ(runSource(R"(
        struct S; S* p;
        struct S { int32 x; };
        int32 main() { S s; s.x = 4; p = &s; return p->x - 4; }
    )", "fwd_ptr_member.c"), 0);
}

TEST_F(TopLevelUserTypeE2E, PointerAliasToForwardStruct) {
    EXPECT_EQ(runSource(R"(
        using P = struct S*;
        struct S { int32 x; };
        int32 main() { S s; s.x = 8; P p = &s; return p->x - 8; }
    )", "fwd_ptr_alias.c"), 0);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "TopLevelUserTypeE2E.(UsingAliasBefore|Forward|PointerAliasToForward)" --output-on-failure`
Expected: 5 个 FAIL，报 `no member named 'x'`（或 `p->x` 同类）。

- [ ] **Step 3: `parseType()` 的定义分支复用占位**

struct 分支：在 `name = nextAnonId()` 之前记录 `const bool isAnonymous = name.empty();`。进入 `{` 后：
- `isAnonymous` → `new StructType(key)`；
- 否则 `TypeContext::instance().getStruct(key)`，为空才 `new`；复用则 `fields.clear()`。
填完字段后 `addStruct(key, structType)` 并返回同一对象。
union 分支镜像：复用 `getUnion(key)`，复用则 `members.clear()`。

- [ ] **Step 4: `parseDeclarationImpl()` 的注册复用占位**

struct 注册块：把 `auto structType = new StructType(structDecl->name);` 改为先取 `getStruct`；为空才 `new`，复用则 `fields.clear()`；填完后 `addStruct` 同一对象。union 注册块镜像（`getUnion` / `members.clear()`）。前向声明分支的“无占位才建”保持不变。**不要**在 parser 置 `isComplete`。

- [ ] **Step 5: 确认 GREEN + 全量**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "TopLevelUserTypeE2E" && ctest -j8`
Expected: 新 5 个通过；全量 1522/1522 通过。

- [ ] **Step 6: 提交**

```bash
git add src/frontend/Parser.cpp tests/e2e/test_toplevel_user_types.cpp
git commit -m "fix(MOD-03): merge forward struct/union placeholders with definitions"
```

---

### Task 3: 块作用域 `using`

**Files:**
- Modify: `src/ast/Type.h`（`TypeContext` 声明）、`src/ast/Type.cpp`（实现）
- Modify: `src/ast/Decl.h`（`UsingDeclAST` 加 `bareName`、`isBlockScope`）
- Modify: `src/frontend/Parser.cpp`（`parseCompoundStmt` ~1346、`parseStmt` ~1490、`lookupNamedType` ~2017）
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit(DeclStmtAST)` ~5165、`visit(CompoundStmtAST)` ~4184、`visit(UsingDeclAST)` ~6077、`resolveTypeByName` ~5650）
- Test: `tests/frontend/test_parser.cpp`、`tests/e2e/test_using_alias.cpp`、`tests/sema/test_annotation_semantics.cpp`

**Interfaces:**
- Produces: `TypeContext::pushLocalTypedefScope()`、`popLocalTypedefScope()`、`Type* addLocalTypedef(const std::string&, Type*)`、`Type* getLocalTypedef(const std::string&) const`。
- Produces: `UsingDeclAST::bareName`（原始拼写）、`bool isBlockScope`、`Type* aliasWrapper`（解析期包装，供 sema 折叠弃用）。
- Consumes: Task 1/2 不动。

- [ ] **Step 1: 写失败测试**

`tests/frontend/test_parser.cpp`：删除 `UsingInBlockRejected`，替换为：

```cpp
TEST_F(ParserDeclTest, UsingInBlockParses) {
    EXPECT_TRUE(parseErrors("int32 main() { using T = int32; T x = 1; return x; }").empty());
}

TEST_F(ParserDeclTest, UsingBlockScopeEndsAtBlock) {
    EXPECT_FALSE(parseErrors("int32 main() { { using T = int32; } T x = 1; }").empty());
}

TEST_F(ParserDeclTest, UsingBlockShadowRestoresOuter) {
    auto tu = parse("using T = int32; int32 main() { { using T = float64; } T y = 2; return y; }");
    ASSERT_NE(tu, nullptr);
    auto* fn = dynamic_cast<FunctionDeclAST*>(tu->declarations[1].get());
    ASSERT_NE(fn, nullptr);
    auto* block = dynamic_cast<CompoundStmtAST*>(fn->body.get());
    ASSERT_NE(block, nullptr);
    auto* declStmt = dynamic_cast<DeclStmtAST*>(block->stmts[1].get());
    ASSERT_NE(declStmt, nullptr);
    auto* var = dynamic_cast<VarDeclAST*>(declStmt->decl.get());
    ASSERT_NE(var, nullptr);
    auto* td = dynamic_cast<TypedefType*>(var->type);
    ASSERT_NE(td, nullptr);
    EXPECT_EQ(td->aliasedType->kind, TypeKind::Int32);
}
```

`tests/e2e/test_using_alias.cpp` 追加：

```cpp
TEST_F(UsingAliasE2E, BlockScopedAliasRuns) {
    EXPECT_EQ(runSource(R"(
        int32 main() { using T = int32; T x = 3; return x - 3; }
    )", "block_alias.c"), 0);
}

TEST_F(UsingAliasE2E, TemplateAliasAcrossFunctions) {
    EXPECT_EQ(runSource(R"(
        template<typename T, usize N> struct Arr { T data[N]; };
        template<typename T> using Vec = Arr<T, 2>;
        int32 first() { Vec<int32> v; v.data[0] = 1; return v.data[0]; }
        int32 second() { Vec<int32> v; v.data[0] = 2; return v.data[0]; }
        int32 main() { return first() + second() - 3; }
    )", "tpl_alias_fns.c"), 0);
}
```

`tests/sema/test_annotation_semantics.cpp` 追加：

```cpp
TEST_F(AnnotationSemantics, DeprecatedBlockUsingW3004) {
    EXPECT_TRUE(hasWarning(
        "int32 main() { [[deprecated(\"old\")]] using T = int32; T x = 1; return x; }",
        "is deprecated: old"));
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "UsingInBlock|UsingBlock" --output-on-failure`
Expected: `UsingInBlockParses`/`UsingBlockShadowRestoresOuter` 失败（`only allowed at namespace scope`）；`UsingBlockScopeEndsAtBlock` 可能已通过（保留为回归）。

- [ ] **Step 3: TypeContext 局部别名栈**

`src/ast/Type.h` 的 `TypeContext` public 区加四个方法声明；private 加：
```cpp
std::vector<std::unordered_map<std::string, Type*>> m_localTypedefs;
```
`src/ast/Type.cpp` 实现：push 压空 map；pop 弹栈；`addLocalTypedef` 写栈顶（栈空则退回 `m_typedefs`）并**返回新建的 `TypedefType*`**；`getLocalTypedef` 自内向外查，未命中返回 `nullptr`。每个别名包装仍是 `new TypedefType(name, type)`。

- [ ] **Step 4: parser 作用域 + 块语句 + 查找**

- `parseCompoundStmt()`：`expect('{')` 后 `pushLocalTypedefScope()`；`expect('}')` 后 `popLocalTypedefScope()`。push 之后没有提前 return。
- `parseStmt()`：保留 `isRemovedTypeAliasStart()` 的硬错误；把 `isLocalUsingStart()` 的拒绝块**删掉**，在 `makeDeclStmt` lambda 定义之后加正向分支：`advance()` 消耗 `using`，取裸名，`advance()` 消耗 `=`，`parseTypeId()`；失败 `skipPastSemicolon()` 返回 `nullptr`；成功 `match(';')`、构造 `UsingDeclAST(bare, type)` 并设 `bareName=bare`、`isBlockScope=true`、`aliasWrapper = addLocalTypedef(bare, type)`，`return makeDeclStmt(std::move(u));`。注解门（`if (isAnnotationStart())`）的条件补 `|| isLocalUsingStart()`。
- `lookupNamedType()`：在限定名早退之后、前缀尝试之前，若非限定名且 `getLocalTypedef(name)` 命中则直接返回。
- `UsingDeclAST`：`src/ast/Decl.h` 加 `std::string bareName;`、`bool isBlockScope = false;`。

- [ ] **Step 5: sema 作用域 + 分派 + 查找**

- `visit(DeclStmtAST)`：增加 `UsingDeclAST` 分派。
- `visit(CompoundStmtAST)`：`enterScope()` 后 `pushLocalTypedefScope()`，循环结束 `popLocalTypedefScope()` 后再 `exitScope()`。
- `visit(UsingDeclAST)`：开头 `validateAnnotations(..., "using")`；`if (node.isBlockScope)` 时取 `parseTimeAlias = getLocalTypedef(bareName)`，`addLocalTypedef(bareName, aliasedType)`，取 `storedAlias`，把 `deprecated` 折叠落到 `node.aliasWrapper`（解析期包装，AST 声明持有它）与新的局部包装两份上，**不** `declare`、不注册 inline enum 枚举量，直接 return；其余走现有全局路径。
- `resolveTypeByName`：非限定名先 `getLocalTypedef(name)`。

- [ ] **Step 6: 确认 GREEN + 全量**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "UsingInBlock|UsingBlock|BlockScopedAlias" && ctest -j8`
Expected: 新测试全过；全量以实际输出为准（基线 1522，净增 5 → 约 1527）。

- [ ] **Step 7: 提交**

```bash
git add src/ast/Type.h src/ast/Type.cpp src/ast/Decl.h src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp tests/frontend/test_parser.cpp tests/e2e/test_using_alias.cpp tests/sema/test_annotation_semantics.cpp
git commit -m "feat(PAR-24): block-scoped using aliases"
```

---

### Task 4: 聚合成员体 `using` + 访问控制

**Files:**
- Modify: `src/frontend/Parser.h`（`parseUsingAliasDecl(bool member)` 声明）
- Modify: `src/frontend/Parser.cpp`（三个成员循环 + 新 helper）
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visitNestedTypeDecls` ~4630、`visitNestedDecl` ~4615、`checkNestedTypeAccess` ~4659）
- Modify: `src/sema/TemplateInstantiator.cpp`（`UsingDeclAST` 克隆分支 ~350）
- Test: `tests/frontend/test_parser.cpp`、`tests/e2e/test_using_alias.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 3 的 `UsingDeclAST::bareName`。
- Produces: `std::unique_ptr<UsingDeclAST> Parser::parseUsingAliasDecl(bool member)`；`member=true` 时登记扁平键并设 `name=flat`、`bareName=bare`。

- [ ] **Step 1: 写失败测试**

`tests/e2e/test_using_alias.cpp` 追加：

```cpp
TEST_F(UsingAliasE2E, StructMemberAlias) {
    EXPECT_EQ(runSource(R"(
        struct S { using T = int32; T x; };
        int32 main() { S s; s.x = 3; return s.x - 3; }
    )", "member_alias_struct.c"), 0);
}

TEST_F(UsingAliasE2E, ClassPublicMemberAliasQualified) {
    EXPECT_EQ(runSource(R"(
        class C { public: using T = int32; };
        int32 main() { C::T v = 4; return v - 4; }
    )", "member_alias_class.c"), 0);
}

TEST_F(UsingAliasE2E, UnionMemberAlias) {
    EXPECT_EQ(runSource(R"(
        union U { using T = int32; T i; };
        int32 main() { U u; u.i = 6; return u.i - 6; }
    )", "member_alias_union.c"), 0);
}

TEST_F(UsingAliasE2E, PrivateMemberAliasInsideMethod) {
    EXPECT_EQ(runSource(R"(
        class C { using T = int32; public: T get() { T v = 5; return v; } };
        int32 main() { C c; return c.get() - 5; }
    )", "member_alias_private_method.c"), 0);
}

TEST_F(UsingAliasE2E, QualifiedMemberAliasBypassesBlockAlias) {
    EXPECT_EQ(runSource(R"(
        class C { public: using T = int32; };
        int32 main() { using T = float64; C::T v = 0; return sizeof(v) == 4 ? 0 : 1; }
    )", "member_alias_qualified.c"), 0);
}
```

`tests/frontend/test_parser.cpp`：删除 `UsingInClassBodyRejected`，替换为：

```cpp
TEST_F(ParserDeclTest, ClassMemberUsingAlias) {
    auto tu = parse("class C { public: using T = int32; };");
    ASSERT_NE(tu, nullptr);
    auto* cls = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(cls, nullptr);
    ASSERT_EQ(cls->nestedTypes.size(), 1u);
    auto* u = dynamic_cast<UsingDeclAST*>(cls->nestedTypes[0].get());
    ASSERT_NE(u, nullptr);
    EXPECT_EQ(u->name, "C_T");
    EXPECT_EQ(u->bareName, "T");
    EXPECT_FALSE(u->isBlockScope);
}
```

`tests/sema/test_semantic_analyzer.cpp` 追加：

```cpp
TEST(SemanticAnalyzerAccessTest, PrivateMemberAliasDeniedOutside) {
    Lexer lexer("t.c", "class C { using T = int32; }; int32 main() { C::T v = 1; return v; }");
    Parser parser(lexer.tokenize());
    auto ast = parser.parse();
    ASSERT_TRUE(ast != nullptr);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    bool found = false;
    for (const auto& e : analyzer.getErrors())
        if (e.code == DiagnosticCode::SemPrivateMemberAccess) found = true;
    EXPECT_TRUE(found);
}

TEST(SemanticAnalyzerAccessTest, PrivateMemberAliasInsideClassOk) {
    EXPECT_TRUE(analyzeOk(
        "class C { using T = int32; public: T get() { T v = 2; return v; } }; "
        "int32 main() { C c; return c.get() - 2; }"));
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "MemberAlias|ClassMemberUsing|PrivateMemberAlias" --output-on-failure`
Expected: 4 个 e2e FAIL（`only allowed at namespace scope` + `unknown type`），parser/sema 新测试 FAIL。

- [ ] **Step 3: 共享解析 helper**

在 `Parser.h` 声明、`Parser.cpp` 实现：
```cpp
std::unique_ptr<UsingDeclAST> Parser::parseUsingAliasDecl(bool member);
```
消费 `using IDENT = type-id ;`；失败 `skipPastSemicolon()` 返回 `nullptr`。`member=false`：`name=bare`、`isBlockScope=true`、`aliasWrapper=addLocalTypedef(bare,type)`；`member=true`：`name=qualifyTypeDeclName(bare)`、`addTypedef(name,type)`、`isBlockScope=false`。两种情况都设 `bareName=bare`。把 Task 3 的块内内联解析改为调用该 helper（行为不变）。

- [ ] **Step 4: 三个成员循环接入**

把每个循环里的 `if (isLocalUsingStart()) { rejectLocalUsing(); continue; }` 改成：
```cpp
if (isLocalUsingStart()) {
    auto u = parseUsingAliasDecl(/*member=*/true);
    if (!u) continue;
    if (/* class 循环 */ false) memberAccess["type:" + u->bareName] = currentAccess;
    nestedTypes.push_back(std::move(u));
    continue;
}
```
class 循环把 `false` 换成 `true` 并记录访问级别；struct/union 不记录。把 `memberAnns` 移入别名的 `annotations`（与嵌套类型一致）。删除已无用的 `rejectLocalUsing` 声明与定义。

- [ ] **Step 5: sema 登记 + 访问链检查**

- `visitNestedTypeDecls`：增加 `UsingDeclAST` 分支，`flatName=u->name`、`bareNested=u->bareName`；随后按现有逻辑登记 `nestedTypeAccess` 并 `visitNestedDecl`。
- `visitNestedDecl`：增加 `UsingDeclAST` → `visit(*u)`。
- `checkNestedTypeAccess`：在现有 `stripTypedef` 之前，先沿 `TypedefType` 链逐层检查 `td->name` 是否在 `nestedTypeAccess`；命中且非 Public、`currentClass != owner` 时，发 E2009（文案沿用现有 `cannot access private type '...' of class '...'`）并 `return`。
- `visitStructDeclImpl` 的字段循环加 `checkNestedTypeAccess(f.type, node);`；`visit(UnionDeclAST)` 的成员循环加 `checkNestedTypeAccess(member.type, node);`，让私有别名字段在类外也被拒。
- `TemplateInstantiator.cpp` 的 `UsingDeclAST` 克隆：补 `cloned->bareName = ud->bareName; cloned->isBlockScope = ud->isBlockScope;`。

- [ ] **Step 6: 确认 GREEN + 全量**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "MemberAlias|ClassMemberUsing|PrivateMemberAlias" && ctest -j8`
Expected: 新测试全过；全量以实际输出为准（基线约 1527，净增 7 → 约 1534）。

- [ ] **Step 7: 提交**

```bash
git add src/frontend/Parser.h src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp src/sema/TemplateInstantiator.cpp tests/frontend/test_parser.cpp tests/e2e/test_using_alias.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(PAR-24): member-scope using aliases with access control"
```

---

### Task 5: 规范文档、示例与进度记录

**Files:**
- Modify: `docs/spec/grammar.ebnf`、`docs/spec/keywords.md`、`docs/spec/semantics.md`
- Modify: `examples/all_features.cmm`
- Modify: `TODO.md`、`Progress.md`

**Interfaces:**
- Consumes: Task 1–4 的最终行为。
- Produces: 无代码 API。

- [ ] **Step 1: 更新语法**

`grammar.ebnf`：
- `class-member` 增加 `| type-alias`。
- `struct-decl` 与 `union-decl` 的两条产生式中，`{ field-decl }` 改为 `{ field-decl | type-alias }`。
- `declaration-statement = declaration | type-alias ;`
- `type-alias` 注释改为“顶层/namespace/块/聚合成员体；块内词法作用域，成员受访问控制”。

- [ ] **Step 2: 更新语义与关键字**

`semantics.md` §5.5：
- 作用域条目改为：顶层、namespace、块内、class/struct/union 成员体均可用；块内词法作用域（声明点至块尾，可遮蔽，块尾失效）；成员别名经 `C::T` 访问，class 内受 public/private/protected 控制，struct/union 恒 public；不继承到派生类。
- 追加：`using N = struct Node;` 隐式前向声明，同名定义与占位合并；`*p` 对任意层指针别名透明。
`keywords.md` 的 `using` 行改为：“唯一类型别名形式 `using X = type-id;`；顶层/namespace/块/聚合成员体（块内词法作用域，成员受访问控制）”。

- [ ] **Step 3: 示例**

`examples/all_features.cmm`：
- 文件作用域（`main` 之前）加：
```c
class AliasHolder { public: using Value = int32; Value get() { return 7; } };
using FwdNode = struct FwdNodeDef;
struct FwdNodeDef { int32 v; };
```
- 第 2 节内加：
```c
    { using LocalInt = int32; LocalInt lv = 5; check(lv == 5, "block-scoped using"); }
    AliasHolder ah; check(ah.get() == 7, "member using");
    FwdNode fnode; fnode.v = 9; check(fnode.v == 9, "forward alias");
```
- 指针别名检查后加：
```c
    int32 dx = 3; IntPtr dpx = &dx; *dpx = 4; check(dx == 4, "pointer alias deref");
```

- [ ] **Step 4: 验证示例与全量**

Run: `cd build && cmake --build . -j$(nproc) && ./bin/cmm ../examples/all_features.cmm -o /tmp/all_features && /tmp/all_features | tail -1 && ctest -j8`
Expected: `ALL FEATURES OK`；全量全绿。

- [ ] **Step 5: TODO / Progress**

- `TODO.md`：MOD-03 改为 `[x]` 并注明“占位与定义合并，`using N = struct Node;`/`struct S; S* p;` 后定义可见成员”；PAR-24 追加“作用域别名 + 成员别名 + 前向合并 + 别名解引用补丁（2026-10-24）”。
- 顶部加 2026-10-24 进度快照，测试数填 Step 4 的实际值。
- `Progress.md` 追加本轮四块改动、关键文件、验证结果。

- [ ] **Step 6: 提交**

```bash
git add docs/spec examples/all_features.cmm TODO.md Progress.md
git commit -m "docs: document scoped using, forward identity, alias deref"
```
