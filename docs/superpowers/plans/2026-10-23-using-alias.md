# using 类型别名收口（删除 typedef / type）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `using` 补到完整类型别名能力，迁走全部 `typedef`/`type` 用法，并让两者变成带提示的硬错误。

**Architecture:** `using` 右侧改用新的 `parseTypeId()`（基础类型 + 固定数组后缀 + 抽象函数指针）；sema 的 `visit(UsingDeclAST)` 补齐内联 enum 枚举量与注解/弃用语义。随后机械迁移测试与示例，最后删除 `TypedefDeclAST`/`TypeDeclAST` 并在顶层、namespace、块、聚合成员体统一报硬错误。

**Tech Stack:** C++17, LLVM 21, GoogleTest, CMake；语言实现位于 `src/frontend|src/ast|src/sema|src/codegen`。

**Spec:** `docs/superpowers/specs/2026-10-23-using-alias-design.md`

## Global Constraints

- 严格 TDD：每个任务先写失败测试，确认 RED，再最小实现，确认 GREEN。
- master 直接提交，每任务独立 commit。
- 全量基线 **1493** 测试（2 个既有 `GTEST_SKIP`）；每任务结束跑 `ctest -j8` 全绿。
- 不新增依赖；沿用现有诊断/命名风格（中文注释，英文诊断文案）。
- 历史文档 `docs/superpowers/specs|plans/` 不改；只改 `docs/spec/` 现行规范。
- 内部 `TypedefType` 保留；只删语言层的 `typedef`/`type` 与 `TypedefDeclAST`/`TypeDeclAST`。
- 构建命令：`cd build && cmake .. && cmake --build . -j$(nproc)`；测试：`ctest -j8`。

## Review Focus

- `using A = int32[4]` 的维度必须穿过 sema/codegen：下标、`sizeof`、slice 退化、设计符初始化补零。
- `using Fn = int32 (*)(int32)` 不得再静默截断：赋值、调用、作形参、作返回值都要可用。
- `using E = enum { A, B };` 的枚举量必须在同一作用域可见。
- `typedef`/`type` 出现在块或 struct/class/union 成员体时必须报带 `using` 提示的错误，不能静默忽略、级联或死循环。
- 块内 `using` 必须报“只允许 namespace 作用域”，不能退化成表达式解析级联。
- 现有模板别名 `template<typename T> using Vec = Arr<T, 2>;` 必须继续工作（Task 4/5 的全量测试覆盖）。

---

### Task 1: using 完整 type-id —— 固定数组

**Files:**
- Modify: `src/frontend/Parser.h`（`parseType()` 声明附近加 `Type* parseTypeId();`）
- Modify: `src/frontend/Parser.cpp`（`parseType()` 之后实现；`using` 分支 ~2697 改调 `parseTypeId()`）
- Create: `tests/e2e/test_using_alias.cpp`（`runSource` fixture 复制自 `tests/e2e/test_toplevel_user_types.cpp` 第 1-72 行，类名改 `UsingAliasE2E`）
- Modify: `tests/frontend/test_parser.cpp`（`TypedefArray` 之后加 `UsingArrayAliasKeepsDimensions`）

**Interfaces:**
- Produces: `Type* Parser::parseTypeId();` —— 消费 `parseType()` 已支持的 base/指针/切片/optional，再消费 `[N]` 固定数组后缀。
- `ArrayType` 字段：`elementType`、`int size`（`src/ast/Type.h:87`）。

- [ ] **Step 1: 写失败测试**

```cpp
// tests/e2e/test_using_alias.cpp
TEST_F(UsingAliasE2E, FixedArrayAlias) {
    EXPECT_EQ(runSource(R"(
        using IntArr = int32[4];
        int32 main() { IntArr a = {1, 2, 3, 4}; return a[3] - 4; }
    )", "using_arr.c"), 0);
}
TEST_F(UsingAliasE2E, MultidimArrayAlias) {
    EXPECT_EQ(runSource(R"(
        using Mat = int32[2][3];
        int32 main() { Mat m; m[1][2] = 6; return m[1][2] - 6; }
    )", "using_mat.c"), 0);
}
TEST_F(UsingAliasE2E, ArrayAliasSizeofAndSlice) {
    EXPECT_EQ(runSource(R"(
        using IntArr = int32[10];
        int32 main() {
            IntArr a; a[9] = 9;
            int32[] s = a;
            return (sizeof(a) == 40 && s.len == 10) ? 0 : 1;
        }
    )", "using_arr_meta.c"), 0);
}
TEST_F(UsingAliasE2E, SliceAliasStillWorks) {
    EXPECT_EQ(runSource(R"(
        using Sl = int32[];
        int32 main() { int32 a[3]; a[0] = 1; Sl s = a; return s[0] - 1; }
    )", "using_slice.c"), 0);
}
TEST_F(UsingAliasE2E, ArrayAliasDesignatedZeroFill) {
    EXPECT_EQ(runSource(R"(
        using Arr = int32[4];
        int32 main() { Arr a = {[1] = 5}; return a[0]; }
    )", "using_arr_di.c"), 0);
}
```

