# Module/Namespace 职责分离与 Record 文法统一 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `module` 固定为文件/导入边界、`namespace` 固定为 API 名字边界；把 `struct`/`class` 的 parser 统一到一套 record 文法，并为 `struct` 补齐访问段与方法/静态/嵌套成员的一致解析。

**Architecture:** 先把 module/import/namespace 的拼写与诊断分开，再用一个 `parseRecordDecl(bool isClass)` 取代 `parseStructDecl`/`parseClassDecl` 的重复成员循环。`StructDeclAST` 增加 `hasAccessSections`；纯数据 `struct` 仍走 `StructType`，带方法或访问段的 `struct` 复用 `ClassType`，布局不变。`union` 保持独立。

**Spec:** `docs/superpowers/specs/2026-11-09-module-namespace-record-design.md`

**Tech Stack:** C++20、LLVM 21、CMake、GoogleTest/CTest、Cmm 自举测试。

## Global Constraints

- 语言类型使用定宽拼写：`int32`、`usize`；入口写 `int32 main()`。
- 语言无 `int/float/double`、无预处理器；测试源文件后缀 `.cmm`。
- module 路径只用 `.`；namespace 名只用 `::`；类型/值限定访问仍用 `::`。
- `struct` 默认 public，`class` 默认 private；`union` 不获得方法/静态/访问段/继承。
- `string`/`string_view` 仍是编译器 prelude 类型，本计划不迁移。
- 每任务 TDD：先写失败测试，确认 RED，再实现，再跑目标测试与全量回归，最后提交。
- 构建：`cmake --build build -j8`。
- 全量：`ctest --test-dir build -j8 --output-on-failure`。
- 完成前必须跑 `examples/all_features.cmm`，输出仍为 `ALL FEATURES OK`。

## Review Focus

- **模块名不变成 namespace**：`import std.vec;` 后 `std::Vec` 可用，`Vec` 不可用；module 名不产生 `std.vec::` 访问。
- **根导出仍可直接访问**：`module util; export int32 helper();` 导入后 `helper()` 可用。
- **拼写错误不级联**：`module a::b;`、`import a::b;`、`namespace A.B` 各自的诊断只报一次，不吞后续声明。
- **struct 访问段**：`struct S { private: int32 x; public: int32 get(); }` 可解析；类外访问 `x` 报 E2009，方法内可访问。
- **内部类型选择**：带方法/访问段的 `struct` 注册为 `ClassType`，但 `[[repr(C)]]` 仍按 struct 规则允许；不要误用 `getStruct` 拿到空类型。
- **attached declarator**：`struct S { int32 get(){...} } s;` 与 `class C { ... } c;` 都保留类型 AST 和变量，字段/方法不丢。

---

### Task 1: Module/Namespace 拼写与查找职责分离

**Files:**
- Modify: `src/frontend/Parser.cpp`（`parse()` 的 module/import 分支；`parseNamespaceDeclImpl`）
- Modify: `docs/spec/grammar.ebnf`（module/import/namespace 产生式）
- Modify: `docs/spec/modules.md`（§2、§3、§4）
- Modify: `examples/all_features.cmm:509`
- Modify: `tests/e2e/test_namespace.cpp:108,147`
- Test: `tests/frontend/test_parser.cpp`
- Test: `tests/driver/test_module_visibility.cpp`

**Interfaces:**
- Consumes: 现有 `Parser::parse()`、`parseNamespaceDeclImpl()`、`ModuleLoader` 的点分文件名映射。
- Produces: 点分 module/import 名；`::` namespace 名；诊断文本：
  - `module names use '.'; '::' is reserved for namespaces`
  - `import specifiers use '.' or a quoted path; '::' is reserved for namespaces`
  - `namespace names use '::'; '.' is reserved for module paths`

- [ ] **Step 1: 写失败测试（parser 拼写）**

在 `tests/frontend/test_parser.cpp` 增加：

