# using 类型别名收口（删除 typedef / type）设计

日期：2026-10-23
状态：已批准（2026-10-23，type 一并删除）

## 1. 背景与意图

上一轮修复了文件作用域的顶层用户类型识别。用户随后的裁定是：**只保留 `using` 类型别名，`typedef` 与 `type` 都变成硬错误**（不再接受、无过渡期；`type` 的 distinct-type 方案随 DEC-16 一并关闭）。

实测发现当前 `using` 的能力远弱于 `typedef`：固定数组、函数指针、内联 enum 枚举量都不完整。直接删 `typedef` 会打断 16 个测试文件里的别名用法。

**意图**：把 `using` 补到能完全替代 `typedef` 的现有能力，迁走全部依赖，再移除 `typedef`。收口后语言只有一种类型别名写法。

## 2. 现状（CLI 实测）

### 2.1 `using` 已可用

| 写法 | 状态 |
|---|---|
| `using I = int32;` | ✅ |
| `using P = int32*;` | ✅（IR 为 `ptr`；`p[0]` 可用；`*p` 对 typedef/using 都失败，见非目标） |
| `using Sl = int32[];` | ✅ |
| `using O = int32?;` | ✅ |
| `using BI = B<int32>;` | ✅ |
| `using S = struct { int32 x; };` | ✅（匿名 struct/union 皆可） |
| `using S2 = struct S2 { int32 x; };` | ✅ |
| `using T = struct S;`（S 已定义） | ✅ |
| `namespace A { using H = int32; }` | ✅ |
| 重复别名 | ✅（后者覆盖） |

### 2.2 `using` 缺口

| 写法 | 现状 |
|---|---|
| `using A = int32[3];` | 解析成 `int32`，维度丢失 |
| `using Fn = int32 (*)(int32);` | 静默截断成 `int32` |
| `using E = enum { A, B };` | 枚举量 `A/B` 不可见（`visit(UsingDeclAST)` 未注册） |
| 块内 / class 内 / struct 内 `using` | 解析错误 |
| `[[deprecated]] using T = ...;` | 不折叠弃用状态 |
| `[[repr(C)]] using T = ...;` | 静默接受（typedef 会报错） |

### 2.3 `typedef` 基线（迁移必须覆盖）

| 写法 | 状态 |
|---|---|
| `typedef int32 I;` | ✅ |
| `typedef int32* P;` | ✅（`p[0]` 可用；`*p` 失败，既有缺陷） |
| `typedef int32 A[4];` / `[2][3]` | ✅ |
| `typedef int32 (*Fn)(int32);` | ✅（含作为参数/返回值） |
| `typedef struct { ... } S;` | ✅ |
| `typedef struct S { ... } S2;` | ✅ |
| `typedef enum : uint16 { ... } E;` | ✅（枚举量可见） |
| `typedef int32? Opt;` | ✅ |
| `namespace A { typedef int32 H; }` | ✅ |
| 重复 typedef | ✅ 允许 |
| 块内 / class 内 / struct 内 `typedef` | ❌（class 体内被静默忽略，struct/块内解析错误） |
| `typedef struct Node Node;` 前向别名 | 解析通过但指向占位类型，定义后无成员（既有缺陷） |

结论：`using` 需要补齐 **固定数组、函数指针、内联 enum 枚举量、注解语义** 四项；作用域与 `typedef` 保持一致（顶层 + namespace），不丢现有能力。既有的模板别名 `template<typename T> using Vec = ...` 必须原样保留。

## 3. 决策

