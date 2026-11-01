#pragma once

// Shared compile-time value model (constexpr evaluator + compile_time queries
// + codegen constant materialization). Keep this header free of LLVM and of
// the compiler's sema implementation so AST and sema can both use it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Type;

struct CtContainer;

struct CtValue {
    enum Type {
        INT,
        DOUBLE,
        CHAR,
        STR,
        TYPE,
        FUNC,
        CONTAINER,
        PTR,
    };

    Type type = INT;
    // Integer payload. Stored as the 64-bit bit pattern; `bits` and
    // `isUnsigned` say how to interpret it. uint64 values above INT64_MAX are
    // therefore negative in intVal and must be read through uint64_t.
    long long intVal = 0;
    unsigned bits = 64;
    bool isUnsigned = false;
    bool isBool = false;

    double doubleVal = 0;
    char charVal = 0;
    std::string strVal;        // STR
    ::Type* typeVal = nullptr; // TYPE
    std::string funcName;      // FUNC: resolved (mangled) function name

    // CONTAINER: the container value. PTR: the owning container plus
    // ptrIndex (used for get_ptr / .data element views).
    std::shared_ptr<CtContainer> container;
    long long ptrIndex = 0;
    // PTR to an evaluator environment slot (`&local`) used for out-parameters.
    std::string ptrName;
};

struct CtContainer {
    enum Kind { Vec, Deque, HashMap, HashSet };

    Kind kind = Vec;
    ::Type* elementType = nullptr; // Vec/Deque
    ::Type* keyType = nullptr;     // HashMap/HashSet
    ::Type* valueType = nullptr;   // HashMap

    // Vec/Deque: physical ring storage, always `cap` entries (or empty when
    // cap == 0). HashMap/HashSet: open-addressing arrays, `cap` entries.
    std::vector<CtValue> items;
    std::vector<CtValue> keys;
    std::vector<CtValue> values;
    std::vector<uint8_t> states;

    std::size_t cap = 0;
    std::size_t len = 0;
    std::size_t head = 0;
    std::size_t tombstones = 0;

    // Resolved (mangled) callback names. Empty means null at runtime.
    std::string hashFn;
    std::string eqFn;
};

struct CtContainerInfo {
    CtContainer::Kind kind;
    ::Type* elementType = nullptr;
    ::Type* keyType = nullptr;
    ::Type* valueType = nullptr;
};

// Integer helpers -----------------------------------------------------------

CtValue ctInt(long long v, unsigned bits = 64, bool isUnsigned = false,
              bool isBool = false);

// Width/signedness of an integer-like type (bool/char/enum included).
bool ctIntInfo(::Type* t, unsigned& bits, bool& isUnsigned, bool& isBool);

// Mask/sign-extend `raw` to `bits` and wrap it in a CtValue.
CtValue ctNormalizeInt(uint64_t raw, unsigned bits, bool isUnsigned,
                       bool isBool = false);

bool ctTruthy(const CtValue& v);
bool ctEquals(const CtValue& a, const CtValue& b);
CtValue ctClone(const CtValue& v);

// Arithmetic/comparison on already-evaluated operands. `resultType` is the
// sema-assigned type of the operation (may be null while an initializer is
// folded before its subexpressions are typed; widths are then inferred).
std::optional<CtValue> ctBinary(const CtValue& left, const CtValue& right,
                                int op, ::Type* resultType);
std::optional<CtValue> ctCast(const CtValue& v, ::Type* to);

// Default zero value for an uninitialized local of `t`.
CtValue ctDefaultValue(::Type* t);

// Detect one of the four std containers from its instantiated class type.
std::optional<CtContainerInfo> ctContainerInfo(::Type* t);