```cpp
TEST_F(ParserTest, ModulePathRejectsColonColon) {
    auto errors = parseErrors("module a::b;\nint32 main() { return 0; }");
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].message.find("module names use '.'"), std::string::npos);
}

TEST_F(ParserTest, ImportSpecifierRejectsColonColon) {
    auto errors = parseErrors("import a::b;\nint32 main() { return 0; }");
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].message.find("import specifiers use '.'"), std::string::npos);
}

TEST_F(ParserTest, NamespaceDeclRejectsDotName) {
    auto errors = parseErrors("namespace A.B { int32 f() { return 1; } }");
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].message.find("namespace names use '::'"), std::string::npos);
}
```

- [ ] **Step 2: 写失败测试（查找职责）**

在 `tests/driver/test_module_visibility.cpp` 增加三个测试，复用现有 `write`/`loadMain`/`jitRun`：

```cpp
TEST_F(ModuleVisibilityTest, RootExportIsUnqualified) {
    write(dir / "util.cmm", "module util;\nexport int32 helper() { return 5; }\n");
    std::vector<std::string> errors;
    auto ast = loadMain("import util;\nint32 main() { return helper() - 5; }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
    EXPECT_EQ(jitRun(*ast), 0);
}

TEST_F(ModuleVisibilityTest, NamespaceMemberRequiresQualification) {
    write(dir / "geom.cmm",
          "module geom;\n"
          "export namespace g { int32 add(int32 a, int32 b) { return a + b; } }\n");
    std::vector<std::string> errors;
    auto ast = loadMain("import geom;\nint32 main() { return add(2, 3); }\n", errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(hasErrorContaining(analyzer.getErrors(), "undeclared function 'add'"));
}

TEST_F(ModuleVisibilityTest, ModuleNameDoesNotCreateQualifier) {
    write(dir / "std" / "vec.cmm",
          "module std.vec;\n"
          "export namespace std { struct Vec { int32 x; }; }\n");
    std::vector<std::string> errors;
    auto ast = loadMain("import std.vec;\nint32 main() { std::Vec v = {}; return v.x; }\n",
                        errors);
    ASSERT_TRUE(errors.empty()) << (errors.empty() ? "" : errors[0]);
    SemanticAnalyzer analyzer;
    analyzer.analyze(*ast);
    EXPECT_TRUE(analyzer.getErrors().empty());
}
```

- [ ] **Step 3: 运行确认 RED**

```bash
cmake --build build -j8
ctest --test-dir build -R 'ParserTest.(ModulePathRejectsColonColon|ImportSpecifierRejectsColonColon|NamespaceDeclRejectsDotName)|ModuleVisibilityTest.(RootExportIsUnqualified|NamespaceMemberRequiresQualification|ModuleNameDoesNotCreateQualifier)' --output-on-failure
```

Expected: 新测试失败；前三个是未报错或错误文本不匹配，后三个中 `NamespaceMemberRequiresQualification` 目前应已通过（用于钉行为），其余按现状确认。

- [ ] **Step 4: 实现 module/import 点分限制**

在 `Parser::parse()` 的 module 分支中，模块名只接受：

```text
identifier { '.' identifier }
```

遇到 `TOKEN_COLON_COLON` 时调用 `error("module names use '.'; '::' is reserved for namespaces", *peek())`，消费该 token 后继续到 `;`，避免级联。import 分支同理；import 仍接受字符串字面量路径。

- [ ] **Step 5: 实现 namespace 的 `::` 限制**

`parseNamespaceDeclImpl()` 只接受 `identifier { '::' identifier }`，拼接时保留 `::`（`name += "::" + part->lexeme`），不再把 namespace 名写成点分形式。遇到 `TOKEN_DOT` 时调用 `error("namespace names use '::'; '.' is reserved for module paths", *peek())`，消费后继续解析 `{...}`，保证只报一次。

- [ ] **Step 6: 迁移仓库内 dot namespace**

```cmm
namespace A.B { ... }   // 改为
namespace A::B { ... }
```

涉及 `examples/all_features.cmm:509`、`tests/e2e/test_namespace.cpp:108,147`。

- [ ] **Step 7: 更新规范**

`docs/spec/grammar.ebnf`：

```ebnf
module-name         = identifier , { '.' , identifier } ;
module-decl         = 'module' , module-name , ';' ;
import-decl         = 'import' , ( module-name | string-literal ) , ';' ;
namespace-decl      = 'namespace' , qualified-name , '{' , ... , '}' ;
```

