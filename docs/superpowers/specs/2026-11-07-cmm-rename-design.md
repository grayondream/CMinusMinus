# Cmm 硬改名设计（语言名 / 后缀 / 内部命名 / README）

- 日期：2026-11-07
- 状态：待用户批准
- 范围：产品身份、构建产物、内部 C++ 命名、源文件后缀、文档与测试；不改语言语义
- 结论：硬切换，不保留 `.smc` 或旧 `smc` 兼容别名

## 1. 意图

把当前以 **SafeModern C / SMC / My_LLVM_C / my_llvm_c** 出现的产品身份，统一为
**Cmm（C minus minus）**。对外语言名、源文件后缀、编译器命令、构建项目名和文档标题
全部使用 Cmm；仓库内部 C++ 命名空间与内部符号跟随改名为 `cmm`。

公开的**语言级命名空间与模块命名空间保持 `std` 不变**，例如：

```cmm
module std.vec;
import std.io;
namespace std { ... }
```

即：`std` 是对外标准库命名空间，不改成 `cmm::std` 或 `std.cmm`；`cmm` 只用于
编译器/运行时的内部 C++ 身份。

## 2. 命名映射

| 旧 | 新 | 说明 |
|---|---|---|
| SafeModern C / SMC | Cmm | 语言正式名；C minus minus |
| `My_LLVM_C` | `Cmm` | CMake 项目名、文档标题、构建身份 |
| `my_llvm_c` | `cmm` | 可执行文件、LLVM module 名、CLI usage、`compile_time.build.compiler` |
| `my_llvm_c_lib` | `cmm_lib` | C++ 库目标 |
| `namespace smc` / `smc::` | `namespace cmm` / `cmm::` | 编译器内部 C++ 命名空间 |
| `smc_utf8` | `cmm_utf8` | 内部 UTF-8 辅助命名空间 |
| `__smc_*` | `__cmm_*` | 内部 builtin / 合成符号（atomic、ct、static 等） |
| `smc.*` 内部 helper 名 | `cmm.*` | `cmm.utf8.*`、`cmm.format.dyn`、`cmm.str.*`、`cmm.string.*` |
| `SMC_*` 宏/环境变量 | `CMM_*` | `CMM_STD_DIR`、`CMM_UPDATE_SNAPSHOTS`、`CMM_INSTALL_*` |
| `share/smc/libs`、`lib/smc/libs` | `share/cmm/libs`、`lib/cmm/libs` | 安装/运行时搜索路径 |
| `.smc` | `.cmm` | Cmm 源文件后缀 |
| `std` | `std` | **不变**：语言模块/命名空间对外保持 std |

## 3. 文件与路径改名

### 3.1 Cmm 源文件

- `libs/std/*.smc` → `libs/std/*.cmm`（22 个）
- `examples/all_features.c` → `examples/all_features.cmm`
- `resources/main_min.c` → `resources/main_min.cmm`
- `tests/e2e/resources/test_const.c` → `tests/e2e/resources/test_const.cmm`
- `tests/resources/test_input/{simple,expressions,control_flow}.c` → `.cmm`

`resources/main.c` 是旧的 C 展示稿（含本语言不支持的 `typedef`/`unsigned long`），
不是可编译的 Cmm 源，保留 `.c` 并在 README 中标注为 legacy C comparison，避免把
非法 Cmm 文件伪装成 `.cmm`。

### 3.2 测试中的合成文件名

所有作为 Cmm 编译输入传给测试 harness 的合成文件名（如 `"test.c"`、
`"test_class_simple.c"`、`"util.smc"`）统一改为 `.cmm`。C++ 宿主源文件（`*.cpp`）
和第三方 C 源（`thirdparty/**/*.c`）不改。

### 3.3 构建与安装

- `project(My_LLVM_C)` → `project(Cmm)`
- `add_library(my_llvm_c_lib ...)` → `add_library(cmm_lib ...)`
- `add_executable(my_llvm_c ...)` → `add_executable(cmm ...)`
- 安装 target、`install(DIRECTORY libs/ ...)` 的 `*.smc` pattern → `*.cmm`
- 安装目录 `<datadir>/smc/libs` → `<datadir>/cmm/libs`
- `src/CMakeLists.txt` 的 `SMC_INSTALL_*` → `CMM_INSTALL_*`
- `scripts/build.sh` / `scripts/run.sh` 的 `build/bin/my_llvm_c` → `build/bin/cmm`
- `vcpkg.json` 的包名改为小写 `cmm`，描述改为 Cmm 编译器（vcpkg manifest
  的包名必须小写，不能用 `Cmm`）

### 3.4 模块加载器 / 驱动

- `ModuleLoader` 的显式路径判断、点分名追加、默认扩展、模块名剥离全部从
  `.smc` 改为 `.cmm`。
- `CompilerDriver` 的 `SMC_STD_DIR`、`share/smc/libs`、`lib/smc/libs` 改
  `CMM_STD_DIR`、`share/cmm/libs`、`lib/cmm/libs`。
- `StdPrelude` 的 `std/c.smc` 路径与缺失标准库错误信息改 `std/c.cmm`。
- 诊断/错误文本中出现的 `SafeModern C` 改 `Cmm`。

### 3.5 `.smc` 硬拒绝

