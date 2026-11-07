# P1-09 字符串库 + format 体系 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 STD-10（字符串操作库）+ STD-11（format 内建与 FMT-08 规格集），print/println/format 共享一套格式解析，新诊断 E2020~E2022，文档与 TODO 收口。

**Architecture:** 分层复用——`src/ast/PrintFormat.cpp` 提供 PrintSpec 结构化解析 + printf 转译（print/format 共享）；`format()` 为内建（sema 全检 + codegen 直拼，字面量走直线的 memcpy/snprintf，动态串走合成 helper）；`find`/`rfind`/`sub`/`split`/`split_destroy` 为内建原语（.cmm 无法触达 str 内部与构造切片值）；其余组合 API 全部进 `libs/std/string.cmm`。

**Tech Stack:** C++20 / LLVM ORC LLJIT / gtest / libc (snprintf/memcpy/malloc)；`%b` 依赖 glibc ≥ 2.35（C23 二进制转换）。

**Spec:** `docs/superpowers/specs/2026-10-09-p109-string-format-design.md`（本计划从 spec 论证，执行者两份都读）

## 对 spec 的两处已论证偏差（执行者须知）

1. **`split`/`split_destroy` 改为内建**（spec §3.2 曾将其列入 string.cmm）：.cmm 只能经数组 decay 得到切片，无法从 `ptr+len` 构造 `[]str` 值（test_slice.cpp 证实无此语法）。而 `string.new/destroy` 本就是 codegen 内建（同样原因），split 与之同构。拼写仍为自由函数 `split(s, sep)` / `split_destroy(parts)`，user-defined 优先规则与 print 一致。其余 API 全部留在库层。
2. **`split` 空 sep 为运行时 panic**（spec §4 曾标「编译期」）：库函数无法产生编译期诊断，`split` 又是内建——内建 sema 对字面量空 sep 可编译期拒绝，非字面量运行时 panic。两态都做。
3. **StrBuilder 为值类型**（spec §3.2 曾写「destroy 释放自身 malloc」）：语言无 `new` 表达式，按值返回含 malloc 字段的类有别名双 free 陷阱（I4 同型）。钉死为栈值 + `init()`，`destroy()` 只释放内部 string。

## Global Constraints

- TDD：先写测试亲见 RED，再实现；每任务结束全量 `ctest --test-dir build` 绿（含已知 flaky：`OptionalResultE2E.OptionalBranchExec`、`EnumUnderlyingE2E.Int8EnumNegativeValue`、`ModuleVisibilityTest.PrivateHelperUsableInsideModule`——隔离重跑确认，不阻塞）。
- 每任务恰一提交；`git add` 只加本任务明确文件清单，禁止 `git add -A`/目录级添加；`docs/superpowers/**` 与 `Progress.md` 需 `git add -f`。
- 新增测试**文件**（Task 6）后需 `cmake -B build` 重配置（GLOB 在 configure 期生效）。
- 诊断码核对：E2015 之后码位空闲，E2020/E2021/E2022 直接启用（spec §2.3 的「顺延」条款不触发）。
- `{:d}` 显式十进制**拒绝**（spec §2.2 钉死）；十进制只用 `{}`。现网无测试用 `{:d}`/`{:g}`，收窄安全。
- print 的 `{}` 旧行为逐字节不变（FMT-06 回归保障）：float 默认 `%g`、`%` 转义、str `%.*s`、既有错误文案。
- 提交信息沿用 conventional 风格（`feat:`/`docs:`/`test:`）。

## Review Focus

spec 未尽而最可能咬人的五类输入（每条已在归属任务里钉了测试或文档）：

1. **glibc 无 `%b`**（< 2.35 会原样输出 `%b`）→ Task 3 `FormatSpecsExec` 值断言 `{:b}` 输出 `101`；若宿主失败，回退方案 = 把 `b` 归入渲染路径手写二进制转换，行为不变。
2. **动态格式串里的规格文本**（如 `"{:x}"` 运行时传入）→ 钉死语义：规格被忽略、按实参静态类型默认转换；Task 3 `FormatDynSpecIgnored` 钉死实际行为并在 stdlib.md 明示「自担」。
3. **str 实参含 NUL/非 UTF-8 字节** → format 对 str 实参走 memcpy 不走 `%s`，字节级保真；Task 3 `FormatStrNulPreserved` 以 len 断言钉死。
4. **print 既有行为回归** → Task 1 全量回归（既有 unit 测试文案逐字保留）+ Task 3 print e2e smoke。
5. **split 视图生存期**（源 string 先 destroy → 视图悬挂）→ 不测试（UB），Task 7 stdlib.md 明示用户责任（I4 同模型）。

