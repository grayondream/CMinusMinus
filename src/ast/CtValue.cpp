#include "ast/CtValue.h"

#include "ast/Expr.h"
#include "ast/Type.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

uint64_t maskFor(unsigned bits) {
    if (bits >= 64) return ~uint64_t{0};
    if (bits == 0) return 0;
    return (uint64_t{1} << bits) - 1;
}

int64_t signExtend(uint64_t raw, unsigned bits) {
    if (bits == 0) return 0;
    if (bits >= 64) return static_cast<int64_t>(raw);
    const uint64_t sign = uint64_t{1} << (bits - 1);
    const uint64_t mask = maskFor(bits);
    raw &= mask;
    return static_cast<int64_t>((raw ^ sign) - sign);
}

Type* stripTypedefs(Type* t) {
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    return t;
}

CtValue makeIntBits(uint64_t raw, unsigned bits, bool isUnsigned,
                    bool isBool = false) {
    return ctNormalizeInt(raw, bits, isUnsigned, isBool);
}

void cloneContainerInto(const CtContainer& src, CtContainer& dst) {
    dst.kind = src.kind;
    dst.elementType = src.elementType;
    dst.keyType = src.keyType;
    dst.valueType = src.valueType;
    dst.cap = src.cap;
    dst.len = src.len;
    dst.head = src.head;
    dst.tombstones = src.tombstones;
    dst.hashFn = src.hashFn;
    dst.eqFn = src.eqFn;
    dst.items.clear();
    dst.items.reserve(src.items.size());
    for (const auto& v : src.items) dst.items.push_back(ctClone(v));
    dst.keys.clear();
    dst.keys.reserve(src.keys.size());
    for (const auto& v : src.keys) dst.keys.push_back(ctClone(v));
    dst.values.clear();
    dst.values.reserve(src.values.size());
    for (const auto& v : src.values) dst.values.push_back(ctClone(v));
    dst.states = src.states;
}

bool isComparisonOp(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq:
        case BinaryOp::NotEq:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge:
        case BinaryOp::And:
        case BinaryOp::Or:
            return true;
        default:
            return false;
    }
}

bool isLogicalOp(BinaryOp op) {
    return op == BinaryOp::And || op == BinaryOp::Or;
}

CtValue intOperand(const CtValue& v, unsigned& bits, bool& isUnsigned) {
    if (v.type == CtValue::CHAR) {
        bits = 8;
        isUnsigned = false;
        return ctInt(static_cast<unsigned char>(v.charVal), 8, false);
    }
    bits = v.bits ? v.bits : 64;
    isUnsigned = v.isUnsigned;
    return v;
}

} // namespace

CtValue ctInt(long long v, unsigned bits, bool isUnsigned, bool isBool) {
    if (bits == 0) bits = 1;
    if (bits > 64) bits = 64;
    return ctNormalizeInt(static_cast<uint64_t>(v), bits, isUnsigned, isBool);
}

CtValue ctNormalizeInt(uint64_t raw, unsigned bits, bool isUnsigned, bool isBool) {
    if (bits == 0) bits = 1;
    if (bits > 64) bits = 64;
    CtValue v;
    v.type = CtValue::INT;
    v.bits = bits;
    v.isUnsigned = isUnsigned;
    v.isBool = isBool;
    if (isUnsigned) {
        v.intVal = static_cast<long long>(raw & maskFor(bits));
    } else {
        v.intVal = static_cast<long long>(signExtend(raw, bits));
    }
    return v;
}