```cpp
// tests/frontend/test_parser.cpp
TEST_F(ParserDeclTest, UsingArrayAliasKeepsDimensions) {
    auto tu = parse("using IntArr = int32[4];");
    ASSERT_NE(tu, nullptr);
    ASSERT_EQ(tu->declarations.size(), 1u);
    auto u = dynamic_cast<UsingDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(u, nullptr);
    ASSERT_EQ(u->aliasedType->kind, TypeKind::Array);
    EXPECT_EQ(static_cast<ArrayType*>(u->aliasedType)->size, 4);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake .. && cmake --build . -j$(nproc) && ctest -R "UsingAliasE2E|UsingArrayAliasKeepsDimensions" --output-on-failure`
Expected: FAIL —— 数组别名当前只得到 `int32`，e2e 解析/语义失败，parser 断言 kind 不是 Array。

- [ ] **Step 3: 实现**

`parseTypeId()`：先 `Type* type = parseType();`；若下一个 token 是 `[` 且其后不是 `]`，循环读 `[NUMBER]` 收集维度（非 NUMBER 报 `array size must be an integer literal` 并返回 nullptr）；从右到左 `new ArrayType(elem, dims[i])`（`i = n-1 .. 1`），最后 `new ArrayType(elem, dims[0])`。`using` 分支把 `parseType()` 换成 `parseTypeId()`。

- [ ] **Step 4: 确认 GREEN**

Run: 同上过滤器；再 `ctest -j8`。Expected: 全绿（1493 + 6）。

- [ ] **Step 5: Commit**

```bash
git add src/frontend/Parser.h src/frontend/Parser.cpp tests/e2e/test_using_alias.cpp tests/frontend/test_parser.cpp
git commit -m "feat(PAR-24): using aliases accept fixed-size array type-ids"
```

---

### Task 2: using 完整 type-id —— 函数指针

**Files:**
- Modify: `src/frontend/Parser.cpp`（`parseTypeId()` 增加抽象函数指针分支）
- Modify: `tests/e2e/test_using_alias.cpp`（3 个用例）

**Interfaces:**
- Consumes: `Type* Parser::parseFunctionPointerDeclarator(Type* returnType, std::string& outName, bool requireName);`（`requireName=false` 已支持无名 `(*)(params)`）。
- Produces: `using Fn = int32 (*)(int32, int32);` 的别名类型为 `Pointer(FunctionType)`。

- [ ] **Step 1: 写失败测试**

```cpp
TEST_F(UsingAliasE2E, FnPtrAliasCall) {
    EXPECT_EQ(runSource(R"(
        using Fn = int32 (*)(int32, int32);
        int32 add(int32 a, int32 b) { return a + b; }
        int32 main() { Fn f = add; return f(2, 3) - 5; }
    )", "using_fnptr.c"), 0);
}
TEST_F(UsingAliasE2E, FnPtrAliasParam) {
    EXPECT_EQ(runSource(R"(
        using Fn = int32 (*)(int32, int32);
        int32 add(int32 a, int32 b) { return a + b; }
        int32 apply(Fn f, int32 x) { return f(x, 1); }
        int32 main() { return apply(add, 4) - 5; }
    )", "using_fnptr_param.c"), 0);
}
TEST_F(UsingAliasE2E, FnPtrAliasReturn) {
    EXPECT_EQ(runSource(R"(
        using Fn = int32 (*)(int32, int32);
        int32 add(int32 a, int32 b) { return a + b; }
        Fn pick() { return add; }
        int32 main() { Fn f = pick(); return f(3, 4) - 7; }
    )", "using_fnptr_ret.c"), 0);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "UsingAliasE2E.*FnPtr" --output-on-failure`
