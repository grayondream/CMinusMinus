# 泛型与 CRTP（P1-03）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CDD 获得函数/类/别名模板 + 非类型参数 + 单态化实例化 + `this` 表达式，端到端支撑 CRTP。

**Architecture:** 模板定义处只 parse 存 AST（`TemplateDeclAST`）；使用点惰性触发实例化——`TemplateInstantiator` 深克隆 AST 并把 `TypeVarType` 占位替换为具体 `Type*`，实例以普通 `FunctionDeclAST`/`StructDeclAST` 走既有 sema + codegen；`TemplateRegistry` 按规范键去重并维护 `NotInstantiated → Instantiating → Done` 状态机。

**Tech Stack:** C++17、LLVM、gtest（构建 `cd build && cmake --build . -j$(nproc)`，全量 `ctest`）。

**Spec:** `docs/superpowers/specs/2026-10-06-generics-crtp-design.md`

## Global Constraints

- Non-goals（硬边界）：特化/偏特化、SFINAE、可变参数模板、模板模板参数、concepts、默认模板实参、类模板实参推导（CTAD）。
- 非类型参数仅整数（`usize`/`isize`/`intN`/`uintN`）；`class T` 写法不支持（诊断提示用 `typename`）。
- 实例化深度上限 **64**；递归实例化自身（值语义自引用）报错。
- 实例命名：类 `Box$i32`、嵌套 `Box$Box$i32`、函数 `max$i32`（既有 `mangleFunction` 在其上追加参数类型）、非类型实参编入 `Array$i32$8`；`$` 拼写复用 `typeToMangled`，指针 `P` 前缀。
- P1-02 的 Optional/Result 内建特判**原样保留**，泛型机制不接管。
- 每任务 TDD：先写测试亲见失败，再实现；每任务结束全量 `ctest` 绿（基线 **952**）并提交一次。
- `Progress.md` 与 `docs/superpowers/` 提交需 `git add -f`。

## Review Focus

- 嵌套实参 `>>` 拆分：`Array<Array<i32,2>,2>` 类型位置——期望正确嵌套实例化（Task 3/5 的测试钉住）。
- 推导含指针形参：`template<typename T> T* pick(T* a)` 传 `int32*`——期望 `T=int32` 而非 `T=int32*`（Task 4 钉住）。
- static 方法中用 `this`——期望诊断「static 方法无 this」而非段错误（Task 6 钉住）。
- 未实例化模板的零符号：只声明不使用——期望 IR 中无对应函数/结构体（Task 8 钉住）。
- 模板体内标识符与实例化后局部变量重名——期望克隆体绑定实例后的具体名，无残留 TypeVar（Task 2 的单测钉住）。

---

### Task 1: 关键字、TypeVarType、TemplateDeclAST 与模板声明解析

**Files:**
- Modify: `src/frontend/Token.h`（`TOKEN_TEMPLATE`、`TOKEN_TYPENAME`）
- Modify: `src/frontend/Lexer.cpp`（keywordMap 注册 `"template"`、`"typename"`）
- Modify: `src/ast/Type.h`（`TypeKind::TypeVar`；`class TypeVarType : public Type { std::string name; }`）
- Modify: `src/ast/Type.cpp`（`TypeContext::getTypeVar(const std::string& name)`，`unordered_map<std::string, TypeVarType*> m_typeVars` 缓存保证同名单射）
- Modify: `src/ast/Decl.h`（`TemplateDeclAST`，见下）
- Modify: `src/frontend/Parser.h/.cpp`（`parseTemplateDecl()`；`parseDeclarationImpl` 收 `TOKEN_TEMPLATE` 分派；parser 成员 `std::vector<std::unordered_set<std::string>> m_templateScopeStack` 使 `parseType` 把参数名解析为 `TypeVarType`）
- Modify: `src/ast/Decl.cpp`（`TemplateDeclAST::codegen`：本轮直接返回 nullptr——codegen 侧 Task 3 处理跳过）

