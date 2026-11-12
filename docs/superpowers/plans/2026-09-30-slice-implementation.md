# TYP-12 Slice 完整实现 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `T[]` slice 从「仅可声明的骨架」补成最小可用闭环：下标读写、`.len`、数组→slice 零拷贝退化，并修复 slice LLVM 类型身份分裂。

**Architecture:** 方案 A（spec §2 D4）——在 sema 现有类型匹配落点（`conversionRank` / `typesCompatible` / `checkAssignmentTypes` / 下标 / 成员访问）内联加 Slice 分支；codegen 在退化点就地构造 `{ptr, i64}` 值；`getLLVMType` 的 Slice 分支改为单例命名结构体 `"Slice"`。

**Tech Stack:** C++20、LLVM IR builder（现有 `CodegenContext`）、GoogleTest、ctest。

**Spec:** `docs/superpowers/specs/2026-09-30-slice-design.md`（决策 D1–D4 以 spec 为准）

## Global Constraints

- 全量 `ctest` 基线 **751 通过**；每个任务结束时全量必须绿。
- TDD：每个任务先写 RED 测试，确认失败后实现。
- 越界**不做**边界检查（D2，UB）；不新增诊断码（spec §6）。
- slice 变量声明未初始化 → `{null, 0}` 零初始化（D3），保留 W3001 警告。
- `.len` 类型 `int64`，isLValue=false；不暴露 `.ptr`（D1）。
- 新测试中的类型/变量名不得与其他测试重名（全局 `TypeContext` 单例跨测试泄漏教训）。
- 新增 `tests/e2e/*.cpp` 后必须 `cd build && cmake .` 重新 configure（GLOB_RECURSE 拾取）。
- 构建命令：`cd build && cmake --build . -j$(nproc)`；聚焦跑测：`./bin/compiler_tests --gtest_filter='<Filter>'`。
- 所有回答与记录用中文；代码注释/标识符遵循仓库现状（英文）。

## Review Focus

- **跨元素类型 slice 互兼容**（现状 bug）：`int32[]` 与 `float64[]` 因 `kind == kind` 分支互相赋值/传参——Task 5 收紧后应报错，测试钉住。
- **全局 slice 变量未初始化**：期望零值 `{null, 0}`（LLVM 全局默认零，但需测试证明不被别的路径破坏）——Task 6 钉住。
- **`.len` 用于非 slice**（如 `int32 x; x.len`）：期望走成员错误通道报错而非崩溃——Task 3 钉住。
- **零拷贝语义**：退化传参后函数内写 `s[i]` 必须影响调用方数组——Task 4 e2e 钉住。
- **嵌套 slice `int32[][]`**：声明 + `getLLVMType` 单例不互相污染——Task 1 / Task 2 覆盖。

---

### Task 1: 规范 Slice LLVM 类型（单例命名结构体）

**Files:**
- Modify: `src/codegen/CodegenContext.cpp:478-484`（getLLVMType 的 `TypeKind::Slice` 分支）
- Test: `tests/e2e/test_slice.cpp`（新建）

**Interfaces:**
- Consumes: 既有 `ctx.getLLVMType(Type*)`。
- Produces: 所有 `T[]` 共享单个 `llvm::StructType`（名字 `"Slice"`，body `{ptr(0), i64}`）；后续任务的 GEP/extractvalue 都基于此形状。

- [ ] **Step 1: 写 RED 测试（新建文件，fixture 从 tests/e2e/test_union.cpp 头部复制，fixture 类名改 `SliceE2ETest`，runSource 原样保留）**

```cpp
// TYP-12 Task 1: slice values cross function boundaries with one canonical
// LLVM type; the LLVM Verifier must accept the module.
TEST_F(SliceE2ETest, SlicePassingVerifierClean) {
    EXPECT_EQ(runSource(R"(
        int32 probe(int32[] s) { return 0; }
        int32 main() {
            int32[] a;
            int32[] b = a;
            return probe(a) + probe(b);
        }
    )", "test_slice_canonical.c"), 0);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cd build && cmake . >/dev/null && cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceE2ETest.*'`
