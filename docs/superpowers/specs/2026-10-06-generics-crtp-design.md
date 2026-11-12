# Design: 泛型与 CRTP（P1-03 / GEN + INH-05）

> **Status**: Approved (design phase — pending spec review)
> **Date**: 2026-10-06
> **Scope**: GEN-01~09、PAR-17/21、LEX-08、TYP-18、SEM-06、CG-07、INH-05

## Overview

CDD 引入对齐 C++ 简单泛型的模板系统：函数模板、类模板、类型别名模板、非类型（整数）参数、显式与推导实例化、编译期单态化（零运行时开销）。并以此支撑 CRTP：`this` 表达式、模板基类、延迟实例化、向下转换。

**Non-goals（GEN-10，硬边界）**：特化/偏特化、SFINAE、可变参数模板、模板模板参数、concepts、默认模板实参、类模板实参推导（CTAD）。

## 语法（PAR-21 / GEN-01 / GEN-02）

### 声明

```
template<typename T> T max(T a, T b) { ... }          // 函数模板
template<typename T> struct Box { T value; };          // 类模板（struct/class 均可）
template<typename T, usize N> struct Array { ... };    // 非类型参数（仅整数）
template<typename T> using Vec = Array<T, 8>;          // 别名模板
```

- `template` / `typename` 为新关键字；`class T` 写法**不支持**（诊断提示改用 `typename`）。
- 模板可修饰：函数、struct/class、type 别名。union/enum 不做模板。
- 非类型参数类型：仅整数——`usize`/`isize`/各宽度 `intN`/`uintN`。实参为整型字面量或整型常量表达式。

### 使用

- 显式实例化：`max<int32>(3, 4)`、`Box<f64> b;`、`Vec<int32>* v;`。
- 推导：`max(3, 4)`——仅函数模板，从调用实参逐参数推导；类模板不推导。
- 模板体在定义处完整 parse 成 AST 存储，**不做 sema**；`T` 解析为 `TypeVarType` 占位。

### 歧义消解（LEX-08 / GEN-04）

- `<` 跟在类型名/模板名后且处于**类型位置**（声明、cast、参数）→ 按实参列表解析；表达式位置一律按比较运算。
- 实参列表内 `>>` 拆成两个 `>`（parser 按嵌套深度处理，lexer 不改）。
- 表达式里 `a < b > (c)` 类真歧义按比较处理（C++ 同款立场），报错时提示用显式实参 `max<int32>(...)` 消歧。

## 类型系统与实例化机制（TYP-18 / GEN-05）

### 新类型节点

- `TypeVarType { std::string name; }`（新 `TypeKind::TypeVar`）：模板体内 `T` 的占位；仅在模板体 AST 内合法，实例化后残留视为内部错误。
- 实例化产物全部是**既有类型节点**：函数模板实例 → 普通 `FunctionType`；类模板实例 → 新 `ClassType`（名字用 mangled 拼写）。不新增实例 Kind。

### 实例化键与缓存

- 类模板键：`名字 + <每个实参的规范类型拼写>`（复用 typeToString 规范化）；别名先展开再算键。
- 函数模板键：`名字 + <显式实参> + (推导后参数类型)`——推导出的 T 也编进键。
- 缓存：新 `TemplateRegistry`（sema 持有），`map<键, 状态>`；状态机 `NotInstantiated → Instantiating → Done`。`Instantiating` 再命中 = 递归实例化自身，报错（防 `struct Node { Box<Node> next; }` 值语义自引用；指针形式 `Box<Node*>` 合法）。
- 实例化深度上限 **64**，超过报错。
- 同实参同 `Type*`：替换经 TypeContext 缓存，保证指针同一性。

### 克隆替换器（主要新代码）

`TemplateInstantiator`：深克隆模板体 AST，重写：
1. 所有指向 `TypeVarType` 的 `Type*`（含嵌套于 `Pointer`/`Array`/`Optional`/`Result`）替换为实参 `Type*`；
2. 非类型参数出现处（数组长度等）替换为整型常量值。

替换后的实例 AST 与手写同构声明（`Box<i32>` ↔ `struct Box$i32 { i32 value; }`）完全等价，后续全走既有 sema/codegen。

### 实例化驱动与顺序

- **惰性**：sema 主遍历遇到模板**使用点**（类型位置或调用）才触发实例化。从未使用的模板零代码、零符号。
- 实例产生的声明当轮插入作用域并继续 sema；类模板实例在其成员方法体实例化前完成布局。
- **延迟方法体**：类模板实例的成员方法体延迟到**首次调用点**展开（GEN-09 延迟实例化要求——CRTP 场景彼时派生类已完整）。

### 别名模板