**Interfaces:**
- Produces: `TemplateDeclAST { std::vector<Param> params; std::unique_ptr<DeclAST> decl; bool isAlias; }`，其中 `struct Param { std::string name; bool isType; Type* nonTypeDefault; }`（`nonTypeDefault` 仅 `isType==false` 时非空，如 `usize` 类型节点）。`decl` 可为 `FunctionDeclAST`/`StructDeclAST`/`UsingDeclAST`（别名）。`TypeContext::getTypeVar(name) -> TypeVarType*`。
- Produces: 诊断文案钉死——union/enum 模板：`'template' is not supported on unions/enums`；`class T` 参数：`use 'typename' instead of 'class' in template parameter list`。

- [ ] **Step 1: 写失败测试**——新建 `tests/frontend/test_template_parse.cpp`（fixture 仿 tests/frontend/ 既有 parser 测试的构造方式）：
  - `FunctionTemplateParses`：`template<typename T> T max(T a, T b) { return a; }` → 单元 `declarations[0]` 是 `TemplateDeclAST`，`params.size()==1 && params[0].isType`，`decl` 为 `FunctionDeclAST`，其 return/param 类型 `kind==TypeKind::TypeVar`。
  - `ClassTemplateParses`：`template<typename T> struct Box { T value; };` → `decl` 为 `StructDeclAST`，字段类型是 TypeVar。
  - `MixedParamsParse`：`template<typename T, usize N> struct Arr { T data[N]; };` → `params[1].isType==false`；数组长度处的 `N` 由克隆器处理（本任务只断言 parse 不报错、字段类型含 Array）。
  - `AliasTemplateParses`：`template<typename T> using Vec = Array<T, 8>;` → `isAlias` 且 `decl` 为 `UsingDeclAST`。
  - `ClassParamRejected`：`template<class T> ...` → 诊断含 `use 'typename' instead of 'class'`。
  - `TemplateOnUnionRejected`：union/enum 版本 → 诊断含 `'template' is not supported`。
- [ ] **Step 2: 跑测试亲见失败**：`cd build && cmake . && cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='TemplateParse.*'` → 新增用例全部 FAIL（编译期即失败也计 RED）。
- [ ] **Step 3: 实现**——按 Interfaces 逐字段落点实现；`m_templateScopeStack` push/pop 包住模板体 parse，`parseType` 的标识符分支先查栈顶集合再查 TypeContext。
- [ ] **Step 4: 跑测试确认通过 + 全量 `ctest` 绿（952 + 新增）**。
- [ ] **Step 5: 提交** `git add ... && git commit -m "feat(gen-01): 模板声明解析与 TypeVarType（GEN-01/PAR-21）"`。

### Task 2: TemplateRegistry 与 TemplateInstantiator（去重 + 克隆替换）

**Files:**
- Create: `src/sema/TemplateRegistry.h/.cpp`
- Create: `src/sema/TemplateInstantiator.h/.cpp`
- Modify: `src/ast/Type.cpp`（`typeToMangled` 加 `TypeVar` 分支返回参数名；实例拼写函数）
- Modify: `tests/CMakeLists.txt` 不需动（GLOB_RECURSE 自动收 src）

