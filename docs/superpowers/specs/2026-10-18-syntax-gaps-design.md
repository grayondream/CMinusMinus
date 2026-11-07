# 语法遗漏收口 — 设计文档

> 对应 TODO：LEX-16 / LEX-18 / PAR-14 / TYP-24 / TYP-05 / CON-01 / CON-04 / FUN-09 /
> CT-07 / INH-06（protected 放宽）/ 文档同步。
> 状态：`[plan]`。本文为作者（+ builder-check 评审）决策后的权威来源。

## 1. 背景与目标

前轮完成 P1-09 std.mem/std.bit/std.math 后，用户要求**暂停标准库**，改为从语法层面
审计语言缺口。审计基于 `docs/spec/grammar.ebnf` 的 `[plan]` 标记、`docs/spec/keywords.md`
的 `[plan]` 标记，并对实际编译器（`build/bin/cmm`）逐项实测。

实测发现两类问题：

1. **语法特性未实现**：多行字符串、Unicode 标识符、`alignof`/`offsetof` 独立关键字、
   `nullptr`、`thread_local`、`atomic`、默认参数、`char8/16/32`、`compile_time` 反射、
   `protected` 派生访问。
2. **静默丢弃缺陷**：`f16 x = 1.0f16;` / `char8 c;` / `atomic int i;` 这类以未知
   类型名开头的顶层声明被解析器**静默跳过**（零诊断、声明消失、变量未声明）。这是
   parser 对未知类型名的宽容路径，比"未实现"更危险。

**目标**：实现上述语法特性 + 修复静默丢弃缺陷 + 同步滞后文档（`str`→`string_view`
硬改名、裸 `return;`、原始字符串、转义全集、`compile_time.align_of/offset_of` 已有等）。

**非目标**：标准库（std.collections 等，用户明确暂停）；`atomic` 的完整并发语义模型
（CON-02/03/09/10 内存序、fence、CAS 另轮）；`thread_local` 的完整存储模型
（CON-06/10 另轮）；`char8/16/32` 的 UTF 字符串字面量类型族（仅类型关键字）。

## 2. 各特性设计

### 2.1 多行字符串 `"""..."""`（LEX-16）

- 语法：`"""` 起，`"""` 止，内容可跨行，**不做缩进剥离**（保持 C 字符串直白语义）。
- 内容中的换行、空白原样保留；转义序列照常处理（复用 `decodeEscapes`）。
- 词法：`scanString` 增加三分支——`peek()` 遇 `"` 时前瞻 `peekNext() == '"' &&
  peekNext2() == '"'` 进入多行扫描；终止条件为连续三个 `"`。
- 诊断：未闭合多行字符串报既有 E0002。
- 实现落点：`Lexer::scanString`（先于 `r"` 与普通字符串分支判定）。

### 2.2 Unicode 标识符（LEX-18）

- 允许范围：标识符首字符与续字符放宽到 **UTF-8 编码的 Unicode 字母/数字**。
  - 首字符：ASCII 字母/`_` + 任意非 ASCII 多字节序列（按 UTF-8 引导字节判定）。
  - 续字符：首字符集 + ASCII 数字。
- **不做 NFC 规范化**（YAGNI，记文档）；关键字冲突天然不存在（关键字全 ASCII）。
- 词法：`scanIdentifier` 当前用 `std::isalpha(ch)`/`isdigit(ch)`（字节级）判定——改为
  字节高位（`ch & 0x80`）计入合法标识符字符；`scanToken` 的 `isalpha(ch) or ch=='_'`
  起始判定同步放宽。
- 诊断：非法字节序列仍报 E0001（非法字符），与现状一致。
- 注意：UTF-8 引导字节（`0xC2..0xF4`）及续字节（`0x80..0xBF`）都计入续字符即可，
  无需逐码点校验（E2015 的 UTF-8 校验仅用于字符串字面量，标识符不做语义校验）。

