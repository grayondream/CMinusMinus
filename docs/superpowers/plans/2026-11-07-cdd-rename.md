# CDD Hard Rename Implementation Plan

> 注：本文档记录 2026-11-07 的历史身份改名；2026-11-12 的 CDD 统一改名已将旧身份/旧后缀机械归一化。日期与当时旧值以 git 历史为准。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the language and toolchain from SafeModern C / SMC / My_LLVM_C to **CDD**, with `.cdd` sources and a formal root README, without changing language semantics or the public `std` namespace.

**Architecture:** Four sequential, independently testable commits: build identity first, then the `.cdd` source/module-loader migration, then internal C++ namespace/symbol normalization, then documentation and README. Each task leaves a buildable, fully tested tree.

**Tech Stack:** C++20, LLVM ≥ 18, CMake, GoogleTest, CDD standard library (`libs/std`).

**Spec:** `docs/superpowers/specs/2026-11-07-cdd-rename-design.md`

## Global Constraints

- Language name is **CDD**; source suffix is **`.cdd`**.
- Public language namespace/modules remain **`std`** (`module std.vec;`, `import std.io;`). Never rename language `std` to `cdd` or `cdd::std`.
- Internal C++ namespace is **`cdd`**; internal prefixes are **`__cdd_*`** and **`cdd.*` helpers.
- No `.smc` compatibility: `.smc` command-line input and imports must produce a clear legacy-extension error.
- Do not modify `thirdparty/`, C++ `std::`, generated `build/`, `.git/`, or the workspace directory name.
- `.c` and extensionless compiler inputs may still parse as CDD, but all product examples, libraries, docs, and tests use `.cdd`.
- Every task ends with `ctest --test-dir build -j8` green (baseline 1796/1796; count may grow with new tests).
- Fixed-width types and `int32 main()` in test sources.
- Do not stage unrelated work; worktree is clean at start except ignored docs.

## Review Focus

- `.smc` input/import must fail with an actionable legacy-extension error, not a confusing dotted-module miss.
- Public `std` module/namespace must be untouched while internal `smc` symbols become `cdd`.
- Installed compiler must find stdlib at `share/cdd/libs`; dev build must still find `libs/`.
- Module file ↔ module name matching must keep working with `.cdd` (including filename/module mismatch diagnostics).
- Mechanical replacement must not touch `thirdparty/`, C++ `std::`, or absolute workspace paths.

---

### Task 1: CDD Build and Toolchain Identity

**Files:**
- Modify: `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`
- Modify: `scripts/build.sh`, `scripts/run.sh`, `vcpkg.json`
- Modify: `src/codegen/CodegenContext.cpp`, `src/driver/CompilerDriver.cpp`, `src/sema/CompileTimeEvaluator.cpp`
- Modify: `tests/driver/test_compiler_driver.cpp`, `tests/sema/test_compile_time_ergonomics.cpp`, `tests/e2e/test_const_constexpr.cpp`

**Interfaces:**
- Produces: CMake project `CDD`, library target `cdd_lib`, executable `cdd`, install path `<datadir>/cdd/libs`.
- Produces: CLI name/usage string `cdd`; `compile_time.build.compiler` returns `"cdd"`; LLVM module name `"cdd"`.
- Produces: env var `CDD_STD_DIR`; install macros `CDD_INSTALL_STD_DIR`, `CDD_INSTALL_MIMALLOC_DIR`.

- [ ] **Step 1: Write failing identity tests**

In `tests/driver/test_compiler_driver.cpp`, add:

```cpp
TEST_F(CompilerDriverTest, PrintHelpUsesCDD) {
    testing::internal::CaptureStdout();
    driver.printHelp();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("Usage: cdd"), std::string::npos);
}
```

