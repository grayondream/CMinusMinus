# Cmm 1.0 — 编译期与反射规范（`compile_time`）

> 对应 TODO：**CT-01 ~ CT-14**、**PAR-15**、**SEM-06/07**、**DEC-05**
> 状态：`[impl]` 以 `src/sema/SemanticAnalyzer.cpp`、`src/frontend/Parser.cpp` 为准；`[plan]` 为目标。
> 原则：**零运行时开销**（ACC-04）；求值在编译期完成，结果以 LLVM 常量落地（CT-12）。**取代** `type_info(T)` 与独立 `static_assert` 关键字（NG-12）。

## 1. 语法入口（CT-01）

- 统一命名空间标识符 `compile_time`，成员用 `.` 访问：`compile_time.static_assert(...)`、`compile_time.if (...)`。
- 旧名 `comptime` 已移除（DEC-18，token 不再存在）。
- `compile_time` **不是保留关键字**：作为声明修饰符或命名空间时按上下文识别（见 keywords.md §9）。
- `compile_time` 修饰函数/变量；`compile_only` 修饰仅编译期函数（见 §10）。
- `constexpr` 已从语言删除；出现时给定向迁移诊断。

## 2. 静态断言（CT-02）

```cmm
compile_time.static_assert(size_of(Point) == 8, "Point must be 8 bytes");
```

- 条件必须是编译期常量布尔值；失败诊断携带**源码位置**与可选消息。
- 语法见 `grammar.ebnf` §8。

## 3. 条件编译（CT-03 / SEM-07）

```cmm
compile_time.if (compile_time.target.os == "linux") {
    // 仅该目标参与语义分析与 codegen
} else {
    ...
}
```

- 条件在语义分析阶段求值；未选分支**不参与**类型检查与代码生成（死代码消除）。
- 顶层形式分支内是声明序列；函数体内另有语句形式 `CompileTimeIfStmtAST`，两个
  分支都是完整块，sema 只分析选中块、codegen 只出选中块。
- 与 `#if` 的区别：无预处理器、作用域/语法完整、仍受语法解析。

## 4. 目标查询（CT-04）

| 成员 | 类型 | 取值示例 |
|---|---|---|
| `compile_time.target.os` | `string_view` | `"linux"`, `"macos"`, `"windows"` |
| `compile_time.target.arch` | `string_view` | `"x86_64"`, `"aarch64"` |
| `compile_time.target.cpu` | `string_view` | CPU 名（来自目标三元组/特性） |
| `compile_time.target.endian` | `string_view` | `"little"` / `"big"` |
| `compile_time.target.pointer_width` | `usize` | 指针位宽（当前 host 64） |
| `compile_time.target.triple` | `string_view` | 默认目标三元组 |
| `compile_time.target.is("linux")` | `bool` | OS 比较的便捷形式 |
| `compile_time.target.has_feature("avx2")` | `bool` | host CPU 特性查询 |

- 取值来自 `-target` 三元组（CG-16）/ host 默认三元组，保证跨平台可预测。

## 5. 构建查询（CT-05）

| 成员 | 类型 | 说明 |
|---|---|---|
| `compile_time.build.debug` | `bool` | Debug 构建 |
| `compile_time.build.optimize` | `string_view` | `"O0".."O3"/"Os"/"Oz"` |
| `compile_time.build.mode` | `string_view` | `"debug"` / `"release"` |
| `compile_time.build.compiler` | `string_view` | `"cmm"` |
| `compile_time.build.host_os` | `string_view` | host OS |
| `compile_time.build.host_arch` | `string_view` | host 架构 |
| `compile_time.build.version` | `string_view` | 编译器版本（无 edition，见 NG-06） |
| `compile_time.version.major/minor/patch` | `usize` | 语义化版本分量 |
| `compile_time.version.at_least("x.y.z")` | `bool` | 版本下限判断 |

## 6. 编译期求值器（CT-06）

