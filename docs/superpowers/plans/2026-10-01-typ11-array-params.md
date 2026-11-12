# TYP-11 数组形参去糖 Slice 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `T name[N]` 一维数组形参去糖为 slice `T[]`（零拷贝，与 `int32[] s` 形参等价），并封堵"返回局部数组视图"悬垂 UB。

**Architecture:** 纯 parser 层去糖（`parseParamDecl`），sema/codegen/mangle 零改动——形参成为 SliceType 后全部走 TYP-12 已验证通道。悬垂封堵是 `visit(ReturnStmtAST)` 前置守卫（D4）。

**Tech Stack:** C++20 / LLVM / gtest（既有编译器代码库）

**Spec:** `docs/superpowers/specs/2026-10-01-typ11-array-params-design.md`（D1–D4 冻结决策）

## Global Constraints

- 基线 **772/772** 全绿；每任务结束全量 `ctest` 必须绿。
- TDD：RED 先行，未亲见失败不写实现。
- 新增 `tests/e2e/*.cpp` 后必须 `cd build && cmake ..` 重新 configure（GLOB_RECURSE）。
- 全局 `TypeContext` 单例跨测试泄漏：测试内函数/类型名必须唯一（新前缀 `AP`，如 `APSum`）。
- 诊断用 `emitError` 无码通道（与 struct no-member 一致）；错误文案逐字使用 spec §3.2 的 "cannot return a slice view of a local array (dangling view)"。
- 提交信息中文，格式 `feat/fix/docs(TYP-11): 描述`。
- `git add -f` 用于 gitignore 目录（Progress.md 惯例）。

## Review Focus

1. **`[N]` 长度不检查**：`int32 sum(int32 a[2])` 传长度 5 的数组应接受（长度动态）→ Task 3 e2e `ArrayParamAnyLengthAccepted`。
2. **多维形参清晰诊断**：`int32 f(int32 a[2][3])` 应报 "multi-dimensional array parameters are not supported" 而非 `expected ')'` → Task 2 `MultiDimArrayParamRejected`。
3. **守卫不误伤 slice 形参 return**：`int32[] pick(int32 arr[2]) { return arr; }` 仍接受（形参已去糖为 slice）→ Task 3 e2e `ArrayParamReturnSlice`。
4. **函数指针形参同享去糖**：`apply(int32 cb(int32 a[2]))` 解析通过（`parseParamDecl` 为两处共用）→ Task 2 `FunctionPointerArrayParamDesugared`。
5. **无名数组形参**：`int32 f(int32[2])` 现状报错（`[2]` 无标识符可挂）→ 保持报错即可，Task 2 `UnnamedArrayParamRejected` 钉住。

---