### 2.3 `alignof`/`offsetof` 独立关键字形式（PAR-14）

- token 已在（`TOKEN_ALIGNOF`/`TOKEN_OFFSETOF`，lexer 已识别），但 **parser 无表达式
  分支**（实测 `unexpected token`）。
- 语法（对齐 C）：
  - `alignof(T)` → `usize`，等价 `compile_time.align_of(T)`。
  - `offsetof(T, member)` → `usize`，等价 `compile_time.offset_of(T, "member")`
    （第二个实参为**裸标识符**，非字符串——C 形态；parser 转成字符串传给既有
    offset_of 求值）。
- 实现：parser 前缀表达式分支构造常量表达式 AST（复用 `compile_time.` 求值器
  `ConstEvaluator` 的 size_of/align_of/offset_of 通路），sema/codegen 按常量折叠。
- `alignof` 类型实参须在 `()` 内（`alignof int32` 无括号形式**不支持**，C 亦要求括号
  形态用于类型）。

### 2.4 `nullptr`（TYP-24）

- 语法：`nullptr` 作为 `null` 的同义空指针字面量。
- 实现：lexer 注册 `{"nullptr", TOKEN_NULL}`（同一 token，最小改动；`nullptr` 变体
  与 `null` 完全等价）。`NULL`（宏）不提供（无预处理器，用户自 `std.c` 或字面量）。
- 文档同步：`conversions.md` 的 `nullptr` `[plan]` 标记置 `[impl]`。

### 2.5 `thread_local` 存储类（CON-04）

- 语法：`thread_local T name = init;`（顶层/命名空间/函数内 static 局部）。
- 词法：注册 `{"thread_local", TOKEN_THREAD_LOCAL}`。
- 解析：存储类位置（与 `extern`/`static` 平级）。约束：仅作用于**变量声明**；
  函数声明前 `thread_local` 报错（E 码复用诊断注册表，新增 `SemInvalidStorageClass`）。
- 语义/codegen：全局变量 `llvm::GlobalVariable::setThreadLocal(true)`；无初始化器时
  零初始化（与既有全局一致）。函数内 `static` 局部 + thread_local → LLVM thread_local
  局部。**不实现**：TLS 初始化/销毁 ABI 细节（CON-10 另轮），LLVM 承担。
- **关键**：`thread_local` 现无 token，`thread_local int x;` 被静默吞——注册 token +
  解析分支即同时修复该静默问题。

### 2.6 `atomic` 类型限定符（CON-01）

- 语法：`atomic T name;`（类型限定符位置，`atomic int32 count;`）。
- 词法：注册 `{"atomic", TOKEN_ATOMIC}`。
- 类型：`Type` 增加 `isAtomic` 标志（或限定符位，与 `isConst`/`isVolatile` 平级）。
  `atomic` 是**限定符而非独立类型**：`atomic int32` 与 `int32` 布局/大小一致。
- 语义（最小，冻结为**本语言 CON-01 范围**）：
  - 直接读写经 LLVM `load atomic`/`store atomic`（`seq_cst`，`align` 由类型决定）。
  - 复合赋值/`++`/`--` 经 `atomicrmw`（add/sub/xchg/…，`seq_cst`）。
  - 禁止：`atomic` 修饰非标量（struct/array/slice/optional/result）——E 码新增；
    `atomic` 取址后按普通指针传递**不保证原子性**（记文档，与 C 一致）。
  - 不实现（CON-02/03/09 另轮）：内存序变体、fence、CAS、`atomic_*` 库函数、
    volatile+atomic 交互细化。
- **关键**：`atomic int i;` 现被静默吞——注册 token + 解析分支即修复。

### 2.7 默认参数（FUN-09）

- 语法：`T name = const-expr` 出现在参数列表（**仅尾随参数可有默认值**；中部默认值
  报错，防调用歧义）。
