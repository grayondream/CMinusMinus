# Cmm 1.0 — `compile_time` / `compile_only` 设计

> 状态：**已实施（2026-11-10）**
> 日期：2026-11-10
> 关联：`FUN-11`、`CT-06`、`CT-14`、`DEC-05`、`BASE-05`、`ACC-04`
> 本轮取代 `constexpr`；不保留兼容别名。

## 1. 意图

把现有的 `constexpr` 拆成两个职责清晰的修饰符：

| 修饰符 | 作用 | 运行期形态 |
|---|---|---|
| `compile_time` | 函数可在编译期求值，也可在运行期调用；变量必须是编译期常量，隐式只读 | 函数正常发射；变量物化为只读数据/常量 |
| `compile_only` | 函数**只**在编译期求值，不发射任何运行期符号 | 不存在 |

目标写法：

```cmm
struct DfaTable {
    int32 next[4][8];
    int32 initial;
};

compile_only DfaTable build_dfa(string_view pattern) {
    DfaTable t;
    // 普通控制流 + 定长数组/POD 字段
    for (usize s = 0; s < 4; s = s + 1) {
        for (usize c = 0; c < 8; c = c + 1) {
            t.next[s][c] = 0;
        }
    }
    t.initial = 0;
    // pattern.len()/char_at() 是编译期纯内建
    for (usize i = 0; i < pattern.len(); i = i + 1) {
        // ...
    }
    return t;
}

compile_time DfaTable EMAIL_DFA = build_dfa("[a-z]+@[a-z]+");
```

`const EMAIL_DFA` 不再承担强制编译期求值的职责；上面的 `compile_time` 变量是唯一推荐写法。

第二个核心能力：

```cmm
compile_time int32 table_at(int32 state, char c) {
    if compile_time {
        // 仅编译期路径；可以直接调用 compile_only
        return EMAIL_DFA.next[state][c - 'a'];
    } else {
        // 运行期路径；读取已物化的只读表
        return EMAIL_DFA.next[state][c - 'a'];
    }
}
```

`if compile_time` 是语句形式，语义等价于 C++ `if consteval` 的 Cmm 版本；现有的 `compile_time.if(cond)` 仍表示“按常量条件选择分支”，两者不合并。

## 2. 目标

1. 删除语言层 `constexpr`；函数/变量统一使用 `compile_time`。
2. 新增 `compile_only` 函数，运行期无符号、无地址、无函数指针、无闭包、无 vtable。
3. `compile_only` 支持定长数组/POD 聚合的返回值，足以实现 DfaTable。
4. `compile_time` 函数可在编译期求值，也可运行期调用。
5. `compile_time` 变量编译期求值、隐式只读、可物化为运行期只读数据。
6. `if compile_time { ... } else { ... }` 提供编译期/运行期双路径。
7. 诊断能指出“运行期路径 → `compile_time` 函数 → `compile_only` 函数”的调用链。
8. `compile_only` 求值有递归深度、步数和聚合元素数量上限。
9. 全量测试、示例和当前 spec 同步迁移。

## 3. 非目标

- 不保留 `constexpr` 别名；`constexpr` 是**移除关键字**，出现即定向报错。
- 不支持 `compile_only` 方法、模板函数、lambda、虚函数、函数指针、闭包捕获、结构体字段或 vtable 槽。
- `compile_only` 返回类型不允许指针、引用、函数指针、`string`、`string_view`、union、位域、柔性数组、带基类的 record。
- `compile_only` 体内不允许运行期函数调用、I/O、`volatile`、`atomic`、`asm`、可变全局、`thread_local`、运行期地址。
- 不支持在 parser 级数组维度/模板实参中直接写 `compile_only` 调用；先用命名 `compile_time` 常量承接返回值：

  ```cmm
  compile_time int32 DFA_STATES = build_dfa("...").initial;
  int32 table[DFA_STATES];
  ```

  这是 escape 位置的有意限制，不是聚合返回的限制。`compile_time.static_assert`、`compile_time.if` 条件和 `compile_time` 变量初始化中的直接调用都支持。
- 不改变运行期 struct/数组 ABI；`DfaTable` 的布局仍由现有 `LayoutBuilder` 决定。
- 不支持 `compile_time` 变量写回、取可变地址、作为 out 参数。
- 不把 `string` 作为 `compile_only` 返回类型；`string` 只作为编译期函数入参（见 §5.4）。
- 不重写历史设计文档；当前 `docs/spec/` 是权威规范。

