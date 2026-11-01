# My_LLVM_C / SafeModern C 1.0 详细待办（工程 backlog）

> 本文件是唯一的需求与进度追踪清单。每条以稳定 ID 编号，便于 issue/commit 引用。
>
> - **状态**：`[ ]` 未开始 ｜ `[~]` 进行中 ｜ `[x]` 已完成 ｜ `[!]` 阻塞
> - **标记**：`(已实现)` = 代码已有但需补测试/清理；`(新)` = 本轮审计新增；`(改)` = 需修订既有条目
> - 提交时在 message 中引用 ID，例如 `feat(LEX-04): ...`
> - 所有特性必须映射到可预测的 C 内存模型与 ABI；无隐藏分配/控制流；comptime 无运行时开销

> **进度快照（2026-09-28）**：P0-01 ~ P0-08 全部完成；转换系统 **TYP-09/20/22/25、AGG-14/15** 完成（enum 显式底层类型、enum 强类型、整数提升 + 常规算术转换）；**LEX-13/17** 词法诊断（E0001 非法字符、E0002 未闭合字面量、E0003 整型溢出、E0004 分隔符位置）与错误恢复完成，**LEX-15** 字面量后缀与默认/基础类型映射在词法层完成，**LEX-05** 浮点指数完成；**P1-08** 部分（`static_cast`/`reinterpret_cast` 已完成，内联 `asm` 待定）。数值类型已统一为定宽集合：删除 `int`/`float`/`double` 关键字与 `TypeKind::Int/Float/Double`，新增 `float16`/`float128`，规范见 `docs/spec/fixed-width-types.md`。**TYP-04** 部分完成：f16/f128 在 x86-64 上全语义可用，print 全格式接入，varargs 提升对齐 C23。**AGG-17/AGG-03** union 补齐：匿名成员提升、typedef 聚合成员访问修复、行为基线固化。**PAR-04/SEM-04/DEC-01** class 访问段落地：默认 private/public 冻结、E2009 访问诊断、既有测试迁移。当前全量 **751** 测试通过，工作区干净。

> **进度快照（2026-10-11）**：环境恢复 + 全量构建验证轮。工程迁到 `/media/nvm1/workspace/My_LLVM_C` 后清理了全部指向已失效 `/media/nvme0n1` 的 in-source CMake 残留与 `build/` 缓存，按 `vcpkg.json` 装齐依赖（`llvm-dev`=21.1.8、`libcxxopts-dev`、`libgtest-dev`，`libspdlog-dev` 原已具备），`scripts/build.sh` 全量重建成功。修正 `tests/frontend/test_lexer.cpp::AsmKeyword.LexesAsmVolatileTemplate`（**LEX-12** 未提交 WIP）的错误期望：token 流实为 `asm volatile ( "nop" ) ;`，字符串在 `tokens[3]`（`tokens[2]` 是 `(`），且字符串 token 的 lexeme 含两端引号（由 `Parser` 剥除）——仅改测试，未动 lexer。当前全量 **1184** 测试 0 失败（2 个为既有 `GTEST_SKIP`；`ModuleVisibilityTest.NonExportedFunctionIsHidden` 为已知并行 LLJIT flaky，隔离重跑通过）。**LEX-12 仍为 `[ ]`**：本轮只到词法 token 层，parser/codegen 的 `asm` 未实现；根目录 `bin/`、`lib/`、`a.out` 为旧路径产物，待清理。

> **进度快照（2026-10-11）**：**P1-08 内联 `asm` 全链路完成**（LEX-12/PAR-16/CG-10/DEC-09 全部 `[x]`）。词法 `TOKEN_ASM`；解析 `AsmOperand`/`AsmExprAST` + `parseAsmExpr()`（至多三段操作数、模板/约束须字符串字面量）；sema 校验 outputs 左值标量/inputs 标量/约束非空（E2023）+ 模板 `%` 转义（E2024）+ 拒绝内存类输出（C1）；codegen 用 `llvm::InlineAsm`（AD_ATT）——实证 LLVM 21 与 GCC 三处差异并适配：输出映射 call 返回值（单值/struct 逐位回写）、`+r`→`=r`+尾部 tied 约束（编号对齐）、模板翻译 `$`→`$$`/`%%`→`%`/`%N`→`$N`；零输出 asm 隐式 volatile（GCC）。e2e 12 用例 LLJIT 执行通过。**顺带修复既有潜在 bug**：`CodegenContext::takeContext()` 把 context 移给 LLJIT（编译后销毁）后 IRBuilder 的 DebugLoc 悬垂 → 析构 `MetadataTracking::untrack` 间歇 UAF（gdb 关 ASLR 下 `InlineAsmE2E.ReadWriteOperand` 15/15 崩），修复为移出前清 DebugLoc，gdb 20/20 不崩；32 处测试 harness 共用该 API 一并受益。当前全量 **1212** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。

> **进度快照（2026-10-11，I4 收口）**：**string 拥有型值语义 + `string_view` 硬改名完成**（终审 I4「string 值拷贝别名双 free」根治）。`str` → `string_view`（DEC-18 硬改名先例，TypeKind::Str 内部名不动）；`string` 升级 C++ `std::string` 式拥有型值——赋值/初始化/传参/返回深拷贝活值、移动新鲜值（`string.new`/返回 string 的调用 rvalue）、赋值自动释放旧缓冲、聚合（struct 含 string 字段）字段感知拷贝；惰性合成 `smc.string.copy`/`smc.string.assign` helper。spec `docs/superpowers/specs/2026-10-11-string-ownership-design.md`，plan `docs/superpowers/plans/2026-10-11-string-ownership.md`。当前全量 **1229** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。

> **进度快照（2026-10-12，I4 收口轮 found issues）**：3 项遗留缺陷全部修复——①`true`/`false` 改为 bool 字面量（`BoolLiteralExprAST`，spec keywords.md「布尔字面量」，重载解析可匹配 bool 形参；`isIntegerType` 含 Bool 故算术/赋值零 ripple）；②未初始化 string/含 string 聚合局部零初始化（扩展 TYP-12 D3 slice 先例，D7 fresh 赋值 free 守卫不再见野指针）；③聚合内数组/optional/result 含 string 的字段感知拷贝（`aggregateHasStringField`/`emitAggregateStringCopy` 扩展 Array/Optional/Result）。当前全量 **1236** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。新遗留：optional/result 嵌套 brace-init codegen 缺陷、optional/result valid→invalid 赋值旧缓冲泄漏（边缘）。

> **进度快照（2026-10-12，新遗留项处理）**：**optional/result 嵌套 brace-init codegen 缺陷修复**（`a0b1887`）——根因 `visit(InitializerListExprAST)` 只 sema 首元素，嵌套列表（struct 字段 optional/result 的 `{ { true, string.new("data") } }`）里 `string.new` 的 MethodCallExprAST 未解析 → codegen `unknown variable: string`；修复为递归所有元素。2 个新 e2e（optional/result 嵌套 brace-init）RED→GREEN。当前全量 **1238** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。剩余遗留：仅 optional/result valid→invalid 聚合赋值旧缓冲泄漏（边缘，未处理）。

> **进度快照（2026-10-12，新遗留项收尾）**：**optional/result valid→invalid 聚合赋值旧缓冲泄漏修复**（`42da409`）——根因 `emitAggregateStringCopy` 只按**源**有效位分支，目标中"变为失效"的槽位从不释放：optional valid→invalid、result ok→err / err→ok 时旧 string 缓冲泄漏且槽位残留旧内容（破坏"valid⟹value 含缓冲"不变量）。修复：读旧 valid/ok 位，槽位失效时 `emitStringValueRelease`/`emitAggregateStringRelease`（镜像拷贝递归，Struct/Class/Array/Optional/Result 全支持）free+置零，再转换时 assign helper 见 null 不双 free。6 个新 e2e（valid→invalid 释放 ×4、err→ok、再转换安全）RED→GREEN。当前全量 **1244** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。**新遗留清单已清空**。

> **进度快照（2026-10-15，P1-07 位域完成）**：**C 位域全链路完成**（PAR-10/SEM-12/AGG-06/CG-09/DEC-08 全部 `[x]`，plan `docs/superpowers/plans/2026-10-13-bitfields.md` 6 任务 TDD）。语法 `T name : N`（struct/class/union/匿名成员）；sema E2025/26/27（基类型/宽度/取址）；布局 System V 存储单元（非 packed）与 GCC packed 连续位流区域模型（`[N x i8]`，零宽按声明类型自然边界取整，尺寸与 gcc 实测核对）；RMW 读写 + 复合赋值/++/--；全局/局部花括号初始化位域感知（无名成员不消费初始值）；union/class（继承链）位域；offset_of 位域拒绝。整分支评审修复：union 位域被当普通字段（Parser/Sema 注册 + unitBits + 全局常量掩码）、派生聚合物化 GEP +1、位宽溢出防回绕。当前全量 **1283** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。已知限制：static 局部变量不支持（DEC 存储类范围）；packed 位域 RMW 用整数自然对齐（x86-64 容忍，严格对齐目标待 Align(1)）。

> **进度快照（2026-10-16，P1-07 指定初始化器 + 柔性数组完成）**：**C99 指定初始化器 + 柔性数组全链路完成**（PAR-13/AGG-04/05 `[x]`，plan `docs/superpowers/plans/2026-10-16-designated-init-flexarray.md` 5 任务 TDD + 终审修复轮）。`.name`/`.a.b`/`[i]` 设计符（parser 链解析 + `indexExpr` 常量表达式承载）；sema `resolveDesignators` 语义目标链（继承链+E2009/union 槽 0/位域/Optional·Result 伪字段拒绝）、数组大小 C99 推断（含多链 `[1][2]=5`）；局部整块补零 + 按链写槽（typedef 数组含）；全局常量递归累积（后写覆盖/位域掩码 OR/基类下降）；乱序/重复/混排全 C99。FAM：`FieldInfo::flexibleArray` 标记 + 约束（仅 struct 尾、非匿名/位域、class/union/初始化拒绝）+ `sizeof` 不含 FAM（gcc 核对）+ malloc 惯用法 + 按值拷贝只拷固定部分。终审修复：物化路径 base 双计数 GEP、全局常量丢基类/多链、union 后写覆盖、typedef 数组补零、多链推断。当前全量 **1326** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。P1-07 剩余：匿名类型定义（PAR-12）、lambda（PAR-11/CG-11/DEC-10）。

> **进度快照（2026-10-17，P1-07 匿名类型定义完成）**：**匿名 class/struct/union 类型定义全链路完成**（PAR-12 `[x]`，plan `docs/superpowers/plans/2026-10-17-anonymous-types.md` 3 任务 TDD + 终审）。每个匿名定义获得唯一合成名 `__anon_N`（`TypeContext::nextAnonId`，C 语义独立类型）；`struct/union/class { ... } v, v2;` 全局/局部/嵌套/数组/指针全通路；匿名 class 字段/方法/访问段注册（`attachedVars` 挂载聚合 AST，sema/codegen 先类型后变量）；**名义身份强制**——两次独立匿名定义赋值/初始化/实参/返回/比较互斥（`checkAssignmentTypes`/`typesCompatible` 同 kind 收紧，保派生→基 INH-04 切片，顺带修复命名 struct 交叉赋值 pre-existing 洞）；AGG-03 匿名成员提升、AGG-11 嵌套类型、位域不回退。当前全量 **1340** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。P1-07 剩余：**lambda（PAR-11/CG-11/DEC-10）**。

> **进度快照（2026-10-17，P1-07 lambda 完成）**：**lambda 表达式全链路完成**（PAR-11/CG-11/DEC-10 `[x]`，plan `docs/superpowers/plans/2026-10-17-lambda.md`）。语法 `[captures](params) -> Ret { body }`（`[]`/`[=]`/`[a,&b]`，返回类型显式或推断，表达式起始 `[` 无歧义识别）；**无捕获 → 函数指针**（零开销：可赋 `int32 (*)(int32)`、立即调用、qsort 回调）；**有捕获 → 匿名闭包 struct + 调用函数** `__lambda_<N>__call(Closure*, params...)`（按值捕获拷贝、按引用捕获写穿原对象；语言无 `auto` → 闭包不可命名，实际形态为**立即调用** DEC 裁决）；`[=]` 语句感知捕获扫描。当前全量 **1351** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。**P1-07 全部完成**。下一里程碑：P1-09 剩余（std.mem/math/collections）与 P2。

> **进度快照（2026-10-17，P1-09 std.mem 纯函数层完成）**：**std.mem（STD-05 范围 a）完成**（spec `docs/superpowers/specs/2026-10-17-stdmem-design.md`，plan `docs/superpowers/plans/2026-10-17-stdmem.md`）。`libs/std/mem.smc`：`alloc`（malloc）/`alloc_zeroed`（calloc）/`resize`（realloc）/`dealloc`（free）/`align_up`/`align_down`/`is_aligned`（2 的幂位运算）/`aligned_alloc`（posix_memalign 包装，失败 null）/`aligned_free`；`std.c` 与内嵌兜底补 `posix_memalign`。**探查实证**：函数可跨模块导出，但 struct/class/template 不可（parse 期类型名注册先于 import）——Allocator/Arena/Pool 接口草案挂账（stdlib.md §11.1），待语言层「模块导出类型」修复后实现；std.collections（STD-06~08）同受此堵。当前全量 **1358** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。P1-09 剩余：std.math/std.bit（STD-13/14）、std.collections（STD-06~08，阻塞于模块导出类型）。

> **进度快照（2026-10-17，P1-09 std.math/std.bit 完成）**：**std.math + std.bit 完成**（STD-13/14 `[x]`，spec/plan `docs/superpowers/specs|plans/2026-10-17-stdmath-bit*`）。`libs/std/bit.smc` 纯语言位工具：SWAR popcount（LLVM 可折叠单条 popcnt）、二分 clz/循环 ctz（对 0 返回位宽冻结）、rotl/rotr（n 按位宽取模）、reinterpret_cast bitcast 包装（7 e2e）。`libs/std/math.smc`：18 个 libc 绑定（sqrt…sinf，`-lm` 链接）+ `constexpr PI/E` + 位级 isnan/isinf/is_finite（IEEE754 纯语言，运行时构造掩码绕开超 int64 常量折叠限制）（5 e2e）。**顺带修复语言层缺口**：namespace 内 C-linkage extern 函数被 scopedName 前缀化 → 符号 `std_sqrt_float64` 而非 libm 裸名 `sqrt`（JIT 解析失败）；修复——`visit(FunctionDeclAST)` 对 C 名跳过前缀、`namespaceCandidates` 限定名回退裸 C 名（SemanticAnalyzer.cpp）。当前全量 **1370** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。P1-09 剩余：仅 std.collections（STD-06~08，阻塞于模块导出类型）。

> **进度快照（2026-10-20，语法缺口第二轮收口）**：**10 项语法特性完成**（spec/plan `docs/superpowers/{specs,plans}/2026-10-20-syntax-gaps-r2*`）：`f16/f32/f64/f128` 类型关键字（TYP-04）、指针后缀 `restrict`（PAR-06）、`u8/u16/u32` 前缀字面量（TYP-05）、C99 复合字面量与 slice 字面量（PAR-08）、类外方法定义（PAR-03）、一元/二元/`[]`/`()` 运算符重载调用点收敛（SEM-17）、非类型模板参数作值（GEN-02）。虚函数（INH-07）与可变参数模板（GEN-10）经用户确认维持 Non-goal。当前全量 **1470** 测试 0 失败。

> **进度快照（2026-10-22，顶层用户类型声明修复）**：**顶层声明识别用户定义类型**（PAR-05 补丁）。根因：struct/class/union 解析器会消费声明末尾的 `;`，而调用方仍把下一个 token 当作本声明的 attached declarator——`struct S { ... }; S g;` 把下一条声明的 `S` 当成变量名。修复：按记录声明的三种形态分派（`;`=完整声明 / `}`=定义+声明符 / 无 body=前向声明或类型引用），仅 `}` 形态允许 attached declarator；前向声明显式注册占位类型。现在 `S g;`、`S* p;`、`V f()`、`struct V f()`、`class C f()`、`B<int32> g;`、`using W = S;`、`V operator+(...)` 均可用（11 个新 e2e）。当前全量 **1493** 测试 0 失败。新增已知缺口：①`f().field`（struct 返回值成员访问）生成非法 GEP（需 rvalue 临时对象）；②`struct S; S* p; struct S { ... }; p->x` 因前向占位与定义未合并而报无成员。`typedef` 暂保留（用户裁定先不删）。

> **进度快照（2026-10-23，using 别名收口 / PAR-24）**：**删除 `typedef` 与 `type`，`using` 成为唯一类型别名形式**（spec/plan `docs/superpowers/specs|plans/2026-10-23-using-alias*`）。`using` RHS 升级为完整 type-id：固定数组（多维）、抽象函数指针、切片/optional/模板实例、内联/命名聚合；内联 enum 枚举量在别名作用域可见；`deprecated`/`repr` 注解语义对齐。`typedef`/`type` 在顶层、namespace、块、聚合成员体统一硬错误并提示改写；删除 `TypedefDeclAST`/`TypeDeclAST`，保留内部 `TypedefType`。迁移 16 个测试文件与 `examples/all_features.c`。当前全量 **1512** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。DEC-16/PAR-24 关闭；块内/类内 `using`、前向别名身份、`*p` 解引用既有缺陷仍挂账。

