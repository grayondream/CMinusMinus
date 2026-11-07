# AGG-10：class/struct static 成员（static 成员函数与 static 成员变量）设计

日期：2026-10-05
状态：已批准（用户 2026-10-05 确认方案 A 与 DS1–DS5）
上游：TODO.md AGG-10 / P1-01；MOD-08/MOD-15（名称修饰）；DEC-01（访问级别）
基线：820/820（master，`cb31f10`）

## 0. 方案选择

**方案 A（选定）：类前缀全局符号去糖**——static 成员 = 符号名去糖为
`Class_member` 的全局变量/全局函数。

依据（已核实）：

- sema 调用点已把限定名拍平：`visit(CallExprAST)` 首行
  `node.callee = resolveNamespaceName(node.callee)`（SemanticAnalyzer.cpp:1194），
  `Vec::create` → `Vec_create` 后走常规 overload 查找；变量访问同构
  （`visit(VariableExprAST)` → `resolveNamespaceName`）。
- 因此**限定查找、mangle、codegen 全部零改动复用**，只改声明侧
  （parser 收集 + sema 改名注册）。
- 方法 this 插入点在 `visit(StructDeclAST)`（SemanticAnalyzer.cpp:1809 附近），
  对**所有**方法无差别插入——static 化即条件化该处。
- `static` 已是关键字 `TOKEN_STATIC`（Lexer.cpp:64），parser 未消费（顶层 static
  函数尚无处理，属 FUN-04 范围）。
- mangle 现状：`mangleFunction(name, params)`（Mangle.cpp:79）无类限定——free
  function `create` 与 static `Vec::create` 去糖后符号不同（`create_…` vs
  `Vec_create_…`），无碰撞。

否决方案：

- **B（隐式 namespace 去糖）**：namespace 语义（开放、可跨模块）与类成员
  （封闭、随类定义）不符，且与 `memberAccess` 机制耦合重。
- **C（成员符号表全量重构）**：范围爆炸，MOD-08 另行推进。

## 1. 语义（冻结点）

```safe
class Vec {
    private:
    static int32 count = 0;                     // 类内初始化器
    static int32 makeID() { return ++count; }   // 无 self
    public:
    static Vec create(int32 n) { ... }
}
Vec::create(3);   // 仅类名限定
Vec::count = 5;   // 读写同语法
v.create();       // ✗ 拒绝（DS1）
```

- **DS1（仅类名限定）**：static 成员只能经 `Class::member` 访问。static 方法
  与变量**不进** `classType->methods` / fields，实例路径（`obj.create()` /
  `obj.count`）天然拒绝。类体内也不做 unqualified 直呼（与 namespace 习惯
  不同，保持单一规则）。
- **DS2（类内初始化）**：`static T name = init;` 的初始化器直接生效为全局
  变量定义；无初始化器则零初始化（对齐本语言全局变量既有语义）。static
  变量**不占对象布局**（不进 fields，`sizeof(Vec)` 不变）。
- **DS3（符号去糖）**：`Class::member` → `Class_member`（`mangleNamespaceName`
  既有变换）。声明侧在 sema 类分析期改写符号名；访问侧既有机制零改动。
  namespace 内类的 static 成员同样加 ns 前缀，调用/访问需**完全限定**
  （`ns::Vec::create`），与 namespace 现状一致。
- **DS4（无 self）**：static 方法不插入 `this` 形参；方法重载照常
  （overload set 按参数区分）；与 free function 同名不碰撞（DS3 符号不同）。
  static 方法体内可调用同类其他 static 成员，仍须限定书写（DS1 单一规则）。
- **DS5（访问控制）**：`public:`/`private:` 段照常作用于 static 成员
  （DEC-01 机制复用）；类外访问 private static 成员报 **E2009**，与实例成员
  同一诊断码。

### 范围外

- 实例调用 static、类内 unqualified 直呼（DS1 冻结为仅限定）。
- 继承链 static 查找：派生类名调基类 static 用基类名限定（INH-06 顺带项）。
- 顶层 `static` 函数/变量存储类（FUN-04）。
- `static` 局部变量（C 语义 static local）。
- 模板类的 static 成员（随 GEN）。