---

### Task 1: PrintSpec 解析器 + print/println 接入 FMT-08（FMT-06/08）

**Files:**
- Modify: `src/ast/PrintFormat.h`、`src/ast/PrintFormat.cpp`
- Modify: `src/ast/Expr.h`（CallExprAST 加 `std::vector<PrintSpec> printSpecs;`）
- Modify: `src/sema/SemanticAnalyzer.cpp`（tryAnalyzePrintCall 传入 outSpecs/errKind；`builtinPrintKind` 加 `TypeKind::String → PrintArgKind::Str`）
- Modify: `src/ast/Expr.cpp`（isPrint 分支：渲染路径槽位）
- Modify: `src/sema/Diagnostic.h`、`src/sema/Diagnostic.cpp`（E2020~22 注册）
- Test: `tests/ast/test_print_format.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Produces（Task 2/3 依赖）:
  ```cpp
  // PrintFormat.h
  struct PrintSpec {
      char fill = ' ';
      char align = 0;      // 0 | '<' | '>' | '^'
      char sign = 0;       // 0 | '+' | '-' | ' '（'-' 为默认，转译为无 flag）
      bool zero = false;   // '0' 填充
      int width = 0;       // 0 = 无
      int precision = -1;  // -1 = 无
      char type = 0;       // 0 | 'x' | 'X' | 'o' | 'b' | 'f' | 'e' | 's'
  };
  enum class PrintFormatError { None, ArgCount, SpecType, SpecSyntax };
  bool parsePrintSpec(const std::string& spec, PrintSpec& out, std::string& error);
  bool specToPrintfConversion(PrintArgKind kind, const PrintSpec& spec,
                              std::string& outConv, bool& outNeedsRender,
                              std::string& error);
  bool buildPrintFormat(const std::string& literal,
                        const std::vector<PrintArgKind>& kinds, bool newline,
                        std::string& outFormat, std::string& error,
                        std::vector<PrintSpec>* outSpecs = nullptr,
                        PrintFormatError* errKind = nullptr);
  ```
  渲染路径判定：`spec.align == '^' || (spec.fill != ' ' && spec.fill != 0)`（width>0 前提）。渲染槽位的扁平串输出 `%s`，codegen 自行预渲染填充。

- [ ] **Step 1: 写失败测试（unit，tests/ast/test_print_format.cpp 追加）**

```cpp
// 文件顶部加别名（沿用既有 build() helper）：
using PA = PrintArgKind;  // PA::I32 不存在 → 直接写 PrintArgKind::Int32 等全名，下同
// 转译正确性（kind 实参用 PrintArgKind:: 全名）
EXPECT_EQ(build("{:x} {:X} {:o} {:b}", {Int32,Int32,Int32,Int32}), "%x %X %o %b");
EXPECT_EQ(build("{:b}", {Int64}), "%llb");          // 64 位补 ll
EXPECT_EQ(build("{:02}", {Int32}), "%02d");
EXPECT_EQ(build("{:05x}", {UInt32}), "%05x");
EXPECT_EQ(build("{:.2f} {:.0e}", {Float,Float}), "%.2f %.0e");
EXPECT_EQ(build("{:.2}", {Float}), "%.2f");          // 浮点无 type 推断为 f
EXPECT_EQ(build("{:>8} {:<8}", {Int32,Int32}), "%8d %-8d");
EXPECT_EQ(build("{:+}", {Int32}), "%+d");            // sign '+' → '+' flag
EXPECT_EQ(build("{:-}", {Int32}), "%d");             // sign '-' → 默认（无 flag）
EXPECT_EQ(build("{: 5}", {Int32}), "% 5d");          // sign ' ' → ' ' flag
EXPECT_EQ(build("{:6}", {Str}), "%-6.*s");           // 字符串默认左对齐（带 width 时）
EXPECT_EQ(build("{:<6}", {Str}), "%-6.*s");
EXPECT_EQ(build("{:>6}", {Str}), "%6.*s");
EXPECT_EQ(build("{:>6}", {Bool}), "%6s");
EXPECT_EQ(build("{:6}", {Bool}), "%-6s");
EXPECT_EQ(build("{:*<5}", {Int32}), "%s");           // 渲染路径 → "%s"
EXPECT_EQ(build("{:^6}", {Str}), "%s");
// 拒绝矩阵（ERR: 前缀，文案 Task 1 内自洽即可）
EXPECT_EQ(build("{:d}", {Int32}), "ERR:...");        // 显式 d 拒绝
EXPECT_EQ(build("{:x}", {Float}), "ERR:...");        // x/X/o/b 仅整数
EXPECT_EQ(build("{:f}", {Int32}), "ERR:...");        // f/e 仅浮点
EXPECT_EQ(build("{:s}", {Int32}), "ERR:...");
EXPECT_EQ(build("{:.2}",  {Int32}), "ERR:...");      // precision 仅浮点
EXPECT_EQ(build("{:x}",  {Bool}), "ERR:...");
EXPECT_EQ(build("{:z}", {Int32}), "ERR:...");        // 未知 type
EXPECT_EQ(build("{:0<5}", {Int32}), "ERR:...");      // 0 与 '<' 冲突
EXPECT_EQ(build("{:0}",  {Int32}), "ERR:...");       // 0 无 width
EXPECT_EQ(build("{:>}",  {Int32}), "ERR:...");       // align 无 width
EXPECT_EQ(build("{:201}",{Int32}), "ERR:...");       // width/precision > 200
EXPECT_EQ(build("{:.2f}", {Char}), "ERR:...");       // Char/CString/Pointer 全规格拒绝
// 回归（逐字保留既有文案）
EXPECT_EQ(build("aa {}", {Int32}), "aa %d");
EXPECT_EQ(build("{}", {Float}), "%g");               // 空 spec 浮点默认仍 %g
EXPECT_EQ(build("{} {}", {Str, Bool}), "%.*s %s");
EXPECT_EQ(build("a {{ b }} c", {}), "a { b } c");
EXPECT_EQ(build("50%", {}), "50%%");
EXPECT_EQ(build("{} {}", {Int32}), "ERR:too few arguments for print format");
```

- [ ] **Step 2: 跑测试确认 FAIL**

Run: `cmake --build build -j && ctest --test-dir build -R PrintFormatTest --output-on-failure`
Expected: 编译失败（PrintSpec/parsePrintSpec 未定义）。

- [ ] **Step 3: 实现 PrintFormat.h/.cpp**

- `parsePrintSpec`：文法 `[fill]align? sign? '0'? width? ('.' precision)? type?`；fill 仅当后随 align 字符时生效；align 无 width → 错；`0` 后必须 width≥1；width/precision > 200 → 错。错误统一进 SpecSyntax 类。
- `specToPrintfConversion`：按 Task 1 Interfaces 的矩阵判定 kind×spec（错 → SpecType 类）；输出 printf 转换：64 位补 `ll`、`'<' → '-'` flag、`'+'/' ' → 同名 flag`、zero → `'0'`、Str 带宽默认 `'-'`（左对齐）；浮点空 type 非空 spec → `f`；渲染路径槽位输出 `%s` + `outNeedsRender=true`。空 spec 走旧默认表（`%d/%lld/%u/%llu/%c/%g/%s/%.*s/%p`）——`conversionFor` 旧逻辑收敛为此函数的空 spec 分支。
- `buildPrintFormat`：扫描逻辑不变（`{{`/`}}`/`%`/槽位计数），换用上述两函数；新增 `outSpecs`（逐槽 PrintSpec）与 `errKind`（ArgCount/SpecType/SpecSyntax），既有错误文案逐字保留。

- [ ] **Step 4: 跑 unit 确认 PASS**

Run: `ctest --test-dir build -R PrintFormatTest --output-on-failure`
Expected: PASS（全绿，含既有用例）。

- [ ] **Step 5: sema/codegen 接线 + 诊断注册**

- Diagnostic.h 枚举追加 `SemFormatArgCount, SemFormatSpecType, SemFormatSpecSyntax`；Diagnostic.cpp 注册 `E2020 "format argument count mismatch"`、`E2021 "format spec does not match argument type"`、`E2022 "invalid format spec"`。
- tryAnalyzePrintCall：`errKind` → `emitError(DiagnosticCode::SemFormatXxx, error, node)`（print 也获得稳定码）；`outSpecs` → `node.printSpecs`。
- builtinPrintKind：`TypeKind::String → PrintArgKind::Str`（string 实参投影 str；print/format 通用；print codegen Str 分支 ExtractValue 0/1 对 {ptr,len,cap} 布局同样成立）。
- Expr.cpp isPrint 分支：对渲染槽位（依 `node.printSpecs`）先 `emitPaddedValue(ctx, spec, kind, v, i64* outLen /*可空*/)` 生成 NUL 结尾填充串（alloca(256) 上 snprintf 裸值——conv 用 spec 去掉 align/fill/width 的版本——再填充循环；`outLen` 供 Task 3 format 路径取精确长度），返回 `char*`；实现为 Expr.cpp 内 static 函数。非渲染槽位走 `promotePrintArg` 原路径。

- [ ] **Step 6: sema 断言 + 全量绿**

`tests/sema/test_semantic_analyzer.cpp` 追加：`analyzeFullyOk` 为假且 `diagnosticId(d.code) == "E2021"` 用例：`println("{:x}", 1.5)`；`"E2020"` 用例：`println("{} {}", 1)`；`"E2022"` 用例：`println("{:d}", 1)`。
Run: `ctest --test-dir build`（全量）。Expected: 绿（e2e print smoke 原有用例不回退）。

- [ ] **Step 7: 提交**

```bash
git add src/ast/PrintFormat.h src/ast/PrintFormat.cpp src/ast/Expr.h src/ast/Expr.cpp \
        src/sema/SemanticAnalyzer.cpp src/sema/Diagnostic.h src/sema/Diagnostic.cpp \
        tests/ast/test_print_format.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(fmt): FMT-08 spec set with PrintSpec parser shared by print/println (E2020-E2022)"
