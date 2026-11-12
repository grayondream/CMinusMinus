# Function and Operator Overloading Design

## Overview

Add function overloading (multiple functions with the same name but different parameter types) and operator overloading (custom operators on structs/unions) to the CDD compiler.

**Key decisions**:
- Exact-match resolution (no implicit conversions)
- Simplified name mangling: `name_type1_type2`
- Operators: arithmetic (+, -, *, /) and comparison (==, !=, <, >, <=, >=)
- Operators only on struct/union types (not primitives)

## 1. Name Mangling

All overloaded functions get unique LLVM IR names via a simplified mangling scheme.

**Format**: `<name>_<type1>_<type2>...`

**Type name mapping**:
| C Type | Mangled |
|--------|---------|
| `int` | `int` |
| `float` | `float` |
| `double` | `double` |
| `char` | `char` |
| `void` | `void` |
| `struct X` | `X` |
| `union X` | `X` |
| `T*` | `Tptr` |
| `T[]` | `Tarr` |

**Examples**:
| Function | Mangled Name |
|----------|-------------|
| `int add(int, int)` | `add_int_int` |
| `float add(float, float)` | `add_float_float` |
| `int add(int, int, int)` | `add_int_int_int` |
| `void print(int)` | `print_int` |
| `void print(char*)` | `print_charptr` |
| `operator+(Point, Point)` | `operator+_Point_Point` |

**Implementation**: New utility function `mangleFunction(name, paramTypes)` in a shared location (likely `src/ast/TypeUtil.h` or `src/codegen/CodegenContext.h`).

## 2. Symbol Table (OverloadSet)

### Current State

`Scope::symbols` is `std::unordered_map<std::string, Symbol*>` — one symbol per name. Redeclaration of the same name triggers an error.

### New Design

Add an `OverloadSet` class:

```cpp
class OverloadSet {
    std::vector<Symbol*> candidates;
public:
    void add(Symbol* sym);
    Symbol* resolve(const std::vector<Type*>& argTypes) const;
    const std::vector<Symbol*>& getCandidates() const;
    size_t size() const;
};
```

**Scope changes**:
- `symbols` becomes `std::unordered_map<std::string, OverloadSet>`
- `declare(name, symbol)` adds to the OverloadSet. Reject only if the exact same signature (name + all param types) already exists.
- `lookup(name)` returns the OverloadSet (or nullptr/empty if not found)

**Resolution**: `resolve(argTypes)` iterates candidates and finds one where `paramTypes[i] == argTypes[i]` for all parameters (exact type match, no implicit conversions). Returns nullptr if no match or if ambiguous (multiple candidates match).

**Type equality**: Two types are equal if they are the same `TypeKind` with the same parameters. `int` != `float`, `int*` != `int`, `struct Point` != `struct Line`.

**Ambiguity detection**: If `resolve()` finds more than one matching candidate, the call is ambiguous and an error is raised.

## 3. Function Overloading

### Parser

No syntax changes needed. Multiple declarations with the same name but different parameter types are naturally parsed as separate `FunctionDeclAST` nodes. The parser already handles this.

### Semantic Analyzer

- `visit(FunctionDeclAST&)`: Instead of rejecting redeclarations, call `currentScope->declare(name, symbol)` which adds to the OverloadSet. Error only if the exact same signature exists.
- `checkFunctionCall(name, args)`: Use `overloadSet.resolve(argTypes)` to find the matching candidate. If none found, error: "no matching function for overload". If multiple match, error: "ambiguous overload".

### Codegen

- `FunctionDeclAST::codegen()`: Use `mangleFunction(name, paramTypes)` as the LLVM function name instead of raw `name`.
- `CallExprAST::codegen()`: Look up the mangled name in the LLVM module.

### Backward Compatibility

Non-overloaded functions still work — they have a single-entry OverloadSet and their name is mangled to `name_type1_type2`. Existing code continues to compile and run.

## 4. Operator Overloading

