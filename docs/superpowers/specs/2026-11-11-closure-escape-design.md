# Cmm 1.0 — Capturing-Closure Escape（按值返回）设计

> 状态：**设计草案，待批准**
> 日期：2026-11-11
> 关联：PAR-11 / CG-11 / DEC-10、README “capturing-closure escape”限制、MOD-06/08（模块类型可见性验证）
> 用户裁决：**按引用捕获（含 `this`）的闭包逃逸在编译期拒绝**；按值捕获闭包允许安全逃逸。

## 1. 意图

当前有捕获 lambda 只能可靠地用于局部变量或立即调用：

```cmm
auto f = [x](int32 y) -> int32 { return x + y; };  // 局部 OK
int32 r = f(1);
```

从函数返回时失败，因为 `auto` 只支持变量、不支持函数返回类型：

```cmm
auto make_adder(int32 x) {
    return [x](int32 y) -> int32 { return x + y; };  // return type mismatch: expected 'auto'
}
```

本轮让**按值捕获闭包可以跨函数生命周期安全返回**，并拒绝不安全的引用逃逸。

## 2. 目标

1. 支持自由函数的 `auto` 返回类型推导。
2. `auto make(int32 x)` 返回按值捕获闭包；调用方 `auto f = make(40);` 可反复调用。
3. 闭包按值返回时复制闭包结构；生命周期独立于创建栈帧。
4. 返回**按引用捕获**或 **`this` 捕获**的闭包 → 编译错误（悬垂风险）。
5. 保持现有 lambda/auto 局部行为、无捕获函数指针行为、模块类型导出行为不变。
6. 增加模块导出类型的回归测试（namespace 内 struct/class/template + 方法/静态/继承）。
7. 更新 README/TODO/spec 的现状说明。

## 3. 非目标

- 不支持在 struct 字段、数组元素、全局变量中保存“匿名闭包类型”（语言仍无 `decltype`/闭包类型名）。
- 不支持普通函数参数直接接收匿名闭包类型；模板参数 `F` 可接收闭包值（沿用泛型类型绑定），但不保证长期存储安全。
- 不支持按引用闭包逃逸（用户已裁决拒绝）。
- 不支持递归或相互递归的 `auto` 返回函数（推导需要已注册返回类型）。
- 不支持方法、`compile_time`、`compile_only` 函数上的 `auto` 返回（v1 只开放自由运行期函数）；模板体上的 `auto` 返回不在 v1 保证/测试范围内。
- 不引入堆分配、引用计数、GC 或类型擦除。

## 4. 语义

### 4.1 `auto` 返回类型推导

```cmm
auto make_adder(int32 x) {
    return [x](int32 y) -> int32 { return x + y; };
}
```

规则：

- `auto` 自由函数必须有函数体。
- 遍历 body 时，首个带值 `return` 的类型决定返回类型；数组返回按现有规则 decay 为指针。
- 后续带值 `return` 必须与首个类型兼容，否则诊断。
- 只有 `return;` 或没有 `return` → `void`。
- 先出现 `return;`、后出现带值 return → 诊断（与 lambda 惰性推断一致）。
- 函数签名在 body 分析结束后注册；因此 **auto 函数必须先定义后调用**（源序）。
- 递归调用自身、或调用尚未注册的后置 auto 函数 → 诊断（v1 限制）。
- `auto` 函数不能只声明不定义；不能是方法/`compile_time`/`compile_only`。模板体上的 `auto` 返回不在 v1 保证范围。

### 4.2 闭包返回

- 按值捕获：闭包 struct 的字段是捕获值的拷贝。返回时复制整个 struct；调用方持有独立副本。安全。
- 按引用捕获：闭包字段保存指向创建栈帧的指针；返回后悬垂。**返回表达式类型是带引用捕获的闭包 → 编译错误**。
- `this` 捕获：闭包保存 `Class*`；退出对象生命周期后可能悬垂。与按引用捕获同等处理，**禁止返回**。
- 错误消息：

```text
capturing closure with by-reference captures cannot escape by return
capturing closure with a 'this' capture cannot escape by return
```

- 通过变量间接返回同样检查：`auto f = [&x]...; return f;`。
- 按值闭包中如果捕获了指针，指针生命周期仍由调用方负责（与裸指针一致），不额外诊断。
- 每个 lambda 表达式生成名义匿名闭包类型；两个字段/参数/返回类型相同的独立 lambda 仍是不同类型（调用函数不同）。因此 `return` 分支应返回同一个闭包变量，不能各自新建 lambda 字面量。
- `this` 捕获检查已实现；由于 v1 不允许方法上的 `auto` 返回，直接构造“方法返回捕获 `this` 的闭包”在当前语法下不可达；同一检查由按引用捕获测试覆盖。

### 4.3 调用

```cmm
int32 main() {
    auto f = make_adder(40);
    return f(2) - 42;
}
```

- `make_adder` 的最终返回类型是匿名闭包 `StructType`；`auto f` 推导为该类型。
- `f(2)` 走现有 `StructType::isClosure` 调用路径（闭包地址 + 参数）。
- 生成的 `__lambda_N__call` 是模块内 internal 函数；跨函数调用正常。
- 跨模块返回按值闭包：函数导出后，调用方可用 `auto` 接收；闭包类型/调用符号仍在定义模块的 IR 中。v1 先覆盖同模块，跨模块加回归测试（若失败则记录限制，不在本轮扩展类型名可见性）。