```

---

### Task 2: format() sema + E2020~22（FMT-07/09/10/11 编译期面）

**Files:**
- Modify: `src/ast/Expr.h`（CallExprAST 加 `bool isFormat = false; std::string formatLiteral; std::vector<PrintSpec> formatSpecs;`）
- Modify: `src/sema/SemanticAnalyzer.h`（声明 `tryAnalyzeFormatCall`）、`src/sema/SemanticAnalyzer.cpp`
- Test: `tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: Task 1 的 `buildPrintFormat(..., outSpecs, errKind)`、`lowerToString`、`builtinPrintKind`。
- Produces（Task 3 依赖）: sema 后 format 调用满足——`node.isFormat = true`；`node.printArgKinds` = 实参 1..n 的 PrintArgKind；字面量 fmt 时 `node.formatLiteral` = 原字面量、`node.formatSpecs` = 逐槽 PrintSpec；动态 fmt 时 `formatLiteral` 为空；`node.type = getStringType()`。分派点：`visit(CallExprAST)` 内 `print/println` 块之后，`callee == "format"` 且无 user-defined 时进入。

- [ ] **Step 1: 写失败测试（tests/sema/test_semantic_analyzer.cpp 追加）**

```cpp
// ok 用例（analyzeFullyOk == true）
"string s = format(\"x={}\", 1);"                     // FMT-07
"str f = \"{}\"; string s = format(f, 1);"           // 动态 fmt 合法、无检查
"class C { public: char* to_string() { return \"C\"; } }; string s = format(\"{}\", C{});"  // FMT-10 方法路径
// class C 无 to_string → format("{}", C{}) 报 cannot format（沿用 print 文案）
// E 码用例（诊断扫描 diagnosticId）
format("{}")        → E2020   // 占位符多于实参
format("x", 1)      → E2020   // 实参多于占位符
format("{:x}", 1.5) → E2021
format("{:f}", 1)   → E2021
format("{:s}", 1)   → E2021
format("{:d}", 1)   → E2022
format("{:z}", 1)   → E2022
format(1, 2)        → 既有类型不匹配文案（非 str 首参）
// user-defined 优先
"string format(str f, int32 v) { return string.new(f); }" + 调用 → analyzeFullyOk
```