### Task 1: 悬垂封堵（D4）

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp:1586`（`visit(ReturnStmtAST)`）
- Test: `tests/sema/test_semantic_analyzer.cpp`（文件尾追加）
- Test: `tests/sema/test_diagnostic_snapshot.cpp`（`SliceMemberError` 用例后插入）

**Interfaces:**
- Consumes: `currentFunction->returnType`（`Type*`）、`TypedefType::aliasedType`（`Type.h:121`）、`emitError(msg, node)`。
- Produces: 无下游消费者；本任务独立交付。

- [ ] **Step 1: 写失败测试**（`test_semantic_analyzer.cpp` 尾部追加）

```cpp
// TYP-11 D4: returning a view of a local array dangles — rejected.
TEST(SliceSemTest, DanglingLocalArrayReturnRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32[] bad() { int32 a[2] = {1,2}; return a; } "
        "int32 main() { return 0; }"));
}
```

快照（`test_diagnostic_snapshot.cpp`，在 `TEST_F(DiagnosticSnapshotTest, SliceMemberError)` 之前插入）：

```cpp
TEST_F(DiagnosticSnapshotTest, LocalArrayReturnDangling) {
    expectSnapshot("local_array_return_dangling", analyzeDiagnostics(R"(
int32[] bad() {
    int32 a[2] = {1, 2};
    return a;
}
)"));
}
```

- [ ] **Step 2: 跑测试确认失败**

`./bin/compiler_tests --gtest_filter='SliceSemTest.DanglingLocalArrayReturnRejected:*LocalArrayReturnDangling'`
Expected: 两项 FAIL（现状 sema 接受悬垂返回；快照文件不存在）。

- [ ] **Step 3: 实现守卫**（`visit(ReturnStmtAST)` 内，`typesCompatible` 检查之前）

去 typedef 后：返回类型 kind == Slice 且返回表达式类型 kind == Array → `emitError("cannot return a slice view of a local array (dangling view)", node)` 并 return。typedef 剥离用内联 while 循环（`TypedefType::aliasedType`；sema 无共享 stripTypedefs helper，勿引 Decl.cpp 的 static）。

- [ ] **Step 4: 跑测试确认通过 + 全量**

`ctest` Expected: 774/774（快照文件 `tests/diagnostics/snapshots/local_array_return_dangling.txt` 用 `CDD_UPDATE_SNAPSHOTS=1` 生成后确认内容含错误行）。

- [ ] **Step 5: 提交**

```bash
git add src/sema/SemanticAnalyzer.cpp tests/sema/test_semantic_analyzer.cpp tests/sema/test_diagnostic_snapshot.cpp tests/diagnostics/snapshots/local_array_return_dangling.txt
git commit -m "fix(TYP-11): 拒绝返回局部数组的 slice 视图（悬垂封堵 D4）"
```

### Task 2: 一维数组形参去糖（D2/D3）

**Files:**
- Modify: `src/frontend/Parser.cpp:2251-2263`（`parseParamDecl`）
- Test: `tests/sema/test_semantic_analyzer.cpp`（尾部追加）

**Interfaces:**
- Consumes: `match(TOKEN_NUMBER)`（先例 `parseVariableDecl` :2218）、`expect`/`errorUnexpected`、`TypeContext::instance().getSliceType(Type*)`（`Type.h:242`；Parser.cpp 已 include/使用）。
- Produces: 形参类型为 SliceType 的 `ParamDeclAST`——sema/codegen/mangle 不感知，无需改动。

- [ ] **Step 1: 写失败测试**（尾部追加，5 项）

```cpp
// TYP-11 D2: T name[N] parameter desugars to slice T[].
TEST(SliceSemTest, ArrayParamAccepted) {
    EXPECT_TRUE(analyzeOk(
        "int32 APSum(int32 a[2]) { return a[0] + a[1]; } "
        "int32 main() { int32 arr[2] = {3,4}; return APSum(arr); }"));
}

TEST(SliceSemTest, CrossElementArrayParamRejected) {
    EXPECT_FALSE(analyzeOk(
        "int64 APF(float64 a[2]) { return 0; } "
        "int32 main() { int32 arr[2] = {3,4}; return APF(arr); }"));
}

TEST(SliceSemTest, FunctionPointerArrayParamDesugared) {
    EXPECT_TRUE(analyzeOk(
        "int32 APUse(int32 cb(int32 a[2])) { return 0; } "
        "int32 main() { return 0; }"));
}

TEST(SliceSemTest, MultiDimArrayParamRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { return 0; } "
        "int32 APBad(int32 a[2][3]) { return 0; }"));
}