## 5. 模块系统现状

当前 HEAD 已验证：

- `export namespace api { struct/class/template ... }` 可被 importer 使用。
- 方法、静态方法、模板方法、继承字段/方法可跨模块编译。
- `import std.mem;` 后 `std::Allocator`/`Allocator` 函数、`std::Arena`、`std::Pool` 可编译运行。
- `import std.vec;` 后 `std::Vec<int32>` 可编译运行。

因此本轮**不重写模块导出实现**；只新增回归测试覆盖 namespace 内类型/方法/模板/继承，并在 TODO/Progress 记录“模块类型导出已可用”。如果用户提供仍失败的样例，按该样例另开修复。

## 6. 实现边界

### 6.1 AST/类型

- `FunctionDeclAST` 增加 `bool isAutoReturn`（或检查 `returnType->kind == Auto`）。
- `StructType` 增加：
  - `bool closureHasByRefCapture = false;`
  - `bool closureCapturesThis = false;`
- 不新增闭包类型名；闭包类型仍是匿名 `StructType`。

### 6.2 Sema

- `visit(FunctionDeclAST)`：
  - 若 `returnType` 是 `Auto` 且是自由运行期函数：
    1. 校验 body 存在、非方法/模板/CT。
    2. 延迟签名/符号注册。
    3. `currentFunction = &node`，临时 `returnType = void`。
    4. 设置 `m_autoReturnFunction = &node`、`m_inferredAutoReturn = nullptr`。
    5. 分析 body（参数先入作用域）。
    6. 结束得到 `node.returnType`（inferred 或 void）；处理 pending bare returns。
    7. 用最终返回类型创建 `FunctionType`、注册 `m_allFunctions`/`compileTimeFunctions`/`compileOnlyFunctions`、`declareSymbol`。
  - `auto` 函数注册后，调用方才能解析；源序限制写入 spec。
- `visit(ReturnStmtAST)`：
  - 若 `currentFunction == m_autoReturnFunction`：
    - 首个带值 return 设 `m_inferredAutoReturn`；后续检查兼容。
    - bare/void return 在已有非 void 推断时诊断；否则挂 pending。
    - 跳过普通 `currentFunction->returnType` 检查。
  - 若返回值类型是闭包：
    - `closureHasByRefCapture` → 引用逃逸错误。
    - `closureCapturesThis` → this 逃逸错误。
- `visit(LambdaExprAST)`：创建闭包时写入两个 closure 标志。

### 6.3 Parser

- `auto f(...) { ... }` 已能解析（`parseBaseType` 返回 `Auto`）；只需 sema 支持。
- `auto f(...);` 原型：sema 报 `auto function must have a body`。

### 6.4 Codegen

- `FunctionDeclAST::codegen` 使用 sema 推断后的 `returnType`；无需新 ABI。
- 闭包返回走普通 struct 返回值（现有 `ReturnStmtAST`/`castValue`）。
- 闭包调用函数 `__lambda_N__call` 仍按需生成。

## 7. 诊断

- `auto function '<name>' must have a body`
- `auto return type is not supported on methods/templates/compile_time/compile_only functions`
- `auto function '<name>' has inconsistent return types: '<T1>' vs '<T2>'`
- `non-void auto function '<name>' must return a value`
- `capturing closure with by-reference captures cannot escape by return`
- `capturing closure with a 'this' capture cannot escape by return`
- `recursive auto-return function is not supported`（或解析失败的自然诊断；v1 记录）
- `auto function '<name>' must be defined before it is called`（源序限制）

## 8. 测试

### 8.1 正向

- `auto id(int32 x) { return x; }` 基本推导。
- `auto make_adder(int32 x) { return [x](int32 y) { return x + y; }; }` + `auto f = make_adder(40); f(2) == 42`。
- 多次调用返回的闭包互不干扰（按值捕获副本）。
- `auto` 返回 `void` / 只有 `return;`。
- `auto` 返回字符串/数组 decay（沿用现有规则）。
- 跨模块 `auto` 返回按值闭包（若当前模块可见性支持）。
- 模块回归：namespace 内 struct/class/template + 方法/静态/继承 + std.mem/std.vec。

### 8.2 负向

- 返回 `[&x]` 闭包 → “by-reference captures”。
- 返回 `[this]` 闭包 → “‘this’ capture”。
- 直接/间接返回引用闭包变量。
- `return;` 后再 `return 1;` → 非 void 诊断。
- 两次 return 类型不一致。
- `auto f(int32 x);` 无 body。
- 方法/模板/compile_time/compile_only + auto 返回。
- 源序：先调用后定义 auto 函数。
- 递归 auto 函数。

## 9. 验收

- 新增测试全绿；全量基线 1865 不回归。
- `examples/all_features.cmm` 输出 `ALL FEATURES OK`（可增加闭包返回小节）。
- README 的 “capturing-closure escape” 改为“按值逃逸已支持；引用/this 逃逸编译期拒绝”。
- `docs/spec/semantics.md` 更新。
- `TODO.md`/`Progress.md` 记录。