- 解释 AST 常量子集，支持：整数/浮点/布尔/字符串/字符字面量；算术、位、比较、逻辑、三元；常量数组/结构体成员访问；字符串比较/拼接；`size_of`/`align_of`/`offset_of`。
- 不得产生副作用；禁止运行时调用（除非标注为 `compile_time` 函数，见 §10）。
- 递归深度、迭代次数设上限并报诊断（防止编译期爆栈）。
- 常量折叠（`compile_time`）复用同一求值器（BASE-05）。

## 7. 反射 API（CT-07 / CT-13）

已实现（2026-10-19）：

| API | 返回 | 说明 |
|---|---|---|
| `compile_time.name_of(T)` | `string_view` | 类型名（`typeToString`） |
| `compile_time.size_of(T)` / `size_of(expr)` | `usize` | 分配大小；表达式形式按表达式类型 |
| `compile_time.align_of(T)` / `align_of(expr)` | `usize` | ABI 对齐；表达式形式按表达式类型 |
| `compile_time.bit_size_of(T)` | `usize` | 位大小 |
| `compile_time.padding_of(T)` | `usize` | 聚合填充字节数（含尾填充） |
| `compile_time.is_packed(T)` | `bool` | `[[packed]]` 查询 |
| `compile_time.offset_of(T, "f")` / `offset_of(T, f)` / `offset_of(T, "a.b")` | `usize` | 字段/嵌套字段偏移；位域拒绝 |
| `compile_time.fields_of(T)` | `Slice<FieldInfo>` | 运行时字段表（仅聚合类型） |
| `compile_time.attributes_of(T)` | `Slice<Attribute>` | 运行时注解读表（当前含 deprecated 标记） |

- `FieldInfo` 内建类型：`{ string_view name; usize offset; usize size; }`。
- `Attribute` 内建类型：`{ string_view name; }`。
- `fields_of` 包含继承字段（基类字段在前）和位域；位域的 `offset` 当前为 0，
  `size` 为声明类型的存储大小。需要位域精确信息时用 `field_is_bitfield`/
  `field_bit_width_of`；`field_offset_of` 对位域拒绝。
- 反射表经 sema→codegen 注册表物化为全局常量数组 + slice 视图（运行时生命周期 = 程序全程）。
- 取代 `type_info(T)`（NG-12）；类型/ABI 细节见 [`abi.md`](./abi.md)。

### 7.1 编译期标量查询

下列 API **只返回编译期常量**，不生成运行时调用、不生成全局表：

| 类别 | API |
|---|---|
| 类型谓词 | `is_same(A,B)`、`is_integer`、`is_float`、`is_bool`、`is_pointer`、`is_array`、`is_aggregate`、`is_class`、`is_struct`、`is_union`、`is_enum`、`is_void`、`is_signed` |
| 数值极限 | `int_min(T)`、`int_max(T)`、`uint_max(T)`、`bit_width(T)`、`float_epsilon(T)`、`float_max(T)`、`float_digits(T)` |
| 类型名 | `type_name_of(expr)`、`type_kind_of(expr)`（kind 为 `integer`/`float`/`pointer`/`struct`/...） |
| 布局 | `layout_size_of(T)`、`layout_align_of(T)` |
| 字段 | `has_field(T,"f")`、`field_count_of(T)`、`field_name_at(T,i)`、`field_type_name_of(T,"f")`、`field_size_of(T,"f")`、`field_offset_of(T,"f")`、`field_is_bitfield(T,"f")`、`field_bit_width_of(T,"f")` |
| 字段注解 | `field_annotation_count_of(T,"f")`、`field_annotation_name_at(T,"f",i)`、`field_annotation_arg_of(T,"f","name",i)` |
| 类型注解 | `has_attribute(T,"name")`、`attribute_count_of(T)`、`attribute_name_at(T,i)`、`attribute_arg_of(T,"name",i)` |
| 枚举 | `enum_count_of(E)`、`enum_name_at(E,i)`、`enum_value_at(E,i)`、`enum_value_of(E,"name")`、`enum_name_of(E,value)`、`enum_underlying_name(E)` |
| 类/继承 | `method_count_of(C)`、`has_method(C,"f")`、`method_name_at(C,i)`、`method_param_count(C,"f")`、`method_param_type_name(C,"f",i)`、`method_return_type_name(C,"f")`、`base_name_of(C)`、`is_base_of(Base,Derived)` |
| 源码位置 | `file`、`line`、`column`、`function` |