- [ ] **Step 2: 跑测试确认 FAIL**

Run: `ctest --test-dir build -R SemanticAnalyzer --output-on-failure`
Expected: FAIL（今日 `format` 走 unresolved call）。

- [ ] **Step 3: 实现 tryAnalyzeFormatCall**

镜像 tryAnalyzePrintCall：args[0] 必须 str 类型（String 亦可，投影）；实参 1..n 逐个 `builtinPrintKind` → `lowerToString`；字面量 fmt → `buildPrintFormat(literal, kinds, /*newline=*/false, fmt, error, &specs, &errKind)`，errKind 非 None → 对应 E 码 emitError；非字面量 fmt → 只分类实参（不解析规格）；置 `isFormat/formatLiteral/formatSpecs/printArgKinds`，`type = getStringType()`。分派块加在 print 块后（user-defined 优先同 assert/panic 先例）。

- [ ] **Step 4: 跑 sema 测试确认 PASS**

Run: `ctest --test-dir build -R SemanticAnalyzer --output-on-failure`
Expected: PASS。

- [ ] **Step 5: 全量 + 提交**

Run: `ctest --test-dir build`。Expected: 绿（format 尚无 codegen，无 e2e 触及）。
```bash
git add src/ast/Expr.h src/sema/SemanticAnalyzer.h src/sema/SemanticAnalyzer.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(fmt): builtin format() sema with compile-time checks (FMT-07/09/10/11)"
```

