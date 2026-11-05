# Lambda 语言缺口补齐（PAR-11 扩展）

> 日期：2026-11-04 ｜ 状态：已实现（Tasks 1–5 完成）｜ 范围：仅语言层 lambda，不涉及 FFI
>
> 注：§1–§2 保留实现前基线与实测缺口，仅作背景；当前行为以 §4 决策与
> `docs/spec/semantics.md` §7.5 为准。

## 1. 意图与 why（实现前基线）

当前 lambda 已能工作：无捕获退化为函数指针，有捕获生成匿名闭包 struct，
`auto` 可保存闭包，`[=]`、`[a]`、`[&a]` 和立即调用均可用。但实测仍有一批
语言完整性缺口，影响正常写法：

- `[&]` 全按引用捕获不支持。
- `[=, &x]` / `[&, x]` 混排不支持。
- `[x = expr]` 捕获初始化不支持。
- `[this]` 只能写 `this->v`；直接写 `v` 报未声明，`[=]` 也不会隐式捕获 `this`。
- 嵌套 lambda 内层未捕获外层变量时会绕过 sema，codegen 段错误。
- 返回类型推断只看 body 顶层第一个 `return`；只在 `if` 内 return 时被推断为 `void`。

本轮把这些补齐到“常见 C++ 风格 lambda 写法可用”，不扩大到闭包类型逃逸
（函数返回、struct 字段、数组元素、非模板参数）等更大的类型系统改造。

## 2. 现状与实测（实现前基线）

已支持：

- `[]`、`[=]`、`[a]`、`[&a]`、`[this]`（仅 `this->v` 形式）。
- 无捕获 → 函数指针；有捕获 → 匿名闭包 struct。
- `auto` 保存闭包并反复调用；立即调用 `[...](...){...}(args)`。
- 按值捕获拷贝、按引用捕获写穿原对象。

实测缺口（`build/bin/my_llvm_c`）：

- `[&]` 解析直接报 “expected capture name or ']'”。
- `[=, &b]` 解析报 “expected ']' after '='”。
- `[x = expr]` 解析失败。
- 方法内 `[this]` + `v` 报 E2001；`[=]` + `v` 报 unknown capture 'v'。
- 嵌套 lambda 内层 `[]` 引用外层捕获变量，编译器段错误。
- `if (c) { return 1; }` 无顶层 return 的 lambda 被推断为 `void`，随后报返回类型不匹配。

## 3. 目标 / 非目标 / 反目标

### 目标

- 捕获列表：`[&]`、`[=, &x]`、`[&, x]`、`[x = expr]`、`[&x = expr]`、`[this]`。
- 方法内 lambda 捕获 `this` 后，body 内可直接写成员名（`v`），等价于 `this->v`。
- 未捕获的外层局部变量在 sema 阶段报错，绝不进入 codegen；修复嵌套 lambda 崩溃。
- 返回类型惰性推断：在 body 语义分析过程中由第一个带值 `return` 推导（不进入嵌套
  lambda），不预先递归扫描整个 body；无带值 return → `void`；显式返回类型优先。
- 既有 `[]`/`[=]`/`[a]`/`[&a]`、无捕获函数指针、`auto` 闭包、立即调用行为不变。

### 非目标

- 有捕获闭包作为函数返回类型、struct 字段、数组元素、普通（非模板）函数参数。
- `mutable` 关键字与单独的只读捕获语义：按值捕获的写入作用于每次调用加载出的
  副本，不回写原对象，也不新增只读诊断（继续沿用 DEC-10 的无 `mutable` 模型）。
- 泛型 lambda `[](auto)`。
- `noexcept`、捕获初始化中的移动语义、`this` 的按引用捕获。
- FFI 回调桥接；本轮不碰。

### 反目标

- 未捕获外层变量静默生成非法 IR 或崩溃。
- `[this]` 捕获后仍要求用户在每个成员前写 `this->`。
- `[&]` 退化成按值捕获，或混排时默认/显式优先级不明确。
- 捕获初始化表达式在 lambda 创建点被求值多次（立即调用路径尤其）。

## 4. 语法与语义决策

### D1：捕获列表语法

```text
lambda-capture  = '[' [ capture-spec ] ']'
capture-spec    = '=' [ ',' override-list ]
                | '&' [ ',' override-list ]
                | capture-list
capture-list    = capture-item { ',' capture-item }
override-list   = capture-item { ',' capture-item }
capture-item    = [ '&' ] identifier [ '=' assignment-expr ]
```