- AST：`ParamDeclAST` 增加 `std::unique_ptr<ExprAST> defaultValue` + `bool hasDefault`。
- 解析：`parseParamDecl` 遇 `=` 解析常量表达式（复用 constexpr 求值）。
- sema/符号表：
  - `FunctionType` 需要记录默认值——**不在 FunctionType 上改**（避免触碰
    mangle/ABI），改在 `FunctionDeclAST`/`MethodDeclAST` 上记录 `defaultArgCount`，
    OverloadSet 解析调用时经符号回查。
  - `OverloadSet::resolve`：候选可接受实参数目 `[minArgs, paramCount]`，其中
    `minArgs = paramCount - defaultArgCount`；`minArgs <= argTypes.size() <= paramCount`
    即可行。**既有 `argTypes.size() != fixedCount` 硬相等放宽为区间判定**。
  - 默认实参的值在**调用点**求值并参与类型转换（转换规则与显式实参一致）。
- codegen：调用点实参不足时补默认值常量。
- 约束：默认值必须可编译期求值（constexpr）；方法（含 `this`）与自由函数一致。
  varargs 函数不参与默认参数（`...` 后不能有默认值）。

### 2.8 `char8/char16/char32` 类型（TYP-05）

- 语法：三个新类型关键字。
- 语义：`char8` = `uint8`、`char16` = `uint16`、`char32` = `uint32` 的**别名**
  （非独立名义类型，YAGNI——C++ 名义 char8_t 语义对本语言无场景；文档记决策）。
- 实现：lexer 注册三 token → parser `parseBaseType` 映射到既有 `getUInt8/UInt16/UInt32`
  ——最小改动，无类型系统涟漪。
- **关键**：`char8 c;` 现被静默吞——注册即修复。

### 2.9 未知类型名声明静默丢弃（缺陷修复）

- 现状：`parseDeclarationImpl` 最终 `return parseDeclarationAsType()`；未知类型名时
  `parseBaseType` 返回 nullptr → 顶层循环 `advance()` 跳过 → **零诊断**。
- 修复：`parseDeclarationAsType` 中 `parseType()` 返回 nullptr 且当前 token 为
  `TOKEN_IDENTIFIER` 或类型起始关键字时，报 **"unknown type 'name'"**（新增 E 码
  `SemUnknownType`，或复用既有未声明标识符诊断）。顶层循环的 `advance()` 保留作为
  错误恢复，但诊断已使编译失败。
- 影响面核对：需全量回归确保不误伤既有合法程序（合法程序不会以未知类型名开头）。
  特别核对模板实例 `Name<...>` 解析失败路径（已有独立诊断）不被双报。

### 2.10 `compile_time` 反射 API（CT-07/13）

现状：`size_of/align_of/offset_of` 已有。补齐：

| 函数 | 签名 | 语义 |
|---|---|---|
| `compile_time.name_of(T)` | `-> str` | 类型名（`typeToString`） |
| `compile_time.fields_of(T)` | `-> Slice<FieldInfo>` | 聚合字段表 |
| `compile_time.attributes_of(T)` | `-> Slice<Attribute>` | 注解表 |

- `FieldInfo`：`{ string_view name; usize offset; usize size; }`（聚合 struct，
  **内建反射类型**——仿 optional/result/string_view 的 parser 特判，恒可用，不依赖
  import std.core）。
- `Attribute`：`{ string_view name; }`（内建反射类型；`args` 反射另轮）。
- **交接机制**（builder-check 裁定，本特性核心）：编译期求值器 `ConstValue` 只承载
  标量（INT/DOUBLE/CHAR/STR），且 STR 逃逸到运行时语境是硬错误；`fields_of`/
  `attributes_of` 返回运行时 `Slice`，无法经现有求值器路径。
  方案：sema 期在**分析器侧注册表**记录反射请求的结果（类型名 / 字段表 / 注解表），
  CallExpr 解析为**专用节点**（如 `ReflectionExprAST`，携带请求键），codegen 阶段
  由 `CodegenContext` 查询注册表物化**全局常量数组**（`FieldInfo`/`Attribute` 聚合
  常量）并构造 `{ptr, len}` slice 视图。sema 与 codegen 之间经共享注册表
  （`ReflectionRegistry`，进程级单例或挂在 TranslationUnitAST 上——计划阶段定）。