---

### Task 3: format() codegen + e2e 值断言（FMT-07/08/12 运行时面）

**Files:**
- Modify: `src/ast/Expr.cpp`（CallExprAST::codegen 加 isFormat 分支；Task 1 的 `emitPaddedValue` 扩展精确长度回传）
- Modify: `src/codegen/CodegenContext.h`、`src/codegen/CodegenContext.cpp`（`getFormatDynFn()`）
- Test: `tests/e2e/test_string.cpp`

**Interfaces:**
- Consumes: Task 2 的 `isFormat/formatLiteral/formatSpecs/printArgKinds`；`promotePrintArg`、`builtinStrArg`（Expr.cpp 既有 static，string→str 投影）。
- Produces: `CodegenContext::getFormatDynFn()` — 合成 `internal {i8*, i64, i64} @cmm.format.dyn(i8* fmt, i64 fmtLen, i8** chunkPtrs, i64* chunkLens, i64 n)`，返回值直接是 string 三域布局 `{ptr, len, cap}`（Task 4/5 无依赖，供本任务动态路径）。

- [ ] **Step 1: 写失败测试（tests/e2e/test_string.cpp 追加，值断言经 `str v = s; v == "..."` 比较、main 返回码）**

```cpp
FormatBasicExec:      format("x={} y={}", 7, -3) == "x=7 y=-3"
FormatEmptyExec:      format("") → len()==0（cap ≥ 1）
FormatSpecsExec:      format("{:x} {:X} {:o} {:b}", 255, 255, 8, 5) == "ff FF 10 101"   // Review Focus 1
FormatZeroPadExec:    format("[{:02}:{:02}]", 5, 45) == "[05:45]"
FormatFloatPrecExec:  format("{:.2f} {:e}", 3.14159, 31415.926) == "3.14 3.141593e+04"
FormatWidthAlignExec: format("{:>5}|{:<5}|", 42, 42) == "   42|42   |"
FormatCenteredExec:   format("{:^5}|", 42) == " 42  |"          // 渲染路径
FormatCustomFillExec: format("{:*<5}", 42) == "42***"
FormatStrExec:        format("s={} t={}", "ab", "cd") == "s=ab t=cd"
FormatStrNulPreserved: str 含 NUL 的实参 → 结果 len 逐字节核对            // Review Focus 3
FormatStringArgExec:  format("{}", string.new("vv")) == "vv"     // string 实参投影
FormatEscapesExec:    format("{{a}}") == "{a}"
FormatDynExec:        str f = "{}-{}"; format(f, 1, 2) == "1-2"
FormatDynEscapeExec:  str f = "a{{b"; format(f) == "a{b"
FormatDynExtraExec:   str f = "{}"; format(f, 1, 2) == "1"       // 多余实参忽略
FormatDynSpecIgnored: str f = "{:x}"; format(f, 10) == "10"      // Review Focus 2：规格忽略、默认转换
FormatDynShortExec:   str f = "{} {}"; format(f, 1) == "1 "      // 缺占位读空串即停
PrintSpecSmokeExec:   println("{:^7}", "ab"); println("{:b}", 5); → 仅编译执行不崩
```

- [ ] **Step 2: 跑测试确认 FAIL**

Run: `ctest --test-dir build -R StringE2E --output-on-failure`
Expected: FAIL（codegen 无 isFormat 分支，走普通函数调用 → unknown function）。

- [ ] **Step 3: 实现 codegen**

- **字面量路径**（`formatLiteral` 非空）：直线 IR——按序扫描字面量：文字段 `CreateGlobalString` + memcpy（长度常量）；槽位：
  - str 类实参：`builtinStrArg` 投影 → 直接 memcpy `{ptr,len}`（零拷贝字节保真）；
  - 渲染槽位（formatSpecs 对齐 Task 1 判定）：`emitPaddedValue(ctx, spec, kind, v, &len)` 返回 `{char* buf, i64 len}`（alloca 上 snprintf 裸值 + 填充，长度显式回传，非 NUL 依赖）；
  - 其余槽位：alloca(max(64, width+precision+32)) 上 `snprintf`（conv 来自 `specToPrintfConversion` 的非渲染结果，实参经 `promotePrintArg`）→ 长度 = 返回值 sext i64；
  - total = 长度加链；`malloc(max(total, 64))`；逐段 memcpy；`insertvalue` 三域 `{ptr, total, cap}`。单缓冲一次分配，无 realloc（FMT-12：长度可预知，增长路径为死代码——spec §2.4「更简者」条款）。