> **进度快照（2026-10-24，类型解析三缺口收口）**：**块/成员体 `using`、前向类型身份、别名指针解引用完成**（spec/plan `docs/superpowers/{specs,plans}/2026-10-24-type-resolution-gaps*`）。块内 `using` 走 TypeContext 局部别名栈（词法作用域、可遮蔽、块尾失效）；class/struct/union 成员别名以扁平键 + `C::T` 暴露，class 受 E2009 访问控制；struct/union 前向占位与定义合并，`using N = struct Node;` / `struct S; S* p;` 后 `p->x` 可用；`*p` 穿透任意层指针别名。全量 **1534** 测试 0 失败。MOD-03 关闭；剩余：前向类型按值使用诊断、单语句体 `using` 作用域、成员别名继承。
> **进度快照（2026-10-25，full_feature 验收示例）**：新增 `examples/full_feature.c`（25 节），单文件覆盖 module/import/export、lambda、运算符重载、块/成员 `using`、前向类型与指针别名解引用、u8/u16/u32/raw/multiline、thread_local/static/atomic/volatile/inline 等；编译运行输出 `FULL FEATURE OK`。全量测试仍 **1535/1535**。
> **进度快照（2026-10-25，比较结果作 bool 实参）**：`conversionRank` 增加整数 → `bool`（rank 2），比较表达式（`int32`）可直接传 `bool` 形参，同时保留 `int32` 重载优先；`bool → 整数` 仍按拓宽。全量 **1537/1537**；两个示例的 `check` 已改回 `bool` 形参并运行通过。
> **进度快照（2026-10-26，语法缺口 1–6 收口）**：先修 CG-05（rvalue 聚合成员按值读取，`make().x` 不再产生非法 GEP）与 MOD-03（前向类型按值使用报 incomplete；指针仍允许）。随后：①模块导入改为 parse 前预载，`export struct/class/using/template` 可用；②强制 `module` 首部/唯一/文件名匹配；③函数内 `static`/`thread_local` 静态变量与局部 `struct/union/enum` 定义；④通用括号声明符（函数返回函数指针、函数类型别名、指向数组的指针、函数指针数组）+ 表达式 callee 间接调用；⑤自引用类指针字段爆栈修复、类内/类外成员运算符重载调用；⑥函数体 `compile_time.if` 与嵌套 `offsetof(T, a.b)`/`offsetof(T, arr[i])`。全量 **1559/1559**；两个示例仍输出 `ALL FEATURES OK` / `FULL FEATURE OK`。提交线：a805740、53768f1、3d88e2b、c2d509b、4d77701、8dd88ca、2597f62。
> **进度快照（2026-10-26，基础 C 声明/语句收口）**：补齐 9 项基础语法：空语句 `;`；聚合成员多声明符 `struct S { int32 a, b; }`；聚合成员直接函数指针字段与指向数组的指针字段；`sizeof`/`alignof` 的完整 type-id（数组/抽象函数指针）；后置限定符 `int32 const`；块内 `extern`；record 定义后的 attached 指针/函数声明符；多声明符中的复杂后继（`int32 (*f)(int), (*g)(int);`）。顺带把带方法的 `struct` 正确路由为 class 类型，并修复布局注解对 `ClassType` 的 UB 强转。全量 **1572/1572**；两个示例仍输出 OK。提交线：0bc3be2、88fca71、b07dc1f、42d3ed1、dfcb9cc、607ff32、dad042d、31a68fb、27a1242。
> **进度快照（2026-10-27，语法缺口审计）**：对第三轮收口后的剩余语法缺口做规范+CLI 复核，按“规范已承诺 / C 兼容性 / 规范未覆盖扩展 / 非语法边界 / Non-goals”分类，详细清单见 **§24**（SG-01 ~ SG-11、SG-B01 ~ SG-B06）。本轮不改代码。
> **进度快照（2026-10-31，P1/P2 语法缺口收口）**：SG-01 ~ SG-07 已全部修复并新增 7 个 `SyntaxGapsE2E` 回归测试；全量 **1611/1611** 通过（2 个既有 `GTEST_SKIP`）。剩余为需要产品裁决的 SG-08 ~ SG-11、SG-20 ~ SG-23，以及非语法的 SG-B01 ~ SG-B06。
> **进度快照（2026-11-01，compile_time 开发体验）**：CT-15 ~ CT-21 完成——源码位置/诊断断言、类型谓词与数值极限、字段/注解/枚举/类/继承标量查询、目标/构建扩展、`format/to_string`、类型值与模板内省、`compile_time.repeat`；全部标量查询无运行时 IR，`LayoutBuilder` 按 `Type*` 缓存。全量 **1658/1658** 通过（2 个既有 `GTEST_SKIP`）。CT-22（`for_fields`/`for_enum_values`）因需 AST 克隆/codegen 特化留待单独设计。
> **进度快照（2026-11-01，标准库补全 + mimalloc）**：**std.mem 切换 mimalloc**（静态链接、`MI_OVERRIDE=ON`、JIT/生成程序同后端），新增 Allocator/Arena/Pool 与字节操作；**新增 std.collections**（Vec/Deque/HashMap/HashSet，显式 init/destroy + 回调哈希/相等）及 std.hash/std.sort/std.parse/std.ascii/std.path/std.fs/std.time/std.os/std.testing；`std.c` 不再内嵌，缺失标准库显式报错；驱动补齐 `-l/-L/-lm` 与 mimalloc 链接；`std.io::read_line` 增加 `cap<=0` 守卫。顺带修复模板 `true/false` 克隆丢失、命名空间模板限定名查找、`string` 非 NUL 终止三个阻塞缺陷。全量 **1679/1679** 通过（2 个既有 `GTEST_SKIP`）。
> **进度快照（2026-11-01，示例收敛）**：删除重复的 `examples/full_feature.c`，仅保留 `examples/all_features.c` 作为唯一验收示例；两者内容高度重叠，维护成本大于收益。全量测试仍 **1679/1679**（2 个既有 `GTEST_SKIP`）。
> **进度快照（2026-11-01，移除 libsafec）**：删除 `src/libsafec/` 与 `tests/libsafec/`，移除 CMake 的 `safec` 目标及其测试链接。该实现只被自身测试引用，编译器/JIT/`libs/std` 均不依赖，且与 mimalloc + `std.mem` 重复。全量 **1657/1657** 通过（2 个既有 `GTEST_SKIP`）。
> **进度快照（2026-11-01，容器文件拆分）**：`std.collections` 按容器拆为 `libs/std/collections/{vec,deque,hashmap,hashset}.smc` 独立模块（`std.collections.vec` 等可直接导入）；`libs/std/collections.smc` 保留为汇总入口，既有 `import std.collections;` 用法不变。新增直接导入测试。全量 **1658/1658** 通过（2 个既有 `GTEST_SKIP`）。
> **进度快照（2026-11-01，constexpr 容器）**：`constexpr` 求值器可执行 `Vec`/`Deque`/`HashMap`/`HashSet`（循环/赋值/`&out`/标量与 `string_view` 元素），`constexpr` 容器变量物化为只读常量数组 + 常量结构体（含 HashMap/HashSet 的开放寻址表与回调函数指针）；物化表禁止修改/可变拷贝。顺带修复 `typeToMangled` 缺失 `Str`/`String` 与模板函数类型内 `TypeVar` 未替换两个阻塞缺陷。全量 **1699/1699** 通过（2 个既有 `GTEST_SKIP`）。
>
> **进度快照（2026-10-27，语法缺口补充审计）**：在 §24 基础上继续补漏，新增 **§25**（SG-12 ~ SG-23、SG-B07 ~ SG-B09），覆盖数组/位域常量表达式、声明符递归与函数类型形参、静态成员类外定义、C 字面量、asm、模块/控制流/类模板扩展。新补项中 P1 为 SG-12、SG-14；SG-12、SG-13、SG-14、SG-15、SG-16、SG-17 与 SG-B07、SG-B09 已修复（SG-15 于 2026-10-28）；SG-18、SG-19 已全部修复；SG-20 ~ SG-23 仍挂账（SG-B07 ~ SG-B09 已全部修复）。


> **进度快照（2026-10-19，语法遗漏收口完成）**：**语法层面审计 + 11 项特性/缺陷收口**（spec/plan `docs/superpowers/specs|plans/2026-10-18-syntax-gaps*`）。多行字符串 `"""..."""` 与 Unicode 标识符（LEX-16/18）；`char8/16/32` 类型（uint 别名）与 `thread_local`（TYP-05/CON-04）；`atomic` 限定符（整型标量，原子 load/store + atomicrmw 子集，CON-01）；默认参数（区间重载 + 少默认优先，FUN-09）；**未知类型名声明静默丢弃修复**（E2031，此前 `f16 x;` 零诊断消失）；compile_time 反射 `name_of/fields_of/attributes_of`（ReflectionRegistry 交接，CT-07/13）；protected 派生访问放宽（INH-06）；`alignof/offsetof` 独立关键字与 `nullptr`（PAR-14/TYP-24）。当前全量 **1418** 测试 0 失败（2 个既有 `GTEST_SKIP` 不变）。P1-09 剩余：仅 std.collections（STD-06~08，阻塞于「模块导出类型」语言缺口）。

---

## 0. 现状基线（已完成，勿重复实现）

- `[x]` **BASE-01** 基于 LLVM（>= 18，实测 22.1.8）的 C 风格编译器；CMake 构建、CTest 测试。
- `[x]` **BASE-02** 词法：标识符/数字/字符串/字符/运算符/关键字；整型 `int8..int128/uint8..uint128/isize/usize`，浮点 `float/float32/float64`；`#`/`...`/`::` 等符号。
- `[x]` **BASE-03** 语法：函数、变量、数组、struct/class/union/enum、typedef、`sizeof`、初始化列表、三元、逗号、赋值复合运算符、前缀/后缀 `++/--`、成员访问 `.`/`->`、下标、方法调用。
- `[x]` **BASE-04** 控制流：if/else、while、do-while、for、switch/case/default（含贯穿）、break、continue、return、defer。（`goto`/label 已按 NG-01 移除）
- `[x]` **BASE-05** 语义：作用域/符号表、函数重载（`OverloadSet`）、运算符重载（`operator`）、左值/右值、隐式类型转换（整数提升 + 常规算术转换，TYP-22）、常量折叠 `constexpr`、`print/println` 内建与 `to_string` 分派。
- `[x]` **BASE-06** 代码生成：基础类型/指针/数组/struct/class/union/enum/函数指针、全局变量、字符串字面量、指针算术（GEP）、负浮点（fneg）、非 void 函数补 `unreachable`。
- `[x]` **BASE-07** 驱动：`-c/-o/-S/-M/-O/-g/-v/-Wall/-Werror/-std/-fsyntax-only/-l/-L`、JIT、`cc` 链接系统 libc。（`-E/-I/-D` 已按 TOOL-02 移除；模块搜索路径改用 `-M/--module-path`）
- `[x]` **BASE-08** 标准库基线。早期自包含 `libsafec`（`src/libsafec/`）已于 2026-11-01 删除：它只被自身测试使用，功能与真正的 `libs/std` 重复；现行标准库为 `libs/std/*.smc`（mimalloc 后端）。
- `[x]` **BASE-09** `resources/main.c` 可完整编译运行。全量测试通过（当前 **683** 项）。
- `[x]` **BASE-10** `(新)` 历史：`libsafec` 曾使用 `namespace safec`，暴露语言层 `namespace` 缺口（见 PAR-22/MOD-12）。该实现已删除；`namespace`（值 + 类型）由 `libs/std` 与测试覆盖。

---

## 1. 基础设施与工程（INF）

- `[ ]` **INF-01** 锁定 LLVM 版本与集成层：CMake `find_package(LLVM)`、最低版本断言、版本不匹配诊断。
- `[ ]` **INF-02** 构建系统：Debug/Release、跨平台（Linux/macOS/Windows）、交叉编译目标配置。
- `[x]` **INF-03** **诊断系统**：统一 `Diagnostic`（源码位置/错误码/严重级别/修复建议）、诊断快照测试设施。已实现错误码注册表 + `formatWithSeverity` + fix-it + 快照设施（`tests/diagnostics/snapshots/`）+ 警告独立通道（`getWarnings`，`-Werror` 可控）；词法器记录记号**起始**位置，表达式/声明节点由解析器打点，`file:line:col` 对解析与语义诊断均可用（`tests/frontend/test_source_location.cpp`）；语句节点（表达式语句/声明语句/`defer`）亦已打点。
- `[ ]` **INF-04** 测试框架完善：单元、集成、黄金文件（IR/输出）、诊断快照；CTest 分组与标签。
- `[ ]` **INF-05** CI：Linux/macOS/Windows 矩阵、交叉编译、ABI 测试、性能基准。（外围仅限构建/测试，不含 LSP/包管理）
- `[x]` **INF-06** 文档系统：语言规范（EBNF 语法）、类型/ABI 文档、`compile_time` API 文档。（**提前到 P0**，见 P0-07；`docs/spec/` 六分册 + 一致性测试已建）
- `[x]` **INF-09** `(新)` **规范性文法（EBNF）**：完整产生式覆盖声明/类型/表达式/语句/注解/模块/`namespace`；作为规范唯一权威来源。（`docs/spec/grammar.ebnf`；ABI/转换/语义分册见 `docs/spec/`；一致性由 `test_spec_conformance` 绑定）
- `[x]` **INF-10** `(新)` **运算符优先级/结合性总表**：表驱动，并与解析器实现绑定一致性测试。（表在 `docs/spec/grammar.ebnf` §9，实现于 `smc::getOperatorInfo`；`tests/frontend/test_operator_precedence.cpp` 10 项）
- `[x]` **INF-11** `(新)` **关键字与保留字总表**：数据类型/存储类/控制流/模块/注解/限定符；区分已用与保留未用。（见 `docs/spec/keywords.md`；DEC-18 已冻结，`goto`/`#` 标记为已移除）
- `[ ]` **INF-12** `(新)` **语言版本标识**：`-std=` 取值、默认版本、特性门控（不含 edition，见 NG-06）。
- `[~]` **INF-13** `(新)` **诊断格式规范**：错误码命名、位置格式、严重级别、稳定输出（配合 INF-03、TOOL-09）。注册表（E0xxx/E1xxx/E2xxx/W3xxx）与 `file:line:col: severity[CODE]: msg` 格式已落地。
- `[ ]` **INF-07** 可复现构建：确定性输出、路径无关、固定优化流程。
- `[ ]` **INF-08** 最小命令行驱动保留：`smc check/build/run/test`（不含 fmt/doc/lsp，见 Non-goals）。

---

## 2. 词法分析（LEX）

- `[ ]` **LEX-01** `(已实现)` 关键字与基础类型关键字词法；核对与规范一致。
- `[x]` **LEX-02** `(新)` 新增关键字 token：`template`、`typename`；弃用未使用的 `generic`。（`generic`/`comptime` token 已按 DEC-18 移除；`template`/`typename` 已注册为 `TOKEN_TEMPLATE`/`TOKEN_TYPENAME`（GEN-01/P1-03）；`this` 按 PAR-17 上下文标识符处理——方法体内隐式 this 参数，非保留关键字 token）
- `[x]` **LEX-03** `(改)` 移除/废弃：C 预处理器与宏相关 token（`#include/#define/#if`）与 `-E/-I/-D`（见 Non-goals）。`#` 仍作为 `TOKEN_HASH` 词法识别，由解析器以“不支持预处理指令”报错。
- `[ ]` **LEX-04** `(已实现)` 整型字面量：十进制/十六进制/二进制/八进制/分隔符/后缀；补全测试。
- `[x]` **LEX-05** `(已实现)` 浮点字面量：小数/指数/`f16/f32/f64/f128` 后缀；补全测试。**已完成**：小数（含“结尾点”形式 `1.` / `1.e3`，数字后紧跟 `.` 一律进入小数部分）、指数 `e/E`（可带 `+`/`-`，数字间允许 `_`）、后缀走 LEX-15；带指数即浮点，无后缀默认 `float64`（`tests/frontend/test_lexer.cpp` 的 `LexerFloatExponentTest`/`LexerTrailingDotFloatTest`）。**明确延后**：十六进制浮点 `0x1p3` 不入规范（无 std.math/位级用例，YAGNI；如需再挂靠 LEX-04/TYP-04）。
- `[ ]` **LEX-06** `(已实现)` 字符与字符串字面量：转义、UTF-8；补原始字符串/多行字符串。
- `[ ]` **LEX-07** `(已实现)` 注释：行注释/块注释；补文档注释。
- `[x]` **LEX-08** `(改)` 泛型 `template<...>` 与比较运算符 `<`、`>>` 拆分的歧义消解（2026-10-06）。类型位置 `<` 跟随标识符 → 实参列表；调用位置投机解析 `name<args>(`，失败回滚按比较（歧义取比较，C++ 立场）；`>>` 按嵌套深度拆分。
- `[ ]` **LEX-09** Slice 语法 `T[]` 的 token 与解析支持（当前已有 `SliceType`，核对）。
- `[x]` **LEX-10** 注解 token——不新增 token；`[[` = 两个 TOKEN_LBRACKET，parser 声明位置前瞻特判（2026-10-07）。
- `[x]` **LEX-11** `static_cast<T>(x)` / `reinterpret_cast<T>(x)` 等转换关键字（为 INH/CRTP 铺路）。已实现为**上下文关键字**（紧跟 `<类型>` 时解析），见 PAR-18。
- `[x]` **LEX-12** 内联汇编 `asm` token。**完成**（2026-10-11，P1-08）：keywordMap 注册 `{"asm", TOKEN_ASM}`（Task 1）；parser/sema/codegen 全链路见 PAR-16/CG-10/DEC-09。
- `[x]` **LEX-13** 错误恢复与词法诊断（非法字符、未闭合字面量、整型溢出）。已实现：`Lexer::getDiagnostics()` 产出 E0001（非法字符，跳过并继续）、E0002（未闭合字符串/字符/块注释，含原始字符串）、E0003（整型字面量超过 128 位，累加器 `unsigned __int128`，LEX-17）；`skipTrivia` 统一跳过空白与注释（修复行注释后空行被误判）；诊断在 `CompilerDriver`/`ModuleLoader`/`StdPrelude` 中上报并使编译失败（`tests/frontend/test_lexer.cpp`、`tests/driver/test_compiler_driver.cpp`、`tests/diagnostics/snapshots/lexical_invalid_character.txt`）。
- `[x]` **LEX-14** `(新)` **关键字/保留字总表落地**（配合 INF-11）：`register`/`cast`/`typeof` 已按 DEC-18 移除 token；`namespace` 关键字与 `::` token 已新增；`template`/`typename` 已注册（LEX-02），`this` 走上下文标识符（PAR-17）。
- `[x]` **LEX-15** `(新)` **字面量默认类型与后缀映射**：无后缀整型/浮点的默认类型，`u`/`l`/`f16`…后缀 → 底层类型表。**词法层已完成**：`Token::literalKind`（`Int/UInt/Long/ULong/Float16/Float32/Float64/Float128`）+ `literalKindName` 映射表；识别 `u`/`l`/`ul`/`lu`（各进制通用）与 `f`/`f16`/`f32`/`f64`/`f128`（兼容 `l`/`L`）；无后缀默认 `int32`/`float64`；规范见 `docs/spec/grammar.ebnf`、`conversions.md §7`（`tests/frontend/test_lexer.cpp`）。**消费端已完成**（2026-09-29）：AST 节点携带 `literalKind`，sema 按 kind 赋定宽类型（无后缀默认 `int32`/`float64`），codegen 按宽度/符号（含无符号）与 APFloat 语义（half/single/double/quad）生成常量，大整数不再截断。**128 位字面量已支持**（2026-10-16）：TokenValue 整数值统一 `unsigned __int128`，lexer 累加至 128 位（E0003 移到超 2^128-1），sema 映射 int128/uint128，codegen APInt 两字构造。
- `[x]` **LEX-16** `(新)` **转义字符全集、原始字符串与多行字符串**的精确词法规则（细化 LEX-06）。**完成**（2026-10-19）：转义全集（`\a\b\f\v\?` + `\u{...}`）与原始字符串 `r"..."` 已在 `decodeEscapes`/`scanString`；**多行字符串 `"""..."""`** 新增 `scanMultilineString`（`Token::isMultilineString`，parser 3 引号剥离）。
- `[x]` **LEX-17** `(新)` **数字分隔符位置规则、进制前缀、整型字面量溢出诊断**（细化 LEX-04/13）。二/八/十六/十进制均按 64 位无符号累加并检测溢出，超 64 位报 **E0003**；分隔符 `_` 仅可位于两个数字之间，前导/结尾/连续/紧邻进制前缀或小数点均报 **E0004**（`src/frontend/Lexer.cpp`；`docs/spec/grammar.ebnf`）。
- `[x]` **LEX-18** `(新)` **标识符 UTF-8/Unicode 规则**：允许范围、NFC 规范化、与关键字冲突处理。**完成**（2026-10-19）：UTF-8 多字节序列（高位字节）计入标识符首/续字符；不做 NFC 规范化（YAGNI）；关键字冲突天然不存在（全 ASCII）。