## 4. 语法

### 4.1 声明修饰符

`compile_time` 和 `compile_only` 是**上下文关键字**，不是新增保留字：

```ebnf
declaration        = [ 'compile_time' ] , [ storage-class ] , decl-core
                   | 'compile_only' , function-decl ;
decl-core          = type-alias | record-decl | enum-decl | template-decl
                   | function-decl | variable-decl-list ;
```

- `compile_time` 可修饰函数和变量。
- `compile_only`只修饰自由函数；变量、参数、字段、方法、lambda 上出现时定向报错。
- `compile_time` 变量隐式只读；显式 `const` 可写但冗余：

  ```cmm
  compile_time DfaTable T = ...;
  compile_time const DfaTable T2 = ...;   // 允许，const 冗余
  ```

  推荐只写 `compile_time`。
- `compile_time` 变量必须有初始化器；不能是 `extern`/`thread_local`。
- `compile_only` 函数必须有定义体；只有声明的 `compile_only` 在分析结束时报错。
- `compile_only` 不能与 `extern` 组合；`static` 可接受但无运行期符号意义。
- `constexpr` 从语言删除。保留 `TOKEN_CONSTEXPR` 仅用于迁移诊断：

  ```text
  'constexpr' has been removed; use 'compile_time' for functions/variables,
  or 'compile_only' for functions that must not exist at runtime
  ```

  解析器消费该 token 并继续按 `compile_time` 恢复，避免级联错误；本次编译仍然失败。

### 4.2 `if compile_time`

```ebnf
compile-time-context-if = 'if' , 'compile_time' , block ,
                          [ 'else' , block ] ;
```

- 写法固定为 `if compile_time { ... } else { ... }`，没有括号形式。
- 两个分支都必须使用花括号；v1 不支持 `else if compile_time` 或单语句 `else`。
- 只能出现在 `compile_time` 或 `compile_only` 函数体内；普通运行期函数中使用报错。
- `else` 可省略。
- 不是表达式；不能写在 `?:`、`static_assert` 条件或初始化器里。
- `compile_time.if(cond)` 继续保留，语义不变。

### 4.3 与现有 `compile_time` 命名空间消歧

- 声明起点遇到标识符 `compile_time`：
  - 后面紧跟 `.` → 现有 `compile_time.if` / `compile_time.static_assert` / 查询；
  - 后面是类型/`const`/`volatile`/`auto` 等声明起点 → 声明修饰符。
- 已有同名变量/参数时仍按现有作用域规则优先；`compile_time` 作为普通标识符的兼容性不变。
- `compile_only` 没有现有命名空间冲突。

## 5. 语义

### 5.1 `compile_time` 函数

- 正常发射运行期函数，签名/重载/调用约定与普通函数一致。
- 在常量上下文（`compile_time` 变量初始化、`static_assert`、`compile_time.if` 条件、`if compile_time` 的编译期分支、另一个 `compile_only` 体内）由编译期求值器执行。
- 求值器只支持纯子集；遇到 I/O、运行期调用、未支持的操作时，该次编译期求值报错，**不会**静默改成运行期调用。
- 运行期调用始终走正常函数体；`compile_time` 不改变运行期 ABI。
- `compile_time` 函数内直接调用 `compile_only` 且没有包在 `if compile_time` 中 → **编译错误**（已确认）。必须写成：

  ```cmm
  compile_time Ret f(...) {
      if compile_time {
          return build_dfa(...);   // 仅编译期
      } else {
          return runtime_fallback(...);
      }
  }
  ```

- 允许 `compile_time` 函数体内出现运行期操作；这些操作只限制该函数能否被编译期求值，不影响运行期调用。
- 允许重载；CT 求值按已解析签名选择具体函数。
- 签名约束：为保证 `compile_time` 不是空承诺，v1 对声明做与现有 `constexpr` 相同的 CT 签名检查，并扩展到聚合：
  - 返回：`void` 或 CT 值类型（标量、enum、固定数组、POD 聚合、现有四类容器）；
  - 参数：CT 值类型，或指向 CT 值类型的指针（仅作为 out 参数）；
  - `string` 不允许（拥有所有权）；`string_view` 可作为只读输入；
  - 不满足时在声明处报错，而不是等调用点才失败。
  - 只有运行期用途的函数不需要 `compile_time`，继续用普通函数声明。