Update `CompileTimeErgonomics.BuildQueries` in `tests/sema/test_compile_time_ergonomics.cpp` to expect `"cdd"`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='CompilerDriverTest.PrintHelpUsesCDD:CompileTimeErgonomics.BuildQueries'`
Expected: FAIL (old usage/name).

- [ ] **Step 3: Rename build/toolchain identity**

Apply exactly these identity changes:
- `project(My_LLVM_C ...)` → `project(CDD ...)`
- `add_library(my_llvm_c_lib ...)` → `add_library(cdd_lib ...)`
- every `my_llvm_c_lib` link/include/definition reference → `cdd_lib`
- `add_executable(my_llvm_c main.cpp)` → `add_executable(cdd main.cpp)`
- all `my_llvm_c` target references in `src/CMakeLists.txt` → `cdd`
- `install(TARGETS my_llvm_c ...)` → `install(TARGETS cdd ...)`
- install destination `${CMAKE_INSTALL_DATADIR}/smc/libs` → `${CMAKE_INSTALL_DATADIR}/cdd/libs`
- `SMC_INSTALL_STD_DIR` → `CDD_INSTALL_STD_DIR`
- `SMC_INSTALL_MIMALLOC_DIR` → `CDD_INSTALL_MIMALLOC_DIR`
- `SMC_STD_DIR` → `CDD_STD_DIR`
- executable-relative stdlib search paths `share/smc/libs`, `lib/smc/libs` → `share/cdd/libs`, `lib/cdd/libs`
- `cxxopts::Options options("my_llvm_c", "A C11 subset compiler built with LLVM")` → `cxxopts::Options options("cdd", "The CDD compiler (C dot dot)")`
- `cArgs.push_back("my_llvm_c")` → `cArgs.push_back("cdd")`
- usage text `Usage: my_llvm_c ...` → `Usage: cdd ...`
- `CompileTimeEvaluator` compiler string `"my_llvm_c"` → `"cdd"`
- `CodegenContext` LLVM module name `"my_llvm_c"` → `"cdd"`
- `scripts/build.sh` and `scripts/run.sh` executable path `build/bin/my_llvm_c` → `build/bin/cdd`
- `vcpkg.json`: `"name": "cdd"`, `"description": "The CDD compiler (C dot dot), built with LLVM"`
- `tests/CMakeLists.txt`: link `cdd_lib`
- every test `argv[0]` literal `"my_llvm_c"` → `"cdd"`

Leave `install(DIRECTORY libs/ ... FILES_MATCHING PATTERN "*.smc")` for Task 2.

- [ ] **Step 4: Reconfigure and run focused tests**

Run: `cmake -S . -B build && cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='CompilerDriverTest.*:CompileTimeErgonomics.BuildQueries'`
Expected: PASS; `build/bin/cdd` exists.

- [ ] **Step 5: Run full suite**

Run: `ctest --test-dir build -j8`
Expected: 100% tests passed.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/CMakeLists.txt tests/CMakeLists.txt scripts/build.sh scripts/run.sh vcpkg.json \
  src/codegen/CodegenContext.cpp src/driver/CompilerDriver.cpp src/sema/CompileTimeEvaluator.cpp \
  tests/driver/test_compiler_driver.cpp tests/sema/test_compile_time_ergonomics.cpp tests/e2e/test_const_constexpr.cpp
git commit -m "refactor(build): rename compiler identity to CDD"
```

---

### Task 2: `.cdd` Sources, Module Loader, and Legacy `.smc` Rejection

**Files:**
- Rename: `libs/std/*.smc` → `libs/std/*.cdd` (22 files)
- Rename: `examples/all_features.c` → `examples/all_features.cdd`
- Rename: `resources/main_min.c` → `resources/main_min.cdd`
- Rename: `tests/e2e/resources/test_const.c` → `tests/e2e/resources/test_const.cdd`
- Rename: `tests/resources/test_input/{simple,expressions,control_flow}.c` → `.cdd`
- Modify: `src/driver/ModuleLoader.cpp`, `src/driver/ModuleLoader.h`
- Modify: `src/driver/StdPrelude.cpp`, `src/driver/StdPrelude.h`
- Modify: `src/driver/CompilerDriver.cpp`
- Modify: `CMakeLists.txt`
- Modify: module-loader/driver/e2e/integration tests and fixtures
- Modify: `examples/all_features.cdd` header comments