- `[=]`：body 中实际引用的外层局部/参数按值捕获。
- `[&]`：同样集合按引用捕获；`this` 永远按值（指针）捕获。
- `[=, &x]` / `[&, x]`：默认模式 + 显式覆盖。
- 显式捕获覆盖默认模式；允许冗余写法（如 `[=, x]`），不额外诊断。
- `[&this]` 诊断：`this` 只能按值捕获。
- `[this = expr]` 诊断：`this` 不能做捕获初始化。

### D2：捕获初始化

- `[x = expr]`：在 lambda 创建点求值 `expr`，把值拷贝进闭包字段；body 内 `x` 是新变量。
- `[&x = expr]`：`expr` 必须是左值；闭包字段存其地址；body 内 `x` 绑定该地址。
- `expr` 在**外层作用域**分析；`[x = x]` 的 RHS 是外层 `x`，LHS 是新的捕获名。
- 捕获名与参数/其他捕获重名 → 诊断。
- 闭包字段名仍为捕获名；codegen 用 `Capture::init` 区分“从外层变量取”与“求值表达式”。
- 按值捕获（含 `[x = expr]`）字段保存创建点拷贝；每次调用把字段加载进本次调用的
  局部 alloca，写入只影响该副本，既不回写原对象也不跨调用保留。当前实现没有
  `mutable` 关键字，也没有只读诊断，本轮保持现状。

### D3：`this` 与隐式成员访问

- `[this]` 显式捕获：闭包字段类型为 `Class*`，按值存储。
- 在捕获了 `this` 的 lambda body 内，未解析的标识符若是当前类（含基类）字段，则
  脱糖为 `this->field`；访问控制沿用当前方法上下文。
- `[=]` / `[&]` 在方法内：若 body 使用 `this` 或某个字段名，自动加入 `this` 捕获
  （按值）；只有局部变量被使用时不会多捕获 `this`。
- `[]` 内不解糖：`this->v` 的 `this` 报 `variable 'this' is not captured by lambda`；
  裸字段 `v` 因未找到符号报 E2001 `use of undeclared identifier 'v'`。
- 非方法上下文 `[this]` → 诊断。
- 捕获初始化表达式在**外层方法作用域**分析；因此 `[x = v]` 仍要求写 `this->v`
  或 `this.v`，不把 lambda body 的隐式成员规则扩展到初始化表达式。

### D4：未捕获外层变量与静态存储

sema 维护 lambda 作用域栈（lambda body scope + 已捕获名集合）。解析标识符时：

- 若符号在 lambda body scope 或其子块内声明 → 允许。
- 若符号在 global/module scope → 允许（全局变量/函数/枚举不受捕获限制）。
- 若符号是函数内 `static` / `thread_local` / 块级 `extern`（静态存储）→ 允许直接访问，
  不进入闭包字段。
- 若符号在 lambda body 之外的局部作用域 → 必须在该 lambda 的已捕获集合中；
  否则报 `variable 'x' is not captured by lambda`。

这条规则同时覆盖嵌套 lambda：内层 `[]` 引用外层捕获变量会在 sema 失败，不再进入 codegen。
捕获发现会进入嵌套 lambda 的 body、显式捕获名和捕获初始化表达式——外层 `[=]`/`[&]`
因此会为内层实际需要的局部变量/`this` 建立字段。按引用捕获的悬垂仍是调用方责任
（与现有 DEC-10 一致）。

> 行为修正：当前 `[=]` 会把全局变量也拷进闭包，导致 `g = 7` 只改副本。本设计按
> C++ 语义改为全局/静态存储直接访问；这是有意的 bug fix，不是兼容性目标。

### D5：返回类型推断

- 显式 `-> Ret`：不做推断，继续由普通 return 检查校验。
- 省略返回类型：**不预扫描**。在 body 正常语义分析过程中，由 `visit(ReturnStmtAST)`
  惰性推断：
  - 第一个带值 `return` 设置返回类型；后续 return 走既有兼容性检查与 codegen cast。
  - 只有无值 `return` 或没有 return：`void`。`return;` 与 `return voidExpr;` 都算无值。
  - 先出现无值 return、后出现带值 return：在带值 return 处把返回类型从 `void` 改为
    该表达式类型，并对先前的**每一个**无值 return 报错（实现按 `pendingVoidReturns`
    列表逐个诊断，而不是只记第一个）。
- 这样 `return` 可以引用 body 内先声明的局部变量/`auto` 闭包，不依赖“先把整个 body
  扫一遍”的顺序假设；`if`/`switch`/循环内的 return 也自然覆盖。