### 5.2 `compile_time` 变量

- 必须有初始化器，初始化器必须是编译期常量表达式。
- 隐式只读：赋值、复合赋值、`++`/`--`、作为可变 out 参数、取可变地址均诊断拒绝。
- 只读性是**变量/符号属性**，不是修改共享 `Type::isConst` 的副作用；避免 `DfaTable` 类型被整个程序误标成 const。
- 全局/namespace/static 局部：物化为只读数据或 LLVM 常量。
  - 标量/浮点/字符：常量折叠；需要地址时仍发射只读存储。
  - 固定数组/POD 聚合：发射私有只读常量（必要时递归生成数组常量）。
  - 现有四类 `std` 容器：沿用现有容器物化路径。
- 块作用域局部：求值后存为常量 alloca；运行期读取该局部时读常量。
- 局部 `compile_time` 变量在**声明点**由 sema 折叠。因此 v1 中其初始化器不能依赖函数参数；需要按调用参数构造的 CT 值，应使用普通局部 + `compile_only` 函数返回聚合，而不是在函数体里声明依赖参数的 `compile_time` 局部。
- 不允许类型：拥有所有权的 `string`、指针/引用、`volatile`、`atomic`、union、位域、柔性数组。`string_view` 只允许作为容器元素或 `compile_only` 入参，不作为顶层 `compile_time` 变量的逃逸数据。
- 地址逃逸：`&T` 只能得到只读地址；若绑定到可变指针/传递给可变形参，按现有 const 规则拒绝。

### 5.3 `compile_only` 函数：调用位置

`compile_only` 调用只允许出现在编译期上下文：

| 上下文 | 允许 | 说明 |
|---|---|---|
| `compile_time` 变量初始化器 | 是 | 直接调用；求值器折叠 |
| `compile_only` 函数体 | 是 | 整个函数只在编译期执行 |
| `if compile_time` 的 then 分支 | 是 | then 是编译期专用路径 |
| `compile_time.static_assert` 条件 | 是 | sema 求值 |
| `compile_time.if(cond)` 条件 | 是 | sema 求值 |
| `if compile_time` 的 else 分支 | 否 | else 是运行期路径 |
| 普通运行期函数体 | 否 | 直接报错 |
| `compile_time` 函数体、且不在 `if compile_time` 内 | 否 | 已确认报错 |
| `const` 全局/局部初始化器 | 否 | `const` 不是编译期上下文；用 `compile_time` |
| parser 级数组维度/模板实参 | 否（直接调用） | 用命名 `compile_time` 常量间接逃逸 |

调用链要求：

- 从运行期根（`main`、导出函数、取地址函数、运行期全局初始化器）出发，任何可达路径都不得包含 `compile_only` 调用。
- `compile_time` 函数是双形态：运行期版本必须存在。因此其函数体内直接调用 `compile_only` 一律报错，即使它当前只被编译期调用。
- `if compile_time` 的 then 分支不进入运行期版本，所以其中的 `compile_only` 调用是安全的。

### 5.4 `compile_only` 函数：签名与返回

允许的编译期值类型：

```text
ct-scalar      = 整数 | 浮点 | bool | char | enum
ct-input       = ct-scalar | string | string_view
ct-return      = void | ct-scalar | fixed-array(ct-return) | pod-record(ct-return)
pod-record     = 无基类、无虚函数、无指针/引用/函数指针/string/
                 位域/柔性数组/union 字段的 struct/class
```

- `compile_only` 参数：`ct-input`、固定数组或上述 POD 聚合；不允许指针/引用参数。
- `compile_only` 返回：`void` 或 `ct-return`；不允许指针、引用、`string`、`string_view`。
- `DfaTable` 属于 `pod-record`；其 `int32 next[4][8]` 是嵌套固定数组，允许返回。
- `string_view` 入参作为编译期不可变字符串值；字符串字面量可直接绑定。 owning `string` 入参在 v1 的调用重载中仍不接受（README/spec 标注为限制）。不会为 `compile_only` 创建运行期字符串对象。
- 参数是编译期常量：调用点由求值器检查；无法求值的实参报错。
- 返回聚合按值传递；在编译期是深拷贝，运行期不存在该函数。
- `compile_only` 可以重载（自由函数）；重载选择用现有 sema 结果。
- `compile_only` 不能取地址、转函数指针、作为值传递、放进 struct 字段、作为闭包捕获、作为 vtable 槽。
- v1 不支持 `compile_only` 方法/模板/lambda；出现时定向报错。