Expected: FAIL —— 别名被截断成 `int32`，赋值/调用报错。

- [ ] **Step 3: 实现**

在 `parseTypeId()` 的数组后缀之后加：

```cpp
std::string ignored;
if (Type* fp = parseFunctionPointerDeclarator(type, ignored, /*requireName=*/false))
    type = fp;
```

- [ ] **Step 4: 确认 GREEN**

Run: 同上过滤器；再 `ctest -j8`。Expected: 全绿（1502）。

- [ ] **Step 5: Commit**

```bash
git add src/frontend/Parser.cpp tests/e2e/test_using_alias.cpp
git commit -m "feat(PAR-24): using aliases accept function-pointer type-ids"
```

---

### Task 3: 内联 enum 枚举量 + 注解/弃用语义

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit(UsingDeclAST&)` ~6109）
- Modify: `tests/e2e/test_using_alias.cpp`（内联 enum 用例）
- Modify: `tests/sema/test_annotation_semantics.cpp`（`ReprCOnTypedefE2011` 附近）

**Interfaces:**
- `validateAnnotations(const std::vector<Annotation>&, ASTNode&, const std::string& target, Type* = nullptr)`：target 传 `"using"`。
- `TypeContext::getTypedef(name)` 返回存储的 `TypedefType*`；`enumConstants` 为 sema 成员。

- [ ] **Step 1: 写失败测试**

```cpp
TEST_F(UsingAliasE2E, InlineEnumAliasExposesEnumerators) {
    EXPECT_EQ(runSource(R"(
        using E = enum : uint16 { Lo = 1, Hi = 2 };
        int32 main() { E e = Hi; return (int32)e + Lo - 3; }
    )", "using_enum.c"), 0);
}
```

```cpp
TEST_F(AnnotationSemantics, ReprCOnUsingE2011) {
    EXPECT_TRUE(hasError("[[repr(C)]] using T = int32;",
                         "annotation 'repr' is not valid on using"));
}
TEST_F(AnnotationSemantics, DeprecatedUsingW3004) {
    EXPECT_TRUE(hasWarning(
        "[[deprecated(\"old\")]] using T = int32;\n"
        "int32 main() { T x = 1; return x; }",
        "is deprecated: old"));
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "InlineEnumAlias|ReprCOnUsing|DeprecatedUsing" --output-on-failure`
Expected: FAIL —— 枚举量未注册；`repr` 被静默接受；弃用不告警。

- [ ] **Step 3: 实现**

`visit(UsingDeclAST& node)` 改为：先 `validateAnnotations(node.annotations, node, "using")`；`typeCtx->addTypedef(node.name, node.aliasedType)`；取 `getTypedef(node.name)`，按 `node.annotations` 中 `deprecated`（含单字符串实参）设置 `deprecated/deprecatedMsg`；若 `stripTypedef(node.aliasedType)` 为 `EnumType`，遍历 `values` 写入 `enumConstants[scopedName(val.first)] = {enumType, val.second}`；最后 `declare(node.name, node.aliasedType)`。

- [ ] **Step 4: 确认 GREEN**

Run: 同上过滤器；再 `ctest -j8`。Expected: 全绿（1505）。

- [ ] **Step 5: Commit**

```bash
git add src/sema/SemanticAnalyzer.cpp tests/e2e/test_using_alias.cpp tests/sema/test_annotation_semantics.cpp
git commit -m "feat(PAR-24): using aliases expose inline enum values and annotations"
```

---

### Task 4: 迁移测试与示例到 using

本任务不改变行为，只把语言源里的 `typedef`/`type` 换成 `using`。每完成一组跑一次全量。

**Files:**
- Modify: `tests/e2e/test_demo_regressions.cpp:290-291,313`
- Modify: `tests/e2e/test_enum_underlying.cpp:121`
- Modify: `tests/e2e/test_generics.cpp:313`
- Modify: `tests/e2e/test_namespace.cpp:130-135`
- Modify: `tests/e2e/test_optional_result.cpp:215`
- Modify: `tests/e2e/test_anon_type.cpp:100,127,148`
- Modify: `tests/e2e/test_designated_init.cpp:212`
- Modify: `tests/e2e/test_union.cpp:181-182`
- Modify: `tests/e2e/test_atomic.cpp:178`
- Modify: `tests/frontend/test_parser.cpp:1510-1535,1660-1668,1729-1745`
- Modify: `tests/sema/test_semantic_analyzer.cpp:1906,1911`
- Modify: `tests/sema/test_annotation_semantics.cpp`（删除 `ReprCOnTypedefE2011`）
- Modify: `tests/sema/test_compile_time.cpp:369-376`
- Modify: `tests/ast/test_decl_nodes.cpp:71-77`
- Modify: `tests/codegen/test_codegen_context.cpp:469-474`
- Modify: `examples/all_features.cdd:68-98`

**Interfaces:**
- Consumes: Task 1 的数组别名、Task 2 的函数指针别名、Task 3 的内联 enum/注解。

- [ ] **Step 1: 迁移 e2e（9 个文件）**

```text
typedef int32 IntArr[4];                 -> using IntArr = int32[4];
typedef int32 Mat[2][3];                 -> using Mat = int32[2][3];
typedef int32 (*BinOp)(int32, int32);    -> using BinOp = int32 (*)(int32, int32);
typedef enum : uint16 { ... } Small;     -> using Small = enum : uint16 { ... };
typedef int32 b;                         -> using b = int32;
namespace A { typedef int32 Handle; }    -> namespace A { using Handle = int32; }
typedef int32? Opt;                      -> using Opt = int32?;
typedef struct { ... } S;                -> using S = struct { ... };
typedef struct { ... } Q;                -> using Q = struct { ... };
typedef int32 Arr[4];                    -> using Arr = int32[4];
typedef struct S2 { ... } S2;            -> using S2 = struct S2 { ... };
typedef union U2 { ... } U2;             -> using U2 = union U2 { ... };
typedef int32 myint;                     -> using myint = int32;
```

同步把注释/测试名/`runSource` 文件名里的 `typedef` 字样改成 `using`/`alias`。

Run: `cd build && cmake --build . -j$(nproc) && ctest -j8`
Expected: 全绿。

- [ ] **Step 2: 迁移单元测试**

- `test_parser.cpp`：6 个 typedef 测试改为 `UsingDeclAST`：
  - `UsingDecl`：`using Integer = int32;`
  - `UsingAnonStruct`：`using Point = struct { int32 x; int32 y; };`
  - `UsingUsage`：`using Integer = int32; Integer x = 5;`
  - `ForwardStructDecl`：`struct Node; struct Node { int32 data; Node* next; };`（断言第 1 个 `StructDeclAST::isForwardDecl == true`，第 2 个名 `Node`）
  - `UsingPointer`：`using IntPtr = int32*; IntPtr p;`（断言别名 kind Pointer、变量 type kind Typedef）
  - `UsingArray`：`using IntArr = int32[10]; IntArr a;`（断言别名 kind Array、变量 type kind Typedef）
- `test_semantic_analyzer.cpp`：两处 `typedef int32 MyInt;` → `using MyInt = int32;`。
- `test_annotation_semantics.cpp`：删除 `ReprCOnTypedefE2011`（Task 3 已有 using 版本）。
- `test_compile_time.cpp`：去掉死分支里的 `typedef int32 GhostInt;` 与循环中的 `"GhostInt x;"`，注释改为“指针/数组包装层”。
- `test_decl_nodes.cpp`：`TypedefDeclAST` 测试改为 `UsingDeclAST`（构造签名相同），断言 `name == "MyInt"`、`aliasedType->kind == TypeKind::Int32`。
- `test_codegen_context.cpp`：`TypedefDeclAST` 改为 `UsingDeclAST`，仍只断言 `verifyModule()`。

Run: `cd build && cmake --build . -j$(nproc) && ctest -j8`
Expected: 全绿。

- [ ] **Step 3: 迁移示例**

`examples/all_features.cdd` 第 2 节：5 个 typedef 改 using（`Integer`、`Point = struct {...}`、`IntPtr`、`IntArr = int32[10]`、`Opt`），`type Speed = float64;` → `using Speed = float64;`；标题/注释/`check` 文案里的 `typedef`/`type` 改为 `using`。

Run:
```bash
./build/bin/cdd examples/all_features.cdd -o /tmp/all_features && /tmp/all_features | tail -1
```
Expected: 退出 0，最后一行 `ALL FEATURES OK`。

- [ ] **Step 4: 全量校验**

Run: `cd build && ctest -j8`
Expected: 全绿（计数为 Task 3 的基线 + 0，仅改名）。

- [ ] **Step 5: Commit**

```bash
git add tests examples/all_features.cdd
git commit -m "test: migrate typedef/type usages to using"
```

---

### Task 5: typedef/type 硬错误 + 删除 AST 节点

**Files:**
- Modify: `src/frontend/Parser.h`（删 `parseTypedefDecl()`；加私有 `isRemovedTypeAliasStart()`、`isLocalUsingStart()`、`rejectRemovedTypeAlias()`、`rejectLocalUsing()`、`skipPastSemicolon()`）
- Modify: `src/frontend/Parser.cpp`（`parseDeclarationImpl` ~2677/2720；`parseStatement` ~1490；三个成员循环 ~3701/4060/4426；删 `parseTypedefDecl` ~4646）
- Modify: `src/ast/Decl.h`（删 `TypedefDeclAST`、`TypeDeclAST`）
- Modify: `src/ast/Decl.cpp`（删两者 `codegen`）
- Modify: `src/sema/SemanticAnalyzer.h` / `.cpp`（删 visit 声明/定义与 dynamic_cast 分派）
- Modify: `src/sema/TemplateInstantiator.cpp`（删两段克隆分支）
- Modify: `tests/frontend/test_parser.cpp`（加 6 个硬错误测试）

**Interfaces:**
- Produces: 旧形式诊断（解析期 error，消息必含 `use 'using Name = Type;'` 或 `only allowed at namespace scope`）。
- 语义：`using` 仍是唯一别名；`TypedefType` 内部类型不变。

- [ ] **Step 1: 写失败测试**

在 `test_parser.cpp` 的 `parse()` 辅助函数后加：

```cpp
static std::vector<Diagnostic> parseErrors(const std::string& source) {
    Lexer lexer("alias_err.c", source);
    Parser parser(lexer.tokenize());
    parser.parse();
    return parser.getErrors();
}
```

```cpp
TEST_F(ParserDeclTest, TypedefRejectedWithHint) {
    auto errs = parseErrors("typedef int32 T;");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("'typedef' has been removed"), std::string::npos);
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}
TEST_F(ParserDeclTest, TypeRejectedWithHint) {
    auto errs = parseErrors("type T = int32;");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("'type' has been removed"), std::string::npos);
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}
TEST_F(ParserDeclTest, TypedefInBlockRejected) {
    auto errs = parseErrors("int32 main() { typedef int32 T; return 0; }");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}