TEST(SliceSemTest, UnnamedArrayParamRejected) {
    EXPECT_FALSE(analyzeOk(
        "int32 main() { return 0; } "
        "int32 APAnon(int32[2]) { return 0; }"));
}
```

- [ ] **Step 2: 跑测试确认失败**

`./bin/compiler_tests --gtest_filter='SliceSemTest.ArrayParamAccepted:SliceSemTest.CrossElementArrayParamRejected:SliceSemTest.FunctionPointerArrayParamDesugared:SliceSemTest.MultiDimArrayParamRejected:SliceSemTest.UnnamedArrayParamRejected'`
Expected: `ArrayParamAccepted`/`CrossElementArrayParamRejected`/`FunctionPointerArrayParamDesugared` FAIL（现状 `[2]` 解析报错被 analyzeOk 忽略 → 实为 parse 失败；若意外 PASS 用 CLI `./build/bin/cdd` 复核 parse 层）；`MultiDimArrayParamRejected`/`UnnamedArrayParamRejected` PASS（现状即报错，钉住）。

- [ ] **Step 3: 实现去糖**（`parseParamDecl`，仅在有标识符时）

标识符消耗后：遇 `[` → 消耗；`match(TOKEN_NUMBER)` 丢弃；`expect(']', "expected ']' after array parameter size")`；`type = TypeContext::instance().getSliceType(type)`；随后再遇 `[` → `errorUnexpected("multi-dimensional array parameters are not supported")` 返回 nullptr。无名形参路径不变。

- [ ] **Step 4: 跑测试确认通过 + 全量**

`ctest` Expected: 779/779。

- [ ] **Step 5: 提交**

```bash
git add src/frontend/Parser.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(TYP-11): 一维数组形参 T name[N] 去糖为 slice"
```

### Task 3: e2e 全套 + 文档收尾

**Files:**
- Create: `tests/e2e/test_array_params.cpp`（fixture 复制 `tests/e2e/test_slice.cpp`，fixture 名 `ArrayParamsE2E`，`runSource(source, filename)` 签名照旧）
- Modify: `docs/spec/conversions.md` §10、`TODO.md`（TYP-11 条目与 P1-01 条目）

**Interfaces:**
- Consumes: Task 2 去糖形参（SliceType ParamDeclAST）；TYP-12 的 `emitArrayToSliceDecay`、`.len`、下标读写。

- [ ] **Step 1: 写失败 e2e 测试**（4 项，函数名唯一前缀 AP）

1. `ArrayParamSum`：`int32 APSum(int32 a[2]) { return a[0] + a[1]; }`，main 传 `{3,4}` → 0。
2. `ArrayParamAnyLengthAccepted`（Review Focus 1）：形参 `int32 a[2]`，实参数组 `int32 big[5] = {1,2,3,4,5}`，`APSum(big)` 前两元素和 → 0。
3. `ArrayParamWriteThrough`（零拷贝）：`void APBump(int32 a[2]) { a[0] = a[0] + 100; }`，调用后原数组 `arr[0] == 101` → 0。
4. `ArrayParamReturnSlice`（Review Focus 3）：`int32[] APPick(int32 arr[2]) { return arr; }`，main 中 `s[1] == 8 && s.len == 2` → 0。

- [ ] **Step 2: configure + 构建确认失败**

`cd build && cmake .. && cmake --build . -j$(nproc)` 后跑 `ArrayParamsE2E.*`。
Expected: 4 项全 FAIL（去糖前 parse 失败）。

- [ ] **Step 3: 确认随 Task 2 转绿**（实现已在 Task 2 落地；此步仅验证 e2e 层）

Expected: 4 项全 OK。若某项 FAIL → 回到 Task 2 实现排查（不写新实现代码）。

- [ ] **Step 4: 全量 + 文档**

全量 `ctest` Expected: 783/783。

`conversions.md` §10 中"`return` 的退化依赖形参数组语法 `T name[N]`（TYP-11），当前未落地"改为："形参数组 `T name[N]` 已去糖为 slice（TYP-11），形参位置无 ArrayType；return 表达式为局部数组时被拒绝（悬垂 D4）"。`TODO.md`：TYP-11 条目标注"一维数组形参去糖已完成（2026-10-01）；多维数组本体/多维形参/VLA 待补"；同步消除 P1-01 条目中 Slice 相关遗留表述。

- [ ] **Step 5: 提交**

```bash
git add tests/e2e/test_array_params.cpp docs/spec/conversions.md TODO.md
git commit -m "feat(TYP-11): 数组形参 e2e 钉住与文档收尾"
```

## Self-Review 记录

- Spec 覆盖：§3.1→Task 2；§3.2→Task 1；§4.1→Task 2 测试 1/2；§4.2→Task 1；§4.3→Task 3；§5→Task 3 Step 4；§6 不实现。无缺口。
- Review Focus 5 条全部有钉住测试（1→Task3.2、2/4/5→Task2、3→Task3.4）。
- 计数核对：772 + 2（Task1）+ 5（Task2）+ 4（Task3）= 783。
- 类型一致性：`getSliceType(Type*)`（Type.h:242）、`emitError(msg, node)` 均与现状签名一致。