### 5.5 `compile_only` 函数体限制

允许：

- 局部变量、参数、block/if/while/do/for/break/continue/return。
- 算术、位、比较、逻辑、三元、显式转换。
- 定长数组下标、POD 聚合字段访问/赋值、嵌套聚合路径。
- 调用 `compile_time` / `compile_only` 函数。
- 纯编译期内建：`string`/`string_view` 的 `len()`、`char_at()`，以及 `compile_time.*` 纯查询。
- `compile_time.fail`、`compile_time.assert_*` 等编译期诊断。
- `&local` 仅作为 `compile_time`/`compile_only` 调用的 out 参数；不允许保存、返回或转换成运行期指针。

禁止：

- 任何运行期用户函数调用（包括运行期 `operator` 重载）。
- `print`/`println`、`assert`/`panic`、I/O、文件/网络/系统访问。
- `volatile`、`atomic`、`asm`、`defer`。
- 可变全局/namespace 变量、`static` 局部、`thread_local`。
- 运行期地址、指针算术、指针返回、函数指针。
- `compile_time.repeat`（v1 在 `compile_only` 内拒绝；用普通 `for`）。
- 动态分配、`string.new`、容器运行期方法（除非未来显式标记为 `compile_time` 方法）。

读取 `compile_time` 变量、枚举常量、局部/参数是允许的；它们不构成运行期状态。

### 5.6 求值资源上限

- 递归深度：沿用 `kConstexprMaxDepth = 128`。
- 总步数：沿用 `kConstexprStepLimit = 1,000,000`。
- 聚合元素：单次求值累计最多 1,000,000 个标量元素；超过报错。
- 超限诊断包含函数名、调用链和限制类别（depth/steps/elements）。
- 求值失败不会退化成运行期调用；`compile_only` 不存在运行期后备。

### 5.7 `if compile_time`

| 所在函数 | 编译期求值 | 运行期 codegen | sema 分析 |
|---|---|---|---|
| `compile_time` 函数 | 执行 then | 只发射 else | then + else 都分析 |
| `compile_only` 函数 | 执行 then | 函数整体不发射 | 只分析 then（else 死分支） |
| 普通运行期函数 | 不允许 | 不允许 | 报错 |

- then 分支是编译期专用上下文：其中的 `compile_only` 调用合法。
- else 分支是运行期上下文：其中的 `compile_only` 调用按 §5.3 报错。
- then/else 是独立 block 作用域；两边声明不互相泄漏。
- 两个分支都可以 `return`；运行期版本只使用 else 的返回，编译期版本只使用 then 的返回。
- `compile_time` 函数运行期发射时，`if compile_time` 节点等价于 `else`；没有 else 时等价于空语句。
- `compile_only` 函数没有运行期发射，所以其 `if compile_time` 的 else 永远不会执行；sema 不分析死分支。
- v1 不允许在 `if compile_time` 的 then/else 分支内声明新的 `struct`/`class`/`union`/`enum` 类型；只允许语句和变量声明。这样两个分支的类型注册不会互相污染；类型定义放在函数外或普通 `compile_time.if` 中。
- 现有 `compile_time.if(cond)` 仍由常量条件选择，和本节点共享 AST/诊断风格但不共享语义。

### 5.8 调用链与诊断

语义分析记录函数调用边：

- 边来源：`CallExprAST`、间接调用、运算符重载调用。
- 边标记：runtime / compile-time-only / compile-only。
- 运行期根：`main`、导出函数、取地址函数、运行期全局初始化器。
- 结束 sema 后从运行期根做可达性搜索；`compile_only` 调用若落在运行期可达路径上，报完整链。

示例诊断：

```text
compile_only function 'build_dfa' is not available at runtime
  runtime path: main -> make_table -> build_dfa
  hint: move the call into a 'compile_time' variable initializer,
        or guard it with 'if compile_time { ... } else { ... }'
```

```text
compile_only function 'build_dfa' cannot be called from compile_time
function 'make_table' outside 'if compile_time'
  path: main -> make_table -> build_dfa
```

其他定向诊断：