- `name_of(T)` 返回 `string_view`（str 已改名）：sema 折叠为字符串字面量 + 视图，
  交接最小化（字符串全局 + slice）。
- 表达式路径：仿 `compile_time.size_of` 既有实现，sema MethodCall/MemberAccess
  钩子特判。
- 约束：`fields_of` 仅对 struct/class/union（enum 另轮，报错或空表——文档记决策）；
  `attributes_of` 返回声明注解。

### 2.11 `protected` 派生类访问放宽（INH-06）

- 现状：`checkStaticMemberAccess`/成员访问判定式 `currentClass == definingClass`，
  派生类方法内经 `this->` 访问基类 protected 成员仍被拒（比 C++ 严格，INH-06 记录）。
- 修复：判定式放宽为 `currentClass == definingClass || 派生链上 currentClass 的基类链
  含 definingClass`（`isDerivedFrom`，沿 base 链）。三处检查点收敛（字段访问、方法
  访问、static 成员访问）。
- 保持：类外访问 private/protected 仍拒（E2009）；private 语义不变。
- 影响面：既有 INH-06 收紧行为变更——相关 pin 测试更新为派生类内放行的新语义。

## 3. 排序与依赖

实现顺序按依赖关系：

1. **词法层**（2.1/2.2/2.4/2.5/2.6/2.8 的 token/扫描部分）：多行字符串、Unicode
   标识符、`nullptr`、`thread_local`、`atomic`、`char8/16/32` 关键字。
2. **类型层**（2.8 完整、2.6 完整、2.5 完整）：新类型映射、atomic 限定符、
   thread_local 存储类。
3. **表达式层**（2.3/2.4/2.7）：`alignof`/`offsetof`、`nullptr` 使用、默认参数。
4. **缺陷修复**（2.9）：未知类型名诊断。
5. **反射**（2.10）：依赖 str/Slice（已有）。
6. **protected 放宽**（2.11）。
7. **文档同步**（§2 各节提及 + §4）。

## 4. 文档同步清单

- `docs/spec/grammar.ebnf`：primitive-type 去 `'str'`（已改名 string_view，补
  `'string_view'`）；multiline-string/Unicode 标识符/`nullptr`/`thread_local`/`atomic`/
  `char8|16|32`/`alignof`/`offsetof`/默认参数/反射函数 的 `[plan]` → `[impl]`；
  protected 继承访问更新。
- `docs/spec/keywords.md`：同步上述状态；`str`/`String` 行修正；裸 `return;`、
  原始字符串、转义全集状态。
- `docs/spec/conversions.md`：`nullptr` `[plan]` → `[impl]`；atomic/thread_local 简述。
- `docs/spec/compile_time.md`：反射函数表补 name_of/fields_of/attributes_of。
- `docs/spec/semantics.md`：重载解析补默认参数规则；protected 派生访问规则。
- `docs/spec/abi.md`：如涉 thread_local/atomic 布局则补一句（大小/对齐不变）。
- `TODO.md` / `Progress.md`：勾记 LEX-16/18、PAR-14、TYP-24/05、CON-01/04、FUN-09、
  CT-07/13、INH-06；新增/更新进度快照。

## 5. 风险与边界

- **静默丢弃修复**（2.9）是全语言行为变更点，全量回归是底线；模板实例解析失败路径
  需防双报。
- **默认参数**（2.7）触碰 `OverloadSet::resolve` 硬相等判定，是重载解析核心——用
  区间判定替换后需全量回归，防误伤既有重载。
