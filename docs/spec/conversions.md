# Cmm 1.0 — 类型转换规范

> 对应 TODO：**TYP-19 ~ TYP-24**、**TYP-20**、**DEC-06/07**
> 状态：`[impl]` 以 `src/sema/SemanticAnalyzer.cpp`、`src/ast/Symbol.cpp`、`src/codegen/CodegenContext.cpp` 为准；`[plan]` 为目标。
> 设计原则：与 C 的内存模型/转换语义对齐，但去掉隐式危险转换（如 enum↔int32、指针↔int32 的静默）。

## 1. 类型等价 `[impl]`

`typesEqual`（`src/ast/Symbol.cpp`）：

- 同 `TypeKind` 基本相等（标量只看 kind）。
- 指针/数组：递归比较元素类型（数组不比较长度）。
- struct/class/union：按**名字**相等。匿名定义（无 tag）获得唯一合成名
  `__anon_N`（PAR-12，2026-10-17）——每次匿名定义是**独立类型**，两次匿名
  定义互不相等（C 语义）。
- enum：按**名字**相等（名义类型，TYP-09/TYP-20）；不同枚举互不相等。
- Optional（TYP-13）：按元素类型相等（`int32?` ≠ `float64?`，2026-10-06）。
- Result（TYP-14）：按两个实参类型相等。
- using 别名：展开后比较。
- 函数：返回类型 + 参数逐个比较（不比较 `isVarArg`）。

**目标 `[plan]`**：数组长度应参与等价（TYP-11）；`const`/限定符应参与兼容性判断（TYP-16）。

## 2. 隐式转换（当前实现）`[impl]`

`typesCompatible` 允许：

| 来源 → 目标 | 允许 |
|---|---|
| 同 kind | ✅（聚合须**同名**，见下） |
| 任意算术 → 任意算术 | ✅（`enum` 除外，见 TYP-20） |
| 指针 ↔ 指针 | ✅ |
| 指针 ↔ `int32` | ✅（含 0 空指针） |
| 数组 ↔ 指针 | ✅（退化） |
| `enum E` ↔ `enum E` | ✅（同一枚举类型） |
| `enum` ↔ 整数/浮点/其它 `enum` | ❌（须显式转换，TYP-20） |

> **聚合名义兼容（PAR-12，2026-10-17）**：struct/class/union 赋值/初始化/
> 实参/返回/比较的同 kind 放行收紧为**同名**（`typesEqual`）或**派生→基**
> （INH-04 值切片，`isDerivedFrom`）。两次独立匿名定义（合成名不同）互斥；
> 同一声明多变量（`struct { ... } a, b;`）共享类型。见 §1。

`getCommonType`（`[impl]`，TYP-22）：两个算术操作数按整数提升 + 常规算术转换取公共类型（见 §3）；非算术按同 kind 或取左。

> `enum` 的强类型（TYP-20）与名义相等已落地：赋值/初始化/实参/返回中的 `enum ↔ int` 报错，须用 `(T)` / `static_cast` / `reinterpret_cast`。
> 常规算术转换（TYP-22）已落地；**隐式窄化**仍被当作兼容（`int32 → int8` 不报错），属 TYP-23 待定。

## 3. 整数提升与常规算术转换 `[impl]`（TYP-22）

实现于 `usualArithmeticType` / `promoteArithmeticType`（`src/ast/Symbol.cpp`），被语义分析与代码生成共享：

1. **整数提升**：`bool`、`char`、`int8/16`、`uint8/16`（以及底层窄于 `int32` 的 `enum`）→ `int32`。
2. **常规算术转换**（二元算术/比较/位运算，对操作数取公共类型）：
   - 若任一为浮点：`float32 < float64`，浮点胜过整数。
   - 否则（提升后同为整数）：
     - 符号性相同 → 取 rank 较高者。
     - 不同符号性：无符号 rank ≥ 有符号 → 无符号；有符号 rank 更高则取有符号（可覆盖无符号全部值）。