Expected: 编译通过、测试可能意外 PASS（现状 `{ptr,i64}` 形状恰好兼容）或 FAIL——无论结果，先记录现状（诊断单例是否分裂：跑一次后用 `llvm-dis` 或在 getLLVMType 临时打点均可，不打点则跳过观察直接实施）。此测试的价值在 Verifier 长期护栏。

- [ ] **Step 3: 实现单例（替换现有 create 分支）**

```cpp
case TypeKind::Slice: {
    // TYP-12: all slices share ONE canonical named struct {ptr, i64}; the
    // element type does not affect the LLVM shape. Per-call create() split
    // the type into "Slice", "Slice.0", ... breaking type identity in calls.
    auto* st = llvm::StructType::getTypeByName(*context, "Slice");
    return st ? st
              : llvm::StructType::create(*context,
                    {llvm::PointerType::get(*context, 0),
                     llvm::Type::getInt64Ty(*context)},
                    "Slice");
}
```

- [ ] **Step 4: 确认 GREEN + 全量回归**

Run: `cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceE2ETest.*' && ctest`
Expected: SliceE2ETest PASS；ctest 752 通过（751 + 1）。

- [ ] **Step 5: Commit**

```bash
git add src/codegen/CodegenContext.cpp tests/e2e/test_slice.cpp
git commit -m "feat(TYP-12): slice 规范 LLVM 类型单例，修复类型分裂"
```

---

### Task 2: slice 下标读写 `s[i]`

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp:1309-1323`（`visit(ArrayAccessExprAST)`）
- Modify: `src/ast/Expr.cpp:810-833`（`ArrayAccessExprAST::codegen`）
- Test: `tests/e2e/test_slice.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 1 的 `{ptr, i64}` 规范类型；`ctx.getLLVMType(elemType)`。
- Produces: `s[i]` 为 lvalue（地址），赋值走既有 AssignmentExpr 机制。

- [ ] **Step 1: 写 RED 测试**

sema（`tests/sema/test_semantic_analyzer.cpp` 追加；本任务的独立 e2e 验证依赖 Task 4/5 的退化才能构造 slice 值，故 e2e 下标断言由 Task 4/5 落位并转 GREEN）：

```cpp
// TYP-12 Task 2: subscript on slice yields the element type and is an lvalue.
TEST(SliceSemTest, SubscriptOnSliceAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 pick(int32[] s) { return s[0]; } "
        "int32 main() { return 0; }"));
}

TEST(SliceSemTest, SubscriptOnIntRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32 x = 1; return x[0]; }"));
}

// Review Focus #5: nested slices parse and type-check through one layer.
TEST(SliceSemTest, NestedSliceSubscriptAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 pick(int32[][] grid) { return grid[0][0]; } "
        "int32 main() { return 0; }"));
}
```

- [ ] **Step 2: 确认 RED**

Run: `cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceSemTest.*'`
Expected: `SubscriptOnSliceAccepted` FAIL（"subscripted value is neither array nor pointer"）；`SubscriptOnIntRejected` PASS（现状已拒绝）。

- [ ] **Step 3: sema 实现（`visit(ArrayAccessExprAST)` 的 if/else 链加 slice 分支）**

在 `TypeKind::Pointer` 分支之后加：

```cpp
} else if (arrayType && arrayType->kind == TypeKind::Slice) {
    // TYP-12: slicing yields an lvalue of the element type.
    node.type = static_cast<SliceType*>(arrayType)->elementType;
```

并把兜底错误文案改为 `"subscripted value is neither array, slice nor pointer, but '"`。

- [ ] **Step 4: codegen 实现（`ArrayAccessExprAST::codegen` 的 elemTy 判定链加分支）**

在 Pointer 分支后加（`arrVal` 此刻是 slice 变量地址，需先 load 结构体再取 ptr 字段）：