- **动态路径**（`formatLiteral` 为空）：每实参按 printArgKinds 默认转换渲染成 chunk（str→视图；标量→alloca(64) snprintf；bool/to_string→既有 promotePrintArg 产物 + strlen），alloca 数组存 `{ptr,len}` 两列；`getFormatDynFn(fmt.ptr, fmt.len, ptrs, lens, n)` 扫描：`{{`/`}}` 转义、`{` 消费下一 chunk（无则空）、单 `}` 视作字面量（动态串不校验，spec §2.3.3）；helper 内 `malloc(fmtLen + ΣchunkLen + 1)` 一次分配，返回 `{ptr, len, cap}`。合成遵循 getUtf8CharCountFn 先例（save/restoreIP；先合成 callee 再接调用点）。
- print 冒烟：isPrint 分支渲染槽位在 Task 1 已接，此处仅跑通 e2e。

- [ ] **Step 4: 跑 e2e 确认 PASS**

Run: `ctest --test-dir build -R "StringE2E|PrintFormat" --output-on-failure`
Expected: PASS（含 Review Focus 1/2/3 三条）。

- [ ] **Step 5: 全量 + 提交**

Run: `ctest --test-dir build`。Expected: 全绿。
```bash
git add src/ast/Expr.cpp src/codegen/CodegenContext.h src/codegen/CodegenContext.cpp tests/e2e/test_string.cpp
git commit -m "feat(fmt): format() codegen - literal straight-line assembly and dynamic chunk scan (FMT-12 single buffer)"
```

---

### Task 4: str 内建原语 find/rfind/sub（STD-10 内建层）

**Files:**
- Modify: `src/ast/Expr.h`（BuiltinMethod 加 `StrFind, StrRFind, StrSub`）
- Modify: `src/sema/SemanticAnalyzer.cpp`（analyzeStrMethod 加三分支）
- Modify: `src/ast/Expr.cpp`（codegenBuiltinMethod 加三 case；CodegenContext 加 `getStrFindFn(bool reverse)` 或 `getStrFindFn/getStrRFindFn`）
- Test: `tests/e2e/test_string.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Produces（Task 5/6 依赖）: `s.find(needle: str|String) -> isize`（首现字节下标，无 → -1，空 needle → 0）；`s.rfind(needle) -> isize`（末现，无 → -1，空 needle → len）；`s.sub(begin: 整数, end: 整数) -> str`（零拷贝视图 `{ptr+begin, end-begin}`；`begin > end || end > len` → panic，文案 `str.sub: out of bounds`）。

- [ ] **Step 1: 写失败测试**

sema：`s.find()` 无参 → 错；`s.find(1)` 非 str → 错；`s.sub(1.5, 2)` 非整数 → 错；`s.sub(1)` 参数个数 → 错。
e2e（值断言）：
```cpp
FindExec:      "hello".find("ll") == 2；find("z") == -1；find("") == 0
RFindExec:     "a/b/c".rfind("/") == 3；rfind("z") == -1
SubExec:       "hello".sub(1, 3) == "el"；sub(0, 5) == "hello"；sub(2, 2) == ""
SubBoundsCompile: "hello".sub(3, 2) → 仅编译执行路径存在（panic 不可运行，参照 StrFromCRuntimeValidates 编译不执行先例）
FindSubOnStringExec: string.new("xabx").find("b") == 3（方法可用于 string 投影）
```

- [ ] **Step 2: 确认 FAIL**（编译失败：枚举未定义）

- [ ] **Step 3: 实现**

- sema 三分支镜像 len/char_at 先例：find/rfind 一 str-like 参 → isize；sub 两整数参 → str。
- codegen：find/rfind 用 `CodegenContext` 合成扫描 helper（手写字节扫描，不用 memmem——避免 _GNU_SOURCE 依赖；reverse 一参两用），返回 i64；sub 为纯 ExtractValue/GEP + 越界比较 → panic 块（镜像 codegenStrFromC 的 validate-fail abort 路径，`nodeSourcePrefix` 前缀 + `str.sub: out of bounds`）。

- [ ] **Step 4: 确认 PASS**（e2e + sema）

- [ ] **Step 5: 全量 + 提交**

```bash
git add src/ast/Expr.h src/ast/Expr.cpp src/sema/SemanticAnalyzer.cpp \
        src/codegen/CodegenContext.h src/codegen/CodegenContext.cpp \
        tests/e2e/test_string.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(str): builtin find/rfind/sub primitives (STD-10)"
