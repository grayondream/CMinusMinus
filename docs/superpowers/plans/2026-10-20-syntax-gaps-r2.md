# 语法缺口收口（第二轮）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现第二轮语法审计发现的 10 项语言缺口（f16 类型关键字、restrict、UTF 前缀字面量、复合字面量、slice 字面量、类外方法定义、运算符重载 3 项、非类型模板参数作为值），全部 TDD 并同步文档。

**Architecture:** 各特性分别触及 Lexer/Parser/AST/sema/codegen 五层，但相互独立——每 Task 一个特性、独立 RED→GREEN→commit。语义决策以设计文档 `2026-10-20-syntax-gaps-r2-design.md` 为权威。

**Tech Stack:** C++17, LLVM 21, CMake, GTest, `build/bin/cmm`（CLI 探测）。

**Spec:** `docs/superpowers/specs/2026-10-20-syntax-gaps-r2-design.md`（已批准，commit `510edfb`）

## Global Constraints

- 全量回归：`cd build && cmake --build . -j$(nproc) && ctest -j8` 无失败；SpecConformance 12/12；工作区 clean。
- 测试自动收集：`tests/CMakeLists.txt` 用 GLOB，新测试文件需重新 configure（新增文件后重跑 cmake）。
- CLI 探测：`./build/bin/cmm -S file.c`（`-S` 输出 stdout、走 sema；`-fsyntax-only` 不跑 sema）。
- 每 Task：先写失败测试（RED），实现后转绿（GREEN），独立 commit 引用 TODO ID。
- commit 直接 master（项目既有约定）。
- 语言内嵌字符串类型 `string_view`（getStrType()，原名 `str`）；无 `int`/`float`/`double`，用定宽 int32/float64。
- 设计文档 §6 的裁定全部生效（禁转义只查直接转义、`(T[]){...}`=slice、operator raw-type mangle、窄 operator() 路径、局部遮蔽拒绝、`foo<N>()` 延后）。

## Review Focus

1. 复合字面量/slice 字面量禁转义：**直接**返回/取址/直赋全局的表达式拒绝；by-value 拷贝到全局允许；副本转义不追踪。
2. 运算符重载调用点：非聚合类型仍走内建（不误伤 `1+2`/`a && b` 短路）；mangledCallee 为空时 codegen 必须走内建分支。
3. `operator[]`/`operator()` 声明：parser 符号表扩展不破坏既有 `(T)x` cast 与 `f(x)` 调用解析。
4. UTF 前缀字面量：`u8"hi"` 类型为 `char8[]` 可退化 `char8*`；无前缀 `"hi"` 保持 `string_view`，互不干扰。
5. 非类型模板参数：N 作值替换后，类型参数（T）的 TypeVar 路径不受影响；模板体内局部 `int32 N` 遮蔽被拒。

---

### Task 1: f16/f32/f64/f128 类型关键字（TYP-04）

**Files:**
- Modify: `src/frontend/Lexer.cpp`（keywordMap，:122-125 后）
- Test: `tests/frontend/test_parser.cpp`、`tests/e2e/test_expr_literals.cpp`

**Interfaces:**
- Consumes: 既有 `TOKEN_FLOAT16/32/64/128` 与 parser 分支（Parser.cpp:1817-1820、1997-2009 已认）。
- Produces: 语言可写 `f16 x = 1.0f16;`、`f64 y = 2.0;` 等；字面量后缀 `1.0f16` 不受影响。

- [ ] **Step 1: 写失败测试**

