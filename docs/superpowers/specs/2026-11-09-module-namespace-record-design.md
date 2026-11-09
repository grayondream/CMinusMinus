# Cmm 1.0 — Module/Namespace 与 Record 面设计（部分裁定）

> 状态：**已实施（2026-11-09）**
> 日期：2026-11-09
> 关联：`TODO.md` MOD-04/06/11/12/13、DEC-02/17；PAR-05/22、AGG、TYP-07/08
> 本文只冻结本轮两项：**module/namespace 职责分离**、**struct/class record 文法统一**。其余 1.0 P0 项仍单独裁定。

## 1. 背景

当前实现同时存在 `module` 与 `namespace`，但两者的职责在语法和查找规则上仍有重叠：

- module 名允许点分和 `::`，import 只接受点分；
- namespace 声明同时接受 `A.B` 和 `A::B`；
- 导入后的根导出符号可直接非限定访问，namespace 成员则必须限定访问；
- `struct` 与 `class` 共用 `StructDeclAST`，但 parser 是两套近乎重复的实现；
- `struct` 能写方法（实现路径），但文法只允许字段/类型别名；
- `struct` 不能写访问段，`class` 默认 private，两者成员能力不一致。

本轮把职责写死，并统一面向用户的 record 文法；不在本轮删除 `struct`，也不迁移 `string`/`string_view`。

## 2. 决策一：module 管文件/导入，namespace 管 API 名字

### 2.1 职责

| 概念 | 职责 | 不负责 |
|---|---|---|
| `module` | 文件、编译单元、导入/导出边界、可见性 | 不产生名字前缀，不提供 `module::name` 或 `module.name` 访问 |
| `import` | 把被导入模块的**导出**声明变为可见 | 不创建模块命名空间，不把模块名注入符号表 |
| `namespace` | 逻辑名字分组、API 限定名、嵌套名字树 | 不决定文件边界或导入 |

### 2.2 语法

- module 名与 import 说明符使用**点分路径**：

  ```cmm
  module std.vec;
  import std.mem;
  import "dir/file.cmm";
  ```

  `module`/`import` 中不允许 `::`；遇到时给出明确诊断：

  - `module names use '.'; '::' is reserved for namespaces`
  - `import specifiers use '.' or a quoted path; '::' is reserved for namespaces`

- namespace 名使用 **`::`**：

  ```cmm
  namespace std { ... }
  namespace geometry::ops { ... }
  ```

  `namespace A.B` 不再作为声明语法；遇到时诊断：

  - `namespace names use '::'; '.' is reserved for module paths`

- 类型/值的限定访问继续使用 `::`：`std::Vec<T>`、`geometry::ops::dot(...)`。
- 字符串路径 import 仍允许，语义与点分 module 名一致。

### 2.3 可见性与查找

- 模块根部的 `export` 声明：导入后以**非限定名**可见。
  - 例：`module util; export int32 helper();` → `import util; helper();`
- namespace 内的 `export` 声明：导入后以**限定名**可见。
  - 例：`export namespace g { int32 add(...); }` → `g::add(...)`
- namespace 成员在 namespace 内部可非限定访问（同级查找）；模块外必须限定。
- module 名本身不进入查找：`import std.vec;` 后不存在 `std.vec::Vec` 这种 module-qualified 访问；`Vec` 也不因模块名而可见；API 名由 `namespace std` 提供，因此正确写法是 `std::Vec`。
- 保留 `DEC-17`：module 与 namespace 正交；同一模块可含多个 namespace，同一 namespace 可跨模块。

### 2.4 迁移

- 仓库内 `namespace A.B` 的 3 处（`examples/all_features.cmm`、`tests/e2e/test_namespace.cpp` 两处）迁移为 `namespace A::B`。
- 仓库内没有 `module a::b` / `import a::b` 的用法；新增负向测试钉住诊断。
- 文档 `docs/spec/modules.md`、`docs/spec/grammar.ebnf` 同步。