实例化即返回替换后的目标 `Type*`，不产生独立符号；键缓存防重复展开。

## sema 与诊断（GEN-03 / GEN-06 / GEN-08 / SEM-06）

### 定义处检查（从简）

不做涉及 `T`/`N` 的类型检查（GEN-06）；只查模板机制错误：参数名重复、非类型参数位置非法（如把 N 当类型用——延到实例点报亦可，取实现简单者）。

### 实例点检查

克隆体跑**全量普通 sema**——所有既有诊断在实例上原样生效（这就是 GEN-06 的实现）。诊断呈现：主 loc 在**使用点**，附 note「in instantiation of template 'Box' declared here」指向模板定义行。

### 推导规则（仅函数模板）

- 形参 `T` / `T*` 逐实参匹配：字面量按其类型（`3` → int32），变量按变量类型。
- 同一 T 推导出不同类型（`max(3, 0.5)`）→ 报错列出各候选，不隐式统一。
- 推导不出 → 报错要求显式实参。
- 显式实参与推导冲突：显式优先，推导跳过。

### 方法（GEN-08）

类模板实例的成员方法即普通方法：隐式 `this` 参数类型为实例 `ClassType*`（复用既有机制）；方法体惰性展开；调用点传 `this` 给 `实例名$method` 符号。

### 重载互动（SEM-16 最小规则）

模板实例与同名普通函数构成重载集时：非模板精确匹配优先，其次模板实例；都不精确匹配报歧义。不做模板重载（同名两个模板）。

### 与 Optional/Result 的关系

P1-02 固化的内建特判（伪字段、严格相等、布局）**原样保留**，泛型机制不接管；`Optional<T>` 语法与泛型共用 `<...>` 解析路径，类型层面仍走 `Optional` Kind。

## 符号修饰与 ABI（GEN-07）

- 类模板实例：`Box$i32`、嵌套 `Box$Box$i32`；实参拼写复用 typeToString，指针加 `P` 前缀（`Pi32`），保证单射。
- 函数模板实例：`max$i32`（命名空间/类限定沿用现有 flattening 前缀）。
- 非类型实参编入名字：`Array$i32$8`。
- 实例名对用户不可见；诊断与文档统一用源拼写 `Box<i32>`。

## codegen（CG-07）

无新机制：实例是普通 ClassType/FunctionType，布局、`getLLVMType`、方法符号、调用全走既有路径。唯一新增是**实例化触发钩子**：sema 在使用点完成实例化后，codegen 按实例缓存找到已检查的克隆 AST 逐实例生成（同一 LLVM module 内；去重由 sema 缓存保证）。

## CRTP 与 this（PAR-17 / INH-05）

### this 表达式

- 方法体内 `this` 为合法表达式，类型 = 当前类（或实例类）指针，rvalue。
- 可传参、可 `static_cast<D*>(this)`（PAR-18 既有转换；检查基类关系）。
- `this->field` 与 `this.field` 都支持。
- 非模板类中 `this` 同样可用（PAR-17 是通用特性）。

### CRTP 链路

```
template<typename D>
class Shape {
    f64 twice_area() { return static_cast<D*>(this)->area() * 2.0; }
    bool same_shape(D* other) { return static_cast<D*>(this)->area() == other->area(); }
};
class Circle : Shape<Circle> {
    f64 r;
    f64 area() { return 3.14159 * r * r; }
};
```

- 实例化 `Shape<Circle>` 时 `D = Circle`（Circle 允许不完整——基类方法体惰性，展开时已完整）。
- `static_cast<Circle*>(this)` 走既有向下转换（INH 轮已覆盖）。
- 方法调用静态绑定（既有 `Class_method` 直呼），无虚表。

## 测试面

- **parser 单测**：模板声明/使用语法、`>>` 拆分、歧义诊断。
- **sema 单测**：推导成功/冲突/失败、实例点类型错误双 loc、递归实例化拒绝、深度上限。
- **e2e**：函数模板（显式 + 推导）、类模板 `Box<T>`、非类型参数 `Array<T, N>`、别名模板、CRTP 全链路（上文用例原样 e2e）、未使用模板零符号（IR 查无该函数）。
- **硬门**：现有 952 项全绿。

## 影响面与风险

- Parser：`template`/`typename` 关键字 + 类型位置 `<...>` 判定——需与既有 `Optional<T>`/`Result<T,E>` 解析路径统一，防止回归。
- `typeToString`：需支持实例拼写（`Box<i32>`）与 `TypeVar`（模板体内诊断用）。
- 最大风险：AST 克隆替换器的完整性（嵌套指针/数组/聚合中的 TypeVar 替换）；以单测逐结构覆盖。
