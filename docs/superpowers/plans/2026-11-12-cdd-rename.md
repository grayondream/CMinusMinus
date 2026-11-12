# CDD Identity Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make CDD / `cdd` / `.cdd` the only canonical language identity across source, build, CLI, tests, stdlib, examples, and docs.

**Architecture:** Apply one case-consistent textual rename over tracked text files and `git mv` every old-suffix source file to `.cdd`; then rebuild from a fresh CMake configuration and let the existing 1888-test suite prove behavior is unchanged. Finish with a repository-wide absence check for the old identity.

**Tech Stack:** C++20, LLVM ≥ 18, CMake, GoogleTest/CTest, CDD standard library, Bash text tooling.

**Spec:** `docs/superpowers/specs/2026-11-12-cdd-identity-design.md`

## Global Constraints

- Display name **CDD**, meaning **C dot dot**, suffix **`.cdd`**, CLI **`cdd`**.
- CMake project **CDD**, library target **`cdd_lib`**, install data **`share/cdd/libs`**.
- C++ namespace **`cdd`**; internal helpers **`cdd_utf8`**, **`cdd.*`**, **`__cdd_*`**.
- Public language namespace stays **`std`**.
- No compatibility alias or second executable.
- Do not rename the workspace directory `My_LLVM_C`.
- Execute natively; no worktree and no per-task subagent implementation.
- Every task ends with a build/test check and a commit.

## Review Focus

- **Old-suffix source file left behind:** expected `find` to return no old-suffix files; otherwise module discovery and docs diverge.
- **Case variant left in text:** expected zero hits for old lowercase, capitalized, and uppercase identities across tracked text.
- **CMake/install identity split:** expected `project(CDD)`, `cdd_lib`, binary `cdd`, and install path `share/cdd/libs` to agree with the driver search path.
- **CLI/help/error identity mismatch:** expected help, version, legacy messages, and `compile_time.build.compiler` to all report CDD/cdd.
- **Internal prefix rename:** expected `__cdd_*`, `cdd.*`, and `cdd_utf8` to rename together so stdlib and sema/codegen still link in the JIT.

---

### Task 1: Rename file suffixes, identities, and build targets

**Files:**
- Rename every tracked file ending in the old source suffix to `.cdd`.
- Rename historical design/plan files whose names contain the old identity.
- Modify all tracked text files containing the old identity or suffix.
- Modify: `CMakeLists.txt`, `src/CMakeLists.txt`, `vcpkg.json`, `README.md`, `TODO.md`, `Progress.md`, `docs/**`, `libs/**`, `examples/**`, `resources/**`, `tests/**`, `src/**`.

**Interfaces:**
- Produces: source suffix `.cdd`; CLI/CMake identity `cdd`; project `CDD`; namespace `cdd`; module loader extension `.cdd`.
- Consumes: nothing from earlier tasks.

- [x] **Step 1: Snapshot the repository**

Run:

```bash
git status --short
git rev-parse HEAD
```

Expected: clean tree. Record the HEAD SHA as `BASE`.

- [x] **Step 2: Preflight the case map**

Run the prepared case map against tracked text files and tracked paths. The
map covers the pre-rename lowercase identity, capitalized display identity,
uppercase macro/guard identity, and source suffix. Review the list for
third-party code; do not touch `thirdparty/`.

- [x] **Step 3: Rename tracked files and paths**

Use `git mv` for every tracked file whose name contains the pre-rename identity
or suffix. Preserve extension order, including the feature tour, standard
library modules, test fixtures, and historical rename design/plan files. Do not
rename `thirdparty/` or the workspace directory.

- [x] **Step 4: Apply the case-consistent textual rename**

Apply one Perl pass over the tracked text files (exclude `.git`, `build`,
`thirdparty`, and binary files) with this map:

1. Camel-case helper `kCddExtension` before the general pass.
2. Lowercase identifier, namespace, binary, and suffix → `cdd` / `.cdd`.
3. Capitalized display identity → `CDD`.
4. Uppercase macro/guard identity → `CDD`.
5. Meaning text → `C dot dot`.

Do not replace inside `thirdparty/`. The prepared absence check returned no
source/doc hits.

- [x] **Step 5: Check CMake and driver identity**

Run:

```bash
grep -nE 'project\\(|cdd_lib|add_executable\\(cdd|share/cdd|cdd_lib|CDD_' CMakeLists.txt src/CMakeLists.txt
grep -nE 'cdd|CDD' src/driver/CompilerDriver.cpp src/driver/ModuleLoader.cpp src/driver/ModuleLoader.h | head -40
```