**Interfaces:**
- Consumes: Task 1 的 `TemplateDeclAST`/`TypeVarType`。
- Produces:
  ```cpp
  class TemplateInstantiator {
  public:
      TemplateInstantiator(const std::unordered_map<std::string, Type*>& typeArgs,
                           const std::unordered_map<std::string, long long>& valueArgs);
      Type* rewrite(Type* t);                        // 递归重写 Pointer/Array/Optional/Result/Typedef 内的 TypeVar；非 TypeVar 原样返回（复用 TypeContext 缓存指针）
      std::unique_ptr<DeclAST> cloneDecl(const DeclAST& decl); // 深克隆 FunctionDeclAST/StructDeclAST/UsingDeclAST，替换全部 Type* 与数组长度（valueArgs）
  };

  class TemplateRegistry {
  public:
      static TemplateRegistry& instance();
      void registerTemplate(std::unique_ptr<TemplateDeclAST> decl);
      TemplateDeclAST* find(const std::string& name) const;
      // 状态机 NotInstantiated→Instantiating→Done；Instantiating 再命中报
      // "recursive instantiation of template 'X'"；深度上限 64 报
      // "template instantiation depth limit exceeded"。
      // 实例 decl 的 name 在克隆前改为 mangled 拼写（Box$i32 / max$i32）。
      StructDeclAST* instantiateClass(const std::string& name,
          const std::vector<Type*>& typeArgs, const std::vector<long long>& valueArgs);
      FunctionDeclAST* instantiateFunction(const std::string& name,
          const std::vector<Type*>& typeArgs, const std::vector<long long>& valueArgs,
          const std::vector<Type*>& deducedParamTypes);
      bool isInstantiating(const std::string& key) const;
  };
  ```
- Produces: 实例 decl 由调用方负责送 sema（Task 3/4 接线）；Registry 暴露 `pendingInstances()`（克隆完成待 sema 的队列，`std::deque<std::unique_ptr<DeclAST>>`，调用方 `pop` 后 visit 并最终转交 codegen）。
- 诊断文案钉死：`recursive instantiation of template '<名>'`、`template instantiation depth limit exceeded (64)`。

- [ ] **Step 1: 写失败测试**——`tests/sema/test_template_registry.cpp`（直接用 Registry/Instantiator，不跑全管线）：
  - `ClassInstanceNaming`：注册 `Box`，`instantiateClass("Box", {getInt32()}, {})` → 返回 `StructDeclAST` name 为 `Box$i32`，字段类型为 int32。
  - `InstanceDedup`：同参二次调用返回**同一个指针**（状态 Done 缓存）。
  - `NestedTypeRewrite`：`template<typename T> struct H { Optional<T> o; T* p; };` 实例化 `H<f64>` → 字段类型分别是 `OptionalKind(f64)` 与 `Pointer(base f64)`。
  - `ValueParamSubstitution`：`template<typename T, usize N> struct Arr { T data[N]; }` 实例化 `{int32}, {4}` → 字段 ArrayType `size==4`。
  - `RecursiveInstantiationRejected`：模板体引用自身值语义（手工构造 `struct Node { Node next; }` 形式的模板）→ instantiate 时诊断含 `recursive instantiation`。
  - `DepthLimit`：以 65 层嵌套实参（循环构造 `Box<Box<...i32>>` 类型）触发 → 诊断含 `depth limit exceeded`。
- [ ] **Step 2: 跑测试亲见失败**（链接失败/断言失败均计 RED）。
- [ ] **Step 3: 实现**。克隆器按 DeclAST/StmtAST/ExprAST 全族写 clone 方法——**每个 AST 节点类都要覆盖**（数量大，机械但必须完整；漏一个 = 实例化含该节点时 nullptr）。`rewrite` 对 `isConst/isVolatile` 等标志原样保留。
- [ ] **Step 4: 跑测试确认通过 + 全量 `ctest` 绿**。
- [ ] **Step 5: 提交** `git commit -m "feat(gen-05): TemplateRegistry 与 AST 克隆替换器"`。

### Task 3: 类型位置实参解析 + 类模板实例化接线（含 codegen 跳过/出码）