- `cannot take address of compile_only function 'build_dfa'`
- `compile_only function 'build_dfa' cannot be used as a value`
- `compile_only function 'build_dfa' cannot be a method/template/lambda`
- `compile_only function 'build_dfa' must return a fixed-size, trivially copyable value without pointers, references, or owning strings`
- `compile_only function 'build_dfa' cannot call runtime function 'log'`
- `compile_only function 'build_dfa' cannot read mutable global 'state'`
- `compile_only function 'build_dfa' cannot use volatile/atomic/asm`
- `compile_time variable 'N' is read-only`
- `compile_time evaluation limit exceeded in 'build_dfa' (steps)`
- `'constexpr' has been removed; ...`

## 6. 编译期聚合值模型

### 6.1 `CtValue` 扩展

在 `src/ast/CtValue.h` 增加：

```cpp
struct CtAggregate;

struct CtValue {
    enum Type {
        INT, DOUBLE, CHAR, STR, TYPE, FUNC, CONTAINER, PTR,
        AGGREGATE,
    };
    // ...
    std::shared_ptr<CtAggregate> aggregate; // AGGREGATE
};

struct CtAggregate {
    ::Type* type = nullptr;            // ArrayType / StructType / ClassType
    std::vector<CtValue> elements;     // 数组行优先；record 按字段顺序
    std::size_t totalElements = 0;     // 预算统计（递归累计）
};
```

约束：

- 数组：`elements.size()` 等于固定维度元素数；多维数组递归为数组的数组。
- record：`elements` 与 `StructType::fields` / `ClassType::fields` 一一对应；v1 不允许基类槽、位域、柔性数组。
- 不存填充字节；LLVM 常量生成时由 `getLLVMType`/`LayoutBuilder` 处理。
- `ctClone` 深拷贝聚合；`ctDefaultValue` 递归零初始化。
- `ctBindValue` 对聚合做类型匹配和逐字段标量转换；数组长度不匹配报错。
- `ctValueToConstant` 递归生成 `llvm::ConstantArray` / `llvm::ConstantStruct`。
- 聚合读取是值语义（克隆）；只有 lvalue 路径（变量/字段/下标）允许原地写。

### 6.2 求值器新增能力

`CompileTimeEvaluator` 和 `SemanticAnalyzer` 的 CT 解释器需要支持：

- `InitializerListExprAST` → 聚合/数组常量，缺省元素零填充。
- `MemberAccessExprAST` → record 字段读取；`array.len` 如现有语法支持则返回长度。
- `ArrayAccessExprAST` → 数组下标读取，越界报错。
- `AssignmentExprAST` / 复合赋值 / `++`/`--` → 变量、字段、下标的 lvalue 路径写回。
- 聚合局部声明、聚合参数绑定、聚合返回、聚合深拷贝。
- `string`/`string_view` 的 `len()` / `char_at()` 纯内建。
- `compile_time` / `compile_only` 调用在 CT 环境中的解析；聚合实参/返回值。
- `if compile_time` 在 CT 解释器中执行 then。
- 现有 step/depth/element 预算检查。

lvalue 解析器返回“变量名 + 聚合路径”，不返回裸运行期指针；实现上可以用：

```cpp
struct CtRef {
    CtValue* slot = nullptr;             // env 槽（小心 rehash 后失效）
    std::shared_ptr<CtAggregate> root;   // 聚合根
    std::vector<std::size_t> path;       // 字段/数组下标的索引路径
};
```

具体实现应避免持有 `unordered_map` 迭代器；使用变量名查找，或先稳定根再走路径。

### 6.3 物化

`VarDeclAST` 增加：

```cpp
bool isCompileTime = false;
std::shared_ptr<CtAggregate> foldedAggregate; // 聚合常量
```

`codegen` 分支：

- 全局：`isCompileTime && foldedAggregate` → `ctValueToConstant`，
  发射 `private` / `constant` 全局；`isCompileTime` 变量本身只读。
- 局部：`alloca` + `store` 常量；需要地址时仍可取得只读地址。
- static 局部：模块级常量 + 绑定局部名。
- 现有 `foldedValue`、`foldedContainer` 路径保留，三者互斥。

`compile_only` 函数：

- `FunctionDeclAST::isCompileOnly = true`。
- `codegenPrototype` 和 `codegen` 在入口直接返回 `nullptr`，不创建 `llvm::Function`（连 `declare` 也不留）。
- TranslationUnit 预扫、方法表、模板实例化路径都必须跳过它。
- 对它的 CT 调用只走 AST 求值器，不引用 LLVM 符号。

