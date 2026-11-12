# P1-06 str / string 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 `str`（UTF-8 视图）与 `string`（动态字符串）内建类型，字面量重定型为 `str`，内建泛型类型名小写化（`optional`/`result`）。

**Architecture:** 两个新 `TypeKind`（`Str` 布局 `{ptr, len}`、`String` 布局 `{ptr, len, capacity}`），走 Slice 的 canonical named-struct 先例（`"str"`/`"string"` 单例 LLVM struct）。内建方法经 `MethodCallExprAST` 新 `builtinMethod` 标记在 sema 分派、codegen 降级为 libc 调用（malloc/realloc/free/memcpy/memcmp/strlen）+ 模块内合成 IR 辅助函数（UTF-8 步进）。UTF-8 验证双实现：sema 期 `src/support/Utf8.*`（诊断 E2015）+ codegen 期合成 IR。

**Tech Stack:** C++20 / LLVM / gtest；测试自动 GLOB（`tests/CMakeLists.txt:13`），新测试文件无需改 CMake。

**Spec:** `docs/superpowers/specs/2026-10-08-str-string-design.md`

## Global Constraints

- 每任务：RED 先行（未亲见失败不写实现）→ 全量 `ctest` 绿 → 一提交（TDD）。
- `git add` 只加明确文件清单，**禁止** `git add -A src/ tests/`（P1-04 教训）；`Progress.md`/`docs/superpowers/` 需 `git add -f`。
- 类型名全小写：`str`/`string`/`optional`/`result`；旧大写拼写成为未知标识符（DEC-18 硬改先例）。
- 无隐式堆分配（DEC-20）：字面量/`str` 赋给 `string` 必须显式 `string.new`。
- `str` 一律不拥有；`string` 拥有其 `ptr`。**`string → str` 隐式视图转换；`str → string` 仅经 `string.new`**（spec §1.4 未显式钉死，本计划补钉，见交接说明）。
- 本轮不做：format 体系（FMT-06/07/08/09）、STD-10 高层操作、SSO、`string` 的 constexpr、运行时边界检查（DEC-06 未决，不加检查）。
- UTF-8 校验规则（两处实现共用）：RFC 3629 子集——接受 U+0000~U+10FFFF，拒绝越界续字节、代理区 U+D800~DFFF、超长编码、超过 4 字节的序列；ASCII（<0x80）快路径。
- 已知 flaky（偶发 SEGFAULT，隔离重跑验证即可，不阻塞）：`OptionalResultE2E.OptionalBranchExec`、`EnumUnderlyingE2E.Int8EnumNegativeValue`。
- 诊断码：E2015 = invalid UTF-8 sequence（下一个空闲码，已核对 `src/sema/Diagnostic.cpp` 无冲突）。

## Review Focus

1. **字面量传 `char*` 形参的既有代码**——用户期望重定型后仍能编译（`str → char*` 隐式）。全量 ctest 暴露；pin 于 T2（`StrToCharPointerPasses`、print 的 Str 实参）。
2. **`str`/`string`/`optional`/`result` 作为用户标识符**——DEC-18 硬改语义：类型位置恒为类型，用户不得再用它们命名变量。pin 于 T6（`TypeNameCannotBeVariable`）。
3. **无效 UTF-8 字面量**（转义构造如 `"\xC0\x80"`、`"\xFF"`、原始字符串）——用户期望编译期 E2015，而非运行时乱码。pin 于 T5。
4. **对已 `destroy()` 的 string 再操作 / 双重 destroy**——C 语义下用户责任，但 `destroy()` 后字段清零使二次 destroy 无害（free(NULL)）。pin 于 T6（`DoubleDestroySafe`）。
5. **空串路径**——`string.new("")` 的 ptr 必须 malloc(1) 保证非空（`str → char*` 视图安全）；`"".len() == 0`、`char_count() == 0`。pin 于 T6（`EmptyStringInvariants`）。

---

### Task 1: 内建类型名小写化（`Optional`→`optional`、`Result`→`result`）

