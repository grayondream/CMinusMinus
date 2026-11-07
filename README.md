# Cmm

**Cmm** — *C minus minus* — is a statically typed, C-like systems language
compiled with LLVM. It keeps C's predictable memory model and ABI, removes the
preprocessor and legacy numeric types, and adds modules, classes, templates,
`compile_time` evaluation, and a standard library written in Cmm itself.

Cmm source files use the **`.cmm`** suffix. The public standard-library
namespace is **`std`**; the compiler's internal C++ namespace is **`cmm`**.
Legacy `.smc` sources are not supported.

## Why Cmm

- **Predictable by construction** — explicit fixed-width types (`int8` …
  `int128`, `float16` … `float128`), a C-compatible memory model, and a stable
  ABI. No hidden allocations or implicit runtime.
- **No preprocessor** — modules and `compile_time` replace textual macros.
- **Modern structure** — `module` / `import`, namespaces, classes, inheritance,
  templates, lambdas, operator overloading, slices, `optional` / `result`.
- **Compile-time execution** — `constexpr`, `compile_time.if`,
  `compile_time.repeat`, reflection helpers, and container evaluation.
- **A real standard library** — `std.io`, `std.mem`, `std.string`, `std.vec`,
  `std.deque`, `std.hashmap`, `std.hashset`, `std.math`, `std.atomic`,
  `std.thread`, and more, implemented in `libs/std/*.cmm`.
- **LLVM backend** — native object files and linked executables, plus an
  in-process JIT used by the test suite.

## Quick Start

### Requirements

- Linux x86-64 (current primary target)
- LLVM **18 or newer** (`llvm-config` or `LLVM_DIR`)
- CMake **3.20+**
- A C++20 compiler
- Optional: `vcpkg` for dependency management

### Build

```bash
./scripts/build.sh
```

Or configure manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The compiler is written to `build/bin/cmm`.

### Hello, Cmm

Create `hello.cmm`:

```cmm
module hello;

int32 main() {
    println("Hello, Cmm!");
    return 0;
}
```

Compile and run:

```bash
build/bin/cmm hello.cmm -o hello
./hello
```

The module name must match the file name: `module hello;` lives in
`hello.cmm`.

## Modules and the Standard Library

The public namespace is `std`, and standard-library modules are imported by
dotted name:

```cmm
module vec_demo;
import std.vec;

int32 main() {
    std::Vec<int32> v = {};
    v.init();
    v.push(42);
    println("{}", v.get(0));
    v.destroy();
    return 0;
}
```

`std.io` exposes helpers such as `std::print_str` and `std::print_int`; the
built-in `println` / `print` family is always available.

## Repository Layout

| Path | Contents |
|---|---|
| `src/` | Compiler implementation: lexer, parser, AST, sema, codegen, driver |
| `libs/std/` | Cmm standard library (`.cmm`) |
| `examples/` | `all_features.cmm` — end-to-end feature tour |
| `tests/` | GoogleTest unit, integration, driver, and e2e tests |
| `docs/spec/` | Normative language specification |
| `docs/superpowers/` | Design and implementation records |
| `resources/` | Runtime resources and legacy C comparison samples |

## Testing

```bash
ctest --test-dir build -j8
```

Run the full feature tour:

```bash
build/bin/cmm examples/all_features.cmm -o /tmp/all_features
/tmp/all_features
```

The last line must be `ALL FEATURES OK`.

## Documentation

- [Language specification](docs/spec/README.md)
- [Grammar](docs/spec/grammar.ebnf)
- [Modules and namespaces](docs/spec/modules.md)
- [Semantics](docs/spec/semantics.md)
- [ABI and type layout](docs/spec/abi.md)
- [Compile-time and reflection](docs/spec/compile_time.md)
- [Standard library](docs/spec/stdlib.md)
- [TODO / roadmap](TODO.md) and [progress log](Progress.md)

## Status and Limitations

Cmm is an actively developed compiler. The normative spec marks features as
`[impl]`, `[plan]`, or `[ng]` (non-goal). Current limitations include
capturing-closure escape, some template-body lambda cases, and platform
support beyond Linux x86-64; see `TODO.md` for the up-to-date list.

## Naming

- Language: **Cmm** (pronounced “C minus minus”)
- Source suffix: **`.cmm`**
- Standard-library namespace/modules: **`std`**
- Compiler binary: **`cmm`**
- Legacy `.smc` input is rejected with a migration hint: rename the file to
  `.cmm`.

## 中文简介

**Cmm（C minus minus）** 是一门基于 LLVM 的静态类型、C 风格系统编程语言，
强调可预测的 C 内存模型与 ABI、定宽数值类型、无预处理器、模块、类与模板、
`compile_time` 编译期求值，以及用 Cmm 自身编写的标准库。

源文件后缀为 **`.cmm`**；标准库的公开命名空间/模块保持 **`std`**
（例如 `import std.io;`、`std::Vec<int32>`）；编译器内部 C++ 命名空间为
`cmm`。旧 `.smc` 源文件不再兼容。

```bash
./scripts/build.sh
build/bin/cmm hello.cmm -o hello
./hello
```

完整语言规范见 `docs/spec/`，当前实现进度与已知缺口见 `TODO.md`。
