# 类型解析三缺口收口：作用域别名 / 前向类型身份 / 别名指针解引用

- 状态：待批准
- 日期：2026-10-24
- 关联：PAR-24（using 唯一别名）、MOD-03（前向声明合并）、SEM-02（指针解引用）

## 1. 意图与问题

`using` 收口（PAR-24）后留下三个已实测缺口：

1. **块/类/结构体内 `using` 被硬拒绝**。块内报 `only allowed at namespace scope`，后续 `T x;` 级联；class/struct 成员体同样被拒，并冒出 `unknown type 'T'`。
2. **前向类型占位与定义不合并**。`using N = struct Node;` 或 `struct Node;` 后再 `struct Node { int32 x; };`，`N n; n.x` 报 `no member named 'x'`。根因：parser 在两处为定义新建 `StructType`/`UnionType` 并覆盖占位对象，早先绑定的别名/指针仍指向旧占位。class 已因 `getOrCreateClass` 复用而正常。
3. **`*p` 不穿透指针别名**。`using P = int32*; P p; *p` 报 `cannot dereference non-pointer type 'P'`。sema 与 codegen 都只检查 `TypeKind::Pointer`，未剥 `TypedefType`。`p->x` 已经能用，只有一元 `*` 缺口。

用户已批准 C/C++ 式语义：块内词法作用域、类内 `C::T` 且受访问控制、`using N = struct Node;` 隐式前向声明并在定义处合并。

## 2. 目标 / 非目标

**目标**

- 块内 `using`：词法作用域（声明点至块尾），可遮蔽外层，块尾失效；嵌套块独立。
- class/struct/union 成员体 `using`：后续成员声明可用裸名；类外经 `C::T` 访问；class 内受 `public/private/protected` 控制；struct/union 恒 public。
- 前向类型身份：同名 struct/union 的占位与定义是**同一对象**；别名、指针、`p->x` 在定义后看到字段。
- `*p` 穿透任意层指针别名；读写、`(*p).x`、多级别名均可用。
- 保留模板别名 `template<typename T> using Vec = ...;` 与 namespace 别名。

**非目标（本轮不做）**

- 前向类型的按值使用诊断（`N n;` 在定义前）——维持现状，不新增 incomplete 检查，避免与 sema 重定义判定纠缠。
- `if (c) using T = ...;` 这类**无花括号单语句体**：作用域会延伸到外层块；文档标注，不单独诊断。
- 成员别名继承到派生类；`C::T` 在派生类内的裸名查找。
- 同作用域重复 `using` 别名的诊断（parser 与 sema 都会重复登记，暂不加）。
- 类作用域别名模板（`class C { template<typename T> using V = ...; };`）。

## 3. 现状机制（设计依据）

- `TypeContext` 是单例、扁平 map：`m_typedefs/m_structs/m_unions/m_enums/m_classes`，无作用域。
- parser 在**解析期**把别名写入 `TypeContext`，类型节点（`TypedefType`）直接烘进 AST；sema 的 `visit(UsingDeclAST)` 会**再登记一次**（新包装）。
- parser 的 `qualifyTypeDeclName()` 用 `m_typeNamespacePrefix` 给类型名加扁平前缀；class/struct/union 体解析时前缀为 `Outer_`，namespace 为 `ns_`。`lookupNamedType()` 先试 `前缀+名字`，再试裸名。因此成员别名登记为 `Outer_T` 即可让类内裸名 `T` 命中，类外 `C::T` 经 `mangleQualifiedTypeName` 也命中同一键。
- 成员循环的 `nestedTypes` 已是通用 `vector<unique_ptr<DeclAST>>`，sema 的 `visitNestedTypeDecls`/`visitNestedDecl` 按类型分派，class 的嵌套类型访问控制经 `nestedTypeAccess` + `checkNestedTypeAccess`。
- `parseCompoundStmt()` 是块解析入口；sema 的 `visit(CompoundStmtAST)` 已有 `enterScope()/exitScope()` 符号作用域，可并列挂类型别名作用域。

## 4. 设计

### 4.1 `*p` 穿透别名（最小、独立）

- `SemanticAnalyzer::visit(UnaryExprAST)` 的 `UnaryOp::Deref`：先 `stripTypedef(operandType)`，再判 `Pointer`，`node.type = stripped->base`。
- `UnaryExprAST::codegen` 的 `Deref`：用 `stripTypedefsT12(operand->type)` 判 `Pointer`；回退分支的 pointee 也取 stripped 的 `base`。
- 不改 `p->x`（已工作）；不改指针算术/比较。
- 测试：`using P = int32*;` 读写、`using PS = struct S*; (*p).x`、`using PP = int32**; **pp`。