**Files:**
- Modify: `src/frontend/Parser.cpp:1535`（`isTypeStart` 名字判断）、`src/frontend/Parser.cpp:1821`（`parseBaseType` 特判；连带 :1838-1857 内错误文案与注释）
- Modify: `tests/e2e/test_optional_result.cpp`（13 处）、`tests/e2e/test_ternary.cpp`、`tests/sema/test_semantic_analyzer.cpp`（11 处）、`tests/sema/test_template_registry.cpp`（1 处，先确认是否为注册名而非源码片段）、`tests/codegen/test_optional_result.cpp`
- Modify: `docs/spec/stdlib.md`、`docs/spec/abi.md`（历史 spec/plan 归档**不改**）
- Test: `tests/frontend/test_parser.cpp`（追加用例）

**Interfaces:**
- Produces: `optional<T>`/`result<T,E>` 为后续所有任务源码的拼写基准；Parser 对外行为不变（AST 类型仍是 `OptionalType`/`ResultType`，**不改类名**）。

- [ ] **Step 1: 写失败测试**（`tests/frontend/test_parser.cpp` 追加，沿用该文件现有 parse 辅助/断言风格）

```cpp
TEST(ParserBuiltinLowercase, OptionalLowercaseParses) {
    // `optional<int32> x;` 解析成功，变量类型 kind == TypeKind::Optional
}
TEST(ParserBuiltinLowercase, ResultLowercaseParses) {
    // `result<int32, bool> r;` 解析成功，kind == TypeKind::Result
}
TEST(ParserBuiltinLowercase, UppercaseNoLongerAType) {
    // `Optional<int32> x;` 解析失败或 sema 报未知类型（parse 期 parseBaseType 返回 nullptr）
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build -j && ctest --test-dir build -R "ParserBuiltinLowercase" --output-on-failure`
Expected: 前两个 FAIL（`Optional<` 才是类型，`optional` 是未知标识符）；第三个可能已 PASS（记录基线）。

- [ ] **Step 3: 改 Parser**——`Parser.cpp` 两处条件与文案：`(name == "Optional" || name == "Result")` → `(name == "optional" || name == "result")`，`if (name == "Optional")` → `if (name == "optional")`；错误文案 `'Optional<T>'`→`'optional<T>'`、`'Result<T, E>'`→`'result<T, E>'`；注释同步。`grep -rn '"Optional"\|"Result"' src/` 确认无残留特判。

- [ ] **Step 4: 迁移测试与文档**——用 grep 找到全部 `Optional<`/`Result<` 出现处（上述 Files 清单），CDD 源码片段与文档表格改为小写；`docs/spec/grammar.ebnf` 若有 Optional/Result 拼写一并改。

- [ ] **Step 5: 全量绿 + 提交**

Run: `ctest --test-dir build --output-on-failure`（1083+3 全绿；两个已知 flaky 隔离重跑）
Expected: PASS

```bash
git add src/frontend/Parser.cpp tests/frontend/test_parser.cpp tests/e2e/test_optional_result.cpp tests/e2e/test_ternary.cpp tests/sema/test_semantic_analyzer.cpp tests/sema/test_template_registry.cpp tests/codegen/test_optional_result.cpp docs/spec/stdlib.md docs/spec/abi.md docs/spec/grammar.ebnf
git commit -m "refactor(types): builtin polymorphic type names lowercase (optional/result)"
```

---

### Task 2: `str`/`string` 类型骨架 + 字面量重定型 + 基础互操作

> 本任务必须一次性打通「字面量 = `str`」的全通路，否则套件不绿：含 `str → char*` 隐式、print 的 str 实参、运行时 `==`/`!=`、CT 边界。