3. 转换等级（rank，由低到高）：`bool < char/int8/uint8 < int16/uint16 < int32/uint32 < int64/uint64/isize/usize < int128/uint128`；同宽度有/无符号同 rank。
4. `[impl]` 函数实参转换（`conversionRank`）允许整数 → `bool`（rank 2，低于拓宽），因此比较结果（类型为 `int32`）可直接传给 `bool` 形参；`bool → 整数` 按拓宽（rank 1）。浮点 → `bool` 仍需显式转换。

| kind | 位宽（`integerRank`） |
|---|---|
| bool | 0 |
| char/int8/uint8 | 1 |
| int16/uint16 | 2 |
| int32/uint32/enum | 3 |
| int64/uint64/isize/usize | 4 |
| int128/uint128 | 5 |
| float32 | 32 位 |
| float64 | 64 位 |

## 4. 转换矩阵 `[plan]`（TYP-23）

图例：`隐`=隐式允许；`警`=隐式允许但告警；`显`=仅显式转换；`✗`=禁止。目标语义，待 TYP-23 定稿。

| 从 \ 到 | 宽整数 | 窄整数 | 有/无符号互换 | 浮点 | bool | 指针 | enum |
|---|---|---|---|---|---|---|---|
| 宽整数 | 隐 | 显 | 隐/显 | 隐 | 隐 | 显 | 显 |
| 窄整数 | 隐 | 隐 | 隐/显 | 隐 | 隐 | 显 | 显 |
| 浮点 | 显 | 显 | — | 隐(宽)/显(窄) | 显 | ✗ | 显 |
| bool | 隐 | 隐 | 隐 | 隐 | 隐 | ✗ | 显 |
| 指针 | 显 | 显 | — | ✗ | 隐(≠0) | 隐(同类型)/显 | 显 |
| enum | 显 | 显 | 显 | 显 | 显 | ✗ | 隐 |

> `enum` 与整数**必须显式转换**（TYP-20）——已实现：`typesCompatible` / `conversionRank` / `checkAssignmentTypes` 对 `enum ↔ 非同类 enum / 整数 / 浮点` 判为不兼容；而算术、位运算与比较运算符仍按整数提升把枚举操作数提升为 `int32`（`e + 1`、`e == 4`、`switch(e)` 合法）。

## 5. 显式转换 `[impl]/[plan]`

- `[impl]` C 风格 `(T)x`：`parseUnary` 先试探性解析完整类型并要求立即 `)`，否则回退为括号表达式；因此支持指针、限定符与命名类型，如 `(int32*)p`、`(struct S*)p`、`(void*)h`（P0-02 / MEM-10）。窄化只告警不报错（`CastExprAST` 检查）。
- `[impl]` `static_cast<T>(x)`（LEX-11 / PAR-18 / DEC-18）：允许算术↔算术（含 enum，按整数）、指针/数组↔指针/数组；但**不**允许指针↔整数（须用 `reinterpret_cast`）。不兼容时报错。代码生成走 `castValue`（值转换）。
- `[impl]` `reinterpret_cast<T>(x)`：任何标量↔标量（按位重解释）。同宽标量（尤其 `float32`↔`int32`）用 `bitcast` 重解释位模式；指针↔指针用 `bitcast`、指针↔整数用 `ptrtoint`/`inttoptr`。不兼容时报错。
- `[impl]` 三者为**上下文关键字**：`static_cast`/`reinterpret_cast` 仍是普通标识符，仅在紧跟 `<类型>` 时按转换运算符解析，否则按普通名字处理。
- `[impl]` 指针 ↔ 整数的显式转换：`castValue` 对 `Int↔Ptr` 做位宽适配（`ptrtoint`/`inttoptr` 的整数扩展/截断）；指针真值经 `ICmpNE null`（TYP-24 / BASE-06）。
- `[impl]` `enum ↔ int` 及其它标量的**显式**规则（TYP-20）：`(T)x` / `static_cast` / `reinterpret_cast` 均可；隐式赋值/初始化/实参/返回报错（E2003 等）。算术/比较/位运算仍按整数提升处理枚举操作数（`e + 1`、`e == 4`、`switch(e)` 合法）。
- 代码生成 `castValue`（`CodegenContext.cpp`）：

