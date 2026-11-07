# P1-09 挂账设计：string 所有权/拷贝语义（I4 收口）+ string_view

日期：2026-10-11
状态：草稿（待 builder-check 与批准）
范围：`string` 拷贝/移动语义改造、`str` 硬改名 `string_view`、I4（值拷贝别名双 free）收口。
依赖：P1-06（str/string 骨架）、P1-09（std.string/format 体系）。

## §0 决策表

| 决策 | 内容 |
|---|---|
| D1 | **两型模型**：`string`（拥有型，`{ptr,len,cap}`）+ `string_view`（非拥有视图，`{ptr,len}`，由 `str` 硬改名而来，DEC-18 先例）。 |
| D2 | **拷贝模型 B（C++ 式拷贝+移动）**：源为**已存在变量** → 深拷贝（独立缓冲）；源为**新鲜值**（构造结果/函数返回值/临时值）→ 移动（缓冲过户、源置空）。 |
| D3 | **`str` 硬改名 `string_view`**：`str` 成为未知标识符，全库迁移（实测 50 文件 / ~551 处）。 |
| D4 | **`destroy()` 保留**（语言无 RAII，NG-03 既有）：每次 free 自有缓冲；同值二次 destroy 沿用既有清零防御（free(NULL) 安全）；moved-from 值置空后 destroy 亦安全。 |
| D5 | **推翻 spec §1.2「无隐式堆分配」对 string 的约束**：string 拷贝/赋值会隐式分配（深拷贝）或隐式释放（赋值覆盖旧缓冲）——这是本次唯一突破，范围严格限于 `string` 类型，不推广到一般聚合。 |
| D6 | **调用点规则**：实参拷贝与否由**形参类型**决定——形参 `string` → 实参深拷贝（活变量）/移动（新鲜值）；形参 `string_view` → 实参**借用零拷贝**（对齐 C++ 传 `const std::string&`/`string_view` 的习惯）。 |
| D7 | **赋值自动释放旧缓冲**（用户确认，对齐 C++ `operator=`）：`b = a;` 先 `free(b.old)`（自赋值守卫）再深拷贝 a——隐式释放是 D5 记录的第二类隐式操作，接受。 |

用户决定（本会话）：两型对齐 C++ `std::string`/`std::string_view`；拷贝模型选 B；`str` 硬改名。

## §1 背景与目标

### 1.1 I4（挂账）

- P1-09 终审挂账 **I4「string 值拷贝别名双 free」**。已复现（本会话，三路径全部 `free(): double free detected` abort）：
  1. `string b = a; a.destroy(); b.destroy();`（值拷贝别名）
  2. 按值传参：`kill(a)` 内 destroy → 调用方再 `a.destroy()`
  3. struct 字段拷贝：`struct S { string s; } y = x;` 双双 destroy
- 根因：`string` 是值类型，C 语义的值拷贝共享同一 malloc 缓冲；`destroy()` 只清自身字段（同值二次 destroy 安全），别名副本仍指已释放内存。
- 原 spec §1.2 将其归为「用户责任」（C 语义，NG-04 无所有权模型）。终审判 Critical 挂账；用户方向：升级为**编译器保证拷贝安全**。

### 1.2 目标

- `string b = a;` 得到**独立缓冲**：`b.destroy()` 不影响 `a`——I4 三路径全部转为安全。
- 对齐 C++ 心智：`string` = 拥有值（拷贝即独立、返回/构造即过户）；`string_view` = 只读借用视图。
- 不引入一般所有权/生命周期模型（NG-04 保持），`string` 是特例而非体系。

### 1.3 验收

- I4 三个复现用例（§9）从「abort」转为「正确结果」。
- 全量 `ctest` 绿（基线 1212）；`str`→`string_view` 迁移后无残留 `str` 标识符。
- 新 e2e 钉住：拷贝独立、移动过户、返回无泄漏、moved-from 空值、聚合含 string 字段拷贝。

## §2 类型模型

| 类型 | 布局 | 拥有 | 生命周期 |
|---|---|---|---|
| `string_view`（原 `str`） | `{ptr, len}` | 否（借用） | 随源数据；源 string destroy 后悬垂（用户责任，既有文档） |
| `string` | `{ptr, len, cap}` | 是 | 显式 `destroy()`；无 RAII |