TEST_F(ParserDeclTest, TypeInClassBodyRejected) {
    auto errs = parseErrors("class C { public: typedef int32 T; int32 x; };");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}
TEST_F(ParserDeclTest, TypeInBlockRejected) {
    auto errs = parseErrors("int32 main() { type T = int32; return 0; }");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}
TEST_F(ParserDeclTest, TypedefInStructBodyRejected) {
    auto errs = parseErrors("struct S { typedef int32 T; int32 x; };");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("using Name = Type;"), std::string::npos);
}
TEST_F(ParserDeclTest, UsingInBlockRejected) {
    auto errs = parseErrors("int32 main() { using T = int32; return 0; }");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("only allowed at namespace scope"), std::string::npos);
}
TEST_F(ParserDeclTest, UsingInClassBodyRejected) {
    auto errs = parseErrors("class C { public: using T = int32; };");
    ASSERT_FALSE(errs.empty());
    EXPECT_NE(errs[0].message.find("only allowed at namespace scope"), std::string::npos);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "TypedefRejected|TypeRejected|TypedefInBlock|TypeInBlock|TypedefInStruct|TypeInClass|UsingInBlock|UsingInClass" --output-on-failure`
Expected: FAIL —— typedef/type 仍解析成功；成员体静默忽略；块内 using 报无关解析错误。

- [ ] **Step 3: 实现诊断**

- `isRemovedTypeAliasStart()`：`TOKEN_TYPEDEF`，或 `IDENTIFIER("type") IDENTIFIER ASSIGN`。
- `isLocalUsingStart()`：`IDENTIFIER("using") IDENTIFIER ASSIGN`。
- `rejectRemovedTypeAlias()`：按关键字发对应 error，然后 `skipPastSemicolon()`。
- `rejectLocalUsing()`：发 scope error，然后 `skipPastSemicolon()`。
- `skipPastSemicolon()`：括号/花括号深度为 0 时消费 `;` 返回；遇到深度 0 的 `}` 停止不消费；EOF 停止。
- `parseDeclarationImpl`：typedef 分支替换为 `rejectRemovedTypeAlias(); return nullptr;`；`type` 分支替换为同一判定。
- `parseStatement`：注解处理之后、声明路径之前，若 `isRemovedTypeAliasStart() || isLocalUsingStart()`，调用对应 reject 并 `return std::make_unique<NullStmtAST>();`。
- 三个成员循环：`parseAnnotations()` 之后、嵌套类型判定之前，做同样判定并 `continue`。

- [ ] **Step 4: 删除旧实现**

- Parser：删 `parseTypedefDecl` 声明/定义与 `type` 分支残留。
- `Decl.h`：删 `TypedefDeclAST`、`TypeDeclAST` 类。
- `Decl.cpp`：删两者 `codegen`。
- `SemanticAnalyzer.h/.cpp`：删 `visit(TypedefDeclAST&)`、`visit(TypeDeclAST&)` 及 `DeclStmtAST`/`TopLevel` 分派中的 dynamic_cast 分支。
- `TemplateInstantiator.cpp`：删 `TypedefDeclAST`/`TypeDeclAST` 克隆分支。
- 保留 `TypedefType` 与 `TypeContext::addTypedef/getTypedef`。

- [ ] **Step 5: 确认 GREEN + 残留检查**

Run:
```bash
cd build && cmake --build . -j$(nproc) && ctest -j8
cd .. && grep -rnE "typedef|[[:space:]]type[[:space:]]+[A-Za-z_].*=" tests examples libs --include=*.c --include=*.cdd
grep -rn "TypedefDeclAST\|TypeDeclAST" src tests
```
Expected: 全绿（1513）；两条 grep 均无输出（`resources/main.c` 不在检查范围：未被构建消费）。

- [ ] **Step 6: Commit**

```bash
git add src tests
git commit -m "feat(PAR-24)!: reject typedef/type and keep using as the only alias syntax"
```

---

### Task 6: 文档与进度

**Files:**
- Modify: `docs/spec/grammar.ebnf:119-121`
- Modify: `docs/spec/keywords.md`（删 `typedef`/`type` 行，保留 `using` 上下文说明）
- Modify: `docs/spec/semantics.md`（别名语义：数组/函数指针/内联 enum/作用域/硬错误）
- Modify: `docs/spec/abi.md`、`docs/spec/conversions.md`、`docs/spec/fixed-width-types.md`（typedef 措辞改 using）
- Modify: `TODO.md`（PAR-24/DEC-16 关闭，PAR-05 补记，进度快照）
- Modify: `Progress.md`（追加本轮记录）

**Interfaces:**
- Consumes: Task 1-5 的最终行为与测试计数。

- [ ] **Step 1: 更新语法与关键字**

`grammar.ebnf` 的 `type-alias` 三条产生式替换为：

```ebnf
type-alias          = 'using' , identifier , '=' , type-id , ';' ;
type-id             = type , { '[' , integer-literal , ']' }
                    , [ '(' , '*' , ')' , '(' , [ parameter-list ] , ')' ] ;