---

## 3. 语法分析与 AST（PAR）

- `[ ]` **PAR-01** `(已实现)` 顶层声明：变量/函数/struct/class/enum/union/`typedef`/`using`/`type`。
- `[ ]` **PAR-02** `(改)` 函数声明与定义：**C 风格**，不使用 `fn`；覆盖 `extern/static/inline`、成员函数。
- `[x]` **PAR-03** 方法定义语法 `Vector::length()`；嵌套/`static` 成员（AGG-11 / AGG-10，2026-10-05）。**类外定义完成**（2026-10-20）：`RetType Class::method(params) { ... }`——声明在类体内、定义在类外；sema 校验类/方法/签名、插入隐式 `this`，签名不符或类内未声明均诊断。
- `[x]` **PAR-04** class 成员：`public/private/protected` 段、成员变量/函数、`static` 成员、嵌套类型。**完成**（2026-10-05）：访问段解析与访问级别记录（DEC-01）、static 成员（AGG-10）、嵌套类型（AGG-11）全部落地。
- `[ ]` **PAR-05** 类型语法：基础类型、指针 `T*`、数组 `T[N]`、Slice `T[]`、模板实例 `Box<i32>`。**顶层用户类型声明已修正**（2026-10-22）：`S g;` / `S* p;` / `V f()` / `struct V f()` / `class C f()` / `B<int32> g;` / `using W = S;` 不再被前一条聚合声明的 attached-declarator 前瞻吞掉；前向声明显式注册占位类型。
- `[x]` **PAR-06** 指针限定符：`const/volatile/restrict/atomic`。**完成**（2026-10-20）：指针后缀 `restrict` 语法接受并记录 `Type::isRestrict`；别名/优化语义仍挂 MEM-04。
- `[x]` **PAR-07** `(新)` 注解挂载点：六处全落地（模块仅解析记录）（2026-10-07）。
- `[x]` **PAR-08** 表达式：字面量/标识符/调用/成员访问/下标/一元/二元/三元/强转。**切片/复合字面量完成**（2026-10-20）：C99 复合字面量 `(T[N]){...}` / `(T[]){...}`（块内栈对象、文件级静态常量，直接转义拒绝）；slice 字面量初始化 `T[] s = {1,2,3}`（栈匿名数组 + 视图，返回即悬垂拒绝）。
- `[x]` **PAR-09** `(改)` 语句：块、if/else、while、do-while、for、switch/case、return、break、continue、声明。（**不含 goto/label**，见 Non-goals；已移除 goto/标签解析）
- `[x]` **PAR-10** `(新)` 位域声明：`T name : N;`（struct/union 成员），含零宽位域。**完成**（2026-10-15，P1-07）：struct/class/union/匿名内联成员四处循环解析 `: N`（parseBitFieldWidth，常量表达式求值）；命名零宽位域 parse 期拒绝（FieldInfo 编码无法区分，Ruling）；超 int32 范围位宽 parse 期拒绝（防回绕绕过 E2026）。
- `[x]` **PAR-11** `(新)` 简单 lambda：语法（建议 C++ 风格 `[capture](params) -> T { ... }` 或最简 `[](p){...}`）、捕获列表。**完成**（2026-10-17，随 P1-07）：`[captures](params) -> Ret { body }`；`[]`/`[=]`/`[a,&b]` 捕获列表；返回类型显式或推断；表达式起始 `[` 识别（下标是后缀无歧义）；立即调用 `[...](...)(args)`。plan `docs/superpowers/plans/2026-10-17-lambda.md`。
- `[x]` **PAR-12** `(新)` 匿名 class/struct/union：匿名类型定义、匿名成员提升。**完成**（2026-10-17）：每个匿名定义获得唯一合成名 `__anon_N`（`TypeContext::nextAnonId`，C 语义独立类型）；`struct/union/class { ... } v, v2;` 全局/局部/嵌套（字段类型）/数组/指针全通路；匿名 class 字段/方法/访问段正确注册（`attachedVars` 挂载聚合 AST）；**名义身份强制**——两次独立匿名定义赋值/初始化/实参/返回/比较互斥（`checkAssignmentTypes`/`typesCompatible` 同 kind 收紧，保留派生→基 INH-04 切片）；匿名成员提升 AGG-03 不回退（`__anon_` 前缀判定）。plan `docs/superpowers/plans/2026-10-17-anonymous-types.md`。
- `[x]` **PAR-13** `(新)` 指定初始化器 `{.field = v}`、柔性数组 `T a[]`。**完成**（2026-10-16）：`.name`/`.a.b`/`[i]` 设计符链（AST `Designator`/`designators`，parser 解析）；sema `resolveDesignators` 展开语义目标链（`resolvedTargets`，继承链 + E2009、union 槽 0、位域、Optional/Result 伪字段拒绝）、数组大小推断 = C99 `max(位置式, 最大下标+1)`（含多链 `[1][2]=5` 外层维）；局部 codegen 整块补零 + 按链写槽（含 typedef 数组）；全局常量递归累积（后写覆盖/位域掩码 OR/基类下降）；乱序/重复/混排均 C99 语义。plan `docs/superpowers/plans/2026-10-16-designated-init-flexarray.md`。
- `[x]` **PAR-14** `(已实现)` `sizeof`；`(新)` `alignof`、`offsetof`。**完成**（2026-10-19）：独立关键字形式 `alignof(T)`/`offsetof(T, member)` 已实现（SizeofExprAST::isAlignOf + OffsetOfExprAST，LayoutBuilder 偏移），与 `compile_time.align_of/offset_of` 一致。
- `[x]` **PAR-15** `(新)` `compile_time` 表达式：`static_assert`、`if`、目标/构建查询完成（2026-10-06）；反射部分未含（另轮）。
- `[x]` **PAR-16** `(新)` 内联汇编 `asm` 语句/表达式。**完成**（2026-10-11，P1-08）：`AsmOperand`/`AsmExprAST` 节点 + `parseAsmExpr()`（至多三段操作数、模板/约束须字符串字面量、`decodeEscapes` 去引号），见 plan `2026-10-10-p108-inline-asm`。
- `[x]` **PAR-17** `(新)` `this` 表达式（2026-10-06）。方法体内 `this` 走隐式 this 参数按普通变量解析；`this.field` 点号对指针自动解引用；static 方法内 this 钉死诊断。
- `[x]` **PAR-18** `(新)` `static_cast<T>(x)` / C 风格强转；向下转换（CRTP 必需）。C 风格 `(T)x` 已泛化到完整类型（P0-02）；`static_cast`/`reinterpret_cast` 已实现（LEX-11/DEC-18）：static 限算术↔算术、指针↔指针；reinterpret 为标量位重解释（同宽 `bitcast`）。
- `[ ]` **PAR-19** AST 节点带源码位置/属性/注释/文档；AST 序列化（增量编译缓存）。
- `[ ]` **PAR-20** 语法错误恢复与高质量诊断。
- `[x]` **PAR-21** `(新)` 模板声明语法（2026-10-06）。函数/struct/class/别名模板；union/enum 拒绝；`class T` 参数诊断提示 typename；非类型参数仅整数。
- `[x]` **PAR-22** `(新)` `namespace` 声明与限定名 `ns::name` 语法。历史需求来源为 `libsafec` 的 `namespace safec`（该实现已删除）；函数/变量成员、嵌套命名空间、限定类型名（struct/class/union/enum/typedef）均已实现。
- `[x]` **PAR-23** `(新)` 运算符优先级/结合性在解析器中的显式实现与表驱动测试（配合 INF-10）。`smc::getOperatorInfo` 表驱动 + `test_operator_precedence` + `test_spec_conformance`。
- `[x]` **PAR-24** `(新)` 别名声明规范形式收敛：**已收口（2026-10-23）**——删除 `typedef`/`type`，只留 `using`；`using` RHS = 完整 type-id（固定数组、函数指针、内联 enum、聚合定义）；删除 `TypedefDeclAST`/`TypeDeclAST`，保留内部 `TypedefType`。 **2026-10-24 补丁**：块/成员体作用域别名、前向占位合并、`*p` 穿透指针别名（MOD-03 关闭）。

---

## 4. 名称解析与模块系统（MOD）

- `[ ]` **MOD-01** 符号表：全局/模块/类型/函数/变量/字段/方法。
- `[ ]` **MOD-02** 作用域规则：块/函数/类型/模块。
- `[x]` **MOD-03** 前向声明：函数/struct/class/enum/union/类型别名。**已完成（2026-10-24）**：struct/union 前向占位与定义合并为同一对象——`using N = struct Node;`、`struct S; S* p; struct S { ... };` 后 `N`/`p->x` 均可见字段；class/enum 原有 eager 注册不变。
- `[x]` **MOD-04** `module` 声明与文件映射。点分/`::` 模块名 → `TranslationUnitAST::moduleName`；`import` 点分名解析 + 文件↔模块名校验（MOD-13）。
- `[x]` **MOD-05** `export` 导出规则；`import` 导入规则与访问语法（DEC-02 已冻结：直接非限定访问）。参与模块默认私有，`export`/`public` 后可见。
- `[x]` **MOD-06** 模块可见性与 `public/private`。每模块私有作用域 + `export` 提升到全局；顶层 `public` 与 `export` 同义（类成员访问级别仍待 PAR-04）。类型名可见性见 `docs/spec/modules.md` §11 限制。
- `[x]` **MOD-07** 模块循环依赖检测。`ModuleLoader` DFS `active` 栈检出导入环并报错（列出环路径）；`loaded` 去重支持菱形依赖。
- `[ ]` **MOD-08** 名称修饰：C ABI 符号、class 方法、**模板单态化符号**、`static` 成员。
- `[x]` **MOD-09** `(改)` **C 互操作（无预处理器方案）**：不解析真实 C 头文件，改用 `extern` 声明 + `std.c` 绑定层。绑定层已文件化到 `libs/std/c.smc`（`src/driver/StdPrelude.*` 优先读文件、内嵌兜底；`--no-prelude` 关闭）；`void*` 形参、函数指针形参与嵌套回调（`int (*cb)(int,int)`）均已支持；回调 API `qsort/bsearch/atexit` 已入库（P0-02 / MEM-10）。
- `[ ]` **MOD-10** 增量编译：模块/AST/类型/IR 缓存与失效策略（内容哈希）。
- `[x]` **MOD-11** 导入符号访问语法待定并实现：**直接非限定访问**（DEC-02 冻结）；限定名由模块内 `namespace` 提供。
- `[x]` **MOD-12** `(新)` `namespace` 语义：嵌套/开放命名空间、限定查找、与模块的关系（DEC-17 已冻结：正交）。（嵌套查找 + 名称隔离 + 限定类型名已实现；规范见 `docs/spec/modules.md`）
- `[x]` **MOD-13** `(新)` 模块/编译单元文件布局：文件 ↔ 模块映射、导出单元、单文件多模块规则。点分名↔目录文件映射 + 声明名与导入说明符一致性校验已实现（`module` 首部/重复声明约束待补）。
- `[x]` **MOD-14** `(新)` `import` 搜索路径与名称解析顺序、循环导入诊断（细化 MOD-07）。（搜索顺序：导入者目录 → `-M/--module-path` → `STD_DIR`；循环报错并列出环路径；规范见 `docs/spec/modules.md`）
- `[~]` **MOD-15** `(新)` **名称修饰方案（规范性）**：C ABI、方法、模板单态化、`static` 成员、namespace 的完整规则（细化 MOD-08）。（现状+目标见 `docs/spec/abi.md`）

---

## 5. 类型系统（TYP）

- `[ ]` **TYP-01** 基础类型大小/对齐表：按目标平台生成。
- `[ ]` **TYP-02** 整数类型：`i8..i128`、`u8..u128`。
- `[ ]` **TYP-03** 平台整数：`isize/usize` 与目标指针宽度绑定。
- `[x]` **TYP-04** 浮点类型：`f16/f32/f64/f128` 的 LLVM 映射与软件兜底（**短拼写类型关键字完成** 2026-10-20：`f16/f32/f64/f128` ≡ `float16/32/64/128`；字面量后缀不受影响）。**已完成**：LLVM 类型映射（常量按 APFloat 语义生成，LEX-15）；x86-64 上 f16/f128 的算术/比较/混合运算/转换、形参/返回、全局变量、struct 字段均可用（LLVM 原生/内部提升承担兜底）；`print`/`println` 全格式说明符支持 f16（无损提升）/f128（截断为 double，精度损失见 `conversions.md` §9.1）；varargs 默认实参提升对齐 C23（`float16→float64`，`float128` 原样传递）。**延后**：非 x86-64 目标（无 half 硬件、需 compiler_rt 的 quad）的验证挂靠 INF-05/TST-08；f128 的 113 位精确十进制输出挂靠 FMT-13。
- `[x]` **TYP-05** `bool/char/char8/char16/char32/void`。`char8/16/32` 类型关键字（2026-10-19，uint8/16/32 别名）；**u8/u16/u32 前缀字面量完成**（2026-10-20）：类型为 `char8/16/32[]`（逐字节存储、不转码），可退化为对应指针。
- `[ ]` **TYP-06** 类型别名 `type Handle = u64;`：展开、循环检测。
- `[ ]` **TYP-07** `struct`：字段、布局、默认公开、C ABI 兼容。
- `[ ]` **TYP-08** `class`：布局等价 struct，成员函数不占空间。
- `[x]` **TYP-09** `enum` 强类型：底层类型 `:uint8`、固定大小、不隐式转换。底层类型/固定大小、名义相等与 `enum ↔ int` 强制显式转换（TYP-20）均已实现。
- `[ ]` **TYP-10** `union`：C 风格布局、初始化、访问。
- `[x]` **TYP-11** 固定数组 `i32[32]`；VLA（可变长度数组）；多维数组。**完成**：一维数组形参 `T name[N]` 去糖为 slice（2026-10-01，spec `docs/superpowers/specs/2026-10-01-typ11-array-params-design.md`）；多维数组本体（2026-10-03，spec docs/superpowers/specs/2026-10-03-multidim-arrays-design.md；顺带修复全局数组带初始化器的既有缺口）；多维形参行 slice 去糖（2026-10-04，spec docs/superpowers/specs/2026-10-04-multidim-array-params-design.md；顺带修复 typesEqual 数组分支忽略元素/长度的缺陷）。**VLA 明确不支持**（2026-10-05 用户决策）：运行时长度分配与安全定位冲突（无界栈增长）、C23 已将 VLA 降为可选、动态长度由 Slice 承接；专用诊断"variable-length arrays are not supported"（声明与形参两处）+ pin 测试。
- `[x]` **TYP-12** `Slice`：`{ptr, length}`，不拥有、零拷贝。**已完成**：下标读写、`.len`、数组→slice 单向零拷贝退化（传参/赋值/初始化）、零初始化 `{null,0}`、规范 LLVM 单例类型、slice 元素类型严格比较。范围外（spec `docs/superpowers/specs/2026-09-30-slice-design.md` §8）：范围切片、显式构造、边界检查、字符串退化；遗留：无（MethodCall 传参退化已完成，2026-10-02——resolveMethod 两阶段匹配 + resolvedParamTypes，spec docs/superpowers/specs/2026-10-02-methodcall-decay-design.md）。全局变量 codegen 缺口已修复（无初始化器全局现为零初始化定义）；`T name[N]` 形参已去糖（TYP-11），return 局部数组被拒绝（悬垂 D4）。
- `[x]` **缺陷（评审发现，2026-10-04，已修复 2026-10-05）**：数组赋值静默写坏内存。修复：`checkAssignmentTypes` 对 Array 左值一律拒绝（C99 6.5.16 数组为不可修改左值；`a=b` 含同形状、`a=p` 均报 SemIncompatibleAssignment，建议 memcpy/逐元素）；`p=a` 退化与 struct/union 拷贝赋值不受影响。测试 4 项（AASG 前缀，816→820）。
- `[x]` **TYP-13** `Optional<T>`：`{ bool valid; T value; }`；显式访问（**无 `?` 传播算子**）。**完成**（2026-10-06）：`T?` 与 `Optional<T>` 双形式同型；`{i1, T}` 布局（valid 在前）；伪字段 `.valid`/`.value` 自由读写；类型相等按元素严格（`int32?` ≠ `float64?`）；spec `docs/superpowers/specs/2026-10-06-optional-result-design.md`。
- `[x]` **TYP-14** `Result<T,E>`：布局与 `.error`/`.value` 语义；显式访问。**完成**（2026-10-06）：`{ bool ok; T value; E error; }`（DEC-03 裁决加判别标志）；伪字段 `.ok`/`.value`/`.error`；实参严格相等；e2e 错误路径/传参/返回可用；spec 同上。
- `[ ]` **TYP-15** 指针类型：`T*`、函数指针、多级指针。
- `[ ]` **TYP-16** 限定符类型：`const/volatile/restrict/atomic`。
- `[ ]` **TYP-17** 函数类型：参数/返回/调用约定/可变参数。
- `[x]` **TYP-18** `(新)` 模板实例类型（2026-10-06）。惰性实例化 + Registry 键去重（同实参同 decl/type）；裸模板名使用诊断。
- `[~]` **TYP-19** 类型相等/兼容/隐式转换/显式转换规则（含常规算术转换）。TYP-20（enum 强类型、名义相等）与 TYP-22（整数提升 + 常规算术转换）已落地；隐式窄化策略与限定符参与见 TYP-23/TYP-16。
- `[x]` **TYP-20** 枚举与整数必须显式转换。`typesCompatible`/`conversionRank`/`checkAssignmentTypes` 拒绝 `enum ↔ 非同类 enum/整数/浮点` 的隐式转换（赋值/初始化/实参/返回），要求 `(T)`/`static_cast`/`reinterpret_cast`；运算符仍按整数提升。枚举名义相等（同名才相等）。
- `[ ]` **TYP-21** ABI 类型检查：`[[repr(C)]]` 下布局可预测；位域布局规则（见 DEC-08）。
- `[x]` **TYP-22** `(新)` **整数提升与常规算术转换**：转换等级、signed×unsigned 混合规则（细化 TYP-19）。已实现于 `usualArithmeticType`/`promoteArithmeticType`/`isUnsignedArithmeticType`（`src/ast/Symbol.cpp`）：sema 按提升+常规算术转换取公共类型，codegen 按有/无符号选 `udiv`/`urem`、无符号比较、`lshr`/`ashr`，并按源符号选 `zext`/`sext`、`uitofp`/`sitofp`（规范见 `docs/spec/conversions.md` §3/§5）。
- `[~]` **TYP-23** `(新)` **隐式/显式转换矩阵**：标量、指针、数组、struct/class、enum、Optional/Result 的完整转换表。（规范矩阵见 `docs/spec/conversions.md` §4；`enum` 强类型 TYP-20 与常规算术转换 TYP-22 已落地；隐式**窄化**仍按兼容处理，其余 `[plan]` 项待实现）
- `[x]` **TYP-24** **空指针常量语义**：`null` 是唯一空指针字面量；`nullptr` 于 2026-10-29 移除（Review 2.3）。`null` 只隐式转指针；`NULL` 宏不提供（无预处理器）。见 `docs/spec/conversions.md`。
- `[x]` **TYP-25** `(新)` enum 默认底层类型、枚举常量作用域与限定访问（细化 TYP-09）。枚举常量可作值/常量表达式（含 `= -1`、`= 1+2+4`），支持限定访问 `A::Red` 与限定类型名 `cfg::Mode`（无需 `enum` 关键字）；显式底层类型 `enum E : uint8`（含 `typedef enum : uint16 {...}`）已实现。
- `[x]` **TYP-26** `(新)` 字符串字面量类型、`str`/`String` 生命周期与所有权（细化 FMT-01/02）。2026-10-08 完成（随 P1-06）：字面量类型 = `str`；所有权/边界见 DEC-20。
- `[~]` **TYP-27** `(新)` 聚合类型 ABI：按值传参/返回（byval/sret）、端序、默认对齐与成员内边距（细化 MEM-09）。（草案见 `docs/spec/abi.md`）