`docs/spec/modules.md` 明确：

- module/import 使用点分路径；
- namespace 使用 `::`；
- import 只控制可见性，不产生模块限定名；
- 根导出非限定、namespace 成员限定；
- `import std.vec;` 的 API 是 `std::Vec`，不是 `Vec` 或 `std.vec::Vec`。

- [ ] **Step 8: GREEN + 目标回归**

```bash
cmake --build build -j8
ctest --test-dir build -R 'ParserTest.(ModulePathRejectsColonColon|ImportSpecifierRejectsColonColon|NamespaceDeclRejectsDotName)|ModuleVisibilityTest|NamespaceE2E' --output-on-failure
```

Expected: 全部 PASS。

- [ ] **Step 9: Commit**

```bash
git add src/frontend/Parser.cpp docs/spec/grammar.ebnf docs/spec/modules.md examples/all_features.cmm tests/e2e/test_namespace.cpp tests/frontend/test_parser.cpp tests/driver/test_module_visibility.cpp
git commit -m "feat(parser): separate module paths from namespace names"
```

---

### Task 2: 统一 struct/class 的 record parser

**Files:**
- Modify: `src/ast/Decl.h`（`StructDeclAST::hasAccessSections`）
- Modify: `src/frontend/Parser.h`（`parseRecordDecl(bool isClass)`）
- Modify: `src/frontend/Parser.cpp`（`parseStructDecl`/`parseClassDecl` 合并）
- Create: `tests/frontend/test_record_grammar.cpp`

**Interfaces:**
- Consumes: `parseQualifiedTypeName()`、`parseFunctionDecl()`、`parseMemberFieldDecl()`、`registerNestedDeclType()`、`isAnonymousAggregate()`、`parseUsingAliasDecl()`。
- Produces:
  - `std::unique_ptr<StructDeclAST> Parser::parseRecordDecl(bool isClass);`
  - `StructDeclAST::hasAccessSections`（bool，默认 false）
  - `StructDeclAST::isClassDecl`：`class` 为 true，`struct` 为 false
  - `StructDeclAST::memberAccess`：两种 record 都记录字段/方法/静态/嵌套类型的访问级别

- [ ] **Step 1: 写失败测试（parser 结构）**

创建 `tests/frontend/test_record_grammar.cpp`。该文件自带与 `test_parser.cpp` 等价的文件内 `lex`/`parse` 辅助（`test_parser.cpp` 的辅助是 `static`，不能跨文件复用）：

```cpp
TEST(RecordGrammarTest, StructParsesAccessSectionsAndMethods) {
    auto tu = parse("struct S { private: int32 x; public: int32 get() { return x; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* s = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->isClassDecl);
    EXPECT_TRUE(s->hasAccessSections);
    EXPECT_EQ(s->memberAccess.at("x"), AccessLevel::Private);
    ASSERT_EQ(s->methods.size(), 1u);
}

TEST(RecordGrammarTest, ClassParsesSameMemberSet) {
    auto tu = parse("class C { int32 x; public: int32 get() { return x; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* c = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(c, nullptr);
    EXPECT_TRUE(c->isClassDecl);
    EXPECT_EQ(c->memberAccess.at("x"), AccessLevel::Private);
    ASSERT_EQ(c->methods.size(), 1u);
}

TEST(RecordGrammarTest, StructDefaultAccessIsPublic) {
    auto tu = parse("struct S { int32 x; int32 get() { return x; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* s = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(s->hasAccessSections);
    EXPECT_EQ(s->memberAccess.at("x"), AccessLevel::Public);
}

TEST(RecordGrammarTest, StructParsesStaticNestedAndOperator) {
    auto tu = parse(
        "struct S { static int32 count; struct Inner { int32 v; }; "
        "int32 operator+(S other) { return count; } };");
    ASSERT_TRUE(tu != nullptr);
    auto* s = dynamic_cast<StructDeclAST*>(tu->declarations[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->staticMembers.size(), 1u);
    EXPECT_EQ(s->nestedTypes.size(), 1u);
    ASSERT_EQ(s->methods.size(), 1u);
}
```