```cpp
} else if (arrType && arrType->kind == TypeKind::Slice) {
    auto* sliceLLVM = ctx.getLLVMType(arrType);
    auto& builder = ctx.getBuilder();
    if (array->isLValue) arrVal = builder.CreateLoad(sliceLLVM, arrVal);
    llvm::Value* base = builder.CreateExtractValue(arrVal, {0});
    if (arrType->elementType) elemTy = ctx.getLLVMType(arrType->elementType);
    arrVal = base; // fall through to the shared GEP below
}
```

（GEP 结果是元素地址，函数返回地址、lvalue 加载由既有 `emitRValue`/调用方机制处理——与数组分支同构。）

- [ ] **Step 5: 确认 GREEN + 全量**

Run: `cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceSemTest.*' && ctest`
Expected: 755 通过（752 + 3）。

- [ ] **Step 6: Commit**

```bash
git add src/sema/SemanticAnalyzer.cpp src/ast/Expr.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(TYP-12): slice 下标读写（sema + codegen）"
```

---

### Task 3: `.len` 成员 + 错误快照

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（`visit(MemberAccessExprAST)`，struct/class/union 分支旁）
- Modify: `src/ast/Expr.cpp:868`（`MemberAccessExprAST::codegen`）
- Test: `tests/sema/test_semantic_analyzer.cpp`、`tests/diagnostics/snapshots/slice_member_error.txt`（新建，由 `CDD_UPDATE_SNAPSHOTS=1` 生成）

**Interfaces:**
- Consumes: Task 1 规范类型（extractvalue 索引 1 = i64 长度）。
- Produces: `.len` → `int64`、isLValue=false。

- [ ] **Step 1: 写 RED 测试**

sema 追加：

```cpp
// TYP-12 Task 3: slice .len member; only member allowed on a slice.
TEST(SliceSemTest, LenMemberAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int64 n(int32[] s) { return s.len; } "
        "int32 main() { return 0; }"));
}

TEST(SliceSemTest, LenOnNonSliceRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32 x = 1; return x.len; }"));
}

TEST(SliceSemTest, SliceOtherMemberRejected) {
    EXPECT_FALSE(analyzeOk(
        "int64 n(int32[] s) { return s.ptr; } "
        "int32 main() { return 0; }"));
}
```

- [ ] **Step 2: 确认 RED**

Run: `cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceSemTest.*'`
Expected: `LenMemberAccepted` FAIL（member access 错误）；两个 Rejected PASS。

- [ ] **Step 3: sema 实现（`visit(MemberAccessExprAST)` 在 union 分支前加 Slice 分支）**

```cpp
} else if (strippedObj && strippedObj->kind == TypeKind::Slice) {
    // TYP-12: `.len` is the only member of a slice (D1: no `.ptr`).
    if (node.memberName == "len") {
        node.type = TypeContext::instance().getInt64();
        node.isLValue = false;
        return;
    }
    emitError("no member named '" + node.memberName + "' in slice", node);
    node.type = nullptr;
    node.isLValue = false;
    return;
}
```

（`getInt64` 的实际访问器名以 `TypeContext` 现有成员为准，grep 后对齐。）

- [ ] **Step 4: codegen 实现（`MemberAccessExprAST::codegen` 入口处、struct 分支前加 slice 短路）**

```cpp
{
    Type* t = node.object->type;
    while (t && t->kind == TypeKind::Typedef) t = static_cast<TypedefType*>(t)->aliasedType;
    if (t && t->kind == TypeKind::Slice && node.memberName == "len") {
        llvm::Value* objVal = node.object->codegen(ctx);
        if (!objVal) return nullptr;
        auto& builder = ctx.getBuilder();
        if (node.object->isLValue) objVal = builder.CreateLoad(ctx.getLLVMType(t), objVal);
        return builder.CreateExtractValue(objVal, {1});
    }
}
```

- [ ] **Step 5: 诊断快照**

`tests/sema/test_diagnostic_snapshot.cpp` 追加（照 `PrivateMemberAccess` 模式）：

```cpp
TEST_F(DiagnosticSnapshotTest, SliceMemberError) {
    expectSnapshot("slice_member_error", analyzeDiagnostics(R"(
int32 main() {
    int32[] s;
    return s.ptr;
}
)"));
}
```

