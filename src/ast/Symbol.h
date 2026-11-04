#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "llvm/IR/Value.h"
#include "ast/Type.h"

class FunctionDeclAST;

class Symbol {
public:
    Symbol(const std::string& name, Type* type, llvm::Value* value = nullptr)
        : name(name), type(type), value(value) {}
public:
    std::string name;
    Type* type;
    llvm::Value* value;

    // SEM-11 unused-variable analysis (W3002). Only local variables opt in via
    // `checkUnused`; parameters, globals and functions never warn here.
    bool isUsed = false;
    bool checkUnused = false;
    // PAR-11: file/namespace-scope variables and block-scope `static`/
    // `thread_local`/`extern` declarations have static storage duration and
    // are therefore never lambda-captured (C++ [expr.prim.lambda] semantics).
    bool isStaticStorage = false;
    // CT: this symbol is a constexpr container variable whose materialized
    // storage is read-only. Symbol identity (not just the name) keeps shadowed
    // locals and same-named parameters mutable.
    bool isConstexprContainer = false;
    std::string declFile;
    int declLine = 0;
    int declColumn = 0;

    // FUN-09: back-pointer to the declaring FunctionDeclAST, set by sema so
    // overload resolution can read default-argument counts. Null for
    // non-function symbols and prelude/declared-only functions.
    FunctionDeclAST* decl = nullptr;
};

// Structural type equality (used by conversionRank and overload matching).
bool typesEqual(Type* a, Type* b);

// Returns the implicit-conversion rank from `from` to `to`:
//   0  = exact match
//   1  = safe arithmetic conversion (integer widening, int -> float, float widening)
//   -1 = not implicitly convertible
int conversionRank(Type* from, Type* to);

// --- TYP-22: integer promotion and usual arithmetic conversions -------------
//
// These operate on AST types (typedefs stripped, enums mapped to their
// underlying integer) and are shared by sema (common-type computation) and
// codegen (signedness of division/remainder/shift/comparison).

// True when `type` (after typedef/enum stripping) is an arithmetic type.
bool isArithmeticType(Type* type);

// Strip typedefs and map an enum to its underlying type (default `int`).
Type* arithmeticUnderlying(Type* type);

// Integer conversion rank: bool < char/int8/uint8 < int16/uint16
// < int/int32/uint32 < int64/uint64/isize/usize < int128/uint128. -1 if not an
// integer type.
int integerRank(TypeKind kind);

// Integer promotion: integer types with rank below `int` widen to `int`
// (TYP-22 §3). Floating-point and other types are returned unchanged.
Type* promoteArithmeticType(Type* type);

// Usual arithmetic conversions for two arithmetic operands (TYP-22 §3).
Type* usualArithmeticType(Type* a, Type* b);

// True when the (promoted) arithmetic type is unsigned: `uint32/64/128`, `usize`.
bool isUnsignedArithmeticType(Type* type);

// True when `type` itself (typedefs stripped, enums via underlying) is an
// unsigned integer type, including narrow `uint8`/`uint16`. Used to pick
// `zext`/`uitofp` when extending a source value (TYP-22).
bool isUnsignedIntegerType(Type* type);

class OverloadSet {
public:
    void add(Symbol* sym);
    Symbol* resolve(const std::vector<Type*>& argTypes) const;
    const std::vector<Symbol*>& getCandidates() const;
    size_t size() const;
    bool empty() const;

private:
    std::vector<Symbol*> candidates;
};

class Scope {
public:
    Scope(Scope* parent) : parent(parent) {}

    Symbol* lookup(const std::string& name) const;
    OverloadSet* lookupOverload(const std::string& name);
    bool declare(const std::string& name, Symbol* sym);

public:
    std::unordered_map<std::string, OverloadSet> symbols;
    Scope* parent;
};