- 字面量即 `string_view`（静态全局，永久有效）。
- `char* → string_view` 经 `str_from_c`（改名后仍为 `str_from_c`，返回类型改为 `string_view`）。
- `string → string_view`：**隐式投影**（借用，零拷贝）——format/print 既有路径（PrintFormat.cpp:252）保持。
- `string_view → string`：不自动，必须 `string.new(view)`（DEC-20 既有规则）。

## §3 拷贝/移动语义（D2，模型 B）

### 3.1 拷贝点（深拷贝：malloc(len) + memcpy，cap=len）

| 场景 | 行为 |
|---|---|
| `string b = a;`（a 为已存在变量/字段/成员访问） | 深拷贝，a 保持有效 |
| 按值传参 `f(a)`（**形参类型为 `string`**，a 为已存在值） | 深拷贝，调用方不受影响 |
| 实参为 `string_view` 形参（`f2(a)`，f2 形参 `string_view`） | **借用零拷贝**（D6） |
| 三元/条件 `cond ? s1 : s2` 产生 string 值（含活变量分支） | 拷贝（s1/s2 均保持有效） |
| 三元**全部**为新鲜值（`cond ? string.new("x") : string.new("y")`） | 移动（避免临时泄漏，§3.2 特例） |
| 聚合（struct 含 string 字段）拷贝/传参/返回 | **字段感知**：string 字段深拷贝，其余字段按原语义 |

### 3.2 移动点（转移 {ptr,len,cap}，源三字段置空）

| 场景 | 行为 |
|---|---|
| `string s = string.new("x");`（源为构造/调用 rvalue） | 移动（无二次分配，无临时泄漏） |
| `string s = f();`（源为函数返回值 rvalue） | 移动 |
| `f(string.new("x"))`（临时实参，形参 `string`） | 移动 |
| `return s;`（返回局部/表达式） | **总是移动**（源在 return 后死亡，移动消除泄漏；`defer s.destroy(); return s;` 对置空源是安全 no-op） |
| `return cond ? s1 : s2;` | 三元先拷贝出临时（§3.1），return 移动该临时（s1/s2 保持有效） |
| 三元全为新鲜值（§3.1 特例） | 移动 |

> 临时值作为 `string_view` 实参（`f2(string.new("x"))`，形参 `string_view`）：借用后临时缓冲无人接管 → 泄漏。与现状（无 RAII 的既有行为）持平，文档明示用户责任（推荐传 `string` 形参走移动，或先存命名变量）。

### 3.3 赋值 `b = a;`

- 深拷贝 a 到 b；**先释放 b 的旧缓冲**（对齐 C++ `std::string::operator=`，防泄漏）。
- 自赋值 `b = b;` 守卫（旧缓冲释放前判 `ptr` 相等则跳过释放/拷贝）。
- 因 D2 下任意两个**活** string 互不共享缓冲（拷贝独立、移动置空源），释放 b.old 不会触碰 a 的缓冲。
- 这是本次第二个隐式操作（隐式释放），与 D5 一并文档化。

### 3.4 moved-from 值

- 置空后表现为**空 string**：`len()==0`、`s[i]` 越界（与空串一致）、`destroy()` = free(NULL) 安全、`append` 重新分配。
- 不引入 use-after-move 诊断（NG-04）：移动只发生在「一次性临时值」与「return 后的死亡局部」，**不存在活变量被静默移动**的路径——`string b = a;` 是拷贝，a 永远保持有效。因此无需移动跟踪/活性分析。

## §4 新鲜值识别（实现核心）

- 规则：**源表达式为「可直接过户的新鲜 rvalue」→ 移动；否则求值后深拷贝**。
- 新鲜 rvalue = 函数调用（含 `string.new`、`std::join/concat/repeat` 等返回 string 的调用）、构造结果。
- 非新鲜 = 变量引用、字段/成员访问、三元/条件、下标等求值出 string 的 lvalue/复合表达式 → 拷贝。
- 落点：sema 在初始化/实参/返回处按表达式 kind 标注 `Copy`/`Move`；codegen 消费标注。
- 默认安全：无法判定时**拷贝**（拷贝永不会损坏源；移动判定错误才会损坏）。

## §5 Codegen

- 合成 helper（既有 `CodegenContext` 惰性函数模式，仿 `getStrFindFn` 等）：
  - `stringCopy(dst: {ptr,len,cap}*, src: {ptr,len,cap}*)`：malloc(src.len) + memcpy + dst.len=src.len、dst.cap=src.len。
  - `stringMove(dst, src)`：字段转移 + src 三字段置零（无分配）。
  - `stringAssign(dst, src)`：自赋值守卫 → 若 dst.ptr != null 且 != src.ptr 则 free → stringCopy（D7）。
  - **聚合拷贝 helper**：单一递归「字段感知拷贝」——遇 string 字段调 `stringCopy`/`stringAssign`，其余字段整段拷贝；统一覆盖所有聚合拷贝点（赋值、初始化、实参、返回值、三元、`optional<string>`/泛型实例化自动继承），不逐个特判。