- 字段查询基于 `semanticFields`，位域不会在布局物化后丢失。
- `field_offset_of` 对位域发出 `cannot take offset of a bit-field`；
  `field_size_of` 对位域返回声明类型的存储大小（位宽用 `field_bit_width_of`）。
- `uint_max(uint64)` 以 `uint64` 位模式返回；`int128/uint128` 极限超出当前 `ConstValue` 64 位表示，诊断拒绝。
- `method_*` 继承链查找；静态方法不在 `ClassType::methods` 中，因此不参与这些查询。

### 7.2 诊断、断言与格式化

| API | 说明 |
|---|---|
| `compile_time.fail(msg)` | 无条件编译错误 |
| `compile_time.warn(msg)` | 编译警告（受 `-Werror` 影响） |
| `compile_time.debug(msg)` | 仅 Debug 构建输出警告；Release 静默 |
| `compile_time.assert_eq(a,b)` / `assert_ne(a,b)` | 常量比较；失败消息含两侧值 |
| `compile_time.assert_size(T,N)` / `assert_align(T,N)` / `assert_offset(T,"f",N)` | 布局断言；失败消息含实际/期望值 |
| `compile_time.format(fmt, args...)` | 编译期格式化，支持 `%d/%i/%u/%x/%X/%s/%c/%f/%%` |
| `compile_time.to_string(value)` | 编译期常量转字符串；bool 输出 `true`/`false` |

字符串结果只能用于编译期上下文；逃逸到运行时报
`compile-time string value cannot be used in runtime context`。

### 7.3 类型值、转换与模板内省

| API | 说明 |
|---|---|
| `type_of(T)`、`array_of(T,N)`、`pointer_to(T)`、`const_of(T)` | 编译期类型值，可嵌套进其他类型查询（如 `size_of(array_of(int32,4))`） |
| `is_convertible(A,B)`、`is_assignable(A,B)` | 基于现有隐式转换秩 |
| `template_name_of(T)`、`template_arg_count(T)`、`template_arg_name(T,i)` | 模板实例占位内省 |

`template_*` 读取的是 parse 期的 `TypeInstance` 占位；布局/字段查询仍要求已解析的具体类型，
因此 `size_of(Box<int32>)` 这类查询当前应改用显式实例化后的类型名，或等待后续把
`TypeInstance` 解析为实例类型。

类型值不得逃逸到运行时；`compile-time type value cannot be used in runtime context`。

### 7.4 编译期重复展开

```cmm
compile_time.repeat(3) {
    total += 1;
}
```

- 计数必须是编译期非负整数，上限 1024。
- sema 只分析一次 body；codegen 展开 `iterations` 份，每次使用独立作用域。
- 不生成运行时循环计数器；`break` 结束剩余展开，`continue` 跳到下一次展开，
  `return` 终止后续展开。
- `static`/`thread_local` 局部变量在 `compile_time.repeat` 内诊断拒绝——否则每次
  展开会错误地创建独立静态对象。
- 字段/枚举遍历（`for_fields`/`for_enum_values`）需要 AST 克隆或 codegen 特化，暂未实现。

### 7.5 编译期容器与常量表

`compile_time` 函数内可以使用 `std.vec` / `std.deque` / `std.hashmap` / `std.hashset`
的四类容器；求值器按运行时 API 语义执行，不解释 `malloc`/`memcpy`/容器方法 AST，
也不生成运行时分配。