### Supported Operators

| Category | Operators |
|----------|----------|
| Arithmetic | `+`, `-`, `*`, `/` |
| Comparison | `==`, `!=`, `<`, `>`, `<=`, `>=` |

### Declaration Syntax

```c
struct Point { int x; int y; };

Point operator+(Point a, Point b) {
    Point result;
    result.x = a.x + b.x;
    result.y = a.y + b.y;
    return result;
}
```

The function name is literally `operator+` (or `operator-`, etc.). The parser recognizes the `operator` keyword followed by an operator symbol as a function name.

### Call Syntax

Same as normal operators:
```c
Point p1 = {1, 2};
Point p2 = {3, 4};
Point p3 = p1 + p2;  // Calls operator+_Point_Point(p1, p2)
```

### Parser Changes

1. `parseFunctionDecl`: After parsing the function name, check if it's the `operator` keyword. If so, read the next token as the operator symbol and combine: `operator+`. This becomes the function name.

2. Binary expression parsing: When parsing `a + b`, check if **at least one** operand is a struct/union type. If so, look up `operator+_<TypeA>_<TypeB>` in the overload set. If found, transform to a function call. If not found, error. Built-in arithmetic operators (+, -, *, /) only apply when **both** operands are primitives (int, float, double, char). Mixed types like `Point + int` require a matching operator overload.

### Semantic Analyzer

- For binary expressions with struct/union operands, look up `operator+_<TypeA>_<TypeB>` in the overload set.
- Validate: operator overloads must be functions, must take exactly 2 parameters.

### Codegen

- Operator functions use the same mangling: `operator+_Point_Point`
- Binary expressions on structs: emit a call to the mangled operator function instead of `builder.CreateAdd()` etc.

## 5. Error Messages

| Scenario | Error Message |
|----------|--------------|
| Duplicate exact signature | `"redefinition of function 'add(int, int)'"` |
| No matching overload | `"no matching function for call to 'add' with arguments (int, float, float)"` |
| Ambiguous overload | `"ambiguous call to overloaded function 'add' — candidates: add(int, float), add(float, int)"` |
| Operator overload on primitive | `"operator+ cannot be overloaded for primitive type 'int'"` |
| Invalid operator overload signature | `"operator+ must take exactly 2 parameters"` |
| Missing operator overload for types | `"no matching operator+ for types 'Point' and 'Float'"` |

## 6. Files to Modify

| File | Changes |
|------|---------|
| `src/ast/Symbol.h` | Add `OverloadSet` class, change `Scope::symbols` type |
| `src/ast/Symbol.cpp` | Implement `OverloadSet::add()`, `resolve()`, update `Scope::declare()` and `lookup()` |
| `src/ast/Type.h` | Add `typeToString()` utility for mangling |
| `src/sema/SemanticAnalyzer.cpp` | Update `declare()` to use OverloadSet, update `checkFunctionCall()` for overload resolution, add operator overload validation |
| `src/frontend/Parser.cpp` | Parse `operator` keyword in function names, transform binary expressions on structs to operator calls |
| `src/ast/Decl.cpp` | Use mangled name in `FunctionDeclAST::codegen()` |
| `src/ast/Expr.cpp` | Use mangled name in `CallExprAST::codegen()`, emit operator function calls for struct binary expressions |
| `src/codegen/CodegenContext.h/cpp` | Add `mangleFunction()` utility |

## 7. Testing

### Parser Tests
- Parse function overloading declarations (same name, different param types)
- Parse `operator+` function declarations
- Parse binary expressions on structs that should resolve to operator calls

### Semantic Analyzer Tests
- Overload resolution with exact match
- Error on duplicate exact signature
- Error on no matching overload
- Error on ambiguous overload
- Operator overload validation (must be function, must take 2 params)
- Operator overload on primitive type error

### E2E Tests
- Call overloaded functions with different argument types
- Use overloaded operators on structs
- Verify correct function is called via return value
