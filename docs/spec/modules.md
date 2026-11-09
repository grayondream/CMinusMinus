# Cmm 1.0 — 模块 / 命名空间 / 名称解析规范

> 对应 TODO：**MOD-01 ~ MOD-15**（重点 MOD-12~15）、**DEC-02/14/17**
> 状态：`[impl]` 以 `src/frontend/Parser.cpp`、`src/sema/SemanticAnalyzer.cpp`、`src/ast/Symbol.cpp` 为准；`[plan]` 为目标。
> 前提：**无预处理器**（NG-02），模块是唯一的跨文件组织方式；C 互操作走 `extern` + `std.c` 绑定层（MOD-09/STD-23）。

## 1. 现状 `[impl]`

| 语法 | 解析结果 | 状态 |
|---|---|---|
| `module a.b;` | `TranslationUnitAST::moduleName`（+ `ModuleDeclAST`） | `[impl]` 点分名（`::` 拒绝）；驱动校验文件↔模块名（MOD-04/13） |
| `import a.b;` / `import "dir/f.cmm";` | 记入 `TranslationUnitAST::imports` | 驱动 `ModuleLoader` 解析并**前插**声明（MOD-04/14） |
| `export decl` / `export namespace A { … }` | 置 `DeclAST::isExported` | `[impl]` 参与模块的默认私有，导出者进全局（MOD-05/06） |
| `namespace A { ... }` | `NamespaceDeclAST` | 名称修饰 + 限定/非限定查找（PAR-22/MOD-12） |

导入解析（`src/driver/ModuleLoader.*`）：搜索顺序为**导入者目录 → `-M/--module-path` → `STD_DIR`**；点分名映射为目录（`a.b` → `a/b.cmm`）；`loaded` 规范化路径去重、`active` DFS 栈检测**导入环并报错**（MOD-07）。被导入的声明**前插**到导入单元之前，因此对其全部可见（单遍语义分析需要）。`std.c` 绑定层即 `libs/std/c.cmm`（内嵌兜底）。

可见性（`src/sema/SemanticAnalyzer.cpp`）：符号表仍为 `Scope` 重载集链。声明 `module NAME;` 的单元为**参与模块**，其声明注册在**每模块私有作用域**（`moduleScopes`）中；只有 `export`/`public` 的声明才同时注册到全局作用域。因此导入者只能看到导出接口，而模块内部仍可使用私有成员。未声明 `module` 的**遗留/匿名单元**（如预置 `std.c` 与既有测试）保持全部全局可见，向后兼容。`main` 始终视为导出（程序入口）。

## 2. 命名空间（MOD-12，DEC-17）`[plan]`

### 2.1 语法（对齐 PAR-22 / grammar.ebnf）

```cmm
namespace geometry {        // 声明
    struct Point { int32 x; int32 y; }
}

namespace geometry::ops {   // 嵌套（namespace 用 `::`）
    int32 dot(Point a, Point b) { ... }
}

namespace geometry {        // 开放命名空间：可再次进入
    struct Circle { Point c; int32 r; }
}
```

- namespace 名与限定访问使用 `::`；`namespace A::B::C` 合法，`namespace A.B`
  不再合法（点分保留给 module 路径）。
- 限定名 `A::B::name`（`::` token 已实现，LEX-14/PAR-22）。
- 命名空间是**编译期名字分组**，与模块（文件/编译单元）**正交**：
  - 模块决定“哪些文件一起编译/如何导入”；
  - namespace 决定“符号的限定名与查找路径”。
- 关系与共存方式最终由 **DEC-17** 裁定；本文件建议：`module` 为物理边界，`namespace` 为逻辑边界，允许同模块多 namespace、同 namespace 跨模块。

### 2.2 查找规则

1. 非限定名：当前块 → 外层块 → 函数 → 当前 namespace（由内向外）→ 其外层 namespace → 模块根 →（若 import）被导入模块的**导出**符号。
2. 限定名 `A::x`：从全局根按名逐级进入 namespace；module 不是名字作用域，
   不产生 `module::x` 或 `module.x::x` 的访问形式。
3. ADL（实参依赖查找）**不引入**（保持 C 风格可预测，见 NG-03）。

## 3. 模块与文件（MOD-04/13）`[impl]`

- **一文件一模块**：每个 `.cmm` 源文件最多一条 `module` 声明；模块名只使用点分路径（如 `module std.vec;`），记入 `TranslationUnitAST::moduleName`。`module a::b;` 会被拒绝，并提示 `::` 保留给 namespace。
- 文件↔模块映射：`module a.b.c;` ↔ 路径 `a/b/c.cmm`；缺省模块名为文件名（去扩展名）。`import a.b;` 解析到该文件后，若文件声明了模块名，则**必须与导入说明符一致**，否则报错（`ModuleLoader`，MOD-13）。
- 编译单元 = 一个模块文件（或显式引入的一组）。
- 导入搜索路径由驱动提供（`-M`/`--module-path`，见 TOOL-04/TOOL-08），默认搜索当前目录与标准库根。

> 待定：`module` 是否存在子模块/分区（partial）语义——归入 DEC-17 一并裁定。
> 现状限制：未强制 `module` 必须位于文件首部、未拒绝重复 `module`；类型名可见性尚未纳入模块作用域（`typeCtx` 仍全局），详见 §11。