---

## 6. 泛型与模板（GEN）〔新〕

> 语法**对齐 C++ 简单泛型**；支持函数模板、类模板、非类型参数、CRTP；单态化展开，零运行时开销。

- `[x]` **GEN-01** 声明语法（2026-10-06）。`template`/`typename` 关键字；TemplateDeclAST；TypeVarType 占位（同名同指针）。
- `[x]` **GEN-02** 非类型（常量）模板参数（2026-10-06）。仅整数；字段数组长度占位（ArrayType::sizeParam）实例化时具体化。**值位置完成**（2026-10-20）：模板体内参数名可作为值表达式使用（`return N`、算术、数组维度）；局部变量/形参遮蔽值参数拒绝；参数作显式模板实参（`f<N>()`）延后。
- `[x]` **GEN-03** 实例化（2026-10-06）。显式 + 推导（形参 T 取实参类型、T* 取去指针类型；冲突/无法推导诊断）；类模板不推导。
- `[x]` **GEN-04** 解析歧义（2026-10-06）。见 LEX-08。
- `[x]` **GEN-05** 单态化（2026-10-06）。AST 克隆替换器（全节点族）+ Registry 状态机去重 + 深度上限 64；未使用模板零符号；调用点按需前向声明。
- `[x]` **GEN-06** 约束策略（2026-10-06）。定义处只 parse；实例点全量普通 sema；诊断附 `in instantiation of template '...'`。
- `[x]` **GEN-07** 符号修饰与 ABI（2026-10-06）。实例名 `名$实参拼写`（typeToMangled 复用，指针 …ptr 后缀）；函数实例符号沿用 mangleFunction 追加参数。
- `[x]` **GEN-08** 模板成员方法解析与 `this` 传递（2026-10-06）。实例即普通类声明，隐式 this 参数为实例类型；基类实例方法体惰性（首次调用 visit）。
- `[x]` **GEN-09** CRTP 支撑（2026-10-06）：见 INH-05（`this`、模板基类、延迟实例化、`static_cast`）。
- `[ ]` **GEN-10** **不做**（Non-goals）：特化/偏特化、SFINAE、可变参数模板、模板模板参数、concepts、默认模板实参。**（2026-10-20 用户确认：可变参数模板维持不做。）**

---

## 7. 语义检查与静态分析（SEM）

- `[x]` **SEM-01** 变量未初始化检查：数据流分析、分支合并、循环。已实现 definite-assignment 分析（if/else 取交集、循环保守、do-while 至少一次、地址取址假定已初始化），产出 W3001 警告（`SemanticAnalyzer::checkInitialization`）。
- `[x]` **SEM-02** 指针未初始化检查；解引用前必须赋值。同一分析覆盖指针（`int* p; *p` → W3001）。
- `[ ]` **SEM-03** 指针对空性检查：`[[nonnull]]`、可选运行时非空断言。
- `[ ]` **SEM-04** 访问控制检查：`public/private/protected`（单继承链）。**部分完成**（PAR-04/DEC-01 轮次）：class 默认 private / struct 默认 public，访问段解析修复（不再截断类体），类外访问 private/protected 成员与方法报 **E2009**（`SemanticAnalyzer`，快照 `tests/diagnostics/snapshots/private_member_access.txt`）；类内方法经 `this->` 可访问。**待补**：protected 在派生类内放开（随 INH 单继承链）、友元。
- `[ ]` **SEM-05** 类型检查：表达式/赋值/调用/返回/字段访问。
- `[x]` **SEM-06** 泛型实例化检查（GEN-06）。已实现（2026-10-06）：实例点全量 sema + 惰性方法体。
- `[x]` **SEM-07** `compile_time` 条件求值与死代码消除——sema 期选分支原位展开，codegen 仅出选中分支（2026-10-06）。死分支类型占位毒化（`Type::ctDeadBranch`），活分支 sema 注册新对象解毒。
- `[x]` **SEM-08** 格式字符串类型检查：`{}`、`{:x}`、`{:f}`、`{:02}`、`{:.2f}`（`print/println`）。**已完成**（2026-10-09，随 FMT-09）：字面量 fmt 全检（E2020~22）；动态 fmt 只查实参可格式化。
- `[ ]` **SEM-09** 数组/Slice 边界检查策略：静态可证明或运行时检查（见 DEC-06）。
- `[ ]` **SEM-10** 整数溢出检查策略：Debug 检查 / Release 行为（见 DEC-07）。
- `[~]` **SEM-11** 警告：未使用变量（W3002）与不可达代码（W3003）已实现（`tests/sema/test_warnings.cpp`；任一引用算使用，`defer` 不影响可达性）；**W3004 弃用 API 已随 ANN-05 落地**（2026-10-07）；enum 穷尽性（依赖 DEC-13）待定；未初始化变量见 SEM-01/02（W3001）。
- `[x]` **SEM-12** 位域语义检查：宽度合法、跨存储单元规则。**完成**（2026-10-15，P1-07）：E2025（基类型非整型族）、E2026（宽度超基类型位宽/负宽）、E2027（取址拒绝）；基础类型放开 bool/int8..128/uint8..128/isize/usize；成员访问标记 isBitField/bitWidth；物化后成员解析优先 semanticFields 快照。
- `[ ]` **SEM-13** UB 清单（**对齐 C 标准**）：为 SEM-09/10/11 与 UBSan 提供依据；不引入所有权/生命周期模型。
- `[~]` **SEM-14** 静态分析诊断：错误码、源码位置、修复建议。主要检查点已带错误码与 fix 字段（见 `src/sema/Diagnostic.*`）。
- `[~]` **SEM-15** `(新)` **求值顺序与序列点**：表达式/函数实参求值顺序，对齐 C 标准（细化 SEM-13）。（草案见 `docs/spec/semantics.md`）
- `[~]` **SEM-16** `(新)` **重载解析规则**：可行候选、最佳匹配、歧义诊断、模板实例参与（细化 FUN-07/GEN-06）。（现状+目标见 `docs/spec/semantics.md`）
- `[x]` **SEM-17** 可重载运算符集合与签名约定。**完成**（2026-10-20）：声明侧与调用侧集合收敛为 `+ - * / % == != < > <= >= && || & | ^ << >> ~ ! ++ -- [] ()`；一元/前后缀、二元补齐、下标与调用重载调用点全部生效；重载 `&&`/`||` 不短路。见 `docs/spec/semantics.md` §4。
- `[~]` **SEM-18** `(新)` **`defer` 语义**：注册顺序、作用域退出执行顺序、与 `return`/`break`/`continue` 的交互（细化 FUN-10）。（现状+待冻结见 `docs/spec/semantics.md`）

---

## 8. 预定义类型结构体/类/枚举/联合体（AGG）

### struct
- `[ ]` **AGG-01** 声明、字段、初始化、赋值、嵌套、前向声明。
- `[ ]` **AGG-02** 默认公开；无隐藏数据；C ABI 兼容布局。
- `[x]` **AGG-03** `(新)` 匿名 struct/union 成员与成员提升。已实现（AGG-17 轮次）：struct/union 体内的匿名聚合成员按 C11 语义提升到外层（`s.i`/`s.x` 直接可用），名字冲突报错；`typedef struct/union` 别名的成员访问同步修复（`.` 与 `->` 均剥离 typedef）。文件作用域匿名 union 延后。
- `[ ]` **AGG-04** `(新)` 柔性数组 `T a[]`。**完成**（2026-10-16，随 PAR-13）：`FieldInfo::flexibleArray` 标记（`parseMemberArraySuffix` 末维 `[]` → `ArrayType(T,0)`）；约束——仅 struct 尾成员、非匿名/非位域、前有具名成员、class/union 拒绝、花括号初始化拒绝；`sizeof` 不含 FAM（对齐 GCC，packed/align 变体实测核对）；`s->data[i]` malloc 惯用法读写；按值拷贝只拷固定部分。plan 同上。
- `[ ]` **AGG-05** `(新)` 指定初始化器 `{.x=1}`。**完成**（2026-10-16，随 PAR-13）：见 PAR-13 条目；`[i]` 数组设计符、`.field` 嵌套、混排/乱序/重复（后覆盖）、未指定补零（局部整块 memset、全局 getNullValue）。plan 同上。
- `[x]` **AGG-06** `(新)` 位域字段布局与 codegen。**完成**（2026-10-15，P1-07）：LayoutBuilder 物化 System V 存储单元（非 packed）与 GCC packed 连续位流"区域"模型（`[N x i8]` 伪字段）；bitFields 布局表 {unitIndex,bitOffset,bitWidth,unitBits}；union 位域成员（对齐字段共享）；RMW 读写（loadBitFieldValue/storeBitFieldValue，含复合赋值/++/--）；全局/局部花括号初始化位域感知（常量 OR 进存储单元、无名成员不消费初始值）；class 继承链位域访问。

### class
- `[ ]` **AGG-07** `class` 声明与 `public/private/protected` 段。**部分完成**：访问段 + 默认 private（DEC-01）+ E2009 访问诊断已实现；`static` 成员/嵌套类型已完成（AGG-10/11，2026-10-05）。
- `[ ]` **AGG-08** 成员变量、成员函数声明/实现、隐式 `self`。
- `[ ]` **AGG-09** 成员函数编译为 `Vector_length(Vector* self)`；方法调用 `v.length()`。
- `[x]` **AGG-10** `static` 成员函数（无 `self`）与 `static` 成员变量。**完成**（2026-10-05，spec `docs/superpowers/specs/2026-10-05-static-members-design.md`）：方案 A 类前缀全局符号去糖（`Class::member` → `Class_member`，与 namespace 拍平同构）；仅类名限定访问（实例路径拒绝）；类内初始化器=全局定义（零初始化默认），不占对象布局；无 `this`；访问控制复用 DEC-01（E2009）；abi.md §4 补编码规范。struct 支持 static 变量（无方法通路；评审 C2 修复：仅含 static 成员的 struct 定义不再落入前向声明回退分支）。评审 I1 修复：static 成员访问级别独立键（`static:<name>`），同名实例成员级别不受覆盖。评审 I2 修复：static 方法先注册后分析体，可后向引用同类 static 方法。已知限制（INH-06 已落地 2026-10-05，protected 派生访问 2026-10-19 已放宽）：protected static 经 `Base::p` 从派生类访问已放行（`canAccessMember`）。
- `[x]` **AGG-11** 嵌套类型、前向声明。**完成**（2026-10-05，spec `docs/superpowers/specs/2026-10-05-nested-types-design.md`）：方案 A 解析期类前缀复用（类/struct/union 体解析压 `m_typeNamespacePrefix += 裸类名_`，与 namespace 同构）；DS1 全种类（enum/struct/class/union）任意深度嵌套，解析期扁平键注册（`Outer::Inner` → `Outer_Inner`）；DS2 成员循环三路判定（keyword+IDENT+`{` → 嵌套声明，平衡括号前瞻排除内联定义+声明符的 C 风格 idiom；keyword+`{` → 匿名内联字段 AGG-03；其余 → 字段）；DS3 引用侧零改动（`mangleQualifiedTypeName` 拍平命中）；DS4 sema `classPathPrefix` 推广 static 去糖公式（`ns::Outer::Inner::v` → `ns_Outer_Inner_v` 双侧严格一致），类嵌套枚举常量注册外层类作用域（`Outer::Red`，不注册裸键防全局泄漏）；DS5 private 嵌套类型访问控制（E2009，检查点 var decl + cast 目标）；DS6 前向声明+定义、自引用指针以测试钉住（codegen 既有类型提前 return 分支同样执行 nestedTypes 遍历）。评审修复：parseDeclaration 空定义回退分支纳入 nestedTypes 判定（防吞后续顶层声明）。已知限制：类体内裸自引用类型（self-type）解析期未注册（顶层类同样存在，含限定名变体）；类体内 unqualified 表达式引用须全限定（单一规则）。
- `[ ]` **AGG-12** 不生成 vtable/构造/析构/GC；生命周期由用户控制（栈/`malloc`/`free`）。
- `[ ]` **AGG-13** 布局等价 struct；`[[repr(C)]]` 下 ABI 稳定。

### enum
- `[x]` **AGG-14** 底层类型 `:uint8` 等；值/作用域/名称解析。隐式/显式值（含负值与常量表达式）、枚举常量名称解析（含 namespace 限定与 `cfg::Mode` 类型名）、switch 标签、显式底层类型 `enum E : uint8`、`typedef enum : uint16 {...}` 均已实现。
- `[x]` **AGG-15** 不隐式转换整数；显式转换语法与实现。见 TYP-20：隐式 `enum ↔ int` 被拒，显式 `(T)`/`static_cast`/`reinterpret_cast` 可用。
- `[~]` **AGG-16** 固定大小与 ABI；`(新)` 穷尽性检查（配合 SEM-11）。固定大小由底层类型决定，已随 AGG-14 实现；`switch` 穷尽性检查是警告还是错误待 **DEC-13** 定夺。

### union
- `[~]` **AGG-17** 声明、字段、布局、初始化与访问；不跟踪活跃成员（C 风格）。**已完成**：声明/`{42}` 初始化/拷贝赋值/嵌套 union/数组成员/指针 `->` 访问/`sizeof` 为最大成员/tag-union 惯用法（`tests/e2e/test_union.cpp` 8 项）；匿名 union 成员提升见 AGG-03。**待补**：union 按值传参/返回（随 MEM-14/TYP-27 聚合 ABI 一起）。

---

## 9. 继承与 CRTP（INH）〔新〕

- `[x]` **INH-01** **单继承**：`struct D : public B`；`public/private/protected` 说明符。**完成**（2026-10-05，spec `docs/superpowers/specs/2026-10-05-inheritance-design.md`）：仅支持公有继承（DS2 用户裁决：缺省一律 public，bare `class D : B` 亦 public——偏离 C++ 的 class 缺省 private 继承，记文档）；`: private B`/`: protected B` 明确诊断不支持；struct 形态补齐（parseStructDecl 继承子句 + StructType::baseClass/base + 布局基类子对象槽 0 + 字段访问沿链）。
- `[x]` **INH-02** **多继承不支持**：`struct D : B, C` 给出明确诊断。**完成**（2026-10-05）：基类名后遇 `,` 即诊断（此前静默误解析为前向声明）；诊断经 Parser::error（无 E 码，E2xxx 注册归 INF-13）。
- `[x]` **INH-03** 继承布局：基类子对象为派生类第一个字段（偏移 0）。**完成**（2026-10-05）：ClassType 路径既有实现 pin 测试钉住；StructType 路径本轮补齐（getLLVMType Struct case + StructDeclAST::codegen isClass 路径）；`[[repr(C)]]` 稳定性待 ANN（P1-05）落地后复查。
- `[x]` **INH-04** 转换：派生→基隐式；基→派生需 `static_cast`。**完成**（2026-10-05）：指针隐式上行、值赋值切片语义、`static_cast<Derived*>` 下行转换均以 e2e pin（此前已可用）。
- `[x]` **INH-05** **CRTP 必需项**。**完成**（2026-10-06，随 P1-03）：`this` 表达式（PAR-17，隐式 this 参数 + `this.field` 点号自动解引用）；基类支持模板实例 `: Shape<Circle>`（parse 捕获源拼写，sema 经 Registry 实例化，派生类前向占位可作实参）；基类实例方法体**惰性**（登记签名 + 首次调用 visit——派生类方法表未就绪时不检查）；`static_cast<Derived*>(this)` 向下转换（PAR-18 既有）。
- `[x]` **INH-06** 成员/方法沿单继承链查找；重写（非虚，静默遮蔽规则）。**完成**（2026-10-05，方案乙）：删除 sema 基类方法表复制循环，`resolveMethod` 沿 `base` 链查找并经 out-param 返回 definingClass——继承方法保留定义类的访问级别与 E2009 归属；私有基方法经派生实例从类外调用的洞闭合（行为收紧，pin）；派生同名重载不再被复制去重吞掉；字段访问沿链 walk 泛化至 StructType（sema + GEP 两路）。**protected 派生访问放宽完成**（2026-10-19）：`canAccessMember(definingClass, level)`——定义类内或（protected 且）派生类内经 `this->` 访问放行；private 仍仅定义类；类外仍 E2009。四检查点（static 成员/字段/方法/指定初始化器）。
- `[ ]` **INH-07** **不做**（Non-goals）：多继承、虚继承、菱形继承、虚函数/vtable/RTTI。**（2026-10-20 用户确认：虚函数/vtable 维持 Non-goal，理由：语言无隐藏控制流。）**

---

## 10. 函数（FUN）

- `[ ]` **FUN-01** 普通函数：声明/定义/调用/递归。
- `[ ]` **FUN-02** 参数传递：值/指针/Slice/模板参数。
- `[ ]` **FUN-03** 返回值：基础类型/struct/class/指针/Slice/Optional/Result。
- `[ ]` **FUN-04** `inline` / `extern` / `static` 函数。
- `[ ]` **FUN-05** `(已实现)` 函数指针：声明/赋值/调用/回调；补测试。
- `[ ]` **FUN-06** 可变参数：与 C varargs 兼容。
- `[ ]` **FUN-07** `(已实现)` 函数重载；补文档与测试。
- `[ ]` **FUN-08** `(已实现)` 运算符重载 `operator`；补文档与测试。
- `[x]` **FUN-09** `(新)` 默认参数（**不做**命名参数，见 DEC-04）。**完成**（2026-10-19）：`T name = const-expr` 仅尾随参数、值须编译期常量；`OverloadSet::resolve` 区间化（`[minArgs, paramCount]`）+ 并列偏好少默认；调用点折叠暂存 + codegen 补参（CallExprAST/MethodCallExprAST）。
- `[ ]` **FUN-10** `(已实现)` `defer`；补测试与文档。
- `[x]` **FUN-11** `(已实现)` `constexpr` 函数/常量；与 `compile_time` 关系已定（DEC-05：并存 + 共享内核，2026-10-06）。
- `[ ]` **FUN-12** `(新)` 程序入口约定：`main` 签名/参数/返回值，JIT 与原生一致（见 DEC-19）。
- `[ ]` **FUN-13** `(新)` 重载解析实现入口与诊断（实现 SEM-16）。
- `[~]` **FUN-14** `(新)` `to_string`/`to_hash`/`equals` 扩展点的精确签名与查找规则（自由函数/成员方法两条路径）。（草案见 `docs/spec/semantics.md`）