1. **RHS 是完整 type-id**：`using Name = <type-id>;`。新增 `Parser::parseTypeId()`，只被 `using` 使用，不改动现有声明解析路径。type-id = `parseType()`（基础类型/指针/切片/optional/模板实例）+ 可选抽象函数指针 `(*)(params)` + 固定数组后缀 `[N]...[M]`。
2. **固定数组后缀**：只接受整数字面量维度（与 typedef 相同）；从右到左建 `ArrayType`，支持多维；VLA 仍拒绝。`parseType()` 已消费 `[]` 切片，固定 `[N]` 留到 `parseTypeId`，两者不冲突。
3. **函数指针**：复用 `parseFunctionPointerDeclarator(type, name, requireName=false)`，产生 `Pointer(FunctionType)`；参数解析复用 `parseFunctionPointerType`（含嵌套回调、`void`、可变参数）。
4. **内联 enum**：`visit(UsingDeclAST)` 增加与 `visit(TypedefDeclAST)` 相同的枚举量注册（`enumConstants`），使 `using E = enum { A, B };` 后 `A/B` 可见。
5. **注解语义对齐**：`UsingDeclAST` 走 `validateAnnotations(..., "using")`；`repr(C)` 等不合法目标报错，`deprecated` 折叠到别名 `TypedefType`（与 typedef 一致）。
6. **typedef / type 硬错误**：`parseDeclarationImpl` 遇到 `TOKEN_TYPEDEF` 或标识符 `type`（后跟 `IDENT =` 形态）立即报错，消息提示 `using Name = Type;`，随后跳到 `;` 恢复；顶层、namespace、块、struct/class/union 成员体全部覆盖。`isTypeStart()` 保留 `TOKEN_TYPEDEF`，保证进入该诊断而不是 “expected expression”。`type` 原先的 distinct-type 语义随 DEC-16 关闭，不提供替代构造。
7. **移除 `TypedefDeclAST` 与 `TypeDeclAST`**：删除 parser 的 `parseTypedefDecl` 与 `type` 分支、两个 AST 节点、sema 的 visit/分派、codegen、TemplateInstantiator 克隆分支，以及对应单元测试。**保留 `TypedefType`** 作为内部别名类型节点（`using` 仍经 `addTypedef` 存储），本轮不做重命名。
8. **作用域**：`using` 仅顶层 + namespace（与 typedef 现有能力对齐）。块内 / class 内 / struct 内别名是**非目标**：它需要真正的局部类型作用域，`TypeContext` 目前是全局扁平表，单开一轮。
9. **`using` 仍是上下文敏感标识符**，不新增词法关键字，避免无关的词法/文档连锁改动。
10. **历史文档不改**：`docs/superpowers/specs|plans/` 里已提交的历史设计保持原样；只更新 `docs/spec/` 下的现行规范。

## 4. 迁移规则

| 旧 | 新 |
|---|---|
| `typedef T A;` | `using A = T;` |
| `typedef int32* A;` | `using A = int32*;` |
| `typedef int32 A[4];` / `[2][3]` | `using A = int32[4];` / `int32[2][3];` |
| `typedef int32 (*Fn)(int32);` | `using Fn = int32 (*)(int32);` |
| `typedef int32? O;` | `using O = int32?;` |
| `typedef int32[] S;` | `using S = int32[];` |
| `typedef struct { ... } S;` | `using S = struct { ... };` |
| `typedef struct S { ... } S2;` | `using S2 = struct S { ... };` |
| `typedef enum : uint16 { ... } E;` | `using E = enum : uint16 { ... };` |
| `typedef struct Node Node;` | `struct Node;` + 直接用 `Node`（避开前向别名占位缺陷） |
| namespace 内 typedef | namespace 内 using |
| `type Name = T;` | `using Name = T;` |

迁移清单（16 个测试文件，47 处含注释；另加 `examples/all_features.cmm`）：
- e2e：`test_demo_regressions`（数组/函数指针）、`test_enum_underlying`、`test_generics`、`test_namespace`、`test_optional_result`、`test_anon_type`、`test_designated_init`、`test_union`、`test_atomic`。
- 单元：`test_parser`（6 处，typedef 语法断言改 using，新增硬错误断言）、`test_semantic_analyzer`（2）、`test_annotation_semantics`（1）、`test_compile_time`（1，死分支毒化用例）、`test_decl_nodes`（TypedefDeclAST 节点测试删除或改 UsingDeclAST）、`test_codegen_context`（1，改 UsingDeclAST 或删）。
- `test_type_nodes` 的 `TypedefType` 测试保留（内部类型仍存在）。
- `test_compile_time` 的死分支别名用例：若死分支块能解析 `using` 则改写；否则删除该子用例，保留指针/数组两种包装。

## 5. 验收矩阵

必须新增/保留的行为测试（e2e 或 parser 级）：

1. `using I = int32;` 变量/参数/返回可用。
2. `using A = int32[4]; A a; a[0]; sizeof(A)==16;` 设计符初始化补零。
3. `using M = int32[2][3]; M m; m[1][2];` 多维下标。
4. `using P = int32*; P p = &x; p[0];`（不测 `*p`，既有缺陷不在本轮）。
5. `using Fn = int32 (*)(int32,int32); Fn f = add; f(1,2);` 作为参数与返回值使用。
6. `using E = enum : uint16 { Lo=1, Hi=2 }; E e=Hi; Lo` 可见。
7. `using S = struct { int32 x; };` / `using U = union { ... };` 匿名聚合。
8. `using S2 = struct S2 { ... };` 命名内联聚合。
9. `using O = int32?;` / `using Sl = int32[];` / `using BI = B<int32>;`。
10. `namespace A { using H = int32; } A::H`。
11. 重复别名后者覆盖。
12. `[[deprecated]] using` 使用时告警；`[[repr(C)]] using` 报 “not valid on using”。
13. `typedef int32 T;` 与 `type T = int32;`（顶层与块内）→ 报错且消息含 `using` 提示。
14. 全量 `ctest` 通过（基线 1493）；`tests/`、`examples/`、`libs/` 下的 `.c`/`.cmm` 语言源与 `docs/spec/` 现行规范中不再有 `typedef` / `type Name =` 用法（历史 docs 除外）。