**Files:**
- Modify: `src/ast/Type.h`（TypeKind 枚举 :54 前加 `Str, String`；`SliceType` :188 后加 `StrType`/`StringType` 空成员类；TypeContext :346 加工厂）
- Modify: `src/ast/Type.cpp`（或 Type.h 内联，随 Slice 先例）：`getStrType()`/`getStringType()` 经 `m_types` 按 TypeKind 单例
- Modify: `src/frontend/Parser.cpp`：`isTypeStart`（:1535 区）加 `name == "str" || name == "string"` → true；`parseBaseType` identifier 分支（:1821 特判**之前**）加裸名分支 → `getStrType()`/`getStringType()`
- Modify: `docs/spec/grammar.ebnf`（类型名清单加 `str`/`string`）
- Modify: `src/sema/SemanticAnalyzer.cpp`：`visit(StringExprAST)` :1332 类型改 `getStrType()`；`typesCompatible` :270 加 `Str → Pointer(char)` 规则（在 `left->kind == right->kind` 泛化规则**之前**，避免它吞掉单向转换）；`typeToString` :341 区加 `"str"`/`"string"`
- Modify: `src/codegen/CodegenContext.cpp`：`getLLVMType` 加两 case——`Str` → named struct `"str" {ptr, i64}`（同 `"Slice"` 单例先例 :552）、`String` → `"string" {ptr, i64, i64}`；`layoutKey` :415 区加 `case TypeKind::Str: return "STR"; case TypeKind::String: return "STRING";`
- Modify: `src/sema/CompileTimeEvaluator.cpp`：`toLLVMType` :350 区加同两 case
- Modify: `src/ast/Expr.cpp`：`StringExprAST::codegen` :197 改为返回 `{CreateGlobalString(value), i64(len)}` 的 `"str"` struct 常量；`BinaryExprAST::codegen` :226 加 Eq/NotEq 对 Str 的分支（见 Step 3 算法）
- Modify: print 通路：`src/sema/SemanticAnalyzer.cpp` 的 `builtinPrintKind`/`lowerToString`（:1690 区）+ `src/ast/Expr.cpp` 的 `promotePrintArg`——str 实参 → `printf("%.*s", (int)len, ptr)`
- Test: `tests/frontend/test_parser.cpp`、`tests/sema/test_semantic_analyzer.cpp`、`tests/e2e/test_string.cpp`（新建）

**Interfaces:**
- Produces: `TypeKind::Str`/`TypeKind::String`、`StrType`/`StringType`、`TypeContext::getStrType()`/`getStringType()`（返回 `StrType*`/`StringType*`，单例指针）；`str` 的 LLVM 形状 = named struct `"str"`（`{i8*, i64}`），`string` = `"string"`（`{i8*, i64, i64}`）——T3/T4 的 codegen 依赖这两个名字。
- Produces: `str → char*` 隐式（sema 规则 + `CodegenContext::castValue` 加分支：Str→Pointer 提取 field 0）；`char* → str` **无**隐式（typesCompatible 不加反向规则，测试 pin）。

- [ ] **Step 1: 写失败测试**（骨架 + 字面量）

`tests/frontend/test_parser.cpp`：
```cpp
TEST(ParserStrTypes, StrTypeParses)       // `str s;` 声明可解析，kind == TypeKind::Str
TEST(ParserStrTypes, StringTypeParses)    // `string s;` → TypeKind::String
TEST(ParserStrTypes, StringLiteralIsStr)  // `"hi"` 表达式经 sema 后 type->kind == Str（或用 sema 测试，随文件既有风格）
```

`tests/e2e/test_string.cpp`（仿 `test_optional_result.cpp` 的 `runSource` fixture）：
```cpp
TEST_F(StringE2E, LiteralToCharPointer)   // `str s = "hi"; char* p = s; if (p[0] != 'h') return 1;` —— pin str→char* 隐式
TEST_F(StringE2E, PrintStrArg)            // `println("{}", "lit");` 可编译执行（print 的 Str 实参 kind）
TEST_F(StringE2E, StrCompareExec)         // if ("abc" == "abc") return 0; else return 1;（运行时 memcmp 路径）
TEST_F(StringE2E, StrNotEqualExec)        // "abc" != "abd" 为真
TEST_F(StringE2E, LiteralToCharPointer)   // `str s = "x"; print("{}:s", s);` 或 `char* p = s;` 可编译执行
```
（具体断言以现有 e2e 风格为准：`runSource` 返回 main 返回值。）

- [ ] **Step 2: 跑测试确认失败**

Run: `ctest --test-dir build -R "ParserStrTypes|StringE2E" --output-on-failure`
Expected: 全 FAIL（`str` 是未知标识符）。

- [ ] **Step 3: 实现骨架**——按 Files 清单逐点落。两个非显然算法钉死：