---

## 11. 指针、内存与 ABI（MEM）

- `[ ]` **MEM-01** `(已实现)` 完整 C 指针：解引用、取地址、指针运算、指针比较；补测试。
- `[ ]` **MEM-02** 地址转换：整数↔指针、不同指针类型显式转换。
- `[ ]` **MEM-03** `(已实现)` 函数指针；补回调与 ABI 测试。
- `[ ]` **MEM-04** 限定符：`const/volatile/restrict/atomic` 语义。
- `[ ]` **MEM-05** `[[nonnull]]`：调用处检查、函数内假设。
- `[~]` **MEM-06** `malloc/calloc/realloc/free` 声明与标准库包装。**声明在 `std.c`；包装在 `std.mem`**（2026-10-17：`alloc/alloc_zeroed/resize/dealloc/aligned_alloc/aligned_free`，STD-05；2026-11-01 后端切 mimalloc）。
- `[~]` **MEM-07** allocator 接口、arena、pool、placement 分配。**2026-11-01**：`std.mem::Allocator`/`Arena`/`Pool` 完成（mimalloc 后端）；placement 分配待补。- `[ ]` **MEM-08** 无隐藏分配；无隐藏控制流（不插入异常/GC/析构）。
- `[ ]` **MEM-09** ABI：`[[repr(C)]]`、`[[packed]]`、`[[align(64)]]`；位域布局（DEC-08）。
- `[x]` **MEM-10** FFI：与 C 函数/结构体/回调互操作（绑定层见 MOD-09）。已支持：`extern` 声明、函数指针形参（含无名与嵌套回调）、`T*`↔`void*` 传参与 C 风格类型转换 `(int*)p`/`((struct S*)p)->f`；libc `qsort/bsearch/atexit` 可直接传入用户函数作为回调。结构体**按值**传参/返回见 MEM-14。
- `[ ]` **MEM-11** 调用约定：C 调用约定（可选其他）。
- `[ ]` **MEM-12** 名称修饰（配合 MOD-08）。
- `[ ]` **MEM-13** `(新)` freestanding / 无 libc 目标（可选）。
- `[~]` **MEM-14** `(新)` 聚合传参/返回 ABI（byval/sret）与栈布局（配合 TYP-27）。（草案见 `docs/spec/abi.md`）
- `[~]` **MEM-15** `(新)` 端序与默认对齐规则；`[[packed]]`/`[[align]]` 交互（细化 MEM-09/DEC-08）。（草案见 `docs/spec/abi.md`）
- `[~]` **MEM-16** `(新)` 调用约定：C 默认约定细节、可变参数约定、参数寄存器/栈规则（细化 MEM-11）。（草案见 `docs/spec/abi.md`）

---

## 12. 编译期与反射（CT）〔新〕

> 以 `compile_time` 命名空间统一承载；**不再使用 `type_info(T)` 与独立 `static_assert` 关键字**。

- `[x]` **CT-01** `compile_time` 语法入口——非关键字；顶层 `compile_time.if`/`static_assert` parseDeclarationImpl 前瞻特判；表达式内 MethodCall/MemberAccess 钩子特判（2026-10-06）。
- `[x]` **CT-02** `compile_time.static_assert(cond, msg)`——顶层包装节点 + 函数体内表达式路径；失败诊断 `static_assert failed[: msg]` 带位置（2026-10-06）。
- `[x]` **CT-03** `compile_time.if(cond)`——仅顶层声明级（函数体内 YAGNI）；sema 期选择，codegen 只出选中分支；死分支类型毒化诊断（2026-10-06）。
- `[x]` **CT-04** 目标查询 `compile_time.target.os/arch/cpu`——host triple 映射（2026-10-06）。
- `[x]` **CT-05** 构建查询 `compile_time.build.debug/optimize/version`——`setBuildConfig` 注入（driver 按 optLevel 接线），无 CLI flag（2026-10-06）。
- `[x]` **CT-06** 编译期求值器 `CompileTimeEvaluator`——ConstValue 迁入 + STR；字面量/算术/位/比较/逻辑/三元/字符串 `==`/`!=`/`+`；size_of/align_of/offset_of 布局查询；constexpr 函数委托（DEC-05）（2026-10-06）。
- `[x]` **CT-07** 反射 API（取代 `type_info`）：类型名、大小、对齐、字段、偏移、属性。**完成**（2026-10-19）：`name_of`/`fields_of`/`attributes_of` 全做——`ReflectionRegistry`（sema→codegen 交接）+ `FieldInfo`/`Attribute` 内建类型 + 全局常量数组/slice 物化。
- `[ ]` **CT-08** 类型作为值：编译期与类型系统交互——另轮；本轮仅落地最小切片（size_of 族类型实参按名解析）。
- `[ ]` **CT-09** 编译期缓存：求值结果缓存、增量编译——另轮（随 MOD-10 协调）。
- `[~]` **CT-10** 编译期错误诊断——本轮仅完成「诊断带源码位置」；求值栈/原因链未做。
- `[~]` **CT-11** 编译期沙箱——YAGNI：求值器无 IO 能力即天然沙箱（2026-10-06 裁决）。
- `[x]` **CT-12** 与 LLVM 常量集成——表达式节点 `ctInt/ctFloat/ctHandled`，codegen 直接出 `llvm::Constant`，零运行时指令（2026-10-06）。
- `[x]` **CT-13** `(新)` `compile_time` 反射 API 的精确签名与返回类型。**完成**（2026-10-19）：`name_of(T)→string_view`、`fields_of(T)→Slice<FieldInfo>`、`attributes_of(T)→Slice<Attribute>`（见 `docs/spec/compile_time.md` §7）。
- `[x]` **CT-14** `(新)` `constexpr` 与 `compile_time` 边界与互操作——并存 + 共享内核（DEC-05 裁决，2026-10-06）。
- `[x]` **CT-15** `(新)` 编译期源码位置与诊断：`file/line/column/function`、`fail/warn/debug`、`assert_eq/ne/size/align/offset`（2026-11-01）。
- `[x]` **CT-16** `(新)` 类型谓词与数值极限：`is_same`/`is_*`、`int_min/max`、`uint_max`、`bit_width`、`float_*`（2026-11-01）。
- `[x]` **CT-17** `(新)` 字段/注解/枚举/类/继承编译期标量查询：`field_*`、`*_attribute_*`、`enum_*`、`method_*`、`base_name_of`/`is_base_of`（2026-11-01）。
- `[x]` **CT-18** `(新)` 目标/构建查询扩展：`target.endian/pointer_width/triple/has_feature/is`、`build.mode/compiler/host_*`、`version.*`（2026-11-01）。
- `[x]` **CT-19** `(新)` 编译期字符串：`format`、`to_string`（2026-11-01）。
- `[x]` **CT-20** `(新)` 类型值与模板内省：`type_of/array_of/pointer_to/const_of`、`is_convertible/is_assignable`、`template_name_of/template_arg_*`（2026-11-01）。
- `[x]` **CT-21** `(新)` `compile_time.repeat(N) { ... }` 编译期展开（上限 1024；零运行时循环）（2026-11-01）。
- `[ ]` **CT-22** `(新)` `compile_time.for_fields(T, f)` / `for_enum_values(E, v)`——需要 AST 克隆或 codegen 特化，单独设计（不伪装成运行时循环）。

---

## 13. 注解系统（ANN）

- `[x]` **ANN-01** 注解语法 `[[attribute]]`、解析与 AST 挂载——`Annotation/AnnotationArg/FieldInfo` 值类型，DeclAST/ParamDeclAST/字段直挂（2026-10-07）。
- `[x]` **ANN-02** `[[repr(C)]]`——仅 struct/union（class 报 E2011）；本轮布局已自然 C 兼容，语义为固化承诺 + 组合校验；TYP-21/MEM-09 复查落点（2026-10-07）。
- `[x]` **ANN-03** `[[packed]]`/`[[align(N)]]`——LayoutBuilder 单源化：packed=无填充、类型级 align=变量 setAlignment + sizeof 补齐、字段级 align=前置 padding（伪字段物化）；union 仅 align 有效（2026-10-07）。
- `[x]` **ANN-04** `[[inline]]`→alwaysinline、`[[cold]]`→cold（LLVM 函数属性）（2026-10-07）。
- `[x]` **ANN-05** `[[nonnull]]`（LLVM 参数属性 + 调用点字面空 W3005）、`[[deprecated]]`（使用处 W3004：函数/变量/类型/字段）（2026-10-07）。方法 nonnull 与 this 偏移挂账。
- `[x]` **ANN-06** 注解目标验证、冲突检测——E2010 未知注解 / E2011 目标不符 / E2012 重复 / E2013 align 非 2 的幂 / E2014 实参非常量（2026-10-07）。
- `[ ]` **ANN-07** 注解反射：可由 `compile_time` 读取——另轮（依赖 `attributes_of` + `Slice<Annotation>`，随 P1-06 str）。
- `[ ]` **ANN-08** 自定义注解预留机制——本轮保守策略：未知注解名 E2010；落地时降级注册制。
- `[x]` **ANN-09** `(新)` 各注解语义与默认行为——并入 `docs/superpowers/specs/2026-10-07-annotations-design.md` §3（2026-10-07）。
- `[x]` **ANN-10** `(新)` 无注解默认行为——并入同 spec §3（默认=现状逐字节不变，回归 pin）（2026-10-07）。

---

## 14. 字符串与格式化（FMT）

### 字符串
- `[x]` **FMT-01** `str`：UTF-8、不拥有、视图语义。完成（2026-10-08，随 P1-06）：`TypeKind::Str`，canonical LLVM named struct `"str" {ptr, i64}`；内建方法 `len()/char_count()/char_at(i)/char_len_at(i)`（MethodCall 机制）；`==`/`!=` 字节比较（长度优先，免越界读）。
- `[x]` **FMT-02** `String`：动态字符串、长度/容量、**C 语义内存管理（显式 `new/destroy`，无析构）**。完成（2026-10-08）：类型名钉死小写 `string`；`string.new(str)`/`destroy()`/`append(str)`/`push(char)`/`len()`/`capacity()`/`s[i]`；增长策略 `cap' = max(cap*2, need)`；`destroy` 清零三字段；无 SSO。
- `[x]` **FMT-03** 字符串字面量类型与 `char*` 互操作。完成（2026-10-08）：字面量重定型为 `str`；`str → char*` 隐式（`typesCompatible`/`checkAssignmentTypes`/`conversionRank`/`castValue` 四点同步）；`char* → str` 仅经内建 `str_from_c`（char 数组实参衰减）；print/println 的 str 实参走 `%.*s`。
- `[x]` **FMT-04** UTF-8 验证与遍历；`str`↔`String` 互转。完成（2026-10-08）：验证点 = 字面量与 `str_from_c` 字面量（sema 期，`support/Utf8`，E2015）+ 运行时缓冲（codegen 合成 IR `smc.utf8.validate`，失败 panic）；遍历 = `char_count`/`char_len_at` 步进 API（IR 辅助 `smc.utf8.*`），无 for-in 语法；`string → str` 隐式视图、`str → string` 显式 `string.new`。
- `[~]` **FMT-05** 切片、连接、比较、查找、替换。部分（2026-10-08）：`==`/`!=` 完成；`str + str` 不提供（拼接 = `string.new(a).append(b)`，CT-06 字面量 `+` 一并移除）。（2026-10-09）查找/分割完成：`find`/`rfind`/`sub`/`split` 内建（见 stdlib.md §9.1）；`replace` 未做。

### format
- `[x]` **FMT-06** `print/println` 内建，`{}` 占位符（2026-10-09：与 format 共享 PrintSpec 解析器，FMT-08 全规格自动生效；D4 裁决不引入 std.format import 形式，文档归 stdlib.md §10）。
- `[x]` **FMT-07** `format(fmt, args...) -> string` 内建（2026-10-09；user-defined 同名优先；字面量/动态 fmt 双路径）。
- `[x]` **FMT-08** 格式说明：`{}`、`{:x}/{:X}/{:o}/{:b}`、`{:f}/{:e}`、`{:02}`、`{:.2f}`、`{:<N}/{:>N}/{:^N}`、自定义 fill、sign（2026-10-09；`{:d}` 拒绝，十进制只用 `{}`；`{:b}` 依赖 glibc ≥ 2.35）。
- `[x]` **FMT-09** 编译期格式字符串解析与类型检查（SEM-08）（2026-10-09；字面量 fmt 全检；动态 fmt 只查实参可格式化，运行时契约见 stdlib.md §10.4）。
- `[x]` **FMT-10** 自定义类型格式化接口：`to_string`（类方法/自由函数两条路径，print/format 共享 lowerToString 管线）（2026-10-09）。
- `[x]` **FMT-11** 错误处理：参数不足/过多（E2020）、类型不匹配（E2021）、格式非法（E2022）——print/format 共用稳定诊断码（2026-10-09）。
- `[~]` **FMT-12** 性能：避免不必要分配。部分（2026-10-09）：format 全程单缓冲一次 malloc（长度可预知，无 realloc 路径）；str 实参零拷贝拼接。
- `[ ]` **FMT-13** `(新)` `print/println` 与 `printf` 的关系与统一语义；数值宽度/精度/类型的精确格式化规则（细化 FMT-08/SEM-08）。
- `[ ]` **FMT-14** `(新)` 自定义格式化 `to_string` 的签名与分派规则（细化 FMT-10/FUN-14）。

---

## 15. 并发（CON）

- `[x]` **CON-01** `atomic` 类型限定符：`atomic i32 count;`（见 DEC-11）。**完成**（2026-10-19，最小范围）：整型标量限定符（克隆类型不污染单例）+ 原子 load/store（seq_cst）+ 复合赋值/`++`/`--` 经 `atomicrmw`（add/sub/and/or/xor）；`* / % << >>` 与浮点/指针拒绝（需 cmpxchg，属 CON-02 边界）。
- `[ ]` **CON-02** 原子 load/store、CAS、fence。
- `[ ]` **CON-03** 内存序：relaxed/acquire/release/acq_rel/seq_cst。
- `[x]` **CON-04** `thread_local`：存储类语法（见 DEC-12）。**完成**（2026-10-19）：`thread_local T name;` 语法 + 存储（LLVM thread-local 全局，每线程副本）；函数上 thread_local 拒绝（E2028）；完整初始化/销毁模型属 CON-10。
- `[ ]` **CON-05** `std.thread`：线程创建/join/分离（C 语义，显式管理）。
- `[ ]` **CON-06** 线程局部存储 ABI；与 LLVM atomic 指令映射。
- `[ ]` **CON-07** 与 C11 atomics 互操作。
- `[ ]` **CON-08** 并发测试：数据竞争检测、压力测试。
- `[ ]` **CON-09** `(新)` `atomic` 语义模型：可原子化类型/大小限制、对齐要求、与 LLVM atomic 映射（细化 CON-01/02）。
- `[ ]` **CON-10** `(新)` `thread_local` 存储模型与初始化/销毁规则（细化 CON-04）。

---

## 16. 标准库（STD）

> 容器采用 **C 语义**：显式 `new/destroy`，不自动释放元素；泛型容器通过扩展点 `to_hash`/`equals`/`to_string` 适配用户类型（无 trait）。

### std.core
- `[x]` **STD-01** 基础类型导出；`panic/assert/abort`。`libs/std/core.smc` 已建（`min/max/clamp`）。`panic`/`assert` 实现为**语言内建**（需调用点编译期信息，无预处理器；见 `docs/spec/stdlib.md`）：`assert(cond)` 失败打印 `file:line: assertion failed` 后 `abort()`，`panic(msg)` 打印 `file:line: panic: <msg>` 后 `abort()`；`abort` 由 `std.c` 绑定层暴露。
- `[x]` **STD-02** `Optional<T>`、`Result<T,E>`（显式访问访问器）。由语言内建承载（2026-10-06）：伪字段即访问器（`.valid/.value`、`.ok/.value/.error`），无 trait/泛型前 stdlib 不设包装函数；见 `docs/spec/stdlib.md` §6。
- `[x]` **STD-03** 内存操作：`memcpy/memset/memmove/memcmp`。**完成**：`std.mem::bytes_copy/bytes_move/bytes_set/bytes_compare` 包装。
- `[ ]` **STD-04** 整数运算与溢出辅助。

### std.mem
- `[x]` **STD-05** `malloc/calloc/realloc/free` 包装；Allocator 接口；Arena/Pool；placement 分配；对齐工具。**完成**（2026-11-01）：后端切换 mimalloc（`mi_*`，静态链接 + override）；`libs/std/mem.smc` 提供 `alloc/alloc_zeroed/resize/dealloc/bytes_*/align_*/aligned_*` + `Allocator`/`Arena`/`Pool`（模块导出类型已可用）。placement 分配仍属后续。

### std.collections 〔新〕
- `[x]` **STD-06** `Vec`/`ArrayList`（增长、容量、索引、迭代）。**完成**：`std.collections::Vec<T>`（reserve/push/pop/get/get_ptr/insert/remove/contains/index_of）；可在 `constexpr` 求值器中执行并物化为只读常量表。
- `[x]` **STD-07** `HashMap`/`HashSet`（依赖 `to_hash`/`equals`）。**完成**：开放寻址 + 墓碑 + 0.7 负载；哈希/相等通过函数指针注入，`std.hash` 提供适配器；`constexpr` 求值与常量表物化已支持。
- `[~]` **STD-08** `Deque`、`List`、`BTree`（可选）。`Deque<T>` 环形缓冲完成；容器已拆分为 `libs/std/collections/{vec,deque,hashmap,hashset}.smc` 独立模块（`std.collections.*` 可直接导入），汇总模块 `std.collections` 保留；`List`/`BTree` 暂未实现。
- `[x]` **STD-09** 扩展点约定文档：`to_string`/`to_hash`/`equals`。`to_string` 为 print/format 降级路径；`std.hash::hash_key_*`/`equal_*` 提供 HashMap/HashSet 回调约定。

### std.string
- `[~]` **STD-10** `string_view`（原 `str`）、`String`、UTF-8 工具、字符串构建器、比较/查找/分割/拼接。核心完成（2026-10-09）：内建原语 find/rfind/sub/split/split_destroy + `libs/std/string.smc`（contains/starts_with/ends_with/trim*/join/concat/repeat/utf8_sub），文档 stdlib.md §9。I4 收口（2026-10-11）：`str` 硬改名 `string_view`、`string` 拥有型值语义（见 P1-09）。字典序 `<`/`>`、`replace` 不做（spec §8）；ASCII 大小写由 std.ascii 提供；独立 StrBuilder 可选（string 自身即 builder；模块导出类型已可用）。