**Interfaces:**
- Produces: `resolveImport("std.c", ...)` finds `<dir>/std/c.cdd`.
- Produces: `.smc` input returns 1 with stderr containing `legacy .smc source is not supported; rename it to .cdd`.
- Produces: `.smc` import appends an error containing `legacy .smc source is not supported; rename it to .cdd`.
- Preserves: `ModuleSearchPaths`, `resolveImport`, `preloadImports`, `processImports`, `loadModuleFile` signatures.

- [ ] **Step 1: Write failing `.cdd` and `.smc` tests**

In `tests/driver/test_module_loader.cpp`:
- update `ResolvesStdCFromLibsDir` to expect `c.cdd`;
- update dotted-path expectation to `std/c.cdd`;
- update all test temp files from `.smc` to `.cdd`;
- add `RejectsLegacySmcImport`:

```cpp
TEST_F(ModuleLoaderTest, RejectsLegacySmcImport) {
    const fs::path dir = fs::temp_directory_path() / "cdd_legacy_import_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto ast = smc::parseStdCPrelude(
        "import \"old.smc\";\nint32 main() { return 0; }\n",
        (dir / "main.cdd").string());
    ASSERT_NE(ast, nullptr);
    std::set<std::string> loaded;
    std::vector<std::string> errors;
    smc::processImports(*ast, dir.string(), {}, loaded, errors);
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors[0].find("legacy .smc source is not supported"), std::string::npos);
    fs::remove_all(dir);
}
```

In `tests/driver/test_compiler_driver.cpp`, add:

```cpp
TEST_F(CompilerDriverTest, RejectsLegacySmcInput) {
    const std::filesystem::path p =
        writeTempSource("legacy_input.smc", "int32 main() { return 0; }\n");
    const std::string path = p.string();
    const char* argv[] = {"cdd", "-fsyntax-only", path.c_str()};
    ASSERT_TRUE(driver.parseArguments(3, argv));
    testing::internal::CaptureStderr();
    EXPECT_EQ(driver.run(), 1);
    const std::string err = testing::internal::GetCapturedStderr();
    EXPECT_NE(err.find("legacy .smc source is not supported"), std::string::npos);
    std::filesystem::remove(p);
}
```

- [ ] **Step 2: Run focused tests to verify they fail**

Run: `cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='ModuleLoaderTest.*:CompilerDriverTest.RejectsLegacySmcInput'`
Expected: FAIL on `.cdd` resolution and missing legacy rejection.

- [ ] **Step 3: Rename source files and update module loader**

Run:

```bash
for f in libs/std/*.smc; do git mv "$f" "${f%.smc}.cdd"; done
git mv examples/all_features.c examples/all_features.cdd
git mv resources/main_min.c resources/main_min.cdd
git mv tests/e2e/resources/test_const.c tests/e2e/resources/test_const.cdd
git mv tests/resources/test_input/simple.c tests/resources/test_input/simple.cdd
git mv tests/resources/test_input/expressions.c tests/resources/test_input/expressions.cdd
git mv tests/resources/test_input/control_flow.c tests/resources/test_input/control_flow.cdd
```

In `src/driver/ModuleLoader.cpp`:
- replace every `.smc` extension with `.cdd` in candidate building, path detection, module-name stripping, and header comments;
- add a helper `bool isLegacySmcSpecifier(const std::string&)` (ends with `.smc`);
- in `preloadImports` and `processImports`, before resolving, if any specifier is legacy, append
  `"legacy .smc source is not supported; rename it to .cdd"` to `errors` and skip that specifier;
- make `resolveImport` return `""` for legacy `.smc` (no dotted-lookup fallback);
- `specifierToModuleName` strips `.cdd` (explicit constant, not a hard-coded length assumption).