不保留 `.smc` 兼容，但要给出明确行为而不是落到点分名解析：

- 命令行输入以 `.smc` 结尾时，驱动直接报错：
  `legacy .smc source is not supported; rename it to .cmm`。
- `import "x.smc"` / `import x.smc;` 进入模块加载器时同样报上述错误，
  不再尝试把 `x.smc` 当成点分模块名拼成 `x/smc.cmm`。
- `.c`、无扩展名等通用输入仍可被编译器当作 Cmm 源读取（扩展名不是语言语义），
  但 README / 示例 / 标准库一律使用 `.cmm`；只有旧 `.smc` 被显式拒绝。

## 4. 文档与 README

### 4.1 正式根 README

新增根 `README.md`，双语：英文主文 + 中文简介（可在批准时改成纯中文/纯英文）。
至少包含：

1. Cmm 名称与含义（C minus minus），与 C/C++ 的关系
2. 设计目标与非目标：可预测 C 内存模型/ABI、无隐藏分配、定宽类型、模块、
   class/template、`compile_time`、标准库
3. Quick start：依赖（LLVM ≥ 18、CMake、vcpkg 可选）、`./scripts/build.sh`、
   `build/bin/cmm`、hello world `.cmm`、编译运行命令
4. 模块与标准库示例：`module`/`import std.io`/`std.vec`
5. 仓库结构：`src/`、`libs/std/`、`tests/`、`docs/spec/`、`examples/`
6. 测试：`ctest --test-dir build -j8`、`examples/all_features.cmm` 验收
7. 文档索引：`docs/spec/README.md` 及各规范分册
8. 当前状态与已知限制（简短，指向 TODO）
9. 命名说明：源文件后缀 `.cmm`；旧 `.smc` 不兼容

### 4.2 现有文档

- `docs/spec/README.md` 与全部分册标题 `SafeModern C 1.0` → `Cmm 1.0`。
- 代码围栏 ````smc```` → ````cmm````。
- 全仓库当前文档、TODO/Progress、历史设计/计划中的产品名、二进制名、内部前缀、
  `.smc` 路径统一机械替换；历史文档顶部可保留“历史记录，名称已归一化”的说明。
- `SMC_UPDATE_SNAPSHOTS` 等开发者环境变量改 `CMM_UPDATE_SNAPSHOTS`。
- 只更新**已跟踪**的历史文档；未跟踪且被 `.gitignore` 排除的临时记录不进入提交。
  本次改名设计/计划本身作为“改名记录”允许在映射表中保留旧名，不作为验收 grep 的
  检查对象。

## 5. 非目标

- 不保留 `.smc` 兼容读取、双后缀、旧 `smc` 别名。
- 不把语言标准库命名空间 `std` 改成 `cmm`。
- 不改 C++ 标准库 `std::`，不改第三方 `thirdparty/mimalloc` 的源码。
- 不重命名工作区根目录（目录名不是产品 API）。
- 不改变任何语言语义、ABI 规则或标准库模块名（`std.vec` 等不变）。
- 不重写历史提交；只在当前工作树上做一次硬改名提交。

## 6. 验收

1. `cmake -S . -B build` / `cmake --build build -j$(nproc)` 成功，产物为
   `build/bin/cmm`。
2. `build/bin/cmm examples/all_features.cmm -o /tmp/all_features && /tmp/all_features`
   输出 `ALL FEATURES OK`。
3. `ctest --test-dir build -j8` 全绿（基线 1796/1796）。
4. 对**已跟踪产品文件**执行 `git grep`（排除 `thirdparty/`、本改名设计/计划记录）
   不再有：
   - `SafeModern`
   - `my_llvm_c` / `My_LLVM_C`（工作区绝对路径中的目录名除外）
   - `namespace smc` / `smc::`
   - `__smc_`
   - `SMC_`
   - `.smc`（仅允许出现在“旧 `.smc` 已不支持”的拒绝诊断/对应测试、README 的迁移说明，以及本改名设计/计划记录中）
   - 旧安装路径 `share/smc` / `lib/smc`
   验收不检查 `build/`、`.git/`、未跟踪的忽略文件，也不要求重命名工作区根目录。
5. 语言模块与命名空间仍为 `std`：`import std.io;`、`module std.vec;` 等不变。
6. 根 README 可按文档中的命令从零构建并运行 hello world。
7. `.smc` 输入/导入明确报错；`.cmm` 示例、标准库、测试全部可构建。

## 7. 风险与处理

- **机械替换误伤**：只替换完整 token/路径模式，不用裸 `smc` 全局替换；先审计
  `smc` 子串，区分 `smc::`、`smc_utf8`、`smc.*`、`.smc`、测试临时名。
- **CMake 缓存旧 target**：改名后重新 configure；旧 `build/bin/my_llvm_c` 不参与
  验收。
- **模块文件名匹配**：`.cmm` 与 `.smc` 同为 4 字符，剥离逻辑仍需改为显式
  `.cmm` 常量，避免长度巧合掩盖错误。
- **历史文档改写**：历史计划/进度中的旧名会被机械归一化，可能降低“历史原貌”；
  这是“全部修改”的代价，提交信息中注明。
- **测试快照/错误文案**：所有断言 `.smc`/`my_llvm_c`/`smc.` 的测试同步更新；
  全量测试兜底。