```

---

### Task 5: split/split_destroy 内建（spec 偏差 1/2 落地）

**Files:**
- Modify: `src/sema/SemanticAnalyzer.{h,cpp}`（`tryAnalyzeSplitCall`；visit 分派：`split`/`split_destroy`，user-defined 优先）
- Modify: `src/ast/Expr.h`（CallExprAST 加 `bool isSplit = false; bool isSplitDestroy = false;`）
- Modify: `src/ast/Expr.cpp`（codegen 两分支）
- Test: `tests/sema/test_semantic_analyzer.cpp`、`tests/e2e/test_string.cpp`

**Interfaces:**
- Produces（Task 6 依赖）: `split(s: str|String, sep: str|String) -> []str`（元素为源数据零拷贝视图；数组 `malloc(n * size_of(str))`；sep 为字面空串 → 编译期 E2022 文案 `split: empty separator`，动态空串 → 运行时 panic 同文案）；`split_destroy(parts: []str) -> void`（`free(parts.ptr)`，不触碰元素）。无分隔符 → n=1 全串（非空数组）。切片 LLVM 规范型参照 `emitArrayToSliceDecay` 的 canonical 类型。

- [ ] **Step 1: 写失败测试**

sema：`split(1, ",")` / `split("a", 2)` → 类型错；`split("a", "")` → E2022（字面空 sep）；user 声明自己的 `split` → 内建让位。
e2e：
```cpp
SplitExec:        parts = split("a,b,c", ","); parts.len == 3; parts[1] == "b"; split_destroy(parts)
SplitNoSepExec:   split("abc", ",") → len == 1; parts[0] == "abc"
SplitViewExec:    string src = string.new("x,y"); parts = split(src, ","); parts[0] == "x"（视图语义）；destroy 顺序按文档（先 split_destroy 后 src.destroy）
SplitTailEmpty:   split("a,", ",") → len == 2; parts[1] == ""    // 尾空段保留
```

- [ ] **Step 2: 确认 FAIL**

- [ ] **Step 3: 实现**

- sema：两 str-like 参 → `SliceType(str)`；字面空 sep → `emitError(SemFormatSpecSyntax, "split: empty separator", node)`。
- codegen：合成扫描（复用 find 的扫描 helper 统计段数 → malloc → 二次扫描逐段写 `{ptr,len}`）→ 返回切片值 `{arr, n}`；split_destroy 提取 ptr → free。

- [ ] **Step 4: 确认 PASS**

- [ ] **Step 5: 全量 + 提交**

```bash
git add src/sema/SemanticAnalyzer.h src/sema/SemanticAnalyzer.cpp src/ast/Expr.h src/ast/Expr.cpp \
        tests/sema/test_semantic_analyzer.cpp tests/e2e/test_string.cpp