In `src/driver/StdPrelude.cpp` and `.h`:
- `std/c.smc` → `std/c.cdd`; comments update.

In `src/driver/CompilerDriver.cpp`:
- at the start of `compileFile`, if `inputFile` ends with `.smc`, print
  `legacy .smc source is not supported; rename it to .cdd` and return 1;
- missing stdlib error `(expected <std>/c.smc)` → `(expected <std>/c.cdd)`.

In `CMakeLists.txt`: install pattern `"*.smc"` → `"*.cdd"`.

- [ ] **Step 4: Update tests, fixtures, and synthetic CDD filenames**

- Replace `.smc` temp paths/imports in tests with `.cdd`, except the two legacy-rejection tests above.
- Update `tests/e2e/test_const_constexpr.cpp` to read `test_const.cdd`.
- Update `tests/integration/test_end_to_end.cpp` to read `simple.cdd`, `expressions.cdd`, `control_flow.cdd`.
- Normalize CDD source labels passed to `Lexer`/`setSourceFile`/`runSource` in `tests/e2e`, `tests/integration`, and `tests/driver` from `.c` to `.cdd`:

```bash
git grep -l -E '"[^"]*\.c"' -- tests/e2e tests/integration tests/driver \
  | xargs perl -pi -e 's/("(?:[^"\\]|\\.)*)\.c"/$1.cdd"/g'
```

- Update the header comment in `examples/all_features.cdd`: command uses `build/bin/cdd`, path uses `all_features.cdd`.
- `resources/main.c` stays as legacy C (not CDD).

- [ ] **Step 5: Audit and run tests**

Run:

```bash
git grep -n '\.smc' -- src libs tests examples resources CMakeLists.txt \
  | grep -v 'legacy .smc source is not supported' || true
```

Expected: only the intentional legacy-rejection diagnostic/tests remain.

Run: `cmake -S . -B build && cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='ModuleLoaderTest.*:CompilerDriverTest.*:ConstConstexprE2E.*:EndToEndTest.*'`
Expected: PASS.

Run: `ctest --test-dir build -j8`
Expected: 100% tests passed.

- [ ] **Step 6: Commit**

```bash
git add -A libs/std examples resources tests src/driver CMakeLists.txt
git commit -m "refactor(lang): move CDD sources to .cdd and reject legacy .smc"
```

---

### Task 3: Internal `cdd` Namespace and Symbols

**Files:**
- Modify: all `src/**/*.cpp`, `src/**/*.h` containing `smc`, `smc_utf8`, `__smc_`, `SMC_`, or `smc.*` helper names
- Modify: `libs/std/atomic.cdd`, other `libs/std/*.cdd` if they contain internal prefixes
- Modify: all `tests/**/*.cpp` containing old internal names
- Test: focused `ModuleLoaderTest.*`, `CompilerDriverTest.*`, `StringE2E.*`, `AtomicE2E.*`, `CompileTimeErgonomics.BuildQueries`

**Interfaces:**
- Produces: C++ namespace `cdd`; callers use `cdd::resolveImport`, `cdd::loadStdCPrelude`, `cdd_utf8::valid`.
- Produces: internal builtins `__cdd_atomic_*`, `__cdd_ct_*`, `__cdd_static_*`.
- Produces: helper symbol names `cdd.utf8.*`, `cdd.format.dyn`, `cdd.str.*`, `cdd.string.copy`, `cdd.string.assign`.
- Produces: test env var `CDD_UPDATE_SNAPSHOTS`.

- [ ] **Step 1: Write failing namespace/symbol expectations**

Update tests that assert internal names:
- `tests/e2e/test_string.cpp`: expect `cdd.utf8.validate`.
- `tests/e2e/test_atomic.cpp`: change all `__smc_atomic_*` calls to `__cdd_atomic_*`.
- Any test asserting `smc.string.*`/`smc.utf8.*` IR names: change expectation to `cdd.*`.
- `tests/sema/test_diagnostic_snapshot.cpp`: env `CDD_UPDATE_SNAPSHOTS`.

