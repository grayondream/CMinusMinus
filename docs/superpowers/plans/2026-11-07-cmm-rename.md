# Cmm Hard Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the language and toolchain from SafeModern C / SMC / My_LLVM_C to **Cmm**, with `.cmm` sources and a formal root README, without changing language semantics or the public `std` namespace.

**Architecture:** Four sequential, independently testable commits: build identity first, then the `.cmm` source/module-loader migration, then internal C++ namespace/symbol normalization, then documentation and README. Each task leaves a buildable, fully tested tree.

**Tech Stack:** C++20, LLVM ≥ 18, CMake, GoogleTest, Cmm standard library (`libs/std`).

**Spec:** `docs/superpowers/specs/2026-11-07-cmm-rename-design.md`

## Global Constraints

- Language name is **Cmm**; source suffix is **`.cmm`**.
- Public language namespace/modules remain **`std`** (`module std.vec;`, `import std.io;`). Never rename language `std` to `cmm` or `cmm::std`.
- Internal C++ namespace is **`cmm`**; internal prefixes are **`__cmm_*`** and **`cmm.*` helpers.
- No `.smc` compatibility: `.smc` command-line input and imports must produce a clear legacy-extension error.
- Do not modify `thirdparty/`, C++ `std::`, generated `build/`, `.git/`, or the workspace directory name.
- `.c` and extensionless compiler inputs may still parse as Cmm, but all product examples, libraries, docs, and tests use `.cmm`.
- Every task ends with `ctest --test-dir build -j8` green (baseline 1796/1796; count may grow with new tests).
- Fixed-width types and `int32 main()` in test sources.
- Do not stage unrelated work; worktree is clean at start except ignored docs.

## Review Focus

- `.smc` input/import must fail with an actionable legacy-extension error, not a confusing dotted-module miss.
- Public `std` module/namespace must be untouched while internal `smc` symbols become `cmm`.
- Installed compiler must find stdlib at `share/cmm/libs`; dev build must still find `libs/`.
- Module file ↔ module name matching must keep working with `.cmm` (including filename/module mismatch diagnostics).
- Mechanical replacement must not touch `thirdparty/`, C++ `std::`, or absolute workspace paths.

---

### Task 1: Cmm Build and Toolchain Identity

**Files:**
- Modify: `CMakeLists.txt`, `src/CMakeLists.txt`, `tests/CMakeLists.txt`
- Modify: `scripts/build.sh`, `scripts/run.sh`, `vcpkg.json`
- Modify: `src/codegen/CodegenContext.cpp`, `src/driver/CompilerDriver.cpp`, `src/sema/CompileTimeEvaluator.cpp`
- Modify: `tests/driver/test_compiler_driver.cpp`, `tests/sema/test_compile_time_ergonomics.cpp`, `tests/e2e/test_const_constexpr.cpp`

**Interfaces:**
- Produces: CMake project `Cmm`, library target `cmm_lib`, executable `cmm`, install path `<datadir>/cmm/libs`.
- Produces: CLI name/usage string `cmm`; `compile_time.build.compiler` returns `"cmm"`; LLVM module name `"cmm"`.
- Produces: env var `CMM_STD_DIR`; install macros `CMM_INSTALL_STD_DIR`, `CMM_INSTALL_MIMALLOC_DIR`.

- [ ] **Step 1: Write failing identity tests**

In `tests/driver/test_compiler_driver.cpp`, add:

```cpp
TEST_F(CompilerDriverTest, PrintHelpUsesCmm) {
    testing::internal::CaptureStdout();
    driver.printHelp();
    const std::string out = testing::internal::GetCapturedStdout();
    EXPECT_NE(out.find("Usage: cmm"), std::string::npos);
}
```