- [ ] **Step 2: 运行确认 RED**

```bash
cmake --build build -j8
ctest --test-dir build -R 'RecordGrammarTest' --output-on-failure
```

Expected: `StructParsesAccessSectionsAndMethods` 解析失败；`ClassParsesSameMemberSet` 通过；`StructDefaultAccessIsPublic`/`StructParsesStaticNestedAndOperator` 当前部分通过（用于钉行为）。

- [ ] **Step 3: 给 AST 加标志**

在 `StructDeclAST` 增加：

```cpp
bool hasAccessSections = false;
```

放在 `isClassDecl` 附近；`TemplateInstantiator` 克隆 `StructDeclAST` 时同步复制（本任务先加字段，Task 3 补克隆）。

- [ ] **Step 4: 提取 `parseRecordDecl(bool isClass)`**

在 `Parser.h` 增加私有声明：

```cpp
std::unique_ptr<StructDeclAST> parseRecordDecl(bool isClass);
```

把 `parseClassDecl()` 的完整实现改为 `parseRecordDecl(/*isClass=*/true)`；`parseStructDecl()` 改为 `parseRecordDecl(/*isClass=*/false)`。公共函数体保留：

- 消费 `TOKEN_STRUCT`/`TOKEN_CLASS`；
- `parseQualifiedTypeName()` + 匿名 `__anon_N`；
- 继承子句（含模板基类拼写）；
- 前向声明；
- 体解析循环。

- [ ] **Step 5: 统一成员循环**

在 `parseRecordDecl` 中：

```cpp
AccessLevel currentAccess = isClass ? AccessLevel::Private : AccessLevel::Public;
bool hasAccessSections = false;
```

访问段分支对两种 record 都生效，置 `hasAccessSections = true`。字段/方法/静态/嵌套类型/using/匿名成员/运算符全部走同一套分支，并像 class 一样写入 `memberAccess`。`isClassDecl` 只在最后设为 `isClass`，`hasAccessSections` 写入 AST。

- [ ] **Step 6: 保持 struct 的 attached declarator 语义**

`parseRecordDecl(false)` 返回的 AST 仍需被 `parseDeclaration` 识别。若本任务后 `parseDeclaration` 的 struct 分支仍只调用 `parseDeclarationFromBaseType`，先不要改；Task 3 统一类型选择时再处理 attached variable。此步只保证 parser 测试通过。

- [ ] **Step 7: 更新文法**

`docs/spec/grammar.ebnf` 用统一产生式替换 `struct-decl`/`class-decl`：

```ebnf
record-decl        = record-keyword , [ identifier ]
                     [ ':' , [ access-specifier ] , qualified-name ]
                     [ '{' , { record-member } , '}' ] , ';' ;
record-keyword     = 'struct' | 'class' ;
record-member      = access-section | field-decl | function-decl
                   | operator-decl | type-alias | nested-type-decl
                   | static-member-decl ;
```

`union-decl` 保持独立；`field-decl` 仍共用。

- [ ] **Step 8: GREEN + 目标回归**

```bash
cmake --build build -j8
ctest --test-dir build -R 'RecordGrammarTest|ParserTest|ClassSupport|TemplateRegistryTest' --output-on-failure
```

Expected: 全部 PASS。

- [ ] **Step 9: Commit**

```bash
git add src/ast/Decl.h src/frontend/Parser.h src/frontend/Parser.cpp docs/spec/grammar.ebnf tests/frontend/test_record_grammar.cpp
git commit -m "refactor(parser): unify struct and class record grammar"
```

---

### Task 3: Record 语义路由与访问控制