### std.format
- `[~]` **STD-11** `format`、`print`、格式说明解析、编译期检查接口、自定义格式化。核心完成（2026-10-09）：format 内建 + FMT-08 规格集 + E2020~22 + to_string 降级 + 动态格式串契约，文档 stdlib.md §10。挂账：`import std.format` 体系（D4 裁决暂不引入）、自定义格式化钩子（Weight 之外的扩展点）、`printf` 关系统一（FMT-13）。

### std.io
- `[x]` **STD-12** `print`、`read`、stdout/stderr、文件读写、缓冲 I/O。`libs/std/io.smc` 已提供 `print_*`、`print_err_*`（stderr，经 `dprintf`）、`read_char/read_int/read_line`、`file_open/close/read/write`、`file_read_line/read_char/write_char/write_str`；基于 libc。

### std.math / std.bit
- `[x]` **STD-13** `sqrt` 等基础数学；浮点分类/舍入/常量；整数数学工具。**完成**（2026-10-17）：`libs/std/math.smc`——libc 绑定 18 个（sqrt/sqrtf/fabs/fabsf/floor/floorf/ceil/ceilf/round/roundf/trunc/truncf/pow/powf/cos/cosf/sin/sinf，`-lm` 链接）+ `constexpr float64 PI/E` + 位级 isnan/isinf/is_finite（IEEE754，不依赖 libc 分类宏）；5 e2e。
- `[x]` **STD-14** `std.bit`：popcount/clz/ctz/rotl/rotr/bitcast。**完成**（2026-10-17）：`libs/std/bit.smc` 纯语言实现——SWAR popcount、二分 clz/循环 ctz（对 0 返回位宽，冻结）、移位+或 rotl/rotr（n 按位宽取模）、reinterpret_cast bitcast 包装；7 e2e。

### std 其他 〔新〕
- `[x]` **STD-15** `std.parse`/`std.convert`：字符串↔数字、进制转换。**完成**：`parse_i32/i64/u64/float64/bool` + `to_string_i64/u64/i32/float64`，支持 2..36 进制及 `0x/0b/0o` 前缀。
- `[x]` **STD-16** `std.sort`、`std.algorithm`。**完成**：`sort/sort_slice/is_sorted/binary_search`（泛型、比较器回调、原地无分配）。
- `[x]` **STD-17** `std.hash`。**完成**：FNV-1a 字节/字符串、splitmix64 整数、`hash_combine` 与容器适配器。
- `[~]` **STD-18** `std.ascii`、`std.unicode`、`std.path`。`std.ascii` + `std.path` 完成；`std.unicode` 暂未实现。
- `[~]` **STD-19** `std.fs`（文件系统）。基础 `exists/is_file/file_size/read_file/write_file/append_file/remove_file/rename_path/create_dir/current_dir` 完成；目录遍历/stat 元数据待补。
- `[x]` **STD-20** `std.time`。`unix_seconds/unix_millis/process_ticks/process_seconds/sleep_ms` 完成。
- `[~]` **STD-21** `std.os`（进程/env/args/syscall 封装）。`env_get/env_set/env_unset/run/pid` 完成；argv/进程控制待补。
- `[~]` **STD-22** `std.testing`（语言内断言/测试；runner 由 `smc test` 提供）。运行时 `fail/expect/expect_eq_*` 完成；`smc test` runner 待补。
- `[x]` **STD-23** `std.c`：手写 libc 绑定层（配合 MOD-09）。`libs/std/c.smc`（内嵌兜底）已扩展：`abort`、`dprintf`（stderr，fd=2）、`fopen/fclose/fread/fwrite/fgets/fputs/fgetc/fputc/fprintf`、`scanf`。

### std.atomic / std.thread
- `[ ]` **STD-24** 原子类型封装、load/store/CAS/fence、内存序常量。
- `[ ]` **STD-25** `Thread`、join/detach、`thread_local` 支持；可选 mutex/condvar。
- `[~]` **STD-26** `(新)` 扩展点库约定：`to_string`/`to_hash`/`equals` 标准签名与示例（落实 STD-09/FUN-14）。（接口草案见 `docs/spec/semantics.md`）
- `[x]` **STD-27** `(新)` `panic/assert/abort` 语义与实现策略：**不可恢复、不可捕获、直接 `abort`**（DEC-21 已冻结）；诊断输出到 stderr 且带 `file:line`；`assert`/`panic` 为内建（用户同名声明优先）。规范见 `docs/spec/stdlib.md`。与 `[[nonnull]]` 的关系（复用终止路径）待 MEM-* 实现。

---

## 17. LLVM 代码生成与后端（CG）

- `[ ]` **CG-01** IR 类型映射：基础类型/指针/数组/struct/class/slice/Optional/Result。
- `[ ]` **CG-02** `(已实现)` 全局变量/常量/字符串字面量；补测试。
- `[ ]` **CG-03** `(已实现)` 函数定义与调用；补测试。
- `[ ]` **CG-04** `(已实现)` 控制流：if/while/do-while/for/switch/break/continue；补测试。
- `[ ]` **CG-05** `(已实现)` 表达式 codegen（含指针 GEP、fneg、前缀/后缀 `++/--`）；补测试。**已知缺陷**（2026-10-22）：`f().field`（struct/class 返回值的成员访问）对 call 结果直接生成 GEP → 非法 IR（`base of getelementptr must be a pointer`）；需先物化 rvalue 临时对象。
- `[ ]` **CG-06** class 方法降级 `Vector_length(Vector* self)`、`static` 成员变量降级。
- `[x]` **CG-07** **模板单态化代码生成**（GEN-05）、实例去重缓存。已实现（2026-10-06）：实例即普通声明走既有 codegen；TU 预扫（实例布局先行 + 函数签名）+ 调用点按需前向声明。
- `[x]` **CG-08** **继承布局与 `this`/`static_cast` codegen**（INH）。**已完成**（2026-10-05，随 INH-01~05/P1-03）：基类子对象槽 0、`this` 隐式参数、`static_cast<Derived*>` 下行、模板基类实例化（CG-07 同轮）。
- `[x]` **CG-09** **位域 codegen**：`iN` 与读-改-写位操作。**完成**（2026-10-15，P1-07）：统一读点 readLValue 分派位域 → loadBitFieldValue（shift+trunc+sext/zext 至基类型）；storeBitFieldValue 掩码 RMW；Assignment/CompoundAssign/PreInc/PostfixIncDec 位域路径；emitBitFieldGEP 沿继承链定位存储单元；union 分支记录 unitBits。
- `[x]` **CG-10** **内联 `asm`** → LLVM `InlineAsm`（操作数约束/clobber/volatile/目标限定）。**完成**（2026-10-11，P1-08）：`AsmExprAST::codegen` 用 `llvm::InlineAsm`（AD_ATT）；输出映射 call 返回值（单值/struct）、`+r`→`=r`+尾部 tied 约束、GCC 模板翻译 `$`→`$$`/`%%`→`%`/`%N`→`$N`；e2e 6 用例 LLJIT 执行通过。
- `[x]` **CG-11** **lambda codegen**：无捕获→函数指针；有捕获→匿名 struct + 函数。**完成**（2026-10-17，随 P1-07）：无捕获 → 静态函数 `__lambda_<N>` + 表达式 bitcast 函数指针（可赋 `int32 (*)(int32)`、qsort 回调、直接调用）；有捕获 → 闭包 struct（`__lambda_<N>`，按值拷贝/按引用指针）+ 调用函数 `__lambda_<N>__call(Closure*, params...)`（body 捕获名按引用写穿原对象）；立即调用经 `LambdaCallExprAST`（栈上构造闭包 → call）。
- `[x]` **CG-12** `compile_time` 常量折叠后 codegen；与 LLVM 常量集成。**已完成**（2026-10-06，随 CT-12）：表达式节点 `ctInt/ctFloat/ctHandled`，codegen 直接出 `llvm::Constant`，零运行时指令。
- `[ ]` **CG-13** 原子操作与 `thread_local`。
- `[ ]` **CG-14** 调试信息：DWARF、源码位置。
- `[ ]` **CG-15** 优化级别 O0/O1/O2/O3/Os/Oz。
- `[ ]` **CG-16** 目标三元组与交叉编译。
- `[ ]` **CG-17** 链接：启动代码、libc、静态/动态库；无强制运行时。
- `[ ]` **CG-18** ABI 一致性测试（含位域、继承、模板符号）。

---

## 18. 工具链与驱动（TOOL）

- `[ ]` **TOOL-01** `(已实现)` 编译器 CLI：`check/build/run`；补 `smc test` runner（STD-22）。
- `[x]` **TOOL-02** `(改)` 移除 C 预处理器相关：`-E/-I/-D`、`src/preprocessor/`、`MacroTable` 及测试（Non-goals）。已删除源码与测试；`-I` 由 `-M/--module-path` 取代。
- `[ ]` **TOOL-03** 调试器集成：DWARF、断点、变量查看。
- `[ ]` **TOOL-04** 交叉编译工具链配置。
- `[ ]` **TOOL-05** 构建系统集成：Make/Ninja/CMake。
- `[ ]` **TOOL-06** 迁移工具：C → SafeModern C 渐进迁移（注意：无 `#include`/goto，需说明限制）。
- `[ ]` **TOOL-07** **不做**（Non-goals）：`smc fmt`、`smc doc`、LSP、包管理器、依赖解析、registry、manifest。
- `[ ]` **TOOL-08** `(新)` `-std=`/语言版本标志与默认版本（实现 INF-12）。
- `[ ]` **TOOL-09** `(新)` 诊断输出格式与错误码注册表（实现 INF-13）。

---

## 19. 测试、验证与发布（TST）

- `[ ]` **TST-01** 词法/语法单元测试；类型系统单元测试。
- `[ ]` **TST-02** 语义分析与诊断快照测试。
- `[ ]` **TST-03** 每个语言特性的集成测试（含模板/CRTP/位域/lambda/asm/匿名类型）。
- `[ ]` **TST-04** ABI/FFI 测试；泛型单态化测试；comptime 测试；格式字符串测试；并发测试。
- `[ ]` **TST-05** `(新)` 数组/Slice 边界检查与整数溢出检查测试（Debug/Release 两种行为）。
- `[ ]` **TST-06** `(新)` **ASan / UBSan** 集成；UB 清单（SEM-13）对应用例。
- `[ ]` **TST-07** `(新)` **覆盖率**（llvm-cov）报告与阈值。
- `[ ]` **TST-08** 跨平台测试 Linux/macOS/Windows；交叉编译测试。
- `[ ]` **TST-09** 可选：差分测试（对照 clang/gcc）、模糊测试（解析器/comptime/格式串）。
- `[ ]` **TST-10** 性能基准：编译速度、运行速度、代码大小（含方法学）。
- `[ ]` **TST-11** 回归测试套件；标准库文档测试。
- `[ ]` **TST-12** 发布流程：二进制包、变更日志（**无版本管理/edition**，见 Non-goals）。

---

## 20. 待定决策（DEC）

- `[x]` **DEC-01** class 默认访问级别：**class 默认 private，struct 默认 public**（C++ 惯例）；`public:`/`private:`/`protected:` 段切换（`protected` 为上下文关键字，继承落地前 ≡ private，见 SEM-04/INH）。实现见 `src/frontend/Parser.cpp`（记录）与 `SemanticAnalyzer`（检查，E2009）。
- `[x]` **DEC-02** `import math;` 后符号访问语法：**直接非限定访问**（`add`）。模块是物理边界，不引入 `math.add` 式运算符；限定名由模块内 `namespace` 提供。
- `[x]` **DEC-03** `Result<T,E>` 精确布局与 `.error`/`.value` 语义（显式访问，无 `?`）。**裁决**（2026-10-06，DS4）：`{ bool ok; T value; E error; }` 三字段加判别标志——两字段草案无法区分成功/失败，哨兵约定与任意 E 冲突；与 Optional `{ bool valid; T value; }` 对称。实现见 TYP-14。
- `[ ]` **DEC-04** 默认参数范围；**确认不做**命名参数。
- `[x]` **DEC-05** `constexpr` 与 `compile_time` 的关系——裁决：并存 + 共享求值器内核（2026-10-06）；constexpr 保持既有语义（运行时可调用），CT 表达式内 constexpr 函数调用委托 `evaluateConstexpr`。
- `[ ]` **DEC-06** Slice/数组边界检查默认策略：静态可证 or 运行时检查，Debug/Release 差异。
- `[ ]` **DEC-07** 整数溢出默认行为：Debug 检查 / Release 回绕。
- `[x]` **DEC-08** 位域布局对齐哪个 C ABI（System V / MSVC）；与 `[[packed]]`/`[[align]]` 交互。**完成**（2026-10-15，P1-07）：对齐 System V / GCC x86-64（非 packed）；packed 用 GCC packed 语义（连续位流 + 零宽按声明类型自然边界取整），各尺寸与 gcc 实测逐一核对；`[[packed]]` 下 bit-field 自然对齐不抬整体对齐。
- `[x]` **DEC-09** 内联 `asm` 的语法与约束模型；是否仅限特定目标。**完成**（2026-10-11，P1-08）：sema 校验 outputs 左值标量/inputs 标量/约束非空（E2023）+ 模板 `%` 转义（E2024）；方言固定 AD_ATT、约束语义透传 LLVM（D2）；目标限定 x86-64 宿主基线。
- `[x]` **DEC-10** lambda 精确语法与捕获模型（默认按值/按引用？）。**完成**（2026-10-17）：`[captures](params) -> Ret { body }` C++ 风格；按值 `[a]`/按引用 `[&a]`/`[=]` 全部按值；**语言无 `auto` → 有捕获闭包不可命名，实际形态为立即调用**（DEC 裁决，见 spec `docs/superpowers/specs/2026-10-17-lambda-design.md`）。
- `[ ]` **DEC-11** `atomic` 是类型限定符还是类型构造器。
- `[ ]` **DEC-12** `thread_local` 存储类语法细节。
- `[ ]` **DEC-13** enum 穷尽检查是警告还是错误。
- `[ ]` **DEC-14** 模块缓存格式与稳定性。
- `[ ]` **DEC-15** 是否默认链接 libc（当前 `print` 依赖系统 `printf`）。
- `[x]` **DEC-16** `(新)` 类型别名规范形式：**裁决（2026-10-23）**——`using` 为唯一形式；`typedef`/`type` 均移除（硬错误 + 改写提示）。
- `[x]` **DEC-17** `(新)` `namespace` 与模块（`module`/`import`）的关系与共存方式：**模块=物理边界，namespace=逻辑边界，二者正交**；同一模块可含多个 namespace，同一 namespace 可跨模块。
- `[x]` **DEC-18** `(新)` `register`/`cast`/`typeof` 等既有 token 的废弃或保留。→ **决定移除**（一并移除 `comptime`/`generic`）；显式转换改用 `static_cast`/`reinterpret_cast`（LEX-11），类型查询改用 `compile_time` 反射（CT-07）。已同步 `Token.h`/`Lexer.cpp`/`Utils.cpp`/`keywords.md`。
- `[ ]` **DEC-19** `(新)` `main` 入口签名与返回值约定。
- `[x]` **DEC-20** `(新)` 字符串字面量所有权/生命周期与 `str`/`String` 边界。→ **决定**（2026-10-08 随 P1-06）：字面量即 `str`（静态存储、全局生命周期、不拥有）；`string` 拥有其缓冲区，显式 `string.new`/`destroy`，无隐式堆分配（`str`/`char*` 赋给 `string` 编译期拒绝）；`string → str → char*` 视图链隐式、反向一律显式；`destroy` 后字段清零（二次 destroy 走 `free(NULL)` 无害）。spec `docs/superpowers/specs/2026-10-08-str-string-design.md`。
- `[x]` **DEC-21** `(新)` `panic` 默认可否被捕获、是否直接 `abort`。→ **决定**：不可捕获（语言无异常/无栈展开，NG-05），直接调用 `abort()`；`assert`/`panic` 走同一终止路径，输出带调用点 `file:line`（见 `docs/spec/stdlib.md` §8）。

---

## 21. 明确不做（Non-goals）

- `[x]` **NG-01** `goto` / 标签（现有 `GotoStmtAST`/`LabelStmtAST` 与解析分支需移除）。已移除 AST/解析/语义/代码生成与测试。
- `[x]` **NG-02** 传统 C 预处理器与宏（`#include/#define/#if/#ifdef/#pragma once`、`-E/-I/-D`）。已移除 `src/preprocessor/`、`MacroTable`、`-E/-I/-D` 及测试；`#` 换以明确报错。
- `[ ]` **NG-03** trait / interface / concept（泛型无约束，靠运算符重载与扩展点）。
- `[ ]` **NG-04** 所有权 / 借用 / 生命周期模型（安全能力仅限边界检查 + 空指针检查 + UB 检查）。
- `[ ]` **NG-05** 错误传播算子 `?`（Result/Optional 一律显式判断）。
- `[ ]` **NG-06** 版本管理 / edition。
- `[ ]` **NG-07** 包管理器、依赖解析、registry、manifest。
- `[ ]` **NG-08** LSP、`smc fmt`、`smc doc`（保留 `smc test`）。
- `[ ]` **NG-09** 多继承 / 虚继承 / 菱形继承 / 虚函数 / vtable / RTTI / 构造析构 / GC。
- `[ ]` **NG-10** 模板特化/偏特化、SFINAE、可变参数模板、模板模板参数、concepts、默认模板实参。
- `[ ]` **NG-11** `fn` 关键字与 Rust 风格命名（统一 C 风格声明）。
- `[ ]` **NG-12** `type_info(T)` 与独立 `static_assert` 关键字（统一由 `compile_time` 承载）。
- `[ ]` **NG-13** 命名参数。

---

## 22. 建议实现顺序（里程碑）

### P0：最小可用编译器（已有大量基础，补齐缺口）
- `[x]` **P0-01** 清理与决策一致的冲突：移除预处理器、goto/label、`-E/-I/-D`（NG-01/02/TOOL-02）。已完成；`-I` 由 `-M/--module-path` 取代。
- `[x]` **P0-02** C 互操作绑定层（MOD-09/STD-23）——无预处理器后的刚需。文件化 `libs/std/c.smc` + `extern` 已可用；函数指针形参/嵌套回调、C 风格指针转换、`qsort/bsearch/atexit` 已补齐（MEM-10）。
- `[x]` **P0-03** 变量初始化检查（SEM-01/02）。已实现（W3001，含分支合并/循环；`-Werror` 可将警告变失败）。
- `[x]` **P0-04** module/import/export（MOD-04~07）。源文件 `import`（搜索路径/点分名/循环报错）、`module` 文件绑定与校验、`export` 可见性、每模块私有作用域均已实现。
- `[x]` **P0-05** 最小 `std.core` / `std.io`（STD-01/12）。`std::min/max/clamp`、`std::print_*`、`std::read_*`、`std::file_*` 可用；`assert`/`panic` 内建、`abort` 经 `std.c`；规范 `docs/spec/stdlib.md`。
- `[x]` **P0-06** 诊断系统与测试设施（INF-03/04）。诊断核心 + 快照设施 + 词法/语法/声明源码位置均已完成；INF-04 的其余测试框架项（黄金 IR/输出、CTest 分组标签）由 INF-04 单独跟踪。
- `[x]` **P0-07** `(新)` **语言规范骨架**：EBNF + 关键字表 + 优先级表 + 转换矩阵（INF-06/09~11、PAR-23、TYP-22/23）。四件套位于 `docs/spec/`，并由 `tests/spec/test_spec_conformance.cpp` 绑定实现；各 `[plan]` 细节仍由对应条目跟踪；TYP-22（整数提升/常规算术转换）已完成。
- `[x]` **P0-08** `(新)` **namespace 支持**（PAR-22、MOD-12、DEC-17）。历史需求来源为 `libsafec`（已删除）；函数/变量、嵌套、限定访问、限定类型名均已实现。

