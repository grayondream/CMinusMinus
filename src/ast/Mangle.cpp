#include "Mangle.h"
#include "ast/Type.h"
#include <unordered_set>

static std::unordered_set<std::string>& cNames() {
    static std::unordered_set<std::string> names;
    return names;
}

// C-linkage externs declared *inside a namespace body* (e.g. `std::sqrt`).
// Only these participate in the `ns::f` → bare-`f` resolution fallback, so
// a global-scope extern (prelude `malloc`, `strlen`, ...) is NOT reachable
// as `std::malloc` / `std::strlen` (F2 — namespace isolation preserved).
static std::unordered_set<std::string>& namespaceCNames() {
    static std::unordered_set<std::string> names;
    return names;
}

void markCName(const std::string& name) {
    cNames().insert(name);
}

void markNamespaceCName(const std::string& name) {
    cNames().insert(name);
    namespaceCNames().insert(name);
}

bool isCName(const std::string& name) {
    return cNames().count(name) != 0;
}

bool isNamespaceCName(const std::string& name) {
    return namespaceCNames().count(name) != 0;
}

std::string typeToMangled(Type* type) {
    if (!type) return "unknown";
    // CON-01: atomic-qualified types get a distinct mangling so overloads and
    // symbols stay isolated from the plain type.
    const std::string atomicTag = type->isAtomic ? "atomic" : "";
    switch (type->kind) {
        case TypeKind::Void: return atomicTag + "void";
        case TypeKind::Char: return atomicTag + "char";
        case TypeKind::Pointer: {
            std::string base = typeToMangled(type->base);
            return base + "ptr";
        }
        case TypeKind::Array: {
            auto* arr = static_cast<ArrayType*>(type);
            return typeToMangled(arr->elementType) + "arr";
        }
        case TypeKind::Struct: {
            auto* s = static_cast<StructType*>(type);
            return s->name;
        }
        case TypeKind::Class: {
            auto* c = static_cast<ClassType*>(type);
            return c->name;
        }
        case TypeKind::Union: {
            auto* u = static_cast<UnionType*>(type);
            return u->name;
        }
        case TypeKind::Enum: {
            // 评审 C1: all enums used to mangle as "int32", colliding
            // f(Optional<int32>) with f(Optional<SomeEnum>) symbols. Use the
            // (unique) enum name; anonymous enums keep the int32 ABI shape.
            auto* et = static_cast<EnumType*>(type);
            return et->name.empty() ? std::string("int32") : et->name;
        }
        case TypeKind::Bool: return atomicTag + "bool";
        case TypeKind::Int8: return atomicTag + "int8";
        case TypeKind::Int16: return atomicTag + "int16";
        case TypeKind::Int32: return atomicTag + "int32";
        case TypeKind::Int64: return atomicTag + "int64";
        case TypeKind::Int128: return atomicTag + "int128";
        case TypeKind::UInt8: return atomicTag + "uint8";
        case TypeKind::UInt16: return atomicTag + "uint16";
        case TypeKind::UInt32: return atomicTag + "uint32";
        case TypeKind::UInt64: return atomicTag + "uint64";
        case TypeKind::UInt128: return atomicTag + "uint128";
        // P1-06: string_view/owning string participate in template arguments
        // and function overload mangling; without explicit names they fell
        // back to "unknown" and instantiated e.g. std_Vec$unknown.
        case TypeKind::Str: return atomicTag + "str";
        case TypeKind::String: return atomicTag + "string";
        case TypeKind::ISize: return atomicTag + "isize";
        case TypeKind::USize: return atomicTag + "usize";
        case TypeKind::Float32: return "float32";
        case TypeKind::Float64: return "float64";
        case TypeKind::Slice: {
            auto* s = static_cast<SliceType*>(type);
            return typeToMangled(s->elementType) + "slice";
        }
        case TypeKind::Optional: {
            auto* o = static_cast<OptionalType*>(type);
            return typeToMangled(o->elementType) + "opt";
        }
        case TypeKind::Result: {
            auto* r = static_cast<ResultType*>(type);
            return typeToMangled(r->successType) + "res" + typeToMangled(r->errorType);
        }
        case TypeKind::Typedef: {
            auto* t = static_cast<TypedefType*>(type);
            return typeToMangled(t->aliasedType);
        }
        case TypeKind::TypeVar: {
            // 仅存在于模板体内；实例化后不残留，此处仅作诊断期兜底拼写。
            auto* tv = static_cast<TypeVarType*>(type);
            return tv->name;
        }
        case TypeKind::TypeInstance: {
            // P1-03: parse 期占位——拼写 = 模板名 + 各实参递归拼写，与实例
            // 名规则一致（`Box<Box<i32>>` → `Box$Box$i32`），保证单射。
            auto* ti = static_cast<TypeInstanceType*>(type);
            std::string s = ti->templateName;
            for (auto* a : ti->typeArgs) s += "$" + typeToMangled(a);
            for (auto v : ti->valueArgs) s += "$" + std::to_string(v);
            return s;
        }
        default: return "unknown";
    }
}

std::string mangleFunction(const std::string& name, const std::vector<Type*>& paramTypes) {
    if (isCName(name)) return name;
    if (paramTypes.empty()) return name;
    std::string result = name;
    for (auto* type : paramTypes) {
        result += "_" + typeToMangled(type);
    }
    return result;
}