git commit -m "feat(str): builtin split/split_destroy returning zero-copy []str views"
```

---

### Task 6: libs/std/string.cmm 组合库

**Files:**
- Create: `libs/std/string.cmm`
- Create: `tests/e2e/test_std_string_lib.cpp`（fixture 复制 test_std_modules.cpp 的 `runWithImports`，`import std.string;`）
- 重配置：`cmake -B build`（新测试文件进 GLOB）

**Interfaces:**
- Consumes: Task 4 的 `find/rfind/sub`、Task 5 的 `split/split_destroy`（内建，无需 import 即可用；库内亦可直用）；`string.new/append/push/len/destroy`、`char_len_at/char_count`（P1-06 内建）；`extern malloc/free/isspace` 自包含声明（io.cmm 先例）。
- Produces（stdlib.md 文档对象）: `export namespace std { contains, starts_with, ends_with, trim_left, trim_right, trim, join, concat, repeat, utf8_sub, class StrBuilder }`。签名钉死：

```
bool     contains(str s, str sub)            // s.find(sub) >= 0
bool     starts_with(str s, str prefix)      // prefix.len() <= s.len() && s.find(prefix) == 0
bool     ends_with(str s, str suffix)        // p = s.rfind(suffix); p >= 0 && p + suffix.len() == s.len()
str      trim_left(str s)                    // isspace 扫描 + sub（零拷贝）
str      trim_right(str s)
str      trim(str s)                         // 两者串联
string   join([]str parts, str sep)          // parts.len == 0 → string.new("")；逐段 append(sep)+append(part)
string   concat(str a, str b)                // string.new(a).append(b)
string   repeat(str s, usize n)              // n==0 → string.new("")
str      utf8_sub(str s, usize start_cp, usize len_cp)  // char_len_at 累计 + sub；越界 panic
class StrBuilder {                             // 值类型（spec 偏差 3）
public: string buf;
public: void init()                            // this.buf = string.new("")——声明后必须先调
public: void append(str s)
public: void push(char c)
public: usize len()
public: str  build()                           // 内部 string 的视图；builder 存活期有效
public: void destroy()                         // 仅释放内部 string
}
```
（`<`/`>` 字典序不做——spec §3.2；`to_lower/to_upper` 不做——spec §8。）

- [ ] **Step 1: 写失败测试（tests/e2e/test_std_string_lib.cpp）**

```cpp
ContainsExec / StartsWithExec / EndsWithExec / TrimExec（trim_left/right/trim 值断言）
JoinExec:  join(split("a,b,c", ","), "-") == "a-b-c"（与内建 split 串联）+ join 结果 destroy
ConcatRepeatExec: concat("ab","cd")=="abcd"; repeat("ab", 3)=="ababab"; repeat("ab", 0) len==0
StrBuilderExec: b.init(); append("x"); push('y'); len()==2; build()=="xy"; destroy()
Utf8SubExec: utf8_sub("héllo", 1, 2) == "él"（码点步进）
SplitEmptySepPanic: split("a", "") 运行时 panic → 编译不执行（Task 5 已钉编译期字面量分支，此处钉动态路径存在性）
```

- [ ] **Step 2: 重配置 + 跑测试确认 FAIL**

Run: `cmake -B build && cmake --build build -j && ctest --test-dir build -R StdStringLib --output-on-failure`
Expected: FAIL（module 文件不存在，import 失败）。

- [ ] **Step 3: 实现 libs/std/string.cmm**

模块头 `module std.string;` + `export namespace std { ... }`（io.cmm 范式）；libc extern 自包含；实现按 Interfaces 签名与语义注释逐条落地。`return this.buf;` 若直返不受支持，退 `str v = this.buf; return v;`。

- [ ] **Step 4: 确认 PASS**

Run: `ctest --test-dir build -R StdStringLib --output-on-failure` → PASS。

- [ ] **Step 5: 全量 + 提交**

```bash
git add libs/std/string.cmm tests/e2e/test_std_string_lib.cpp
git commit -m "feat(std): std.string combinators - trim/split-view/join/concat/repeat/StrBuilder/utf8_sub (STD-10)"
```

---

### Task 7: 文档 + TODO 收口 + Progress（无代码）

**Files:**
- Modify: `docs/spec/stdlib.md`（新增 §std.string 与 §std.format 两节）
- Modify: `docs/spec/abi.md`（布局表补 `str`/`string` 两行——终审 M5 清账）
- Modify: `TODO.md`（STD-09 `[~]` to_string 文档；STD-10 `[x]`（to_lower/to_upper、字典序挂账）；STD-11 `[~]`（print/format 核心，std.format 工具库另轮）；FMT-06 `[x]`（内建直用+文档归属，D4 裁决）；FMT-07/08/09/10/11 `[x]`；FMT-12 `[~]`（单缓冲精确分配）；FMT-13/14 保持；P1-09 行追加本轮完成摘要与挂账）
- Modify: `Progress.md`（`git add -f`）

- [ ] **Step 1: 写 stdlib.md 两节**——std.string：API 表 + 所有权模型（split 视图生存期 = Review Focus 5 文档化；join/concat/repeat/StrBuilder 显式 destroy）+ StrBuilder.init 约定；std.format：format 签名与所有权、FMT-08 规格文法（§2.2 全集 + 拒绝矩阵）、E2020~22 表、动态格式串契约（规格忽略/缺位补空/多余忽略——Review Focus 2）。
- [ ] **Step 2: abi.md 补两行**——`str = {i8* ptr, i64 len}`；`string = {i8* ptr, i64 len, i64 cap}`（对照 src/ast/Type.cpp 实际布局核对后再写）。
- [ ] **Step 3: TODO 收口**——按上文 Files 逐行改，日期 2026-10-09。
- [ ] **Step 4: 验证**——`ctest --test-dir build` 全量仍绿（文档任务零代码）；`git diff --stat` 确认无越界改动。
- [ ] **Step 5: Progress.md 记录 + 提交**

```bash
git add docs/spec/stdlib.md docs/spec/abi.md TODO.md
git add -f Progress.md
git commit -m "docs: std.string/std.format spec sections, abi str/string rows, TODO closeout (P1-09)"
```

---

## 验收基线

- 全量 ctest 绿（基线 1123+，本轮净增约 60 用例），已知 3 个 flaky 隔离重跑通过。
- spec §6/§7 的 T1~T6 全覆盖：T1→Task 1、T2→Task 2、T3→Task 3、T4→Task 4+5、T5→Task 6、T6→Task 7。
- Review Focus 五条：1/2/3/4 有测试钉死（Task 1/3），5 文档化（Task 7）。