Run: `cd build && cmake --build . -j$(nproc) && CDD_UPDATE_SNAPSHOTS=1 ./bin/compiler_tests --gtest_filter='*Snapshot*SliceMemberError*'`
Expected: 生成 `tests/diagnostics/snapshots/slice_member_error.txt`，内容含 `error[E2xxx]` 通道行（码以实际 emit 通道为准）；再跑一次不带环境变量确认 PASS。

- [ ] **Step 6: 确认 GREEN + 全量**

Run: `ctest`
Expected: 759 通过（755 + 3 sema + 1 快照）。

- [ ] **Step 7: Commit**

```bash
git add src/sema/SemanticAnalyzer.cpp src/ast/Expr.cpp tests/sema/test_semantic_analyzer.cpp tests/sema/test_diagnostic_snapshot.cpp tests/diagnostics/snapshots/slice_member_error.txt
git commit -m "feat(TYP-12): slice .len 成员与错误快照"
```

---

### Task 4: 数组→slice 传参退化（零拷贝）

**Files:**
- Modify: `src/ast/Symbol.cpp:193`（`conversionRank`）、必要时 `src/ast/Symbol.h:30`（导出 `typesEqual`，若未导出）
- Modify: `src/ast/Expr.cpp:584-596`（`CallExprAST::codegen` 实参循环）、`src/ast/Expr.cpp:611`（`MethodCallExprAST::codegen` 实参循环，同款处理）
- Test: `tests/e2e/test_slice.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 1 规范类型；`conversionRank` 既有「数组→指针 rank 1」先例（Symbol.cpp:203）；`typesEqual`（Symbol.cpp 内已有定义，先 grep Symbol.h 确认是否已声明，未声明则补声明）。
- Produces: 静态辅助 `emitArrayToSliceDecay(CodegenContext&, llvm::Value* arrayAddr, ArrayType*)`（Expr.cpp 文件内 static），返回 `{ptr, N}` 值——Task 5 复用。

- [ ] **Step 1: 写 RED 测试**

sema：

```cpp
// TYP-12 Task 4: T[N] decays to T[] at call sites (zero-copy).
TEST(SliceSemTest, ArrayDecaysToSliceParam) {
    EXPECT_TRUE(analyzeOk(
        "int32 total(int32[] s) { return 0; } "
        "int32 main() { int32 arr[3] = {1,2,3}; return total(arr); }"));
}

