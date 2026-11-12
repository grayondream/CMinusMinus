# CDD Identity Rename Design

## Intent

Rename the language identity from its previous name to **CDD** and make the new
identity the only canonical one across the repository, build, CLI, standard
library, tests, examples, and documentation.

## Naming

- Display name: **CDD**
- Meaning/pronunciation: **C dot dot**
- Source suffix: **`.cdd`**
- Compiler executable: **`cdd`**
- CMake project: **CDD**
- CMake library target: **`cdd_lib`**
- Install data directory: **`share/cdd/libs`**
- C++ namespace: **`cdd`**
- Internal symbol/helper namespace: **`cdd`**, including `cdd_utf8`, `cdd.*`
  runtime helper names, and `__cdd_*` builtins
- Environment/CMake definitions: **`CDD_*`**
- Public language namespace: unchanged (`std`)

## Scope

- Rename all tracked source files using the old source suffix to `.cdd`.
- Rename all text references to the old language identity, case-consistently:
  display name to `CDD`, lowercase identifier/namespace/binary to `cdd`,
  uppercase macro/guard to `CDD`.
- Rename LLVM module/debug identity, `compile_time.build.compiler`, vcpkg
  manifest, install paths, test argv, fixtures, examples, specs, README, TODO,
  and Progress entries.
- Rename historical design/plan filenames that contain the old identity so the
  repository has no stale identity in paths.
- Set the meaning to “C dot dot”; the pre-rename meaning is retired.

## Non-goals

- No language semantics changes.
- No public `std` namespace changes.
- No repository directory rename (`My_LLVM_C` stays as the workspace path).
- No compatibility alias or second executable name.
- Direct compiler inputs with an arbitrary suffix remain readable because the
  suffix is not semantic; module discovery uses `.cdd`.

## Acceptance

- No text file tracked by git contains the old lowercase identity, old
  capitalized identity, old uppercase identity, or old source suffix.
- No tracked file name contains the old identity or old source suffix.
- `build/bin/cdd` builds; `build/bin/compiler_tests` passes in full.
- `build/bin/cdd examples/all_features.cdd` prints `ALL FEATURES OK`.
- `ctest --test-dir build -j8` is fully green.