bool ctIntInfo(Type* t, unsigned& bits, bool& isUnsigned, bool& isBool) {
    t = stripTypedefs(t);
    if (!t) return false;
    isBool = false;
    switch (t->kind) {
        case TypeKind::Bool: bits = 1; isUnsigned = true; isBool = true; return true;
        case TypeKind::Char: bits = 8; isUnsigned = false; return true;
        case TypeKind::Int8: bits = 8; isUnsigned = false; return true;
        case TypeKind::Int16: bits = 16; isUnsigned = false; return true;
        case TypeKind::Int32: bits = 32; isUnsigned = false; return true;
        case TypeKind::Int64: bits = 64; isUnsigned = false; return true;
        case TypeKind::UInt8: bits = 8; isUnsigned = true; return true;
        case TypeKind::UInt16: bits = 16; isUnsigned = true; return true;
        case TypeKind::UInt32: bits = 32; isUnsigned = true; return true;
        case TypeKind::UInt64: bits = 64; isUnsigned = true; return true;
        case TypeKind::ISize: bits = 64; isUnsigned = false; return true;
        case TypeKind::USize: bits = 64; isUnsigned = true; return true;
        case TypeKind::Int128:
        case TypeKind::UInt128:
            // 128-bit constants are not representable in CtValue; callers
            // diagnose them. Report the width so the fallback is explicit.
            bits = 128;
            isUnsigned = t->kind == TypeKind::UInt128;
            return true;
        case TypeKind::Enum: {
            auto* et = static_cast<EnumType*>(t);
            if (et->underlyingType &&
                ctIntInfo(et->underlyingType, bits, isUnsigned, isBool)) {
                return true;
            }
            bits = 32;
            isUnsigned = false;
            return true;
        }
        default:
            return false;
    }
}

bool ctTruthy(const CtValue& v) {
    switch (v.type) {
        case CtValue::INT: return v.intVal != 0;
        case CtValue::CHAR: return v.charVal != 0;
        case CtValue::DOUBLE: return v.doubleVal != 0.0;
        case CtValue::STR: return !v.strVal.empty();
        case CtValue::TYPE: return v.typeVal != nullptr;
        case CtValue::FUNC: return !v.funcName.empty();
        case CtValue::CONTAINER: return v.container != nullptr;
        case CtValue::PTR: return v.container != nullptr;
        case CtValue::AGGREGATE: return v.aggregate != nullptr;
    }
    return false;
}

bool ctEquals(const CtValue& a, const CtValue& b) {
    if (a.type != b.type) {
        if ((a.type == CtValue::INT || a.type == CtValue::CHAR) &&
            (b.type == CtValue::INT || b.type == CtValue::CHAR)) {
            const long long av = a.type == CtValue::CHAR ? a.charVal : a.intVal;
            const long long bv = b.type == CtValue::CHAR ? b.charVal : b.intVal;
            return av == bv;
        }
        return false;
    }
    switch (a.type) {
        case CtValue::INT:
            if (a.isUnsigned || b.isUnsigned) {
                return static_cast<uint64_t>(a.intVal) ==
                       static_cast<uint64_t>(b.intVal);
            }
            return a.intVal == b.intVal;
        case CtValue::CHAR: return a.charVal == b.charVal;
        case CtValue::DOUBLE: return a.doubleVal == b.doubleVal;
        case CtValue::STR: return a.strVal == b.strVal;
        case CtValue::TYPE: return a.typeVal == b.typeVal;
        case CtValue::FUNC: return a.funcName == b.funcName;
        case CtValue::CONTAINER: return a.container == b.container;
        case CtValue::PTR:
            return a.container == b.container && a.ptrIndex == b.ptrIndex;
        case CtValue::AGGREGATE: return a.aggregate == b.aggregate;
    }
    return false;
}

CtValue ctClone(const CtValue& v) {
    CtValue out = v;
    if (v.type == CtValue::CONTAINER && v.container) {
        auto copy = std::make_shared<CtContainer>();
        cloneContainerInto(*v.container, *copy);
        out.container = std::move(copy);
    }
    if (v.type == CtValue::AGGREGATE && v.aggregate) {
        auto copy = std::make_shared<CtAggregate>();
        copy->type = v.aggregate->type;
        copy->totalElements = v.aggregate->totalElements;
        copy->elements.reserve(v.aggregate->elements.size());
        for (const auto& e : v.aggregate->elements) {
            copy->elements.push_back(ctClone(e));
        }
        out.aggregate = std::move(copy);
    }
    // PTR keeps the owner (it is a view, not an owning value).
    return out;
}