## 7. 只读性与 `const` 的关系

- `const` = 运行期只读，不强制编译期求值。
- `compile_time` = 编译期常量 + 隐式只读。
- `compile_time const` 允许但 `const` 冗余。
- `const` 全局初始化器不是编译期上下文；不能直接调用 `compile_only`。
- `compile_time` 变量的只读性记录在 `VarDeclAST` / `Symbol`（新增 `isCompileTime` 标记），
  不通过修改共享 `Type::isConst` 实现。
- 普通 `const` 变量、可变指针、out 参数等既有检查继续按原规则工作；
  新增 `compile_time` 分支到赋值/取地址/传参检查。

## 8. 迁移

### 8.1 语言层

- `src/frontend/Lexer.cpp`：保留 `TOKEN_CONSTEXPR` 仅用于迁移诊断；不新增 `compile_time`/`compile_only` 保留字。
- `src/frontend/Parser.cpp/.h`：
  - 移除 `constexpr` 解析路径；加迁移诊断。
  - 加 `compile_time` 修饰符识别（函数/变量）。
  - 加 `compile_only` 自由函数识别。
  - 加 `if compile_time` 语句节点。
- `src/ast/Decl.h/.cpp` / `Stmt.h` / `Expr.h`：
  - `isConstexpr` → `isCompileTime`；
  - 新增 `isCompileOnly`、`foldedAggregate`、`CompileTimeContextIfStmtAST`。
- `src/sema/SemanticAnalyzer.*`：
  - `constexprValues` → `compileTimeValues`；
  - `constexprFunctions` → `compileTimeFunctions`；
  - 新增 `compileOnlyFunctions`、CT 调用上下文、调用图、聚合求值、资源预算。
- `src/sema/CompileTimeEvaluator.*`：
  - 聚合表达式/语句/调用支持；
  - `if compile_time` 的 CT 分支。
- `src/codegen`/`src/ast/Decl.cpp`：聚合物化、`compile_only` 无符号、`if compile_time` 只出 else。
- `src/support/Utils.cpp`：`constexpr` 的 token 名称保留用于诊断。

### 8.2 标准库/测试/示例

把语言层 `constexpr` 全部迁移为 `compile_time`：

- `libs/std/core.cmm`：`min`/`max`；
- `libs/std/math.cmm`：`PI`/`E`；
- `libs/std/thread.cmm`：`SYS_NPROCESSORS_ONLN`；
- `libs/std/time.cmm`：`CLOCKS_PER_SEC`；
- `tests/e2e/test_const_constexpr.cpp`（可改名/改内容）；
- `tests/e2e/test_constexpr_function.cpp`；
- `tests/e2e/test_constexpr_containers.cpp`；
- `tests/e2e/test_compile_time.cpp`；
- `tests/frontend/test_parser.cpp` 的 `constexpr` 词法/解析测试改为迁移诊断 + `compile_time` 正向测试；
- `tests/sema/test_semantic_analyzer.cpp` 对应诊断；
- `examples/all_features.cmm` 增加 DfaTable/`compile_only`/`if compile_time` 节。

C++ 内部的 `static constexpr` 常量不受影响。历史 `docs/superpowers/` 设计文档不重写；当前 `docs/spec/` 更新。

### 8.3 当前 spec 文档

- `docs/spec/compile_time.md`：新增 `compile_time` 函数/变量、`compile_only`、`if compile_time`、聚合逃逸；把 DEC-05 标为 superseded。
- `docs/spec/grammar.ebnf`：声明修饰符、`if compile_time`、`compile_only`。
- `docs/spec/keywords.md`：`constexpr` → `[removed]`；`compile_time`/`compile_only` 上下文关键字。
- `docs/spec/semantics.md`：只读性、CT 上下文、无运行期符号。
- `docs/spec/stdlib.md`：标准库 `constexpr` → `compile_time`。
- `TODO.md`：FUN-11/CT-14/DEC-05 更新，新增 CT-23 ~ CT-27。
- `Progress.md`：实现记录。

建议新增 TODO 编号：

| 编号 | 内容 |
|---|---|
| CT-23 | `compile_time` 函数/变量修饰符替代 `constexpr` |
| CT-24 | `compile_only` 自由函数与无运行期符号 |
| CT-25 | 编译期定长数组/POD 聚合值与物化 |
| CT-26 | `if compile_time` 双路径 |
| CT-27 | `constexpr` 迁移与全库清理 |