## 6. 非目标

- 块内 / class 内 / struct 内 `using`（需要局部类型作用域）。
- 前向别名与定义的类型身份合并（`using N = struct Node;` 后定义仍无成员）。
- 把内部 `TypedefType` 重命名为 `AliasType`。
- 修复 `*p` 对指针别名不解引用的既有缺陷（typedef/using 同样存在）。
- 保留 typedef 的 deprecated 警告期（已裁定硬错误）。
- 保留 `type` 的 distinct-type 语义（已裁定一并删除）。

## 7. 风险与对策

- **type-id 后缀顺序**：`parseType()` 已吃 `*`/`[]`/`?`；`parseTypeId` 在其后处理 `[N]` 与 `(*)(...)`。测试覆盖 `int32*[4]`、`int32[2][3]`、函数指针，防止静默截断。
- **函数指针复用**：`parseFunctionPointerDeclarator` 的 `requireName=false` 分支已存在，先加“无名抽象声明符”测试再改。
- **enum 注册**：镜像 `visit(TypedefDeclAST)` 的 `enumConstants` 逻辑；注意限定名路径（namespace 内 using）。
- **删除 TypedefDeclAST**：引用集中在 parser/sema/TemplateInstantiator/Decl/4 个测试；先迁移测试再删节点，保证每步全量绿。
- **硬错误恢复**：报错后跳到 `;`，避免级联与死循环；顶层与块内各加一个测试。
- **静默行为变化**：`[[repr]] using` 从“接受”变“报错”是本次目标，需在 Progress 里写明。

## 8. 任务拆分（供 plan）

1. `using` type-id：固定数组（1D/多维/数组指针）。
2. `using` type-id：函数指针与组合。
3. `using` 内联 enum 枚举量 + 注解/deprecated 对齐。
4. 迁移全部测试到 `using`（保持全量绿）。
5. `typedef` 硬错误 + 删除 `TypedefDeclAST`（parser/AST/sema/codegen/测试）。
6. 文档（grammar/keywords/semantics/abi/conversions/fixed-width-types）+ TODO/Progress。

## 9. Builder-check 决议

独立 builder 审阅后提出的问题，逐条落定：

1. **`type Name = T;`（TypeDeclAST）**——**已裁定：一并删除，只留 `using`**（选项 A）。`type` 与 `typedef` 都变硬错误；`examples/all_features.cmm` 的 `type Speed = float64;` 迁移为 `using Speed = float64;`；DEC-16/PAR-24 标记为“删除，只留 using”。
2. **成员体内的 `typedef`**——硬错误覆盖所有位置：顶层、namespace、块、struct/class/union 成员体。class 体当前“静默忽略”的洞必须堵上；成员循环遇到 `TOKEN_TYPEDEF` 报同一提示并跳到 `;`。
3. **示例与资源**——`examples/all_features.cmm` 纳入迁移（第 2 节全是 typedef/using/type），并加一次“示例可编译”验证。`resources/main.c` 不纳入：它不被测试/构建消费，且含 `unsigned long` 等本语言不支持的写法（C 展示稿）。
4. **“无 typedef”验证口径**——检查范围：`tests/`、`examples/`、`libs/` 下的 `.c`/`.cmm` 语言源，以及 `docs/spec/` 现行规范。豁免：`src/frontend/Lexer.cpp` 的 `"typedef"→TOKEN_TYPEDEF` 映射、`Token.*` 名称、`Parser.cpp` 的诊断文案（硬错误需要识别该 token）。
5. **块/类内 `using` 诊断**——仍是非目标，但不再留“expected expression”级联：语句入口与成员循环识别 `using IDENT =` 形态，报“`using` alias declarations are only allowed at namespace scope”，跳到 `;`。加 parser 测试。

## 10. 任务拆分（修订）

1. `using` type-id：固定数组（1D/多维/数组指针）。
2. `using` type-id：函数指针与组合。
3. `using` 内联 enum 枚举量 + 注解/deprecated 对齐。
4. 迁移测试与 `examples/all_features.cmm` 到 `using`（保持全量绿）。
5. `typedef` / `type` 硬错误（含成员体）+ 删除 `TypedefDeclAST` / `TypeDeclAST`；块/类内 `using` 定向诊断。
6. 文档（grammar/keywords/semantics/abi/conversions/fixed-width-types）+ TODO/Progress（DEC-16/PAR-24 关闭）。