### P1：核心现代能力
- `[x]` **P1-01** class / enum / union / 数组 / Slice（AGG、TYP-11/12）。enum、class 访问段（DEC-01）、union（AGG-03/17）、Slice（TYP-12）、嵌套类型（AGG-11，2026-10-05）全部完成。
- `[x]` **P1-02** Optional / Result 显式访问（TYP-13/14/STD-02）。完成（2026-10-06）：内建魔术类型（同 Slice 先例）；`Optional<T>`/`Result<T,E>` 类型位置尖括号 + `T?` 糖；严格类型相等；codegen 全通路（e2e 7 项）；DEC-03 随此轮裁决；spec `docs/superpowers/specs/2026-10-06-optional-result-design.md`，plan `docs/superpowers/plans/2026-10-06-optional-result.md`。
- `[x]` **P1-03** **泛型 + CRTP**（GEN、INH-05、PAR-17/18、CG-07/08）。2026-10-06 完成：989/989。
- `[x]` **P1-04** `compile_time` 与反射——核心纵向切片完成（2026-10-06）：CT-01/02/03/04/05/06/12/14 + PAR-15 部分 + SEM-07 + DEC-05；反射 CT-07/08/13 另轮（与 P1-06 str 排序协调）。
- `[x]` **P1-05** 注解系统（ANN）——核心纵向完成（2026-10-07）：ANN-01~06/09/10 + LEX-10/PAR-07 + W3004/W3005；ANN-07/08 挂账。
- `[x]` **P1-06** str / String / format（FMT、STD-10/11）。核心纵向完成（2026-10-08）：FMT-01/02/03/04 + FMT-05 比较 + TYP-26/DEC-20 + 内建类型小写化（`optional`/`result`，DEC-18 先例）；E2015 新诊断；`grammar.ebnf` 同步。format（FMT-06~09）与 STD-10 高层操作另轮。spec `docs/superpowers/specs/2026-10-08-str-string-design.md`，plan `docs/superpowers/plans/2026-10-08-str-string.md`。挂账：`string` 不参与 constexpr；CT `+` 已移除；SSO 不做。
- `[x]` **P1-07** 位域、匿名类型、指定初始化器、lambda（AGG-03~06、PAR-10~13）。**位域全链路已完成**（2026-10-15：PAR-10/SEM-12/AGG-06/CG-09/DEC-08）；**指定初始化器与柔性数组已完成**（2026-10-16：PAR-13/AGG-04/05）；**匿名类型定义已完成**（2026-10-17：PAR-12）；**lambda 已完成**（2026-10-17：PAR-11/CG-11/DEC-10，plan `docs/superpowers/plans/2026-10-17-lambda.md`，全量 1351 绿）。**P1-07 全部完成**。
- `[x]` **P1-08** `static_cast` / 内联 `asm`（PAR-18、CG-10）。`static_cast`/`reinterpret_cast` 已完成（LEX-11/PAR-18）；内联 `asm` 全链路完成（2026-10-11）：LEX-12/PAR-16/CG-10/DEC-09，spec `docs/superpowers/specs/2026-10-10-p108-inline-asm-design.md`，plan `docs/superpowers/plans/2026-10-10-p108-inline-asm.md`，e2e 6 用例 LLJIT 执行通过（全量 1203 绿）。
- `[x]` **P1-09** std.mem / std.string / std.format / std.math / **std.collections**（STD-05~14）。字符串库 + format 体系完成（2026-10-09）：FMT-06~11 全落地、FMT-12 单缓冲、STD-10 组合库、STD-11 核心；E2020~22 新诊断；顺带修复既有编译器 bug 两枚——`&&`/`||` 按位降级（真值截断 + 无短路）与派生→基类赋值整结构 store 溢出（潜在栈腐败）。spec `docs/superpowers/specs/2026-10-09-p109-string-format-design.md`，plan `docs/superpowers/plans/2026-10-09-p109-string-format.md`。模块导出类型/模板已可用；独立 StrBuilder 为可选优化；`replace`/字典序不做（ASCII 大小写已由 std.ascii 提供）；std.mem/math/collections 已补齐。**终审 I4（string 值拷贝别名双 free）已收口**（2026-10-11）：string 拥有型值语义 + string_view 硬改名，spec `docs/superpowers/specs/2026-10-11-string-ownership-design.md`，plan `docs/superpowers/plans/2026-10-11-string-ownership.md`，e2e 16 用例（全量 1229 绿）。**std.mem 纯函数层完成**（2026-10-17，STD-05 范围 a：alloc/alloc_zeroed/resize/dealloc/align_up/align_down/is_aligned/aligned_alloc/aligned_free，7 e2e，全量 1358 绿；Allocator/Arena/Pool 接口草案挂账于「模块导出类型」语言缺口，见 stdlib.md §11.1）。**std.math/std.bit 完成**（2026-10-17，STD-13/14：libs/std/bit.smc 纯语言位工具 7 e2e + libs/std/math.smc libc 绑定/常量/位级分类 5 e2e，顺带修复 namespace 内 C 名 extern 符号前缀化缺口，全量 1370 绿；spec/plan `docs/superpowers/specs|plans/2026-10-17-stdmath-bit*`）。**std.collections/扩展库完成**（2026-11-01）：Vec/Deque/HashMap/HashSet + hash/sort/parse/ascii/path/fs/time/os/testing；mem 切 mimalloc + Allocator/Arena/Pool；全量 1657/1657（2 个既有 Skip；移除 `libsafec` 自身测试后）。

### P2：工程化与并发
- `[ ]` **P2-01** atomic / thread_local / std.thread（CON、STD-24/25）。
- `[~]` **P2-02** 标准库补齐：parse/sort/algorithm/hash/bit/ascii/unicode/path/fs/time/os/testing/c（STD-15~23）。parse/sort/hash/ascii/path/fs/time/os/testing 基础层完成；unicode/完整算法/完整 fs/os 待补。
- `[ ]` **P2-03** 增量编译与模块缓存（MOD-10）。
- `[ ]` **P2-04** ABI/FFI 完整测试（CG-18）。
- `[ ]` **P2-05** 跨平台与交叉编译（CG-16、TOOL-04）。
- `[ ]` **P2-06** ASan/UBSan/覆盖率（TST-05~07）。

### P3：1.0 稳定
- `[ ]` **P3-01** 性能优化（CG-15、TST-10）。
- `[ ]` **P3-02** ABI 冻结（无 edition，靠 ABI 测试 + 文档）。
- `[ ]` **P3-03** 规范定稿与文档（INF-06）。
- `[ ]` **P3-04** 标准库稳定；迁移指南（TOOL-06）。
- `[ ]` **P3-05** freestanding 目标（MEM-13，可选）。

---

## 23. 验收总原则

- `[ ]` **ACC-01** 所有特性映射到可预测的 C 内存模型与 ABI。
- `[ ]` **ACC-02** class 只是语法封装：无 vtable、构造、析构、GC、隐藏生命周期。
- `[ ]` **ACC-03** 无隐藏分配、无隐藏控制流、无强制运行时。
- `[ ]` **ACC-04** `compile_time` 不产生运行时开销。
- `[ ]` **ACC-05** 保持 C 风格调用模型与 FFI 能力（经 `extern` 绑定层）。
- `[ ]` **ACC-06** 静态分析优先；运行时检查可配置且不默认破坏性能。
- `[ ]` **ACC-07** UB 行为对齐 C 标准（SEM-13），并由 ASan/UBSan 验证。

---

## 24. 语法缺口审计（2026-10-27，第三轮后）

> 范围：仅列**语法/解析层**尚未覆盖、或只覆盖简化形式的能力；语义/ABI/codegen 边界见 §24.4。
> 方法：对照 `docs/spec/grammar.ebnf`、本文件，并用 `build/bin/my_llvm_c` 对每个例子做 `-fsyntax-only` 与完整编译探测。
> 基线：基础 C 声明/语句 9 项收口后，全量 **1572/1572**，两个示例 OK；本轮只做审计，未改代码。
> 结论：基础 C 主干已基本收口；剩余集中在模板常量实参、块内函数声明、C 声明符完整性，以及规范未覆盖的现代语法扩展。

### 24.1 规范已承诺、实现未收口（优先）

#### SG-01 模板常量表达式实参 / 依赖值实参 `[fixed][P1]`

```c
template<int32 N> struct A { int32 x; };
A<1 + 2> a;                         // 已支持：常量表达式实参

template<int32 N> struct B { A<N> a; };  // 已支持：依赖值实参
template<int32 N> int32 g() { return N; }
g<1 + 1>();                         // 已支持
g<N>();                             // 已支持（模板体内）
```

- 规范：`template-argument = type | const-expr`。
- 目标：解析期/实例化期求值常量表达式；依赖值参数在单态化时替换。
- 关联：GEN-02、GEN-04、PAR-21、SEM-06。

- 2026-10-31 完成：类型/调用位置接受整型常量表达式；依赖值参数表达式（`N`、`N + 1`）在实例化时替换。

#### SG-02 块内 `extern` 函数声明 `[fixed][P1]`

```c
int32 main() {
    extern int32 puts(const char* s);
    puts("x");
    return 0;
}
```

- 当前：块内 `extern` 只按变量声明解析，遇 `(` 报 `expected ';' after variable declaration`。
- 已支持：块内 `extern int32 g;`（提交 `dfcb9cc`）。
- 目标：块内 `declaration-statement` 接受 `extern` 函数原型；sema 按外部函数声明处理。
- 关联：PAR-02、MOD-09、MEM-10。

- 2026-10-31 完成：块内 `extern int32 f(...)` 解析为函数原型，sema 在块作用域注册，codegen 复用模块级声明。

### 24.2 C 兼容性 / 声明符文法只覆盖简化形式

#### SG-03 `offsetof` 常量表达式下标 `[fixed][P1]`

```c
offsetof(S, a[1 + 1]);   // 已支持：常量表达式下标
offsetof(S, a[K]);       // 已支持：枚举常量下标
```

- 规范现状：`member-designator = identifier { '.' identifier | '[' integer-literal ']' }`。
- 已支持：`offsetof(S, a[1])`、`offsetof(S, a.b)`、嵌套成员。
- 目标：若按 C 语义补齐，下标改为 `const-expr`，在 parse/sema 期求值。
- 关联：PAR-14。

- 2026-10-31 完成：`offsetof` 下标改为常量表达式，sema 折叠（含枚举常量）。

#### SG-04 C 风格强转到抽象函数指针类型 `[fixed][P1]`

```c
((int32 (*)(int32))p)(1);        // 已支持：完整 type-id
using F = int32 (*)(int32);
((F)p)(1);                       // 可绕过
```

- 当前：C 风格 cast 的类型分支仍走 `parseType()`；`sizeof`/`alignof` 已改走完整 `parseTypeId()`。
- 目标：cast 的 type-id 支持抽象函数指针/指针数组等声明符；数组本身仍不能作 cast 目标。
- 关联：PAR-14、PAR-18。

- 2026-10-31 完成：C 风格 cast 的类型分支改走完整 type-id，支持抽象函数指针。

#### SG-05 深层嵌套声明符 `[fixed][P2]`

```c
int32 (*(*g)(int32))(int32);
```

- 当前：单层函数指针、函数返回函数指针、函数指针数组已支持；多层嵌套仍失败。
- 目标：声明符递归（内层后缀 + 指针 + 外层后缀，反向应用）覆盖任意可表达嵌套。
- 关联：PAR-05。

- 2026-10-31 完成：声明符解析改为递归构造器列表，支持任意嵌套括号/指针/后缀。

#### SG-06 C99 数组形参限定符 `[fixed][P2]`

```c
int32 f(int32 a[static 3]);       // 已支持：数组形参限定符
```

- 当前：`static` 被当作数组长度表达式，报 VLA/数组大小错误。
- 说明：这不是 VLA；`static` 表示数组形参的最小长度。
- 目标：形参数组后缀接受 `static`/`const`/`volatile`/`restrict` 限定。
- 关联：TYP-11、PAR-05。

- 2026-10-31 完成：数组形参后缀接受 `static`/`const`/`volatile`/`restrict`。

#### SG-07 相邻字符串字面量拼接 `[fixed][P2]`

```c
string_view s = "a" "b";          // 已支持：相邻字面量拼接
```

- 当前：两个独立字符串 token，报 `expected ';' after variable declaration`。
- 目标：按 C 翻译阶段拼接相邻字符串字面量（lexer 末尾或 parser 初始化/实参位置均可，需统一）。
- 关联：LEX-06。

- 2026-10-31 完成：parser 在字符串主表达式处拼接相邻字符串 token（含 raw/多行）。

### 24.3 规范未覆盖的现代/C++ 兼容扩展（需先裁决，不直接当 bug）

#### SG-08 namespace 的 using 语法 `[ext][P2]`

```c
using namespace A;
using A::f;
namespace B = A;
```

- 当前：三者均解析失败；只支持 `using X = Type;` 类型别名。
- 已支持：`namespace A::B { ... }` 嵌套命名空间、限定名 `A::f`。
- 说明：`grammar.ebnf` 未定义 using-declaration / namespace-alias；需补文法与作用域/可见性规则。
- 关联：PAR-22。

#### SG-09 lambda 扩展捕获与 `noexcept` `[ext][P2]`

```c
[&]() { ... }
[=, &x]() { ... }
[y = x]() { ... }
[]() noexcept { ... }
```

- 当前：均解析失败。
- 已支持：`[]`、`[=]`、`[a]`、`[&a]`；文法只承诺这四种。
- 目标：若扩展 DEC-10，需同时定义隐式捕获集合、初始化捕获生命周期、`noexcept` 语义（当前无异常）。
- 关联：PAR-11、DEC-10。

#### SG-10 注解的扩展形式 `[ext][P2]`

```c
[[gnu::packed]] struct S { ... };
[[inline, cold]] int32 f();
int32 f() [[nodiscard]];
enum E { A [[deprecated]] };
```

- 当前：均解析失败。
- 已支持：简单单名注解，如 `[[packed]]`、`[[align(8)]]`、`[[deprecated]]`。
- 说明：`grammar.ebnf` 只定义单个简单 `annotation-name`，无命名空间限定、逗号列表、尾置位置、枚举成员挂载。
- 关联：ANN-01、PAR-07。

#### SG-11 class 语法扩展 `[ext/ng][P2]`

```c
class C { public: int32 get() const; };       // const 成员函数
class C { public: operator int32(); };         // 转换运算符
class C { friend int32 get(C); };              // friend
class D : B { using B::f; };                   // 类内 using 基类成员
class C { compile_time.if (true) { int32 x; } };  // 类体 compile_time.if
enum class E { A };                            // enum class
class C { public: template<typename T> T id(T x); }; // 成员模板
```

- 当前：以上均解析失败。
- 规范现状：`class-member = access-section | field-decl | function-decl | type-alias`，不含上述形式；`enum-decl` 不含 `class`。
- 裁定：
  - 构造/析构/GC 仍是 **Non-goal**（ACC-02、NG-09）；构造当前虽能“解析”，sema 会因类型未完成而拒绝，不应视为已支持。
  - 其余项（`const` 成员、转换运算符、`friend`、类内 `using`、类体 CT-if、`enum class`、成员模板）均需单独 DEC；成员模板若纳入，应与 GEN-08 的模板成员方法能力区分。
- 关联：PAR-04、PAR-21、GEN-08、ACC-02、NG-09。

### 24.4 不是语法缺口（语义 / ABI / codegen 边界）

- **SG-B01 `static` 局部聚合初始化列表**：`static S s = {1};` 能解析，但 sema 报 `static local variable 's' must be initialized with a constant expression`。需要聚合常量折叠，不是 parser 缺口。关联 PAR-13/AGG。
- **SG-B02 成员 `operator[]` 写穿**：`v[0] = 1;` 报 `no matching operator[] for types 'class V' and 'int32'`；当前成员 `operator[]` 路径只提交值，未形成可写左值。关联 SEM-17、CG。
- **SG-B03 类方法内裸成员名**：方法体内 `return x;` 仍需写 `this->x`；这是名称查找/语言设计现状，不是语法解析。
- **SG-B04 `-fsyntax-only` 只验证解析**：未知注解（如 `[[nodiscard]]`）可能 parse 通过、完整编译报 E2010；不能用 `-fsyntax-only` 证明特性可用。
- **SG-B05 模块类型可见性**：非导出模块类型名仍可能被导入者看到；属 MOD 可见性，不是语法。
- **SG-B06 聚合按值 ABI**：struct/class/union 的 byval/sret、对齐等由 MEM-14/TYP-27 跟踪，不是语法。

### 24.5 明确不做（不计入缺口）

按 §21 Non-goals，下列不应作为“语法缺口”排期：

- `goto` / 标签（NG-01）
- 预处理器与宏（NG-02）
- VLA（TYP-11 用户裁决；动态长度由 Slice 承接）
- 模板特化/偏特化、SFINAE、可变参数模板、模板模板参数、concepts、默认模板实参（GEN-10）
- 多继承/虚继承/虚函数/vtable/RTTI（INH-07、NG-09）
- 构造/析构/GC（ACC-02、NG-09）
- `typedef` / `type`（PAR-24 已删除）
- 异常与 `?` 传播（NG-05）
- 命名参数（NG-13）

### 24.6 建议顺序与验收

1. **P1 语法收口（已完成 2026-10-31）**：SG-01 模板常量实参、SG-02 块内 extern 函数、SG-04 cast 完整 type-id、SG-03 `offsetof` 常量下标。
2. **P2 C 兼容（已完成 2026-10-31）**：SG-07 相邻字符串、SG-05 深层声明符、SG-06 数组形参限定符。
3. **先裁决再实现**：SG-08 namespace using、SG-09 lambda 扩展、SG-10 注解扩展、SG-11 class 扩展。
4. **独立排期**：§24.4 的 sema/codegen/ABI 边界，不与 parser 修复混做。

每项落地时：先补 RED 测试（至少 parser + e2e 各一条），再改实现；全量 `ctest -j8`；同步 `grammar.ebnf` 与本文件状态；涉及公开语法时更新 `examples/all_features.c` 或新增示例节。

> 第二轮补充项（SG-12 ~ SG-23）的优先级与并入顺序见 **§25.5**。

### 24.7 审计证据

- 审计基线：全量 **1572/1572** 通过（2 个既有 `GTEST_SKIP`）；`examples/all_features.c` → `ALL FEATURES OK`，`examples/full_feature.c` → `FULL FEATURE OK`。
- 每个 `[gap]` 例子均已用 `build/bin/my_llvm_c -fsyntax-only` 复现解析失败；涉及 sema/codegen 的 `[boundary]` 例子额外做了完整编译。
- 本文档只记录现状与建议，不改动代码；新增语法前仍需按 §21 的 DEC/Non-goal 规则裁决。
- 第二轮补充审计（SG-12 ~ SG-23、SG-B07 ~ SG-B09）见 **§25**。