TEST(SliceSemTest, CrossElementDecayRejected) {
    EXPECT_FALSE(analyzeOk(
        "int64 total(float64[] s) { return 0; } "
        "int32 main() { int32 arr[3] = {1,2,3}; return total(arr); }"));
}
```

e2e（零拷贝语义钉住，Review Focus 第 4 条）：

```cpp
// TYP-12 Task 4: writes through a decayed slice are visible to the caller
// (the slice views the original array, no copy).
TEST_F(SliceE2ETest, DecayedSliceIsZeroCopy) {
    EXPECT_EQ(runSource(R"(
        void bump(int32[] s) { s[0] = s[0] + 100; }
        int32 main() {
            int32 arr[2] = {1, 2};
            bump(arr);
            return arr[0] == 101 ? 0 : 1;
        }
    )", "test_slice_decay.c"), 0);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceSemTest.*:SliceE2ETest.DecayedSliceIsZeroCopy'`
Expected: `ArrayDecaysToSliceParam` FAIL（E2007）；e2e FAIL（同一 E2007 在 runSource 里返回 -1）。

- [ ] **Step 3: `conversionRank` 加分支（Symbol.cpp，紧跟「数组→指针」分支之后）**

```cpp
// TYP-12: an array decays to a slice view of itself (zero-copy).
if (from->kind == TypeKind::Array && to->kind == TypeKind::Slice &&
    typesEqual(static_cast<ArrayType*>(from)->elementType,
               static_cast<SliceType*>(to)->elementType)) return 1;
```

（`Symbol.h` 若未声明 `typesEqual` 则补 `bool typesEqual(Type*, Type*);` 声明。）

- [ ] **Step 4: codegen 退化包装（Expr.cpp 文件内 static helper + 两处实参循环接入）**

```cpp
// TYP-12: build {ptr, len} view over a statically-sized array (zero-copy).
static llvm::Value* emitArrayToSliceDecay(CodegenContext& ctx,
                                          llvm::Value* arrayAddr,
                                          ArrayType* arrayType) {
    auto& builder = ctx.getBuilder();
    llvm::Type* sliceLLVM = ctx.getLLVMType(
        TypeContext::instance().getSliceType(arrayType->elementType));
    llvm::Value* base = arrayAddr; // array operands already yield the
                                   // address of their first element
    llvm::Value* v = llvm::Constant::getNullValue(sliceLLVM);
    v = builder.CreateInsertValue(v, base, {0});
    return builder.CreateInsertValue(
        v, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()),
                                  static_cast<uint64_t>(arrayType->size)), {1});
}
```

接入点（两处同款）：实参循环内 `castValue` 之前——

```cpp
Type* rawArg = args[i]->type;      // MethodCall 同理
while (rawArg && rawArg->kind == TypeKind::Typedef)
    rawArg = static_cast<TypedefType*>(rawArg)->aliasedType;
if (i < resolvedParamTypes.size() && rawArg && rawArg->kind == TypeKind::Array) {
    Type* rawParam = resolvedParamTypes[i];
    while (rawParam && rawParam->kind == Typedef 剥离同款);
    if (rawParam && rawParam->kind == TypeKind::Slice) {
        argVal = emitArrayToSliceDecay(ctx, argVal, static_cast<ArrayType*>(rawArg));
        argsV.push_back(argVal);
        continue;
    }
}
```

注意：数组实参经 `emitLoad` 后已是首元素地址（Expr.cpp:810 注释先例），若实测为聚合值则先 `CreateAlloca` + `CreateStore` materialize 再取址（spec §5.2 预案）。`ArrayType` 的长度字段名以 `src/ast/Type.h:61` 定义为准（grep 对齐后替换 `->size`）。

- [ ] **Step 5: 确认 GREEN + 全量**

Run: `ctest`
Expected: 762 通过（759 + 2 sema + 1 e2e）。

- [ ] **Step 6: Commit**

```bash
git add src/ast/Symbol.cpp src/ast/Symbol.h src/ast/Expr.cpp tests/e2e/test_slice.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(TYP-12): 数组→slice 传参零拷贝退化"
```

---

### Task 5: 赋值/初始化退化 + 跨元素类型收紧

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp:237`（`typesCompatible`）、`src/sema/SemanticAnalyzer.cpp:435`（`checkAssignmentTypes`）
- Modify: `src/ast/Expr.cpp:691`（`AssignmentExprAST::codegen`）、`src/ast/Decl.cpp:215`（`VarDeclAST::codegen` 初始化路径）
- Test: `tests/e2e/test_slice.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 4 的 `emitArrayToSliceDecay`；`typesEqual`。
- Produces: `T[] b = arr;`、`b = arr;`、`return arr;`（slice 返回型函数）全线可用；slice 同型比较收紧为「元素类型相等」。

- [ ] **Step 1: 写 RED 测试**

sema：

```cpp
// TYP-12 Task 5: array->slice decay in assignment/init/return; cross-element
// slice compatibility is rejected (Review Focus #1).
TEST(SliceSemTest, ArrayToSliceAssignmentAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 main() { int32 arr[3] = {1,2,3}; int32[] s = arr; return 0; }"));
}

TEST(SliceSemTest, SliceToArrayRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32[] s; int32 arr[3] = {1,2,3}; arr = s; return 0; }"));
}

TEST(SliceSemTest, CrossElementSliceAssignmentRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { int32[] a; float64[] b = a; return 0; }"));
}

TEST(SliceSemTest, ArrayReturnedAsSliceAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32[] pick(int32 arr[2]) { return arr; } "
        "int32 main() { int32 a[2] = {1,2}; return 0; }"));
}
```

e2e（Task 2 预留下标断言此时落位转 GREEN；`SubscriptReadWrite` 在 Task 2 已确认因退化缺失而 RED）：

```cpp
// TYP-12 Task 5: subscript read + write through a decayed slice.
TEST_F(SliceE2ETest, SubscriptReadWrite) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = {7, 8, 9};
            int32[] s = arr;
            return s[1] == 8 ? 0 : 1;
        }
    )", "test_slice_subscript.c"), 0);
}

// TYP-12 Task 5: decay through plain assignment too.
TEST_F(SliceE2ETest, DecayThroughAssignment) {
    EXPECT_EQ(runSource(R"(
        int32 main() {
            int32 arr[3] = {7, 8, 9};
            int32[] s = arr;
            s[2] = 42;
            return arr[2] == 42 && s[0] == 7 ? 0 : 1;
        }
    )", "test_slice_assign.c"), 0);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceSemTest.*:SliceE2ETest.DecayThroughAssignment:SliceE2ETest.SubscriptReadWrite'`
Expected: `ArrayToSliceAssignmentAccepted`、`ArrayReturnedAsSliceAccepted`、`CrossElementSliceAssignmentRejected`、两个 e2e FAIL；`SliceToArrayRejected` 现状可能 PASS（kind 不同即拒），保持。

- [ ] **Step 3: sema 实现**

`typesCompatible`（:237）在数组→指针分支旁加：

```cpp
// TYP-12: arrays decay to slices; slices must match by element type.
if (left->kind == TypeKind::Slice && right->kind == TypeKind::Slice)
    return typesEqual(static_cast<SliceType*>(left)->elementType,
                      static_cast<SliceType*>(right)->elementType);
if (left->kind == TypeKind::Slice && right->kind == TypeKind::Array)
    return typesEqual(static_cast<SliceType*>(left)->elementType,
                      static_cast<ArrayType*>(right)->elementType);
```

`checkAssignmentTypes`（:435）在 `lhsS->kind == rhsS->kind` 分支**之前**加：

```cpp
// TYP-12: slice targets must match by element type (tightens the generic
// same-kind rule that silently accepted int32[] = float64[]).
if (lhsS->kind == TypeKind::Slice && rhsS->kind == TypeKind::Slice)
    return typesEqual(static_cast<SliceType*>(lhsS)->elementType,
                      static_cast<SliceType*>(rhsS)->elementType) ? lhsRaw : nullptr;
if (lhsS->kind == TypeKind::Slice && rhsS->kind == TypeKind::Array)
    return typesEqual(static_cast<SliceType*>(lhsS)->elementType,
                      static_cast<ArrayType*>(rhsS)->elementType) ? lhsRaw : nullptr;
```

（返回 nullptr 走既有 E2003 通道。）

- [ ] **Step 4: codegen 实现**

`AssignmentExprAST::codegen`（:691）与 `VarDeclAST::codegen` 初始化路径（Decl.cpp:261 附近的 `initVal` 处理）：store 前若 RHS 是数组而 LHS 类型是 slice → `argVal = emitArrayToSliceDecay(ctx, rhsAddr, static_cast<ArrayType*>(rhsType))`（剥 typedef 后判定；`return` 经 `ReturnStmtAST` codegen 复用 VarDecl 同款包装——在 ReturnStmt 的值处理处加同款三行判定）。

- [ ] **Step 5: 确认 GREEN + 全量**

Run: `ctest`
Expected: 768 通过（762 + 4 sema + 2 e2e，其中 `SubscriptReadWrite` 由 RED 转 GREEN）。

- [ ] **Step 6: Commit**

```bash
git add src/sema/SemanticAnalyzer.cpp src/ast/Expr.cpp src/ast/Decl.cpp tests/e2e/test_slice.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(TYP-12): 赋值/初始化/返回退化 + 跨元素 slice 收紧"
```

---

### Task 6: slice 零初始化（D3）

**Files:**
- Modify: `src/ast/Decl.cpp:241-247`（`VarDeclAST::codegen` 局部 alloca 路径）
- Test: `tests/e2e/test_slice.cpp`

**Interfaces:**
- Consumes: Task 1 规范类型（`zeroinitializer` 即 `{null, 0}`）。
- Produces: 无（行为修复）。

- [ ] **Step 1: 写 RED 测试**

```cpp
// TYP-12 D3: an uninitialized slice variable is the empty slice {null, 0}.
TEST_F(SliceE2ETest, UninitializedSliceIsEmpty) {
    EXPECT_EQ(runSource(R"(
        int64 probe(int32[] s) { return s.len; }
        int32 main() {
            int32[] s;
            if (s.len != 0) return 1;
            return probe(s) == 0 ? 0 : 2;
        }
    )", "test_slice_zero.c"), 0);
}

// Review Focus #2: the same zero-init holds for globals.
TEST_F(SliceE2ETest, GlobalSliceZeroInitialized) {
    EXPECT_EQ(runSource(R"(
        int32[] g;
        int32 main() { return g.len == 0 ? 0 : 1; }
    )", "test_slice_global.c"), 0);
}
```

- [ ] **Step 2: 确认 RED**

Run: `cmake --build . -j$(nproc) && ./bin/compiler_tests --gtest_filter='SliceE2ETest.UninitializedSliceIsEmpty:SliceE2ETest.GlobalSliceZeroInitialized'`
Expected: `UninitializedSliceIsEmpty` FAIL（垃圾 len）；Global 版现状大概率 PASS（LLVM 全局零值默认），记录并保留为护栏。

- [ ] **Step 3: 实现（alloca 后、initExpr 分支前加）**

```cpp
// TYP-12 D3: uninitialized slices are the empty slice {null, 0}.
{
    Type* st = stripTypedefs(type);
    if (st && st->kind == TypeKind::Slice)
        ctx.getBuilder().CreateStore(llvm::Constant::getNullValue(llvmType), alloca);
}
```

（`stripTypedefs` 以 Decl.cpp 既有可见 helper 为准，grep 对齐。）

- [ ] **Step 4: 确认 GREEN + 全量**

Run: `ctest`
Expected: 770 通过（768 + 2）。

- [ ] **Step 5: Commit**

```bash
git add src/ast/Decl.cpp tests/e2e/test_slice.cpp
git commit -m "feat(TYP-12): slice 变量零初始化为空切片"
```

---

### Task 7: 文档收尾 + backlog 更新 + 全量回归

**Files:**
- Modify: `docs/spec/conversions.md`（新增退化规则节）
- Modify: `TODO.md`（TYP-12 置 `[~]` 或 `[x]`、P1-01 更新）
- Modify: `Progress.md`（追加记录）
- Test: 全量 `ctest`

- [ ] **Step 1: conversions.md 增补**

新增节「数组→Slice 退化」：单向 `T[N]` → `T[]`；发生位置（传参/赋值/初始化/return）；零拷贝语义（视图，写入可见）；越界 UB（D2）；零初始化 `{null, 0}`（D3）；`.len` 唯一成员；范围内不含范围切片/显式构造（D1，挂 TODO）。

顺带核对 `docs/spec/grammar.ebnf` 的 `slice-suffix = '[' ']'` 条目与 `[impl]` 注释是否与实现一致（应已一致，仅核对）。

- [ ] **Step 2: TODO.md**

- `TYP-12` 置 `[x]`（或 `[~]` + 范围外清单引用 spec §8）。
- `P1-01` 描述更新：Slice 已完成，剩余项清单同步。
- 范围外项若需挂靠，新增条目引用 spec `docs/superpowers/specs/2026-09-30-slice-design.md` §8。

- [ ] **Step 3: 全量回归**

Run: `cd build && ctest`
Expected: 770/770 通过。

- [ ] **Step 4: Progress.md 追加 + Commit**

```bash
git add docs/spec/conversions.md TODO.md Progress.md
git commit -m "docs(TYP-12): slice 退化规则文档与 backlog 收尾"
```