| 能力 | 说明 |
|---|---|
| 容器类型 | `std::Vec<T>`、`std::Deque<T>`、`std::HashMap<K,V>`、`std::HashSet<K>` |
| 元素类型 | int8/16/32/64、uint8/16/32/64、bool、char、float32/64、enum、`string_view` |
| 控制流 | block/if/while/do/for/break/continue/return，赋值与复合赋值、`++`/`--` |
| 出参 | `pop(&out)`、`get(key, &out)` 等 `&local` 在调用边界 copy-in/copy-out |
| 回调 | `HashMap`/`HashSet` 的 hash/equal 与 `Vec::contains/index_of` 的相等函数须可在编译期求值 |
| 限制 | 任意指针、嵌套容器、`string` 元素、128 位整数常量暂不支持 |

- `compile_time` 变量（命名空间或块作用域）可以是上述容器类型；初始值折叠后，
  codegen 物化为私有只读常量：每个底层数组一个常量全局，加一个与运行时 ABI
  一致的容器结构体。`Vec`/`Deque` 的 `data` 指向常量数组，`Deque` 归一化
  `head = 0`；`HashMap`/`HashSet` 保留开放寻址的 `keys/values/states` 与回调
  函数指针。
- 物化后的容器是**只读**的：`push`/`pop`/`put`/`insert`/`remove`/`clear`/
  `destroy`/字段或元素赋值均诊断拒绝；把常量表复制到可变容器、可变变量或
  `const` 容器也拒绝（v1 尚未对容器方法/字段实施 const 检查），按值传给可变
  形参同样拒绝。`compile_time Vec B = A` 与按值传参在编译期求值中是深拷贝，允许。
  `get_ptr` 以及从 `.data` 取得的可变指针也拒绝；只允许 `get`/`len`/`cap`/
  `contains`/`size`/`bucket_count` 与 `.data[i]` 读取。
- 返回容器类型的 `compile_time` 函数仍是普通运行时函数；`compile_time` 只增加编译期
  求值能力，运行时调用走正常容器分配/拷贝语义。
- 求值步数上限 1,000,000；未知回调、不可求值操作或超限都会使该常量求值失败并
  给出定位诊断，不会静默退化成运行时值。

## 8. 类型作为值（CT-08）

- 编译期一等公民：类型可作模板实参（GEN-03）、反射返回值、比较与选择。
- 类型值不等同运行时数据；不得逃逸到运行时。

## 9. 缓存 / 诊断 / 沙箱 / 常量集成（CT-09 ~ CT-12）

- **缓存（CT-09）**：按输入内容哈希缓存求值结果，接入增量编译（MOD-10）。
- **诊断（CT-10）**：错误包含源码位置、求值栈、原因；失败即中止该编译单元。
- **沙箱（CT-11）**：默认禁止文件/网络/系统访问；如需，必须显式白名单（接口待定）。
- **LLVM 常量集成（CT-12）**：求值结果直接生成 `llvm::Constant`，不留运行时指令。
- **CTX-01 性能边界**：类型谓词、极限、字段/枚举/类查询全部是编译期标量，零运行时 IR；
  `LayoutBuilder` 结果按 `Type*` 在单次 sema 内缓存，重复 `size_of`/字段查询不重复布局。
  仅 `fields_of`/`attributes_of` 保留运行时只读表；`compile_time.repeat` 展开代码但不生成
  运行时循环计数器。

## 10. `compile_time` / `compile_only` 声明（CT-23 ~ CT-27 / DEC-05 superseded）

### 10.1 语法

```cmm
compile_time int32 factorial(int32 n) { ... }       // CT + runtime
compile_time int32 N = factorial(5);                 // implicit read-only
compile_only DfaTable build_dfa(string_view p) { ... } // CT-only, no symbol
```