Run: `cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='StringE2E.*:AtomicE2E.*:CompileTimeErgonomics.BuildQueries'`
Expected: FAIL with old symbol names.

- [ ] **Step 2: Rename internal namespaces and symbols**

Apply in this order:
1. `smc_utf8` → `cdd_utf8`.
2. `__smc_` → `__cdd_`.
3. `smc.utf8` / `smc.format` / `smc.str` / `smc.string` / `smc.` internal helper prefixes → `cdd.` equivalent.
4. `namespace smc` → `namespace cdd`; `smc::` → `cdd::`.
5. `SMC_UPDATE_SNAPSHOTS` → `CDD_UPDATE_SNAPSHOTS`.
6. Remaining standalone `smc` temp paths/test class names → `cdd` / `CDD`.

Do not touch language-level `std`, C++ `std::`, `thirdparty/`, or `.smc` rejection diagnostics.

- [ ] **Step 3: Audit remaining old internal names**

Run:

```bash
git grep -n -E '\bsmc\b|smc::|__smc_|SMC_|smc\.utf8|smc\.format|smc\.str|smc\.string' \
  -- src libs tests examples || true
```

Expected: only intentional legacy-rejection text (none should contain lowercase `smc` except `.smc`), or no output.

- [ ] **Step 4: Run focused and full tests**

Run: `cmake -S . -B build && cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='ModuleLoaderTest.*:CompilerDriverTest.*:StringE2E.*:AtomicE2E.*:CompileTimeErgonomics.BuildQueries'`
Expected: PASS.

Run: `ctest --test-dir build -j8`
Expected: 100% tests passed.

- [ ] **Step 5: Commit**

```bash
git add -A src libs tests
git commit -m "refactor: rename internal smc namespace and symbols to cdd"
```

---

### Task 4: Documentation, README, and Final Acceptance

**Files:**
- Create: `README.md`
- Modify: `docs/spec/README.md`, `docs/spec/abi.md`, `docs/spec/compile_time.md`, `docs/spec/conversions.md`, `docs/spec/fixed-width-types.md`, `docs/spec/grammar.ebnf`, `docs/spec/keywords.md`, `docs/spec/modules.md`, `docs/spec/semantics.md`, `docs/spec/stdlib.md`
- Modify: `TODO.md`, `Progress.md`
- Modify: tracked historical docs under `docs/superpowers/**`
- Add: `docs/superpowers/specs/2026-11-07-cdd-rename-design.md`
- Add: `docs/superpowers/plans/2026-11-07-cdd-rename.md`
- Test: `tests/spec/test_spec_conformance.cpp`, full `ctest`

**Interfaces:**
- Produces: formal bilingual README with working `build/bin/cdd` commands.
- Produces: current spec titled `CDD 1.0` and describing `.cdd`.
- Produces: TODO/Progress entry for the CDD rename.

- [ ] **Step 1: Normalize documentation text**

Use targeted replacements (never `thirdparty/`, never C++ `std::`):
- `SafeModern C` → `CDD`
- `SMC` / `SMC_UPDATE_SNAPSHOTS` → `CDD` in prose, `CDD_UPDATE_SNAPSHOTS` in commands
- `my_llvm_c` → `cdd`
- `My_LLVM_C` → `CDD` in prose/project names; keep the absolute workspace path `/media/nvm1/workspace/My_LLVM_C` unchanged
- `.smc` → `.cdd` in current docs and historical docs, except the legacy-rejection diagnostic and this design/plan record
- `share/smc` / `lib/smc` → `share/cdd` / `lib/cdd`
- `smc.utf8.*` / `smc.format.dyn` / `smc.str.*` / `smc.string.*` → `cdd.*`
- generic tooling text `smc test` → `cdd test`
- fenced code blocks ````smc```` → ````cdd````