Update `CompileTimeErgonomics.BuildQueries` in `tests/sema/test_compile_time_ergonomics.cpp` to expect `"cmm"`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='CompilerDriverTest.PrintHelpUsesCmm:CompileTimeErgonomics.BuildQueries'`
Expected: FAIL (old usage/name).

- [ ] **Step 3: Rename build/toolchain identity**

Apply exactly these identity changes:
- `project(My_LLVM_C ...)` → `project(Cmm ...)`
- `add_library(my_llvm_c_lib ...)` → `add_library(cmm_lib ...)`
- every `my_llvm_c_lib` link/include/definition reference → `cmm_lib`
- `add_executable(my_llvm_c main.cpp)` → `add_executable(cmm main.cpp)`
- all `my_llvm_c` target references in `src/CMakeLists.txt` → `cmm`
- `install(TARGETS my_llvm_c ...)` → `install(TARGETS cmm ...)`
- install destination `${CMAKE_INSTALL_DATADIR}/smc/libs` → `${CMAKE_INSTALL_DATADIR}/cmm/libs`
- `SMC_INSTALL_STD_DIR` → `CMM_INSTALL_STD_DIR`
- `SMC_INSTALL_MIMALLOC_DIR` → `CMM_INSTALL_MIMALLOC_DIR`
- `SMC_STD_DIR` → `CMM_STD_DIR`
- executable-relative stdlib search paths `share/smc/libs`, `lib/smc/libs` → `share/cmm/libs`, `lib/cmm/libs`
- `cxxopts::Options options("my_llvm_c", "A C11 subset compiler built with LLVM")` → `cxxopts::Options options("cmm", "The Cmm compiler (C minus minus)")`
- `cArgs.push_back("my_llvm_c")` → `cArgs.push_back("cmm")`
- usage text `Usage: my_llvm_c ...` → `Usage: cmm ...`
- `CompileTimeEvaluator` compiler string `"my_llvm_c"` → `"cmm"`
- `CodegenContext` LLVM module name `"my_llvm_c"` → `"cmm"`
- `scripts/build.sh` and `scripts/run.sh` executable path `build/bin/my_llvm_c` → `build/bin/cmm`
- `vcpkg.json`: `"name": "cmm"`, `"description": "The Cmm compiler (C minus minus), built with LLVM"`
- `tests/CMakeLists.txt`: link `cmm_lib`
- every test `argv[0]` literal `"my_llvm_c"` → `"cmm"`

Leave `install(DIRECTORY libs/ ... FILES_MATCHING PATTERN "*.smc")` for Task 2.

- [ ] **Step 4: Reconfigure and run focused tests**

Run: `cmake -S . -B build && cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='CompilerDriverTest.*:CompileTimeErgonomics.BuildQueries'`
Expected: PASS; `build/bin/cmm` exists.

- [ ] **Step 5: Run full suite**

Run: `ctest --test-dir build -j8`
Expected: 100% tests passed.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/CMakeLists.txt tests/CMakeLists.txt scripts/build.sh scripts/run.sh vcpkg.json \
  src/codegen/CodegenContext.cpp src/driver/CompilerDriver.cpp src/sema/CompileTimeEvaluator.cpp \
  tests/driver/test_compiler_driver.cpp tests/sema/test_compile_time_ergonomics.cpp tests/e2e/test_const_constexpr.cpp
git commit -m "refactor(build): rename compiler identity to Cmm"
```

---

### Task 2: `.cmm` Sources, Module Loader, and Legacy `.smc` Rejection

**Files:**
- Rename: `libs/std/*.smc` → `libs/std/*.cmm` (22 files)
- Rename: `examples/all_features.c` → `examples/all_features.cmm`
- Rename: `resources/main_min.c` → `resources/main_min.cmm`
- Rename: `tests/e2e/resources/test_const.c` → `tests/e2e/resources/test_const.cmm`
- Rename: `tests/resources/test_input/{simple,expressions,control_flow}.c` → `.cmm`
- Modify: `src/driver/ModuleLoader.cpp`, `src/driver/ModuleLoader.h`
- Modify: `src/driver/StdPrelude.cpp`, `src/driver/StdPrelude.h`
- Modify: `src/driver/CompilerDriver.cpp`
- Modify: `CMakeLists.txt`
- Modify: module-loader/driver/e2e/integration tests and fixtures
- Modify: `examples/all_features.cmm` header comments

**Interfaces:**
- Produces: `resolveImport("std.c", ...)` finds `<dir>/std/c.cmm`.
- Produces: `.smc` input returns 1 with stderr containing `legacy .smc source is not supported; rename it to .cmm`.
- Produces: `.smc` import appends an error containing `legacy .smc source is not supported; rename it to .cmm`.
- Preserves: `ModuleSearchPaths`, `resolveImport`, `preloadImports`, `processImports`, `loadModuleFile` signatures.