std::optional<CtValue> ctBinary(const CtValue& left, const CtValue& right,
                                int opInt, Type* resultType) {
    const BinaryOp op = static_cast<BinaryOp>(opInt);

    if (left.type == CtValue::STR && right.type == CtValue::STR) {
        if (op == BinaryOp::Eq || op == BinaryOp::NotEq) {
            const bool eq = left.strVal == right.strVal;
            return makeIntBits(eq == (op == BinaryOp::Eq) ? 1 : 0, 1, true, true);
        }
        return std::nullopt;
    }

    if ((left.type == CtValue::FUNC && right.type == CtValue::FUNC) ||
        (left.type == CtValue::TYPE && right.type == CtValue::TYPE)) {
        if (op == BinaryOp::Eq || op == BinaryOp::NotEq) {
            const bool eq = ctEquals(left, right);
            return makeIntBits(eq == (op == BinaryOp::Eq) ? 1 : 0, 1, true, true);
        }
        return std::nullopt;
    }

    const bool leftIsInt = left.type == CtValue::INT || left.type == CtValue::CHAR;
    const bool rightIsInt = right.type == CtValue::INT || right.type == CtValue::CHAR;

    if ((left.type == CtValue::DOUBLE && (rightIsInt || right.type == CtValue::DOUBLE)) ||
        (right.type == CtValue::DOUBLE && leftIsInt)) {
        auto asDouble = [](const CtValue& v) {
            if (v.type == CtValue::DOUBLE) return v.doubleVal;
            if (v.type == CtValue::CHAR) return static_cast<double>(v.charVal);
            return static_cast<double>(v.intVal);
        };
        const double a = asDouble(left);
        const double b = asDouble(right);
        CtValue out;
        switch (op) {
            case BinaryOp::Add: out.type = CtValue::DOUBLE; out.doubleVal = a + b; return out;
            case BinaryOp::Sub: out.type = CtValue::DOUBLE; out.doubleVal = a - b; return out;
            case BinaryOp::Mul: out.type = CtValue::DOUBLE; out.doubleVal = a * b; return out;
            case BinaryOp::Div:
                if (b == 0.0) return std::nullopt;
                out.type = CtValue::DOUBLE; out.doubleVal = a / b; return out;
            case BinaryOp::Eq: return makeIntBits(a == b ? 1 : 0, 1, true, true);
            case BinaryOp::NotEq: return makeIntBits(a != b ? 1 : 0, 1, true, true);
            case BinaryOp::Lt: return makeIntBits(a < b ? 1 : 0, 1, true, true);
            case BinaryOp::Gt: return makeIntBits(a > b ? 1 : 0, 1, true, true);
            case BinaryOp::Le: return makeIntBits(a <= b ? 1 : 0, 1, true, true);
            case BinaryOp::Ge: return makeIntBits(a >= b ? 1 : 0, 1, true, true);
            default: return std::nullopt;
        }
    }

    if (!leftIsInt || !rightIsInt) {
        return std::nullopt;
    }

    unsigned lb = 64, rb = 64;
    bool lu = false, ru = false;
    CtValue l = intOperand(left, lb, lu);
    CtValue r = intOperand(right, rb, ru);

    const bool comparison = isComparisonOp(op);
    unsigned opBits;
    bool opUnsigned;
    if (!comparison && resultType) {
        unsigned tbits = 0;
        bool tuns = false, tbool = false;
        if (ctIntInfo(resultType, tbits, tuns, tbool) && tbits <= 64) {
            opBits = tbits;
            opUnsigned = tuns;
        } else {
            opBits = std::max(lb, rb);
            opUnsigned = lu || ru;
        }
    } else {
        opBits = std::max(lb, rb);
        opUnsigned = lu || ru;
        // C integer promotion when the operation has not been typed yet:
        // narrower operands promote to int before arithmetic.
        if (!comparison && opBits < 32) {
            opBits = 32;
            opUnsigned = false;
        }
    }
    if (opBits > 64) {
        return std::nullopt;
    }

    const uint64_t mask = maskFor(opBits);
    const uint64_t ar = static_cast<uint64_t>(l.intVal) & mask;
    const uint64_t br = static_cast<uint64_t>(r.intVal) & mask;
    const int64_t as = signExtend(ar, opBits);
    const int64_t bs = signExtend(br, opBits);

    auto boolResult = [](bool value) {
        return makeIntBits(value ? 1 : 0, 1, true, true);
    };

    if (isLogicalOp(op)) {
        if (op == BinaryOp::And) return boolResult(ctTruthy(left) && ctTruthy(right));
        return boolResult(ctTruthy(left) || ctTruthy(right));
    }

    if (comparison) {
        switch (op) {
            case BinaryOp::Eq:
                return boolResult(opUnsigned ? ar == br : as == bs);
            case BinaryOp::NotEq:
                return boolResult(opUnsigned ? ar != br : as != bs);
            case BinaryOp::Lt:
                return boolResult(opUnsigned ? ar < br : as < bs);
            case BinaryOp::Gt:
                return boolResult(opUnsigned ? ar > br : as > bs);
            case BinaryOp::Le:
                return boolResult(opUnsigned ? ar <= br : as <= bs);
            case BinaryOp::Ge:
                return boolResult(opUnsigned ? ar >= br : as >= bs);
            default:
                return std::nullopt;
        }
    }

    uint64_t result = 0;
    switch (op) {
        case BinaryOp::Add: result = ar + br; break;
        case BinaryOp::Sub: result = ar - br; break;
        case BinaryOp::Mul: result = ar * br; break;
        case BinaryOp::Div:
            if (br == 0) return std::nullopt;
            if (!opUnsigned && opBits == 64 && as == std::numeric_limits<int64_t>::min() && bs == -1) {
                // Two's-complement wraparound; C++ signed division would trap.
                result = static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
            } else {
                result = opUnsigned ? (ar / br)
                                    : static_cast<uint64_t>(as / bs);
            }
            break;
        case BinaryOp::Mod:
            if (br == 0) return std::nullopt;
            if (!opUnsigned && opBits == 64 && as == std::numeric_limits<int64_t>::min() && bs == -1) {
                result = 0;
            } else {
                result = opUnsigned ? (ar % br)
                                    : static_cast<uint64_t>(as % bs);
            }
            break;
        case BinaryOp::BitAnd: result = ar & br; break;
        case BinaryOp::BitOr: result = ar | br; break;
        case BinaryOp::BitXor: result = ar ^ br; break;
        case BinaryOp::LShift: {
            const unsigned shift = static_cast<unsigned>(br & (opBits - 1));
            result = ar << shift;
            break;
        }
        case BinaryOp::RShift: {
            const unsigned shift = static_cast<unsigned>(br & (opBits - 1));
            result = opUnsigned ? (ar >> shift)
                                : static_cast<uint64_t>(as >> shift);
            break;
        }
        default:
            return std::nullopt;
    }
    return makeIntBits(result, opBits, opUnsigned);
}