| 转换 | 指令 |
|---|---|
| 整数→更宽 | `zext`（无符号源或 `i1`）/ `sext`（有符号源，TYP-22） |
| 整数→更窄 | `trunc` |
| 整数→浮点 | `uitofp`（无符号源）/ `sitofp` |
| 浮点→整数 | `fptosi` |
| 浮点→更宽/更窄 | `fpext` / `fptrunc` |
| 指针→指针 | `bitcast` |
| `reinterpret_cast` 同宽标量（`float32`↔`int32` 等） | `bitcast` |
| `reinterpret_cast` 指针↔整数 | `ptrtoint` / `inttoptr` |

> `castValue` 提供带源 AST 类型的重载（`castValue(val, fromAST, target)`），按源的有/无符号选择 `zext`/`uitofp`；赋值、初始化、实参、返回与二元运算均走该重载（TYP-22）。

二元运算按公共类型的有/无符号选择指令（`src/ast/Expr.cpp`）：

| 运算 | 有符号 | 无符号 |
|---|---|---|
| `/` `%` | `sdiv` / `srem` | `udiv` / `urem` |
| `<` `>` `<=` `>=` | `icmp slt/…` | `icmp ult/…` |
| `>>` | `ashr` | `lshr` |

## 6. 空指针常量（TYP-24）

- `[impl]` `null` 是唯一空指针字面量，在 `parsePrimary` 中变成整数常量 `0`。
- `[impl]` `nullptr` 不再是关键字；使用它按未声明标识符报错（Review 2.3）。
- `[impl]` `null` 只隐式转换为指针（以及既有整数/bool 通道）；不转换为 `optional`/`result`。
- `[impl]` 指针作为条件/`!`/逻辑运算：归一化为“与 null 比较”（`ICmpNE null`），见 BASE-06。
- `[impl]` 指针与整数的**比较**：非指针一侧提升到指针宽度后按整数比较。
- `[plan]` `NULL` 宏（无预处理器，不提供）与指针↔整数的静默转换规则（TYP-24 余项）。

## 7. 常量求值与溢出

- `compile_time` / 常量折叠 `[impl]`（BASE-05）。
- 整型字面量溢出诊断：见 LEX-17 `[impl]`——二/八/十六/十进制按 64 位无符号累加，超过 64 位报 `E0003`；分隔符 `_` 仅可位于两个数字之间，违规报 `E0004`（`src/frontend/Lexer.cpp`）。
- 字面量默认类型与后缀（LEX-15，词法层）`[impl]`：无后缀整型 → `int32`，`u` → `uint32`，`l` → `int64`，`ul`/`lu` → `uint64`；无后缀浮点 → `float64`，`f`/`f32` → `float32`，`f16`/`f64`/`f128` → 对应类型。结果经 `Token::literalKind` + `literalKindName` 暴露；AST/sema/codegen 按字面量类型参与推导与 codegen 已实现（LEX-15 消费端：AST 携带 `literalKind`，sema 按 kind 赋定宽类型，codegen 按宽度/符号与 APFloat 语义生成常量）。
- 浮点指数（LEX-05）`[impl]`：`e`/`E` + 可选 `+`/`-` + 十进制数字，数字间允许 `_`；带指数者即使无小数点为浮点，默认 `float64`，后缀照常生效（如 `1e10f`→`float32`）。`e` 后不足一个数字时不构成指数（`1e` → `1` + 标识符 `e`）。
- 整数溢出行为：Debug 检查 / Release 回绕，见 **DEC-07**（SEM-10）。
- 数组/Slice 边界检查策略见 **DEC-06**（SEM-09）。

## 8. 重载解析中的转换等级 `[impl]`