- [ ] **Step 1: Write failing `.cmm` and `.smc` tests**

In `tests/driver/test_module_loader.cpp`:
- update `ResolvesStdCFromLibsDir` to expect `c.cmm`;
- update dotted-path expectation to `std/c.cmm`;
- update all test temp files from `.smc` to `.cmm`;
- add `RejectsLegacySmcImport`:

```cpp
TEST_F(ModuleLoaderTest, RejectsLegacySmcImport) {
    const fs::path dir = fs::temp_directory_path() / "cmm_legacy_import_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto ast = smc::parseStdCPrelude(
        "import \"old.smc\";\nint32 main() { return 0; }\n",
        (dir / "main.cmm").string());
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
    const char* argv[] = {"cmm", "-fsyntax-only", path.c_str()};
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
Expected: FAIL on `.cmm` resolution and missing legacy rejection.

- [ ] **Step 3: Rename source files and update module loader**

Run:

```bash
for f in libs/std/*.smc; do git mv "$f" "${f%.smc}.cmm"; done
git mv examples/all_features.c examples/all_features.cmm
git mv resources/main_min.c resources/main_min.cmm
git mv tests/e2e/resources/test_const.c tests/e2e/resources/test_const.cmm
git mv tests/resources/test_input/simple.c tests/resources/test_input/simple.cmm
git mv tests/resources/test_input/expressions.c tests/resources/test_input/expressions.cmm
git mv tests/resources/test_input/control_flow.c tests/resources/test_input/control_flow.cmm
```

In `src/driver/ModuleLoader.cpp`:
- replace every `.smc` extension with `.cmm` in candidate building, path detection, module-name stripping, and header comments;
- add a helper `bool isLegacySmcSpecifier(const std::string&)` (ends with `.smc`);
- in `preloadImports` and `processImports`, before resolving, if any specifier is legacy, append
  `"legacy .smc source is not supported; rename it to .cmm"` to `errors` and skip that specifier;
- make `resolveImport` return `""` for legacy `.smc` (no dotted-lookup fallback);
- `specifierToModuleName` strips `.cmm` (explicit constant, not a hard-coded length assumption).

In `src/driver/StdPrelude.cpp` and `.h`:
- `std/c.smc` → `std/c.cmm`; comments update.

In `src/driver/CompilerDriver.cpp`:
- at the start of `compileFile`, if `inputFile` ends with `.smc`, print
  `legacy .smc source is not supported; rename it to .cmm` and return 1;
- missing stdlib error `(expected <std>/c.smc)` → `(expected <std>/c.cmm)`.

In `CMakeLists.txt`: install pattern `"*.smc"` → `"*.cmm"`.

- [ ] **Step 4: Update tests, fixtures, and synthetic Cmm filenames**

- Replace `.smc` temp paths/imports in tests with `.cmm`, except the two legacy-rejection tests above.
- Update `tests/e2e/test_const_constexpr.cpp` to read `test_const.cmm`.
- Update `tests/integration/test_end_to_end.cpp` to read `simple.cmm`, `expressions.cmm`, `control_flow.cmm`.
- Normalize Cmm source labels passed to `Lexer`/`setSourceFile`/`runSource` in `tests/e2e`, `tests/integration`, and `tests/driver` from `.c` to `.cmm`:

```bash
git grep -l -E '"[^"]*\.c"' -- tests/e2e tests/integration tests/driver \
  | xargs perl -pi -e 's/("(?:[^"\\]|\\.)*)\.c"/$1.cmm"/g'
```

- Update the header comment in `examples/all_features.cmm`: command uses `build/bin/cmm`, path uses `all_features.cmm`.
- `resources/main.c` stays as legacy C (not Cmm).

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
git commit -m "refactor(lang): move Cmm sources to .cmm and reject legacy .smc"
```

---

### Task 3: Internal `cmm` Namespace and Symbols

**Files:**
- Modify: all `src/**/*.cpp`, `src/**/*.h` containing `smc`, `smc_utf8`, `__smc_`, `SMC_`, or `smc.*` helper names
- Modify: `libs/std/atomic.cmm`, other `libs/std/*.cmm` if they contain internal prefixes
- Modify: all `tests/**/*.cpp` containing old internal names
- Test: focused `ModuleLoaderTest.*`, `CompilerDriverTest.*`, `StringE2E.*`, `AtomicE2E.*`, `CompileTimeErgonomics.BuildQueries`

**Interfaces:**
- Produces: C++ namespace `cmm`; callers use `cmm::resolveImport`, `cmm::loadStdCPrelude`, `cmm_utf8::valid`.
- Produces: internal builtins `__cmm_atomic_*`, `__cmm_ct_*`, `__cmm_static_*`.
- Produces: helper symbol names `cmm.utf8.*`, `cmm.format.dyn`, `cmm.str.*`, `cmm.string.copy`, `cmm.string.assign`.
- Produces: test env var `CMM_UPDATE_SNAPSHOTS`.

- [ ] **Step 1: Write failing namespace/symbol expectations**

Update tests that assert internal names:
- `tests/e2e/test_string.cpp`: expect `cmm.utf8.validate`.
- `tests/e2e/test_atomic.cpp`: change all `__smc_atomic_*` calls to `__cmm_atomic_*`.
- Any test asserting `smc.string.*`/`smc.utf8.*` IR names: change expectation to `cmm.*`.
- `tests/sema/test_diagnostic_snapshot.cpp`: env `CMM_UPDATE_SNAPSHOTS`.

Run: `cmake --build build -j$(nproc) && build/bin/compiler_tests --gtest_filter='StringE2E.*:AtomicE2E.*:CompileTimeErgonomics.BuildQueries'`
Expected: FAIL with old symbol names.

- [ ] **Step 2: Rename internal namespaces and symbols**

Apply in this order:
1. `smc_utf8` → `cmm_utf8`.
2. `__smc_` → `__cmm_`.
3. `smc.utf8` / `smc.format` / `smc.str` / `smc.string` / `smc.` internal helper prefixes → `cmm.` equivalent.
4. `namespace smc` → `namespace cmm`; `smc::` → `cmm::`.
5. `SMC_UPDATE_SNAPSHOTS` → `CMM_UPDATE_SNAPSHOTS`.
6. Remaining standalone `smc` temp paths/test class names → `cmm` / `CMM`.

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
git commit -m "refactor: rename internal smc namespace and symbols to cmm"
```