## 4. 导入与可见性（MOD-05/06/11，DEC-02）

### 4.1 导入 `[impl]`

```cmm
import math;            // 形式 1：引入模块的导出符号
// 形式 2/3（选择性导入、别名）仍为提案，未实现；模块说明符不要写 `::`。
```

- 被导入模块内**未标记 `export`/`public`** 的符号不可见（`[impl]`）。
- 访问语法由 **DEC-02** 裁定：`import` 只控制可见性，**不产生模块名限定**：
  - 模块**根部**的 `export` 声明：导入后直接非限定访问（`import math; add(...)`）；
  - `namespace` 内的 `export` 声明：导入后必须限定访问（`std::min(...)`）。
  - 因此 `import std.vec;` 的 API 是 `std::Vec`，不是 `Vec`，也不是
    `std.vec::Vec`。模块是物理边界，namespace 是逻辑名字边界（DEC-17）。

### 4.2 可见性 `[impl]`

| 标记 | 作用域 | 状态 |
|---|---|---|
| `export` | 模块外可见（其后声明，或整个 namespace） | `[impl]` |
| `public` | 顶层与 `export` 同义；类成员访问级别待补（PAR-04） | `[impl]`（顶层） |
| `private` | 仅本模块/本类型可见 | `[plan]`（类成员） |
| 无标记 | 参与模块：默认**模块内私有**；遗留/匿名单元：全局可见 | `[impl]` |
| `main` | 始终导出（程序入口） | `[impl]` |

参与模块的私有声明存放在该模块的私有作用域中，既对导入者不可见、又不与其
他模块的同名私有声明在**名字解析**阶段冲突；但注意代码生成仍共用同一 LLVM
模块符号空间（同名私有函数需靠 MOD-08 名称修饰进一步隔离，见 §11）。

## 5. 名称解析与作用域（MOD-01/02/03）`[impl → 待扩展]`

- 现有 `Scope`：块/函数/全局；`declare` 拒绝同层重名，`lookup` 沿父链上升。
- 重载：同层同名非函数符号冲突；函数进入 `OverloadSet`（见 semantics.md §3）。
- 前向声明：函数/struct/class/enum/union 已有占位类型 `[impl]`。
- **目标**：在全局之上增加 模块层 → namespace 层，查找顺序见 §2.2。

## 6. 循环依赖（MOD-07）`[impl]`

- `ModuleLoader` 用 DFS 的 `active` 栈检出导入环，报错并列出环路径，例如
  `circular import detected: …/a.cmm -> …/b.cmm -> …/a.cmm`；加载随后终止。
- 菱形依赖（多条无环路径到同一模块）由 `loaded` 集合去重，不报错。
- 类型级前向引用通过占位类型允许，但**导入环**禁止（与 Rust 类似）。
- 纯类型循环（`struct A { B* b; }` / `struct B { A* a; }`）允许，参照 C。

## 7. 名称修饰（MOD-15）

- 现状与目标编码见 [`abi.md` §4](./abi.md)。namespace 与模板实例需纳入编码，保证跨模块稳定、可复现（INF-07）。

## 8. 增量编译与缓存（MOD-10，DEC-14）`[plan]`

- 以**内容哈希**为键缓存 模块/AST/类型/IR；模块接口（导出符号表）单独序列化。
- 失效策略：实现变更、导入闭包任一变更 → 失效。
- 缓存格式与稳定性见 **DEC-14**；AST 序列化基础设施见 PAR-19。

## 9. C 互操作（MOD-09 / STD-23）

- 不解析真实 C 头文件；用 `extern` 声明 + 手写 `std.c` 绑定层。
- `extern` 符号保持未修饰 C 名（`isCName`，见 abi.md §4）。
- 关键缺口，列为 P0-02。

## 10. 已裁定项（DEC-02 / DEC-17）`[frozen]`

| 决策 | 结论 |
|---|---|
| DEC-02 | 导入后**直接非限定访问**；不引入 `math.add` 式模块成员运算符。需要限定名时用模块内的 `namespace`。 |
| DEC-17 | 模块 = **物理边界**（文件/编译单元/导入），namespace = **逻辑边界**（名字分组）；二者正交：同一模块可含多个 namespace，同一 namespace 可跨模块（由 `module` 决定可见性）。 |

其余未决：DEC-14（模块缓存格式，见 §8）。

## 11. 现状限制（`[plan]` 待补）

- **类型可见性**：导出类型（`export struct/class/union/enum/using/template`）在解析
  导入者前预载，已可跨模块使用；但 `typeCtx` 仍是全局表，非导出类型名对导入者
  暂不可隐藏，纳入模块作用域归入 MOD-06/08 后续。
- **私有同名符号**：名字解析阶段已按模块作用域隔离，但代码生成共用单一 LLVM
  符号空间；跨模块同名私有函数/全局变量仍需 MOD-08/MOD-15 的名称修饰。
- **模块声明位置**：已强制 `module` 位于文件首部、唯一、具名，并校验入口文件
  module 名 ↔ 路径；导入时仍按 MOD-13 校验声明名与说明符。