- 不进入嵌套 lambda 的 return（各自独立推断）。

### D6：闭包构造与立即调用

- 闭包构造逻辑抽成单一 helper，`LambdaExprAST::codegen` 与 `LambdaCallExprAST::codegen`
  共用，避免捕获初始化表达式被求值两次。
- 立即调用直接使用 `LambdaExprAST::codegen` 返回的闭包值，spill 到栈槽后把地址传给
  生成的 `__lambda_N__call`，不再重新构造闭包。
- `[this]`、普通按值/按引用、捕获初始化都在同一 helper 中处理。

## 5. 数据模型变更

`LambdaExprAST::Capture`：

```cpp
struct Capture {
    std::string name;
    bool byRef = false;
    std::unique_ptr<ExprAST> init;  // 非空 = `[x = expr]` / `[&x = expr]`
};
```

- `Capture` 变为 move-only；sema 内用 move 构建 effective 列表。
- `LambdaExprAST` 新增 `bool captureAllByRef = false;`。
- `VariableExprAST` 新增 `std::unique_ptr<ExprAST> implicitThisAccess;`
  sema 填充合成 `MemberAccessExprAST(this, field)`；codegen 优先委托。
- `Symbol` 新增 `bool isStaticStorage = false;`，sema 在变量声明时标记
  global/static/thread_local/块级 extern；捕获过滤用它排除静态存储。
- `SemanticAnalyzer` 新增 lambda frame 栈（body scope、captured names、capturesThis）
  与本地作用域查找 helper；新增“正在推断 lambda 返回类型”的 frame 字段，
  以及先前的无值 return 记录（用于后出现带值 return 的冲突诊断）。

## 6. 边界与生命周期

- 捕获初始化按值存储的是值拷贝；含 `string` 等拥有型聚合时沿用现有聚合拷贝规则，
  本轮不新增深拷贝语义。
- `[&x = expr]` 的引用生命周期由调用方负责；与 `[&x]` 相同。
- `[this]` 捕获的是指针；lambda 不能比对象活得更久，沿用调用方责任。
- 未捕获局部变量诊断后不再生成 IR，避免任何 backend 崩溃。

## 7. 测试计划

- Parser：`[&]`、`[=, &x]`、`[&, x]`、`[x = expr]`、`[&x = expr]`、`[this]`、`[=]` 兼容。
- Sema：未捕获外层变量诊断（含嵌套 lambda）、`this` 隐式成员、捕获初始化类型、
  `[&x = expr]` 非左值诊断、非方法 `[this]`。
- 已实现但未单独覆盖：`[&this]`、`[this = expr]`、重复捕获均有 sema 诊断，但没有
  独立测试；本轮保持 docs-only，不新增测试。
- E2E：
  - `[&]` 写穿多个局部。
  - `[=, &x]` 按值/按引用混排。
  - `[&, x]` 默认引用 + 单项按值。
  - `[x = expr]` 捕获初始化（值只求值一次）。
  - `[&x = expr]` 引用捕获初始化。
  - 方法内 `[this]` 直接写 `v`；`[=]` 自动捕获 `this`。
  - 嵌套 lambda `[=]` 正常；内层 `[]` 未捕获 → 编译错误而非崩溃。
  - 返回类型推断：`if` 内 return、`switch` 内 return、无 return → void。
- 全量 `ctest --test-dir build -j8` 无回归；`examples/all_features.c` 仍输出 `ALL FEATURES OK`。

## 8. 文档

- `docs/spec/grammar.ebnf` lambda-capture 规则。
- `docs/spec/semantics.md` §7.5 捕获模式、`this`、未捕获诊断、返回推断。
- `TODO.md` / `Progress.md` 记录本轮 lambda 缺口收口。

## 9. 实现注记（2026-11-04 收口）

- D5 的“无值 return”包含 `return;` 与 `return voidExpr;`；先无值后有值的场景通过
  `pendingVoidReturns` 列表逐个补报，而不是只记录首个。
- D2/D6 的按值捕获初始化对定长数组按整聚合拷贝（不能走数组→指针 decay 路径）。
- 有捕获闭包/file-scope 捕获初始化在 `currentFunction == nullptr`（文件作用域初始化器）
  时由 sema 直接诊断，避免 codegen 缺少插入点崩溃；错误文本见 semantics §7.5。
- 其余决策（`Capture::init`、`captureAllByRef`、`implicitThisAccess`、`Symbol::isStaticStorage`、
  `LambdaFrame`）与实现一致，未调整。