**Files:**
- Modify: `src/frontend/Parser.cpp`（`parseType` 的标识符分支：名字命中 `TemplateRegistry::find` 且后随 `<` → 解析实参列表，`>>` 按嵌套深度拆分——仿 `Optional<Optional<int32>>` 既有处理 Parser.cpp:1695；类型占位 `TypeInstanceType{ 模板名+实参拼写 }` 暂记到 AST）
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit` 遇类型占位：调 `instantiateClass` 得实例 `StructDeclAST`，visit 它（建 `ClassType` 名 `Box$i32` 入 TypeContext），占位 Type 原地改指实例 ClassType）
- Modify: `src/ast/Decl.cpp`（`TranslationUnitAST::codegen`/sema 对 `TemplateDeclAST` 跳过；Registry `pendingInstances` 中已 visit 的实例 decl codegen 出码——在 `analyze` 末尾把实例 decl 追加到 `TranslationUnitAST::declarations` 尾部， TranslationUnitAST 增加 `std::vector<std::unique_ptr<DeclAST>> templateDecls` 单独存放模板定义避免 codegen 重复出码）
- Modify: `src/ast/Type.cpp`（`typeToString`：TypeVar → 参数名；实例占位 → `Box<i32>` 源拼写）

**Interfaces:**
- Consumes: Task 1 parse、Task 2 Registry。
- Produces: 源码 `Box<i32> b;` 端到端可用；`b.value` 字段访问、赋值、作为函数参数/返回值均走既有路径。**类型占位节点的具体形态**（新 `TypeKind::TypeInstance` 挂 `TemplateDeclAST*` + 实参）允许实现者自行定夺，但必须：sema 后不复存在（全部已解析为具体 ClassType）。

- [ ] **Step 1: 写失败测试**——sema 侧追加到 `tests/sema/test_semantic_analyzer.cpp`（新前缀 `Tpl*` 防单例碰撞）：
  - `ClassTemplateFieldAccessOk`：`template<typename T> struct Box { T value; }; int32 main() { Box<i32> b; b.value = 3; return 0; }` → analyzeOk。
  - `ClassTemplateTypeMismatch`：同上但 `b.value = 0.5;` → 报错（实例点检查生效）。
  - `InstanceErrorDualLoc`：模板体内 `T x; x + oops;` 类错误 → 错误主 loc 在使用点，消息含 `in instantiation of`。
- [ ] **Step 2: 跑测试亲见失败**。
- [ ] **Step 3: 实现**接线（parser 实参解析 + sema 触发 + codegen 跳过/出码）。
- [ ] **Step 4: 新增 e2e `tests/e2e/test_generics.cpp`（fixture 仿 `OptionalResultE2E`）**：
  - `BoxRoundTrip`：`Box<i32>` 存取返回值 → 断言执行结果。先亲见失败（Step 4.1）再实现剩余缝隙后转绿。
  - `BoxNested`：`Box<Box<i32>>` 嵌套实参。
- [ ] **Step 5: 全量 `ctest` 绿**。
- [ ] **Step 6: 提交** `git commit -m "feat(gen-03): 类模板实例化端到端（TYP-18/SEM-06）"`。

### Task 4: 函数模板——显式实参与推导

**Files:**
- Modify: `src/frontend/Parser.cpp`（call 位置：标识符命中模板且后随 `<` → **投机**解析 `<实参>` 后必须紧跟 `(`，失败回滚按比较表达式处理——沿用 parseUnary 强转投机的前后文模式）
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit(CallExprAST)`：模板名命中 → 推导 → `instantiateFunction` → visit 克隆体 → 调用点绑定实例符号；无参推导（全 TypeVar 且无实参可依）→ 诊断 `cannot deduce template argument for '<名>'`；同 T 多实参冲突 → `conflicting deduction for 'T': 'int32' vs 'float64'`）

**Interfaces:**
- Consumes: Task 2 `instantiateFunction`。
- Produces: `max<int32>(3,4)`、`max(3,4)` 均可用；推导规则——形参 `T` 按实参类型、形参 `T*` 按去指针类型；字面量 `3` → int32。重载互动最小规则：同名普通函数精确匹配优先于模板实例。
- 诊断文案钉死：`conflicting deduction for 'T': '<A>' vs '<B>'`、`cannot deduce template argument for '<名>'`。

- [ ] **Step 1: 写失败测试**——sema（前缀 `TplFn*`）：
  - `ExplicitFuncTemplateOk` / `DeducedFuncTemplateOk`：`max(3,4)` analyzeOk 且（e2e 断言值）。
  - `PointerParamDeduction`：`T* pick(T* a)` 传 `int32*` → 实例键含 `int32`（可经 `pick` 返回值解引用验证）。
  - `ConflictingDeduction`：`max(3, 0.5)` → 诊断 `conflicting deduction`。
  - `Undeducible`：`template<typename T> T make();` 裸调用 `make()` → `cannot deduce`。
  - `NonTemplateExactMatchWins`：同名普通函数 + 模板，精确匹配走普通函数（通过 e2e 返回值区分）。
- [ ] **Step 2: 跑测试亲见失败**。
- [ ] **Step 3: 实现投机解析 + 推导 + 实例化接线**。
- [ ] **Step 4: e2e `GenericFuncRoundTrip`（`max(3,4)` 返回 4）、`GenericFuncExplicit`**——先 RED 后 GREEN。
- [ ] **Step 5: 全量 `ctest` 绿**。
- [ ] **Step 6: 提交** `git commit -m "feat(gen-02/03/06): 函数模板显式与推导实例化"`。

### Task 5: 非类型参数常量语境与别名模板展开

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（数组长度位置的整型常量求值复用 constexpr 既有机制；别名模板使用点展开为 `rewrite(aliasTarget)` 后的 Type，键缓存防重复展开）

**Interfaces:**
- Consumes: Task 1 别名 parse、Task 2 valueArgs 替换。
- Produces: `template<typename T, usize N> struct Array { T data[N]; }`、`Array<i32,4>` 可用；`template<typename T> using Vec = Array<T, 8>;` → `Vec<i32>` 与 `Array<i32,8>` 同型（互相赋值 analyzeOk）。

- [ ] **Step 1: 写失败测试**——sema（前缀 `TplArr*`）：
  - `NonTypeParamArrayOk`：上例 `Array<i32,4> a; a.data[0] = 1;` → analyzeOk。
  - `AliasEquivalent`：`Vec<i32> v = {}; Array<i32,8> a = v;`（值语义赋值）→ analyzeOk。
  - `NonIntegralValueParamRejected`：`Array<i32, 0.5>` → 诊断（文案 `non-type template argument must be an integer constant`）。
- [ ] **Step 2: 跑测试亲见失败**。
- [ ] **Step 3: 实现**。
- [ ] **Step 4: e2e `FixedArrayTemplate`（`Array<i32,4>` 写读求和断言）、`AliasTemplateE2E`——先 RED 后 GREEN。
- [ ] **Step 5: 全量 `ctest` 绿**。
- [ ] **Step 6: 提交** `git commit -m "feat(gen-02): 非类型参数与别名模板（GEN-02）"`。

### Task 6: this 表达式（PAR-17）

**Files:**
- Modify: `src/frontend/Parser.cpp`（若 `this` 已是关键字则只需表达式路径；否则 `this` 进 keywordMap，`parsePrimary` 产出 `ThisExprAST`——新节点，`src/ast/Expr.h/.cpp` 各一处）
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit(ThisExprAST)`：`currentClass` 非空且非 static → type = `Ptr(currentClass)`，rvalue；static 方法/全局域 → 诊断 `'this' is not valid in a static method`）

**Interfaces:**
- Produces: 方法体内 `this` 可传参、可 `return this;`、`this->field`、`this.field`（`.` 对指针自动解引用——沿用既有 methodcall decay 机制）。

- [ ] **Step 1: 写失败测试**——e2e `test_generics.cpp`（前缀沿用 GenericE2E fixture）：
  - `ThisFieldAccess`：`struct A { i32 v; i32 get() { return this.v; } };` 实例调用返回字段值。
  - `ThisPassthrough`：`A* self() { return this; }` 链式调用改字段。
  - `ThisInStaticRejected`：static 方法用 `this` → 诊断 `'this' is not valid in a static method`（sema 单测）。
- [ ] **Step 2: 跑测试亲见失败**。
- [ ] **Step 3: 实现**。
- [ ] **Step 4: 全量 `ctest` 绿**。
- [ ] **Step 5: 提交** `git commit -m "feat(par-17): this 表达式"`。

### Task 7: CRTP 全链路（INH-05 / GEN-09）

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（基类解析：`baseClass` 字符串形如 `Shape<Circle>`（模板实例拼写）→ 经 Registry 实例化为基类；**派生类声明允许引用尚不完整的派生类型作实参**——前向占位；实例基类的方法体惰性：其克隆体不在实例化时 visit，登记到调用点触发表）

**Interfaces:**
- Consumes: Task 2 状态机、Task 6 this。
- Produces: spec 的 Shape/Circle 用例端到端可运行；`static_cast<Circle*>(this)` 走既有 PAR-18 向下转换。

- [ ] **Step 1: 写失败测试**——e2e（spec 原用例）：
  - `CrtpStaticDispatch`：`Circle c; c.twice_area()` 断言执行值（r=1.0 → 6.28…，断言 `>6.0 && <6.3` 的整型投影，或 `area*2` 与手算值比较）。
  - `CrtpSameShape`：`same_shape(&c2)` 同型比较 → 1。
  - sema：`CrtpRecursiveValueFieldRejected`——模板基类含 `D next;` 值字段 → `recursive instantiation` 诊断。
- [ ] **Step 2: 跑测试亲见失败**。
- [ ] **Step 3: 实现基类实例拼写解析 + 方法体惰性触发**。
- [ ] **Step 4: 全量 `ctest` 绿**。
- [ ] **Step 5: 提交** `git commit -m "feat(inh-05): CRTP——模板基类、延迟实例化与 this 向下转换"`。

### Task 8: 硬化、文档收口

**Files:**
- Modify: `tests/e2e/test_generics.cpp`、`tests/sema/test_semantic_analyzer.cpp`（硬化用例）
- Modify: `TODO.md`（GEN-01~09、PAR-17/21、LEX-08、TYP-18、SEM-06、CG-07、INH-05 → `[x]`，注明关键决策）
- Modify: `Progress.md`

- [ ] **Step 1: 写失败测试**：
  - `UnusedTemplateNoSymbol`：只声明 `template<typename T> T unused_fn(T x) {...}` 不使用 → e2e 中 `lookup("unused_fn")` 失败/IR 无该符号（`ctx.getModule().getFunction`）。
  - `ShadowedTypeParamOk`：模板体内声明与 T 同名的局部（克隆后 T 已具体化，不得残留 TypeVar）——`template<typename T> T f(T a) { i32 T = 3; return a; }` 若语法允许 → analyzeOk；不允许则改为无冲突变体并记录决定。
  - `DeepNestingBox`：`Box<Box<Box<i32>>>` 三层。
- [ ] **Step 2: 亲见失败 → 修复 → 绿**。
- [ ] **Step 3: 全量 `ctest` 绿；提交** `git commit -m "test(gen): 硬化用例与文档收口"`。
- [ ] **Step 4: `Progress.md` 追加记录（`git add -f`）**。

---

## Self-Review 记录

- **Spec 覆盖**：GEN-01→T1、GEN-02→T5、GEN-03/06→T4、GEN-04/LEX-08→T1/T3、GEN-05→T2、GEN-07→T2（命名）/T3（出码）、GEN-08→T3/T6、GEN-09→T7、PAR-17→T6、PAR-21→T1、TYP-18→T3、SEM-16 最小规则→T4、CG-07→T3。无缺口。
- **步骤粒度**：每个代码步只钉签名/落点/文案，主体留给实现者；测试步全部含断言。
- **类型一致性**：`instantiateClass`/`instantiateFunction`/`pendingInstances`/`getTypeVar` 在 T2/T3/T4/T7 引用一致。
- **Review Focus 回填**：`>>` 拆分→T3 e2e `BoxNested`+T5；指针推导→T4 `PointerParamDeduction`；static this→T6；零符号→T8；TypeVar 残留→T2 `NestedTypeRewrite`+T8。
- **比例**：计划 ≈ spec 体量 1.5 倍，无代码转写。