- 挂接点：赋值（Expr.cpp:1334 区域）、初始化（Decl 初始化）、实参传值（按形参类型 string/string_view 分流，D6）、返回值、聚合拷贝路径。
- `destroy()` 既有实现（清零字段）不动；moved-from 值 destroy 自然安全。

## §6 重命名迁移（str → string_view，D3）

- 量：50 文件 / ~551 处（src 140、tests/docs/libs 411）。
- 顺序：`src/`（类型定义、sema、codegen、AST）→ `libs/std/*.cmm` → `tests/` → `docs/`。
- 机制：`StrType` → `string_view` 类型名注册（Parser `isTypeStart`/`parseBaseType` 名字特判，与 P1-06 小写化同路径）；`TypeKind::Str` 保留内部名。
- DEC-18 先例：旧名成为未知标识符（`Optional→optional` 同款，量级更大）。`str_from_c` 函数名保留（参数/返回类型改 `string_view`）。
- `[]str`（split 返回）→ `[]string_view`；`split_destroy` 签名不变。

## §7 与既有机制的关系

- **I3（append 自别名）保留**：`string_view` 借用可能仍指向接收者自身缓冲（如 `a.append(a_view)`），realloc 后视图悬垂——既有运行时谓词（UGE/ULT + 重投影 + memmove）继续生效，不因本设计冗余。
- **split 视图生存期**：`split` 返回 `[]string_view`，元素借用源数据；源 string destroy 后悬垂为**用户责任**（既有文档，I4 同族改文档口径：值拷贝已安全，视图借用仍须用户管理）。
- **format/print**：string 投影为 string_view 借用（PrintFormat.cpp:252 不变）。
- **NG-04**：不引入一般所有权/生命周期模型。string 的拷贝/移动是**单类型语义**（编译器为 string 特判），不是借用检查器；`string_view` 悬垂仍是用户责任（对齐 C++ 的 dangling string_view 也是 UB）。
- **constexpr**：`string` 不参与 constexpr（既有挂账不变）；`string_view` 字面量 CT 比较沿用。

## §8 明确不做

- RAII/自动析构、GC/引用计数、借用检查器（NG-04）、SSO、move 语法/`std::move`（移动是编译器在拷贝点的自动判定，非用户可写）。
- `string_view` 的悬垂检测（对齐 C++：UB，用户责任）。
- 一般类型（非 string 的含指针 struct）的拷贝安全（不在本设计范围，保持 C 语义）。

## §9 测试与验收

- **I4 收口**（原 abort 三路径转安全）：`CopyAliasIndependent`、`PassByValueIndependent`、`StructFieldCopyIndependent`。
- 拷贝/移动语义：`CopyModifyOriginalUnaffected`、`MoveFromNewNoLeak`、`MoveFromCallNoLeak`、`MovedFromIsEmpty`、`DestroyMovedFromSafe`、`ReturnMovesLocal`（含 `defer destroy; return`）、`ConditionalReturnCopiesOperands`、`AssignFreesOldBuffer`、`SelfAssignSafe`。
- 聚合：`StructWithStringCopyDeep`、`StructWithStringAssignFreesOld`。
- 迁移：全量无 `str` 标识符残留（grep 断言）；`string_view` 全 API 回归（原 str 用例改名）。
- 回归：现有 string/format/split/split_destroy/string.cmm 全部用例；全量 `ctest` 绿。

## §10 已定案与留给 builder 的点

- **用户确认**：D7 赋值自动释放旧缓冲；D3 `str` 硬改名 `string_view`；D2 拷贝模型 B。
- 执行者 call（builder-check 后补定）：D6 调用点规则（形参类型决定拷贝/借用）；三元全新鲜值 → 移动；聚合拷贝用单一递归 helper；新鲜值判定「无法判定→拷贝」默认安全策略；`str_from_c` 保留函数名、仅改返回类型。
- 实现细节（builder 查代码定夺）：当前 struct 参数/返回值的 LLVM ABI 搬移方式、全部拷贝点清单、rename 迁移顺序细节。