```

`keywords.md` 删除 `typedef` 与 `type` 行；`using` 标注为“上下文标识符，仅顶层/namespace”。

- [ ] **Step 2: 更新语义文档**

在 `semantics.md` 别名小节写明：`using Name = type-id;` 为唯一形式；支持固定数组、函数指针、切片、optional、模板实例、内联/命名聚合与 enum；枚举量在别名作用域可见；`typedef`/`type` 为硬错误；块/类内别名不支持。`abi.md`/`conversions.md`/`fixed-width-types.md` 仅替换措辞。

- [ ] **Step 3: 更新 TODO/Progress**

- `TODO.md`：`PAR-24` 与 `DEC-16` 标 `[x]`，写明“删除 typedef/type，只留 using”；`PAR-05` 增补 using 完整 type-id；新增 2026-10-23 进度快照（全量计数）。
- `Progress.md`：记录根因/改动/迁移/验证/遗留（块内 using、前向别名身份、`*p` 解引用既有缺陷）。

- [ ] **Step 4: 校验**

Run:
```bash
grep -rn "typedef" docs/spec || true
grep -n "type-alias\|type-id" docs/spec/grammar.ebnf
cd build && ctest -j8
```
Expected: 第一条无输出；第二条显示新的 `type-alias`/`type-id` 产生式；全量绿。

- [ ] **Step 5: Commit**

```bash
git add docs/spec TODO.md Progress.md
git commit -m "docs(PAR-24): document using as the sole alias syntax"
```
