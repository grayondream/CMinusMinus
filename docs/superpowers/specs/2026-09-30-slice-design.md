# TYP-12：Slice 完整实现 — 设计文档

- 日期：2026-09-30
- 状态：已获用户批准的设计（方案 A + 三项冻结决策），待实施
- 关联：TODO `TYP-12`、`P1-01`（最后子项 A）；方案对比与决策过程见本轮 brainstorm 记录

## 1. 背景与目标

Cmm 1.0 已有 slice 的**骨架**：语法 `T[]`（grammar `slice-suffix`，Parser.cpp:1731）、
`SliceType{elementType}`（Type.h:130）、LLVM 表示 `{ptr, i64}`（CodegenContext.cpp:478）。
实证现状（2026-09-30 探查）：

| 场景 | 现状 |
|---|---|
| `int32[] s;` 声明 | 可编译，但值是**垃圾** `{ptr, len}`（无零初始化） |
| `s[0]` 下标 | sema 报 "subscripted value is neither array nor pointer, but 'slice'" |
| `s.len` | sema 报 "member access with '.' requires struct/class/union type" |
| `f(arr)`（`arr` 为 `int32[3]`，形参 `int32[]`） | E2007 无匹配函数（无退化转换） |
| `int32[] b = a;` / `f(b)`（slice→slice） | 可行，但 `getLLVMType` 每次 `StructType::create("Slice")` 会分裂出 `Slice`/`Slice.0` 等不同命名类型——**ABI 类型身份隐患** |

目标：把 slice 变成**最小可用闭环**——下标读写、`.len`、数组→slice 零拷贝退化，
并修复 LLVM 类型身份问题。

## 2. 冻结决策（用户批准）

| # | 决策 | 内容 |
|---|---|---|
| D1 | 范围 | 下标读写 + `.len` + 数组→slice 退化；**不做**范围切片 `arr[1..3]`、显式 slice 构造（YAGNI） |
| D2 | 越界 | **不做边界检查**，越界为 UB（与数组/指针下标现状一致；调试模式检查留待未来） |
| D3 | 默认初始化 | 声明未初始化的 slice 变量**零初始化** `{null, 0}`（空切片）；保留既有 W3001 警告 |
| D4 | 实现路线 | **方案 A**：内联转换点 + 规范 LLVM 类型；不建通用转换矩阵（B）、不暴露 `.ptr`（C） |

## 3. 语法与类型规则

- 语法不变：`T[]` 为 `T` 的 slice（`elementType = T`）。`T` 可为任意完整类型（含 struct/class/union/指针/slice——多维 slice `int32[][]` 天然成立，无需特判）。
- 语义模型：slice = **非拥有、零拷贝视图** `{元素指针, 长度}`，不管理生命周期。
- `.len` 是 slice 的唯一合法成员，类型 `int64`，不可赋值（isLValue=false）。不暴露 `.ptr`（保持视图安全，伪造 `{野指针, len}` 不可能）。

## 4. Sema 规则（SemanticAnalyzer）

### 4.1 退化转换（`T[N]` → `T[]`，单向）

允许发生的位置：

1. **传参**：`conversionRank(argType, paramType)`（Symbol.cpp:193，调用匹配唯一入口）加分支——`from` 为 ArrayType 且 `to` 为 SliceType 且元素类型相同（`typesEqual`，已存在）→ 返回「退化」档位（rank 取值参照既有先例：数组→指针退化为 rank 1，Array→Slice 同样取 1 或并列档位，实施期定）。
2. **赋值/初始化**：`checkAssignmentTypes`（SemanticAnalyzer.cpp:435）加分支——lhs Slice + rhs Array（元素相同）→ 接受，返回 lhs。slice→数组、slice↔数组元素不同 → 现有 E2003 报错。
3. **return**：`visit(ReturnStmtAST)` 走的是 `typesCompatible`（SemanticAnalyzer.cpp:1558，与赋值不同路径）——在该函数加同款 Array→Slice 分支。

反向（slice→数组）、跨元素类型退化**均不允许**。

### 4.2 下标（`visit(ArrayAccessExprAST)`，现 :1309）

- 基表达式类型为 Slice → `node.type = elementType`（剥 typedef），`isLValue = true`（可读可写）。
- 索引仍须整数（现有检查复用）。
- 越界不检查（D2）。错误消息 "subscripted value is neither array nor pointer" 保持现码（E2005 路径），文案微调为 "neither array, slice nor pointer"（对不可下标类型）。

### 4.3 `.len`（`visit(MemberAccessExprAST)`）

- 对象剥 typedef 后为 Slice 且成员名为 `len` → `node.type = int64`，`isLValue = false`。
- 其他成员名 → 报 "no member named 'x' in slice"（沿用现有错误通道，消息针对 slice 特化）。
- struct/class/union 分支不受影响。

### 4.4 类型相等