Expected: `project(CDD)`, `add_library(cdd_lib ...)`, `add_executable(cdd ...)`, `share/cdd/libs`, `cdd_lib` definitions, `kCddExtension = ".cdd"`, and CLI/help strings using `cdd`/`CDD`.

- [x] **Step 6: Commit**

```bash
git add -A
git commit -m "refactor: rename language identity to CDD"
```

---

### Task 2: Reconfigure, rebuild, and verify the renamed toolchain

**Files:**
- No new production files expected.
- Test: existing full CTest suite.

**Interfaces:**
- Consumes: Task 1 renamed targets and suffix.
- Produces: a clean `build/bin/cdd` and passing `build/bin/compiler_tests`.

- [x] **Step 1: Reconfigure after the target rename**

Run:

```bash
cmake -S . -B build
```

Expected: CMake configures project `CDD`, finds LLVM, and reports no missing `cdd_lib` target.

- [x] **Step 2: Build**

```bash
cmake --build build -j$(nproc)
```

Expected: `build/bin/cdd` and `build/bin/compiler_tests` exist; no old identity in target names.

- [x] **Step 3: Verify CLI and suffix**

```bash
build/bin/cdd --help | head -5
printf 'int32 main() { return 0; }\\n' > /tmp/opencode/cdd_smoke.cdd
build/bin/cdd /tmp/opencode/cdd_smoke.cdd -o /tmp/opencode/cdd_smoke
/tmp/opencode/cdd_smoke
```

Expected: help says `cdd`/CDD; compile and run exit 0.

- [x] **Step 4: Run the full suite**

```bash
ctest --test-dir build -j8 --output-on-failure
```

Expected: **1888/1888** pass (or the current full count if tests changed only by rename).

- [x] **Step 5: Run the feature tour**

```bash
build/bin/cdd examples/all_features.cdd -o /tmp/all_features
/tmp/all_features
```

Expected last line: `ALL FEATURES OK`.

- [x] **Step 6: Commit any build-driven fixes**

```bash
git add -A
git commit -m "build: verify CDD toolchain after identity rename"
```

If no file changed, skip this commit.

---

### Task 3: Documentation and absence verification

**Files:**
- Modify: `README.md`, `TODO.md`, `Progress.md`, `docs/spec/**`, `docs/superpowers/**` (already renamed in Task 1).
- Test: repository-wide absence checks and full suite.

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces: current docs and a recorded rename snapshot.

- [x] **Step 1: Update TODO and Progress**

Add a dated `2026-11-12` snapshot to `TODO.md` and a chronological entry to `Progress.md` stating:
- previous identity renamed to CDD / `cdd` / `.cdd`;
- CMake project/targets, namespace, internal prefixes, install path, CLI, and docs all renamed;
- full test count and feature-tour output;
- the workspace directory `My_LLVM_C` and public `std` namespace remain unchanged.

- [x] **Step 2: Verify README/spec identity**

```bash
grep -nE 'CDD|cdd|\\.cdd|C dot dot' README.md docs/spec/README.md docs/spec/keywords.md | head -40
```

Expected: titles, hello example, commands, repo-layout table, and naming note use CDD/cdd/`.cdd`; the pronunciation is “C dot dot”.

- [x] **Step 3: Repository-wide absence check**

```bash
OLD=$(printf 'c\x6d\x6d')
git grep -n -i "$OLD" -- ':!thirdparty' || true
git ls-files | grep -i "$OLD" || true
git ls-files "*.$OLD" || true
```

Expected: all three commands produce no output. (Build artifacts outside git may
still contain old strings; they are not tracked.)

- [x] **Step 4: Final full verification**

```bash
ctest --test-dir build -j8 --output-on-failure
build/bin/cdd examples/all_features.cdd -o /tmp/all_features
/tmp/all_features
git status --short
```

Expected: full suite green; last line `ALL FEATURES OK`; clean working tree.

- [x] **Step 5: Commit**

```bash
git add README.md TODO.md Progress.md docs/spec docs/superpowers
git commit -m "docs: record CDD identity rename"
```

---

## Self-Review Notes

- The rename is mechanical; the existing 1888-test suite is the regression
  gate. No semantic or ABI change is intended.
- The only intentionally retained names are the workspace path `My_LLVM_C` and
  the public `std` namespace.
- Historical docs are normalized like the previous rename did; no old identity
  is left in tracked text or paths.