**Files:**
- Modify: `src/frontend/Parser.cpp`（struct 分支类型注册、attachedVars）
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visitStructDeclImpl` 的 `isClass` 判定）
- Modify: `src/sema/TemplateInstantiator.cpp`（复制 `hasAccessSections`）
- Test: `tests/sema/test_semantic_analyzer.cpp`
- Test: `tests/e2e/test_record_grammar.cpp`（新建或追加）

**Interfaces:**
- Consumes: Task 2 的 `StructDeclAST::hasAccessSections`、`memberAccess`。
- Produces:
  - `isClassRecord = node.isClassDecl || !node.methods.empty() || node.hasAccessSections`
  - 带方法/访问段的 `struct` 注册/复用 `ClassType`；纯数据 `struct` 仍 `StructType`。
  - `struct S { ... } s;` 的 attached variable 使用正确的 record 类型，不创建空 `StructType`。

- [ ] **Step 1: 写失败测试（sema 访问）**

在 `tests/sema/test_semantic_analyzer.cpp` 增加：

```cpp
TEST(SemanticAnalyzerAccessTest, StructPrivateSectionDeniedOutside) {
    EXPECT_FALSE(analyzeOk(
        "struct S { private: int32 x; public: int32 get() { return this->x; } }; "
        "int32 main() { S s; return s.x; }"));
}

TEST(SemanticAnalyzerAccessTest, StructPrivateAccessibleInsideMethod) {
    EXPECT_TRUE(analyzeOk(
        "struct S { private: int32 x; public: int32 get() { return this->x; } }; "
        "int32 main() { S s; return s.get(); }"));
}

TEST(SemanticAnalyzerAccessTest, StructPublicSectionAfterPrivate) {
    EXPECT_TRUE(analyzeOk(
        "struct S { private: int32 hidden; public: int32 open; }; "
        "int32 main() { S s; s.open = 1; return s.open; }"));
}