在 `tests/frontend/test_parser.cpp` 加：`f16`/`f32`/`f64`/`f128` 作为类型关键字映射到 `TypeKind::Float16/32/64/128`（对照既有 `float16`/`float128` 断言写法）。
在 `tests/e2e/test_expr_literals.cpp` 加：`f16 a = 1.0f16; f32 b = 2.5f32; f64 c = 3.0; f128 d = 1.0f128; int32 main() { return (int32)a + (int32)b + (int32)c + (int32)d - 7; }` 期望 exit 0。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "Parser.*|ExprLiterals.*" --output-on-failure`
Expected: 新用例 FAIL（`f16` 仍是标识符 → unknown type）。

- [ ] **Step 3: Lexer keywordMap 注册 4 个别名**

`src/frontend/Lexer.cpp` keywordMap 补 `{"f16", TokenType::TOKEN_FLOAT16}`、`{"f32", TOKEN_FLOAT32}`、`{"f64", TOKEN_FLOAT64}`、`{"f128", TOKEN_FLOAT128}`（`float16`/`float128` 条目旁）。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上 ctest。
Expected: 新用例 PASS，全量无回归（尤其 `1.0f16` 后缀用例——最大匹配在 scanNumber 内，不经 keywordMap）。

- [ ] **Step 5: 提交**

```bash
git add src/frontend/Lexer.cpp tests/frontend/test_parser.cpp tests/e2e/test_expr_literals.cpp
git commit -m "feat(TYP-04): f16/f32/f64/f128 type keywords"
```

### Task 2: 指针后置 restrict 限定符（PAR-06）

**Files:**
- Modify: `src/ast/Type.h`（Type 加 isRestrict）、`src/frontend/Parser.cpp`（parseType 指针后缀循环，:478/:507 附近）、`src/sema/TemplateInstantiator.cpp`（Pointer rewrite，:25-32）
- Test: `tests/frontend/test_parser.cpp`、`tests/e2e/test_pointer.cpp`（或既有指针测试文件）

**Interfaces:**
- Consumes: `TOKEN_RESTRICT`（已注册）。
- Produces: `Type::isRestrict`（bool，仅 Pointer 置位）；`int32* restrict p` 声明可用；`T* const restrict` 组合可用。

- [ ] **Step 1: 写失败测试**

parser 测试：`int32* restrict` 解析为 Pointer 且 `isRestrict == true`；`int32* const restrict` 组合 `isRestrict == true && isConst == true`。
e2e 测试：`int32 f(int32* restrict p) { return *p; } int32 main() { int32 x = 5; return f(&x) - 5; }` 期望 exit 0。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "Parser.*" --output-on-failure`
Expected: FAIL（`expected ')' after parameter list`）。

- [ ] **Step 3: Type 加字段 + parseType 循环认 restrict**

`src/ast/Type.h`：`bool isRestrict = false;`。`Parser.cpp` 指针后缀限定符循环条件加入 `TOKEN_RESTRICT`，置位 `isRestrict`；`TemplateInstantiator::rewrite` Pointer case（:25-32）同步拷贝 `isRestrict`（防模板实例化丢字段）。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: PASS，无回归。

- [ ] **Step 5: 提交**

```bash
git add src/ast/Type.h src/frontend/Parser.cpp src/sema/TemplateInstantiator.cpp tests/frontend/test_parser.cpp tests/e2e/
git commit -m "feat(PAR-06): pointer postfix restrict qualifier"
```

### Task 3: u8/u16/u32 前缀字面量（TYP-05）

**Files:**
- Modify: `src/frontend/Token.h`（StringPrefix 枚举 + token 字段）、`src/frontend/Lexer.cpp`（scanToken/scanString 前缀识别）、`src/frontend/Parser.cpp`（StringExprAST 构造）、`src/ast/Expr.h`（StringExprAST 加 prefix）、`src/sema/SemanticAnalyzer.cpp`（字符串字面量类型映射）、`src/ast/Expr.cpp`（codegen）
- Test: `tests/frontend/test_lexer.cpp`、`tests/e2e/test_utf_prefix.cpp`（新建）

**Interfaces:**
- Consumes: 既有 `TOKEN_CHAR8/16/32` 类型（uint8/16/32 别名）。
- Produces: `enum class StringLiteralPrefix { None, U8, U16, U32 }`；`StringExprAST::prefix`；`u8"hi"` 类型 `char8[]`。

- [ ] **Step 1: 写失败测试**