`str == str` codegen（`BinaryExprAST::codegen` 内，先于现有指针比较路径）：
```
取两侧 "str" value（load/lvalue 语义后）：
  lenEq = icmp eq (extractvalue a,1) (extractvalue b,1)
  不等 → 常量 0（Ne 取 1），不调用 memcmp（避免越界读对方缓冲区）
  相等 → memcmp(a.ptr, b.ptr, a.len) == 0（getOrInsertFunction("memcmp", i32(i8*,i8*,i64))），Eq 取结果、Ne 取反
```
print str 实参：`builtinPrintKind` 加 Str 分支 → 新 `PrintArgKind::Str`；`promotePrintArg` 对 Str 提取 (len 截 int, ptr) 两实参、格式串该占位记 `%.*s`（`buildPrintFormat` 同步）。

字面量常量：`StringExprAST::codegen` 返回 `ConstantStruct` 或 `CreateGlobalStringPtr` + `ConstantInt` 组装成 `"str"` 类型常量（LLVM named struct 常量用 `ConstantStruct::get`；若 named struct 未定形则 `create` 后组装）。

- [ ] **Step 4: 跑新测试通过，再全量**

Run: `ctest --test-dir build -R "ParserStrTypes|StringE2E" --output-on-failure` → PASS
Run: `ctest --test-dir build --output-on-failure`
Expected: 全绿。**此处是重定型破坏面收口点**：既有测试中字面量传 `char*`、`print`/`println("{}", "lit")`、CT-06 字符串比较等若报错，按 Global Constraints 修（补 castValue 分支/print kind/CT toLLVMType），不许回退字面量类型。

- [ ] **Step 5: CT 边界**——`CompileTimeEvaluator::evalBinary` :207 区 STR 分支：**删除 `BinaryOp::Add` case**（语言层不再有 `str + str`），保留 Eq/NotEq。测试（`tests/sema/test_compile_time.cpp` 追加）：

```cpp
TEST(CompileTimeStr, LiteralConcatNoLongerCT)  // constexpr 语境 `"a" + "b"` → 分析报错（CT 求值失败路径的诊断）
TEST(CompileTimeStr, LiteralEqStillCT)         // constexpr `"a" == "a"` 仍可折叠为 true
TEST(StrOperators, ConcatOperatorRejected)     // 非 CT 语境 `str a = "x"; str b = a + b;` → sema invalid operand（语言层无 str+str，spec §2.3）
```
先确认失败（Add 仍折叠），删 case，跑绿。

- [ ] **Step 6: 提交**

```bash
git add src/ast/Type.h src/ast/Type.cpp src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp src/sema/CompileTimeEvaluator.cpp src/codegen/CodegenContext.cpp src/ast/Expr.cpp tests/frontend/test_parser.cpp tests/sema/test_semantic_analyzer.cpp tests/sema/test_compile_time.cpp tests/e2e/test_string.cpp docs/spec/grammar.ebnf
git commit -m "feat(types): str/string builtin types; string literals are str (FMT-01/02/03)"
```

---

### Task 3: `str` 内建方法 + `str_from_c`