### 4.2 前向类型身份（struct/union 占位复用）

- `Parser::parseType()` 的 struct/union **定义**分支（内联 `struct S { ... }`）：
  - 具名时取 `TypeContext::getStruct(key)`/`getUnion(key)`；存在且未完成则复用，否则新建。
  - 复用后先清空 `fields`/`members` 再填充（仅解析期，此时未布局）。
- `Parser::parseDeclarationImpl()` 的 struct/union **定义**注册：
  - 同样先取占位；存在则复用（清空字段后填充），不再 `new` 覆盖。
  - 前向声明分支保持“无占位才建”。
- class 路径已复用 `getClass`，不动。
- sema 已经 `getStruct/getUnion` 复用并按名补字段，不动；`isComplete` 仍由 sema 置位（parser 不置，避免 `isTypeRedefined` 误判）。
- 效果：`using N = struct Node;` 与 `struct Node;` 创建的占位对象，在定义处被填字段；别名/指针/`p->x` 看到成员。

### 4.3 块内 `using`（词法作用域）

- **TypeContext**：新增局部别名作用域栈（API 与语义见 §9.2）。块别名以**原始标识符**进栈，不碰全局 `m_typedefs`。
- **parser**：`parseCompoundStmt()` 在 `{` 之后 push、`}` 之后 pop（异常路径也 pop）；函数体、if/while/for/lambda 的 `{ }` 天然获得独立作用域。
- **parser 语句**：`parseStmt()` 把 `isLocalUsingStart()` 的拒绝改成解析 `using IDENT = type-id ;`，`addLocalTypedef(IDENT, type)`，`UsingDeclAST{name=IDENT, bareName=IDENT, isBlockScope=true, aliasWrapper=解析期包装}`，包成 `DeclStmtAST`；失败时 `skipPastSemicolon()` 返回空。
- **sema**：`visit(DeclStmtAST)` 增加 `UsingDeclAST` 分派；`visit(CompoundStmtAST)` 在 `enterScope()` 后 push、`exitScope()` 前 pop；块别名不 `declare` 值符号（§9.4）。
- 遮蔽：非限定名先查局部栈（内层优先），pop 恢复外层；`C::T` 等限定名跳过局部栈（§9.1）。

### 4.4 class/struct/union 成员 `using`

- **parser 成员循环**（struct/class/union 三处）：把 `isLocalUsingStart()` 的拒绝改成成员别名分支：
  - 解析 `using IDENT = type-id ;`；`flat = qualifyTypeDeclName(IDENT)`，`addTypedef(flat, type)`；创建 `UsingDeclAST`，新增字段 `bareName = IDENT`；压入 `nestedTypes`。
  - class 路径同时 `memberAccess["type:" + IDENT] = currentAccess`（复用现有访问段状态）；struct/union 不记或记 Public。
  - 裸名可见性由现有 `lookupNamedType` 的“前缀优先”保证；类外 `C::T` 走扁平键。
- **sema**：
  - `visitNestedTypeDecls` 识别 `UsingDeclAST`：`flatName = u->name`，`bareNested = u->bareName`，登记 `nestedTypeAccess`，再 `visitNestedDecl`。
  - `visitNestedDecl` 增加 `UsingDeclAST` → `visit(*u)`。
  - `checkNestedTypeAccess` 在剥裸 `TypedefType` 前，先沿 typedef 链检查每个 `TypedefType::name` 是否在 `nestedTypeAccess` 中；命中则按现有 E2009 规则判定。这样 `C::T` 作为变量类型/转换目标时受访问控制。
- **不变**：`UsingDeclAST::codegen` 仍返回空；成员别名不占对象布局、不进方法表。

## 5. 诊断与兼容

- `typedef`/`type` 的硬错误与提示不变。
- 删除“块/成员体 using 只允许 namespace”的诊断路径；`isLocalUsingStart()` 改作正向解析入口。
- 既有断言该拒绝行为的 parser 测试（`UsingInBlockRejected`、`UsingInClassBodyRejected`）改为正向测试，并补 struct/union 成员用例。
- class 私有成员别名在类外使用：沿用 E2009 文案（`cannot access private type 'C_T' of class 'C' ...`）。
- namespace 别名、顶层别名、模板别名行为不变；模板别名回归必须继续绿。

## 6. 验收矩阵