## 9. 测试计划

### 9.1 Parser

- `compile_time int32 f()` / `compile_time int32 N = 1;` 正向。
- `compile_only int32 f()` 正向。
- `compile_only` 变量/参数/字段/方法/lambda 负向。
- `constexpr` 迁移诊断，且不级联。
- `compile_time.if(...)` 与 `if compile_time { ... } else { ... }` 共存。
- `if compile_time` 在普通函数中负向。
- `compile_time` 作为普通标识符/命名空间仍可用。

### 9.2 Sema

- `compile_time` 变量无初始化器/非常量初始化器报错。
- `compile_time` 变量赋值、`++`、out 参数、可变指针报错。
- `compile_time` 函数直接调用 `compile_only`（不在 `if compile_time` 内）报错。
- `if compile_time` then 内调用 `compile_only` 通过；else 内报错。
- 普通函数调用 `compile_only` 报错，含 `main -> f -> compile_only` 链。
- `compile_only` 体内调用运行期函数报错。
- `compile_only` 体内读可变全局/`volatile`/`atomic`/`asm` 报错。
- `compile_only` 返回指针/`string`/union/位域/柔性数组报错。
- 取 `compile_only` 地址、函数指针、值传递、字段、闭包捕获报错。
- 资源上限（depth/steps/elements）诊断。

### 9.3 求值器/聚合

- 零初始化局部 `DfaTable t;`。
- 嵌套定长数组字段读/写：`t.next[s][c] = ...`。
- 聚合参数/返回/深拷贝。
- `string`/`string_view` 入参的 `len()`/`char_at()`。
- `compile_only` 调用 `compile_time` 辅助函数。
- 多返回值场景：`build_dfa(...).initial`。
- 数组越界、字段不存在、聚合类型不匹配诊断。
- 元素预算超限。

### 9.4 E2E

1. `compile_time` factorial 运行期调用 + 编译期变量。
2. `compile_time` 变量只读。
3. `DfaTable` 例：`compile_only build_dfa`、`compile_time EMAIL_DFA`、运行期 `matches` 读取。
4. IR 断言：`Module::getFunction("build_dfa") == nullptr`；无 `define`/`declare`。
5. `compile_time` 函数在 `if compile_time` 中调用 `compile_only`；运行期调用走 else。
6. 普通函数调用 `compile_only` 的完整链路诊断。
7. `compile_only` 函数体调用运行期函数诊断。
8. `constexpr` 迁移诊断。
9. `examples/all_features.cmm` 输出 `ALL FEATURES OK`。
10. 全量 `ctest --test-dir build -j8` 通过。

### 9.5 验收

- 全量测试（基线 1829）全部通过，新增测试全绿。
- `examples/all_features.cmm` 编译成功并输出 `ALL FEATURES OK`。
- 仓库内语言层 `constexpr` 清零（C++ `static constexpr` 除外）。
- `DfaTable` 示例的 IR 中不存在 `build_dfa` 符号。
- 诊断示例覆盖 §5.8 的链路/hint。
- 当前 spec/grammar/keywords/semantics/stdlib 同步。

## 10. 实现边界与顺序

### 10.1 涉及文件

| 层 | 文件 |
|---|---|
| Lexer/Parser | `src/frontend/Lexer.cpp`、`Token.h`、`Parser.cpp/.h`、`Utils.cpp` |
| AST | `src/ast/Decl.h/.cpp`、`Stmt.h`、`Expr.h`、`CtValue.h/.cpp` |
| Sema | `src/sema/SemanticAnalyzer.h/.cpp`、`CompileTimeEvaluator.h/.cpp` |
| Codegen | `src/ast/Decl.cpp`、`src/codegen/CodegenContext.*` |
| Stdlib | `libs/std/core.cmm`、`math.cmm`、`thread.cmm`、`time.cmm` |
| Tests | `tests/frontend`、`tests/sema`、`tests/e2e`、`tests/ast`、`tests/codegen`（IR 断言） |
| Docs | `docs/spec/*`、`TODO.md`、`Progress.md`、`examples/all_features.cmm` |

### 10.2 推荐阶段