std::optional<CtValue> ctCast(const CtValue& v, Type* to) {
    to = stripTypedefs(to);
    if (!to) return std::nullopt;

    unsigned bits = 0;
    bool isUnsigned = false, isBool = false;
    if (ctIntInfo(to, bits, isUnsigned, isBool)) {
        if (bits > 64) return std::nullopt;
        if (to->kind == TypeKind::Char) {
            CtValue out;
            out.type = CtValue::CHAR;
            out.charVal = static_cast<char>(v.type == CtValue::DOUBLE
                                                ? static_cast<long long>(v.doubleVal)
                                                : (v.type == CtValue::CHAR ? v.charVal
                                                                           : v.intVal));
            return out;
        }
        long long raw = 0;
        switch (v.type) {
            case CtValue::INT: raw = v.intVal; break;
            case CtValue::CHAR: raw = v.charVal; break;
            case CtValue::DOUBLE: raw = static_cast<long long>(v.doubleVal); break;
            default: return std::nullopt;
        }
        return ctNormalizeInt(static_cast<uint64_t>(raw), bits, isUnsigned, isBool);
    }

    if (to->kind == TypeKind::Float32 || to->kind == TypeKind::Float64 ||
        to->kind == TypeKind::Float16 || to->kind == TypeKind::Float128) {
        CtValue out;
        out.type = CtValue::DOUBLE;
        switch (v.type) {
            case CtValue::INT: out.doubleVal = static_cast<double>(v.intVal); break;
            case CtValue::CHAR: out.doubleVal = static_cast<double>(v.charVal); break;
            case CtValue::DOUBLE: out.doubleVal = v.doubleVal; break;
            default: return std::nullopt;
        }
        return out;
    }

    return std::nullopt;
}