| # | 程序 | 期望 |
|---|---|---|
| 1 | `int32 main(){ using T=int32; T x=1; return x-1; }` | 0 |
| 2 | 内层块 `using T=float64` 覆盖外层 `T=int32`，块后外层仍 int32 | 0 |
| 3 | 块后用 `T` | 解析错误（作用域已失效） |
| 4 | `struct S{ using T=int32; T x; };` | 0 |
| 5 | `class C{ public: using T=int32; }; C::T v=1;` | 0 |
| 6 | `class C{ using T=int32; };` 类外用 `C::T` | E2009 |
| 7 | 类内方法体用私有 `T` | 0 |
| 8 | `union U{ using T=int32; T x; };` | 0 |
| 9 | `using N=struct Node; struct Node{int32 x;}; N n; n.x=5;` | 0 |
| 10 | `struct Node; using N=Node; struct Node{int32 x;}; N* p; p->x=5;` | 0 |
| 11 | `using U=union V; union V{int32 i;}; U u; u.i=1;` | 0 |
| 12 | `using P=int32*; int32 x=7; P p=&x; *p=8; return x-8;` | 0 |
| 13 | `using PS=struct S*; (*p).x`、`p->x` | 0 |
| 14 | `using PP=int32**; **pp` | 0 |
| 15 | `template<typename T> using Vec=Arr<T,2>;` 顶层/跨函数使用 | 0 |

## 7. 风险与缓解

- **TypeContext 单例的作用域配平**：parser 与 sema 各自 push/pop；parser 在 sema 之前完成并已 pop；sema 在自己的 `CompoundStmt` 重新登记块别名。任何提前 return 都必须 pop（`parseCompoundStmt` 只有 `{` 失败前不 push）。
- **模板别名不进局部栈**（§9.3）：别名模板展开走 `m_aliasCache`，成员/全局别名走 `m_typedefs`，局部栈只收块别名。回归：两个函数先后使用同一模板别名。
- **块别名的声明起始识别**：`isTypeStart()` 已调用 `lookupNamedType()`，后者加入局部栈查找后，`T x;` 自动被识别为声明，无需另改 `parseStmt` 的前瞻。
- **`isTypeRedefined` 依赖 `isComplete`**：parser 复用占位但**不**置 `isComplete`，仍由 sema 置位，避免首次定义被误判重定义。
- **类外 `C::T` 查找**：依赖 `mangleQualifiedTypeName` 与 `qualifyTypeDeclName` 同一扁平键，沿用 AGG-11 已验证机制。
- **前向复用清字段**：只在解析期、未布局时清空；重定义本就 E2004，不承诺语义。
- **测试基数**：当前 1514；本轮增删后以实际 `ctest` 为准，不预设数字。

## 8. 实施顺序

1. `*p` 穿透别名（独立，最小）。
2. 前向类型身份（struct/union 占位复用）。
3. 块作用域 `using`（TypeContext 作用域栈 + parser/sema 挂接）。
4. 成员体 `using` + 访问控制。
5. 文档（grammar/keywords/semantics）、TODO/Progress、示例增补。

每步 TDD：先写失败测试，再实现，`ctest -j8` 全绿后提交。最终用 `examples/all_features.cdd` 与新增 e2e 验证。

## 9. Builder-check 决议（覆盖前文的实现细节）

独立 builder 提出 4 个会影响计划的点，全部由本轮裁决：

1. **块别名用独立词法键，不用 `qualifyTypeDeclName`。** §4.3 的“先 qualify 再 addTypedef”作废。块内 `using T = ...` 以**原始标识符 `T`** 登记到独立作用域栈；`lookupNamedType` 对**非限定名**先查栈（内层优先），再走现有前缀/裸名逻辑；限定名 `C::T` 只走扁平表，不受块别名影响。这样 `T` 能真正遮蔽全局/namespace/成员别名，而 `C::T` 始终指向成员别名。
2. **作用域机制用分层 delta 栈，不用整表快照。** TypeContext 新增：
   - `pushLocalTypedefScope()` / `popLocalTypedefScope()`
   - `addLocalTypedef(name, type)`（写入栈顶层）
   - `getLocalTypedef(name)`（自内向外查）
   全局 `m_typedefs` 不被块别名触碰；嵌套块自然分层，pop 即恢复。
3. **模板别名风险不成立。** 已核实别名模板展开缓存在 `m_aliasCache`，不写 `m_typedefs`；成员/全局别名仍走全局表。删除原“模板实例化被 pop”的缓解；只保留“两个函数先后使用同一模板别名”的回归测试。
4. **块别名不 `declare` 值符号。** 成员/全局别名保持现有 `declare(node.name, ...)`；块别名只进类型作用域，避免污染值命名空间。代价：同块内 `using T = int32; int32 T;` 暂不诊断为冲突（记非目标）。