---

### Task 4: Documentation, README, and Final Acceptance

**Files:**
- Create: `README.md`
- Modify: `docs/spec/README.md`, `docs/spec/abi.md`, `docs/spec/compile_time.md`, `docs/spec/conversions.md`, `docs/spec/fixed-width-types.md`, `docs/spec/grammar.ebnf`, `docs/spec/keywords.md`, `docs/spec/modules.md`, `docs/spec/semantics.md`, `docs/spec/stdlib.md`
- Modify: `TODO.md`, `Progress.md`
- Modify: tracked historical docs under `docs/superpowers/**`
- Add: `docs/superpowers/specs/2026-11-07-cmm-rename-design.md`
- Add: `docs/superpowers/plans/2026-11-07-cmm-rename.md`
- Test: `tests/spec/test_spec_conformance.cpp`, full `ctest`

**Interfaces:**
- Produces: formal bilingual README with working `build/bin/cmm` commands.
- Produces: current spec titled `Cmm 1.0` and describing `.cmm`.
- Produces: TODO/Progress entry for the Cmm rename.

- [ ] **Step 1: Normalize documentation text**

Use targeted replacements (never `thirdparty/`, never C++ `std::`):
- `SafeModern C` → `Cmm`
- `SMC` / `SMC_UPDATE_SNAPSHOTS` → `Cmm` in prose, `CMM_UPDATE_SNAPSHOTS` in commands
- `my_llvm_c` → `cmm`
- `My_LLVM_C` → `Cmm` in prose/project names; keep the absolute workspace path `/media/nvm1/workspace/My_LLVM_C` unchanged
- `.smc` → `.cmm` in current docs and historical docs, except the legacy-rejection diagnostic and this design/plan record
- `share/smc` / `lib/smc` → `share/cmm` / `lib/cmm`
- `smc.utf8.*` / `smc.format.dyn` / `smc.str.*` / `smc.string.*` → `cmm.*`
- generic tooling text `smc test` → `cmm test`
- fenced code blocks ````smc```` → ````cmm````