CtValue ctDefaultValue(Type* t) {
    t = stripTypedefs(t);
    if (!t) {
        return ctInt(0);
    }

    if (auto info = ctContainerInfo(t)) {
        auto c = std::make_shared<CtContainer>();
        c->kind = info->kind;
        c->elementType = info->elementType;
        c->keyType = info->keyType;
        c->valueType = info->valueType;
        CtValue out;
        out.type = CtValue::CONTAINER;
        out.container = std::move(c);
        return out;
    }

    if (t->kind == TypeKind::Array) {
        auto* arr = static_cast<ArrayType*>(t);
        auto agg = std::make_shared<CtAggregate>();
        agg->type = t;
        const int n = arr->size > 0 ? arr->size : 0;
        agg->elements.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            agg->elements.push_back(ctDefaultValue(arr->elementType));
        }
        agg->totalElements = static_cast<std::size_t>(n);
        CtValue out;
        out.type = CtValue::AGGREGATE;
        out.aggregate = std::move(agg);
        return out;
    }

    if (t->kind == TypeKind::Struct || t->kind == TypeKind::Class) {
        const std::vector<FieldInfo>* fields =
            t->kind == TypeKind::Struct
                ? &static_cast<StructType*>(t)->fields
                : &static_cast<ClassType*>(t)->fields;
        auto agg = std::make_shared<CtAggregate>();
        agg->type = t;
        agg->elements.reserve(fields->size());
        for (const auto& f : *fields) {
            agg->elements.push_back(ctDefaultValue(f.type));
        }
        agg->totalElements = fields->size();
        CtValue out;
        out.type = CtValue::AGGREGATE;
        out.aggregate = std::move(agg);
        return out;
    }

    switch (t->kind) {
        case TypeKind::Bool: return ctInt(0, 1, true, true);
        case TypeKind::Char: {
            CtValue v;
            v.type = CtValue::CHAR;
            v.charVal = 0;
            return v;
        }
        case TypeKind::Float16:
        case TypeKind::Float32:
        case TypeKind::Float64:
        case TypeKind::Float128: {
            CtValue v;
            v.type = CtValue::DOUBLE;
            return v;
        }
        case TypeKind::Str: {
            CtValue v;
            v.type = CtValue::STR;
            return v;
        }
        case TypeKind::Enum: {
            unsigned bits = 32;
            bool uns = false, b = false;
            ctIntInfo(t, bits, uns, b);
            return ctInt(0, bits, uns);
        }
        default: {
            unsigned bits = 64;
            bool uns = false, b = false;
            if (ctIntInfo(t, bits, uns, b)) {
                if (bits > 64) bits = 64;
                return ctInt(0, bits, uns, b);
            }
            return ctInt(0);
        }
    }
}