**Files:**
- Modify: `src/ast/Expr.h`：`MethodCallExprAST`（:286）加 `enum class BuiltinMethod { None, StrLen, StrCharCount, StrCharAt, StrCharLenAt, StringNew, StringDestroy, StringAppend, StringPush, StringLen, StringCapacity };` 与成员 `BuiltinMethod builtinMethod = BuiltinMethod::None;`；`CallExprAST` 加 `bool isStrFromC = false;`
- Modify: `src/sema/SemanticAnalyzer.cpp`：`visit(MethodCallExprAST)` :2382 顶部——(a) 静态构造：object 为 `VariableExprAST` 且 name=="string" 且 `lookup("string")` 非变量 → 仅接受 `new`，1 参、实参 kind==Str（char* 报错并提示 `str_from_c`）；置 `builtinMethod = StringNew`，`node.type = getStringType()`。(b) 实例方法：objType->kind==Str → 方法表（len/0参→usize；char_count/0→usize；char_at/1 参整数→char；char_len_at/1 参整数→usize；其他→E 风格错误 `no member named ... in str`）
- Modify: `src/sema/SemanticAnalyzer.cpp`：`visit(CallExprAST)` 内建区（:1779 print 先例旁）加 `str_from_c`：1 参 `Pointer(char)`（const 可）；实参为 `StringExprAST` 字面量时静态 UTF-8 验证（复用 T5 `support/Utf8`，先占位声明）；`node.isStrFromC = true; node.type = getStrType()`
- Modify: `src/codegen/CodegenContext.{h,cpp}`：合成 IR 辅助（缓存成员，internal linkage，跨模块单例名 `cdd.utf8.char_count`/`cdd.utf8.char_len_at`/`cdd.utf8.validate`，签名 `i64(i8*,i64)`/`i64(i8*,i64)`/`i1(i8*,i64)`；循环体按 Global Constraints 的 RFC 3629 规则生成）
- Modify: `src/ast/Expr.cpp`：`MethodCallExprAST::codegen` :668 顶部按 `builtinMethod` 分派（先于类方法逻辑）：StrLen→extractvalue 1；StrCharCount→call `cdd.utf8.char_count`；StrCharAt→GEP+load i8；StrCharLenAt→call `cdd.utf8.char_len_at`；对象实参按 rvalue/lvalue 统一 load 成 `"str"` value 后 extractvalue。`CallExprAST::codegen` 加 `isStrFromC` 分支：strlen（或 `cdd.utf8.validate`+panic）→ 组装 `"str"` value
- Test: `tests/e2e/test_string.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: T2 的 `"str"` LLVM 形状、`getStrType()`。
- Produces: `BuiltinMethod` 枚举（T4 用 `StringNew`/`StringDestroy`/`StringAppend`/`StringPush`/`StringLen`/`StringCapacity`）；`cdd.utf8.*` 三个 IR 辅助（T4/T5 复用）；str 方法签名——`len()/char_count()/char_len_at(i) → usize`、`char_at(i) → char`。

- [ ] **Step 1: 写失败测试**

`tests/e2e/test_string.cpp`：
```cpp
TEST_F(StringE2E, StrLen)               // `str s = "hello"; return (int)s.len();` → 5
TEST_F(StringE2E, StrCharCount)         // "héllo"（含 2 字节 é）→ char_count()==5、len()==6
TEST_F(StringE2E, StrCharAt)            // "héllo".char_at(1) == 0xC3（按字节）
TEST_F(StringE2E, StrCharLenAt)         // "héllo".char_len_at(1)==2、char_len_at(0)==1
TEST_F(StringE2E, StrFromCExec)         // `char* p = "abc"; str s = str_from_c(p); return (int)s.len();` → 3
TEST_F(StringE2E, StringNewStrict)      // sema：`string s = string.new("a");`（占位）→ 本任务只测 str 方法与 str_from_c；string.new 留 T4
```

`tests/sema/test_semantic_analyzer.cpp`：
```cpp
TEST(StrMethods, UnknownMethodDiag)     // `"a".bogus()` → "no member named 'bogus' in str"
TEST(StrMethods, ArityDiag)             // `"a".len(1)` → arity 错误
TEST(StrFromC, RejectsStrArg)           // `str_from_c("a")` 传 str 非法？——钉死：仅 Pointer(char) 接受；str 实参报错
```

- [ ] **Step 2: 跑测试确认失败**（`-R "StringE2E|StrMethods|StrFromC"`，全 FAIL）

- [ ] **Step 3: 实现**——按 Files 清单。`cdd.utf8.char_count` 循环 IR：`i=0; n=0; while(i<len){ i+=char_len_at(p,i); n+=1; } return n;`——`char_len_at` 本体：lead=p[i]；lead<0x80→1；0xC0~0xDF→2；0xE0~0xEF→3；0xF0~0xF7→4；否则 0。此处不验证续字节（视图已有 UTF-8 保证，§2.1 注：`char_len_at` 对无效序列返回 0 是防御，正常不可达）。

- [ ] **Step 4: 跑绿**（新测试 → 全量 ctest）

- [ ] **Step 5: 提交**

```bash
git add src/ast/Expr.h src/ast/Expr.cpp src/sema/SemanticAnalyzer.cpp src/codegen/CodegenContext.h src/codegen/CodegenContext.cpp tests/e2e/test_string.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(str): builtin methods (len/char_count/char_at/char_len_at) and str_from_c"
```

---

### Task 4: `string` 内存与方法（new/destroy/append/push/len/capacity/下标）+ `string → str` 视图

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`：`visit(MethodCallExprAST)` 加 String 实例方法表（destroy/0→void；append/1 参 Str→void；push/1 参整数→void；len/0、capacity/0→usize）；`typesCompatible` 加 `String → Str`（在 Str→Pointer 规则旁）；`visit(IndexExprAST)` :2140 区加 `TypeKind::String` → char lvalue
- Modify: `src/ast/Expr.cpp`：`MethodCallExprAST::codegen` 的 `BuiltinMethod` 分派补 String 各 case；`IndexExprAST::codegen` 加 String 分支（同 Slice：extractvalue ptr + GEP + load）
- Modify: `src/codegen/CodegenContext.cpp`：`castValue` 加 `String→Str`（extractvalue {0,1}）；`getOrInsertFunction` 的 libc：malloc/realloc/free/memcpy/strlen（printf 先例 :550）
- Test: `tests/e2e/test_string.cpp`、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: T3 的 `BuiltinMethod`、`cdd.utf8.*` 不需要；`"string"` LLVM 形状（T2）。
- Produces: 完整方法语义（下表即 spec §2.2 的钉死版，供 T6/T7 引用）：
  - `string.new(s: str) → string`：`cap = max(s.len, 1)`；`ptr = malloc(cap)`；`memcpy(ptr, s.ptr, s.len)`；`len = s.len`
  - `destroy()`：`free(ptr)` 后三字段全部清零（ptr=null, len=0, cap=0）
  - `append(s: str)`：`need = len + s.len`；`need > cap` 时 `cap' = max(cap*2, need)`、`realloc`；`memcpy(ptr+len, s.ptr, s.len)`；`len = need`
  - `push(c: char)`：同 append 单字节（`cap` 初次不足时 `max(cap*2, len+1)`，下限 1）
  - `len()`/`capacity()` → usize；`s[i] → char`（无边界检查，DEC-06 策略）
  - 变值方法（append/push/destroy）的 this 是 `string*`：对象为 lvalue → 用其地址；为 rvalue → 临时 alloca + store 后传址