1. **纯改名迁移**：`constexpr` → `compile_time`，AST/sema 字段重命名，stdlib/tests/docs 迁移；行为保持不变，全量绿。
2. **`compile_time` 变量只读/符号标记**：不修改共享 Type；补只读诊断。
3. **`compile_only` 声明/无符号/调用上下文/诊断**：先支持标量/容器，再接入聚合。
4. **CT 聚合模型**：`CtValue`/`CtAggregate`、求值器 lvalue/成员/下标、物化、DfaTable e2e。
5. **`if compile_time`**：AST/parser/sema/codegen/CT 解释器。
6. **调用链诊断/资源上限/迁移收尾**：IR 无符号断言、全量、示例、文档。

聚合支持是最大块，必须在阶段 3 前完成最小值模型，避免 `compile_only` 先落地后返工签名限制。

### 10.3 当前代码的可行性检查

已对照当前实现做 builder 可行性检查：

- `CtValue` 目前只有 `INT/DOUBLE/CHAR/STR/TYPE/FUNC/CONTAINER/PTR`，没有聚合；`CtValue.cpp` 的 `ctDefaultValue` 只处理标量和容器。
- `CompileTimeEvaluator::eval` 目前不处理 `InitializerListExprAST`、`MemberAccessExprAST`（非 `compile_time` 根）、`ArrayAccessExprAST`、聚合赋值路径。
- `SemanticAnalyzer` 的 `evalConstexprStmt` 支持局部标量/容器声明，但 `ctDefaultValue` 对聚合会落回整数 0。
- `VarDeclAST` 只有 `foldedValue`/`foldedContainer`；`Decl.cpp` 的物化只处理标量和容器。
- `FunctionDeclAST::codegen` 无 `compile_only` 跳过；需要同时改 `codegenPrototype` 与 TU 预扫。
- `constexpr` 是真实 token；`compile_time` 是非关键字命名空间；`if compile_time` 不存在。
- `constexprFunctions`/`constexprValues`/`isConstexpr` 的改名是机械迁移，但 `parseVariableDecl` 对非标量 `constexpr` 直接改共享 `Type::isConst`，必须替换为符号/声明标记，否则 `DfaTable` 类型会被全局污染。

结论：**可行**，但需要按 §10.2 分阶段；聚合 lvalue 与 LLVM 常量物化是主要风险点，必须用 DfaTable e2e 和 IR 断言锁定。

## 11. 已冻结的决策

1. `constexpr` 删除，不保留别名。
2. `compile_time` 变量隐式只读；`const` 不再强制 CT。
3. `compile_time` 函数直接调用 `compile_only`（不在 `if compile_time` 内）报错。
4. `compile_only` 第一版支持定长数组/POD 聚合返回值（DfaTable）。
5. `if compile_time` 只允许在 `compile_time`/`compile_only` 函数内。
6. `compile_only` v1 只支持自由函数；方法/模板/lambda/vtable/函数指针不支持。
7. `compile_only` 返回禁止指针/引用/`string`/`string_view`/union/位域/柔性数组/基类。
8. `compile_only` 入参允许 `string`/`string_view` 作为编译期不可变字符串。
9. parser 级数组维度/模板实参只接受命名 `compile_time` 常量，不直接接受 `compile_only` 调用；`static_assert`/`compile_time.if`/变量初始化支持直接调用。
10. 调用链诊断覆盖运行期根可达路径。

## 12. 待实现时确认的细节

- `compile_only` 在导入模块中的可见性：v1 要求定义体在当前编译可见；只有声明没有定义报错。若模块加载已提供 AST 体，则可跨模块；否则 v1 诊断。
- `compile_only` 重载 CT 求值的签名选择：复用 `CallExprAST::resolvedParamTypes`/`mangleFunction`；若未解析则回退裸名。
- 聚合初始化列表的 designated initializer 支持：优先复用 `resolvedTargets`；若实现成本高，v1 只支持位置式 + 零填充，并在 spec 标注。
- `compile_time` 变量作为 `const` 指针目标：使用现有 const 检查的同一套 helper，避免两套规则分叉。
- 局部 `compile_time` 变量在 CT 函数体内依赖参数时，是继续按声明点折叠（v1 限制）还是延迟到调用环境求值（更理想但需要新的“待求值常量”状态）；本设计按前者实现，文档明确限制。
- 调用图对模板实例/方法调用的边：v1 先覆盖自由函数直接调用；间接调用涉及 `compile_only` 时已直接禁止，不影响安全性。