CtValue ctConvertValue(const CtValue& v, Type* target) {
    Type* t = stripTypedefs(target);
    if (!t) return ctClone(v);

    if (v.type == CtValue::INT || v.type == CtValue::CHAR ||
        v.type == CtValue::DOUBLE) {
        unsigned bits = 0;
        bool isUnsigned = false;
        bool isBool = false;
        const bool intTarget = ctIntInfo(t, bits, isUnsigned, isBool);
        const bool floatTarget =
            t->kind == TypeKind::Float16 || t->kind == TypeKind::Float32 ||
            t->kind == TypeKind::Float64 || t->kind == TypeKind::Float128;
        if (intTarget || floatTarget) {
            if (auto converted = ctCast(v, t)) return *converted;
        }
    }

    if (v.type != CtValue::AGGREGATE || !v.aggregate) return ctClone(v);

    if (t->kind == TypeKind::Array) {
        auto* arr = static_cast<ArrayType*>(t);
        auto out = std::make_shared<CtAggregate>();
        out->type = t;
        const std::size_t n = static_cast<std::size_t>(arr->size > 0 ? arr->size : 0);
        out->elements.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            CtValue elem = i < v.aggregate->elements.size()
                               ? v.aggregate->elements[i]
                               : ctDefaultValue(arr->elementType);
            out->elements.push_back(ctConvertValue(elem, arr->elementType));
        }
        out->totalElements = n;
        CtValue result;
        result.type = CtValue::AGGREGATE;
        result.aggregate = std::move(out);
        return result;
    }

    if (t->kind == TypeKind::Struct || t->kind == TypeKind::Class) {
        const std::vector<FieldInfo>* fields =
            t->kind == TypeKind::Struct
                ? &static_cast<StructType*>(t)->fields
                : &static_cast<ClassType*>(t)->fields;
        auto out = std::make_shared<CtAggregate>();
        out->type = t;
        out->elements.reserve(fields->size());
        for (std::size_t i = 0; i < fields->size(); ++i) {
            CtValue elem = i < v.aggregate->elements.size()
                               ? v.aggregate->elements[i]
                               : ctDefaultValue((*fields)[i].type);
            out->elements.push_back(ctConvertValue(elem, (*fields)[i].type));
        }
        out->totalElements = fields->size();
        CtValue result;
        result.type = CtValue::AGGREGATE;
        result.aggregate = std::move(out);
        return result;
    }

    return ctClone(v);
}

std::optional<CtContainerInfo> ctContainerInfo(Type* t) {
    t = stripTypedefs(t);
    if (!t) return std::nullopt;

    const std::vector<FieldInfo>* fields = nullptr;
    std::string name;
    if (t->kind == TypeKind::Class) {
        auto* ct = static_cast<ClassType*>(t);
        fields = &ct->fields;
        name = ct->name;
    } else if (t->kind == TypeKind::Struct) {
        auto* st = static_cast<StructType*>(t);
        fields = &st->fields;
        name = st->name;
    } else {
        return std::nullopt;
    }

    auto fieldType = [&](const std::string& field) -> Type* {
        for (const auto& f : *fields) {
            if (f.name == field) return f.type;
        }
        return nullptr;
    };
    auto pointee = [](Type* ft) -> Type* {
        ft = stripTypedefs(ft);
        if (ft && ft->kind == TypeKind::Pointer) return ft->base;
        return ft;
    };

    CtContainerInfo info;
    if (name.rfind("std_Vec$", 0) == 0) {
        info.kind = CtContainer::Vec;
        info.elementType = pointee(fieldType("data"));
        return info;
    }
    if (name.rfind("std_Deque$", 0) == 0) {
        info.kind = CtContainer::Deque;
        info.elementType = pointee(fieldType("data"));
        return info;
    }
    if (name.rfind("std_HashMap$", 0) == 0) {
        info.kind = CtContainer::HashMap;
        info.keyType = pointee(fieldType("keys"));
        info.valueType = pointee(fieldType("values"));
        return info;
    }
    if (name.rfind("std_HashSet$", 0) == 0) {
        info.kind = CtContainer::HashSet;
        info.keyType = pointee(fieldType("keys"));
        return info;
    }
    return std::nullopt;
}