- `compile_time` 和 `compile_only` 是上下文关键字，不新增保留字。
- `compile_time` 变量隐式只读；只读性记录在声明/符号上，不修改共享 `Type::isConst`。
- `compile_time` 函数可在常量上下文求值，也可正常运行期调用；运行期 ABI 不变。
- `compile_only` 函数只存在于编译期：codegen 不发射 `define` 或 `declare`。
- `compile_only` 仅支持自由函数；禁止方法、模板、lambda、地址、函数指针、字段、闭包、vtable。
- `compile_only` 返回 `void` 或定长、可平凡拷贝的 CT 值：标量、enum、固定数组、POD record；
  禁止指针/引用/拥有型 `string`/`string_view`/union/位域/柔性数组/基类槽。
- `compile_only` 参数允许 CT 值或只读 `string`/`string_view`；禁止指针/引用参数。
- `compile_only` 体内禁止运行期函数调用、I/O、`volatile`、`atomic`、`asm`、可变全局、
  `thread_local`、`defer`、`compile_time.repeat`；允许 `compile_time`/`compile_only` 调用与
  纯 `string`/`string_view` 的 `len()`/`char_at()`。
- 调用位置：`compile_time` 变量初始化、`compile_only` 体、`if compile_time` 的 then、
  `compile_time.static_assert`/`compile_time.if` 条件。运行期路径和 `compile_time` 函数体
  的直接调用（不在 `if compile_time` 内）报错。
- parser 级数组维度/模板实参使用命名 `compile_time` 常量承接返回值，不直接调用
  `compile_only`。

### 10.2 `if compile_time`

```cmm
compile_time int32 table_at(int32 s, int32 c) {
    if compile_time {
        return EMAIL_DFA.next[s][c];   // CT path
    } else {
        return EMAIL_DFA.next[s][c];   // runtime path (materialized data)
    }
}
```

- 只允许在 `compile_time`/`compile_only` 函数内；两个分支必须用花括号。
- `compile_time` 函数：CT 求值执行 then，codegen 只发射 else；sema 分析两分支。
- `compile_only` 函数：CT 只执行 then；函数整体无运行期符号，else 不分析。
- 普通运行期函数中出现报错。与 `compile_time.if(cond)` 不同：后者按常量条件选分支。

### 10.3 聚合值

- `CtValue::AGGREGATE` + `CtAggregate` 表示固定数组/POD record；数组行优先，record 按字段序。
- 求值器支持聚合局部、字段/下标读写、聚合参数/返回、深拷贝和递归物化。
- `compile_time DfaTable EMAIL_DFA = build_dfa("...");` 由 codegen 发射私有只读常量。
- 资源上限：递归深度 128、步数 1,000,000、聚合元素 1,000,000（单次求值）。

### 10.4 诊断

- 运行期可达链：`main -> make_table -> build_dfa`，附“移入 compile_time 初始化/加 if compile_time”提示。
- `compile_time` 函数直接调用 `compile_only`：`outside 'if compile_time'`。
- 地址/值使用、运行期函数调用、可变全局、非法返回类型均有定向诊断。
- `constexpr`：`'constexpr' has been removed; use 'compile_time' ... or 'compile_only' ...`。

## 11. 与泛型的关系（GEN-06 / SEM-06）

- 模板实例化点做类型检查；反射可在实例化时读取类型信息，实现无 trait 的适配（`to_string`/`to_hash`/`equals`，见 semantics.md §5）。
- `compile_time` 不可绕过实例化规则，不得引入运行时多态（NG-09）。

## 12. 未决项

| 决策 | 内容 |
|---|---|
| DEC-05 | 已由 CT-23/24/27 取代：删除 `constexpr`，使用 `compile_time`/`compile_only` |
| DEC-09 | 内联 `asm`（与 `compile_time` 正交，但共享目标查询） |
| CT-13 | 反射 API 精确签名与返回类型 |
| CT-22 | `for_fields`/`for_enum_values` 的 AST 克隆或 codegen 特化方案 |