`getSliceType` 不 intern（Type.cpp:221），两处 `int32[]` 是不同 Type 对象。sema 匹配**不得依赖指针同一性**：`conversionRank`/`typesCompatible` 内已剥 typedef 并有 `typesEqual` 结构相等 helper（Symbol.cpp:195），slice 匹配直接复用，无新增设施。

## 5. Codegen 设计

### 5.1 规范 LLVM 类型（修复类型分裂）

`CodegenContext::getLLVMType` 的 Slice 分支改为**单例**：

```cpp
case TypeKind::Slice: {
    auto* st = llvm::StructType::getTypeByName(*context, "Slice");
    return st ? st : llvm::StructType::create(*context,
        {llvm::PointerType::get(*context, 0), llvm::Type::getInt64Ty(*context)}, "Slice");
}
```

所有 `T[]`（无论元素类型）共用这一个 `{ptr(0), i64}` 命名结构体——slice 的 LLVM 形状与元素类型无关，天然成立。这是对现有 ABI 隐患的修复。

### 5.2 退化值构造（传参/赋值/初始化的 rhs 为数组时）

```
%base = getelementptr [N x T], ptr %arr, i64 0, i64 0   ; 数组首元素地址
%slice = insertvalue {ptr, i64} poison, ptr %base, 0    ; 零拷贝，不复制元素
%slice2 = insertvalue {ptr, i64} %slice, i64 N, 1
```

落点：CallExprAST 实参包装（依据 sema 记录的 resolvedParamTypes/退化标记）、赋值/初始化 Store。实现注意：需确认数组右值在现有 codegen 中产出的是**聚合值还是首元素指针**，据实选择 GEP 形态（设计假定可从 alloca 取首地址；对数组成员 `c.arr`、嵌套数组等表达式，先 materialize 到临时 alloca 再取址，保持实现简单）。

### 5.3 下标

```
%tmp  = load {ptr, i64}, ptr %s            ; slice 变量
%base = extractvalue {ptr, i64} %tmp, 0
%addr = getelementptr T, ptr %base, i64 %i
; 读: %v = load T, ptr %addr    写: store T %v, ptr %addr
```

### 5.4 `.len`

`extractvalue {ptr, i64} %tmp, 1` → i64。

### 5.5 零初始化（D3）

slice 变量声明无初始化 → `store zeroinitializer`（对规范 "Slice" 结构体），即 `{null, 0}` 空切片。

### 5.6 ABI

slice 按值传参/返回复用既有 struct 按值机制（`{ptr, i64}` 16 字节，class Point 按值已验证可走通）。若实测触发 MEM-14 同类缺口（struct 按值失败的通用问题），**在本项内修复该路径**（属于退化传参的必要前提），并在 Progress.md 记录。

## 6. 错误处理汇总

| 场景 | 诊断 |
|---|---|
| 不可下标类型（含 slice 以外的非法基） | 现有 E2005 通道，文案微调 |
| `s.foo`（非 len 成员） | 现有成员错误通道，"no member named 'foo' in slice" |
| slice→数组赋值 / 跨元素类型退化 | 现有 E2003 |
| 退化传参匹配失败 | 现有 E2007（转换档位机制内自然工作） |

不新增诊断码；快照 1 份：`tests/diagnostics/snapshots/slice_member_error.txt`。

## 7. 测试计划（TDD，RED 先行）

1. **e2e**（`tests/e2e/test_slice.cpp`，fixture 参照 test_union.cpp）：
   - 下标读/写经退化传参的 slice（函数内修改可见于调用方数组——验证零拷贝）
   - `.len` 对退化 slice = 静态数组长度；对空切片 = 0
   - 零初始化：`int32[] s;` 后 `s.len == 0`
   - slice→slice 赋值/传参回归
   - runSource 全管线 + LLVM Verifier
2. **sema 单测**（test_semantic_analyzer.cpp，类名/变量名唯一——TypeContext 全局泄漏教训）：
   - `arr` 退化传参 OK；`s[i]` OK；`.len` OK
   - slice→数组赋值拒绝；跨元素类型退化拒绝；`s.foo` 拒绝；`s.len = 1` 拒绝
3. **诊断快照**：slice 成员错误 1 份。
4. **文档**：`docs/spec/conversions.md` 增补退化规则节；grammar.ebnf `slice-suffix` 的 `[impl]` 注释核对（已有）。
5. 全量 `ctest`（基线 751）必须绿。

## 8. 范围外（YAGNI，挂 TODO）

- 范围切片 `arr[1..3]`、显式 slice 构造（字面量/内置函数）
- 边界检查 / 安全模式
- slice 的 `.ptr` 暴露
- 字符串字面量 → `char[]` slice 退化（随字符串子系统再议）

## 9. 开放问题

无——全部决策已冻结（D1–D4）。实现期待确认的两个技术细节（数组右值 codegen 形态、MEM-14 路径是否被触发）已在 §5.2/§5.6 标注处置方式。