- **atomic/thread_local** 按"最小可工作 + 文档边界"交付，不做完整并发模型（CON
  其余条目保持 `[ ]`）。
- **protected 放宽**（2.11）改变既有收紧语义，相关 pin 测试更新。
- 所有特性 TDD（RED→GREEN→提交），每特性独立提交，commit 引用对应 TODO ID。

## 6. 作者决策记录（builder-check 后补充）

builder-check（subagent `ses_eb0d6b2aeffekPHUdpkVKt9Eg3`）返回 8 项问题，裁定如下：

1. **默认参数符号回查机制**：`Scope::declare` 只建 `Symbol{name, funcType}`，无 Decl 回指针；
   `ClassType::methods` 仅存 `{name, FunctionType*}`。裁定——`Symbol` 增加可空
   `FunctionDeclAST* decl`（函数声明时设置），方法经 `ClassType` 方法表增加
   `MethodDeclAST*` 回指针；**默认值在调用点由 sema 折叠后暂存到 `CallExprAST`
   （`defaultArgValues`），codegen 直接读取**——不做 codegen 期回查。
2. **重载区间化连带面**：除 `OverloadSet::resolve` 外，`resolveMethod`
   （SemanticAnalyzer.cpp:5382/5406）与 `checkFunctionCall`（:728-748/763）的硬等号
   检查一并放宽；`resolve` 循环 `argTypes[i]` 按**实参个数**限界（防
   `nArgs < paramCount` 时越界）；并列时**偏好少用默认参数的候选**（C++ 语义），
   仍并列才歧义。
3. **atomic 别名陷阱**：标量基础类型是共享单例（`getInt32()` 等），`isAtomic` 原地
   置位会污染后续全部同型声明。裁定——**限定符克隆类型**：`atomic T` 构造带
   `isAtomic` 标志的副本 Type（不触碰共享单例）；`typesEqual`/mangle 需纳入
   `isAtomic`（`atomic int32` ≠ `int32`）。`isConst/isVolatile` 的既有别名隐患
   （pre-existing）不在本轮范围。
4. **atomic 操作集边界**：`atomicrmw` 仅支持 `add/sub/and/or/xor`（整型）。裁定——
   本轮 atomic 限定符**仅限整型标量**；`+ - & | ^`（含 `++/--` 映射 add/sub 1）经
   `atomicrmw`；`* / % << >>` 与浮点/指针 atomic 一律**编译期拒绝**（需 cmpxchg 或
   LLVM 不支持，属 CON-02 边界）。
5. **反射交接机制**：见 §2.10 重写——专用节点 + 注册表 + codegen 物化全局数组。
   `FieldInfo`/`Attribute` 为内建类型。用户批准**全做**（name_of + fields_of +
   attributes_of）。`name_of` 签名 `-> string_view`。
6. **未知类型双报防护**：模板实参解析失败会恢复光标到 `bar`，若按光标 token 报
   "unknown type" 将二次误报。裁定——记录声明尝试前的诊断计数，**仅当本次尝试未
   新增诊断**才报 unknown type，且用**声明起始 token** 的名字（非光标）。
7. **protected 放宽实为 4 处**：补 designated-initializer 路径
   （SemanticAnalyzer.cpp:3600），连同 checkStaticMemberAccess(:202)、字段(:3021)、
   方法(:3119)。`isDerivedFrom` 已有（:5354）。
8. **多行字符串引号剥离**：`decodeEscapes(lex.substr(1, size-2))` 只剥一侧 1 引号，
   `"""..."""` 会残留 2 引号。裁定——lexer 对多行字符串在 token 上打标记
   （`Token::isMultiline` 或复用 literalKind 区分），parser 按 3 引号剥离；
   新增 `Lexer::peekNext2()`。解码仍复用 `decodeEscapes`（剥离后内容）。

用户裁决：反射深度 = **选项 1（全做）**。