## 2. 实现落点

1. **AST**：`FunctionDeclAST` 新增 `bool isStatic`（默认 false）；
   `StructDeclAST` 新增 `std::vector<std::unique_ptr<VarDeclAST>> staticMembers`
   （parser 预构建完整 VarDeclAST，复用其初始化器/constexpr/全局 codegen 路径）。
2. **Parser**（`parseClassDecl` 成员循环，Parser.cpp:2388 起）：识别
   `TOKEN_STATIC` → 标记随后的成员：
   - `static T name(` → `parseFunctionDecl` 结果 `isStatic = true` 进 methods；
   - `static T name ;`/`= init;` → 构造 `VarDeclAST`（名字暂记
     `Class::name`）进 `staticMembers`；记录 `memberAccess[name] = currentAccess`。
   - struct 与 class 的成员循环各自独立（`parseStructDecl` :2356 /
     `parseClassDecl` :2388），两处同步识别 `TOKEN_STATIC`（struct 默认
     public，无需额外访问段处理）。
3. **Sema**（`visit(StructDeclAST)`，SemanticAnalyzer.cpp:1790 起）：
   - static 方法：跳过 this 插入与 `classType->addMethod`；方法名改写为
     `mangleNamespaceName(Class::name)` 后按全局函数声明（进 overload set）。
   - staticMembers：名字改写同上后调用 `visit(VarDeclAST)`（全局级声明，
     ns 前缀由 `scopedName` 既有逻辑附加）。
   - **E2009 挂点**：调用/变量访问点对去糖后的 static 成员符号做访问级别
     检查（利用 `classType->memberAccessLevel`；实现细节由计划阶段定，
     候选：sema 保留 `Class_member` → `(classType, member)` 映射）。
4. **Codegen**（`StructDeclAST::codegen`，Decl.cpp:458 起）：在"类型已注册"
   提前 return 分支**也**生成 `staticMembers`（全局变量幂等：已存在即复用）；
   static 方法走 `FunctionDeclAST::codegen` 零改动（名字已去糖、无 this 参数）。
5. **规范**：`docs/spec/abi.md` MOD-15 节补一条：`Class_member` /
   `Class_method_params` 的静态成员编码。

## 3. 测试计划（TDD；前缀 `SM`，sema + e2e；基线 820）

sema（test_semantic_analyzer.cpp）：

1. `SMStaticMethodQualifiedCall`——`Vec::create(3)` 类外限定调用 OK；
2. `SMStaticVarQualifiedAccess`——`Vec::count = 5;` 读写 OK；
3. `SMPrivateStaticExternalRejected`——类外访问 private static 成员报错
   （显式管线断言 E2009/消息）；
4. `SMInstancePathRejected`——`obj.create()` / `obj.count` 拒绝；
5. `SMStaticInstanceSameName`——static 与实例方法同名共存（参数表区分）；
6. `SMStaticOverload`——static 方法重载；
7. `SMStaticVarNoInitZero`——无初始化器 static 变量合法（零初始化）；
8. `SMStaticNotInLayout`——`sizeof(Vec)` 不含 static 变量。

e2e（test_agg.cpp 或新文件）：

9. `SMCounterPersist`——static 计数器跨调用持久（三次调用后 `r == 3`）；
10. `SMFactoryMethod`——static 工厂返回对象并可用；
11. `SMPrivateInternalUse`——private static 成员经 public 方法内部使用
    （完全限定书写）。

RED 预判：1–8 现状均为解析错误（`static` 在类体内未处理）→ 分析失败；
9–11 同。计划阶段逐项确认失败形态。

## 4. 验收

全量 `ctest` 绿（基线 820 → 约 831）；TODO.md AGG-10/PAR-04/P1-01 勾记；
Progress.md 记录；subagent 整分支评审 → TDD 修复 → Rulings 汇报。