## 3. 决策二：struct/class 统一 record 文法

### 3.1 面向用户的统一产生式

`struct` 与 `class` 共用一个 record 文法；`union` 仍是独立的存储模型，不纳入本轮统一。

```ebnf
record-decl        = record-keyword , [ identifier ]
                     [ ':' , [ access-specifier ] , qualified-name ]
                     [ '{' , { record-member } , '}' ] , ';' ;
record-keyword     = 'struct' | 'class' ;
record-member      = access-section
                   | field-decl
                   | function-decl
                   | operator-decl
                   | type-alias
                   | nested-type-decl
                   | static-member-decl ;
access-section     = ( 'public' | 'private' | 'protected' ) , ':' ;
```

两种 record 都允许：

- 字段、位域、多声明符、匿名聚合成员；
- 实例方法、运算符重载、`static` 数据/方法；
- 嵌套 `struct/class/union/enum`、成员 `using`；
- 访问段、继承子句、attached declarator；
- 注解（按 record 种类保留现有语义限制）。

### 3.2 默认访问级别

- `struct` 默认 **public**；
- `class` 默认 **private**；
- 访问段对两者都生效；`private:`/`public:` 可来回切换；
- `protected` 继续按现有单继承规则处理。

### 3.3 内部表示

- 纯数据 `struct` 继续使用 `StructType`；
- 带方法、访问段或需要 class 语义的 `struct`，内部复用 `ClassType`（布局仍等价）；
- `StructDeclAST::isClassDecl` 只记录**关键字来源**，不改变 `struct` 的注解规则：
  - `[[repr(C)]]` 仍只允许 `struct`/`union`，`class` 报 E2011；
  - 柔性数组仍只允许 `struct`；
- 新增 `StructDeclAST::hasAccessSections`，用于 parser/sema 判定是否需要走 `ClassType` 路径；
- 模板实例化克隆 AST 时必须复制该标志。

### 3.4 语义限制（不变）

- `class` 不支持柔性数组；
- `class` 不接受 `[[repr(C)]]`；
- `union` 不获得方法/静态成员/访问段/继承；
- 多继承仍不支持；
- `protected` 仅按现有单继承链放宽。

## 4. 1.0 scope 表（本轮相关部分）

| 层面 | 1.0 核心 | 1.0 后扩展 | 非目标 |
|---|---|---|---|
| module | 点分 module 名、路径 import、导出可见性 | import alias/选择性导入 | module partitions、模块缓存 |
| namespace | `::` 声明与限定访问、开放/嵌套 namespace | `using namespace` 等便利语法 | ADL |
| record | struct/class 统一成员文法、默认访问级别 | 删除 `struct`（如仍要做） | 多继承、虚函数 |
| string | 保持编译器 prelude 类型 | 真正迁移到 `std.string` | 本轮不做 |

## 5. 本轮明确不做

- **不删除 `struct`**；只统一文法。
- **不迁移 `string`/`string_view`**；它们仍是编译器已知的 prelude 类型。
- **不实现** `import M as N;`、`import M::{a,b};`。
- **不实现** module partitions、模块缓存、ADL。
- **不改变** `union` 的成员模型。
- **不改变** `std` 公开 API 的命名；仍为 `std::Vec`、`std::min` 等。

## 6. 验收标准

1. `module a::b;`、`import a::b;`、`namespace A.B` 均有明确诊断，不产生级联解析错误。
2. `import std.vec; std::Vec<int32>` 可通过；`Vec<int32>` 仍不可见；`import util; helper()`（根部导出）可通过。
3. `struct S { private: ... public: ... int32 get(); }` 可解析、可访问；private 成员类外访问报 E2009。
4. `class C { ... }` 默认 private 行为不回归；`struct` 默认 public 行为不回归。
5. 全量 `ctest` 通过；`examples/all_features.cmm` 仍输出 `ALL FEATURES OK`。