TEST(SemanticAnalyzerAccessTest, StructWithMethodsKeepsReprC) {
    EXPECT_TRUE(analyzeOk(
        "[[repr(C)]] struct S { int32 x; int32 get() { return x; } }; "
        "int32 main() { S s; s.x = 1; return s.get(); }"));
}
```

- [ ] **Step 2: 写失败测试（e2e attached + layout）**

在 `tests/e2e/test_record_grammar.cpp` 增加（复用 `runSource` 风格）：

```cpp
TEST_F(RecordGrammarE2E, StructWithAccessAndMethods) {
    EXPECT_EQ(runSource(R"(
        struct S {
        private:
            int32 x;
        public:
            int32 get() { return this->x; }
            void set(int32 v) { this->x = v; }
        };
        int32 main() { S s; s.set(7); return s.get() - 7; }
    )", "record_access.cmm"), 0);
}

TEST_F(RecordGrammarE2E, StructAttachedVariableKeepsMethods) {
    EXPECT_EQ(runSource(R"(
        struct S {
            int32 x;
            int32 get() { return this->x; }
        } s;
        int32 main() { s.x = 9; return s.get() - 9; }
    )", "record_attached.cmm"), 0);
}

TEST_F(RecordGrammarE2E, StructAndClassLayoutSame) {
    EXPECT_EQ(runSource(R"(
        struct S { int32 x; int32 y; };
        class C { public: int32 x; int32 y; };
        int32 main() {
            return compile_time.size_of(S) == compile_time.size_of(C) ? 0 : 1;
        }
    )", "record_layout.cmm"), 0);
}
```

- [ ] **Step 3: 运行确认 RED**

```bash
cmake --build build -j8
ctest --test-dir build -R 'SemanticAnalyzerAccessTest.(StructPrivateSectionDeniedOutside|StructPrivateAccessibleInsideMethod|StructPublicSectionAfterPrivate|StructWithMethodsKeepsReprC)|RecordGrammarE2E' --output-on-failure
```

Expected: 访问段测试因 parser 已能解析但 sema 未按 class 路径处理而失败；attached 测试可能因 `getStruct` 拿不到 `ClassType` 失败。

- [ ] **Step 4: 统一 `isClass` 判定**

`SemanticAnalyzer::visitStructDeclImpl` 中：

```cpp
bool isClass = node.isClassDecl || !node.methods.empty() || node.hasAccessSections;
```

确保 `memberAccess` 进入 `ClassType`，访问检查生效；`validateAnnotations` 仍使用 `node.isClassDecl ? "class" : "struct"`，所以 `struct` 的 `repr(C)` 不回归。

- [ ] **Step 5: 统一 parser 类型注册**

`parseDeclaration` 的 struct 分支与 `registerNestedDeclType` 中，把判定改为：

```cpp
const bool isClassRecord =
    structDecl->isClassDecl || !structDecl->methods.empty() || structDecl->hasAccessSections;
```

`isClassRecord` 为 true 时使用/创建 `ClassType`；否则使用 `StructType`。attached declarator 分支改为：

```cpp
auto* type = isClassRecord
    ? static_cast<Type*>(TypeContext::instance().getClass(structDecl->name))
    : static_cast<Type*>(TypeContext::instance().getStruct(structDecl->name));
if (!type) { /* 创建对应类型 */ }
structDecl->attachedVars = parseDeclarationFromBaseType(type);
return structDecl;
```

类分支保持现有 `attachedVars` 行为。

- [ ] **Step 6: 复制 AST 标志**

`TemplateInstantiator` 克隆 `StructDeclAST` 时补：

```cpp
cloned->hasAccessSections = st->hasAccessSections;
```

- [ ] **Step 7: GREEN + 目标回归**

```bash
cmake --build build -j8
ctest --test-dir build -R 'SemanticAnalyzerAccessTest|RecordGrammarE2E|RecordGrammarTest|TemplateRegistryTest|GenericE2E' --output-on-failure
```

Expected: 全部 PASS。

- [ ] **Step 8: Commit**

```bash
git add src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp src/sema/TemplateInstantiator.cpp tests/sema/test_semantic_analyzer.cpp tests/e2e/test_record_grammar.cpp
git commit -m "feat(sema): route access-bearing structs through class semantics"
```

---

### Task 4: 文档、TODO/Progress 与最终验证

**Files:**
- Modify: `TODO.md`（MOD-04/06/11/12/13、DEC-02/17、PAR-05/22、TYP-07/08 状态与快照）
- Modify: `Progress.md`（本轮记录、验证证据）
- Modify: `docs/superpowers/specs/2026-11-09-module-namespace-record-design.md`（状态改为已实施）
- Verify: 全量测试、示例、工作区

- [ ] **Step 1: 更新 TODO/Progress**

记录：

- module 点分、namespace `::`、import 只控制可见性；
- 把 DEC-02 的“直接非限定访问”限定为**模块根部导出**；namespace 成员必须限定访问；
- struct/class 共用 record 文法，`hasAccessSections` 路由到 `ClassType`；
- `string`/`string_view` 迁移、删除 `struct` 仍是后续项；
- 全量测试数量与 `ALL FEATURES OK` 证据。

- [ ] **Step 2: 全量回归**

```bash
cmake --build build -j8
ctest --test-dir build -j8 --output-on-failure
```

Expected: 全量 PASS，0 失败。

- [ ] **Step 3: 示例验收**

```bash
build/bin/cmm examples/all_features.cmm -o /tmp/opencode/all_features
/tmp/opencode/all_features
```

Expected: 程序退出码 0，输出包含 `ALL FEATURES OK`。

- [ ] **Step 4: 工作区检查**

```bash
git status --short
```

Expected: 只包含本轮预期修改；无构建产物入库。

- [ ] **Step 5: Commit**

```bash
# docs/superpowers 被 .gitignore 的 docs/* 覆盖；新计划/设计文档需 -f 纳入。
git add TODO.md Progress.md
git add -f docs/superpowers/specs/2026-11-09-module-namespace-record-design.md \
            docs/superpowers/plans/2026-11-09-module-namespace-record.md
git commit -m "docs: record module/namespace and record grammar decisions"
```

---

## 计划自检

- **规范覆盖**：Decision 1 → Task 1；Decision 2 文法 → Task 2；Decision 2 语义 → Task 3；验收/文档 → Task 4。
- **类型一致**：`hasAccessSections`、`isClassDecl`、`memberAccess`、`parseRecordDecl(bool)` 在 Task 2/3 中使用一致。
- **未覆盖风险**：`union` 不在本轮；`string`/`string_view` 迁移不在本轮；这两项在 spec §5 明确为非目标。
- **Review Focus 测试归属**：模块名不变成 namespace → Task 1；struct 访问段/内部类型/attached → Task 3；拼写级联 → Task 1。