Update `docs/spec/README.md` title to `# Cmm 1.0 语言规范（Spec）`; update each spec title from `SafeModern C 1.0` to `Cmm 1.0`.

Add a `Progress.md` section `## 2026-11-07 — Cmm 硬改名` with scope, commits, and final test count. Add a matching progress snapshot/entry at the top of `TODO.md`.

- [ ] **Step 2: Write the formal root README**

Create `README.md` with this structure (English main body + 中文简介):
1. `# Cmm` / tagline: C minus minus, a statically typed C-like systems language compiled with LLVM.
2. Why Cmm: predictable C memory model/ABI, fixed-width types, no preprocessor, modules, classes/templates, `compile_time`, first-class standard library.
3. Quick start: dependencies (`LLVM >= 18`, CMake, C++20 compiler, optional vcpkg), `./scripts/build.sh`, `build/bin/cmm`.
4. Hello world in `hello.cmm`; `build/bin/cmm hello.cmm -o hello`; `./hello`.
5. Modules/stdlib example: `import std.io;`, `std.vec`, public namespace stays `std`.
6. Repository layout: `src/`, `libs/std/`, `tests/`, `docs/spec/`, `examples/all_features.cmm`.
7. Testing: `ctest --test-dir build -j8`; full feature example command and `ALL FEATURES OK`.
8. Documentation index and project status/limitations.
9. Naming note: `.cmm` canonical; legacy `.smc` rejected; Cmm = C minus minus.
10. 中文简介 section with the same essentials.

Every command in the README must be copy-paste runnable from the repo root.

- [ ] **Step 3: Run spec/full tests**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -j8`
Expected: 100% tests passed.

Run: `rm -rf /tmp/cmm-install && cmake --install build --prefix /tmp/cmm-install`
Expected: `/tmp/cmm-install/bin/cmm` and `/tmp/cmm-install/share/cmm/libs/std/c.cmm` exist.

- [ ] **Step 4: Run end-to-end acceptance**

Run:

```bash
build/bin/cmm examples/all_features.cmm -o /tmp/all_features_cmm && /tmp/all_features_cmm
```

Expected: last line `ALL FEATURES OK`, exit 0.

Run the acceptance grep:

```bash
git grep -n -E 'SafeModern|my_llvm_c|namespace smc|smc::|__smc_|SMC_|\.smc|share/smc|lib/smc' -- . \
  ':!thirdparty' \
  ':!docs/superpowers/specs/2026-11-07-cmm-rename-design.md' \
  ':!docs/superpowers/plans/2026-11-07-cmm-rename.md'
```

Expected: only the intentional legacy `.smc` rejection diagnostic/tests and the README migration note. Separately inspect `git grep -n 'My_LLVM_C'`; only the absolute workspace path may remain. Verify public modules are still `std`: `git grep -n 'module std\.' -- libs/std`.

- [ ] **Step 5: Commit**

```bash
git add README.md docs/spec TODO.md Progress.md
git add -f docs/superpowers/specs/2026-11-07-cmm-rename-design.md \
  docs/superpowers/plans/2026-11-07-cmm-rename.md
git add -u docs/superpowers
git commit -m "docs: rebrand language as Cmm and add formal README"
```

---

## Self-Review Checklist

- [ ] Every spec section maps to Task 1–4.
- [ ] `.smc` rejection has both driver-input and module-import tests.
- [ ] `std` public modules/namespace are explicitly checked as unchanged.
- [ ] CMake target/binary/install path are all `cmm`; no stale `my_llvm_c_lib`.
- [ ] Internal `cmm::`, `__cmm_`, `cmm.*`, `CMM_*` names are consistent.
- [ ] README commands run from repo root and produce `ALL FEATURES OK`.
- [ ] Historical docs and TODO/Progress are normalized; design/plan record is exempt.