lexer 测试：`u8"hi"`/`u16"hi"`/`u32"hi"` 各产出一个 `TOKEN_STRING`，前缀分别为 U8/U16/U32；无前缀 `"hi"` 前缀为 None。
e2e（新建 `tests/e2e/test_utf_prefix.cpp`）：`char8 a[] = u8"hi"; char16 b[] = u16"hi"; char32 c[] = u32"hi"; int32 main() { return (int32)a[0] + (int32)b[0] + (int32)c[0] - ('h'*3); }` 期望 exit 0（断言字符字节值与 `.len`）。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "Lexer.*|UtfPrefix.*" --output-on-failure`
Expected: 新用例 FAIL（`u8"hi"` 解析为 `u8` 标识符 + 字符串）。

- [ ] **Step 3: 词法 + AST + sema + codegen 全链路**

`Token.h` 加枚举与字段；`Lexer.cpp` scanToken 在 `r"` 分支旁加 `u8"`/`u16"`/`u32"` 识别（`peek()=='u' && isdigit(peekNext()) && peekNext2()=='"'`），复用 scanString 逻辑；`Parser.cpp` StringExprAST 构造带 prefix；`Expr.h` StringExprAST 加 `prefix` 字段 + 构造参数；sema 字符串字面量类型映射按 prefix 返回 `ArrayType(char8/16/32, n)`；codegen 生成对应定宽数组全局常量 + 退化指针（复用既有字符串字面量 codegen，改元素类型）。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；无前缀字符串回归全绿。

- [ ] **Step 5: 提交**

```bash
git add src/frontend/Token.h src/frontend/Lexer.cpp src/frontend/Parser.cpp src/ast/Expr.h src/ast/Expr.cpp src/sema/SemanticAnalyzer.cpp tests/frontend/test_lexer.cpp tests/e2e/test_utf_prefix.cpp
git commit -m "feat(TYP-05): u8/u16/u32 prefixed string literals"
```

### Task 4: C99 复合字面量（PAR-08）

**Files:**
- Modify: `src/ast/Expr.h`/`Expr.cpp`（新增 CompoundLiteralExprAST + codegen）、`src/frontend/Parser.cpp`（parsePrimary `(` 分支 :469-498 加 `{` 处理）、`src/sema/SemanticAnalyzer.cpp`（visit + 禁转义检查）
- Test: `tests/frontend/test_parser.cpp`、`tests/e2e/test_compound_literals.cpp`（新建）

**Interfaces:**
- Consumes: 既有聚合初始化 sema/codegen（struct/array/union）；`emitArrayToSliceDecay`。
- Produces: `CompoundLiteralExprAST`（type + 初始化列表）；`(int32[3]){1,2,3}`、`(struct S){...}`、`(int32[]){1,2,3}`（slice 形式）；块内=栈对象、文件级=静态常量；直接转义拒绝。

- [ ] **Step 1: 写失败测试**

parser 测试：`(int32[2]){1,2}` 解析为 CompoundLiteralExprAST（type=Array，含初始化列表）。
e2e（新建 `tests/e2e/test_compound_literals.cpp`）：
- 数组：`int32 main() { int32* p = (int32[3]){1,2,3}; return p[0]+p[2]-4; }` 期望 exit 0。
- struct：`(struct P){1,2}` 字段访问。
- slice 形式：`(int32[]){1,2,3}` 退化为 slice 后 `.len == 3`。
- 禁转义：`return (int32[3]){1,2,3};` 与 `&(int32[3]){1,2,3}` 报错（exit 非 0 / 诊断断言）。
- by-value 拷贝到全局允许：`struct P g; g = (struct P){1,2};` 编译通过。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "Parser.*|CompoundLiteral.*" --output-on-failure`
Expected: FAIL（`(int32[]){...}` 现按 cast 处理报错）。

- [ ] **Step 3: AST + parser + sema + codegen**

`Expr.h` 新增 `CompoundLiteralExprAST`（持 `Type*` + `std::vector<std::unique_ptr<ExprAST>> initList`，`Type* literalType` 由 sema 解析）；`Parser.cpp` parsePrimary `(` 分支 cast 探测成功后，若 `check(TOKEN_LBRACE)` 则解析初始化列表构造复合字面量（`(` 已消费，直接解析 `{...}`）；sema `visit` 走既有聚合初始化校验（struct/array/union），slice 形式走 Task 5 的 slice 初始化路径；标量 `(int32){5}` 退化为普通 cast；`type = 目标类型`。禁转义：sema 检查 UnaryExprAST(AddressOf) 的 operand 是复合字面量、ReturnStmtAST 表达式是复合字面量、全局初始化/赋值 LHS 为全局时 RHS 是 slice/数组复合字面量 → 报错。codegen：块内 alloca + 初始化存储；文件级全局常量。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；`(T)x` cast 回归全绿。

- [ ] **Step 5: 提交**

```bash
git add src/ast/Expr.h src/ast/Expr.cpp src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp tests/frontend/test_parser.cpp tests/e2e/test_compound_literals.cpp
git commit -m "feat(PAR-08): C99 compound literals"
```

### Task 5: slice 字面量初始化 `T[] s = {1,2,3}`（PAR-08）

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（聚合初始化 Slice case，:3717 附近）、`src/ast/Expr.cpp`（初始化列表 codegen Slice 分支）
- Test: `tests/e2e/test_slice_literal.cpp`（新建）

**Interfaces:**
- Consumes: Task 4 的 slice 初始化路径（若已建）；`emitArrayToSliceDecay`；Slice 类型.
- Produces: `int32[] s = {1,2,3};` 合法，`s.len == 3`、`s[0]` 可读写；返回/存全局拒绝。

- [ ] **Step 1: 写失败测试**

e2e（新建 `tests/e2e/test_slice_literal.cpp`）：`int32[] s = {1,2,3}; int32 main() { s[1] = 9; return s[0] + s[1] + s[2] - 13; }` 期望 exit 0（1+9+3=13）。
禁转义：`int32[] make() { int32[] s = {1,2}; return s; }` 报错（exit 非 0）。
元素类型不匹配：`int32[] s = {1.0, 2};` 报错。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "SliceLiteral.*" --output-on-failure`
Expected: FAIL（`initializer list cannot initialize type 'slice'`）。

- [ ] **Step 3: sema 聚合初始化 Slice 分支 + codegen**

sema 聚合初始化分析处（:3717 的 else 分支前）加 Slice case：元素类型 = Slice 元素，逐元素校验（复用既有元素初始化路径），`len` = 列表长度，类型 = 原 Slice 类型；为承载存储，在函数体声明区注册伴随匿名数组（或让 codegen 侧处理）。codegen：alloca 定长匿名数组（元素类型 × len）→ 逐元素 store → `emitArrayToSliceDecay` 组装 `{ptr,len}`。禁转义：ReturnStmt 表达式是 slice 字面量变量 / 赋全局 → 拒绝。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；既有 slice 测试回归全绿。

- [ ] **Step 5: 提交**

```bash
git add src/sema/SemanticAnalyzer.cpp src/ast/Expr.cpp tests/e2e/test_slice_literal.cpp
git commit -m "feat(PAR-08): slice literal initialization"
```

### Task 6: 类外方法定义 `S::get() {...}`（PAR-03）

**Files:**
- Modify: `src/ast/Decl.h`（FunctionDeclAST 加 className）、`src/frontend/Parser.cpp`（parseDeclarationAsType `IDENTIFIER :: IDENTIFIER (` 形态）、`src/sema/SemanticAnalyzer.cpp`（visit(FunctionDeclAST) className 处理）、`src/ast/Decl.cpp`（codegen mangle）
- Test: `tests/e2e/test_out_of_class_method.cpp`（新建）、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: 既有类内方法声明（parseClassDecl 方法表）；`mangleMethod`（或等价 mangle 函数）。
- Produces: `FunctionDeclAST::className`（空 = 普通函数）；`RetType S::m() {...}` 类外定义，签名一致绑定、类内未声明拒绝。

- [ ] **Step 1: 写失败测试**

e2e（新建 `tests/e2e/test_out_of_class_method.cpp`）：`struct S { int32 v; int32 get(); }; int32 S::get() { return v; } int32 main() { S s; s.v = 5; return s.get() - 5; }` 期望 exit 0。
sema 负例：`struct S { int32 v; int32 get(); }; int32 S::get(int32 x) { return x; }`（签名不一致）报错；`int32 S::nope() { return 0; }`（类内未声明）报错；`int32 X::f() { return 0; }`（类不存在）报 E2031。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "OutOfClass.*" --output-on-failure`
Expected: FAIL（`expected ';' after variable declaration`）。

- [ ] **Step 3: parser + sema + codegen**

`Decl.h` FunctionDeclAST 加 `std::string className;`。`Parser.cpp` parseDeclarationAsType 在 parseType 后（operator/函数指针分支后、普通标识符分支前）：若 `IDENTIFIER :: IDENTIFIER (`，className=前段，方法名=后段，走 parseFunctionDecl。sema `visit(FunctionDeclAST)`：className 非空时查类（不存在 E2031），类内找同名方法（不存在或签名不一致报诊断），绑定定义体（更新方法定义）。codegen：mangle 用类前缀（与类内定义一致）。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；既有类内方法回归全绿。

- [ ] **Step 5: 提交**

```bash
git add src/ast/Decl.h src/frontend/Parser.cpp src/sema/SemanticAnalyzer.cpp src/ast/Decl.cpp tests/e2e/test_out_of_class_method.cpp tests/sema/
git commit -m "feat(PAR-03): out-of-class method definitions"
```

### Task 7: 一元 operator 重载调用点（SEM-17）

**Files:**
- Modify: `src/ast/Expr.h`/`Expr.cpp`（UnaryExprAST 加 mangledCallee + codegen 调用）、`src/sema/SemanticAnalyzer.cpp`（visit(UnaryExprAST) 查一元 operator；visit(PostfixIncDecExprAST) 查前缀/后缀）、`src/sema/TemplateInstantiator.cpp`（UnaryExprAST clone 拷贝 mangledCallee）
- Test: `tests/e2e/test_operator_unary.cpp`（新建）

**Interfaces:**
- Consumes: `isOverloadOperand`；`mangleFunction`；`OverloadSet::resolve`；`getOperatorMangledName`（模式）。
- Produces: `UnaryExprAST::mangledCallee`；`-v`/`~v`/`++v`/`v++` 对聚合类型走重载函数。

- [ ] **Step 1: 写失败测试**

e2e（新建 `tests/e2e/test_operator_unary.cpp`）：
- `struct V { int32 x; }; struct V operator-(struct V a) { struct V r; r.x = -a.x; return r; }` + `v = -v` 期望 exit 0。
- `struct V operator~(struct V a) {...}` + `r = ~v`。
- 前缀 `operator++`：`struct V operator++(struct V a) { a.x = a.x+1; return a; }` + `++v` 期望 `v.x == 2`。
- 后缀 `operator++(int32)`：`struct V operator++(struct V a, int32) { a.x = a.x+1; return a; }` + `v++` 期望调用生效。
- 非聚合回归：`int32 x = 1; x = -x;` 仍走内建。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "OperatorUnary.*" --output-on-failure`
Expected: FAIL（`invalid operand to unary '-'` / `invalid operand to '++'`）。

- [ ] **Step 3: sema 一元查找 + codegen 调用**

sema `visit(UnaryExprAST)`：switch 前，若 `isOverloadOperand(operandType)`，按 op 查 `operator-`/`operator+`/`operator~`/`operator!`（`mangleFunction(opName, {operandType})`），查到则 `node.mangledCallee` + `node.type = 返回类型`；未查到回退内建。`visit(PostfixIncDecExprAST)`：聚合类型查 `operator++`/`operator--`（mangle 带 `{operandType, int32}` 后缀形态）。`Expr.h` UnaryExprAST 加 `mangledCallee`；`Expr.cpp` UnaryExprAST::codegen 若 mangledCallee 非空 CreateCall（单参，emitRValue）；PostfixIncDecExprAST::codegen 同理（对象 + 哑参 0）。`TemplateInstantiator` UnaryExprAST clone 拷 mangledCallee。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；一元内建回归全绿。

- [ ] **Step 5: 提交**

```bash
git add src/ast/Expr.h src/ast/Expr.cpp src/sema/SemanticAnalyzer.cpp src/sema/TemplateInstantiator.cpp tests/e2e/test_operator_unary.cpp
git commit -m "feat(SEM-17): unary operator overloading call sites"
```

### Task 8: 二元 operator 集合收敛（SEM-17）

**Files:**
- Modify: `src/sema/SemanticAnalyzer.cpp`（visit(BinaryExprAST) operatorName switch :1771 + getOperatorMangledName :499）
- Test: `tests/e2e/test_operator_extended.cpp`（新建）

**Interfaces:**
- Consumes: Task 7 的 mangledCallee 通道（codegen 已通用）；`BinaryOp::Mod/And/Or/BitAnd/BitOr/BitXor/Shl/Shr`。
- Produces: 聚合类型 `a % b`/`a << n`/`a && b`/`a & b`/`a | b`/`a ^ b`/`a >> n`/`a || b` 走重载函数。

- [ ] **Step 1: 写失败测试**

e2e（新建 `tests/e2e/test_operator_extended.cpp`）：
- `operator%`：`r = a % b` 期望 exit 0。
- `operator<<`：`r = a << 3` 期望 `r.x == 8`。
- `operator&&`：重载后两实参都求值（非短路）——用带副作用的实参验证返回结果。
- `operator&`/`operator|`/`operator^`/`operator>>`/`operator||` 各一。
- 非聚合回归：`int32 a=6; return a % 4;` 仍走内建（=2）。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "OperatorExtended.*" --output-on-failure`
Expected: FAIL（`invalid operands to binary '%'` 等）。

- [ ] **Step 3: sema switch 补 8 种**

`visit(BinaryExprAST)`（:1771 的 operatorName switch）与 `getOperatorMangledName`（:499）各补：`Mod→"operator%"`、`And→"operator&&"`、`Or→"operator||"`、`BitAnd→"operator&"`、`BitOr→"operator|"`、`BitXor→"operator^"`、`Shl→"operator<<"`、`Shr→"operator>>"`。codegen 走既有 mangledCallee CreateCall 分支（`&&`/`||` 重载后天然非短路——mangledCallee 分支优先于短路分支）。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；短路回归全绿（内建 `&&`/`||` 未设 mangledCallee 仍走短路分支）。

- [ ] **Step 5: 提交**

```bash
git add src/sema/SemanticAnalyzer.cpp tests/e2e/test_operator_extended.cpp
git commit -m "feat(SEM-17): converge binary operator overload set"
```

### Task 9: operator[] / operator() 重载（SEM-17 扩展）

**Files:**
- Modify: `src/frontend/Parser.cpp`（operator 符号表加 `[`/`(`，:2843-2868）、`src/ast/Expr.h`/`Expr.cpp`（ArrayAccessExprAST 加 mangledCallee + codegen 调用；CallExprAST 加 calleeExpr + codegen 调用）、`src/sema/SemanticAnalyzer.cpp`（visit(ArrayAccessExprAST) / visit(CallExprAST) 查 operator）
- Test: `tests/e2e/test_operator_extended.cpp`（追加）、`tests/frontend/test_parser.cpp`

**Interfaces:**
- Consumes: `isOverloadOperand`；`mangleFunction`；Task 7/8 的 mangledCallee 模式。
- Produces: `operator[]`（2 参 mangle：obj,idx）、`operator()`（1+N 参 mangle：obj,args...）；`v[i]`/`v(args)` 对聚合走重载。

- [ ] **Step 1: 写失败测试**

parser 测试：`struct V { int32 operator[](int32 i); };` 声明解析成功（函数名 `operator[]`）；`struct V { int32 operator()(int32 a, int32 b); };` 解析成功。
e2e（追加 `tests/e2e/test_operator_extended.cpp`）：
- `operator[]`：`struct V { int32 data[4]; }; int32 operator[](struct V v, int32 i) { return v.data[i]; }` + `v[2]` 期望返回 `v.data[2]`。
- `operator()`：`int32 operator()(struct V v, int32 a, int32 b) { return a+b; }` + `v(1,2)` 期望 3。
- 内建数组下标回归：`int32 a[3]; a[0]=1; return a[0];` 仍走内建。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "OperatorExtended.*|Parser.*" --output-on-failure`
Expected: FAIL（`expected operator symbol after 'operator'`）。

- [ ] **Step 3: parser 符号表 + AST + sema + codegen**

`Parser.cpp` operator 符号表加 `TOKEN_LBRACKET → "[]"`；`TOKEN_LPAREN → "()"`（消费 `()` 后进 parseFunctionDecl 参数表）。`Expr.h` ArrayAccessExprAST 加 `std::string mangledCallee;`；CallExprAST 加 `std::unique_ptr<ExprAST> calleeExpr;`（与字符串 callee 二选一）。sema `visit(ArrayAccessExprAST)`：array 类型为聚合且非 Array/Pointer/Slice 时，查 `operator[]`（`mangleFunction("operator[]", {arrayType, indexType})`），设 mangledCallee + 返回类型；否则维持内建。sema `visit(CallExprAST)`：若 calleeExpr 存在且类型聚合，查 `operator()`（`mangleFunction("operator()", {objType, argTypes...})`）。codegen：ArrayAccessExprAST::codegen 若 mangledCallee 非空 CreateCall(array, index)；CallExprAST::codegen 若 calleeExpr 走 operator 分支 CreateCall(obj, args...)。parser `(x)` 与 `f(x)` 路径不动（calleeExpr 仅在解析到非标识符首 token 的调用时设置）。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；cast/调用回归全绿。

- [ ] **Step 5: 提交**

```bash
git add src/frontend/Parser.cpp src/ast/Expr.h src/ast/Expr.cpp src/sema/SemanticAnalyzer.cpp tests/e2e/test_operator_extended.cpp tests/frontend/test_parser.cpp
git commit -m "feat(SEM-17): operator[] / operator() overloading"
```

### Task 10: 非类型模板参数作为值（GEN-02）

**Files:**
- Modify: `src/sema/TemplateInstantiator.cpp`（cloneExpr VariableExprAST case :103-109 加 value 替换）、`src/sema/SemanticAnalyzer.cpp`（模板体局部遮蔽值参数拒绝）
- Test: `tests/e2e/test_non_type_param_value.cpp`（新建）、`tests/sema/test_semantic_analyzer.cpp`

**Interfaces:**
- Consumes: `TemplateInstantiator::m_valueArgs`（name→value，构造已收）；`NumberExprAST`。
- Produces: `template<int32 N> int32 c() { return N; }` 实例化 `c<5>()` 返回 5；N 可在算术/常量位置使用；局部遮蔽拒绝。

- [ ] **Step 1: 写失败测试**

e2e（新建 `tests/e2e/test_non_type_param_value.cpp`）：
- 函数模板：`template<int32 N> int32 c() { return N; } int32 main() { return c<5>() - 5; }` 期望 exit 0。
- N 参与算术：`template<int32 N> int32 d() { return N * 2; }` + `d<7>()` 期望 14。
- 既有回归：`template<typename T, usize N> struct Arr { T data[N]; };` + `Arr<int32,4>` 仍可用。
sema 负例：`template<int32 N> int32 e() { int32 N = 3; return N; }` 报局部遮蔽拒绝。

- [ ] **Step 2: 运行确认 RED**

Run: `cd build && cmake --build . -j$(nproc) && ctest -R "NonTypeParam.*" --output-on-failure`
Expected: FAIL（`E2001: undeclared identifier 'N'`）。

- [ ] **Step 3: cloneExpr 替换 + 遮蔽拒绝**

`TemplateInstantiator::cloneExpr` VariableExprAST case：name 在 `m_valueArgs` 中 → 克隆为 `NumberExprAST(value, LiteralKind::Int)`（替换表达式引用）。sema 模板体作用域注册：声明局部变量时若 name 命中当前模板的 valueArgMap 键 → 报诊断（`shadowing of template value parameter 'N' is not allowed`）。

- [ ] **Step 4: 运行确认 GREEN**

Run: 同上。
Expected: 新用例 PASS；类型参数路径（TypeVar 不在此表）回归全绿。

- [ ] **Step 5: 提交**

```bash
git add src/sema/TemplateInstantiator.cpp src/sema/SemanticAnalyzer.cpp tests/e2e/test_non_type_param_value.cpp tests/sema/
git commit -m "feat(GEN-02): non-type template params usable as values"
```

### Task 11: 文档同步

**Files:**
- Modify: `docs/spec/grammar.ebnf`、`docs/spec/keywords.md`、`docs/spec/semantics.md`、`TODO.md`、`Progress.md`

**Interfaces:**
- Consumes: 各 Task 的最终行为。
- Produces: 规范/进度文档与实现一致。

- [ ] **Step 1: grammar.ebnf 更新**

`f16/f32/f64/f128` 类型关键字去 `[plan]` 标 `[impl]`；pointer-suffix 补 `restrict`；复合字面量产生式补 `[impl]`；`u8/u16/u32` 前缀字面量产生式补 `[impl]`；类外方法定义说明；operator 可重载集合注记（17 二元 + 一元 + `[]`/`()`）。

- [ ] **Step 2: keywords.md + semantics.md 更新**

`keywords.md`：`f16/f32/f64/f128` `[plan]`→`[impl]`；`restrict` 状态更新；`u8/u16/u32` 记 TYP-05。`semantics.md`：SEM-17 可重载集合收敛表；复合字面量/前缀字面量/slice 字面量存储与禁转义；非类型模板参数常量语义。

- [ ] **Step 3: TODO.md + Progress.md 更新**

`TODO.md`：TYP-04/PAR-06/TYP-05/PAR-08/PAR-03/SEM-17/GEN-02 更新状态 + 进度快照（含测试数）；INH-07（虚函数）与 GEN-10（可变参数模板）加"用户确认维持 Non-goal"注记。`Progress.md` 追加本轮完成记录。

- [ ] **Step 4: 提交**

```bash
git add docs/spec/grammar.ebnf docs/spec/keywords.md docs/spec/semantics.md TODO.md Progress.md
git commit -m "docs: round-2 syntax gaps — grammar/keywords/semantics/TODO/Progress"
```