`conversionRank`（`src/ast/Symbol.cpp`）用于 `OverloadSet::resolve`：

| 值 | 含义 |
|---|---|
| `0` | 精确匹配 |
| `1` | 安全算术转换：整数加宽、整数→浮点、浮点加宽、数组→指针、函数→函数指针、`string_view`/`string` → `const char*` |
| `2` | C 指针转换：任意指针→任意指针（尤其 `T*`↔`void*`）（MEM-10） |
| `3` | 空指针常量（整数 / `null`）→指针（MEM-10） |
| `-1` | 不可隐式转换 |

> 目标：随 §3/§4 定稿后细化等级（含限定符、指针限定、模板实参推导），见 SEM-16。

## 9. 默认实参提升（可变参数）`[impl]`

调用可变参数函数（如 `printf`）时，**尾部实参**（固定形参之外）按 C 的默认实参提升规则转换（`promoteVarArg`，`src/ast/Expr.cpp`）：

| 源类型 | 目标 | 指令 |
|---|---|---|
| `float32` | `float64` | `fpext` |
| `float16` | `float64` | `fpext`（TYP-04，对齐 C23 `_Float16` 提升） |
| `bool` | `int32` | `zext` |
| `int8` / `int16` / `char` | `int32` | `sext` |
| `uint8` / `uint16` | `int32` | `zext` |
| `enum`（窄底层） | 按底层类型的提升 | 递归 |

- 固定形参仍按 `resolvedParamTypes` 做常规转换（§5）。
- 任意标量（含指针）以外的类型不提升，原样传递。`float128` 不提升（对齐 C23 `__float128`；x86-64 ABI 原生支持四精度传递）。
- 这是 C ABI 正确性要求：窄类型在寄存器/栈槽中按 `int32` 宽度传递，否则 `printf("%d", x)` 读到未定义的高位。

### 9.1 `print`/`println` 对浮点的格式化 `[impl]`（TYP-04）

`float16`/`float128` 可与 `float32`/`float64` 一样使用 `print`/`println` 的**全部格式说明符**（`{}`、`{:f}`、`{:.2f}`、`{:e}`、`{:08.3f}` 等）：实参在 codegen 阶段统一提升为 `float64` 后走既有格式化路径——

- `float16` → `float64`：**无损**（half 是 double 的子集）。
- `float128` → `float64`：**精度截断**到 double（约 17 位十进制有效数字；值域超出 double 时按 IEEE 754 溢出为 `±inf`）。113 位尾数的精确十进制输出挂靠 FMT-13。

## 10. 数组→Slice 退化 `[impl]`（TYP-12）

`T[N]` 到 `T[]` 的退化为**单向、零拷贝**：slice 仅持有 `{首元素指针, 长度}` 视图，不复制元素、不管理生命周期。写入经 slice 视图直达原数组。

**发生位置**：函数传参、赋值、变量初始化。形参数组 `T name[N]`（含空括号 `T name[]`，TYP-11）已去糖为 slice，形参位置不存在 ArrayType。

`return` 局部数组被拒绝（悬垂 D4，"cannot return a slice view of an array with local storage"）。当前所有 ArrayType 值均具局部存储，故全局数组同样被保守拒绝；待全局/堆数组语义明确后按需放开。

**表示与初始化**：slice 的 LLVM 形状为规范命名结构体 `{ptr(0), i64}`（所有元素类型共用单例）。声明未初始化的 slice 变量零初始化为**空切片** `{null, 0}`。

**成员**：`.len`（`int64`，只读）是 slice 的唯一成员；`.ptr` 不暴露（保持视图安全）。

**越界**：`s[i]` 不做边界检查（D2），越界为未定义行为。

**元素类型**：slice 匹配按元素类型严格相等（`int32[]` ≠ `float64[]`）；跨元素类型赋值/传参/退化均拒绝。

**范围内不含**（spec §8）：范围切片 `arr[1..3]`、显式 slice 构造、边界检查/安全模式、字符串字面量退化。