- [ ] **Step 1: 写失败测试**

`tests/e2e/test_string.cpp`：
```cpp
TEST_F(StringE2E, NewLenExec)            // `string s = string.new("hello"); return (int)s.len();` → 5
TEST_F(StringE2E, AppendExec)            // new("ab").append("cd") → len 4、char_at(2)=='c'（经 s[i]）
TEST_F(StringE2E, AppendGrowthExec)      // 循环 append 使 realloc 多次触发，len 正确（如 append 100 次 "ab"）
TEST_F(StringE2E, PushExec)              // push('x') 三次 → len 3
TEST_F(StringE2E, CapacityExec)          // new("hi").capacity()==2；append("abc") 后 capacity()>=5
TEST_F(StringE2E, DestroyFrees)          // destroy() 后 len()==0、capacity()==0（字段清零可观测）
TEST_F(StringE2E, StringToStrView)       // `string s = string.new("ab"); str v = s; return (int)v.len();` → 2（隐式视图）
TEST_F(StringE2E, AppendAcceptsString)   // `string a = new("x"); string b = new("y"); a.append(b);` → len 2（String→Str 实参转换）
TEST_F(StringE2E, SubscriptExec)         // `string s = string.new("abc"); return (int)s[1];` → 'b'
```

`tests/sema/test_semantic_analyzer.cpp`：
```cpp
TEST(StringTypes, NoImplicitStrToString)  // `string s = "a";` → 报错（DEC-20：无隐式堆分配；文案含 string.new 提示）
TEST(StringTypes, NoImplicitCharPtrToString) // `char* p; string s = p;` → 报错
TEST(StringMethods, UnknownMethodDiag)    // `string.new("a").bogus()` → 诊断
```

- [ ] **Step 2: 跑测试确认失败**

- [ ] **Step 3: 实现**——按 Files 清单与上表语义；rvalue 链（`string.new(a).append(b)`）经临时 alloca 物化后可连续调用（T3 的 rvalue load 路径扩展为 rvalue → alloca+store → 传址）。