---

## 25. 语法缺口补充审计（2026-10-27，第二轮）

> 范围：§24 之后的补漏，只列**语法/解析层**尚未覆盖、或规范已承诺但只实现简化形式的能力；语义/ABI/codegen 边界见 §25.4。
> 方法：对照 `docs/spec/grammar.ebnf`、§24，并用 `build/bin/my_llvm_c -fsyntax-only` 与完整编译逐例复核；每个例子都给出当前失败形态。
> 基线：沿用 §24 的 **1572/1572**（2 个既有 `GTEST_SKIP`）与两个示例 OK；本轮只改文档，未改代码。
> 结论：§24 的 11 项之外，仍有一批“规范承诺未贯通、C 声明符/字面量兼容、规范未覆盖扩展”的缺口。真正需要优先补的 **SG-12 数组常量维度**、**SG-14 显式转换完整 type-id** 已于 2026-10-27 修复；其余按 §25.5 推进。

### 25.1 规范已承诺、实现未完全贯通（优先）

#### SG-12 数组维度只接受整数字面量 `[fixed][P1]`

```c
int32 g[1 + 1];                         // 顶层：expected ';' after variable declaration
int32 main() {
    int32 a[1 + 1];                     // 局部：解析失败
    enum { N = 3 };
    int32 b[N];                         // 失败：unknown type 'N'
}
struct S { int32 a[1 + 1]; };           // 成员：expected ';' after member declaration
int32 f(int32 a[1 + 1]);                // 形参：expected ']' after array parameter size
```

- 当前：`parseVariableDecl`、`parseMemberArraySuffix`、`parseParamDecl` 与 `parseTypeId` 的数组分支都直接吃 `TOKEN_NUMBER`；非数字会落入 VLA/维度错误路径并导致后续恢复失败。
- 规范：`variable-decl` 与 `member-declarator` 的维度写的是 `const-expr`，不是 `integer-literal`。
- 目标：只对**编译期常量**求值（运行时长度 VLA 仍是 Non-goal，TYP-11 用户裁决不变）；解析期保存维度表达式，sema 求值并校验正数/溢出。
- 补充：`sizeof(int32[1 + 1])`、`using A = int32[1 + 1];` 也失败；`type-id` 文法当前写的是 `integer-literal`，若要 C 兼容可一并扩展，但不算本项的规范承诺。
- 关联：PAR-05、PAR-10、TYP-11、SG-03。
- **已修复（2026-10-27）**：新增解析期常量表（enum / `const`/`constexpr` 整型变量）与 `parseArrayDimension()`；变量、成员、形参、`type-id` 的数组维度现在接受字面量表达式/枚举/const 整型。新增 `SyntaxGapsE2E.ArrayDimensionsAcceptConstExpressions`、`TypeIdArrayDimensionsAcceptConstExpressions`；全量 **1577/1577** 通过（2 个既有 `GTEST_SKIP`）。VLA 仍是 Non-goal。

#### SG-13 位域宽度不能解析 enum/const 标识符 `[fixed][P2]`

```c
enum { N = 2 };
struct S { int32 a : N; };              // 失败：bit-field width must be a constant expression
const int32 M = 2;
struct T { int32 b : M; };              // 同样失败
struct U { int32 c : 1 + 1; };          // 通过：字面量表达式路径存在
```

- 当前：`parseBitFieldWidth()` 已支持字面量与一元/二元运算，但 `evalIntConstExpr()` 不查 enum 常量、`const`/`constexpr` 变量。
- 规范：`bit-field-decl = type identifier ':' const-expr ';'`。
- 目标：复用 sema 的常量求值器，或先把 enum/const 值注册进解析期常量表；不能用“只支持数字”替代 const-expr。
- 关联：AGG-06、PAR-10、TYP-25。
- **已修复（2026-10-27）**：`evalIntConstExpr()` 增加标识符回调，enum 值在解析时登记进常量表，`parseBitFieldWidth()` 使用同一查找；`int32 a : WBits`、`: VBits`、`: 1 + 1` 均通过。新增 `SyntaxGapsE2E.BitFieldWidthsAcceptEnumAndConst`；全量 **1577/1577** 通过。

#### SG-14 `static_cast` / `reinterpret_cast` 目标类型仍不走完整 type-id `[fixed][P1]`

```c
static_cast<int32 (*)(int32)>(p);       // 解析失败
reinterpret_cast<int32 (*)[2]>(p);      // 解析失败
```

- 当前：C 风格 cast 已由 SG-04 记录；`static_cast` / `reinterpret_cast` 的 `<...>` 分支仍只调 `parseType()`，没有 `parseFunctionPointerDeclarator` / 数组 type-id 路径。
- 目标：所有显式转换统一走完整 type-id；数组本身仍不能作为 cast 目标，函数指针、指向数组的指针应可用。
- 说明：这是 SG-04 的姊妹缺口，建议与 SG-04 同轮修，避免只修 C 风格而留下 C++ 风格。
- 关联：PAR-14、PAR-18、SG-04。
- **已修复（2026-10-27）**：`static_cast` / `reinterpret_cast` 的类型分支从 `parseType()` 改为 `parseTypeId()`；新增 `CastOperatorsE2E.StaticCastToFunctionPointerTypeId` 与 `ReinterpretCastToPointerToArrayTypeId`，全量 **1574/1574** 通过（2 个既有 `GTEST_SKIP`）。

### 25.2 C 兼容性 / 声明符与字面量

#### SG-15 静态数据成员的类外定义 `[fixed][P2]`

```c
struct S { static int32 x; };
int32 S::x = 1;                         // 失败：unknown type 'x'
```

- 已支持：类内 `static int32 x = 1;`、`S::x` 读取、静态成员函数声明。
- 当前：类外定义把 `S::x` 当成未知类型/变量；解析器只对 `Class::method(` 做了 out-of-class 方法分支，没有 `Class::data` 分支。
- 目标：接受 `type Class::name [= init];`，按静态数据成员绑定；与 AGG-10、MOD-08 一起确认符号/链接语义。
- 关联：AGG-10、MOD-08、SG-B07。
- **已修复（2026-10-28）**：parser 增加 `Class::name` 数据定义分支；sema 用 `staticDataDecls` 把类外初始化器挂到类内静态成员，避免重复全局。新增 `SyntaxGapsE2E.OutOfClassStaticDataDefinition`；全量 **1583/1583** 通过（2 个既有 `GTEST_SKIP`）。

#### SG-16 括号声明符与多函数声明符 `[fixed][P2]`

```c
int32 (x);                              // 失败：unknown type 'x'
int32 (f)(void);                        // 失败：unknown type 'f'
int32 (a)[3];                           // 失败：unknown type 'a'
int32 f(), g();                         // 失败：unknown type 'g'
```

- 已支持：`int32 (*p);`、`int32 (*p)[3];`、`int32 (*f)(void);` 等**指针引导**的括号声明符。
- 当前：声明符递归仍以 `*` 为入口；没有“无指针的括号声明符”与“一条声明多个函数声明符”。
- 目标：用统一 declarator parser（内层声明符 + 后缀反向应用）覆盖 `(name)`、`(name)[N]`、`(name)(params)`；再决定 `f(), g();` 是否纳入 C 兼容。
- 说明：`int32 (*p)` 等已通过，不能把本条误判成 SG-05 的深层函数指针回归。
- 关联：PAR-05、SG-05。
- **已修复（2026-10-27）**：`parseFunctionPointerDeclarator` 允许省略 `*`，`int32 (x);`、`int32 (f)(void);`、`int32 (a)[3];` 已可用（新增 `SyntaxGapsE2E.ParenthesizedDeclarators`）。`int32 f(), g();` 通过 `MultiVarDeclAST` 承载多个 `FunctionDeclAST`，sema/codegen 分派已补齐（新增 `SyntaxGapsE2E.MultipleFunctionDeclarators`）。全量 **1582/1582** 通过。

#### SG-17 函数类型形参未按 C 调整为指针 `[fixed][P2]`

```c
int32 apply(int32 g(int32), int32 x);   // 失败
```

- C 语义：形参中的函数类型会退化为函数指针（`int32 (*g)(int32)`）。
- 当前：参数解析只接受 `parseType()` 后的可选函数指针声明符，纯函数类型 `int32(int32)` 在参数位无法继续。
- 目标：参数类型位接受抽象函数类型并按 C 规则 decay；与 `using F = int32(int32)` 已有的函数类型别名保持一致。
- 关联：TYP-17、PAR-05、MEM-10。
- **已修复（2026-10-27）**：`parseParamDecl()` 在参数名后遇 `(` 时用 `parseAbstractFunctionSuffix()` 解析函数类型，并包装为指针（C decay）。新增 `SyntaxGapsE2E.FunctionTypeParameterDecaysToPointer`；全量 **1580/1580** 通过。

#### SG-18 C 字面量兼容子集 `[fixed][P2]`

```c
float64 a = .5;            // 失败：前导小数点
int64 b = 1LL;             // 失败：LL/ULL 后缀
int32 c = L'a';            // 失败：宽字符前缀
string_view s = L"x";      // 失败：宽字符串前缀
string_view t = R"(x)";    // 失败：大写 raw 字符串
float64 h = 0x1p3;         // 失败：十六进制浮点
```

- 已支持：`1.`、`1e3`、`1.e3`、`u8"..."`、`r"..."`、`0b`/`0o`、数字分隔符。
- 说明：这不是语言自洽的阻塞项；若目标是吸收更多 C/C++ 源码，再按 LEX-04/05/06 补子集。相邻字符串拼接已单列 SG-07。
- 关联：LEX-04、LEX-05、LEX-06、SG-07。
- **已修复（2026-10-28）**：lexer 支持前导小数点 `.5`、`LL`/`ULL` 后缀、`L'a'`/`u'a'`/`U'a'`/`u8'a'` 字符前缀、`L"..."`/`u"..."`/`U"..."` 字符串前缀、`R"(...)"` / `R"delim(...)delim"` 原始字符串、十六进制浮点 `0x1p3` / `0x1.8p1` / `0x.8p1`。`L` 字符串映射到 32 位 code unit（本语言无独立 `wchar_t`）。
- 新增 `SyntaxGapsE2E.LeadingDotFloatAndLongLongSuffix`、`CCharLiteralPrefixes`、`CStringLiteralPrefixesAndRaw`、`HexFloatLiteral`；均先 RED 后 GREEN。

#### SG-19 asm 连续冒号与命名操作数 `[fixed][P2]`

```c
asm("nop" ::: "memory");                  // 失败：`::` 被词法化为 COLON_COLON
asm("nop" : : : "memory");                // 通过：clobber 段本身已支持
asm("mov $1, %[v]" : [v] "=r"(x));        // 失败：命名操作数
```

- 已支持：`asm("...")`、`asm volatile(...)`、输出/输入/clobber 三段（空格分隔）、`"=r"(x)` 约束。
- 当前：clobber 段不是缺失，而是 `:::` 连续冒号在 lexer 层被切成 `COLON_COLON` + `COLON`，导致 parser 在模板后立即失败；`[name]` 命名操作数未实现。
- 目标：若要吸收 GCC/C++ 写法，补 lexer 对 `:::` 的上下文处理（或 parser 兼容 `COLON_COLON`），并定义命名操作数的绑定；labels 仍随 `goto` Non-goal 不做。
- 关联：PAR-16、CG-10、DEC-09。
- **已修复（2026-10-28）**：`parseAsmExpr()` 把 `COLON_COLON` 计作两个空段分隔符，`asm("nop" ::: "memory")` 已通过；`[name]` 命名操作数在 parser 中解析、sema 校验重名/未知名、codegen 按 outputs→inputs 顺序把模板 `%[name]` 翻译为 `$N`。新增 `SyntaxGapsE2E.AsmAdjacentEmptyOperandSections`、`NamedAsmOperand`；两者先 RED 后 GREEN。

### 25.3 规范未覆盖的现代/C++ 兼容扩展（需先裁决）

#### SG-20 模块语法扩展 `[ext][P2]`

```c
export module M;        // 失败：unknown type 'M'
module A:B;             // 失败：unknown type 'B'
module;                 // 失败：expected module name after 'module'
import M as N;          // 失败：unknown type 'as'
export import N;        // 失败：unknown type 'N'
```

- 已支持：`module M;`（文件名匹配）、`import M;`、`export { ... }`、`export namespace/struct/class/using/template`。
- 说明：`export module`、分区、global module fragment、import alias、`export import` 都不在 `grammar.ebnf` 的 `module-decl` / `export-decl` 中；需要先决定是否吸收 C++20 模块子集。
- 关联：MOD-04 ~ MOD-07、MOD-13。

#### SG-21 控制流扩展 `[ext][P2]`

```c
if (int32 x = f(); x) { }               // if-init
for (int32 x : a) { }                   // range-for
switch (x) { case 1 ... 3: break; }     // case range
defer { f(); }                          // defer 块
```

- 当前均解析失败；`defer expr;` 已实现，`if`/`for`/`switch` 的现有文法是 C 经典形式。
- 目标：逐项裁决。`defer` 块尤其要定义作用域与注册顺序；range-for 需要迭代协议，不能只补语法。
- 关联：BASE-04、FUN-10、SEM-18。

#### SG-22 struct/class 成员扩展 `[ext][P2]`

```c
struct S { int32 operator[](int32 i) { return this->a[i]; } };  // struct 路径解析失败
class C { public: int32 operator()(int32 x) { return x; } };    // 解析通过，调用 sema 无匹配
struct M { mutable int32 x; };                                   // 解析失败
struct D { int32 x = 1; };                                       // 默认成员初始化失败
```

- 当前：class 的成员运算符分支能解析 `operator[]`/`operator()`；struct 的成员循环没有对应分支，尽管 struct 方法本身可用。
- 另外：类成员 `operator[]`/`operator()` 的调用绑定不完整，读/写都可能报 `no matching operator...`（SG-B02 的写穿只是其中一例）。
- `mutable`、默认成员初始化不在 `class-member`/`field-decl` 文法中；若要吸收需先定义初始化发生在何时。
- 关联：PAR-04、SEM-17、AGG-08、SG-B02、SG-B08。

#### SG-23 模板类静态成员与类外模板成员定义 `[ext][P2]`

```c
template<typename T> struct S { static int32 x = 1; };
int32 main() { return S<int32>::x; }     // 解析失败

template<typename T> struct V { T x; T f(); };
template<typename T> T V<T>::f() { return this->x; }  // 解析失败
```

- 已支持：普通模板 class/function、`S<int32>` 实例、模板别名、`template<typename T> using`。
- 当前：`S<int32>::x` 这种模板实例限定名没有进入静态成员访问路径；类外模板成员定义只识别 `Class::method`，不识别 `V<T>::method`。
- 目标：若模板成员纳入 GEN-08，需要同时补限定名解析、实例化绑定与符号修饰（MOD-08/MOD-15）。
- 关联：GEN-08、PAR-21、MOD-08、MOD-15。

### 25.4 不是语法缺口（语义 / codegen 边界）

- **SG-B07 类外静态成员函数定义 `[fixed]`**：`struct S { static int32 f(); }; int32 S::f(){ return 1; }` 曾能解析但 sema 报 `method 'f' is not declared in class 'S'`。已让 sema 在 `staticMemberIndex` 中查找并绑定到脱糖后的全局符号，不再插入 `this`；新增 `SyntaxGapsE2E.OutOfClassStaticMethodDefinition`。关联 AGG-10、MOD-08、FUN-04。
- **SG-B08 成员 `operator[]` / `operator()` 调用绑定 `[fixed]`**：sema 先尝试自由函数，再按类方法表解析成员运算符，并以对象地址作为隐式 `this`；`c[0]` 读取与 `c(1)` 调用均已打通。新增 `SyntaxGapsE2E.MemberSubscriptOperatorRead`、`MemberCallOperator`。写穿仍受语言无引用返回类型限制（SG-B02），不属于本次调用绑定缺口。关联 SEM-17、CG。
- **SG-B09 全局 `string_view` 字面量 codegen 崩溃 `[fixed]`**：`string_view s = "abc"; int32 main(){return 0;}` 用 `-c` 编译曾段错误（`-fsyntax-only` 通过）；`"\x41"`、`"\u{41}"` 同样触发。根因：全局初始化时 IRBuilder 没有 insertion block，`CreateGlobalString` 未显式传 Module，解引用空 block。已改为传 `&ctx.getModule()`；新增 `SyntaxGapsE2E.GlobalStringViewLiteralDoesNotCrash`。关联 LEX-06、CG、MEM。

### 25.5 建议顺序与验收

1. **P1 规范收口（已完成）**：SG-12 数组常量维度、SG-14 显式转换完整 type-id、SG-01 模板常量实参、SG-02 块内 extern 函数、SG-04 cast 完整 type-id、SG-03 `offsetof` 常量下标均已修复（2026-10-31）。
2. **P2 声明符/常量贯通（已完成）**：SG-15 静态数据类外定义、SG-16 括号/多函数声明符、SG-17 函数类型形参、SG-13 位域 enum/const、SG-05 深层声明符、SG-06 数组形参限定符、SG-07 相邻字符串均已修复（2026-10-31）。
3. **P2 C 兼容可选（已完成）**：SG-18、SG-19、SG-07、SG-05、SG-06 已全部修复。
4. **先裁决再实现**：SG-20 模块扩展、SG-21 控制流扩展、SG-22 struct/class 成员扩展、SG-23 模板类成员。**2026-10-28 裁决：本轮暂不扩展，保留为后续产品决策项。**
5. **边界单列（已全部修复）**：SG-B07 类外静态方法、SG-B08 成员运算符调用、SG-B09 全局 `string_view` 字面量崩溃均已修复。

每项落地时：先补 RED 测试（至少 parser + e2e 各一条），再改实现；全量 `ctest --test-dir build -j8`；同步 `grammar.ebnf` 与 `TODO.md`；涉及公开语法时更新 `examples/all_features.c` 或新增示例节。

### 25.6 审计证据

- 基线与 §24 相同：全量 **1572/1572**（2 个既有 `GTEST_SKIP`）；`examples/all_features.c` → `ALL FEATURES OK`，`examples/full_feature.c` → `FULL FEATURE OK`。
- 所有 `[gap]` 例子均已用 `build/bin/my_llvm_c -fsyntax-only` 复现；SG-B07/SG-B08/SG-B09 额外用 `-c` 复现 sema/codegen 失败。
- 对照通过项：`int32 (*p)[3]`、`sizeof(int32(*)(int32))`、`struct U { int32 c : 1 + 1; }`、`extern "C"` 均完整编译通过；`class` 成员运算符至少解析通过（`operator+` 的类内按值返回仍受 incomplete 边界限制），避免把已支持能力误列为缺口。
- 本轮只记录现状与建议，不改代码；新增语法前仍按 §21 的 DEC/Non-goal 规则裁决。