Update `docs/spec/README.md` title to `# CDD 1.0 语言规范（Spec）`; update each spec title from `SafeModern C 1.0` to `CDD 1.0`.

Add a `Progress.md` section `## 2026-11-07 — CDD 硬改名` with scope, commits, and final test count. Add a matching progress snapshot/entry at the top of `TODO.md`.

- [ ] **Step 2: Write the formal root README**

Create `README.md` with this structure (English main body + 中文简介):
1. `# CDD` / tagline: C dot dot, a statically typed C-like systems language compiled with LLVM.
2. Why CDD: predictable C memory model/ABI, fixed-width types, no preprocessor, modules, classes/templates, `compile_time`, first-class standard library.
3. Quick start: dependencies (`LLVM >= 18`, CMake, C++20 compiler, optional vcpkg), `./scripts/build.sh`, `build/bin/cdd`.
4. Hello world in `hello.cdd`; `build/bin/cdd hello.cdd -o hello`; `./hello`.
5. Modules/stdlib example: `import std.io;`, `std.vec`, public namespace stays `std`.
6. Repository layout: `src/`, `libs/std/`, `tests/`, `docs/spec/`, `examples/all_features.cdd`.
7. Testing: `ctest --test-dir build -j8`; full feature example command and `ALL FEATURES OK`.
8. Documentation index and project status/limitations.
9. Naming note: `.cdd` canonical; legacy `.smc` rejected; CDD = C dot dot.
10. 中文简介 section with the same essentials.

Every command in the README must be copy-paste runnable from the repo root.

- [ ] **Step 3: Run spec/full tests**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -j8`
Expected: 100% tests passed.

Run: `rm -rf /tmp/cdd-install && cmake --install build --prefix /tmp/cdd-install`
Expected: `/tmp/cdd-install/bin/cdd` and `/tmp/cdd-install/share/cdd/libs/std/c.cdd` exist.

- [ ] **Step 4: Run end-to-end acceptance**

Run:

```bash
build/bin/cdd examples/all_features.cdd -o /tmp/all_features_cdd && /tmp/all_features_cdd
```

Expected: last line `ALL FEATURES OK`, exit 0.

Run the acceptance grep:

```bash
git grep -n -E 'SafeModern|my_llvm_c|namespace smc|smc::|__smc_|SMC_|\.smc|share/smc|lib/smc' -- . \
  ':!thirdparty' \
  ':!docs/superpowers/specs/2026-11-07-cdd-rename-design.md' \
  ':!docs/superpowers/plans/2026-11-07-cdd-rename.md'
```

Expected: only the intentional legacy `.smc` rejection diagnostic/tests, the README migration note, and historical static-member fixture identifiers such as `SMC_v` (test identifiers, not product names). Separately inspect `git grep -n 'My_LLVM_C'`; only the absolute workspace path may remain. Verify public modules are still `std`: `git grep -n 'module std\.' -- libs/std`.

- [ ] **Step 5: Commit**

```bash
git add README.md docs/spec TODO.md Progress.md
git add -f docs/superpowers/specs/2026-11-07-cdd-rename-design.md \
  docs/superpowers/plans/2026-11-07-cdd-rename.md
git add -u docs/superpowers
git commit -m "docs: rebrand language as CDD and add formal README"
```

---

## Self-Review Checklist

- [ ] Every spec section maps to Task 1–4.
- [ ] `.smc` rejection has both driver-input and module-import tests.
- [ ] `std` public modules/namespace are explicitly checked as unchanged.
- [ ] CMake target/binary/install path are all `cdd`; no stale `my_llvm_c_lib`.
- [ ] Internal `cdd::`, `__cdd_`, `cdd.*`, `CDD_*` names are consistent.
- [ ] README commands run from repo root and produce `ALL FEATURES OK`.
- [ ] Historical docs and TODO/Progress are normalized; design/plan record is exempt.