- [ ] **Step 4: 跑绿**（新测试 → 全量 ctest）

- [ ] **Step 5: 提交**

```bash
git add src/sema/SemanticAnalyzer.cpp src/ast/Expr.cpp src/codegen/CodegenContext.cpp tests/e2e/test_string.cpp tests/sema/test_semantic_analyzer.cpp
git commit -m "feat(string): memory management and mutation methods (FMT-02, DEC-20)"
```

---

### Task 5: UTF-8 验证 + E2015

**Files:**
- Create: `src/support/Utf8.h` / `src/support/Utf8.cpp`——`namespace cdd_utf8 { bool valid(std::string_view s); size_t seqLen(std::string_view s, size_t i); }`（sema 期用；规则 = Global Constraints；与 codegen 期 IR 辅助语义一致）
- Modify: `src/sema/Diagnostic.h`：enum 加 `SemInvalidUtf8`（SemAnnotationArgNotConstant 后）
- Modify: `src/sema/Diagnostic.cpp`：registry 加 `{DiagnosticCode::SemInvalidUtf8, "E2015", "invalid UTF-8 sequence"}`
- Modify: `src/sema/SemanticAnalyzer.cpp`：`visit(StringExprAST)` 调 `cdd_utf8::valid` → 失败 `emitError(DiagnosticCode::SemInvalidUtf8, ...)`（沿用该文件 emitError 带 code 的既有重载；若仅有 message 版则扩展）；Task 3 的 `str_from_c` 静态路径接 `cdd_utf8::valid`；非字面量运行时路径接 `cdd.utf8.validate`（validate=false → panic，复用 panic 内建 codegen 先例 Expr.cpp:515）
- Modify: `src/CMakeLists.txt`：SOURCES 加 `support/Utf8.cpp`（若该表是显式清单而非 GLOB，核对后决定）
- Test: `tests/sema/test_semantic_analyzer.cpp`、`tests/e2e/test_string.cpp`

**Interfaces:**
- Consumes: T3 的 `str_from_c` 骨架（此处补验证）。
- Produces: E2015 诊断；`cdd_utf8::valid`（T6 边界测试复用）。

- [ ] **Step 1: 写失败测试**

`tests/sema/test_semantic_analyzer.cpp`（沿用 hasError 风格）：
```cpp
TEST(Utf8Validation, OverlongLiteralE2015)   // str s = "\xC0\x80"; → E2015
TEST(Utf8Validation, LoneContinuationE2015)  // "\x80"
TEST(Utf8Validation, SurrogateE2015)         // "\xED\xA0\x80"（U+D800）
TEST(Utf8Validation, OutOfRangeE2015)        // "\xF5\x80\x80\x80"（> U+10FFFF）
TEST(Utf8Validation, RawStringValidated)     // r"..." 内含无效字节 → E2015
TEST(Utf8Validation, ValidMultiByteOk)       // "中🎉" 无诊断
```

`tests/e2e/test_string.cpp`：
```cpp
TEST_F(StringE2E, StrFromCRuntimeInvalid)   // 非字面量 char*（如经数组拼出 0xFF）→ str_from_c 运行时 panic（进程非零退出或返回约定值——按 panic 既有 e2e 断言风格，参照 test_compile_time.cpp 的 panic 测试）
```

- [ ] **Step 2: 跑测试确认失败**（E2015 未注册，诊断缺失）

- [ ] **Step 3: 实现**——`Utf8.{h,cpp}` + 诊断注册 + sema 两个验证点 + codegen validate/panic。UTF-8 解码表（两实现一致）：lead 0x80~0xBF（孤立续字节）→无效；0xC0/0xC1（超长）→无效；0xF8~0xFF→无效；续字节须 0x80~0xBF。

- [ ] **Step 4: 跑绿**（新测试 → 全量 ctest）

- [ ] **Step 5: 提交**

```bash
git add src/support/Utf8.h src/support/Utf8.cpp src/sema/Diagnostic.h src/sema/Diagnostic.cpp src/sema/SemanticAnalyzer.cpp src/ast/Expr.cpp src/CMakeLists.txt tests/sema/test_semantic_analyzer.cpp tests/e2e/test_string.cpp
git commit -m "feat(str): UTF-8 validation at literal and str_from_c points (E2015, FMT-04)"
```

---

### Task 6: 硬化与边界

**Files:**
- Test: `tests/e2e/test_string.cpp`、`tests/sema/test_semantic_analyzer.cpp`、`tests/frontend/test_parser.cpp`
- Modify: 实现（视 Step 1 暴露的缺口，预期改动点：`src/frontend/Parser.cpp`、`src/sema/SemanticAnalyzer.cpp`）

**Interfaces:**
- Consumes: T2~T5 全部。

- [ ] **Step 1: 写失败测试**（Review Focus 逐条 pin）

```cpp
// tests/sema/test_semantic_analyzer.cpp
TEST(TypeNames, TypeNameCannotBeVariable)   // `int32 str;` / `int32 string;` / `optional<int32> optional;` → 解析或 sema 拒绝（DEC-18 语义钉死）
// tests/e2e/test_string.cpp
TEST_F(StringE2E, DoubleDestroySafe)        // destroy(); destroy(); 不崩、len()==0（free(NULL) 路径）
TEST_F(StringE2E, EmptyStringInvariants)    // string.new("").len()==0、capacity()>=1、`char* p = s;` 后 p 非空指针（遍历 0 字节）
TEST_F(StringE2E, CharAtBeyondLen)          // s[s.len()] 读越界——DEC-06 未决：不加检查，测试 pin 为「能编译、结果未定义不做断言」→ 改为只编译通过的冒烟（不执行越界读）
// tests/frontend/test_parser.cpp
TEST(ParserStrTypes, EmptyLiteralIsStr)     // "" 字面量 → str 类型
```

- [ ] **Step 2: 跑测试确认失败**——逐条核实：`DoubleDestroySafe`/`EmptyStringInvariants` 若已过（记录为基线），`TypeNameCannotBeVariable` 预期 FAIL（当前 Parser 对裸 `str`/`string` 的类型特判在 `parseBaseType`，声明语句 `int32 string;` 里 `string` 可能被当类型名产生怪异解析而非清晰报错）。

- [ ] **Step 3: 修缺口**——按失败项最小修复：类型位置恒识别（isTypeStart/parseBaseType 已保证）；变量声明位置出现 `str`/`string` 作名字 → parse 期报「expected identifier, found type name」或等价清晰诊断（随既有 parse 错误机制）。

- [ ] **Step 4: 跑绿**（新测试 → 全量 ctest）

- [ ] **Step 5: 提交**

```bash
git add src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp tests/e2e/test_string.cpp tests/sema/test_semantic_analyzer.cpp tests/frontend/test_parser.cpp
git commit -m "harden(str/string): edge cases, type-name positions, empty-string invariants"
```

---

### Task 7: TODO 收口与文档

**Files:**
- Modify: `TODO.md`——`FMT-01/02/03/04` 置 `[x]` 带完成注记（日期 2026-10-08、ctest 计数、spec/plan 路径、`string → str` 隐式视图补钉说明）；`FMT-05` 标注部分完成（比较 `==`/`!=` 本轮完成；查找/分割/替换/连接挂 STD-10）；`FMT-02` 内注记 `String` 拼写钉死为小写 `string`；`TYP-26` 置 `[x]`；`P1-06` 行更新（format FMT-06~09 与 STD-10 另轮）；挂账区（若有）记：CT `+` 移除、`string` 不参与 constexpr、SSO 不做
- Modify: `Progress.md`——本轮回记录
- Test: 无新测试；验证 = 全量 ctest + TODO 勾选与实际一致

**Interfaces:**
- Consumes: T1~T6 全部完成态。

- [ ] **Step 1: 全量验证**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: 全绿（基线 1083 + 本轮新增约 35+）。

- [ ] **Step 2: 更新 TODO.md 与 Progress.md**（勾选项注记格式对齐 P1-05 先例：`[x] **FMT-01** ... 完成日期、要点、spec/plan 路径`）

- [ ] **Step 3: 提交**

```bash
git add -f TODO.md Progress.md
git add docs/superpowers/plans/2026-10-08-str-string.md
git commit -m "docs(todo): close P1-06 core (FMT-01~04, TYP-26); format and STD-10 deferred"
```
