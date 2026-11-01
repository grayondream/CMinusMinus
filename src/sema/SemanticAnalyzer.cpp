#include "sema/SemanticAnalyzer.h"
#include "sema/CompileTimeEvaluator.h"
#include "sema/ReflectionRegistry.h"
#include "ast/Expr.h"
#include "ast/Stmt.h"
#include "ast/Decl.h"
#include "ast/Type.h"
#include "ast/Mangle.h"
#include "sema/TemplateRegistry.h"
#include "support/Utf8.h"
#include "sema/TemplateInstantiator.h"
#include <algorithm>
#include <climits>
#include <functional>
#include <sstream>
#include <unordered_map>

// Strip any number of typedef/alias layers, returning the underlying type.
static Type* stripTypedef(Type* type) {
    while (type && type->kind == TypeKind::Typedef) {
        type = static_cast<TypedefType*>(type)->aliasedType;
    }
    return type;
}

// Review 2.5: a view/literal may implicitly become a read-only C string,
// never a mutable char*. Explicit casts remain the interop escape hatch.
static bool isConstCharPointer(Type* t) {
    t = stripTypedef(t);
    if (!t || t->kind != TypeKind::Pointer) return false;
    Type* base = stripTypedef(t->base);
    return base && base->kind == TypeKind::Char && base->isConst;
}

// P1-07 / SEM-12: bit-field base types — bool + the fixed-width integer
// family (int8..int128 / uint8..uint128 / isize / usize). char and enum are
// NOT allowed (spec §4.3).
static bool isBitFieldBaseType(Type* t) {
    t = stripTypedef(t);
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::Bool:
        case TypeKind::Int8: case TypeKind::Int16: case TypeKind::Int32:
        case TypeKind::Int64: case TypeKind::Int128:
        case TypeKind::UInt8: case TypeKind::UInt16: case TypeKind::UInt32:
        case TypeKind::UInt64: case TypeKind::UInt128:
        case TypeKind::ISize: case TypeKind::USize:
            return true;
        default: return false;
    }
}

// P1-07 / SEM-12: storage width of a bit-field base type in bits
// (Bool→1, isize/usize→64, otherwise the fixed width).
static int bitFieldTypeBits(Type* t) {
    t = stripTypedef(t);
    if (!t) return 0;
    switch (t->kind) {
        case TypeKind::Bool: return 1;
        case TypeKind::Int8: case TypeKind::UInt8: return 8;
        case TypeKind::Int16: case TypeKind::UInt16: return 16;
        case TypeKind::Int32: case TypeKind::UInt32: return 32;
        case TypeKind::Int64: case TypeKind::UInt64:
        case TypeKind::ISize: case TypeKind::USize: return 64;
        case TypeKind::Int128: case TypeKind::UInt128: return 128;
        default: return 0;
    }
}

// SEM-11: true when `stmt` unconditionally transfers control (return/break/
// continue). Used for the unreachable-code warning (W3003). Trailing `defer`
// statements do not clear termination — they still run on scope exit.
static bool stmtAlwaysTransfers(const StmtAST* stmt) {
    if (!stmt) return false;
    if (dynamic_cast<const ReturnStmtAST*>(stmt) ||
        dynamic_cast<const BreakStmtAST*>(stmt) ||
        dynamic_cast<const ContinueStmtAST*>(stmt)) {
        return true;
    }
    if (auto* compound = dynamic_cast<const CompoundStmtAST*>(stmt)) {
        for (auto it = compound->stmts.rbegin(); it != compound->stmts.rend(); ++it) {
            if (!*it) continue;
            if (dynamic_cast<const DeferStmtAST*>(it->get())) continue;
            return stmtAlwaysTransfers(it->get());
        }
        return false;
    }
    if (auto* ifStmt = dynamic_cast<const IfStmtAST*>(stmt)) {
        return ifStmt->thenStmt && ifStmt->elseStmt &&
               stmtAlwaysTransfers(ifStmt->thenStmt.get()) &&
               stmtAlwaysTransfers(ifStmt->elseStmt.get());
    }
    return false;
}

SemanticAnalyzer::SemanticAnalyzer()
    : globalScope(std::make_unique<Scope>(nullptr)),
      currentScope(globalScope.get()),
      currentFunction(nullptr),
      typeCtx(&TypeContext::instance()) {}

const std::vector<Diagnostic>& SemanticAnalyzer::getErrors() const {
    return errors;
}

const std::vector<Diagnostic>& SemanticAnalyzer::getWarnings() const {
    return warnings;
}

void SemanticAnalyzer::emitError(const std::string& msg, const ASTNode& node) {
    errors.emplace_back(Diagnostic::Level::Error, msg, node.sourceFile, node.sourceLine,
                        node.sourceColumn);
    if (!m_instStack.empty())
        errors.back().message +=
            " (in instantiation of template '" + m_instStack.back() + "')";
}

void SemanticAnalyzer::emitWarning(const std::string& msg, const ASTNode& node) {
    warnings.emplace_back(Diagnostic::Level::Warning, msg, node.sourceFile, node.sourceLine, node.sourceColumn);
}

void SemanticAnalyzer::emitError(DiagnosticCode code, const std::string& msg, const ASTNode& node) {
    errors.emplace_back(Diagnostic::Level::Error, code, msg, node.sourceFile, node.sourceLine, node.sourceColumn);
    if (!m_instStack.empty())
        errors.back().message +=
            " (in instantiation of template '" + m_instStack.back() + "')";
}

void SemanticAnalyzer::emitWarning(DiagnosticCode code, const std::string& msg, const ASTNode& node) {
    warnings.emplace_back(Diagnostic::Level::Warning, code, msg, node.sourceFile, node.sourceLine, node.sourceColumn);
}

void SemanticAnalyzer::enterScope() {
    currentScope = new Scope(currentScope);
}

void SemanticAnalyzer::exitScope() {
    // SEM-11: report local variables/arrays that were never referenced (W3002).
    // Parameters and globals do not opt in, and must not warn here.
    if (currentScope != globalScope.get()) {
        for (auto& entry : currentScope->symbols) {
            for (Symbol* sym : entry.second.getCandidates()) {
                if (sym->checkUnused && !sym->isUsed) {
                    warnings.emplace_back(Diagnostic::Level::Warning,
                                          DiagnosticCode::WarnUnusedVariable,
                                          "unused variable '" + sym->name + "'",
                                          sym->declFile, sym->declLine, sym->declColumn);
                }
            }
        }
    }
    Scope* parent = currentScope->parent;
    if (currentScope != globalScope.get()) {
        delete currentScope;
    }
    currentScope = parent;
}

bool SemanticAnalyzer::declare(const std::string& name, Type* type) {
    return declareSymbol(name, type) != nullptr;
}

Symbol* SemanticAnalyzer::declareSymbol(const std::string& name, Type* type) {
    // An exported declaration of a participating module is also visible to
    // importers, so it is registered in the global scope; everything else at
    // module top level stays in the module's private scope (MOD-05/06).
    Scope* target = currentScope;
    if (activeModuleScope != nullptr && currentScope == activeModuleScope && currentDeclExported) {
        target = globalScope.get();
    }
    Symbol* sym = new Symbol(name, type);
    if (!target->declare(name, sym)) {
        delete sym;
        return nullptr;
    }
    return sym;
}

Symbol* SemanticAnalyzer::lookup(const std::string& name) {
    for (const auto& candidate : namespaceCandidates(name)) {
        if (Symbol* sym = currentScope->lookup(candidate)) {
            return sym;
        }
    }
    return nullptr;
}

// Namespace helpers ---------------------------------------------------------

std::string SemanticAnalyzer::mangleNamespaceName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == ':' && i + 1 < name.size() && name[i + 1] == ':') {
            out.push_back('_');
            ++i;
        } else if (name[i] == '.') {
            out.push_back('_');
        } else {
            out.push_back(name[i]);
        }
    }
    return out;
}

// AGG-10/DS5: qualified access to a class static member goes through the
// desugared global symbol; check the declaring class's access level here so
// private static members stay unreachable from outside (E2009, as instance
// members). `originalName` is the pre-resolution spelling, e.g. `SMP::secret`.
void SemanticAnalyzer::checkStaticMemberAccess(const std::string& originalName, ExprAST& node) {
    if (originalName.find("::") == std::string::npos) return;
    auto it = staticMemberIndex.find(mangleNamespaceName(originalName));
    if (it == staticMemberIndex.end()) return;
    ClassType* definingClass = it->second.first;
    if (!definingClass) return;
    // I1: static members are keyed "static:<name>" so a same-name instance
    // member's access level is never consulted (independent keys).
    AccessLevel level = definingClass->memberAccessLevel("static:" + it->second.second);
    if (!canAccessMember(definingClass, level)) {
        emitError(DiagnosticCode::SemPrivateMemberAccess,
                  "cannot access " +
                      std::string(level == AccessLevel::Private ? "private" : "protected") +
                      " member '" + it->second.second + "' of class '" + definingClass->name +
                      "' outside the class; make it public or add an accessor",
                  node);
    }
}

// Candidate symbol-table keys for `name`, innermost namespace first.
std::vector<std::string> SemanticAnalyzer::namespaceCandidates(const std::string& name) const {
    std::vector<std::string> candidates;
    if (name.find("::") != std::string::npos || name.find('.') != std::string::npos) {
        candidates.push_back(mangleNamespaceName(name));
        // C-linkage extern functions keep their plain symbol name even when
        // spelled `ns::f`; allow a fallback to the bare C name so `std::sqrt`
        // resolves to the extern `sqrt` (registered under the bare key).
        // F2: only namespace-declared externs (`isNamespaceCName`) participate —
        // a global-scope extern like prelude `malloc` is NOT reachable as
        // `std::malloc` (namespace isolation preserved).
        std::string bare = name;
        size_t pos = bare.rfind("::");
        if (pos != std::string::npos) bare = bare.substr(pos + 2);
        if (isNamespaceCName(bare)) candidates.push_back(bare);
        return candidates;
    }
    std::string prefix = namespacePrefix; // e.g. "A_B_"
    while (!prefix.empty()) {
        candidates.push_back(prefix + name);
        if (prefix.back() == '_') prefix.pop_back();
        size_t pos = prefix.rfind('_');
        prefix = (pos == std::string::npos) ? std::string() : prefix.substr(0, pos + 1);
    }
    candidates.push_back(name);
    return candidates;
}

std::string SemanticAnalyzer::scopedName(const std::string& name) const {
    if (atGlobalLevel() && !namespacePrefix.empty()) {
        return namespacePrefix + name;
    }
    return name;
}

bool SemanticAnalyzer::atGlobalLevel() const {
    return currentScope == globalScope.get() || currentScope == activeModuleScope;
}

Scope* SemanticAnalyzer::moduleScopeFor(const std::string& moduleName) {
    auto it = moduleScopes.find(moduleName);
    if (it != moduleScopes.end()) {
        return it->second.get();
    }
    auto scope = std::make_unique<Scope>(globalScope.get());
    Scope* raw = scope.get();
    moduleScopes[moduleName] = std::move(scope);
    return raw;
}

void SemanticAnalyzer::enterModuleContext(const std::string& moduleName) {
    currentModule = moduleName;
    if (moduleName.empty()) {
        activeModuleScope = nullptr;
        currentScope = globalScope.get();
    } else {
        activeModuleScope = moduleScopeFor(moduleName);
        currentScope = activeModuleScope;
    }
}

std::string SemanticAnalyzer::resolveNamespaceName(const std::string& name) const {
    for (const auto& candidate : namespaceCandidates(name)) {
        if (OverloadSet* set = currentScope->lookupOverload(candidate)) {
            if (!set->empty()) {
                return candidate;
            }
        }
    }
    return name;
}

bool SemanticAnalyzer::isIntegerType(Type* type) const {
    type = stripTypedef(type);
    if (!type) return false;
    return type->kind == TypeKind::Char || type->kind == TypeKind::Enum ||
           type->kind == TypeKind::Bool ||
           type->kind == TypeKind::Int8 || type->kind == TypeKind::Int16 ||
           type->kind == TypeKind::Int32 || type->kind == TypeKind::Int64 || type->kind == TypeKind::Int128 ||
           type->kind == TypeKind::UInt8 || type->kind == TypeKind::UInt16 ||
           type->kind == TypeKind::UInt32 || type->kind == TypeKind::UInt64 || type->kind == TypeKind::UInt128 ||
           type->kind == TypeKind::ISize || type->kind == TypeKind::USize;
}

bool SemanticAnalyzer::isFloatType(Type* type) const {
    type = stripTypedef(type);
    if (!type) return false;
    return type->kind == TypeKind::Float32 || type->kind == TypeKind::Float64 ||
           type->kind == TypeKind::Float16 || type->kind == TypeKind::Float128;
}

bool SemanticAnalyzer::isArithmeticType(Type* type) const {
    return isIntegerType(type) || isFloatType(type);
}

bool SemanticAnalyzer::isPointerOrArray(Type* type) const {
    type = stripTypedef(type);
    if (!type) return false;
    return type->kind == TypeKind::Pointer || type->kind == TypeKind::Array;
}

bool SemanticAnalyzer::isScalarType(Type* type) const {
    return isArithmeticType(type) || isPointerOrArray(type);
}

bool SemanticAnalyzer::isConditionType(Type* type) const {
    Type* t = stripTypedef(type);
    if (!t) return false;
    return isScalarType(t) || t->kind == TypeKind::Optional ||
           t->kind == TypeKind::Result;
}

bool SemanticAnalyzer::typesCompatible(Type* left, Type* right) const {
    left = stripTypedef(left);
    right = stripTypedef(right);
    if (!left || !right) return false;
    // Enum types are strong (TYP-09 / TYP-20): only the exact same enum type is
    // implicitly compatible; enum <-> integer/float requires an explicit cast.
    if (left->kind == TypeKind::Enum || right->kind == TypeKind::Enum) {
        return left->kind == TypeKind::Enum && right->kind == TypeKind::Enum &&
               static_cast<EnumType*>(left)->name == static_cast<EnumType*>(right)->name;
    }
    // TYP-12: slices match by element type only (int32[] != float64[]);
    // arrays decay to slice views. Checked before the generic same-kind rule.
    if (left->kind == TypeKind::Slice && right->kind == TypeKind::Slice)
        return typesEqual(static_cast<SliceType*>(left)->elementType,
                          static_cast<SliceType*>(right)->elementType);
    if (left->kind == TypeKind::Slice && right->kind == TypeKind::Array)
        return typesEqual(static_cast<SliceType*>(left)->elementType,
                          static_cast<ArrayType*>(right)->elementType);
    // P1-02 (TYP-13/14): Optional/Result match strictly by their type
    // arguments (int32? != float64?) — before the generic same-kind rule.
    if (left->kind == TypeKind::Optional && right->kind == TypeKind::Optional)
        return typesEqual(static_cast<OptionalType*>(left)->elementType,
                          static_cast<OptionalType*>(right)->elementType);
    if (left->kind == TypeKind::Result && right->kind == TypeKind::Result)
        return typesEqual(left, right);
    // P1-06 (FMT-03): str is a byte view — implicit str -> char* is allowed
    // (drops the UTF-8 guarantee and the length). char* -> str is NOT
    // implicit (length/UTF-8 cannot be recovered; use the str_from_c
    // builtin). ONE direction only: left is the conversion TARGET, right the
    // source — callers (VarDecl init, return, init-lists) pass
    // (target, source); a symmetric rule silently admitted `str s = p;`
    // with an uninitialized length field (final review C1). The reverse
    // direction falls through to the final `return false`.
    if (isConstCharPointer(left) &&
        (right->kind == TypeKind::Str || right->kind == TypeKind::String))
        return true;
    if (left->kind == TypeKind::Str && right->kind == TypeKind::Str)
        return true;
    // P1-06 (FMT-02): string -> str is an implicit (non-owning) view;
    // str -> string is NOT implicit (use string.new).
    if (left->kind == TypeKind::Str && right->kind == TypeKind::String)
        return true;
    if (left->kind == TypeKind::String && right->kind == TypeKind::String)
        return true;
    // PAR-12 (TYP-19 落点): 聚合是名义类型——struct/class/union 同 kind 必须
    // 名字相同；派生→基（INH-04 切片）是唯一隐式跨聚合兼容。两次独立的
    // 匿名定义名字不同 → 比较/三元/返回/初始化/实参互斥（C 语义）。
    if (left->kind == right->kind &&
        (left->kind == TypeKind::Struct || left->kind == TypeKind::Class ||
         left->kind == TypeKind::Union)) {
        if (typesEqual(left, right)) return true;
        if (isDerivedFrom(right, left)) return true;
        return false;
    }
    if (left->kind == right->kind) return true;
    if (isArithmeticType(left) && isArithmeticType(right)) return true;
    if (left->kind == TypeKind::Pointer && right->kind == TypeKind::Pointer) return true;
    if (left->kind == TypeKind::Pointer && right->kind == TypeKind::Int32) return true;
    if (left->kind == TypeKind::Int32 && right->kind == TypeKind::Pointer) return true;
    // Arrays decay to a pointer to their first element.
    if (left->kind == TypeKind::Pointer && right->kind == TypeKind::Array) return true;
    if (left->kind == TypeKind::Array && right->kind == TypeKind::Pointer) return true;
    return false;
}

Type* SemanticAnalyzer::getCommonType(Type* left, Type* right) const {
    left = stripTypedef(left);
    right = stripTypedef(right);
    if (!left) return right;
    if (!right) return left;
    // TYP-22: integer promotion + usual arithmetic conversions.
    if (isArithmeticType(left) && isArithmeticType(right)) {
        return usualArithmeticType(left, right);
    }
    if (left->kind == right->kind) return left;
    return left;
}

std::string SemanticAnalyzer::typeToString(Type* type) const {
    if (!type) return "<unknown>";
    switch (type->kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Char: return "char";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int8: return "int8";
        case TypeKind::Int16: return "int16";
        case TypeKind::Int32: return "int32";
        case TypeKind::Int64: return "int64";
        case TypeKind::Int128: return "int128";
        case TypeKind::UInt8: return "uint8";
        case TypeKind::UInt16: return "uint16";
        case TypeKind::UInt32: return "uint32";
        case TypeKind::UInt64: return "uint64";
        case TypeKind::UInt128: return "uint128";
        case TypeKind::ISize: return "isize";
        case TypeKind::USize: return "usize";
        case TypeKind::Float32: return "float32";
        case TypeKind::Float64: return "float64";
        case TypeKind::Float16: return "float16";
        case TypeKind::Float128: return "float128";
        case TypeKind::Slice: return "slice";
        case TypeKind::Optional: return "optional";
        case TypeKind::Result: return "result";
        case TypeKind::Str: return "string_view";
        case TypeKind::String: return "string";
        case TypeKind::Auto: return "auto";
        case TypeKind::Pointer: {
            std::string baseStr = typeToString(type->base);
            if (type->isConst) baseStr = "const " + baseStr;
            return baseStr + "*";
        }
        case TypeKind::Array: {
            auto* arrType = static_cast<ArrayType*>(type);
            return typeToString(arrType->elementType) + "[" + std::to_string(arrType->size) + "]";
        }
        case TypeKind::Struct: {
            auto* structType = static_cast<StructType*>(type);
            return "struct " + structType->name;
        }
        case TypeKind::Class: {
            auto* classType = static_cast<ClassType*>(type);
            return "class " + classType->name;
        }
        case TypeKind::Union: {
            auto* unionType = static_cast<UnionType*>(type);
            return "union " + unionType->name;
        }
        case TypeKind::Enum: {
            auto* enumType = static_cast<EnumType*>(type);
            return "enum " + enumType->name;
        }
        case TypeKind::Function: return "<function>";
        case TypeKind::Typedef: {
            auto* typedefType = static_cast<TypedefType*>(type);
            return typedefType->name;
        }
        default: return "<unknown>";
    }
}

std::string SemanticAnalyzer::binaryOpToString(BinaryOp op) const {
    switch (op) {
        case BinaryOp::Add: return "+";
        case BinaryOp::Sub: return "-";
        case BinaryOp::Mul: return "*";
        case BinaryOp::Div: return "/";
        case BinaryOp::Mod: return "%";
        case BinaryOp::Eq: return "==";
        case BinaryOp::NotEq: return "!=";
        case BinaryOp::Lt: return "<";
        case BinaryOp::Gt: return ">";
        case BinaryOp::Le: return "<=";
        case BinaryOp::Ge: return ">=";
        case BinaryOp::And: return "&&";
        case BinaryOp::Or: return "||";
        case BinaryOp::BitAnd: return "&";
        case BinaryOp::BitOr: return "|";
        case BinaryOp::BitXor: return "^";
        case BinaryOp::LShift: return "<<";
        case BinaryOp::RShift: return ">>";
        default: return "<unknown>";
    }
}

bool SemanticAnalyzer::isStructOrUnionType(Type* type) const {
    if (!type) return false;
    return type->kind == TypeKind::Struct || type->kind == TypeKind::Class || type->kind == TypeKind::Union;
}

std::string SemanticAnalyzer::getOperatorMangledName(BinaryOp op, Type* left, Type* right) {
    std::string opName;
    switch (op) {
        case BinaryOp::Add: opName = "operator+"; break;
        case BinaryOp::Sub: opName = "operator-"; break;
        case BinaryOp::Mul: opName = "operator*"; break;
        case BinaryOp::Div: opName = "operator/"; break;
        // SEM-17: converge the call-site set with the declaration-side set.
        case BinaryOp::Mod: opName = "operator%"; break;
        case BinaryOp::Eq: opName = "operator=="; break;
        case BinaryOp::NotEq: opName = "operator!="; break;
        case BinaryOp::Lt: opName = "operator<"; break;
        case BinaryOp::Gt: opName = "operator>"; break;
        case BinaryOp::Le: opName = "operator<="; break;
        case BinaryOp::Ge: opName = "operator>="; break;
        case BinaryOp::And: opName = "operator&&"; break;
        case BinaryOp::Or: opName = "operator||"; break;
        case BinaryOp::BitAnd: opName = "operator&"; break;
        case BinaryOp::BitOr: opName = "operator|"; break;
        case BinaryOp::BitXor: opName = "operator^"; break;
        case BinaryOp::LShift: opName = "operator<<"; break;
        case BinaryOp::RShift: opName = "operator>>"; break;
        default: return "";
    }
    return mangleFunction(opName, {left, right});
}

Type* SemanticAnalyzer::checkBinaryTypes(BinaryOp op, Type* left, Type* right, ExprAST& node) {
    if (!left || !right) return nullptr;

    switch (op) {
        case BinaryOp::Add:
        case BinaryOp::Sub:
        case BinaryOp::Mul:
        case BinaryOp::Div:
        case BinaryOp::Mod:
            if (isArithmeticType(left) && isArithmeticType(right)) {
                return getCommonType(left, right);
            }
            if (isPointerOrArray(left) && isIntegerType(right)) return left;
            if (isIntegerType(left) && isPointerOrArray(right)) return right;
            // 指针差 `p - q` → 元素个数（ptrdiff_t，映射 isize），C 语义。
            if (op == BinaryOp::Sub &&
                stripTypedef(left) && stripTypedef(left)->kind == TypeKind::Pointer &&
                stripTypedef(right) && stripTypedef(right)->kind == TypeKind::Pointer) {
                return typeCtx->getISize();
            }
            emitError("invalid operands to binary '" + binaryOpToString(op) + "': cannot apply '" 
                + binaryOpToString(op) + "' to '" + typeToString(left) + "' and '" + typeToString(right) + "'", node);
            return nullptr;

        case BinaryOp::Eq:
        case BinaryOp::NotEq:
        case BinaryOp::Lt:
        case BinaryOp::Gt:
        case BinaryOp::Le:
        case BinaryOp::Ge:
            // Arithmetic operands (including enums, via integer promotion) are
            // comparable even though enum <-> integer is not implicitly
            // *assignable* (TYP-20). Pointers compare against pointers/integers.
            if (isArithmeticType(left) && isArithmeticType(right)) {
                return typeCtx->getInt32();
            }
            // P1-06 (FMT-01): str compares only against str (byte compare);
            // mixed str/char* comparisons are rejected — view the str as
            // char* or convert explicitly. `string` does not compare (T4
            // owns no == semantics; a same-kind pass-through would emit
            // ICmp on a struct).
            {
                auto stripT = [](Type* t) {
                    while (t && t->kind == TypeKind::Typedef)
                        t = static_cast<TypedefType*>(t)->aliasedType;
                    return t;
                };
                Type* ls = stripT(left);
                Type* rs = stripT(right);
                if (ls->kind == TypeKind::String || rs->kind == TypeKind::String ||
                    (ls->kind == TypeKind::Str) != (rs->kind == TypeKind::Str)) {
                    emitError("comparison of incompatible types: '" + typeToString(left) + "' and '"
                        + typeToString(right) + "' with '" + binaryOpToString(op) + "'", node);
                    return nullptr;
                }
            }
            if (!typesCompatible(left, right)) {
                emitError("comparison of incompatible types: '" + typeToString(left) + "' and '" 
                    + typeToString(right) + "' with '" + binaryOpToString(op) + "'", node);
                return nullptr;
            }
            return typeCtx->getInt32();

        case BinaryOp::And:
        case BinaryOp::Or:
            // 评审 I1: logical operands must be scalar — Optional/Result
            // reaching `and`/`br` produced invalid IR.
            if (!isConditionType(left) || !isConditionType(right)) {
                emitError("logical operator '" + binaryOpToString(op) +
                              "' requires scalar operands, got '" +
                              typeToString(left) + "' and '" + typeToString(right) + "'",
                          node);
                return nullptr;
            }
            return typeCtx->getInt32();

        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::LShift:
        case BinaryOp::RShift:
            if (isIntegerType(left) && isIntegerType(right)) {
                return getCommonType(left, right);
            }
            emitError("bitwise '" + binaryOpToString(op) + "' applied to non-integer types: '" 
                + typeToString(left) + "' and '" + typeToString(right) + "'", node);
            return nullptr;

        default:
            return nullptr;
    }
}

Type* SemanticAnalyzer::checkAssignmentTypes(Type* lhs, Type* rhs, ExprAST& node) {
    if (!lhs || !rhs) return nullptr;

    Type* lhsRaw = lhs;
    Type* lhsS = stripTypedef(lhs);
    Type* rhsS = stripTypedef(rhs);

    if (lhsS->kind == TypeKind::Void || rhsS->kind == TypeKind::Void) {
        emitError("cannot assign to or from 'void' type", node);
        return nullptr;
    }

    // Enum types are strong (TYP-09 / TYP-20): assigning between an enum and an
    // integer (or between distinct enums) requires an explicit cast.
    if (lhsS->kind == TypeKind::Enum || rhsS->kind == TypeKind::Enum) {
        if (lhsS->kind == TypeKind::Enum && rhsS->kind == TypeKind::Enum &&
            static_cast<EnumType*>(lhsS)->name == static_cast<EnumType*>(rhsS)->name) {
            return lhsRaw;
        }
        emitError(DiagnosticCode::SemIncompatibleAssignment,
                  "cannot assign '" + typeToString(rhs) + "' to '" + typeToString(lhs) +
                      "' without an explicit conversion (TYP-20)", node);
        return nullptr;
    }

    if (isArithmeticType(lhsS) && isArithmeticType(rhsS)) return lhsRaw;
    // Arrays are non-modifiable lvalues (C99 6.5.16): assignment to an array
    // is rejected regardless of the RHS kind. Previously the generic
    // same-kind / pointer-or-array rules let `a = b` (even shape-mismatched)
    // and `a = p` through, where codegen stored a decayed pointer into the
    // array's storage — silent memory corruption. Copies go through brace
    // initializers, memcpy, or element assignment.
    if (lhsS->kind == TypeKind::Array) {
        emitError(DiagnosticCode::SemIncompatibleAssignment,
                  "cannot assign to array '" + typeToString(lhs) +
                      "': arrays are not modifiable lvalues (use memcpy or element assignment)",
                  node);
        return nullptr;
    }
    // TYP-12: slice targets must match by element type; arrays decay to
    // slice views (zero-copy). Tightens the generic same-kind rule that
    // silently accepted int32[] = float64[].
    if (lhsS->kind == TypeKind::Slice && rhsS->kind == TypeKind::Slice)
        return typesEqual(static_cast<SliceType*>(lhsS)->elementType,
                          static_cast<SliceType*>(rhsS)->elementType) ? lhsRaw : nullptr;
    if (lhsS->kind == TypeKind::Slice && rhsS->kind == TypeKind::Array)
        return typesEqual(static_cast<SliceType*>(lhsS)->elementType,
                          static_cast<ArrayType*>(rhsS)->elementType) ? lhsRaw : nullptr;
    if (lhsS->kind == rhsS->kind) {
        // P1-02 (TYP-13/14): Optional/Result are strict in their type
        // arguments — the generic same-kind rule would mis-accept
        // int32? = float64?.
        if (lhsS->kind == TypeKind::Optional || lhsS->kind == TypeKind::Result)
            return typesEqual(lhsS, rhsS) ? lhsRaw : nullptr;
        // PAR-12 (TYP-19 落点): 聚合是名义类型——struct/class/union 必须
        // 名字相同（typesEqual）才可赋值；两次独立的匿名定义名字不同，
        // 赋值拒绝（C 语义）。例外：派生→基 值赋值（INH-04 切片语义）。
        if (lhsS->kind == TypeKind::Struct || lhsS->kind == TypeKind::Class ||
            lhsS->kind == TypeKind::Union) {
            if (typesEqual(lhsS, rhsS)) return lhsRaw;
            if (isDerivedFrom(rhsS, lhsS)) return lhsRaw;
            emitError(DiagnosticCode::SemIncompatibleAssignment,
                      "cannot assign '" + typeToString(rhs) + "' to '" +
                          typeToString(lhs) + "': distinct types",
                      node);
            return nullptr;
        }
        return lhsRaw;
    }
    if (isPointerOrArray(lhsS) && isPointerOrArray(rhsS)) return lhsRaw;
    if (isPointerOrArray(lhsS) && isIntegerType(rhsS)) return lhsRaw;
    // P1-06 (FMT-03): str -> char* implicit byte view (drops the UTF-8
    // guarantee and the length); char* -> str is NOT implicit (use
    // str_from_c). Targets of type str are same-kind (above).
    if (isConstCharPointer(lhsS) &&
        (rhsS->kind == TypeKind::Str || rhsS->kind == TypeKind::String))
        return lhsRaw;
    // P1-06 (FMT-02): string -> str implicit view. The reverse direction is
    // rejected below with the DEC-20 guidance (string.new).
    if (lhsS->kind == TypeKind::Str && rhsS->kind == TypeKind::String)
        return lhsRaw;

    // P1-06 (DEC-20): no implicit heap allocation — str/char* values do not
    // become `string`; construction is explicit.
    if (lhsS->kind == TypeKind::String &&
        (rhsS->kind == TypeKind::Str ||
         (rhsS->kind == TypeKind::Pointer && rhsS->base &&
          rhsS->base->kind == TypeKind::Char))) {
        emitError(DiagnosticCode::SemIncompatibleAssignment,
                  "incompatible types in assignment: cannot assign '" +
                      typeToString(rhs) + "' to 'string' implicitly; construct explicitly with string.new(...)",
                  node);
        return nullptr;
    }

    emitError(DiagnosticCode::SemIncompatibleAssignment,
              "incompatible types in assignment: cannot assign '" + typeToString(rhs) +
                  "' to '" + typeToString(lhs) + "'", node);
    return nullptr;
}

Type* SemanticAnalyzer::checkFunctionCall(const std::string& name, const std::vector<std::unique_ptr<ExprAST>>& args, ExprAST& node, FunctionType** outFuncType) {
    OverloadSet* overloadSet = currentScope->lookupOverload(name);
    if (!overloadSet || overloadSet->empty()) {
        emitError(DiagnosticCode::SemUnresolvedCall, "use of undeclared function '" + name + "'", node);
        return nullptr;
    }

    // Collect argument types
    std::vector<Type*> argTypes;
    for (auto& arg : args) {
        Type* argType = getExprType(*arg);
        argTypes.push_back(argType);
    }

    // Resolve overload
    Symbol* resolved = overloadSet->resolve(argTypes);
    if (!resolved) {
        // Distinguish "no viable candidate" from "ambiguous".
        bool anyViable = false;
        for (auto* sym : overloadSet->getCandidates()) {
            if (sym->type->kind != TypeKind::Function) continue;
            auto* funcType = static_cast<FunctionType*>(sym->type);
            size_t fixedCount = funcType->paramTypes.size();
            size_t defaultCount = sym->decl ? static_cast<size_t>(sym->decl->defaultArgCount) : 0;
            size_t minArgs = fixedCount > defaultCount ? fixedCount - defaultCount : 0;
            if (funcType->isVarArg) {
                if (argTypes.size() < minArgs) continue;
            } else if (argTypes.size() < minArgs || argTypes.size() > fixedCount) {
                continue;
            }
            bool viable = true;
            size_t checked = std::min(argTypes.size(), fixedCount);
            for (size_t i = 0; i < checked; ++i) {
                if (conversionRank(argTypes[i], funcType->paramTypes[i]) < 0) {
                    viable = false;
                    break;
                }
            }
            if (viable) {
                anyViable = true;
                break;
            }
        }
        if (anyViable) {
            emitError(DiagnosticCode::SemAmbiguousCall, "ambiguous call to overloaded function '" + name + "'", node);
        } else {
            emitError(DiagnosticCode::SemUnresolvedCall, "no matching function for call to '" + name + "'", node);
        }
        return nullptr;
    }

    if (resolved->type->kind != TypeKind::Function) {
        emitError("cannot call non-function '" + name + "' (type: " + typeToString(resolved->type) + ")", node);
        return nullptr;
    }

    FunctionType* funcType = static_cast<FunctionType*>(resolved->type);
    size_t defaultCount = resolved->decl ? static_cast<size_t>(resolved->decl->defaultArgCount) : 0;
    size_t minArgs = funcType->paramTypes.size() > defaultCount
                         ? funcType->paramTypes.size() - defaultCount : 0;
    if (!funcType->isVarArg &&
        (args.size() > funcType->paramTypes.size() || args.size() < minArgs)) {
        emitError("wrong number of arguments to function '" + name + "': expected "
            + std::to_string(minArgs) + ".." + std::to_string(funcType->paramTypes.size())
            + ", got " + std::to_string(args.size()), node);
        return nullptr;
    }

    // FUN-09: stash default-argument values on the call node for codegen.
    if (args.size() < funcType->paramTypes.size()) {
        if (auto* call = dynamic_cast<CallExprAST*>(&node)) {
            if (resolved->decl) {
                for (size_t i = args.size(); i < funcType->paramTypes.size(); ++i) {
                    DefaultArgValue dv;
                    if (auto* def = resolved->decl->params[i]->defaultValue.get()) {
                        auto v = evaluateConstexpr(def);
                        if (v) {
                            switch (v->type) {
                                case ConstValue::INT:   dv.isInt = true; dv.intVal = v->intVal; break;
                                case ConstValue::DOUBLE: dv.isDouble = true; dv.doubleVal = v->doubleVal; break;
                                case ConstValue::CHAR:   dv.isChar = true; dv.charVal = v->charVal; break;
                                default: break;
                            }
                        }
                    }
                    call->defaultArgValues.push_back(dv);
                }
            }
        }
    }

    if (outFuncType) *outFuncType = funcType;
    return funcType->returnType;
}

Type* SemanticAnalyzer::typeForLiteralKind(LiteralKind kind) {
    // LEX-15: suffix/default -> fixed-width base type.
    switch (kind) {
        case LiteralKind::Int:      return typeCtx->getInt32();
        case LiteralKind::UInt:     return typeCtx->getUInt32();
        case LiteralKind::Long:     return typeCtx->getInt64();
        case LiteralKind::ULong:    return typeCtx->getUInt64();
        case LiteralKind::Float16:  return typeCtx->getFloat16();
        case LiteralKind::Float32:  return typeCtx->getFloat32();
        case LiteralKind::Float64:  return typeCtx->getFloat64();
        case LiteralKind::Float128: return typeCtx->getFloat128();
        case LiteralKind::None:     break;
    }
    return nullptr;
}

Type* SemanticAnalyzer::getExprType(ExprAST& expr) {
    visit(expr);
    return expr.type;
}

// ---- P1-04 / CT-04/05/12: compile_time 特判 ----

CompileTimeEvaluator& SemanticAnalyzer::ctEval() {
    if (!m_ctEval) {
        m_ctEval = std::make_unique<CompileTimeEvaluator>(*this);
    }
    return *m_ctEval;
}

bool SemanticAnalyzer::isCompileTimeRoot(const ExprAST* expr) {
    // 沿 MemberAccess 对象链下行，根为 VariableExpr("compile_time") 即真。
    while (expr) {
        if (auto* ma = dynamic_cast<const MemberAccessExprAST*>(expr)) {
            expr = ma->object.get();
            continue;
        }
        if (auto* var = dynamic_cast<const VariableExprAST*>(expr)) {
            if (var->name != "compile_time") return false;
            // P1-04 评审 I4（spec §1 消歧）：作用域内已声明的同名变量优先，
            // 其成员访问走普通路径，不被编译期命名空间劫持。
            if (currentScope && currentScope->lookup("compile_time")) return false;
            return true;
        }
        return false;
    }
    return false;
}

bool SemanticAnalyzer::containsCompileTimeRoot(const ExprAST* expr) {
    if (!expr) return false;
    if (isCompileTimeRoot(expr)) return true;
    if (auto* bin = dynamic_cast<const BinaryExprAST*>(expr)) {
        return containsCompileTimeRoot(bin->left.get())
            || containsCompileTimeRoot(bin->right.get());
    }
    if (auto* un = dynamic_cast<const UnaryExprAST*>(expr)) {
        return containsCompileTimeRoot(un->operand.get());
    }
    if (auto* ter = dynamic_cast<const TernaryExprAST*>(expr)) {
        return containsCompileTimeRoot(ter->cond.get())
            || containsCompileTimeRoot(ter->then.get())
            || containsCompileTimeRoot(ter->elseExpr.get());
    }
    if (auto* mc = dynamic_cast<const MethodCallExprAST*>(expr)) {
        if (containsCompileTimeRoot(mc->object.get())) return true;
        for (const auto& a : mc->args) {
            if (containsCompileTimeRoot(a.get())) return true;
        }
        return false;
    }
    if (auto* ma = dynamic_cast<const MemberAccessExprAST*>(expr)) {
        return containsCompileTimeRoot(ma->object.get());
    }
    if (auto* call = dynamic_cast<const CallExprAST*>(expr)) {
        for (const auto& a : call->args) {
            if (containsCompileTimeRoot(a.get())) return true;
        }
    }
    return false;
}

// INT 结果的节点类型映射：按值域 int32/int64（spec §4）。
// CTX-01: unsigned limits (uint_max) keep their unsigned type.
static Type* ctIntTypeFor(TypeContext* typeCtx, long long v,
                          bool isUnsigned = false) {
    if (isUnsigned) return typeCtx->getUInt64();
    return (v < -2147483648LL || v > 2147483647LL)
        ? typeCtx->getInt64() : typeCtx->getInt32();
}

// CTX-01: scalar boolean queries. Keeping their AST type bool lets users
// assign the result to a bool parameter without an extra cast.
static bool ctBoolQuery(const std::string& name) {
    return name == "is_same" || name == "is_integer" || name == "is_float" ||
           name == "is_bool" || name == "is_pointer" || name == "is_array" ||
           name == "is_aggregate" || name == "is_class" || name == "is_struct" ||
           name == "is_union" || name == "is_enum" || name == "is_void" ||
           name == "is_signed" || name == "is_convertible" ||
           name == "is_assignable" || name == "has_field" ||
           name == "has_attribute" || name == "field_is_bitfield" ||
           name == "has_method" || name == "is_base_of" || name == "is" ||
           name == "at_least" ||
           name == "has_feature" || name == "debug";
}

// CTX-01: human-readable value for assertion diagnostics.
static std::string ctValueToString(const CompileTimeEvaluator::ConstValue& v) {
    switch (v.type) {
        case CompileTimeEvaluator::ConstValue::INT:
            return std::to_string(v.intVal);
        case CompileTimeEvaluator::ConstValue::DOUBLE: {
            std::ostringstream os;
            os << v.doubleVal;
            return os.str();
        }
        case CompileTimeEvaluator::ConstValue::CHAR:
            return std::string("'") + v.charVal + "'";
        case CompileTimeEvaluator::ConstValue::STR:
            return "\"" + v.strVal + "\"";
        case CompileTimeEvaluator::ConstValue::TYPE:
            return "<type>";
    }
    return "?";
}

// P1-04 评审 I3: 沿 typedef/指针/数组包装层检查毒化标志，返回毒化层
// 的可读名字（聚合名或 typedef 名）。
static bool ctIsPoisonedType(Type* t, std::string& name) {
    bool poisoned = false;
    while (t) {
        if (t->ctDeadBranch) {
            poisoned = true;
            if (name.empty()) {
                switch (t->kind) {
                    case TypeKind::Struct:
                    case TypeKind::Class: name = static_cast<StructType*>(t)->name; break;
                    case TypeKind::Union: name = static_cast<UnionType*>(t)->name; break;
                    case TypeKind::Enum:  name = static_cast<EnumType*>(t)->name; break;
                    case TypeKind::Typedef: name = static_cast<TypedefType*>(t)->name; break;
                    default: break;
                }
            }
        }
        switch (t->kind) {
            case TypeKind::Typedef: t = static_cast<TypedefType*>(t)->aliasedType; continue;
            case TypeKind::Pointer: t = t->base; continue;
            case TypeKind::Array:   t = static_cast<ArrayType*>(t)->elementType; continue;
            default: return poisoned;
        }
    }
    return poisoned;
}

// P1-05 / ANN-05: 沿 typedef/指针/数组层检查弃用标志。
void SemanticAnalyzer::checkDeprecatedTypeUse(Type* t, ASTNode& at) {
    std::string name;
    while (t) {
        if (t->deprecated) {
            if (t->deprecatedMsg.empty()) {
                emitWarning(DiagnosticCode::WarnDeprecated,
                            "'" + name + "' is deprecated", at);
            } else {
                emitWarning(DiagnosticCode::WarnDeprecated,
                            "'" + name + "' is deprecated: " + t->deprecatedMsg, at);
            }
            return;
        }
        switch (t->kind) {
            case TypeKind::Typedef:
                name = static_cast<TypedefType*>(t)->name;
                t = static_cast<TypedefType*>(t)->aliasedType;
                continue;
            case TypeKind::Pointer:
                t = t->base;
                continue;
            case TypeKind::Array:
                t = static_cast<ArrayType*>(t)->elementType;
                continue;
            case TypeKind::Struct:
            case TypeKind::Class:
                name = static_cast<StructType*>(t)->name;
                return;
            case TypeKind::Union:
                name = static_cast<UnionType*>(t)->name;
                return;
            case TypeKind::Enum:
                name = static_cast<EnumType*>(t)->name;
                return;
            default:
                return;
        }
    }
}

void SemanticAnalyzer::checkCtDeadBranchUse(Type* t, ASTNode& at) {
    if (!t) return;
    std::string name;
    if (!ctIsPoisonedType(t, name)) return;
    if (!name.empty()) {
        emitError("use of type '" + name + "' from a non-selected compile_time.if branch", at);
    } else {
        emitError("use of a type declared in a non-selected compile_time.if branch", at);
    }
}

// P1-05 / ANN-02/03: [[align(N)]] 实参折叠（validateAnnotations 已保证常量
// 且 2 的幂；此处仅取值）。
static uint64_t ctAlignArg(const Annotation& ann, SemanticAnalyzer& sema) {
    if (ann.args.size() == 1 && ann.args[0].kind == AnnotationArg::Kind::Expr
        && ann.args[0].expr) {
        if (auto v = sema.evaluateConstexpr(ann.args[0].expr.get());
            v && v->type == SemanticAnalyzer::ConstValue::INT) {
            return static_cast<uint64_t>(v->intVal);
        }
    }
    return 0;
}

// P1-05 / ANN-06: 注解验证——未知名/重复/目标/实参（文案 spec §5 逐字）。
void SemanticAnalyzer::validateAnnotations(const std::vector<Annotation>& anns,
                                           ASTNode& at, const std::string& target,
                                           Type* paramType) {
    static const std::unordered_set<std::string> kKnown = {
        "repr", "packed", "align", "inline", "cold", "nonnull", "deprecated"};
    std::unordered_set<std::string> seen;
    for (const auto& ann : anns) {
        if (!kKnown.count(ann.name)) {
            emitError(DiagnosticCode::SemUnknownAnnotation,
                      "unknown annotation '" + ann.name + "'", at);
            continue;
        }
        if (!seen.insert(ann.name).second) {
            emitError(DiagnosticCode::SemDuplicateAnnotation,
                      "duplicate annotation '" + ann.name + "'", at);
            continue;
        }
        bool ok = false;
        if (ann.name == "repr") ok = (target == "struct" || target == "union");
        // 评审 I5: 带方法的聚合路由为 "class" 目标——packed/align 同样合法
        // （LayoutBuilder 对 ClassType 一致处理）。
        else if (ann.name == "packed") ok = (target == "struct" || target == "class" || target == "union" || target == "field");
        else if (ann.name == "align") ok = (target == "struct" || target == "class" || target == "union" || target == "field" || target == "variable");
        else if (ann.name == "inline" || ann.name == "cold") ok = (target == "function");
        else if (ann.name == "nonnull") ok = (target == "parameter");
        else if (ann.name == "deprecated") ok = true;
        if (!ok) {
            emitError(DiagnosticCode::SemInvalidAnnotationTarget,
                      "annotation '" + ann.name + "' is not valid on " + target, at);
            continue;
        }
        if (ann.name == "repr") {
            // 实参须为 Ident "C"（E2011 码位，文案独立——评审可读性）。
            if (ann.args.size() != 1 || ann.args[0].kind != AnnotationArg::Kind::Ident
                || ann.args[0].text != "C") {
                emitError(DiagnosticCode::SemInvalidAnnotationTarget,
                          "repr argument must be 'C'", at);
            }
        }
        if (ann.name == "nonnull" && target == "parameter") {
            Type* t = paramType;
            while (t && t->kind == TypeKind::Typedef) {
                t = static_cast<TypedefType*>(t)->aliasedType;
            }
            if (!t || t->kind != TypeKind::Pointer) {
                emitError(DiagnosticCode::SemInvalidAnnotationTarget,
                          "annotation 'nonnull' is not valid on parameter", at);
            }
        }
        if (ann.name == "align") {
            if (ann.args.size() != 1 || ann.args[0].kind != AnnotationArg::Kind::Expr
                || !ann.args[0].expr) {
                emitError(DiagnosticCode::SemAnnotationArgNotConstant,
                          "annotation argument must be a compile-time constant", at);
                continue;
            }
            auto v = evaluateConstexpr(ann.args[0].expr.get());
            long long n = 0;
            bool isConst = false;
            if (v && v->type == ConstValue::INT) {
                n = v->intVal;
                isConst = true;
            } else if (v && v->type == ConstValue::DOUBLE) {
                n = static_cast<long long>(v->doubleVal);
                isConst = true;
            }
            if (!isConst) {
                emitError(DiagnosticCode::SemAnnotationArgNotConstant,
                          "annotation argument must be a compile-time constant", at);
                continue;
            }
            if (n < 1 || (n & (n - 1)) != 0) {
                emitError(DiagnosticCode::SemAlignNotPowerOfTwo,
                          "align argument must be a power of two", at);
            }
        }
    }
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evalCompileTime(ExprAST* expr, ASTNode& at) {
    return ctEval().eval(expr, at);
}

bool SemanticAnalyzer::tryAnalyzeCompileTimeChain(MemberAccessExprAST& node) {
    if (!isCompileTimeRoot(node.object.get())) {
        return false;
    }
    // 根链成员名序列（e.g. compile_time.target.os → [target, os]）。
    std::vector<std::string> members;
    {
        std::vector<const MemberAccessExprAST*> chain;
        const ExprAST* cur = &node;
        while (auto* ma = dynamic_cast<const MemberAccessExprAST*>(cur)) {
            chain.push_back(ma);
            cur = ma->object.get();
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            members.push_back((*it)->memberName);
        }
    }
    node.ctHandled = true;
    node.isLValue = false;
    auto v = ctEval().eval(&node, node);
    if (!v) {
        node.type = nullptr; // 未知成员等诊断已由求值器发出
        return true;
    }
    switch (v->type) {
        case ConstValue::INT:
            node.ctInt = v->intVal;
            // 成员值类型映射（spec §4）：bool 查询 → bool，无符号极限 →
            // uint64，其余按值域。
            if (!members.empty() && ctBoolQuery(members.back())) {
                node.type = typeCtx->getBool();
            } else if (v->isUnsigned) {
                node.type = typeCtx->getUInt64();
            } else {
                node.type = ctIntTypeFor(typeCtx, v->intVal);
            }
            break;
        case ConstValue::DOUBLE:
            node.ctFloat = v->doubleVal;
            node.type = typeCtx->getFloat64();
            break;
        case ConstValue::CHAR:
            // CHAR 有运行时类型——防御回退（评审 I2）。
            node.type = nullptr;
            break;
        case ConstValue::TYPE:
            emitError("compile-time type value cannot be used in runtime context", node);
            node.type = nullptr;
            break;
        default: // STR
            // STR 逃逸：编译期上下文（static_assert/if/constexpr 初始化）
            // 整树由求值器接管、不经本钩子——凡到钩子必是运行时上下文。
            emitError("compile-time string value cannot be used in runtime context", node);
            node.type = nullptr;
            break;
    }
    return true;
}

bool SemanticAnalyzer::tryAnalyzeCompileTimeCall(MethodCallExprAST& node) {
    auto* obj = dynamic_cast<VariableExprAST*>(node.object.get());
    if (!obj || obj->name != "compile_time") {
        return false;
    }
    node.ctHandled = true;
    node.isLValue = false;

    // P1-04 / CT-02: compile_time.static_assert(cond[, msg])。
    if (node.methodName == "static_assert") {
        if (node.args.empty() || node.args.size() > 2) {
            emitError("compile_time.static_assert requires (condition) or (condition, message)", node);
            node.type = nullptr;
            return true;
        }
        auto cond = ctEval().eval(node.args[0].get(), node);
        if (!cond || cond->type != ConstValue::INT) {
            emitError("compile_time argument must be a compile-time constant", node);
            node.type = nullptr;
            return true;
        }
        if (cond->intVal == 0) {
            std::string msg = "static_assert failed";
            if (node.args.size() == 2) {
                if (auto cv = ctEval().eval(node.args[1].get(), node);
                    cv && cv->type == ConstValue::STR) {
                    msg += ": " + cv->strVal;
                }
            }
            emitError(msg, node);
        }
        node.type = nullptr;
        return true;
    }

    // CTX-01: compile-time diagnostics.
    if (node.methodName == "fail" || node.methodName == "warn" ||
        node.methodName == "debug") {
        if (node.args.size() != 1) {
            emitError("compile_time." + node.methodName + " requires exactly one message", node);
            node.type = nullptr;
            return true;
        }
        auto msg = ctEval().eval(node.args[0].get(), node);
        if (!msg || msg->type != ConstValue::STR) {
            emitError("compile_time." + node.methodName + " message must be a compile-time string",
                      node);
            node.type = nullptr;
            return true;
        }
        if (node.methodName == "fail") {
            emitError("compile_time.fail: " + msg->strVal, node);
        } else if (node.methodName == "warn") {
            emitWarning("compile_time.warn: " + msg->strVal, node);
        } else if (m_ctBuildDebug) {
            emitWarning("compile_time.debug: " + msg->strVal, node);
        }
        node.type = nullptr;
        return true;
    }

    // CTX-01: assertion helpers. They are sema-only and emit no runtime code.
    if (node.methodName == "assert_eq" || node.methodName == "assert_ne") {
        if (node.args.size() != 2) {
            emitError("compile_time." + node.methodName + " requires (left, right)", node);
            node.type = nullptr;
            return true;
        }
        auto left = ctEval().eval(node.args[0].get(), node);
        auto right = ctEval().eval(node.args[1].get(), node);
        if (!left || !right) {
            emitError("compile_time." + node.methodName +
                          " arguments must be compile-time constants",
                      node);
            node.type = nullptr;
            return true;
        }
        bool equal = left->type == right->type;
        if (equal) {
            switch (left->type) {
                case ConstValue::INT: equal = left->intVal == right->intVal; break;
                case ConstValue::DOUBLE: equal = left->doubleVal == right->doubleVal; break;
                case ConstValue::CHAR: equal = left->charVal == right->charVal; break;
                case ConstValue::STR: equal = left->strVal == right->strVal; break;
                case ConstValue::TYPE:
                    equal = left->typeVal && right->typeVal &&
                            typeToString(left->typeVal) == typeToString(right->typeVal);
                    break;
            }
        }
        const bool wantEqual = node.methodName == "assert_eq";
        if (equal != wantEqual) {
            emitError("compile_time." + node.methodName + " failed: " +
                          ctValueToString(*left) + " vs " + ctValueToString(*right),
                      node);
        }
        node.type = nullptr;
        return true;
    }
    if (node.methodName == "assert_size" || node.methodName == "assert_align" ||
        node.methodName == "assert_offset") {
        const bool isOffset = node.methodName == "assert_offset";
        const size_t expectedArgs = isOffset ? 3 : 2;
        if (node.args.size() != expectedArgs) {
            emitError("compile_time." + node.methodName + " has the wrong argument count", node);
            node.type = nullptr;
            return true;
        }
        Type* argType = nullptr;
        if (auto* sized = dynamic_cast<SizeofExprAST*>(node.args[0].get()))
            argType = sized->sizeofType;
        else if (auto* name = dynamic_cast<VariableExprAST*>(node.args[0].get()))
            argType = resolveTypeByName(name->name);
        if (!argType) {
            emitError("unknown type in compile_time." + node.methodName, node);
            node.type = nullptr;
            return true;
        }
        std::string field;
        if (isOffset) {
            if (auto* s = dynamic_cast<StringExprAST*>(node.args[1].get())) field = s->value;
            else if (auto* v = dynamic_cast<VariableExprAST*>(node.args[1].get())) field = v->name;
            else {
                emitError("compile_time.assert_offset field must be a string literal", node);
                node.type = nullptr;
                return true;
            }
        }
        auto expected = ctEval().eval(node.args[expectedArgs - 1].get(), node);
        if (!expected || expected->type != ConstValue::INT) {
            emitError("compile_time." + node.methodName +
                          " expected value must be a compile-time integer",
                      node);
            node.type = nullptr;
            return true;
        }
        auto layoutCall = std::make_unique<MethodCallExprAST>(
            std::make_unique<VariableExprAST>("compile_time"),
            isOffset ? "offset_of" : (node.methodName == "assert_size" ? "size_of" : "align_of"),
            std::vector<std::unique_ptr<ExprAST>>{});
        layoutCall->args.push_back(std::make_unique<SizeofExprAST>(argType, nullptr));
        if (isOffset) layoutCall->args.push_back(std::make_unique<StringExprAST>(field));
        auto actual = ctEval().eval(layoutCall.get(), node);
        if (!actual || actual->type != ConstValue::INT) {
            node.type = nullptr;
            return true;
        }
        if (actual->intVal != expected->intVal) {
            emitError("compile_time." + node.methodName + " failed for '" +
                          typeToString(argType) + "': expected " +
                          std::to_string(expected->intVal) + ", got " +
                          std::to_string(actual->intVal),
                      node);
        }
        node.type = nullptr;
        return true;
    }

    // CT-07: compile_time reflection — name_of / fields_of / attributes_of.
    if (node.methodName == "name_of" || node.methodName == "fields_of" ||
        node.methodName == "attributes_of") {
        return analyzeReflectionCall(node);
    }

    auto v = ctEval().eval(&node, node);
    if (v && v->type == ConstValue::INT) {
        node.ctInt = v->intVal;
        if (ctBoolQuery(node.methodName)) {
            node.type = typeCtx->getBool();
        } else if (v->isUnsigned) {
            node.type = typeCtx->getUInt64();
        } else {
            node.type = ctIntTypeFor(typeCtx, v->intVal);
        }
    } else if (v && v->type == ConstValue::DOUBLE) {
        node.ctFloat = v->doubleVal;
        node.type = typeCtx->getFloat64();
    } else if (v && v->type == ConstValue::STR) {
        // STR 结果逃逸（理由同链形钩子）。
        emitError("compile-time string value cannot be used in runtime context", node);
        node.type = nullptr;
    } else if (v && v->type == ConstValue::TYPE) {
        emitError("compile-time type value cannot be used in runtime context", node);
        node.type = nullptr;
    } else if (v) {
        // CHAR：有运行时类型，防御回退（评审 I2）。
        node.type = nullptr;
    } else {
        // static_assert/size_of 族/if 由 CT-02/CT-06 后续任务接管；此处
        // type=nullptr，外层类型检查兜底。
        node.type = nullptr;
    }
    return true;
}

// CT-07: compile_time.name_of/fields_of/attributes_of(T).
bool SemanticAnalyzer::analyzeReflectionCall(MethodCallExprAST& node) {
    node.ctHandled = true;
    node.isLValue = false;

    if (node.args.size() != 1) {
        emitError("compile_time." + node.methodName + " requires exactly one type argument", node);
        node.type = nullptr;
        return true;
    }
    Type* targetType = nullptr;
    std::string targetName;
    if (auto* sized = dynamic_cast<SizeofExprAST*>(node.args[0].get())) {
        targetType = sized->sizeofType;
    } else if (auto* name = dynamic_cast<VariableExprAST*>(node.args[0].get())) {
        targetName = name->name;
        targetType = resolveTypeByName(name->name);
    }
    if (!targetType) {
        emitError("unknown type '" + (targetName.empty() ? "(unnamed)" : targetName) +
                      "' in compile_time expression", node);
        node.type = nullptr;
        return true;
    }

    std::string key = typeToMangled(targetType);
    auto& registry = smc::ReflectionRegistry::instance();

    if (node.methodName == "name_of") {
        registry.registerName(key, typeToString(targetType));
        node.reflectKind = MethodCallExprAST::ReflectionKind::NameOf;
        node.reflectType = targetType;
        node.type = typeCtx->getStrType();
        return true;
    }

    if (node.methodName == "fields_of") {
        Type* raw = targetType;
        while (raw && raw->kind == TypeKind::Typedef)
            raw = static_cast<TypedefType*>(raw)->aliasedType;
        if (!raw || (raw->kind != TypeKind::Struct && raw->kind != TypeKind::Class &&
                     raw->kind != TypeKind::Union)) {
            emitError(DiagnosticCode::SemReflectNonAggregate,
                      "compile_time.fields_of requires an aggregate type (got '" +
                          typeToString(targetType) + "')",
                      node);
            node.type = nullptr;
            return true;
        }
        auto fields = ctEval().evalReflectFields(targetType, node);
        std::vector<smc::FieldInfoData> data;
        for (auto& f : fields) data.push_back({f.name, f.offset, f.size});
        registry.registerFields(key, std::move(data));
        node.reflectKind = MethodCallExprAST::ReflectionKind::FieldsOf;
        node.reflectType = targetType;
        node.type = typeCtx->getSliceType(typeCtx->getFieldInfoType());
        return true;
    }

    if (node.methodName == "attributes_of") {
        std::vector<smc::AttributeData> attrs;
        if (targetType->deprecated) attrs.push_back({"deprecated"});
        registry.registerAttributes(key, std::move(attrs));
        node.reflectKind = MethodCallExprAST::ReflectionKind::AttributesOf;
        node.reflectType = targetType;
        node.type = typeCtx->getSliceType(typeCtx->getAttributeType());
        return true;
    }

    emitError("unknown compile_time member '" + node.methodName + "'", node);
    node.type = nullptr;
    return true;
}
std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evalConstexprCallCT(CallExprAST& call,
                                      const std::vector<ConstValue>& argValues) {
    std::string calleeName = call.callee;
    auto it = constexprFunctions.find(calleeName);
    if (it == constexprFunctions.end()) {
        calleeName = resolveNamespaceName(calleeName);
        it = constexprFunctions.find(calleeName);
    }
    if (it == constexprFunctions.end()) {
        return std::nullopt;
    }
    FunctionDeclAST* fn = it->second;
    if (!fn->body || argValues.size() != fn->params.size()) {
        return std::nullopt;
    }
    ConstEnv env = activeEnv ? *activeEnv : ConstEnv{};
    for (size_t i = 0; i < fn->params.size(); ++i) {
        env[fn->params[i]->name] = ctClone(argValues[i]);
    }
    ConstEnv* saved = activeEnv;
    activeEnv = &env;
    if (ctEvalDepth == 0) {
        ctSteps = 0;
        ctStepReported = false;
    }
    constexprCallDepth++;
    CtFlow flow = CtFlow::Normal;
    auto result = evalConstexprStmt(fn->body.get(), env, 1, flow);
    constexprCallDepth--;
    activeEnv = saved;
    return flow == CtFlow::Return ? result : std::nullopt;
}

bool SemanticAnalyzer::ctStepAllowance(ASTNode& at) {
    if (++ctSteps <= kConstexprStepLimit) {
        return true;
    }
    if (!ctStepReported) {
        ctStepReported = true;
        emitError("compile-time evaluation step limit exceeded (1000000)", at);
    }
    return false;
}

std::optional<SemanticAnalyzer::ConstValue> SemanticAnalyzer::evaluateConstexpr(ExprAST* expr) {
    if (!expr) return std::nullopt;

    struct DepthGuard {
        int& depth;
        ~DepthGuard() { --depth; }
    } guard{ctEvalDepth};
    if (ctEvalDepth++ == 0) {
        ctSteps = 0;
        ctStepReported = false;
    }
    if (!ctStepAllowance(*expr)) {
        return std::nullopt;
    }

    // P1-04 / CT-12: 表达式树含 compile_time 根 → 整树委托编译期求值器
    // （含 target/build 值与字符串运算；DEC-05 共享内核由此对接）。
    if (containsCompileTimeRoot(expr)) {
        return evalCompileTime(expr, *expr);
    }

    if (auto* num = dynamic_cast<NumberExprAST*>(expr)) {
        unsigned bits = 32;
        bool isUnsigned = false;
        switch (num->literalKind) {
            case LiteralKind::UInt: bits = 32; isUnsigned = true; break;
            case LiteralKind::Long: bits = 64; isUnsigned = false; break;
            case LiteralKind::ULong: bits = 64; isUnsigned = true; break;
            case LiteralKind::Int:
                bits = (num->value >= INT32_MIN && num->value <= INT32_MAX) ? 32 : 64;
                break;
            default: break;
        }
        return ctInt(num->value, bits, isUnsigned);
    }

    // Found issue: bool 字面量按整数（0/1）参与 constexpr 折叠。
    if (auto* bl = dynamic_cast<BoolLiteralExprAST*>(expr)) {
        return ctInt(bl->value ? 1 : 0, 1, true, true);
    }

    if (auto* chr = dynamic_cast<CharExprAST*>(expr)) {
        ConstValue cv;
        cv.type = ConstValue::CHAR;
        cv.charVal = chr->value;
        return cv;
    }

    if (auto* flt = dynamic_cast<FloatExprAST*>(expr)) {
        ConstValue cv;
        cv.type = ConstValue::DOUBLE;
        cv.doubleVal = flt->value;
        return cv;
    }

    if (auto* assign = dynamic_cast<AssignmentExprAST*>(expr)) {
        if (!activeEnv) return std::nullopt;
        auto* var = dynamic_cast<VariableExprAST*>(assign->lhs.get());
        if (!var) return std::nullopt;
        auto slot = activeEnv->find(var->name);
        if (slot == activeEnv->end()) return std::nullopt;

        auto rhs = evaluateConstexpr(assign->rhs.get());
        if (!rhs) return std::nullopt;

        if (assign->op == AssignOp::Assign) {
            slot->second = ctClone(*rhs);
            return slot->second;
        }

        BinaryOp op = BinaryOp::Invalid;
        switch (assign->op) {
            case AssignOp::AddAssign: op = BinaryOp::Add; break;
            case AssignOp::SubAssign: op = BinaryOp::Sub; break;
            case AssignOp::MulAssign: op = BinaryOp::Mul; break;
            case AssignOp::DivAssign: op = BinaryOp::Div; break;
            case AssignOp::ModAssign: op = BinaryOp::Mod; break;
            case AssignOp::BitAndAssign: op = BinaryOp::BitAnd; break;
            case AssignOp::BitOrAssign: op = BinaryOp::BitOr; break;
            case AssignOp::BitXorAssign: op = BinaryOp::BitXor; break;
            case AssignOp::LShiftAssign: op = BinaryOp::LShift; break;
            case AssignOp::RShiftAssign: op = BinaryOp::RShift; break;
            default: return std::nullopt;
        }
        auto folded = ctBinary(slot->second, *rhs, static_cast<int>(op),
                               assign->lhs->type);
        if (!folded) return std::nullopt;
        slot->second = *folded;
        return slot->second;
    }

    if (auto* post = dynamic_cast<PostfixIncDecExprAST*>(expr)) {
        if (!activeEnv) return std::nullopt;
        auto* var = dynamic_cast<VariableExprAST*>(post->operand.get());
        if (!var) return std::nullopt;
        auto slot = activeEnv->find(var->name);
        if (slot == activeEnv->end()) return std::nullopt;
        ConstValue old = slot->second;
        auto delta = ctInt(1, slot->second.bits, slot->second.isUnsigned);
        auto folded = ctBinary(slot->second, delta,
                               static_cast<int>(post->isIncrement ? BinaryOp::Add
                                                                  : BinaryOp::Sub),
                               post->operand->type);
        if (!folded) return std::nullopt;
        slot->second = *folded;
        return old;
    }

    if (auto* bin = dynamic_cast<BinaryExprAST*>(expr)) {
        auto left = evaluateConstexpr(bin->left.get());
        if (!left) return std::nullopt;
        if (bin->op == BinaryOp::And || bin->op == BinaryOp::Or) {
            const bool truth = ctTruthy(*left);
            if ((bin->op == BinaryOp::And && !truth) ||
                (bin->op == BinaryOp::Or && truth)) {
                return ctInt(truth ? 1 : 0, 1, true, true);
            }
        }
        auto right = evaluateConstexpr(bin->right.get());
        if (!right) return std::nullopt;
        return ctBinary(*left, *right, static_cast<int>(bin->op), bin->type);
    }

    if (auto* unary = dynamic_cast<UnaryExprAST*>(expr)) {
        auto operand = evaluateConstexpr(unary->operand.get());
        if (!operand) return std::nullopt;

        if (unary->op == UnaryOp::PreInc || unary->op == UnaryOp::PreDec) {
            if (!activeEnv) return std::nullopt;
            auto* var = dynamic_cast<VariableExprAST*>(unary->operand.get());
            if (!var) return std::nullopt;
            auto slot = activeEnv->find(var->name);
            if (slot == activeEnv->end()) return std::nullopt;
            const long long one = 1;
            auto delta = ctInt(one, slot->second.bits, slot->second.isUnsigned);
            auto folded = ctBinary(slot->second, delta,
                                   static_cast<int>(unary->op == UnaryOp::PreInc
                                                        ? BinaryOp::Add
                                                        : BinaryOp::Sub),
                                   unary->operand->type);
            if (!folded) return std::nullopt;
            slot->second = *folded;
            return slot->second;
        }

        if (operand->type == ConstValue::INT || operand->type == ConstValue::CHAR) {
            const bool isChar = operand->type == ConstValue::CHAR;
            const long long raw = isChar ? operand->charVal : operand->intVal;
            const unsigned opBits = isChar ? 8 : operand->bits;
            const bool opUnsigned = isChar ? false : operand->isUnsigned;
            switch (unary->op) {
                case UnaryOp::Plus: return *operand;
                case UnaryOp::Minus:
                    return ctNormalizeInt(static_cast<uint64_t>(-static_cast<uint64_t>(raw)),
                                          opBits, opUnsigned);
                case UnaryOp::Not:
                    return ctInt(!ctTruthy(*operand) ? 1 : 0, 1, true, true);
                case UnaryOp::BitNot:
                    return ctNormalizeInt(~static_cast<uint64_t>(raw), opBits, opUnsigned);
                default: return std::nullopt;
            }
        }
        if (operand->type == ConstValue::DOUBLE) {
            ConstValue cv;
            cv.type = ConstValue::DOUBLE;
            switch (unary->op) {
                case UnaryOp::Plus:  cv.doubleVal = operand->doubleVal; break;
                case UnaryOp::Minus: cv.doubleVal = -operand->doubleVal; break;
                default: return std::nullopt;
            }
            return cv;
        }
        return std::nullopt;
    }

    // Explicit cast in a constant expression, e.g. `(int)SomeEnumerator`
    // (TYP-20 requires the cast for enum -> int).
    if (auto* cast = dynamic_cast<CastExprAST*>(expr)) {
        auto operand = evaluateConstexpr(cast->expr.get());
        if (!operand) return std::nullopt;
        Type* to = stripTypedef(cast->castType);
        if (!to) return std::nullopt;
        return ctCast(*operand, to);
    }

    if (auto* var = dynamic_cast<VariableExprAST*>(expr)) {
        // A parameter/local of the constexpr function being interpreted shadows
        // any module-level constexpr variable.
        if (activeEnv) {
            auto envIt = activeEnv->find(var->name);
            if (envIt != activeEnv->end()) {
                return envIt->second;
            }
        }
        auto it = constexprValues.find(var->name);
        if (it != constexprValues.end()) {
            return it->second;
        }
        // Enumerator constant.
        for (const auto& candidate : namespaceCandidates(var->name)) {
            auto eit = enumConstants.find(candidate);
            if (eit != enumConstants.end()) {
                ConstValue cv;
                cv.type = ConstValue::INT;
                cv.intVal = eit->second.second;
                return cv;
            }
        }
        return std::nullopt;
    }

    if (auto* call = dynamic_cast<CallExprAST*>(expr)) {
        return evalConstexprCall(*call, 0);
    }

    return std::nullopt;
}

bool SemanticAnalyzer::constValueTruthy(const ConstValue& v) {
    return ctTruthy(v);
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evalConstexprCall(CallExprAST& call, int depth) {
    if (depth > kConstexprMaxDepth || constexprCallDepth >= kConstexprMaxDepth) {
        return std::nullopt; // runaway recursion: fall back to runtime evaluation
    }
    // The call may not have been through visit(CallExprAST) yet (e.g. a
    // `constexpr` initializer is folded before its subexpressions are typed),
    // so resolve a namespace-qualified callee here as well.
    std::string calleeName = call.callee;
    auto it = constexprFunctions.find(calleeName);
    if (it == constexprFunctions.end()) {
        calleeName = resolveNamespaceName(calleeName);
        it = constexprFunctions.find(calleeName);
    }
    if (it == constexprFunctions.end()) {
        return std::nullopt; // not a constexpr function (e.g. a libc call)
    }
    FunctionDeclAST* fn = it->second;
    if (!fn->body || call.args.size() != fn->params.size()) {
        return std::nullopt;
    }

    // Evaluate arguments in the caller's environment, then bind parameters.
    ConstEnv env = activeEnv ? *activeEnv : ConstEnv{};
    for (size_t i = 0; i < fn->params.size(); ++i) {
        auto arg = evaluateConstexpr(call.args[i].get());
        if (!arg) {
            return std::nullopt;
        }
        env[fn->params[i]->name] = ctClone(*arg);
    }

    ConstEnv* saved = activeEnv;
    activeEnv = &env;
    constexprCallDepth++;
    CtFlow flow = CtFlow::Normal;
    auto result = evalConstexprStmt(fn->body.get(), env, depth + 1, flow);
    constexprCallDepth--;
    activeEnv = saved;
    return flow == CtFlow::Return ? result : std::nullopt;
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evalConstexprStmt(StmtAST* stmt, ConstEnv& env, int depth,
                                    CtFlow& flow) {
    flow = CtFlow::Normal;
    if (!stmt || depth > kConstexprMaxDepth) {
        return std::nullopt;
    }
    if (!ctStepAllowance(*stmt)) {
        return std::nullopt;
    }

    if (auto* ret = dynamic_cast<ReturnStmtAST*>(stmt)) {
        flow = CtFlow::Return;
        if (!ret->value) {
            return ctInt(0);
        }
        return evaluateConstexpr(ret->value.get());
    }

    if (auto* block = dynamic_cast<CompoundStmtAST*>(stmt)) {
        std::optional<ConstValue> result;
        for (auto& inner : block->stmts) {
            CtFlow innerFlow = CtFlow::Normal;
            result = evalConstexprStmt(inner.get(), env, depth + 1, innerFlow);
            if (innerFlow != CtFlow::Normal) {
                flow = innerFlow;
                return result;
            }
        }
        return result;
    }

    if (auto* ifs = dynamic_cast<IfStmtAST*>(stmt)) {
        auto cond = evaluateConstexpr(ifs->cond.get());
        if (!cond) {
            return std::nullopt;
        }
        if (ctTruthy(*cond)) {
            return evalConstexprStmt(ifs->thenStmt.get(), env, depth + 1, flow);
        }
        return evalConstexprStmt(ifs->elseStmt.get(), env, depth + 1, flow);
    }

    if (auto* wh = dynamic_cast<WhileStmtAST*>(stmt)) {
        while (true) {
            auto cond = evaluateConstexpr(wh->cond.get());
            if (!cond) return std::nullopt;
            if (!ctTruthy(*cond)) break;
            if (!ctStepAllowance(*stmt)) return std::nullopt;
            CtFlow bodyFlow = CtFlow::Normal;
            auto result = evalConstexprStmt(wh->body.get(), env, depth + 1, bodyFlow);
            if (bodyFlow == CtFlow::Break) break;
            if (bodyFlow == CtFlow::Return) {
                flow = CtFlow::Return;
                return result;
            }
            // Continue and Normal both proceed to the next condition check.
        }
        return std::nullopt;
    }

    if (auto* dw = dynamic_cast<DoWhileStmtAST*>(stmt)) {
        while (true) {
            if (!ctStepAllowance(*stmt)) return std::nullopt;
            CtFlow bodyFlow = CtFlow::Normal;
            auto result = evalConstexprStmt(dw->body.get(), env, depth + 1, bodyFlow);
            if (bodyFlow == CtFlow::Break) break;
            if (bodyFlow == CtFlow::Return) {
                flow = CtFlow::Return;
                return result;
            }
            auto cond = evaluateConstexpr(dw->cond.get());
            if (!cond) return std::nullopt;
            if (!ctTruthy(*cond)) break;
        }
        return std::nullopt;
    }

    if (auto* fr = dynamic_cast<ForStmtAST*>(stmt)) {
        CtFlow initFlow = CtFlow::Normal;
        if (fr->init) {
            evalConstexprStmt(fr->init.get(), env, depth + 1, initFlow);
            if (initFlow == CtFlow::Return) {
                flow = CtFlow::Return;
                return std::nullopt;
            }
        }
        while (true) {
            if (fr->cond) {
                auto cond = evaluateConstexpr(fr->cond.get());
                if (!cond) return std::nullopt;
                if (!ctTruthy(*cond)) break;
            }
            if (!ctStepAllowance(*stmt)) return std::nullopt;
            CtFlow bodyFlow = CtFlow::Normal;
            auto result = evalConstexprStmt(fr->body.get(), env, depth + 1, bodyFlow);
            if (bodyFlow == CtFlow::Break) break;
            if (bodyFlow == CtFlow::Return) {
                flow = CtFlow::Return;
                return result;
            }
            if (fr->inc) {
                if (!evaluateConstexpr(fr->inc.get())) return std::nullopt;
            }
        }
        return std::nullopt;
    }

    if (dynamic_cast<BreakStmtAST*>(stmt)) {
        flow = CtFlow::Break;
        return std::nullopt;
    }

    if (dynamic_cast<ContinueStmtAST*>(stmt)) {
        flow = CtFlow::Continue;
        return std::nullopt;
    }

    if (dynamic_cast<NullStmtAST*>(stmt)) {
        return std::nullopt;
    }

    if (auto* ds = dynamic_cast<DeclStmtAST*>(stmt)) {
        if (auto* vd = dynamic_cast<VarDeclAST*>(ds->decl.get())) {
            if (vd->initExpr) {
                if (auto value = evaluateConstexpr(vd->initExpr.get())) {
                    env[vd->name] = ctClone(*value);
                }
            } else {
                env[vd->name] = ctDefaultValue(vd->type);
            }
        }
        return std::nullopt;
    }

    if (auto* es = dynamic_cast<ExprStmtAST*>(stmt)) {
        evaluateConstexpr(es->expr.get());
        return std::nullopt;
    }

    return std::nullopt;
}

void SemanticAnalyzer::visit(NumberExprAST& node) {
    // LEX-15: the literal kind chooses the fixed-width type; fall back to int32.
    node.type = typeForLiteralKind(node.literalKind);
    if (!node.type) node.type = typeCtx->getInt32();
    // LEX-17: 128 位字面量（valueHi != 0）直接映射 int128/uint128——value 只
    // 是低 64 位，不能再按它做值域提升。
    if (node.valueHi != 0) {
        node.type = (node.literalKind == LiteralKind::UInt ||
                     node.literalKind == LiteralKind::ULong)
                        ? typeCtx->getUInt128()
                        : typeCtx->getInt128();
        node.isLValue = false;
        return;
    }
    // 无后缀整型字面量按值域提升（C 语义：十进制取能容纳的最小有符号类型）——
    // 否则 int64 v = 4000000000 会以 int32 有符号构造 APInt（Debug 断言崩溃，
    // Release 静默截断为错误值）。
    if (node.literalKind == LiteralKind::Int) {
        const long long v = node.value;
        if (v >= INT32_MIN && v <= INT32_MAX) {
            node.type = typeCtx->getInt32();
        } else if (v >= INT64_MIN && v <= INT64_MAX) {
            node.type = typeCtx->getInt64();
        } else {
            node.type = typeCtx->getInt128();
        }
    }
    node.isLValue = false;
}

// Found issue: `true`/`false` 是布尔字面量（spec keywords.md）——bool 类型。
void SemanticAnalyzer::visit(BoolLiteralExprAST& node) {
    node.type = typeCtx->getBool();
    node.isLValue = false;
}

void SemanticAnalyzer::visit(FloatExprAST& node) {
    // LEX-15: unsuffixed floats default to float64.
    node.type = typeForLiteralKind(node.literalKind);
    if (!node.type) node.type = typeCtx->getFloat64();
    node.isLValue = false;
}

void SemanticAnalyzer::visit(CharExprAST& node) {
    node.type = typeCtx->getChar();
    node.isLValue = false;
}

void SemanticAnalyzer::visit(StringExprAST& node) {
    // P1-06 (FMT-04): literals are validated at their single point of
    // creation — invalid sequences are a compile error (E2015), never a
    // runtime surprise. TYP-05: prefixed literals store raw bytes (no
    // transcoding), so they are exempt from the UTF-8 check.
    if (node.prefix == StringLiteralPrefix::None && !smc_utf8::valid(node.value)) {
        emitError(DiagnosticCode::SemInvalidUtf8,
                  "invalid UTF-8 sequence in string literal", node);
    }
    // TYP-05: u8/u16/u32-prefixed literals are fixed-width arrays (decay to
    // char8*/char16*/char32*); the plain form stays `str` (string_view).
    if (node.prefix != StringLiteralPrefix::None) {
        Type* elem = nullptr;
        switch (node.prefix) {
            case StringLiteralPrefix::U8: elem = typeCtx->getUInt8(); break;
            case StringLiteralPrefix::U16: elem = typeCtx->getUInt16(); break;
            case StringLiteralPrefix::U32: elem = typeCtx->getUInt32(); break;
            default: break;
        }
        node.type = new ArrayType(elem, static_cast<int>(node.value.size()));
        node.isLValue = false;
        return;
    }
    // P1-06 (FMT-03): string literals are `str` (non-owning UTF-8 view).
    node.type = typeCtx->getStrType();
    node.isLValue = false;
}

void SemanticAnalyzer::visit(VariableExprAST& node) {
    // PAR-17: static 方法内无 this——钉死诊断（非 static 方法里 `this` 是
    // 隐式插入的参数，按普通变量解析）。
    if (node.name == "this" && m_inStaticMethod) {
        emitError("'this' is not valid in a static method", node);
        node.type = nullptr;
        node.isLValue = false;
        return;
    }
    // AGG-10/DS5: qualified static-member access — access check against the
    // declaring class before the ordinary symbol resolution proceeds.
    checkStaticMemberAccess(node.name, node);

    // P1-05 / ANN-05: 弃用变量使用（W3004）。
    if (m_deprecatedVars.count(node.name)) {
        std::string msg = m_deprecatedMsgs.count("var:" + node.name)
            ? m_deprecatedMsgs["var:" + node.name] : "";
        if (msg.empty()) {
            emitWarning(DiagnosticCode::WarnDeprecated,
                        "'" + node.name + "' is deprecated", node);
        } else {
            emitWarning(DiagnosticCode::WarnDeprecated,
                        "'" + node.name + "' is deprecated: " + msg, node);
        }
    }

    // Enumerator (possibly namespace-qualified, e.g. `A::Red`)?
    for (const auto& candidate : namespaceCandidates(node.name)) {
        auto it = enumConstants.find(candidate);
        if (it != enumConstants.end()) {
            node.name = candidate;
            node.isEnumConstant = true;
            node.enumValue = it->second.second;
            node.type = it->second.first;
            node.isLValue = false;
            return;
        }
    }

    const std::string originalName = node.name;
    const std::string key = resolveNamespaceName(node.name);
    Symbol* sym = currentScope->lookup(key);
    if (!sym && key != originalName) {
        sym = lookup(originalName);
    }
    if (sym) {
        sym->isUsed = true; // SEM-11: any reference counts as a use (W3002).
    }
    if (!sym) {
        emitError(DiagnosticCode::SemUndeclaredIdentifier, "use of undeclared identifier '" + originalName + "'", node);
        node.type = nullptr;
    } else if (sym->type && sym->type->kind == TypeKind::Function) {
        // A function used as a value decays to a function pointer.
        auto* funcType = static_cast<FunctionType*>(sym->type);
        node.name = key;
        node.isFunctionRef = true;
        node.resolvedFunctionName = mangleFunction(node.name, funcType->paramTypes);
        node.type = new Type(TypeKind::Pointer, funcType);
        node.isLValue = false;
        return;
    } else {
        node.name = key;
        node.type = sym->type;
    }
    node.isLValue = true;
}

// PAR-08: key for stack-backed slice-literal tracking — scoped by the owning
// function so same-named locals in different functions do not collide.
static std::string sliceLiteralKey(const FunctionDeclAST* fn,
                                   const std::string& name) {
    return fn ? fn->name + "::" + name : name;
}

// PAR-08: true when an lvalue roots at a global variable (bare name or a
// subobject path: field / subscript), used for dangling-escape checks.
static bool rootsAtGlobalVariable(
    const ExprAST* e, const std::unordered_set<std::string>& globals) {
    if (auto* v = dynamic_cast<const VariableExprAST*>(e))
        return globals.count(v->name) > 0;
    if (auto* m = dynamic_cast<const MemberAccessExprAST*>(e))
        return rootsAtGlobalVariable(m->object.get(), globals);
    if (auto* a = dynamic_cast<const ArrayAccessExprAST*>(e))
        return rootsAtGlobalVariable(a->array.get(), globals);
    return false;
}

// SEM-17: operand kinds that may carry user-defined operator overloads
// (mirrors the aggregate check used by the binary-operator call site).
static bool isOperatorOverloadOperand(Type* t) {    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::Struct:
        case TypeKind::Class:
        case TypeKind::Union:
        case TypeKind::Optional:
        case TypeKind::Result:
            return true;
        default:
            return false;
    }
}

void SemanticAnalyzer::visit(BinaryExprAST& node) {
    // P1-04 / CT-12: 表达式树含 compile_time 根 → 优先整树编译期折叠。
    if (containsCompileTimeRoot(&node)) {
        size_t baseErrors = getErrors().size();
        auto v = evalCompileTime(&node, node);
        if (v && (v->type == ConstValue::INT || v->type == ConstValue::DOUBLE)) {
            node.ctHandled = true;
            node.isLValue = false;
            if (v->type == ConstValue::INT) {
                node.ctInt = v->intVal;
                node.type = ctIntTypeFor(typeCtx, v->intVal);
            } else {
                node.ctFloat = v->doubleVal;
                node.type = typeCtx->getFloat64();
            }
            return;
        }
        if (v && v->type == ConstValue::STR) {
            // STR 逃逸到运行时上下文（spec §2 意图为类型错误）。
            emitError("compile-time string value cannot be used in runtime context", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // CHAR 有运行时类型：回退普通路径（子节点钩子已产常量）——评审 I2。
        if (getErrors().size() > baseErrors) {
            // 求值器已诊断（未知成员/深度超限）：就地报错，不回退。
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // 混入运行时操作数且无诊断：回退普通路径——CT 子节点由各自钩子
        // 求值成常量，本节点作普通运行时表达式处理。
    }

    Type* leftType = getExprType(*node.left);
    Type* rightType = getExprType(*node.right);

    // Check for operator overloading on aggregate-like types (struct/union/
    // class/Optional/Result). 评审 I1: Optional/Result must take this path —
    // falling through to the generic comparison switch emitted ICmp on a
    // struct and aborted the compiler.
    auto isOverloadOperand = [](Type* t) {
        while (t && t->kind == TypeKind::Typedef)
            t = static_cast<TypedefType*>(t)->aliasedType;
        if (!t) return false;
        switch (t->kind) {
            case TypeKind::Struct:
            case TypeKind::Class:
            case TypeKind::Union:
            case TypeKind::Optional:
            case TypeKind::Result:
                return true;
            default:
                return false;
        }
    };
    auto isOptionalOrResult = [](Type* t) {
        t = stripTypedef(t);
        return t && (t->kind == TypeKind::Optional ||
                     t->kind == TypeKind::Result);
    };
    bool logicalOnOptional =
        (node.op == BinaryOp::And || node.op == BinaryOp::Or) &&
        (isOptionalOrResult(leftType) || isOptionalOrResult(rightType));
    if (!logicalOnOptional &&
        (isOverloadOperand(leftType) || isOverloadOperand(rightType))) {
        std::string mangledName = getOperatorMangledName(node.op, leftType, rightType);
        if (!mangledName.empty()) {
            // Look up by the unmangled operator name (e.g., "operator+")
            std::string opName;
            switch (node.op) {
                case BinaryOp::Add: opName = "operator+"; break;
                case BinaryOp::Sub: opName = "operator-"; break;
                case BinaryOp::Mul: opName = "operator*"; break;
                case BinaryOp::Div: opName = "operator/"; break;
                // SEM-17: converge the call-site set with the declaration side.
                case BinaryOp::Mod: opName = "operator%"; break;
                case BinaryOp::Eq: opName = "operator=="; break;
                case BinaryOp::NotEq: opName = "operator!="; break;
                case BinaryOp::Lt: opName = "operator<"; break;
                case BinaryOp::Gt: opName = "operator>"; break;
                case BinaryOp::Le: opName = "operator<="; break;
                case BinaryOp::Ge: opName = "operator>="; break;
                case BinaryOp::And: opName = "operator&&"; break;
                case BinaryOp::Or: opName = "operator||"; break;
                case BinaryOp::BitAnd: opName = "operator&"; break;
                case BinaryOp::BitOr: opName = "operator|"; break;
                case BinaryOp::BitXor: opName = "operator^"; break;
                case BinaryOp::LShift: opName = "operator<<"; break;
                case BinaryOp::RShift: opName = "operator>>"; break;
                default: opName = ""; break;
            }
            OverloadSet* overloadSet = currentScope->lookupOverload(opName);
            if (overloadSet && !overloadSet->empty()) {
                std::vector<Type*> argTypes = {leftType, rightType};
                Symbol* resolved = overloadSet->resolve(argTypes);
                if (resolved) {
                    // Store the mangled name for codegen
                    node.mangledCallee = mangledName;
                    // 表达式类型 = operator 函数声明的返回类型（镜像普通调用
                    // node.type = funcType->returnType）——设成左操作数类型会
                    // 让外层表达式（int32 s = p + q / p + q == 10）看到错误类型。
                    if (resolved->type && resolved->type->kind == TypeKind::Function) {
                        node.type = static_cast<FunctionType*>(resolved->type)->returnType;
                    } else {
                        node.type = leftType;
                    }
                    node.isLValue = false;
                    return;
                }
            }
            // Member form: `a + b` -> `a.operator+(b)`.
            Type* lt = stripTypedef(leftType);
            if (lt && lt->kind == TypeKind::Class) {
                auto* ct = static_cast<ClassType*>(lt);
                ClassType* definingClass = nullptr;
                Symbol* method = resolveMethod(ct, opName, {rightType}, &definingClass);
                if (method && method->type && method->type->kind == TypeKind::Function) {
                    if (definingClass &&
                        !canAccessMember(definingClass, definingClass->memberAccessLevel(opName))) {
                        emitError(DiagnosticCode::SemPrivateMemberAccess,
                                  "cannot access private/protected operator '" + opName +
                                      "' of class '" + definingClass->name + "'",
                                  node);
                    } else {
                        auto* ft = static_cast<FunctionType*>(method->type);
                        node.isMethodOperator = true;
                        node.resolvedParamTypes = ft->paramTypes;
                        node.mangledCallee = mangleFunction(
                            opName, {new Type(TypeKind::Pointer, definingClass), rightType});
                        node.type = ft->returnType;
                        node.isLValue = false;
                        delete method;
                        return;
                    }
                }
                if (method) delete method;
            }
            emitError("no matching " + binaryOpToString(node.op) + " operator for types '" 
                + typeToString(leftType) + "' and '" + typeToString(rightType) + "'", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
    }

    node.type = checkBinaryTypes(node.op, leftType, rightType, node);
    node.isLValue = false;
}

void SemanticAnalyzer::visit(UnaryExprAST& node) {
    // P1-04 / CT-12: 表达式树含 compile_time 根 → 优先整树编译期折叠。
    if (containsCompileTimeRoot(&node)) {
        size_t baseErrors = getErrors().size();
        auto v = evalCompileTime(&node, node);
        if (v && (v->type == ConstValue::INT || v->type == ConstValue::DOUBLE)) {
            node.ctHandled = true;
            node.isLValue = false;
            if (v->type == ConstValue::INT) {
                node.ctInt = v->intVal;
                node.type = ctIntTypeFor(typeCtx, v->intVal);
            } else {
                node.ctFloat = v->doubleVal;
                node.type = typeCtx->getFloat64();
            }
            return;
        }
        if (v && v->type == ConstValue::STR) {
            // STR 逃逸到运行时上下文（spec §2 意图为类型错误）。
            emitError("compile-time string value cannot be used in runtime context", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // CHAR 有运行时类型：回退普通路径（子节点钩子已产常量）——评审 I2。
        if (getErrors().size() > baseErrors) {
            // 求值器已诊断（未知成员/深度超限）：就地报错，不回退。
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // 混入运行时操作数且无诊断：回退普通路径——CT 子节点由各自钩子
        // 求值成常量，本节点作普通运行时表达式处理。
    }

    Type* operandType = getExprType(*node.operand);

    // Review 2.4: `!opt` / `!res` mean "invalid" / "error" before any
    // user-defined operator overload lookup.
    if (node.op == UnaryOp::Not) {
        Type* ot = stripTypedef(operandType);
        if (ot && (ot->kind == TypeKind::Optional || ot->kind == TypeKind::Result)) {
            node.type = typeCtx->getInt32();
            node.isLValue = false;
            return;
        }
    }

    // SEM-17: unary operator overloading on aggregate operands. Resolve the
    // operator as a free function; when none exists, fall through to the
    // built-in checks (which diagnose aggregate operands as before).
    if (isOperatorOverloadOperand(operandType)) {
        std::string opName;
        switch (node.op) {
            case UnaryOp::Plus:   opName = "operator+"; break;
            case UnaryOp::Minus:  opName = "operator-"; break;
            case UnaryOp::BitNot: opName = "operator~"; break;
            case UnaryOp::Not:    opName = "operator!"; break;
            case UnaryOp::PreInc: opName = "operator++"; break;
            case UnaryOp::PreDec: opName = "operator--"; break;
            default: break;
        }
        if (!opName.empty()) {
            OverloadSet* overloadSet = currentScope->lookupOverload(opName);
            if (overloadSet && !overloadSet->empty()) {
                Symbol* resolved = overloadSet->resolve({operandType});
                if (resolved && resolved->type &&
                    resolved->type->kind == TypeKind::Function) {
                    node.mangledCallee = mangleFunction(opName, {operandType});
                    node.type = static_cast<FunctionType*>(resolved->type)->returnType;
                    node.isLValue = false;
                    return;
                }
            }
            // Member form: `-v` -> `v.operator-()`.
            Type* ot = stripTypedef(operandType);
            if (ot && ot->kind == TypeKind::Class) {
                auto* ct = static_cast<ClassType*>(ot);
                ClassType* definingClass = nullptr;
                Symbol* method = resolveMethod(ct, opName, {}, &definingClass);
                if (method && method->type && method->type->kind == TypeKind::Function) {
                    if (definingClass &&
                        !canAccessMember(definingClass, definingClass->memberAccessLevel(opName))) {
                        emitError(DiagnosticCode::SemPrivateMemberAccess,
                                  "cannot access private/protected operator '" + opName +
                                      "' of class '" + definingClass->name + "'",
                                  node);
                    } else {
                        auto* ft = static_cast<FunctionType*>(method->type);
                        node.isMethodOperator = true;
                        node.resolvedParamTypes = ft->paramTypes;
                        node.mangledCallee = mangleFunction(
                            opName, {new Type(TypeKind::Pointer, definingClass)});
                        node.type = ft->returnType;
                        node.isLValue = false;
                        delete method;
                        return;
                    }
                }
                if (method) delete method;
            }
            // No overload: `!aggregate` has no built-in meaning (coerceToBool
            // cannot lower an aggregate) — diagnose instead of emitting bad IR.
            if (node.op == UnaryOp::Not) {
                emitError("invalid operand to unary '!': '" + typeToString(operandType) + "'", node);
                node.type = nullptr;
                node.isLValue = false;
                return;
            }
        }
    }

    switch (node.op) {
        case UnaryOp::Plus:
        case UnaryOp::Minus:
            if (!isArithmeticType(operandType)) {
                emitError("invalid operand to unary '" + std::string(node.op == UnaryOp::Plus ? "+" : "-") + "': '" + typeToString(operandType) + "'", node);
                node.type = nullptr;
            } else {
                node.type = operandType;
            }
            break;
        case UnaryOp::Not:
            node.type = typeCtx->getInt32();
            break;
        case UnaryOp::BitNot:
            if (!isIntegerType(operandType)) {
                emitError("invalid operand to unary '~': '" + typeToString(operandType) + "'", node);
                node.type = nullptr;
            } else {
                node.type = operandType;
            }
            break;
        case UnaryOp::Deref: {
            Type* pointee = stripTypedef(operandType);
            if (pointee && pointee->kind == TypeKind::Pointer) {
                node.type = pointee->base;
            } else {
                emitError("cannot dereference non-pointer type '" + typeToString(operandType) + "'", node);
                node.type = nullptr;
            }
            break;
        }
        case UnaryOp::AddressOf:
            // P1-07 / SEM-12: a bit-field has no address (C constraint).
            if (auto* ma = dynamic_cast<MemberAccessExprAST*>(node.operand.get())) {
                if (ma->isBitField) {
                    emitError(DiagnosticCode::SemBitFieldNotAddressable,
                              "cannot take the address of a bit-field", node);
                    node.type = nullptr;
                    break;
                }
            }
            // PAR-08: taking the address of a compound literal is a dangling
            // escape (the temporary dies at the end of the full-expression).
            if (auto* cl = dynamic_cast<CompoundLiteralExprAST*>(node.operand.get())) {
                emitError("cannot take the address of a compound literal (temporary object)", node);
                node.type = nullptr;
                break;
            }
            // 函数设计符 `&fn`：函数名在值位置已 decay 为 Pointer(Function)
            // 且非 lvalue；C 语义下 &fn 与 fn 同为函数指针，取址是无操作。
            {
                Type* strippedOp = operandType;
                while (strippedOp && strippedOp->kind == TypeKind::Typedef)
                    strippedOp = static_cast<TypedefType*>(strippedOp)->aliasedType;
                if (strippedOp && strippedOp->kind == TypeKind::Pointer &&
                    strippedOp->base && strippedOp->base->kind == TypeKind::Function) {
                    node.type = operandType;
                    break;
                }
            }
            if (node.operand->isLValue) {
                node.type = new Type(TypeKind::Pointer, operandType);
            } else {
                emitError("cannot take address of non-lvalue expression", node);
                node.type = nullptr;
            }
            break;
        case UnaryOp::PreInc:
        case UnaryOp::PreDec:
            if (!isArithmeticType(operandType) && !isPointerOrArray(operandType)) {
                emitError("invalid operand to '" + std::string(node.op == UnaryOp::PreInc ? "++" : "--") + "': '" + typeToString(operandType) + "'", node);
                node.type = nullptr;
            } else {
                node.type = operandType;
            }
            break;
        case UnaryOp::Sizeof:
            node.type = typeCtx->getInt32();
            break;
    }
    // A dereference denotes the pointee and is therefore an lvalue.
    node.isLValue = (node.op == UnaryOp::Deref);
}

bool SemanticAnalyzer::lowerToString(CallExprAST& node, size_t argIndex, Type* argType) {
    Type* t = argType;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }

    auto returnsCString = [](FunctionType* ft) {
        return ft->returnType && ft->returnType->kind == TypeKind::Pointer &&
               ft->returnType->base && ft->returnType->base->kind == TypeKind::Char;
    };

    // Preferred: a `to_string()` method on the class.
    ClassType* classType = nullptr;
    if (t && t->kind == TypeKind::Class) {
        classType = static_cast<ClassType*>(t);
    } else if (t && t->kind == TypeKind::Pointer && t->base &&
               t->base->kind == TypeKind::Class) {
        classType = static_cast<ClassType*>(t->base);
    }
    if (classType) {
        Symbol* method = resolveMethod(classType, "to_string", {});
        if (method) {
            bool usable = method->type && method->type->kind == TypeKind::Function &&
                          returnsCString(static_cast<FunctionType*>(method->type));
            delete method;
            if (usable) {
                node.args[argIndex] = std::make_unique<MethodCallExprAST>(
                    std::move(node.args[argIndex]), "to_string",
                    std::vector<std::unique_ptr<ExprAST>>{});
                getExprType(*node.args[argIndex]);
                return true;
            }
        }
    }

    // Otherwise a free function `to_string(T)`.
    OverloadSet* overloadSet = currentScope->lookupOverload("to_string");
    if (overloadSet && !overloadSet->empty()) {
        std::vector<Type*> argTypes{argType};
        Symbol* resolved = overloadSet->resolve(argTypes);
        if (resolved && resolved->type && resolved->type->kind == TypeKind::Function &&
            returnsCString(static_cast<FunctionType*>(resolved->type))) {
            auto call = std::make_unique<CallExprAST>(
                "to_string", std::vector<std::unique_ptr<ExprAST>>{});
            call->args.push_back(std::move(node.args[argIndex]));
            node.args[argIndex] = std::move(call);
            getExprType(*node.args[argIndex]);
            return true;
        }
    }

    return false;
}

bool SemanticAnalyzer::tryAnalyzePrintCall(CallExprAST& node) {
    node.type = nullptr;
    node.isLValue = false;

    if (node.args.empty()) {
        emitError("'" + node.callee + "' requires a format string argument", node);
        return true;
    }

    auto* literal = dynamic_cast<StringExprAST*>(node.args[0].get());
    if (!literal) {
        emitError("the format argument to '" + node.callee + "' must be a string literal", node);
        return true;
    }

    const bool newline = node.callee == "println";
    std::vector<PrintArgKind> kinds;
    kinds.reserve(node.args.size() - 1);

    for (size_t k = 1; k < node.args.size(); ++k) {
        Type* argType = getExprType(*node.args[k]);
        if (!argType) {
            return true; // already reported
        }

        PrintArgKind kind;
        if (builtinPrintKind(argType, kind)) {
            kinds.push_back(kind);
            continue;
        }
        if (lowerToString(node, k, argType)) {
            kinds.push_back(PrintArgKind::ToString);
            continue;
        }

        emitError("cannot format value of type '" + typeToString(argType) + "' with '{}' in '" +
                  node.callee + "'; define a to_string for it", node);
        return true;
    }

    std::string format, error;
    std::vector<PrintSpec> specs;
    PrintFormatError errKind = PrintFormatError::None;
    if (!buildPrintFormat(literal->value, kinds, newline, format, error,
                          &specs, &errKind)) {
        DiagnosticCode code = DiagnosticCode::None;
        switch (errKind) {
            case PrintFormatError::ArgCount:  code = DiagnosticCode::SemFormatArgCount;  break;
            case PrintFormatError::SpecType:  code = DiagnosticCode::SemFormatSpecType;  break;
            case PrintFormatError::SpecSyntax: code = DiagnosticCode::SemFormatSpecSyntax; break;
            case PrintFormatError::None: break;
        }
        emitError(code, error, node);
        return true;
    }

    node.isPrint = true;
    node.printNewline = newline;
    node.printCFormat = std::move(format);
    node.printArgKinds = std::move(kinds);
    node.printSpecs = std::move(specs);
    node.type = typeCtx->getVoid();
    node.isLValue = false;
    return true;
}

// P1-09 (FMT-07/09/10/11): builtin `format(fmt, args...) -> string`. The
// first argument is a str (literal or dynamic); literal formats get the full
// compile-time spec/arity checks, dynamic formats only classify their
// arguments (runtime contract documented in stdlib.md).
bool SemanticAnalyzer::tryAnalyzeFormatCall(CallExprAST& node) {
    node.type = nullptr;
    node.isLValue = false;

    if (node.args.empty()) {
        emitError("'format' requires a format string argument", node);
        return true;
    }

    Type* fmtType = getExprType(*node.args[0]);
    if (!fmtType) return true; // already reported
    Type* stripped = fmtType;
    while (stripped && stripped->kind == TypeKind::Typedef) {
        stripped = static_cast<TypedefType*>(stripped)->aliasedType;
    }
    if (!stripped ||
        (stripped->kind != TypeKind::Str && stripped->kind != TypeKind::String)) {
        emitError("the format argument to 'format' must be a string_view", node);
        return true;
    }

    std::vector<PrintArgKind> kinds;
    kinds.reserve(node.args.size() - 1);
    for (size_t k = 1; k < node.args.size(); ++k) {
        Type* argType = getExprType(*node.args[k]);
        if (!argType) return true; // already reported

        PrintArgKind kind;
        if (builtinPrintKind(argType, kind)) {
            kinds.push_back(kind);
            continue;
        }
        if (lowerToString(node, k, argType)) {
            kinds.push_back(PrintArgKind::ToString);
            continue;
        }
        emitError("cannot format value of type '" + typeToString(argType) +
                      "' with '{}'; define a to_string for it",
                  node);
        return true;
    }

    node.isFormat = true;
    node.printArgKinds = std::move(kinds);
    node.type = typeCtx->getStringType();
    node.isLValue = false;

    if (auto* literal = dynamic_cast<StringExprAST*>(node.args[0].get())) {
        std::string error;
        std::vector<PrintSpec> specs;
        PrintFormatError errKind = PrintFormatError::None;
        std::string out; // format() assembles at codegen; only validate here.
        if (!buildPrintFormat(literal->value, node.printArgKinds, /*newline=*/false,
                              out, error, &specs, &errKind)) {
            DiagnosticCode code = DiagnosticCode::None;
            switch (errKind) {
                case PrintFormatError::ArgCount:  code = DiagnosticCode::SemFormatArgCount;  break;
                case PrintFormatError::SpecType:  code = DiagnosticCode::SemFormatSpecType;  break;
                case PrintFormatError::SpecSyntax: code = DiagnosticCode::SemFormatSpecSyntax; break;
                case PrintFormatError::None: break;
            }
            emitError(code, error, node);
            return true;
        }
        node.formatLiteral = literal->value;
        node.formatSpecs = std::move(specs);
    }
    return true;
}

// P1-09 (STD-10): builtin `split(s, sep) -> []str` — zero-copy views over a
// malloc'd str array (spec deviation 1: slice values are not constructible
// from .smc, so split mirrors string.new/destroy as a codegen builtin). A
// literal empty separator is rejected at compile time (spec deviation 2);
// a dynamic one panics at runtime. split_destroy frees the array.
bool SemanticAnalyzer::tryAnalyzeSplitCall(CallExprAST& node) {
    node.type = nullptr;
    node.isLValue = false;

    if (node.callee == "split_destroy") {
        if (node.args.size() != 1) {
            emitError("'split_destroy' requires exactly one argument", node);
            return true;
        }
        Type* argType = getExprType(*node.args[0]);
        if (!argType) return true; // already reported
        Type* stripped = argType;
        while (stripped && stripped->kind == TypeKind::Typedef)
            stripped = static_cast<TypedefType*>(stripped)->aliasedType;
        auto* sliceTy = stripped && stripped->kind == TypeKind::Slice
                            ? static_cast<SliceType*>(stripped)
                            : nullptr;
        if (!sliceTy || !sliceTy->elementType) {
            emitError("'split_destroy' requires a []string_view value", node);
            return true;
        }
        Type* elem = sliceTy->elementType;
        while (elem && elem->kind == TypeKind::Typedef)
            elem = static_cast<TypedefType*>(elem)->aliasedType;
        if (!elem || (elem->kind != TypeKind::Str && elem->kind != TypeKind::String)) {
            emitError("'split_destroy' requires a []string_view value", node);
            return true;
        }
        // split produces []str only; []string has no legitimate producer and
        // would let an array-decayed string[] reach free() (stack pointer).
        if (elem->kind != TypeKind::Str) {
            emitError("'split_destroy' requires a []string_view value", node);
            return true;
        }
        node.isSplitDestroy = true;
        node.type = typeCtx->getVoid();
        return true;
    }

    if (node.args.size() != 2) {
        emitError("'split' requires exactly two arguments (string, separator)", node);
        return true;
    }
    const auto isStrLike = [](Type* t) {
        while (t && t->kind == TypeKind::Typedef)
            t = static_cast<TypedefType*>(t)->aliasedType;
        return t && (t->kind == TypeKind::Str || t->kind == TypeKind::String);
    };
    for (size_t k = 0; k < 2; ++k) {
        Type* argType = getExprType(*node.args[k]);
        if (!argType) return true; // already reported
        if (!isStrLike(argType)) {
            emitError("'split' arguments must be string_view (got '" +
                          typeToString(argType) + "')",
                      node);
            return true;
        }
    }
    if (auto* lit = dynamic_cast<StringExprAST*>(node.args[1].get())) {
        if (lit->value.empty()) {
            emitError(DiagnosticCode::SemFormatSpecSyntax,
                      "split: empty separator", node);
            return true;
        }
    }
    node.isSplit = true;
    node.type = typeCtx->getSliceType(typeCtx->getStrType());
    return true;
}

bool SemanticAnalyzer::tryAnalyzeAssertCall(CallExprAST& node) {    node.type = typeCtx->getVoid();
    node.isLValue = false;
    if (node.args.size() != 1) {
        emitError("'assert' takes exactly one condition", node);
        return true;
    }
    Type* condType = getExprType(*node.args[0]);
    if (!condType) {
        return true; // already reported
    }
    if (!isConditionType(condType)) {
        emitError("'assert' condition must be a scalar or optional/result, got '" + typeToString(condType) + "'", node);
        return true;
    }
    node.isAssert = true;
    return true;
}

bool SemanticAnalyzer::tryAnalyzePanicCall(CallExprAST& node) {
    node.type = typeCtx->getVoid();
    node.isLValue = false;
    if (node.args.size() != 1) {
        emitError("'panic' takes exactly one message", node);
        return true;
    }
    Type* msgType = getExprType(*node.args[0]);
    if (!msgType) {
        return true; // already reported
    }
    // P1-06 (FMT-03): a str message is accepted — viewed as its char* bytes.
    if (!isPointerOrArray(msgType) && msgType->kind != TypeKind::Str) {
        emitError("'panic' message must be a C string, got '" + typeToString(msgType) + "'", node);
        return true;
    }
    node.isPanic = true;
    return true;
}

// P1-06 (FMT-04): builtin `str_from_c(cstr) -> str` — build a view over a
// NUL-terminated buffer by strlen. Accepts Pointer(char) and `str` (identity
// view); the length of a char* cannot be known statically, so no implicit
// char* -> str exists anywhere else.
bool SemanticAnalyzer::tryAnalyzeStrFromCCall(CallExprAST& node) {
    node.type = nullptr;
    node.isLValue = false;
    if (node.args.size() != 1) {
        emitError("'str_from_c' takes exactly one argument", node);
        return true;
    }
    Type* argType = getExprType(*node.args[0]);
    if (!argType) return true; // already reported
    Type* t = argType;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    const bool isCharPointer = t && t->kind == TypeKind::Pointer &&
                               t->base && t->base->kind == TypeKind::Char;
    // char arrays decay to char* (C semantics; loadValue performs the decay).
    const bool isCharArray = t && t->kind == TypeKind::Array &&
                             static_cast<ArrayType*>(t)->elementType &&
                             static_cast<ArrayType*>(t)->elementType->kind == TypeKind::Char;
    if (!isCharPointer && !isCharArray && (!t || t->kind != TypeKind::Str)) {
        emitError("'str_from_c' requires a char* (or string_view) argument, got '" +
                      typeToString(argType) + "'",
                  node);
        return true;
    }
    // P1-06 (FMT-04): a literal argument is validated statically (E2015);
    // runtime buffers are validated at codegen (smc.utf8.validate + panic).
    if (auto* lit = dynamic_cast<StringExprAST*>(node.args[0].get())) {
        if (!smc_utf8::valid(lit->value)) {
            emitError(DiagnosticCode::SemInvalidUtf8,
                      "invalid UTF-8 sequence passed to str_from_c", node);
            return true;
        }
    }
    node.isStrFromC = true;
    node.type = typeCtx->getStrType();
    return true;
}

// P1-06 (FMT-02): static `string.new(str)` constructor — `string` is a type
// name, not a variable, so the receiver must be detected before ordinary
// object resolution reports an undeclared identifier.
bool SemanticAnalyzer::tryAnalyzeStringStaticCall(MethodCallExprAST& node) {
    auto* objVar = dynamic_cast<VariableExprAST*>(node.object.get());
    if (!objVar || objVar->name != "string") return false;
    // A user-visible symbol named `string` shadows the builtin (defensive;
    // type names are not declarable as variables since the retype).
    if (currentScope && currentScope->lookup("string")) return false;

    node.isLValue = false;
    if (node.methodName != "new") {
        emitError("unknown static method 'string." + node.methodName + "'", node);
        node.type = nullptr;
        return true;
    }
    if (node.args.size() != 1) {
        emitError("'string.new' takes exactly one string_view argument", node);
        node.type = nullptr;
        return true;
    }
    Type* argType = getExprType(*node.args[0]);
    if (!argType) return true; // already reported
    Type* t = argType;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    if (!t || t->kind != TypeKind::Str) {
        emitError("'string.new' requires a string_view argument; use str_from_c for char* buffers", node);
        node.type = nullptr;
        return true;
    }
    node.builtinMethod = BuiltinMethod::StringNew;
    node.type = typeCtx->getStringType();
    return true;
}

// P1-06 (FMT-01): str builtin methods. len/char_count/char_len_at yield
// usize; char_at yields the raw byte (no codepoint-boundary validation,
// DEC-06 policy).
void SemanticAnalyzer::analyzeStrMethod(MethodCallExprAST& node, Type* objType) {
    (void)objType;
    node.isLValue = false;
    const std::string& name = node.methodName;

    std::vector<Type*> argTypes;
    for (auto& arg : node.args) {
        argTypes.push_back(getExprType(*arg));
    }
    for (auto* t : argTypes) {
        if (!t) {
            node.type = nullptr; // argument already reported
            return;
        }
    }

    if (name == "len" || name == "char_count") {
        if (!argTypes.empty()) {
            emitError("'" + name + "()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod =
            name == "len" ? BuiltinMethod::StrLen : BuiltinMethod::StrCharCount;
        node.type = typeCtx->getUSize();
        return;
    }
    if (name == "data") {
        if (!argTypes.empty()) {
            emitError("'data()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        auto* ch = new Type(*typeCtx->getChar());
        ch->isConst = true;
        node.builtinMethod = BuiltinMethod::StrData;
        node.type = new Type(TypeKind::Pointer, ch);
        return;
    }
    if (name == "char_at" || name == "char_len_at") {
        if (argTypes.size() != 1 || !isIntegerType(argTypes[0])) {
            emitError("'" + name + "()' requires exactly one integer argument", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod =
            name == "char_at" ? BuiltinMethod::StrCharAt : BuiltinMethod::StrCharLenAt;
        node.type = name == "char_at" ? typeCtx->getChar() : typeCtx->getUSize();
        return;
    }
    // P1-09 (STD-10): find/rfind yield the byte index as isize (-1 when
    // absent); sub yields a zero-copy byte-range view with a runtime bounds
    // panic (str.sub: out of bounds).
    if (name == "find" || name == "rfind") {
        const auto isStrLike = [](Type* t) {
            while (t && t->kind == TypeKind::Typedef)
                t = static_cast<TypedefType*>(t)->aliasedType;
            return t && (t->kind == TypeKind::Str || t->kind == TypeKind::String);
        };
        if (argTypes.size() != 1 || !isStrLike(argTypes[0])) {
            emitError("'" + name + "()' requires exactly one string_view argument", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod =
            name == "find" ? BuiltinMethod::StrFind : BuiltinMethod::StrRFind;
        node.type = typeCtx->getISize();
        return;
    }
    if (name == "sub") {
        if (argTypes.size() != 2 || !isIntegerType(argTypes[0]) ||
            !isIntegerType(argTypes[1])) {
            emitError("'sub()' requires exactly two integer arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::StrSub;
        node.type = typeCtx->getStrType();
        return;
    }
    emitError("no member named '" + name + "' in string_view", node);
    node.type = nullptr;
}

// P1-06 (FMT-02): string instance methods. Mutating methods (append/push/
// destroy) require an addressable receiver at codegen; sema records the
// signatures only.
void SemanticAnalyzer::analyzeStringMethod(MethodCallExprAST& node, Type* objType) {
    (void)objType;
    node.isLValue = false;
    const std::string& name = node.methodName;

    std::vector<Type*> argTypes;
    for (auto& arg : node.args) {
        argTypes.push_back(getExprType(*arg));
    }
    for (auto* t : argTypes) {
        if (!t) {
            node.type = nullptr; // argument already reported
            return;
        }
    }

    auto isStrLike = [](Type* t) {
        while (t && t->kind == TypeKind::Typedef)
            t = static_cast<TypedefType*>(t)->aliasedType;
        return t && (t->kind == TypeKind::Str || t->kind == TypeKind::String);
    };

    if (name == "len" || name == "capacity" || name == "char_count") {
        if (!argTypes.empty()) {
            emitError("'" + name + "()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        if (name == "char_count")
            node.builtinMethod = BuiltinMethod::StringCharCount;
        else
            node.builtinMethod =
                name == "len" ? BuiltinMethod::StringLen : BuiltinMethod::StringCapacity;
        node.type = typeCtx->getUSize();
        return;
    }
    if (name == "data") {
        if (!argTypes.empty()) {
            emitError("'data()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::StringData;
        node.type = new Type(TypeKind::Pointer, typeCtx->getChar());
        return;
    }
    if (name == "destroy") {
        if (!argTypes.empty()) {
            emitError("'destroy()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::StringDestroy;
        node.type = typeCtx->getVoid();
        return;
    }
    if (name == "append") {
        if (argTypes.size() != 1 || !isStrLike(argTypes[0])) {
            emitError("'append()' requires exactly one string_view argument; use str_from_c for char* buffers", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::StringAppend;
        node.type = typeCtx->getVoid();
        return;
    }
    if (name == "push") {
        if (argTypes.size() != 1 || !isIntegerType(argTypes[0])) {
            emitError("'push()' requires exactly one char argument", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::StringPush;
        node.type = typeCtx->getVoid();
        return;
    }
    emitError("no member named '" + name + "' in string", node);
    node.type = nullptr;
}

void SemanticAnalyzer::analyzeOptionalMethod(MethodCallExprAST& node, Type* objType) {
    auto* opt = static_cast<OptionalType*>(stripTypedef(objType));
    std::vector<Type*> argTypes;
    for (auto& arg : node.args) argTypes.push_back(getExprType(*arg));

    if (node.methodName == "is_some" || node.methodName == "is_none") {
        if (!argTypes.empty()) {
            emitError("'" + node.methodName + "()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = node.methodName == "is_some"
            ? BuiltinMethod::OptionalIsSome : BuiltinMethod::OptionalIsNone;
        node.type = typeCtx->getBool();
        node.isLValue = false;
        return;
    }
    if (node.methodName == "unwrap") {
        if (!argTypes.empty()) {
            emitError("'unwrap()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::OptionalUnwrap;
        node.type = opt->elementType;
        node.isLValue = false;
        return;
    }
    if (node.methodName == "value_or") {
        if (argTypes.size() != 1 || !typesCompatible(opt->elementType, argTypes[0])) {
            emitError("'value_or()' requires one value compatible with '" +
                          typeToString(opt->elementType) + "'", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::OptionalValueOr;
        node.type = opt->elementType;
        node.isLValue = false;
        return;
    }
    emitError("no member named '" + node.methodName + "' in optional", node);
    node.type = nullptr;
    node.isLValue = false;
}

void SemanticAnalyzer::analyzeResultMethod(MethodCallExprAST& node, Type* objType) {
    auto* res = static_cast<ResultType*>(stripTypedef(objType));
    std::vector<Type*> argTypes;
    for (auto& arg : node.args) argTypes.push_back(getExprType(*arg));

    if (node.methodName == "is_ok" || node.methodName == "is_err") {
        if (!argTypes.empty()) {
            emitError("'" + node.methodName + "()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = node.methodName == "is_ok"
            ? BuiltinMethod::ResultIsOk : BuiltinMethod::ResultIsErr;
        node.type = typeCtx->getBool();
        node.isLValue = false;
        return;
    }
    if (node.methodName == "unwrap") {
        if (!argTypes.empty()) {
            emitError("'unwrap()' takes no arguments", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::ResultUnwrap;
        node.type = res->successType;
        node.isLValue = false;
        return;
    }
    if (node.methodName == "value_or") {
        if (argTypes.size() != 1 || !typesCompatible(res->successType, argTypes[0])) {
            emitError("'value_or()' requires one value compatible with '" +
                          typeToString(res->successType) + "'", node);
            node.type = nullptr;
            return;
        }
        node.builtinMethod = BuiltinMethod::ResultValueOr;
        node.type = res->successType;
        node.isLValue = false;
        return;
    }
    emitError("no member named '" + node.methodName + "' in result", node);
    node.type = nullptr;
    node.isLValue = false;
}

void SemanticAnalyzer::visit(CallExprAST& node) {
    // AGG-10/DS5: qualified static-method call — access check against the
    // declaring class (uses the pre-resolution callee spelling).
    checkStaticMemberAccess(node.callee, node);

    // Resolve a namespace-qualified or namespace-local callee to its mangled key.
    node.callee = resolveNamespaceName(node.callee);

    // General expression callee: `arr[0](1)`, `get()(1)`. The parser stores
    // the callee expression; sema types it as a function pointer and records
    // the signature for codegen's indirect call.
    if (node.isExprCallee && node.calleeExpr) {
        Type* ct = stripTypedef(getExprType(*node.calleeExpr));
        Type* pointee = ct && ct->kind == TypeKind::Pointer ? stripTypedef(ct->base) : nullptr;
        if (!pointee || pointee->kind != TypeKind::Function) {
            emitError("called object is not a function pointer", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        auto* ft = static_cast<FunctionType*>(pointee);
        for (auto& arg : node.args) getExprType(*arg);
        node.resolvedParamTypes = ft->paramTypes;
        node.type = ft->returnType;
        node.isLValue = false;
        return;
    }

    // P1-03 / GEN-03/06: 函数模板调用（显式实参或推导）。模板命中但普通
    // 函数精确匹配可行时走普通路径（SEM-16 最小规则）；诊断型失败返回
    // true 就地结束。
    if (node.hasTemplateArgs || TemplateRegistry::instance().find(node.callee)) {
        if (tryAnalyzeTemplateCall(node)) return;
    }

    // Builtin `assert`/`panic` (only when the user has not declared them).
    // They need the call site's source location, which a library function
    // cannot see without a preprocessor (STD-27 / DEC-21).
    if (node.callee == "assert" || node.callee == "panic") {
        OverloadSet* userDefined = currentScope->lookupOverload(node.callee);
        if (!userDefined || userDefined->empty()) {
            if (node.callee == "assert") {
                tryAnalyzeAssertCall(node);
            } else {
                tryAnalyzePanicCall(node);
            }
            return;
        }
    }

    // P1-06 (FMT-04): builtin `str_from_c(char*) -> str` (only when the user
    // has not declared a function of that name).
    if (node.callee == "str_from_c") {
        OverloadSet* userDefined = currentScope->lookupOverload(node.callee);
        if (!userDefined || userDefined->empty()) {
            tryAnalyzeStrFromCCall(node);
            return;
        }
    }

    // Builtin `print`/`println` (only when the user has not declared them).
    if (node.callee == "print" || node.callee == "println") {
        OverloadSet* userDefined = currentScope->lookupOverload(node.callee);
        if (!userDefined || userDefined->empty()) {
            tryAnalyzePrintCall(node);
            return;
        }
    }

    // P1-09 (FMT-07): builtin `format(fmt, args...) -> string` (only when the
    // user has not declared it). Shares print's spec/argument pipeline.
    if (node.callee == "format") {
        OverloadSet* userDefined = currentScope->lookupOverload(node.callee);
        if (!userDefined || userDefined->empty()) {
            tryAnalyzeFormatCall(node);
            return;
        }
    }

    // P1-09 (STD-10): builtin `split` / `split_destroy` (user-defined first).
    if (node.callee == "split" || node.callee == "split_destroy") {
        OverloadSet* userDefined = currentScope->lookupOverload(node.callee);
        if (!userDefined || userDefined->empty()) {
            tryAnalyzeSplitCall(node);
            return;
        }
    }

    // Review 2.6: call through a stored captured-lambda closure.
    if (Symbol* sym = lookup(node.callee)) {
        Type* st = stripTypedef(sym->type);
        if (st && st->kind == TypeKind::Struct) {
            auto* closure = static_cast<StructType*>(st);
            if (closure->isClosure) {
                sym->isUsed = true;
                if (node.args.size() != closure->closureParamTypes.size()) {
                    emitError("closure call expects " +
                                  std::to_string(closure->closureParamTypes.size()) +
                                  " argument(s), got " +
                                  std::to_string(node.args.size()), node);
                    node.type = nullptr;
                    node.isLValue = false;
                    return;
                }
                for (size_t i = 0; i < node.args.size(); ++i) {
                    Type* at = getExprType(*node.args[i]);
                    if (!at) continue;
                    if (!typesCompatible(closure->closureParamTypes[i], at)) {
                        emitError("closure argument " + std::to_string(i) +
                                      ": expected '" +
                                      typeToString(closure->closureParamTypes[i]) +
                                      "', got '" + typeToString(at) + "'", node);
                    }
                }
                node.isClosureCall = true;
                node.mangledCallee = closure->closureCallName;
                node.resolvedParamTypes = closure->closureParamTypes;
                node.type = closure->closureReturnType;
                node.isLValue = false;
                return;
            }
        }
    }

    // Indirect call through a function-pointer variable?
    if (Symbol* sym = lookup(node.callee)) {
        // 函数指针 typedef（typedef int32 (*BinOp)(...)）变量调用——剥 typedef。
        Type* st = sym->type;
        while (st && st->kind == TypeKind::Typedef)
            st = static_cast<TypedefType*>(st)->aliasedType;
        Type* pointee = st && st->kind == TypeKind::Pointer ? st->base : nullptr;
        while (pointee && pointee->kind == TypeKind::Typedef)
            pointee = static_cast<TypedefType*>(pointee)->aliasedType;
        if (st && st->kind == TypeKind::Pointer && pointee &&
            pointee->kind == TypeKind::Function) {
            auto* funcType = static_cast<FunctionType*>(pointee);
            for (auto& arg : node.args) getExprType(*arg);
            node.isIndirect = true;
            node.resolvedParamTypes = funcType->paramTypes;
            node.type = funcType->returnType;
            node.isLValue = false;
            return;
        }
    }

    // SEM-17: `obj(args)` where `obj` is an aggregate variable — call
    // operator() on the object.
    if (Symbol* sym = lookup(node.callee)) {
        Type* st = sym->type;
        while (st && st->kind == TypeKind::Typedef)
            st = static_cast<TypedefType*>(st)->aliasedType;
        if (isOperatorOverloadOperand(st)) {
            node.calleeExpr = std::make_unique<VariableExprAST>(node.callee);
            getExprType(*node.calleeExpr);
            std::vector<Type*> params{st};
            for (auto& arg : node.args) params.push_back(getExprType(*arg));
            OverloadSet* overloadSet = currentScope->lookupOverload("operator()");
            if (overloadSet && !overloadSet->empty()) {
                Symbol* resolved = overloadSet->resolve(params);
                if (resolved && resolved->type &&
                    resolved->type->kind == TypeKind::Function) {
                    node.mangledCallee = mangleFunction("operator()", params);
                    node.type = static_cast<FunctionType*>(resolved->type)->returnType;
                    node.isLValue = false;
                    return;
                }
            }

            // Member `operator()` — resolve against the class method table.
            if (st && st->kind == TypeKind::Class) {
                auto* classType = static_cast<ClassType*>(st);
                ClassType* definingClass = nullptr;
                std::vector<Type*> methodArgs(params.begin() + 1, params.end());
                Symbol* method = resolveMethod(classType, "operator()",
                                               methodArgs, &definingClass);
                if (method && method->type &&
                    method->type->kind == TypeKind::Function) {
                    AccessLevel level = definingClass->memberAccessLevel("operator()");
                    if (!canAccessMember(definingClass, level)) {
                        emitError(DiagnosticCode::SemPrivateMemberAccess,
                                  "cannot call operator() of class '" +
                                      definingClass->name + "' outside the class",
                                  node);
                        node.type = nullptr;
                        node.isLValue = false;
                        delete method;
                        return;
                    }
                    auto* ft = static_cast<FunctionType*>(method->type);
                    node.mangledCallee = mangleFunction("operator()", ft->paramTypes);
                    node.memberOperator = true;
                    node.type = ft->returnType;
                    node.isLValue = false;
                    delete method;
                    return;
                }
                delete method;
            }

            emitError("no matching operator() for type '" + typeToString(st) + "'", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
    }

    FunctionType* funcType = nullptr;
    node.type = checkFunctionCall(node.callee, node.args, node, &funcType);
    if (funcType) {
        node.resolvedParamTypes = funcType->paramTypes;
    }
    // P1-05 / ANN-05: deprecated 函数使用（W3004）。
    if (m_deprecatedFuncs.count(node.callee)) {
        std::string msg = m_deprecatedMsgs.count(node.callee)
            ? m_deprecatedMsgs[node.callee] : "";
        if (msg.empty()) {
            emitWarning(DiagnosticCode::WarnDeprecated,
                        "'" + node.callee + "' is deprecated", node);
        } else {
            emitWarning(DiagnosticCode::WarnDeprecated,
                        "'" + node.callee + "' is deprecated: " + msg, node);
        }
    }
    // P1-05 / ANN-05: nonnull 形参传字面空（W3005；null 字面量即 0）。
    auto npIt = m_nonnullParams.find(node.callee);
    if (npIt != m_nonnullParams.end()) {
        for (auto& [pidx, pname] : npIt->second) {
            if (pidx < node.args.size()) {
                if (auto* num = dynamic_cast<NumberExprAST*>(node.args[pidx].get())) {
                    if (num->value == 0) {
                        emitWarning(DiagnosticCode::WarnNullNonnull,
                                    "null passed to nonnull parameter '" + pname
                                        + "' of '" + node.callee + "'", node);
                    }
                }
            }
        }
    }
    node.isLValue = false;
}

// P1-03 / GEN-03/06: 函数模板调用。
// 显式实参 → 直接映射；否则从调用实参推导（形参 `T` 取实参类型、形参
// `T*` 取去指针类型）。同参冲突 / 无法推导 → 诊断。实例化 + visit 实例
// 后把 callee 重写为实例名，走普通调用解析。返回 true = 已处理。
bool SemanticAnalyzer::tryAnalyzeTemplateCall(CallExprAST& node) {
    auto& reg = TemplateRegistry::instance();
    TemplateDeclAST* tpl = reg.find(node.callee);
    if (!tpl) return false;
    auto* fnTpl = dynamic_cast<FunctionDeclAST*>(tpl->decl.get());
    if (!fnTpl) return false; // 类模板命中——让普通路径给出诊断

    // 实参类型先解析。
    std::vector<Type*> argTypes;
    for (auto& a : node.args) argTypes.push_back(getExprType(*a));

    // SEM-16 最小规则：无显式实参时，同名普通函数有可行匹配则优先。
    if (!node.hasTemplateArgs) {
        if (OverloadSet* overloads = currentScope->lookupOverload(node.callee)) {
            if (overloads->resolve(argTypes)) return false;
        }
    }

    // 评审 I1：显式实参定位后推导跳过——显式绑定单独记录（区别于推导
    // 绑定，后者仍须做冲突检测）。
    std::unordered_set<std::string> explicitlyBound;

    // typename/value 参数按声明顺序分配。
    std::vector<TemplateDeclAST::Param*> typeParams;
    std::vector<TemplateDeclAST::Param*> valueParams;
    for (auto& p : tpl->params) {
        if (p.isType) typeParams.push_back(const_cast<TemplateDeclAST::Param*>(&p));
        else valueParams.push_back(const_cast<TemplateDeclAST::Param*>(&p));
    }

    std::unordered_map<std::string, Type*> typeArgMap;
    std::unordered_map<std::string, long long> valueArgMap;
    // 显式实参。
    if (node.hasTemplateArgs) {
        if (node.explicitTemplateArgs.size() != typeParams.size() ||
            node.explicitTemplateValues.size() != valueParams.size()) {
            emitError("wrong number of template arguments for '" + node.callee + "' (expected " +
                          std::to_string(typeParams.size()) + " type(s), " +
                          std::to_string(valueParams.size()) + " value(s))",
                      node);
            return true;
        }
        for (size_t i = 0; i < typeParams.size(); ++i) {
            typeArgMap[typeParams[i]->name] = node.explicitTemplateArgs[i];
            explicitlyBound.insert(typeParams[i]->name);
        }
        for (size_t i = 0; i < valueParams.size(); ++i)
            valueArgMap[valueParams[i]->name] = node.explicitTemplateValues[i];
    }

    // 推导：逐位置匹配形参形状。
    auto deduce = [&](const std::string& name, Type* arg) -> bool {
        auto it = typeArgMap.find(name);
        if (it != typeArgMap.end()) {
            if (it->second != arg) {
                emitError("conflicting deduction for '" + name + "': '" +
                              typeToString(it->second) + "' vs '" + typeToString(arg) + "'",
                          node);
                return false;
            }
        } else {
            typeArgMap[name] = arg;
        }
        return true;
    };
    for (size_t i = 0; i < fnTpl->params.size() && i < argTypes.size(); ++i) {
        Type* p = fnTpl->params[i]->type;
        Type* a = argTypes[i];
        if (!a) continue;
        if (p && p->kind == TypeKind::TypeVar) {
            // 评审 I1：显式实参定位后推导跳过（plan 钉死：显式优先）。
            if (explicitlyBound.count(static_cast<TypeVarType*>(p)->name)) continue;
            if (!deduce(static_cast<TypeVarType*>(p)->name, a)) return true;
        } else if (p && p->kind == TypeKind::Pointer && p->base &&
                   p->base->kind == TypeKind::TypeVar) {
            if (explicitlyBound.count(static_cast<TypeVarType*>(p->base)->name)) continue;
            Type* aStripped = a;
            while (aStripped && aStripped->kind == TypeKind::Typedef)
                aStripped = static_cast<TypedefType*>(aStripped)->aliasedType;
            if (aStripped && aStripped->kind == TypeKind::Pointer) {
                if (!deduce(static_cast<TypeVarType*>(p->base)->name, aStripped->base))
                    return true;
            }
        }
    }

    // 未定的 typename 参数 → 诊断。
    for (auto* tp : typeParams) {
        if (!typeArgMap.count(tp->name)) {
            emitError("cannot deduce template argument for '" + tp->name + "'", node);
            return true;
        }
    }
    for (auto* vp : valueParams) {
        if (!valueArgMap.count(vp->name)) {
            emitError("cannot deduce template argument for '" + vp->name + "'", node);
            return true;
        }
    }

    // 实参顺序列表（按声明顺序）。
    std::vector<Type*> typeArgs;
    for (auto* tp : typeParams) typeArgs.push_back(typeArgMap[tp->name]);
    std::vector<long long> valueArgs;
    for (auto* vp : valueParams) valueArgs.push_back(valueArgMap[vp->name]);

    std::string instName = TemplateRegistry::instanceName(node.callee, typeArgs, valueArgs);
    auto* inst = reg.instantiateFunction(node.callee, typeArgs, valueArgs, argTypes);
    if (!inst) {
        emitError("cannot instantiate template '" + node.callee + "'", node);
        return true;
    }
    if (!m_visitedInstances.count(instName)) {
        m_visitedInstances.insert(instName);
        m_instStack.push_back(node.callee + "<...>");
        // 评审 C1：实例符号必须落在全局作用域——否则同一实例的第二个调用
        // 点（另一函数/另一块作用域）找不到声明。
        Scope* savedScope = currentScope;
        currentScope = globalScope.get();
        visit(*inst);
        currentScope = savedScope;
        m_instStack.pop_back();
    }

    // 调用点重写到实例符号，交普通调用解析（类型检查/绑定/codegen 名）。
    node.callee = instName;
    return false;
}

void SemanticAnalyzer::visit(AssignmentExprAST& node) {
    Type* lhsType = getExprType(*node.lhs);
    if (lhsType && lhsType->isConst) {
        emitError("cannot assign to const variable", node);
        return;
    }
    // PAR-08: assigning an array/slice literal (or a stack-backed slice
    // variable) to a global stores a pointer to stack storage — dangling.
    // By-value scalar/struct copies remain allowed (design §2.4).
    if (node.op == AssignOp::Assign) {
        if (rootsAtGlobalVariable(node.lhs.get(), m_globalVarNames)) {
            bool escape = false;
            if (auto* cl = dynamic_cast<CompoundLiteralExprAST*>(node.rhs.get())) {
                Type* lt = cl->literalType;
                while (lt && lt->kind == TypeKind::Typedef)
                    lt = static_cast<TypedefType*>(lt)->aliasedType;
                escape = lt && (lt->kind == TypeKind::Array ||
                                lt->kind == TypeKind::Slice);
            }
            if (!escape) {
                if (auto* rv = dynamic_cast<VariableExprAST*>(node.rhs.get())) {
                    escape = m_sliceLiteralVars.count(
                                 sliceLiteralKey(currentFunction, rv->name)) > 0;
                }
            }
            if (escape) {
                emitError("cannot store a stack-backed array/slice literal in a global (dangling)", node);
                return;
            }
        }
    }
    // CON-01: atomic LHS supports only `= += -= &= |= ^=` — the atomicrmw
    // subset. `*= /= %= <<= >>=` need a cmpxchg loop (CON-02, not built).
    if (lhsType && lhsType->isAtomic && node.op != AssignOp::Assign) {
        bool supported = node.op == AssignOp::AddAssign ||
                         node.op == AssignOp::SubAssign ||
                         node.op == AssignOp::BitAndAssign ||
                         node.op == AssignOp::BitOrAssign ||
                         node.op == AssignOp::BitXorAssign;
        if (!supported) {
            emitError(DiagnosticCode::SemAtomicBadRMWOp,
                      "operator not supported on atomic value (need CAS)", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
    }
    // 评审 I5: general assignability gate — an rvalue LHS (e.g. a call
    // result's pseudo-field) must not reach codegen as a store destination.
    // Skip when lhsType is null: resolution already diagnosed (e.g. E2009).
    if (node.lhs && lhsType && !node.lhs->isLValue) {
        emitError("expression is not assignable", node);
        node.type = nullptr;
        node.isLValue = false;
        return;
    }
    Type* rhsType = getExprType(*node.rhs);
    node.type = checkAssignmentTypes(lhsType, rhsType, node);
    node.isLValue = true;
}

void SemanticAnalyzer::visit(TernaryExprAST& node) {
    // P1-04 / CT-12: 表达式树含 compile_time 根 → 优先整树编译期折叠。
    if (containsCompileTimeRoot(&node)) {
        size_t baseErrors = getErrors().size();
        auto v = evalCompileTime(&node, node);
        if (v && (v->type == ConstValue::INT || v->type == ConstValue::DOUBLE)) {
            node.ctHandled = true;
            node.isLValue = false;
            if (v->type == ConstValue::INT) {
                node.ctInt = v->intVal;
                node.type = ctIntTypeFor(typeCtx, v->intVal);
            } else {
                node.ctFloat = v->doubleVal;
                node.type = typeCtx->getFloat64();
            }
            return;
        }
        if (v && v->type == ConstValue::STR) {
            // STR 逃逸到运行时上下文（spec §2 意图为类型错误）。
            emitError("compile-time string value cannot be used in runtime context", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // CHAR 有运行时类型：回退普通路径（子节点钩子已产常量）——评审 I2。
        if (getErrors().size() > baseErrors) {
            // 求值器已诊断（未知成员/深度超限）：就地报错，不回退。
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // 混入运行时操作数且无诊断：回退普通路径——CT 子节点由各自钩子
        // 求值成常量，本节点作普通运行时表达式处理。
    }

    Type* condType = getExprType(*node.cond);
    Type* thenType = getExprType(*node.then);
    Type* elseType = getExprType(*node.elseExpr);

    if (condType && !isConditionType(condType)) {
        emitError("ternary condition must be scalar or optional/result, but got '" + typeToString(condType) + "'", node);
    }

    // 三元根因轮：分支类型须兼容——非算术对且 typesCompatible 不成立时
    // 拒绝（旧 getCommonType 对异 kind 静默取左，`t ? s : 5` 混型不报错）。
    // Optional/Result 同型分支在此放行：codegen 已按值 phi（根因已修）。
    auto strippedBranch = [](Type* t) {
        while (t && t->kind == TypeKind::Typedef)
            t = static_cast<TypedefType*>(t)->aliasedType;
        return t;
    };
    Type* bl = strippedBranch(thenType);
    Type* br = strippedBranch(elseType);
    if (bl && br && !(isArithmeticType(bl) && isArithmeticType(br)) &&
        !typesCompatible(bl, br)) {
        emitError("ternary branches have incompatible types: '" +
                      typeToString(thenType) + "' and '" + typeToString(elseType) + "'",
                  node);
        node.type = nullptr;
        node.isLValue = false;
        return;
    }

    node.type = getCommonType(thenType, elseType);
    node.isLValue = false;
}

void SemanticAnalyzer::visit(CastExprAST& node) {
    // AGG-11/DS5: cast targets of private nested types are E2009 (the main
    // heap-allocation path is `(Outer::Secret*)malloc(...)`).
    node.castType = resolveTypeInstance(node.castType, node);
    checkNestedTypeAccess(node.castType, node);
    // P1-04 评审 I3: cast 目标不得来自 compile_time.if 死分支。
    checkCtDeadBranchUse(node.castType, node);
    Type* exprType = getExprType(*node.expr);
    if (exprType && node.castType) {
        Type* from = stripTypedef(exprType);
        Type* to = stripTypedef(node.castType);
        bool ok = true;
        const char* what = "cast";
        // Review 2.5: explicit interop cast from a view/owning string to a
        // raw char pointer. Implicit conversion is restricted separately.
        bool explicitViewToPointer =
            (from->kind == TypeKind::Str || from->kind == TypeKind::String) &&
            to->kind == TypeKind::Pointer;
        switch (node.castKind) {
            case CastKind::Static:
                // Arithmetic <-> arithmetic (incl. enum, which are integers) or
                // pointer/array <-> pointer/array. Pointer <-> integer must use
                // reinterpret_cast (or a C-style cast).
                ok = (isArithmeticType(from) && isArithmeticType(to)) ||
                     (isPointerOrArray(from) && isPointerOrArray(to)) ||
                     explicitViewToPointer;
                what = "static_cast";
                break;
            case CastKind::Reinterpret:
                // Any scalar <-> scalar: bit-level reinterpretation.
                ok = isScalarType(from) && isScalarType(to);
                what = "reinterpret_cast";
                break;
            case CastKind::CStyle:
            default:
                // A C-style cast is the explicit escape hatch: arithmetic <->
                // arithmetic (incl. enum, TYP-20), pointer/array <-> pointer/
                // array, and pointer <-> integer. Anything else keeps the
                // historical "incompatible cast" warning.
                ok = (isArithmeticType(from) && isArithmeticType(to)) ||
                     (isPointerOrArray(from) && isPointerOrArray(to)) ||
                     (isPointerOrArray(from) && isIntegerType(to)) ||
                     (isIntegerType(from) && isPointerOrArray(to)) ||
                     explicitViewToPointer ||
                     typesCompatible(exprType, node.castType);
                what = "cast";
                break;
        }
        if (!ok) {
            std::string msg;
            if (node.castKind == CastKind::CStyle) {
                // Preserve the historical C-style cast wording.
                msg = "incompatible cast from '" + typeToString(exprType) + "' to '" +
                      typeToString(node.castType) + "'";
                emitWarning(DiagnosticCode::SemIncompatibleCast, msg, node);
            } else {
                msg = std::string(what) + ": incompatible cast from '" +
                      typeToString(exprType) + "' to '" + typeToString(node.castType) + "'";
                emitError(DiagnosticCode::SemIncompatibleCast, msg, node);
            }
        }
    }
    node.type = node.castType;
    node.isLValue = false;
}

void SemanticAnalyzer::visit(CommaExprAST& node) {
    getExprType(*node.left);
    node.type = getExprType(*node.right);
    node.isLValue = node.right->isLValue;
}

void SemanticAnalyzer::visit(PostfixIncDecExprAST& node) {
    Type* operandType = getExprType(*node.operand);
    // SEM-17: postfix `x++`/`x--` on an aggregate → `operator++(V, int32)` /
    // `operator--(V, int32)` free function (C++ dummy-int convention).
    if (isOperatorOverloadOperand(operandType)) {
        std::string opName = node.isIncrement ? "operator++" : "operator--";
        OverloadSet* overloadSet = currentScope->lookupOverload(opName);
        if (overloadSet && !overloadSet->empty()) {
            Type* dummy = typeCtx->getInt32();
            Symbol* resolved = overloadSet->resolve({operandType, dummy});
            if (resolved && resolved->type &&
                resolved->type->kind == TypeKind::Function) {
                node.mangledCallee = mangleFunction(opName, {operandType, dummy});
                node.type = static_cast<FunctionType*>(resolved->type)->returnType;
                node.isLValue = false;
                return;
            }
        }
    }
    if (!isArithmeticType(operandType) && !isPointerOrArray(operandType)) {
        emitError("invalid operand to postfix '" + std::string(node.isIncrement ? "++" : "--") + "': '" + typeToString(operandType) + "'", node);
        node.type = nullptr;
    } else {
        node.type = operandType;
    }
    node.isLValue = false;
}

void SemanticAnalyzer::visit(ArrayAccessExprAST& node) {
    Type* arrayType = getExprType(*node.array);
    Type* indexType = getExprType(*node.index);

    // SEM-17: `v[i]` on an aggregate object → `operator[]` free function.
    if (isOperatorOverloadOperand(stripTypedef(arrayType))) {
        OverloadSet* overloadSet = currentScope->lookupOverload("operator[]");
        if (overloadSet && !overloadSet->empty()) {
            std::vector<Type*> params{arrayType, indexType};
            Symbol* resolved = overloadSet->resolve(params);
            if (resolved && resolved->type &&
                resolved->type->kind == TypeKind::Function) {
                node.mangledCallee = mangleFunction("operator[]", params);
                node.type = static_cast<FunctionType*>(resolved->type)->returnType;
                node.isLValue = false;
                return;
            }
        }

        // Member `operator[]` — resolve against the class's method table.
        Type* operand = stripTypedef(arrayType);
        if (operand && operand->kind == TypeKind::Class) {
            auto* classType = static_cast<ClassType*>(operand);
            ClassType* definingClass = nullptr;
            Symbol* method = resolveMethod(classType, "operator[]",
                                           {indexType}, &definingClass);
            if (method && method->type && method->type->kind == TypeKind::Function) {
                AccessLevel level = definingClass->memberAccessLevel("operator[]");
                if (!canAccessMember(definingClass, level)) {
                    emitError(DiagnosticCode::SemPrivateMemberAccess,
                              "cannot call operator[] of class '" +
                                  definingClass->name + "' outside the class",
                              node);
                    node.type = nullptr;
                    node.isLValue = false;
                    delete method;
                    return;
                }
                auto* ft = static_cast<FunctionType*>(method->type);
                node.mangledCallee = mangleFunction("operator[]", ft->paramTypes);
                node.memberOperator = true;
                node.type = ft->returnType;
                node.isLValue = false;
                delete method;
                return;
            }
            delete method;
        }

        emitError("no matching operator[] for types '" + typeToString(arrayType) +
                      "' and '" + typeToString(indexType) + "'", node);
        node.type = nullptr;
        node.isLValue = false;
        return;
    }

    if (!isIntegerType(indexType)) {
        emitError("array subscript must be integer, but got '" + typeToString(indexType) + "'", node);
    }

    // 数组 typedef（typedef int32 IntArr[4]）剥开再判种类。
    Type* strippedArray = stripTypedef(arrayType);
    if (strippedArray && strippedArray->kind == TypeKind::Array) {
        node.type = static_cast<ArrayType*>(strippedArray)->elementType;
    } else if (strippedArray && strippedArray->kind == TypeKind::Pointer) {
        node.type = strippedArray->base;
    } else if (strippedArray && strippedArray->kind == TypeKind::Slice) {
        // TYP-12: subscripting a slice yields an lvalue of the element type.
        node.type = static_cast<SliceType*>(strippedArray)->elementType;
    } else if (strippedArray && strippedArray->kind == TypeKind::String) {
        // P1-06 (FMT-02): subscripting a string yields a byte (char) lvalue;
        // no bounds check (DEC-06 policy).
        node.type = typeCtx->getChar();
    } else if (strippedArray && strippedArray->kind == TypeKind::Str) {
        // P1-06 (FMT-01): subscripting a string_view yields a byte (char)
        // lvalue——等价 char_at(i)。
        node.type = typeCtx->getChar();
    } else {
        emitError("subscripted value is neither array, slice nor pointer, but '" + typeToString(arrayType) + "'", node);
        node.type = nullptr;
    }
    node.isLValue = true;
}

void SemanticAnalyzer::visit(RangeExprAST& node) {
    Type* objType = getExprType(*node.object);
    if (!objType) {
        node.type = nullptr;
        node.isLValue = false;
        return;
    }
    if (node.begin) {
        Type* bt = getExprType(*node.begin);
        if (bt && !isIntegerType(bt)) {
            emitError("range begin must be an integer, got '" +
                          typeToString(bt) + "'", node);
        }
    }
    if (node.end) {
        Type* et = getExprType(*node.end);
        if (et && !isIntegerType(et)) {
            emitError("range end must be an integer, got '" +
                          typeToString(et) + "'", node);
        }
    }
    Type* st = stripTypedef(objType);
    if (!st) {
        node.type = nullptr;
        node.isLValue = false;
        return;
    }
    switch (st->kind) {
        case TypeKind::Str:
        case TypeKind::String:
            node.type = typeCtx->getStrType();
            break;
        case TypeKind::Array:
            node.type = typeCtx->getSliceType(
                static_cast<ArrayType*>(st)->elementType);
            break;
        case TypeKind::Slice:
            node.type = typeCtx->getSliceType(
                static_cast<SliceType*>(st)->elementType);
            break;
        default:
            emitError("range access requires an array, slice, string_view, or string, got '" +
                          typeToString(objType) + "'", node);
            node.type = nullptr;
            break;
    }
    node.isLValue = false;
}

void SemanticAnalyzer::visit(MemberAccessExprAST& node) {
    // P1-04 / CT-04/05: compile_time 成员链特判——先于对象解析（根标识符是
    // 编译期命名空间，不是普通变量，普通路径会误报 undeclared）。
    if (tryAnalyzeCompileTimeChain(node)) {
        return;
    }

    Type* objType = getExprType(*node.object);

    if (!objType) {
        node.type = nullptr;
        node.isLValue = false;
        return;
    }

    // P1-05 / ANN-05: 弃用字段使用（W3004）。
    {
        Type* stripped = objType;
        while (stripped && stripped->kind == TypeKind::Typedef) {
            stripped = static_cast<TypedefType*>(stripped)->aliasedType;
        }
        if (stripped && stripped->kind == TypeKind::Struct
            && static_cast<StructType*>(stripped)->fieldDeprecated.count(node.memberName)) {
            auto* st = static_cast<StructType*>(stripped);
            const std::string& msg = st->fieldDeprecatedMsg[node.memberName];
            if (msg.empty()) {
                emitWarning(DiagnosticCode::WarnDeprecated,
                            "'" + node.memberName + "' is deprecated", node);
            } else {
                emitWarning(DiagnosticCode::WarnDeprecated,
                            "'" + node.memberName + "' is deprecated: " + msg, node);
            }
        }
    }

    Type* memberBaseType = nullptr;
    // Typedefs to aggregates resolve to the underlying type (AGG-17).
    Type* strippedObj = nullptr;
    Type* strippedBase = nullptr;
    {
        Type* t = objType;
        while (t && t->kind == TypeKind::Typedef) {
            t = static_cast<TypedefType*>(t)->aliasedType;
        }
        strippedObj = t;
        if (t && t->kind == TypeKind::Pointer) {
            Type* b = t->base;
            while (b && b->kind == TypeKind::Typedef) {
                b = static_cast<TypedefType*>(b)->aliasedType;
            }
            strippedBase = b;
        }
    }
    // A field of an rvalue aggregate is an xvalue: readable, not assignable.
    // Arrow access (and dot auto-deref through a pointer) always names the
    // pointed-to object, so the field remains an lvalue.
    const bool pointerAccess =
        node.accessKind == MemberAccessKind::Arrow ||
        (strippedObj && strippedObj->kind == TypeKind::Pointer);
    const bool memberLValue = pointerAccess || node.object->isLValue;
    if (!strippedObj || (strippedObj->kind == TypeKind::Pointer && !strippedBase)) {
        node.type = nullptr;
        node.isLValue = false;
        return;
    }
    if (node.accessKind == MemberAccessKind::Arrow) {
        if (strippedObj->kind != TypeKind::Pointer) {
            emitError("member access with '->' requires pointer to struct/class, but got '" + typeToString(objType) + "'", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        if (strippedBase->kind != TypeKind::Struct && strippedBase->kind != TypeKind::Class &&
            strippedBase->kind != TypeKind::Union) {
            emitError("member access with '->' requires pointer to struct/class/union, but '" + typeToString(objType) + "' points to '" + typeToString(objType->base) + "'", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        memberBaseType = strippedBase;
    } else {
        // Review 2.2: property-first sequence API. `.len` works on arrays,
        // slices, string_view, and string; `.capacity` / `.char_count` /
        // `.data` are string properties. Method forms stay as aliases.
        if (strippedObj->kind == TypeKind::Array) {
            if (node.memberName == "len") {
                node.type = TypeContext::instance().getInt64();
                node.isLValue = false;
                return;
            }
            emitError("no member named '" + node.memberName + "' in array", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        if (strippedObj->kind == TypeKind::Str) {
            if (node.memberName == "len" || node.memberName == "char_count") {
                node.type = TypeContext::instance().getUSize();
                node.isLValue = false;
                return;
            }
            if (node.memberName == "data") {
                auto* ch = new Type(*TypeContext::instance().getChar());
                ch->isConst = true;
                node.type = new Type(TypeKind::Pointer, ch);
                node.isLValue = false;
                return;
            }
            emitError("no member named '" + node.memberName + "' in string_view", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        if (strippedObj->kind == TypeKind::String) {
            if (node.memberName == "len" || node.memberName == "capacity" ||
                node.memberName == "char_count") {
                node.type = TypeContext::instance().getUSize();
                node.isLValue = false;
                return;
            }
            if (node.memberName == "data") {
                node.type = new Type(TypeKind::Pointer,
                                     TypeContext::instance().getChar());
                node.isLValue = false;
                return;
            }
            emitError("no member named '" + node.memberName + "' in string", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // TYP-12: `.len` is the only member of a slice (D1: no `.ptr`).
        if (strippedObj->kind == TypeKind::Slice) {
            if (node.memberName == "len") {
                node.type = TypeContext::instance().getInt64();
                node.isLValue = false;
                return;
            }
            emitError("no member named '" + node.memberName + "' in slice", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // P1-02 (TYP-13/14): Optional/Result pseudo-fields — freely readable
        // and writable (DS3, C semantics); no runtime check is implied.
        if (strippedObj->kind == TypeKind::Optional) {
            auto* optType = static_cast<OptionalType*>(strippedObj);
            // 评审 I5: propagate object lvalue-ness — `make().value = 5` must
            // not present an rvalue slot as assignable.
            if (node.memberName == "valid") {
                node.type = TypeContext::instance().getBool();
                node.isLValue = node.object->isLValue;
                return;
            }
            if (node.memberName == "value") {
                node.type = optType->elementType;
                node.isLValue = node.object->isLValue;
                return;
            }
            emitError("no member named '" + node.memberName + "' in Optional", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        if (strippedObj->kind == TypeKind::Result) {
            auto* resType = static_cast<ResultType*>(strippedObj);
            if (node.memberName == "ok") {
                node.type = TypeContext::instance().getBool();
                node.isLValue = node.object->isLValue;
                return;
            }
            if (node.memberName == "value") {
                node.type = resType->successType;
                node.isLValue = node.object->isLValue;
                return;
            }
            if (node.memberName == "error") {
                node.type = resType->errorType;
                node.isLValue = node.object->isLValue;
                return;
            }
            emitError("no member named '" + node.memberName + "' in Result", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        // PAR-17: `this.field`/`p.field`——点号对指针自动解引用（沿用
        // methodcall decay 机制）；codegen 侧本就支持指针对象。
        if (node.accessKind == MemberAccessKind::Dot && strippedObj->kind == TypeKind::Pointer &&
            strippedBase && (strippedBase->kind == TypeKind::Struct ||
                             strippedBase->kind == TypeKind::Class ||
                             strippedBase->kind == TypeKind::Union)) {
            strippedObj = strippedBase;
        }
        if (strippedObj->kind != TypeKind::Struct && strippedObj->kind != TypeKind::Class &&
            strippedObj->kind != TypeKind::Union) {
            emitError("member access with '.' requires struct/class/union type, but got '" + typeToString(objType) + "'", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        memberBaseType = strippedObj;
    }

    if (memberBaseType->kind == TypeKind::Struct) {
        auto* structType = static_cast<StructType*>(memberBaseType);
        // INH-01: walk the base chain — inherited fields resolve at their
        // defining struct (mirrors the class branch below). Depth-capped:
        // a redefinition-shaped cycle must never hang the compiler (评审 C1).
        StructType* definingStruct = nullptr;
        int walkDepth = 0;
        for (StructType* cur = structType; cur && !definingStruct;) {
            // P1-07 / AGG-06: 物化后 fields 已被替换为伪字段（位域名丢失）——
            // 优先用物化前语义字段快照。
            const auto& lookupFields = !cur->semanticFields.empty()
                                           ? cur->semanticFields : cur->fields;
            for (auto& field : lookupFields) {
                if (field.name == node.memberName) {
                    node.type = field.type;
                    node.isLValue = memberLValue;
                    // P1-07 / SEM-12: bit-field access marker (isLValue stays
                    // true — the sema assignment gate depends on it; codegen
                    // returns the storage-unit address).
                    node.isBitField = field.bitWidth > 0;
                    node.bitWidth = static_cast<uint32_t>(field.bitWidth > 0 ? field.bitWidth : 0);
                    definingStruct = cur;
                    break;
                }
            }
            if (definingStruct) break;
            if (cur->baseClass.empty()) break;
            Type* baseType = cur->base;
            if (!baseType || baseType->kind != TypeKind::Struct) break;
            cur = static_cast<StructType*>(baseType);
            if (++walkDepth > 64) break;
        }
        if (definingStruct) return;
        emitError("no member named '" + node.memberName + "' in struct '" + structType->name + "'", node);
    } else if (memberBaseType->kind == TypeKind::Class) {
        auto* classType = static_cast<ClassType*>(memberBaseType);
        const std::string className = classType->name;
        ClassType* definingClass = nullptr;
        // Search this class and its base classes for the field.
        // Depth-capped: a redefinition-shaped cycle must never hang the
        // compiler (评审 C1).
        int classWalkDepth = 0;
        while (classType) {
            // P1-07 / AGG-06: 物化后 fields 已被替换为伪字段——优先用语义快照。
            const auto& lookupFields = !classType->semanticFields.empty()
                                           ? classType->semanticFields : classType->fields;
            for (auto& field : lookupFields) {
                if (field.name == node.memberName) {
                    node.type = field.type;
                    node.isLValue = memberLValue;
                    // P1-07 / SEM-12: bit-field access marker (see struct path).
                    node.isBitField = field.bitWidth > 0;
                    node.bitWidth = static_cast<uint32_t>(field.bitWidth > 0 ? field.bitWidth : 0);
                    definingClass = classType;
                    break;
                }
            }
            if (definingClass) break;
            Type* baseType = classType->base;
            if (!baseType || baseType->kind != TypeKind::Class) break;
            classType = static_cast<ClassType*>(baseType);
            if (++classWalkDepth > 64) break;
        }
        // SEM-04/DEC-01: non-public members are only reachable from inside
        // the defining class (protected ≡ private until INH lands).
        if (definingClass) {
            AccessLevel level = definingClass->memberAccessLevel(node.memberName);
            if (!canAccessMember(definingClass, level)) {
                emitError(DiagnosticCode::SemPrivateMemberAccess,
                          "cannot access " +
                              std::string(level == AccessLevel::Private ? "private" : "protected") +
                              " member '" + node.memberName + "' of class '" + definingClass->name +
                              "' outside the class; make it public or add an accessor",
                          node);
                node.type = nullptr;
                node.isLValue = false;
            }
            return;
        }
        emitError("no member named '" + node.memberName + "' in class '" + className + "'", node);
    } else if (memberBaseType->kind == TypeKind::Union) {
        auto* unionType = static_cast<UnionType*>(memberBaseType);
        for (auto& member : unionType->members) {
            if (member.name == node.memberName) {
                node.type = member.type;
                node.isLValue = memberLValue;
                // P1-07 / SEM-12: bit-field access marker (see struct path).
                node.isBitField = member.bitWidth > 0;
                node.bitWidth = static_cast<uint32_t>(member.bitWidth > 0 ? member.bitWidth : 0);
                return;
            }
        }
        emitError("no member named '" + node.memberName + "' in union '" + unionType->name + "'", node);
    }
    node.type = nullptr;
    node.isLValue = false;
}

void SemanticAnalyzer::visit(MethodCallExprAST& node) {
    // P1-04 / CT-01/02: compile_time 成员调用特判（先于对象解析，理由同链形）。
    if (tryAnalyzeCompileTimeCall(node)) {
        return;
    }

    // P1-06 (FMT-02): static `string.new` — `string` is a type name; detect
    // before object resolution (mirrors the compile_time chain special case).
    if (tryAnalyzeStringStaticCall(node)) {
        return;
    }

    Type* objType = getExprType(*node.object);
    if (!objType) {
        node.type = nullptr;
        node.isLValue = false;
        return;
    }

    // Review 2.2: `len()` stays as a compatibility alias on arrays/slices.
    {
        Type* seq = stripTypedef(objType);
        if (seq && (seq->kind == TypeKind::Array || seq->kind == TypeKind::Slice)) {
            if (node.methodName == "len" && node.args.empty()) {
                node.builtinMethod = BuiltinMethod::SequenceLen;
                node.type = typeCtx->getInt64();
                node.isLValue = false;
                return;
            }
            emitError("no member named '" + node.methodName + "' in sequence type", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
    }

    // Review 2.4: Optional/Result unpacking methods.
    {
        Type* ot = stripTypedef(objType);
        if (ot && ot->kind == TypeKind::Optional)
            return analyzeOptionalMethod(node, ot);
        if (ot && ot->kind == TypeKind::Result)
            return analyzeResultMethod(node, ot);
    }

    // P1-06 (FMT-01): str builtin methods (identifier-based builtins, not
    // class methods). object is an lvalue/rvalue str expression.
    if (objType->kind == TypeKind::Str) {
        return analyzeStrMethod(node, objType);
    }
    // P1-06 (FMT-02): string builtin methods.
    if (objType->kind == TypeKind::String) {
        return analyzeStringMethod(node, objType);
    }

    ClassType* classType = nullptr;
    if (objType->kind == TypeKind::Class) {
        classType = static_cast<ClassType*>(objType);
    } else if (objType->kind == TypeKind::Pointer && objType->base && objType->base->kind == TypeKind::Class) {
        classType = static_cast<ClassType*>(objType->base);
    }

    std::vector<Type*> argTypes;
    for (auto& arg : node.args) {
        Type* argType = getExprType(*arg);
        argTypes.push_back(argType);
    }

    // PAR-05: `s.f(args)` / `c.f(args)` where `f` is a function-pointer
    // aggregate field. This is an indirect call, not a class method.
    {
        auto strip = [](Type* t) {
            while (t && t->kind == TypeKind::Typedef)
                t = static_cast<TypedefType*>(t)->aliasedType;
            return t;
        };
        Type* owner = strip(objType);
        if (owner && owner->kind == TypeKind::Pointer) owner = strip(owner->base);
        Type* fieldType = nullptr;
        Type* accessOwner = owner;
        std::function<Type*(Type*, const std::string&)> findField =
            [&](Type* t, const std::string& name) -> Type* {
                t = strip(t);
                if (!t) return nullptr;
                if (t->kind == TypeKind::Struct) {
                    auto* st = static_cast<StructType*>(t);
                    if (Type* ft = st->getFieldType(name)) return ft;
                    if (!st->baseClass.empty()) {
                        if (auto* b = typeCtx->getStruct(st->baseClass))
                            if (Type* ft = findField(b, name)) return ft;
                    }
                } else if (t->kind == TypeKind::Class) {
                    auto* ct = static_cast<ClassType*>(t);
                    if (Type* ft = ct->getFieldType(name)) return ft;
                    if (!ct->baseClass.empty()) {
                        if (auto* b = typeCtx->getClass(ct->baseClass))
                            if (Type* ft = findField(b, name)) return ft;
                    }
                } else if (t->kind == TypeKind::Union) {
                    auto* ut = static_cast<UnionType*>(t);
                    for (auto& m : ut->members)
                        if (m.name == name) return m.type;
                }
                return nullptr;
            };
        fieldType = findField(owner, node.methodName);
        if (fieldType) {
            Type* ft = strip(fieldType);
            FunctionType* fnType = nullptr;
            if (ft && ft->kind == TypeKind::Pointer) {
                Type* pointee = strip(ft->base);
                if (pointee && pointee->kind == TypeKind::Function)
                    fnType = static_cast<FunctionType*>(pointee);
            } else if (ft && ft->kind == TypeKind::Function) {
                fnType = static_cast<FunctionType*>(ft);
            }
            if (fnType) {
                if (accessOwner && accessOwner->kind == TypeKind::Class) {
                    auto* ct = static_cast<ClassType*>(accessOwner);
                    AccessLevel level = ct->memberAccessLevel(node.methodName);
                    if (!canAccessMember(ct, level)) {
                        emitError(DiagnosticCode::SemPrivateMemberAccess,
                                  "cannot access " +
                                      std::string(level == AccessLevel::Private
                                                      ? "private" : "protected") +
                                      " member '" + node.methodName + "' of class '" +
                                      ct->name + "'",
                                  node);
                        node.type = nullptr;
                        node.isLValue = false;
                        return;
                    }
                }
                if (argTypes.size() != fnType->paramTypes.size()) {
                    emitError("function-pointer field '" + node.methodName +
                                  "' expects " + std::to_string(fnType->paramTypes.size()) +
                                  " arguments, got " + std::to_string(argTypes.size()),
                              node);
                    node.type = nullptr;
                    node.isLValue = false;
                    return;
                }
                for (size_t i = 0; i < argTypes.size(); ++i) {
                    checkAssignmentTypes(fnType->paramTypes[i], argTypes[i], *node.args[i]);
                }
                const bool wasLValue = node.object->isLValue;
                node.fieldCallee = std::make_unique<MemberAccessExprAST>(
                    MemberAccessKind::Dot, std::move(node.object), node.methodName);
                node.fieldCallee->type = fieldType;
                node.fieldCallee->isLValue = wasLValue;
                node.isFunctionPointerCall = true;
                node.resolvedParamTypes = fnType->paramTypes;
                node.type = fnType->returnType;
                node.isLValue = false;
                return;
            }
        }
    }

    if (!classType) {
        emitError("cannot call method on non-class type '" + typeToString(objType) + "'", node);
        node.type = nullptr;
        node.isLValue = false;
        return;
    }

    // INH-06（方案乙）: the class whose table provided the method (possibly a
    // base along the chain) — access levels attribute to it.
    ClassType* definingClass = nullptr;
    Symbol* method = resolveMethod(classType, node.methodName, argTypes, &definingClass);
    if (!method || method->type->kind != TypeKind::Function) {
        emitError("no matching method '" + node.methodName + "' in class '" + classType->name + "'", node);
        node.type = nullptr;
        node.isLValue = false;
        return;
    }
    // P1-03 / INH-05 / GEN-09: 基类实例方法体惰性——首次调用触发 visit
    // （this 插入 + body 检查），此时派生类方法表已完整。
    if (definingClass) visitLazyMethodsOf(definingClass->name);

    // SEM-04/DEC-01 + INH-06: non-public methods are only callable from    // inside the DEFINING class (which may be a base along the chain); the
    // level comes from the defining class's own member-access map.
    {
        AccessLevel level = definingClass->memberAccessLevel(node.methodName);
        if (!canAccessMember(definingClass, level)) {
            emitError(DiagnosticCode::SemPrivateMemberAccess,
                      "cannot call " +
                          std::string(level == AccessLevel::Private ? "private" : "protected") +
                          " method '" + node.methodName + "' of class '" + definingClass->name +
                          "' outside the class; make it public or add a public wrapper",
                      node);
            node.type = nullptr;
            node.isLValue = false;
            delete method;
            return;
        }
    }

    auto* funcType = static_cast<FunctionType*>(method->type);
    // Store the declared parameter types (incl. this) so codegen mangles
    // against the definition site — required for decayed arguments.
    node.resolvedParamTypes = funcType->paramTypes;
    // FUN-09: fold default values for trailing params not supplied.
    if (node.args.size() + 1 < funcType->paramTypes.size()) {
        auto dit = m_methodDecls.find(definingClass->name + "::" + node.methodName);
        if (dit != m_methodDecls.end() && dit->second) {
            auto* decl = dit->second;
            for (size_t i = node.args.size() + 1; i < funcType->paramTypes.size(); ++i) {
                DefaultArgValue dv;
                if (i < decl->params.size() && decl->params[i]->defaultValue) {
                    auto v = evaluateConstexpr(decl->params[i]->defaultValue.get());
                    if (v) {
                        switch (v->type) {
                            case ConstValue::INT:   dv.isInt = true; dv.intVal = v->intVal; break;
                            case ConstValue::DOUBLE: dv.isDouble = true; dv.doubleVal = v->doubleVal; break;
                            case ConstValue::CHAR:   dv.isChar = true; dv.charVal = v->charVal; break;
                            default: break;
                        }
                    }
                }
                node.defaultArgValues.push_back(dv);
            }
        }
    }
    node.type = funcType->returnType;
    node.isLValue = false;
    // P1-05 评审 I3: 弃用方法使用（W3004；键 = 裸成员名，跨类同名误报已挂账）。
    if (m_deprecatedFuncs.count(node.methodName)) {
        std::string msg = m_deprecatedMsgs.count(node.methodName)
            ? m_deprecatedMsgs[node.methodName] : "";
        if (msg.empty()) {
            emitWarning(DiagnosticCode::WarnDeprecated,
                        "'" + node.methodName + "' is deprecated", node);
        } else {
            emitWarning(DiagnosticCode::WarnDeprecated,
                        "'" + node.methodName + "' is deprecated: " + msg, node);
        }
    }
    delete method;
}

void SemanticAnalyzer::visit(SizeofExprAST& node) {
    // Resolve the type of a `sizeof(expr)` operand; the expression itself is
    // not evaluated, but its type is needed by codegen.
    node.sizeofType = resolveTypeInstance(node.sizeofType, node);
    if (!node.sizeofType && node.expr) {
        node.sizeofType = getExprType(*node.expr);
        if (!node.sizeofType) {
            emitError("invalid operand to sizeof", node);
        }
    }
    node.type = node.isAlignOf ? typeCtx->getUSize() : typeCtx->getInt32();
    node.isLValue = false;
}

// PAR-14: `offsetof(T, member)` — resolves to usize; the field name is
// validated against the aggregate at codegen (layout query).
void SemanticAnalyzer::visit(OffsetOfExprAST& node) {
    // SG-03: subscript designators are constant expressions, folded here so
    // codegen sees the same resolved integer as before.
    for (auto& step : node.path) {
        if (!step.isIndex || !step.indexExpr) continue;
        auto folded = evaluateConstexpr(step.indexExpr.get());
        if (!folded || folded->type != ConstValue::INT || folded->intVal < 0) {
            emitError("offsetof array index must be a non-negative constant expression", node);
            node.type = nullptr;
            node.isLValue = false;
            return;
        }
        step.index = folded->intVal;
    }
    node.type = typeCtx->getUSize();
    node.isLValue = false;
}

void SemanticAnalyzer::visit(InitializerListExprAST& node) {
    // Found issue: 此前只 sema 首元素——嵌套 brace-init（如 struct 字段
    // optional/result 的 `{ { true, string.new("data") } }`）其余元素
    // （`string.new` 静态调用）未解析 → codegen 时 builtinMethod 空 → 回退
    // codegen object → `unknown variable: string`。与其它表达式 visit 一致，
    // 递归所有元素；node.type 仍取首元素类型（既有启发）。
    if (!node.initializers.empty()) {
        node.type = getExprType(*node.initializers[0]);
        for (size_t i = 1; i < node.initializers.size(); ++i) {
            getExprType(*node.initializers[i]);
        }
    } else {
        node.type = typeCtx->getInt32();
    }
    node.isLValue = false;
}

// PAR-08: C99 compound literal `(T){...}` — analyze the initializer list
// against the target type (mirror VarDeclAST init-list analysis), then treat
// the node as an object of that type.
void SemanticAnalyzer::visit(CompoundLiteralExprAST& node) {
    if (!node.literalType || !node.initList) {
        node.type = nullptr;
        node.isLValue = false;
        return;
    }
    Type* lt = node.literalType;
    while (lt && lt->kind == TypeKind::Typedef)
        lt = static_cast<TypedefType*>(lt)->aliasedType;

    // Scalar target: `(int32){5}` is equivalent to `(int32)5` (design §2.4).
    // Only the first initializer is meaningful; extra elements are diagnosed
    // as too many (C semantics).
    if (lt && (lt->kind == TypeKind::Int8 || lt->kind == TypeKind::Int16 ||
               lt->kind == TypeKind::Int32 || lt->kind == TypeKind::Int64 ||
               lt->kind == TypeKind::Int128 || lt->kind == TypeKind::UInt8 ||
               lt->kind == TypeKind::UInt16 || lt->kind == TypeKind::UInt32 ||
               lt->kind == TypeKind::UInt64 || lt->kind == TypeKind::UInt128 ||
               lt->kind == TypeKind::Char || lt->kind == TypeKind::Bool ||
               lt->kind == TypeKind::Float16 || lt->kind == TypeKind::Float32 ||
               lt->kind == TypeKind::Float64 || lt->kind == TypeKind::Float128 ||
               lt->kind == TypeKind::ISize || lt->kind == TypeKind::USize)) {
        if (node.initList->initializers.size() > 1) {
            emitError("too many initializers for scalar compound literal", node);
        }
        if (!node.initList->initializers.empty()) {
            getExprType(*node.initList->initializers[0]);
        }
        node.type = node.literalType;
        node.isLValue = false;
        return;
    }

    // Aggregate target: analyze the init list exactly like a variable init.
    node.initList->type = node.literalType;
    node.initList->isLValue = false;
    for (auto& e : node.initList->initializers) getExprType(*e);
    resolveDesignators(*node.initList, node.literalType, "<compound literal>");

    // Slice target: `(int32[]){1,2,3}` — deferred element/len handling lives
    // with slice-literal init (Task 5); sema here only types it as the slice.
    if (lt && lt->kind == TypeKind::Slice) {
        node.type = node.literalType;
        node.isLValue = false;
        return;
    }

    node.type = node.literalType;
    // An aggregate compound literal is a temporary object (addressable for
    // array decay / struct value copy).
    node.isLValue = true;
}

// PAR-11: `[=]` 全部按值捕获——收集 body 表达式引用的变量名。
void SemanticAnalyzer::collectUsedNames(ExprAST* expr, std::unordered_set<std::string>& out) {
    if (!expr) return;
    if (auto* v = dynamic_cast<VariableExprAST*>(expr)) {
        out.insert(v->name);
        return;
    }
    if (auto* b = dynamic_cast<BinaryExprAST*>(expr)) {
        collectUsedNames(b->left.get(), out);
        collectUsedNames(b->right.get(), out);
        return;
    }
    if (auto* u = dynamic_cast<UnaryExprAST*>(expr)) {
        collectUsedNames(u->operand.get(), out);
        return;
    }
    if (auto* c = dynamic_cast<CallExprAST*>(expr)) {
        for (auto& a : c->args) collectUsedNames(a.get(), out);
        return;
    }
    if (auto* m = dynamic_cast<MethodCallExprAST*>(expr)) {
        collectUsedNames(m->object.get(), out);
        for (auto& a : m->args) collectUsedNames(a.get(), out);
        return;
    }
    if (auto* ma = dynamic_cast<MemberAccessExprAST*>(expr)) {
        collectUsedNames(ma->object.get(), out);
        return;
    }
    if (auto* t = dynamic_cast<TernaryExprAST*>(expr)) {
        collectUsedNames(t->cond.get(), out);
        collectUsedNames(t->then.get(), out);
        collectUsedNames(t->elseExpr.get(), out);
        return;
    }
    if (auto* a = dynamic_cast<AssignmentExprAST*>(expr)) {
        collectUsedNames(a->lhs.get(), out);
        collectUsedNames(a->rhs.get(), out);
        return;
    }
    if (auto* s = dynamic_cast<SizeofExprAST*>(expr)) {
        collectUsedNames(s->expr.get(), out);
        return;
    }
    if (auto* il = dynamic_cast<InitializerListExprAST*>(expr)) {
        for (auto& e : il->initializers) collectUsedNames(e.get(), out);
        return;
    }
    if (auto* lc = dynamic_cast<LambdaCallExprAST*>(expr)) {
        for (auto& a : lc->args) collectUsedNames(a.get(), out);
        return;
    }
    if (auto* lam = dynamic_cast<LambdaExprAST*>(expr)) {
        for (auto& stmt : lam->body->stmts) collectUsedNamesStmt(stmt.get(), out);
        return;
    }
}

void SemanticAnalyzer::collectUsedNamesStmt(StmtAST* stmt, std::unordered_set<std::string>& out) {
    if (!stmt) return;
    if (auto* es = dynamic_cast<ExprStmtAST*>(stmt)) {
        collectUsedNames(es->expr.get(), out);
        return;
    }
    if (auto* rs = dynamic_cast<ReturnStmtAST*>(stmt)) {
        collectUsedNames(rs->value.get(), out);
        return;
    }
    if (auto* cs = dynamic_cast<CompoundStmtAST*>(stmt)) {
        for (auto& s : cs->stmts) collectUsedNamesStmt(s.get(), out);
        return;
    }
    if (auto* is = dynamic_cast<IfStmtAST*>(stmt)) {
        collectUsedNames(is->cond.get(), out);
        collectUsedNamesStmt(is->thenStmt.get(), out);
        collectUsedNamesStmt(is->elseStmt.get(), out);
        return;
    }
    if (auto* ws = dynamic_cast<WhileStmtAST*>(stmt)) {
        collectUsedNames(ws->cond.get(), out);
        collectUsedNamesStmt(ws->body.get(), out);
        return;
    }
    if (auto* dws = dynamic_cast<DoWhileStmtAST*>(stmt)) {
        collectUsedNames(dws->cond.get(), out);
        collectUsedNamesStmt(dws->body.get(), out);
        return;
    }
    if (auto* fs = dynamic_cast<ForStmtAST*>(stmt)) {
        collectUsedNamesStmt(fs->init.get(), out);
        collectUsedNames(fs->cond.get(), out);
        collectUsedNames(fs->inc.get(), out);
        collectUsedNamesStmt(fs->body.get(), out);
        return;
    }
    if (auto* ds = dynamic_cast<DeclStmtAST*>(stmt)) {
        if (ds->decl) collectUsedNamesDecl(ds->decl.get(), out);
        return;
    }
}

void SemanticAnalyzer::collectUsedNamesDecl(DeclAST* decl, std::unordered_set<std::string>& out) {
    if (!decl) return;
    if (auto* v = dynamic_cast<VarDeclAST*>(decl)) {
        collectUsedNames(v->initExpr.get(), out);
        return;
    }
    if (auto* a = dynamic_cast<ArrayDeclAST*>(decl)) {
        collectUsedNames(a->initExpr.get(), out);
        return;
    }
}

// PAR-11: lambda 表达式语义。无捕获 → 函数指针类型；有捕获 → 闭包 struct。
void SemanticAnalyzer::visit(LambdaExprAST& node) {
    // getExprType may revisit an already-analyzed initializer (e.g. `auto`
    // inference followed by the normal initializer check). Keep the closure
    // type stable — a second visit must not mint `__lambda___anon_1`.
    if (node.type) return;

    // 1) 参数类型。
    std::vector<Type*> paramTypes;
    for (auto& p : node.params) {
        p->type = resolveTypeInstance(p->type, node);
        paramTypes.push_back(p->type);
    }

    // 2) 捕获解析：`[=]` 扫描 body 引用的局部/参数名；显式列出则逐个查找。
    std::vector<LambdaExprAST::Capture> effective;
    if (node.captureAll) {
        // 扫描 body 全部语句/表达式引用的变量名（含 return/if/循环）。
        std::unordered_set<std::string> used;
        if (node.body) {
            for (auto& stmt : node.body->stmts) {
                collectUsedNamesStmt(stmt.get(), used);
            }
        }
        for (auto& n : used) {
            LambdaExprAST::Capture c;
            c.name = n;
            effective.push_back(std::move(c));
        }
        node.captures = effective;  // 写回供 codegen 使用
    } else {
        for (auto& c : node.captures) effective.push_back(c);
    }

    // 3) 返回类型：显式或从 body 推断（推断在 body 作用域内，参数/捕获已声明）。
    Type* retType = node.returnType;

    // 4) 闭包类型：无捕获 → 函数指针；有捕获 → 闭包 struct。
    //    需先建闭包（字段类型引用外层符号），再推断返回类型（body 需要
    //    闭包字段可解析），最后分析 body。
    if (effective.empty()) {
        node.type = nullptr;  // 暂置，codegen 前由下面 FunctionType 决定
        node.closureType = nullptr;
    } else {
        std::string closureKey = "__lambda_" + typeCtx->nextAnonId();
        auto* closure = new StructType(closureKey);
        for (auto& c : effective) {
            Symbol* sym = currentScope ? currentScope->lookup(c.name) : nullptr;
            if (!sym || !sym->type) {
                emitError("unknown capture '" + c.name + "' in lambda", node);
                continue;
            }
            Type* capType = c.byRef ? new Type(TypeKind::Pointer, sym->type) : sym->type;
            closure->addField(c.name, capType);
        }
        typeCtx->addStruct(closureKey, closure);
        node.closureName = closureKey;
        node.closureType = closure;
    }

    // 5) 分析 body：入作用域，声明参数与捕获，再推断返回类型、访问 body。
    if (node.body) {
        // visit(ReturnStmtAST) 读 currentFunction->returnType——临时挂一个
        // 伪函数承载返回类型检查（lambda 返回类型已推断）。注意 FunctionDeclAST
        // 构造会 move 掉 params/body，故传局部空容器，body 仍由 node 持有。
        FunctionDeclAST* prev = currentFunction;
        std::vector<std::unique_ptr<ParamDeclAST>> emptyParams;
        std::unique_ptr<CompoundStmtAST> emptyBody;
        FunctionDeclAST tmpFn("<lambda>", retType ? retType : typeCtx->getVoid(),
                              emptyParams, emptyBody, false);
        currentFunction = &tmpFn;
        enterScope();
        for (auto& p : node.params) {
            if (!declare(p->name, p->type)) {
                emitError("redeclaration of lambda parameter '" + p->name + "'", *p);
            }
        }
        // 捕获按声明为局部（使 body 内引用 name 可解析）。
        for (auto& c : effective) {
            if (currentScope->lookup(c.name)) continue;  // 与参数同名冲突
            Symbol* sym = nullptr;
            // 从外层作用域解析捕获类型。
            for (Scope* s = currentScope->parent; s; s = s->parent) {
                if ((sym = s->lookup(c.name))) break;
            }
            if (sym && sym->type) {
                declare(c.name, sym->type);
            }
        }
        // 推断返回类型：在 body 作用域内取首个 return 表达式类型。
        if (!retType && node.body) {
            for (auto& stmt : node.body->stmts) {
                if (auto* rs = dynamic_cast<ReturnStmtAST*>(stmt.get())) {
                    if (rs->value) {
                        retType = getExprType(*rs->value);
                        break;
                    }
                }
            }
            if (!retType) retType = typeCtx->getVoid();
            node.returnType = retType;
            tmpFn.returnType = retType;  // 同步伪函数的返回类型供 return 检查
        }
        visit(*node.body);
        exitScope();
        currentFunction = prev;
    }
    if (!retType) retType = typeCtx->getVoid();
    node.returnType = retType;

    // 6) 表达式类型落定。
    auto* fnType = new FunctionType(retType, paramTypes);
    if (node.closureType) {
        auto* closure = static_cast<StructType*>(node.closureType);
        closure->isClosure = true;
        closure->closureCallName = closure->name + "__call";
        closure->closureParamTypes = paramTypes;
        closure->closureReturnType = retType;
        node.lambdaFnName = closure->closureCallName;
        node.type = node.closureType;
    } else {
        node.type = new Type(TypeKind::Pointer, fnType);
    }
    node.isLValue = false;
}

// PAR-11: 立即调用 `[...](...){...}(args)` —— 校验实参，类型 = 返回类型。
void SemanticAnalyzer::visit(LambdaCallExprAST& node) {
    getExprType(*node.lambda);
    Type* retType = node.lambda->returnType;
    if (!retType) retType = typeCtx->getVoid();
    node.type = retType;
    node.isLValue = false;

    // 实参数量/类型与形参匹配（复用函数调用检查的宽松路径）。
    size_t np = node.lambda->params.size();
    if (node.args.size() != np) {
        emitError("lambda call expects " + std::to_string(np) + " argument(s), got " +
                      std::to_string(node.args.size()),
                  node);
        return;
    }
    for (size_t i = 0; i < np; ++i) {
        Type* argType = getExprType(*node.args[i]);
        Type* paramType = node.lambda->params[i]->type;
        if (argType && paramType && !typesCompatible(paramType, argType)) {
            emitError("lambda argument " + std::to_string(i) + ": expected '" +
                          typeToString(paramType) + "', got '" + typeToString(argType) + "'",
                      node);
        }
    }
}


// PAR-13: struct/class 类型侧语义字段序（物化优先，未物化用原始序）。
static std::vector<FieldInfo>* semanticFieldList(Type* t) {
    if (t->kind == TypeKind::Struct) {
        auto* st = static_cast<StructType*>(t);
        return st->semanticFields.empty() ? &st->fields : &st->semanticFields;
    }
    if (t->kind == TypeKind::Class) {
        auto* ct = static_cast<ClassType*>(t);
        return ct->semanticFields.empty() ? &ct->fields : &ct->semanticFields;
    }
    return nullptr;
}

bool SemanticAnalyzer::foldDesignatorIndex(ExprAST* expr, int64_t& out, ASTNode& at) {
    if (!expr) return false;
    auto v = ctEval().eval(expr, at);
    if (!v) return false;
    if (v->type != CompileTimeEvaluator::ConstValue::INT) return false;
    out = v->intVal;
    return true;
}

int SemanticAnalyzer::inferArrayInitSize(InitializerListExprAST& node) {
    int64_t maxIdx = -1;
    size_t cursor = 0;
    for (size_t i = 0; i < node.initializers.size(); ++i) {
        bool hasDs = i < node.designators.size() && !node.designators[i].empty();
        int64_t idx;
        if (!hasDs) {
            idx = (int64_t)cursor;
        } else {
            const auto& dspec = node.designators[i];
            // 顶层数组初始化：设计符链首节为 Index → 该下标即顶层维度位置
            //（链可含多节，如 `[1][2]=5` 命中外层维度 1）。非 Index 首节
            //（非法，由 resolveDesignators 报错）推断保守取游标。
            if (!dspec.empty() && dspec[0].kind == Designator::Kind::Index &&
                foldDesignatorIndex(dspec[0].indexExpr.get(), idx, node)) {
                // idx 已折叠
            } else {
                idx = (int64_t)cursor;
            }
        }
        if (idx > maxIdx) maxIdx = idx;
        cursor = (size_t)(idx + 1);
    }
    return (maxIdx < 0) ? 0 : (int)(maxIdx + 1);
}

void SemanticAnalyzer::resolveDesignators(InitializerListExprAST& node, Type* aggType,
                                          const std::string& ctxName) {
    aggType = stripTypedef(aggType);
    if (!aggType) return;
    node.resolvedTargets.clear();
    node.resolvedTargets.resize(node.initializers.size());

    // Optional/Result：伪字段不作设计符目标——resolvedTargets 留空（codegen 走
    // 既有位置式路径）；含设计符 → 明确拒绝。
    if (aggType->kind == TypeKind::Optional || aggType->kind == TypeKind::Result) {
        for (size_t i = 0; i < node.initializers.size(); ++i) {
            if (i < node.designators.size() && !node.designators[i].empty()) {
                emitError("designator cannot target pseudo-field of optional/result; use a positional initializer",
                          node);
            }
        }
        return;
    }

    // PAR-08 (Task 5): Slice literal init `T[] s = {a,b,c}` — every positional
    // element must be assignable to the slice element type; length = list size
    // (codegen allocates a stack array of that length). Designators are not
    // supported on a slice literal.
    if (aggType->kind == TypeKind::Slice) {
        auto* st = static_cast<SliceType*>(aggType);
        Type* elemType = st->elementType;
        for (size_t i = 0; i < node.initializers.size(); ++i) {
            if (i < node.designators.size() && !node.designators[i].empty()) {
                emitError("designator cannot target a slice element; use a positional initializer",
                          node);
            }
            Type* it = getExprType(*node.initializers[i]);
            if (it && !typesCompatible(elemType, it)) {
                emitError("slice element type mismatch: expected '" + typeToString(elemType) +
                          "', got '" + typeToString(it) + "'", node);
            }
        }
        return;
    }

    size_t structCursor = 0; // struct/class 语义字段游标（跳过 name==""）
    size_t arrayCursor = 0;  // 数组下标游标

    for (size_t i = 0; i < node.initializers.size(); ++i) {
        auto& item = *node.initializers[i];
        const bool hasDs = i < node.designators.size() && !node.designators[i].empty();
        Type* elemType = nullptr;
        std::vector<Designator> chain;

        if (!hasDs) {
            // 位置式：按游标推进（AGG-06：name=="" 伪字段不消费初始化值）。
            if (aggType->kind == TypeKind::Array) {
                auto* at = static_cast<ArrayType*>(aggType);
                if ((int64_t)arrayCursor >= at->size) {
                    emitError("too many elements in initializer for array '" + ctxName + "'", node);
                } else {
                    Designator d;
                    d.kind = Designator::Kind::Index;
                    d.index = (int64_t)arrayCursor;
                    chain.push_back(std::move(d));
                    elemType = at->elementType;
                }
                ++arrayCursor;
            } else if (aggType->kind == TypeKind::Struct || aggType->kind == TypeKind::Class) {
                auto* fields = semanticFieldList(aggType);
                while (structCursor < fields->size() && (*fields)[structCursor].name.empty())
                    ++structCursor;
                if (structCursor >= fields->size()) {
                    emitError("too many initializers for '" + ctxName + "'", node);
                } else {
                    auto& f = (*fields)[structCursor];
                    if (f.flexibleArray) {
                        emitError("flexible array member cannot be initialized in a brace-enclosed initializer", node);
                    } else {
                        Designator d;
                        d.kind = Designator::Kind::Field;
                        d.field = f.name;
                        chain.push_back(std::move(d));
                        elemType = f.type;
                    }
                    ++structCursor;
                }
            } else if (aggType->kind == TypeKind::Union) {
                auto* ut = static_cast<UnionType*>(aggType);
                if (!ut->members.empty()) {
                    Designator d;
                    d.kind = Designator::Kind::Field;
                    d.field = ut->members[0].name;
                    chain.push_back(std::move(d));
                    elemType = ut->members[0].type;
                }
            } else {
                emitError("initializer list cannot initialize type '" + typeToString(aggType) + "'", node);
            }
        } else {
            // 设计符链：逐链节解析。
            const auto& dspec = node.designators[i];
            Type* cur = aggType;
            bool chainOk = true;
            for (const auto& des : dspec) {
                if (des.kind == Designator::Kind::Field) {
                    if (cur->kind == TypeKind::Struct || cur->kind == TypeKind::Class) {
                        auto* fields = semanticFieldList(cur);
                        size_t found = SIZE_MAX;
                        for (size_t k = 0; k < fields->size(); ++k) {
                            if ((*fields)[k].name == des.field) { found = k; break; }
                        }
                        Type* fieldType = nullptr;
                        bool ownField = false;
                        if (found != SIZE_MAX) {
                            ownField = true;
                            fieldType = (*fields)[found].type;
                            if ((*fields)[found].flexibleArray) {
                                emitError("flexible array member cannot be initialized in a brace-enclosed initializer", node);
                                fieldType = nullptr;
                            }
                        } else {
                            // INH: 沿 base 链查找（含访问控制 E2009）。
                            Type* walk = cur;
                            int depth = 0;
                            while (walk && (walk->kind == TypeKind::Struct || walk->kind == TypeKind::Class)) {
                                auto* wf = semanticFieldList(walk);
                                size_t wk = SIZE_MAX;
                                for (size_t k = 0; k < wf->size(); ++k) {
                                    if ((*wf)[k].name == des.field) { wk = k; break; }
                                }
                                if (wk != SIZE_MAX) {
                                    fieldType = (*wf)[wk].type;
                                    // 访问控制：非公有成员仅定义类内可写。
                                    if (walk->kind == TypeKind::Class) {
                                        auto* ct = static_cast<ClassType*>(walk);
                                        AccessLevel lvl = ct->memberAccessLevel(des.field);
                                        if (!canAccessMember(ct, lvl)) {
                                            emitError(DiagnosticCode::SemPrivateMemberAccess,
                                                      "cannot access " +
                                                          std::string(lvl == AccessLevel::Private ? "private" : "protected") +
                                                          " member '" + des.field + "' of class '" + ct->name +
                                                          "' outside the class; make it public or add an accessor",
                                                      node);
                                            fieldType = nullptr;
                                        }
                                    }
                                    break;
                                }
                                if (walk->kind == TypeKind::Struct) {
                                    auto* st = static_cast<StructType*>(walk);
                                    if (st->baseClass.empty() || !st->base) break;
                                    walk = st->base;
                                } else {
                                    auto* ct = static_cast<ClassType*>(walk);
                                    if (ct->baseClass.empty() || !ct->base) break;
                                    walk = ct->base;
                                }
                                if (++depth > 64) break;
                            }
                        }
                        if (!fieldType) {
                            if (ownField) {
                                // flexibleArray 已报，不重复。
                            } else {
                                emitError("no member named '" + des.field + "' in '" + typeToString(cur) + "'", node);
                            }
                            chainOk = false;
                            break;
                        }
                        Designator d;
                        d.kind = Designator::Kind::Field;
                        d.field = des.field;
                        chain.push_back(std::move(d));
                        // 续位：自身字段 → 之后继续；基类字段 → 派生字段 0 起。
                        if (cur == aggType) {
                            if (ownField) structCursor = found + 1;
                            else structCursor = 0;
                        }
                        cur = stripTypedef(fieldType);
                    } else if (cur->kind == TypeKind::Union) {
                        auto* ut = static_cast<UnionType*>(cur);
                        Type* fieldType = nullptr;
                        for (auto& m : ut->members) {
                            if (m.name == des.field) { fieldType = m.type; break; }
                        }
                        if (!fieldType) {
                            emitError("no member named '" + des.field + "' in '" + typeToString(cur) + "'", node);
                            chainOk = false;
                            break;
                        }
                        Designator d;
                        d.kind = Designator::Kind::Field;
                        d.field = des.field;
                        chain.push_back(std::move(d));
                        cur = stripTypedef(fieldType);
                    } else {
                        emitError("cannot apply designator '.' to non-aggregate type '" + typeToString(cur) + "'", node);
                        chainOk = false;
                        break;
                    }
                } else {
                    // Index 设计符
                    if (cur->kind != TypeKind::Array) {
                        emitError("cannot apply designator '[' to non-array type '" + typeToString(cur) + "'", node);
                        chainOk = false;
                        break;
                    }
                    auto* at = static_cast<ArrayType*>(cur);
                    int64_t idx = 0;
                    if (!foldDesignatorIndex(des.indexExpr.get(), idx, node)) {
                        emitError("designator index must be a constant expression", node);
                        chainOk = false;
                        break;
                    }
                    if (idx < 0 || idx >= at->size) {
                        emitError("array designator index " + std::to_string(idx) +
                                      " is out of bounds for size " + std::to_string(at->size),
                                  node);
                        chainOk = false;
                        break;
                    }
                    Designator d;
                    d.kind = Designator::Kind::Index;
                    d.index = idx;
                    chain.push_back(std::move(d));
                    if (cur == aggType) arrayCursor = (size_t)idx + 1;
                    cur = stripTypedef(at->elementType);
                }
            }
            if (chainOk) elemType = cur;
        }

        if (chain.empty()) continue;
        node.resolvedTargets[i] = std::move(chain);

        // 嵌套列表递归：目标类型 = 链末元素类型。
        if (auto* nested = dynamic_cast<InitializerListExprAST*>(&item)) {
            nested->type = elemType;
            nested->isLValue = false;
            for (auto& e : nested->initializers) getExprType(*e);
            resolveDesignators(*nested, elemType, ctxName);
        }
    }
}

void SemanticAnalyzer::checkFlexibleArrayConstraint(StructDeclAST& node) {
    for (size_t i = 0; i < node.fields.size(); ++i) {
        auto& f = node.fields[i];
        if (!f.flexibleArray) continue;
        if (node.isClassDecl) {
            emitError("flexible array member is not allowed in a class", node);
            f.flexibleArray = false;
            continue;
        }
        if (i != node.fields.size() - 1) {
            emitError("flexible array member must be the last member of a struct", node);
            f.flexibleArray = false;
            continue;
        }
        if (f.name.empty()) {
            emitError("flexible array member must be named", node);
            f.flexibleArray = false;
            continue;
        }
        if (f.bitWidth != 0) {
            emitError("flexible array member cannot be a bit-field", node);
            f.flexibleArray = false;
            continue;
        }
        bool hasNamedBefore = false;
        for (size_t k = 0; k < i; ++k) {
            if (!node.fields[k].name.empty()) { hasNamedBefore = true; break; }
        }
        if (!hasNamedBefore) {
            emitError("flexible array member requires at least one preceding named member", node);
            f.flexibleArray = false;
        }
    }
}

void SemanticAnalyzer::visit(CompoundStmtAST& node) {
    enterScope();
    typeCtx->pushLocalTypedefScope();
    bool terminated = false;
    bool warned = false;
    for (auto& stmt : node.stmts) {
        if (!stmt) {
            continue;
        }
        // SEM-11: statements after an unconditional transfer are unreachable
        // (W3003). Warn once per block; trailing `defer`s stay legal.
        if (terminated && !warned && !dynamic_cast<DeferStmtAST*>(stmt.get())) {
            emitWarning(DiagnosticCode::WarnUnreachableCode, "unreachable code", *stmt);
            warned = true;
        }
        visit(*stmt);
        if (stmtAlwaysTransfers(stmt.get())) {
            terminated = true;
        }
    }
    typeCtx->popLocalTypedefScope();
    exitScope();
}

void SemanticAnalyzer::visit(ExprStmtAST& node) {
    if (node.expr) {
        getExprType(*node.expr);
    }
}

void SemanticAnalyzer::visit(ReturnStmtAST& node) {
    if (node.value) {
        Type* retValType = getExprType(*node.value);
        if (currentFunction && retValType) {
            // PAR-08: returning an array/slice compound literal escapes the
            // temporary — dangling view/pointer. Reject (design §2.4).
            if (auto* cl = dynamic_cast<CompoundLiteralExprAST*>(node.value.get())) {
                Type* lt = cl->literalType;
                while (lt && lt->kind == TypeKind::Typedef)
                    lt = static_cast<TypedefType*>(lt)->aliasedType;
                if (lt && (lt->kind == TypeKind::Array || lt->kind == TypeKind::Slice)) {
                    emitError("cannot return a compound literal with local storage (dangling)", node);
                    return;
                }
            }
            // PAR-08 (Task 5): returning a slice whose storage is a stack
            // anonymous array is a dangling escape (design §2.5).
            if (auto* ve = dynamic_cast<VariableExprAST*>(node.value.get())) {
                if (m_sliceLiteralVars.count(
                        sliceLiteralKey(currentFunction, ve->name))) {
                    emitError("cannot return a slice backed by a stack literal (dangling)", node);
                    return;
                }
            }
            // TYP-11 D4: 返回局部数组的 slice 视图是悬垂 UB（栈帧随 return
            // 消亡）。当前所有 ArrayType 值均为局部变量，保守拒绝。
            Type* fnRet = currentFunction->returnType;
            while (fnRet && fnRet->kind == TypeKind::Typedef)
                fnRet = static_cast<TypedefType*>(fnRet)->aliasedType;
            Type* val = retValType;
            while (val && val->kind == TypeKind::Typedef)
                val = static_cast<TypedefType*>(val)->aliasedType;
            if (fnRet && val && fnRet->kind == TypeKind::Slice &&
                val->kind == TypeKind::Array) {
                emitError("cannot return a slice view of an array with local storage (dangling view)", node);
                return;
            }
            if (!typesCompatible(currentFunction->returnType, retValType)) {
                emitError("return type mismatch in function '" + std::string(currentFunction->name) + "': expected '" 
                    + typeToString(currentFunction->returnType) + "', got '" + typeToString(retValType) + "'", node);
            }
        }
    } else if (currentFunction && currentFunction->returnType->kind != TypeKind::Void) {
        emitError("non-void function '" + std::string(currentFunction->name) + "' must return a value", node);
    }
}

void SemanticAnalyzer::visit(IfStmtAST& node) {
    Type* condType = getExprType(*node.cond);
    if (condType && !isConditionType(condType)) {
        emitError("if condition must be scalar or optional/result, but got '" + typeToString(condType) + "'", node);
    }
    if (node.thenStmt) visit(*node.thenStmt);
    if (node.elseStmt) visit(*node.elseStmt);
}

void SemanticAnalyzer::visit(WhileStmtAST& node) {
    Type* condType = getExprType(*node.cond);
    if (condType && !isConditionType(condType)) {
        emitError("while condition must be scalar or optional/result, but got '" + typeToString(condType) + "'", node);
    }
    if (node.body) visit(*node.body);
}

void SemanticAnalyzer::visit(DoWhileStmtAST& node) {
    Type* condType = getExprType(*node.cond);
    if (condType && !isConditionType(condType)) {
        emitError("do-while condition must be scalar or optional/result, but got '" + typeToString(condType) + "'", node);
    }
    if (node.body) visit(*node.body);
}

void SemanticAnalyzer::visit(ForStmtAST& node) {
    enterScope();
    if (node.init) visit(*node.init);
    if (node.cond) {
        Type* condType = getExprType(*node.cond);
        if (condType && !isConditionType(condType)) {
            emitError("for condition must be scalar or optional/result, but got '" + typeToString(condType) + "'", node);
        }
    }
    if (node.inc) getExprType(*node.inc);
    if (node.body) visit(*node.body);
    exitScope();
}

void SemanticAnalyzer::visit(SwitchStmtAST& node) {
    Type* condType = getExprType(*node.cond);
    if (condType && !isIntegerType(condType)) {
        emitError("switch expression must be integer type, but got '" + typeToString(condType) + "'", node);
    }
    // Type the case labels so enumerator constants and constexpr values are
    // resolved (codegen then folds them to integer constants).
    for (auto& label : node.caseLabels) {
        if (label) getExprType(*label);
    }
    for (auto& c : node.cases) {
        if (c) visit(*c);
    }
}

void SemanticAnalyzer::visit(BreakStmtAST& node) {}

void SemanticAnalyzer::visit(ContinueStmtAST& node) {}

void SemanticAnalyzer::visit(NullStmtAST& node) {}

void SemanticAnalyzer::visit(VarDeclAST& node) {
    if (m_ctRepeatDepth > 0 && (node.isStaticLocal || node.isThreadLocal)) {
        emitError("static/thread_local local is not supported inside "
                  "compile_time.repeat",
                  node);
    }
    // Review 2.6: `auto` infers the variable type from its initializer.
    if (node.type && stripTypedef(node.type)->kind == TypeKind::Auto) {
        if (!node.initExpr) {
            emitError("'auto' variable '" + node.name +
                          "' must have an initializer", node);
            node.type = nullptr;
            return;
        }
        Type* inferred = getExprType(*node.initExpr);
        if (!inferred) {
            node.type = nullptr;
            return;
        }
        node.type = inferred;
    }
    // AGG-10: out-of-class static data definition. Attach its initializer to
    // the in-class static member and do not declare/emit a second global.
    if (node.isOutOfClassStatic) {
        std::string key = scopedName(mangleNamespaceName(
            node.staticClass + "::" + node.staticMember));
        auto it = staticDataDecls.find(key);
        if (it == staticDataDecls.end()) {
            emitError("static data member '" + node.staticClass + "::" +
                          node.staticMember + "' is not declared in its class",
                      node);
            return;
        }
        VarDeclAST* target = it->second;
        if (node.initExpr) {
            if (target->initExpr) {
                emitError(DiagnosticCode::SemRedefinition,
                          "redefinition of static data member '" + node.staticClass +
                              "::" + node.staticMember + "'",
                          node);
                return;
            }
            Type* initType = getExprType(*node.initExpr);
            if (initType && !typesCompatible(target->type, initType)) {
                emitError("type mismatch in initialization of '" + node.staticClass +
                              "::" + node.staticMember + "': expected '" +
                              typeToString(target->type) + "', got '" +
                              typeToString(initType) + "'",
                          node);
                return;
            }
            target->initExpr = std::move(node.initExpr);
        }
        return;
    }
    // CON-01: `atomic` is allowed only directly on an integer scalar type.
    // `atomic int32 x` is valid; `atomic int32* p` (atomic wrapped under a
    // pointer/array/slice) and `atomic float64 f` are rejected.
    if (node.type) {
        auto isCompositeLayer = [](Type* t) {
            return t->kind == TypeKind::Pointer || t->kind == TypeKind::Array ||
                   t->kind == TypeKind::Slice;
        };
        Type* outer = node.type;
        while (outer && outer->kind == TypeKind::Typedef)
            outer = static_cast<TypedefType*>(outer)->aliasedType;
        bool atomicUnderComposite = false;
        if (outer && isCompositeLayer(outer)) {
            // Dig through pointer/array/slice layers to find an atomic base.
            Type* inner = outer;
            while (inner && isCompositeLayer(inner)) {
                if (inner->kind == TypeKind::Pointer) inner = inner->base;
                else if (inner->kind == TypeKind::Array) inner = static_cast<ArrayType*>(inner)->elementType;
                else inner = static_cast<SliceType*>(inner)->elementType;
            }
            atomicUnderComposite = inner && inner->isAtomic;
        }
        if (atomicUnderComposite ||
            (outer && outer->isAtomic && !isIntegerType(outer))) {
            emitError(DiagnosticCode::SemAtomicNonScalar,
                      "atomic requires an integer scalar type (got '" +
                          typeToString(node.type) + "')",
                      node);
        }
    }
    // P1-05 / ANN-06: 变量注解验证。
    validateAnnotations(node.annotations, node, "variable");
    // P1-05 / ANN-05: 弃用类型使用（W3004）与弃用变量记录。
    checkDeprecatedTypeUse(node.type, node);
    for (const auto& ann : node.annotations) {
        if (ann.name == "deprecated") {
            std::string msg;
            if (ann.args.size() == 1 && ann.args[0].kind == AnnotationArg::Kind::String) {
                msg = ann.args[0].text;
            }
            m_deprecatedVars.insert(node.name);
            m_deprecatedMsgs["var:" + node.name] = msg;
        }
    }
    // P1-05 / ANN-03: [[align(N)]] 折叠至 declAlign（codegen setAlignment）。
    // 类型级 forcedAlign 传导（评审 I2）：变量未显式 align 时取聚合 forcedAlign。
    if (node.type) {
        Type* t = node.type;
        while (t && t->kind == TypeKind::Typedef) {
            t = static_cast<TypedefType*>(t)->aliasedType;
        }
        if (t) {
            // P1-05 终审 C1 教训：StructType/ClassType/UnionType 布局不同，
            // 禁止跨 kind 静态转换读取。
            uint64_t fa = 0;
            if (t->kind == TypeKind::Struct) {
                fa = static_cast<StructType*>(t)->forcedAlign;
            } else if (t->kind == TypeKind::Class) {
                fa = static_cast<ClassType*>(t)->forcedAlign;
            } else if (t->kind == TypeKind::Union) {
                fa = static_cast<UnionType*>(t)->forcedAlign;
            }
            if (fa) node.declAlign = fa;
        }
    }
    for (const auto& ann : node.annotations) {
        if (ann.name == "align" && ann.args.size() == 1
            && ann.args[0].kind == AnnotationArg::Kind::Expr && ann.args[0].expr) {
            if (auto v = evaluateConstexpr(ann.args[0].expr.get());
                v && v->type == ConstValue::INT) {
                node.declAlign = static_cast<uint64_t>(v->intVal);
            }
        }
    }
    // AGG-11/DS5: naming a private nested type outside its class is E2009.
    node.type = resolveTypeInstance(node.type, node);
    checkNestedTypeAccess(node.type, node);
    // MOD-03: a by-value variable requires a complete aggregate type. Pointers
    // and slices are fine (they only need a declaration), so peel typedefs and
    // array layers but stop at the first pointer/slice.
    if (node.type) {
        Type* valueType = node.type;
        while (valueType && valueType->kind == TypeKind::Typedef)
            valueType = static_cast<TypedefType*>(valueType)->aliasedType;
        while (valueType && valueType->kind == TypeKind::Array)
            valueType = static_cast<ArrayType*>(valueType)->elementType;
        while (valueType && valueType->kind == TypeKind::Typedef)
            valueType = static_cast<TypedefType*>(valueType)->aliasedType;
        if (valueType && node.typeIncompleteAtParse) {
            emitError("variable '" + node.name + "' has incomplete type '" +
                          typeToString(valueType) +
                          "'; define the type before using it by value",
                      node);
        }
    }
    // P1-04 / CT-03 / 评审 I3: 毒化类型（含 typedef/指针/数组包装层）拒绝。
    checkCtDeadBranchUse(node.type, node);
    std::string sourceVarName = node.name;
    node.name = scopedName(node.name);
    // PAR-08: remember global variables so assignment of an array/slice
    // literal to one (or to a subobject of one) can be rejected as dangling.
    if (atGlobalLevel()) {
        m_globalVarNames.insert(node.name);
        m_globalVarNames.insert(sourceVarName);
    }
    // C static local: initialization must be a compile-time constant; the
    // storage itself is emitted at module scope by codegen.
    if (node.isStaticLocal && node.initExpr) {
        auto folded = evaluateConstexpr(node.initExpr.get());
        if (!folded) {
            emitError("static local variable '" + node.name +
                          "' must be initialized with a constant expression",
                      node);
        } else {
            FoldedValue fv;
            fv.type = static_cast<FoldedValue::Type>(folded->type);
            switch (folded->type) {
                case ConstValue::INT: fv.intVal = folded->intVal; break;
                case ConstValue::DOUBLE: fv.doubleVal = folded->doubleVal; break;
                case ConstValue::CHAR: fv.charVal = folded->charVal; break;
            }
            node.foldedValue = fv;
        }
    }

    if (node.isConstexpr) {
        if (!node.initExpr) {
            emitError("constexpr variable '" + node.name + "' must have initializer", node);
        } else {
            auto folded = evaluateConstexpr(node.initExpr.get());
            if (!folded) {
                emitError("constexpr variable '" + node.name + "' must be initialized with a constant expression", node);
            } else {
                constexprValues[node.name] = *folded;
                FoldedValue fv;
                fv.type = static_cast<FoldedValue::Type>(folded->type);
                switch (folded->type) {
                    case ConstValue::INT: fv.intVal = folded->intVal; break;
                    case ConstValue::DOUBLE: fv.doubleVal = folded->doubleVal; break;
                    case ConstValue::CHAR: fv.charVal = folded->charVal; break;
                }
                node.foldedValue = fv;
            }
        }
    }

    if (node.initExpr) {
        if (auto* initList = dynamic_cast<InitializerListExprAST*>(node.initExpr.get())) {
            initList->type = node.type;
            initList->isLValue = false;
            for (auto& e : initList->initializers) getExprType(*e);
            // PAR-08 (Task 5): a slice initialized from a brace literal is
            // backed by a stack anonymous array — track it for the dangling
            // return check (design §2.5).
            {
                Type* vt = node.type;
                while (vt && vt->kind == TypeKind::Typedef)
                    vt = static_cast<TypedefType*>(vt)->aliasedType;
                if (vt && vt->kind == TypeKind::Slice) {
                    m_sliceLiteralVars.insert(
                        sliceLiteralKey(currentFunction, node.name));
                }
            }
            // PAR-13: 设计符解析（struct/class/union/数组/嵌套；Optional/Result
            // 仅拒绝伪字段设计符）。结构变量在 codegen 前由 emitAggregateInitializer
            // 消费 resolvedTargets。
            resolveDesignators(*initList, node.type, node.name);
            // P1-02 (TYP-13/14): aggregate shape check for Optional/Result —
            // arity and per-field types (DS4 layout: flag, value[, error]).
            Type* vt = node.type;
            while (vt && vt->kind == TypeKind::Typedef)
                vt = static_cast<TypedefType*>(vt)->aliasedType;
            if (vt && (vt->kind == TypeKind::Optional || vt->kind == TypeKind::Result)) {
                size_t expected = vt->kind == TypeKind::Optional ? 2 : 3;
                std::vector<Type*> fieldTypes;
                if (vt->kind == TypeKind::Optional) {
                    auto* ot = static_cast<OptionalType*>(vt);
                    fieldTypes = {TypeContext::instance().getBool(), ot->elementType};
                } else {
                    auto* rt = static_cast<ResultType*>(vt);
                    fieldTypes = {TypeContext::instance().getBool(), rt->successType,
                                  rt->errorType};
                }
                if (initList->initializers.size() != expected) {
                    emitError("initializer list for '" + node.name + "' has " +
                                  std::to_string(initList->initializers.size()) +
                                  " elements, expected " + std::to_string(expected),
                              node);
                } else {
                    for (size_t i = 0; i < fieldTypes.size(); ++i) {
                        // Nested brace lists recurse through codegen's
                        // emitAggregateInitializer (struct precedent: no sema
                        // element-level check); only scalar slots are typed.
                        if (dynamic_cast<InitializerListExprAST*>(
                                initList->initializers[i].get())) {
                            continue;
                        }
                        // getExprType (not ->type): element types are computed
                        // lazily and the node field is not yet populated here.
                        Type* it = getExprType(*initList->initializers[i]);
                        if (!it) continue;
                        // Flag fields (valid/ok) follow the language's normal
                        // bool-assignment channel (`true` literals are int32
                        // in this language, C-style). Value/error fields are
                        // positional literal slots: exact typesEqual — else
                        // `{5, true}` would silently accept bool as the value.
                        bool okField;
                        if (i == 0) {
                            // Flag fields (valid/ok) follow the language's normal
                            // bool-assignment channel (`true` literals are int32
                            // in this language, C-style).
                            okField = typesCompatible(fieldTypes[i], it);
                        } else {
                            // Value/error slots are positional literal slots:
                            // exact match after stripping typedefs on BOTH
                            // sides (评审 I2: typesEqual does not strip —
                            // `Optional<MyInt> o = {true, 5}` was mis-rejected).
                            Type* ft = fieldTypes[i];
                            while (ft && ft->kind == TypeKind::Typedef)
                                ft = static_cast<TypedefType*>(ft)->aliasedType;
                            Type* its = it;
                            while (its && its->kind == TypeKind::Typedef)
                                its = static_cast<TypedefType*>(its)->aliasedType;
                            okField = typesEqual(ft, its);
                        }
                        if (!okField) {
                            emitError("type mismatch in initializer " +
                                          std::to_string(i) + " of '" + node.name +
                                          "': expected '" + typeToString(fieldTypes[i]) +
                                          "', got '" + typeToString(it) + "'",
                                      node);
                        }
                    }
                }
            }
        } else {
            Type* initType = getExprType(*node.initExpr);
            // P1-06 (DEC-20): a clearer message before the generic mismatch —
            // str/char* values never become `string` implicitly.
            {
                Type* initStripped = initType;
                while (initStripped && initStripped->kind == TypeKind::Typedef)
                    initStripped = static_cast<TypedefType*>(initStripped)->aliasedType;
                Type* varStripped = node.type;
                while (varStripped && varStripped->kind == TypeKind::Typedef)
                    varStripped = static_cast<TypedefType*>(varStripped)->aliasedType;
                if (varStripped && varStripped->kind == TypeKind::String && initStripped &&
                    (initStripped->kind == TypeKind::Str ||
                     (initStripped->kind == TypeKind::Pointer && initStripped->base &&
                      initStripped->base->kind == TypeKind::Char))) {
                    emitError("cannot initialize 'string' from '" + typeToString(initType) +
                                  "'; construct explicitly with string.new(...) (DEC-20)",
                              node);
                    return;
                }
            }
            if (initType && !typesCompatible(node.type, initType)) {
                emitError("type mismatch in initialization of '" + node.name + "': expected '" 
                    + typeToString(node.type) + "', got '" + typeToString(initType) + "'", node);
            }
        }
    }
    if (!declare(node.name, node.type)) {
        emitError(DiagnosticCode::SemRedefinition, "redeclaration of variable '" + node.name + "' in the same scope", node);
    }
    // SEM-11: remember local variables for the unused-variable check (W3002).
    if (currentFunction) {
        if (Symbol* sym = currentScope->lookup(node.name)) {
            sym->checkUnused = true;
            sym->declFile = node.sourceFile;
            sym->declLine = node.sourceLine;
            sym->declColumn = node.sourceColumn;
        }
    }
}

void SemanticAnalyzer::visit(ArrayDeclAST& node) {
    // P1-05 / ANN-06: 数组变量注解验证。
    validateAnnotations(node.annotations, node, "variable");
    // AGG-11 评审 I2: array declarations are var declarations (DS5) — a
    // private nested type must not slip through as the element type.
    node.elementType = resolveTypeInstance(node.elementType, node);
    checkNestedTypeAccess(node.elementType, node);
    // P1-04 评审 I3: 元素类型不得来自 compile_time.if 死分支。
    checkCtDeadBranchUse(node.elementType, node);
    std::string sourceArrayName = node.name;
    node.name = scopedName(node.name);
    // PAR-08: record global array variables for dangling-escape checks.
    if (atGlobalLevel()) {
        m_globalVarNames.insert(node.name);
        m_globalVarNames.insert(sourceArrayName);
    }
    if (auto* initList = dynamic_cast<InitializerListExprAST*>(node.initExpr.get())) {
        if (node.size == 0) {
            // PAR-13: 含设计符时按 C99 最大下标+1 推断（含设计符后位置式续位）；
            // 无设计符维持既有"列表元素数"。
            bool hasDs = false;
            for (auto& d : initList->designators) {
                if (!d.empty()) { hasDs = true; break; }
            }
            node.size = hasDs ? inferArrayInitSize(*initList)
                              : static_cast<int>(initList->initializers.size());
        }
    }
    Type* arrayType = new ArrayType(node.elementType, node.size);
    if (node.initExpr) {
        if (auto* initList = dynamic_cast<InitializerListExprAST*>(node.initExpr.get())) {
            initList->type = arrayType;
            initList->isLValue = false;
            for (auto& e : initList->initializers) getExprType(*e);
            resolveDesignators(*initList, arrayType, node.name);
        } else {
            // A non-list initializer cannot populate an array: at global scope
            // it was silently zero-filled, locally it stored a scalar into an
            // array alloca. Require a brace list ( elementType checks happen
            // per element during aggregate initialization).
            emitError("array initializer must be a brace-enclosed list", node);
        }
    }
    if (!declare(node.name, arrayType)) {
        emitError("redeclaration of array '" + node.name + "' in the same scope", node);
    }
    // SEM-11: remember local arrays for the unused-variable check (W3002).
    if (currentFunction) {
        if (Symbol* sym = currentScope->lookup(node.name)) {
            sym->checkUnused = true;
            sym->declFile = node.sourceFile;
            sym->declLine = node.sourceLine;
            sym->declColumn = node.sourceColumn;
        }
    }
}

void SemanticAnalyzer::visitNestedDecl(DeclAST& node) {
    if (auto* sd = dynamic_cast<StructDeclAST*>(&node)) {
        visit(*sd);
    } else if (auto* ed = dynamic_cast<EnumDeclAST*>(&node)) {
        visit(*ed);
    } else if (auto* ud = dynamic_cast<UnionDeclAST*>(&node)) {
        visit(*ud);
    } else if (auto* u = dynamic_cast<UsingDeclAST*>(&node)) {
        visit(*u);
    }
}

// AGG-11/DS4: nested types are analyzed before static members and method
// bodies (same ordering rule as codegen). The caller has already pushed the
// owning class's bare-name path onto classPathPrefix, so desugared
// static-member symbols inside nested classes match what the qualified
// access spelling (`ns::Outer::Inner::v`) flattens to.
void SemanticAnalyzer::visitNestedTypeDecls(
    std::vector<std::unique_ptr<DeclAST>>& nestedTypes, ClassType* owner) {
    for (auto& nested : nestedTypes) {
        if (!nested) continue;
        std::string flatName;
        std::string bareNested;
        if (auto* sd = dynamic_cast<StructDeclAST*>(nested.get())) {
            flatName = sd->name;
            bareNested = sd->bareName;
        } else if (auto* ed = dynamic_cast<EnumDeclAST*>(nested.get())) {
            flatName = ed->name;
            bareNested = ed->bareName;
        } else if (auto* ud = dynamic_cast<UnionDeclAST*>(nested.get())) {
            flatName = ud->name;
            bareNested = ud->bareName;
        } else if (auto* u = dynamic_cast<UsingDeclAST*>(nested.get())) {
            flatName = u->name;
            bareNested = u->bareName;
        }
        if (bareNested.empty()) bareNested = flatName;
        // DS5: record the access level so E2009 checks can find it. Struct
        // and union nested types are always public (owner == null).
        if (owner) {
            nestedTypeAccess[flatName] =
                {owner, owner->memberAccessLevel("type:" + bareNested)};
        }
        visitNestedDecl(*nested);
    }
}

// AGG-11/DS5: a private nested type may only be named from inside its
// defining class — checked at var declarations and cast targets.
void SemanticAnalyzer::checkNestedTypeAccess(Type* type, const ASTNode& site) {
    if (!type) return;
    // PAR-24: a class-scoped alias is itself an access-controlled name; walk
    // each typedef chain before stripping it away.
    auto aliasDenied = [&](Type* raw) -> bool {
        for (Type* alias = raw; alias && alias->kind == TypeKind::Typedef;
             alias = static_cast<TypedefType*>(alias)->aliasedType) {
            auto* td = static_cast<TypedefType*>(alias);
            auto it = nestedTypeAccess.find(td->name);
            if (it == nestedTypeAccess.end()) continue;
            if (it->second.second == AccessLevel::Public) continue;
            if (currentClass == it->second.first) continue;
            emitError(DiagnosticCode::SemPrivateMemberAccess,
                      "cannot access private type '" + td->name + "' of class '" +
                          it->second.first->name +
                          "' outside the class; make it public or add an accessor",
                      site);
            return true;
        }
        return false;
    };
    if (aliasDenied(type)) return;
    Type* t = stripTypedef(type);
    // AGG-11 评审 I2: arrays are named-type carriers too (`NTP::Secret a[3]`).
    // Check the inner spelling before stripping its aliases so `C::T*` is
    // rejected just like the by-value form.
    while (t && (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array)) {
        // Array types are ArrayType objects (codebase convention, cf.
        // typeToString); pointer element is `base`.
        Type* inner = t->kind == TypeKind::Pointer
                          ? t->base
                          : static_cast<ArrayType*>(t)->elementType;
        if (aliasDenied(inner)) return;
        t = stripTypedef(inner);
    }
    if (!t) return;
    std::string name;
    switch (t->kind) {
        case TypeKind::Class: name = static_cast<ClassType*>(t)->name; break;
        case TypeKind::Struct: name = static_cast<StructType*>(t)->name; break;
        case TypeKind::Union: name = static_cast<UnionType*>(t)->name; break;
        case TypeKind::Enum: name = static_cast<EnumType*>(t)->name; break;
        default: return;
    }
    auto it = nestedTypeAccess.find(name);
    if (it == nestedTypeAccess.end()) return;
    if (it->second.second == AccessLevel::Public) return;
    if (currentClass == it->second.first) return;
    emitError(DiagnosticCode::SemPrivateMemberAccess,
              "cannot access private type '" + name + "' of class '" +
                  it->second.first->name +
                  "' outside the class; make it public or add an accessor",
              site);
}

void SemanticAnalyzer::visit(StructDeclAST& node) {
    m_definingStack.push_back(node.name);
    visitStructDeclImpl(node);
    m_definingStack.pop_back();
}

// P1-07 / SEM-12: validate a bit-field field. A field is a bit-field when
// `bitWidth != 0` (any sign) or it is unnamed (`name == ""`, incl. the
// zero-width align marker). Named zero-width is rejected at parse time (the
// FieldInfo encoding cannot represent it), so a named field reaching here
// always has bitWidth != 0.
void SemanticAnalyzer::checkBitFieldField(FieldInfo& f, ASTNode& node) {
    if (f.bitWidth == 0 && !f.name.empty()) return;  // 普通字段
    if (!isBitFieldBaseType(f.type)) {
        emitError(DiagnosticCode::SemBitFieldInvalidType,
                  "invalid bit-field base type (expected bool or a fixed-width integer type)",
                  node);
        return;
    }
    int bits = bitFieldTypeBits(f.type);
    // 无名位域允许 0..bits（0 = 零宽对齐）；命名位域要求 1..bits。
    int min = f.name.empty() ? 0 : 1;
    if (f.bitWidth < min || f.bitWidth > bits) {
        emitError(DiagnosticCode::SemBitFieldWidthOutOfRange,
                  "bit-field width out of range", node);
    }
}

void SemanticAnalyzer::visitStructDeclImpl(StructDeclAST& node) {
    // P1-03 / GEN-03: 实例字段/方法签名中的实例占位先解析。
    for (auto& f : node.fields) {
        validateAnnotations(f.annotations, node, "field");
        f.type = resolveTypeInstance(f.type, node);
        // SG-01: a nested instantiation produced during template rewriting
        // (e.g. `A<N>` inside `B`) is a placeholder whose declaration still
        // needs its layout/fields materialized.
        ensureInstanceVisited(f.type, node);
        // PAR-24: a private class-scoped alias may not name a field here.
        checkNestedTypeAccess(f.type, node);
        // P1-05 评审 I4: 字段类型弃用检查。
        checkDeprecatedTypeUse(f.type, node);
        // P1-04 评审 I3: 字段类型不得来自 compile_time.if 死分支。
        checkCtDeadBranchUse(f.type, node);
        // P1-07 / SEM-12: 位域基础类型与宽度范围校验。
        checkBitFieldField(f, node);
    }
    // P1-07 / AGG-04: 柔性数组成员约束（struct 尾成员/具名/非位域/≥1 前导；class 拒绝）。
    checkFlexibleArrayConstraint(node);
    for (auto& m : node.methods) {
        m->returnType = resolveTypeInstance(m->returnType, node);
        ensureInstanceVisited(m->returnType, node);
        for (auto& p : m->params) {
            p->type = resolveTypeInstance(p->type, node);
            ensureInstanceVisited(p->type, node);
        }
        // P1-05 评审 I3: 方法弃用记录（方法不经 visit(FunctionDeclAST)）。
        for (const auto& ann : m->annotations) {
            if (ann.name == "deprecated") {
                std::string msg;
                if (ann.args.size() == 1
                    && ann.args[0].kind == AnnotationArg::Kind::String) {
                    msg = ann.args[0].text;
                }
                m_deprecatedFuncs.insert(m->name);
                m_deprecatedMsgs[m->name] = msg;
            }
        }
    }
    // P1-03 / INH-05 / GEN-09: 实例字段值语义自嵌套（CRTP 的 `D next`）拒绝。
    checkInstanceFieldComplete(node);
    // PAR-04/DEC-01: `class` declarations are classes even without methods —
    // their members default to private and carry access levels.
    // INH-01: struct declarations with a base stay structs — only classes
    // (isClassDecl) or method-bearing declarations route to the class branch.
    bool isClass = node.isClassDecl || !node.methods.empty();

    // P1-05 / ANN-06: struct/class 与字段注解验证——class 的 repr(C) 目标
    // 不符（spec：仅 struct/union）。
    validateAnnotations(node.annotations, node, node.isClassDecl ? "class" : "struct");

    // P1-05 / ANN-02/03: 布局注解折叠进 Type（LayoutBuilder 消费）。
    {
        Type* agg = isClass ? static_cast<Type*>(typeCtx->getOrCreateClass(node.name))
                            : static_cast<Type*>(typeCtx->getStruct(node.name));
        if (agg) {
            // StructType and ClassType carry identical annotation/layout
            // members but are unrelated subclasses; dispatch to the actual
            // kind instead of casting a ClassType to StructType (UB).
            auto applyAnnotations = [&](auto* st) {
                st->annotations = node.annotations;
                for (const auto& ann : node.annotations) {
                    if (ann.name == "packed") st->isPacked = true;
                    else if (ann.name == "repr") st->reprC = true;
                    else if (ann.name == "align") st->forcedAlign = ctAlignArg(ann, *this);
                    else if (ann.name == "deprecated") {
                        st->deprecated = true;
                        if (ann.args.size() == 1
                            && ann.args[0].kind == AnnotationArg::Kind::String) {
                            st->deprecatedMsg = ann.args[0].text;
                        }
                    }
                }
                for (const auto& f : node.fields) {
                    for (const auto& ann : f.annotations) {
                        if (ann.name == "align") {
                            st->fieldAligns[f.name] = ctAlignArg(ann, *this);
                        } else if (ann.name == "deprecated") {
                            st->fieldDeprecated[f.name] = true;
                            if (ann.args.size() == 1
                                && ann.args[0].kind == AnnotationArg::Kind::String) {
                                st->fieldDeprecatedMsg[f.name] = ann.args[0].text;
                            }
                        }
                    }
                }
            };
            if (isClass) applyAnnotations(static_cast<ClassType*>(agg));
            else applyAnnotations(static_cast<StructType*>(agg));
        }
    }

    if (isClass) {
        auto* classType = typeCtx->getOrCreateClass(node.name);
        // Redef 轮: a definition completes the class; forward declarations
        // leave the placeholder incomplete. A second definition (or a
        // definition colliding with any other completed kind) is E2004.
        if (!node.isForwardDecl) {
            if (isTypeRedefined(node.name)) {
                emitError(DiagnosticCode::SemRedefinition, "redefinition of type '" + node.name + "'", node);
            }
            classType->isComplete = true;
        }

        // Add fields if not already added
        for (auto& field : node.fields) {
            if (!classType->getFieldType(field.name)) {
                classType->addField(field.name, field.type, field.bitWidth, field.annotations);
                if (field.flexibleArray && !classType->fields.empty())
                    classType->fields.back().flexibleArray = true;
            }
        }

        // P1-03 / INH-05 / GEN-09: 模板实例基类（CRTP）——baseClass 拼写为
        // `Shape<Circle>`。实参在此刻解析（派生类自身的前向占位 ClassType
        // 已由 getOrCreateClass 创建，可作实参）；实例方法体惰性——派生类
        // 方法表未就绪，延迟到首次调用（见 visit(MethodCallExprAST)）。
        if (node.baseClass.find('<') != std::string::npos) {
            size_t lt = node.baseClass.find('<');
            std::string tplName = node.baseClass.substr(0, lt);
            std::string argsSpelling = node.baseClass.substr(lt + 1, node.baseClass.size() - lt - 2);
            std::vector<Type*> baseArgs;
            std::string cur;
            auto flushArg = [&]() {
                if (cur.empty()) return;
                Type* argType = resolveTypeByName(cur);
                if (!argType) {
                    emitError("unknown template base class argument '" + cur + "' of '" +
                                  node.baseClass + "'",
                              node);
                }
                baseArgs.push_back(argType);
                cur.clear();
            };
            for (size_t i = 0; i <= argsSpelling.size(); ++i) {
                if (i == argsSpelling.size() || argsSpelling[i] == ',') flushArg();
                else if (argsSpelling[i] != ' ') cur += argsSpelling[i];
            }
            if (!baseArgs.empty() && std::all_of(baseArgs.begin(), baseArgs.end(), [](Type* t) { return t; })) {
                std::string instName = TemplateRegistry::instanceName(tplName, baseArgs, {});
                auto* inst = TemplateRegistry::instance().instantiateClass(tplName, baseArgs, {});
                if (!inst) {
                    emitError("cannot instantiate template base class '" + node.baseClass + "'", node);
                    return;
                }
                if (!m_visitedInstances.count(instName)) {
                    m_visitedInstances.insert(instName);
                    m_lazyInstanceMethods.insert(instName);
                    m_instStack.push_back(tplName + "<...>");
                    visit(*inst);
                    m_instStack.pop_back();
                }
                node.baseClass = instName;
            }
        }

        if (!node.baseClass.empty()) {
            auto* baseType = typeCtx->getClass(node.baseClass);
            if (!baseType) {
                emitError("base class '" + node.baseClass + "' of class '" + node.name + "' not found", node);
            } else if (hasCircularInheritance(node.name, node.baseClass)) {
                emitError("circular inheritance detected involving class '" + node.name + "'", node);
            } else if (!baseType->isComplete) {
                // INH-01: a forward-declared (placeholder) base has no
                // definition yet — deriving from it is rejected, mirroring
                // C++'s incomplete-type rule.
                emitError("base class '" + node.baseClass + "' of class '" + node.name +
                              "' is incomplete; define it before deriving from it",
                          node);
            } else {
                // INH-06（方案乙）: no method-table copy — resolveMethod walks
                // the base chain, so inherited methods keep their defining
                // class, access level and definition-site signature.
                classType->baseClass = node.baseClass;
                classType->base = baseType;
            }
        }

        // P1-03 / INH-05 / GEN-09: 基类实例的方法体惰性——派生类方法表未
        // 就绪，只登记签名，body 延迟到首次调用（visit(MethodCallExprAST)
        // 触发）。非实例路径不变。
        const bool lazyMethods = m_lazyInstanceMethods.count(node.name) > 0;

        for (auto& method : node.methods) {
            // AGG-10/DS1: static methods are not instance members — they
            // never enter the class method table, so instance calls reject.
            if (method->isStatic) continue;
            // Only add if not already present
            if (!classType->getMethod(method->name)) {
                std::vector<Type*> paramTypes;
                paramTypes.push_back(new Type(TypeKind::Pointer, classType));
                for (auto& param : method->params) {
                    paramTypes.push_back(param->type);
                }
                auto* methodType = new FunctionType(method->returnType, std::move(paramTypes));
                classType->addMethod(method->name, methodType);
            }
            if (lazyMethods) {
                m_pendingLazyMethods[node.name].push_back(method.get());
            }
        }

        typeCtx->addClass(node.name, classType);

        // PAR-04/DEC-01: carry the parser-recorded access levels into the type.
        for (const auto& kv : node.memberAccess) {
            classType->setMemberAccess(kv.first, kv.second);
        }

        // AGG-11/DS4: nested types first (before static members and methods).
        // classPathPrefix is extended with THIS class's bare name while its
        // nested types are visited (their members resolve against it).
        std::string savedPath = classPathPrefix;
        classPathPrefix += mangleNamespaceName(
            node.bareName.empty() ? node.name : node.bareName) + "_";
        visitNestedTypeDecls(node.nestedTypes, classType);
        classPathPrefix = savedPath;

        // AGG-10/DS2+DS3: static data members become globals under the
        // desugared key; the in-class initializer is their definition.
        // Declared BEFORE method bodies are analyzed so method bodies can
        // reference them (declaration order matters in this pipeline).
        for (auto& vd : node.staticMembers) {
            if (!vd) continue;
            std::string origName = vd->name;
            std::string clsPath = classPathPrefix +
                (node.bareName.empty() ? node.name : node.bareName);
            vd->name = mangleNamespaceName(clsPath + "::" + origName);
            staticMemberIndex[scopedName(vd->name)] = {classType, origName};
            staticDataDecls[scopedName(vd->name)] = vd.get();
            ClassType* savedClass = currentClass;
            currentClass = classType;
            visit(*vd);
            currentClass = savedClass;
        }

        // AGG-10/I2: register all static methods BEFORE analyzing any method
        // body, so a static method may call a sibling declared later in the
        // class body (same declaration-order freedom as instance methods,
        // whose table is filled below). declare() tolerates the later
        // re-declaration inside visit() (prototype pattern); only two
        // definitions of one signature error, via definedFunctions.
        for (auto& method : node.methods) {
            if (!method || !method->isStatic) continue;
            std::string origName = method->name;
            std::string clsPath = classPathPrefix +
                (node.bareName.empty() ? node.name : node.bareName);
            method->name = mangleNamespaceName(clsPath + "::" + origName);
            staticMemberIndex[scopedName(method->name)] = {classType, origName};
            std::vector<Type*> pts;
            for (auto& param : method->params) pts.push_back(param->type);
            declare(scopedName(method->name),
                    new FunctionType(method->returnType, std::move(pts), method->isVarArg));
        }

        for (auto& method : node.methods) {
            // P1-03 / INH-05: 基类实例方法体惰性——首次调用时 visit。
            if (lazyMethods) continue;
            ClassType* savedClass = currentClass;
            currentClass = classType;
            if (method->isStatic) {
                // AGG-10/DS3+DS4: desugar to a global function symbol
                // `Class_method`; no `this` parameter is inserted. Qualified
                // call sites resolve via the existing namespace machinery.
                // The desugared name uses the BARE class name — scopedName
                // (applied inside visit) adds the namespace prefix, matching
                // what the fully qualified access spelling flattens to.
                m_inStaticMethod = true;
                visit(*method);
                m_inStaticMethod = false;
            } else {
                auto* thisType = new Type(TypeKind::Pointer, classType);
                auto thisParam = std::make_unique<ParamDeclAST>("this", thisType);
                // FUN-09: 登记方法默认参数计数（定义类沿继承链 = classType）。
                {
                    int dc = 0;
                    for (auto& p : method->params) if (p->hasDefault) ++dc;
                    m_methodDefaultCounts[classType->name + "::" + method->name] = dc;
                    m_methodDecls[classType->name + "::" + method->name] = method.get();
                }
                method->params.insert(method->params.begin(), std::move(thisParam));
                visit(*method);
            }
            currentClass = savedClass;
        }
    } else {
        // INH-01: reuse the parse-time registration when present — variable
        // declarations already point at that StructType object, so the base
        // wiring must land on it (mirrors getOrCreateClass on the class path).
        auto* structType = typeCtx->getStruct(node.name);
        if (!structType) {
            structType = new StructType(node.name);
        }
        for (auto& field : node.fields) {
            if (!structType->getFieldType(field.name)) {
                structType->addField(field.name, field.type, field.bitWidth, field.annotations);
                if (field.flexibleArray && !structType->fields.empty())
                    structType->fields.back().flexibleArray = true;
            }
        }
        // INH-01: single public inheritance — wire the base before layout
        // (codegen puts the base sub-object in field slot 0) and validate it
        // like the class branch does.
        // Redef 轮: mirror of the class branch — duplicate definition of the
        // same name (any kind) is E2004.
        if (!node.isForwardDecl) {
            if (isTypeRedefined(node.name)) {
                emitError(DiagnosticCode::SemRedefinition, "redefinition of type '" + node.name + "'", node);
            }
            structType->isComplete = true;
        }
        if (!node.baseClass.empty()) {
            auto* baseType = typeCtx->getStruct(node.baseClass);
            if (!baseType) {
                emitError("base struct '" + node.baseClass + "' of struct '" + node.name + "' not found", node);
            } else if (hasCircularInheritance(node.name, node.baseClass)) {
                emitError("circular inheritance detected involving struct '" + node.name + "'", node);
            } else if (!baseType->isComplete) {
                emitError("base struct '" + node.baseClass + "' of struct '" + node.name +
                              "' is incomplete; define it before deriving from it",
                          node);
            } else {
                structType->baseClass = node.baseClass;
                structType->base = baseType;
            }
        }
        typeCtx->addStruct(node.name, structType);
        // AGG-11/DS4: nested types first (structs' nested types are public).
        std::string savedPath = classPathPrefix;
        classPathPrefix += mangleNamespaceName(
            node.bareName.empty() ? node.name : node.bareName) + "_";
        visitNestedTypeDecls(node.nestedTypes, nullptr);
        classPathPrefix = savedPath;
        // AGG-10: struct static data members — same desugar; no access-level
        // index (struct members are always public).
        for (auto& vd : node.staticMembers) {
            if (!vd) continue;
            std::string origName = vd->name;
            std::string clsPath = classPathPrefix +
                (node.bareName.empty() ? node.name : node.bareName);
            vd->name = mangleNamespaceName(clsPath + "::" + origName);
            staticDataDecls[scopedName(vd->name)] = vd.get();
            visit(*vd);
        }
    }

    // PAR-12: 内联变量 `class { ... } c;` / `struct S { ... } s;` ——类型已
    // 注册完毕，现在处理变量（进入普通声明分派，含 MultiVarDeclAST 展开）。
    if (node.attachedVars) {
        analyzeTopLevelDecl(*node.attachedVars);
    }
}

void SemanticAnalyzer::visit(UnionDeclAST& node) {
    // P1-05 / ANN-06: union 与成员注解验证。
    validateAnnotations(node.annotations, node, "union");
    for (auto& member : node.members) {
        validateAnnotations(member.annotations, node, "field");
        // PAR-24: a private class-scoped alias may not name a union member.
        checkNestedTypeAccess(member.type, node);
        // P1-07 / SEM-12: 位域基础类型与宽度范围校验。
        checkBitFieldField(member, node);
    }
    // P1-05 / ANN-02/03: union 仅 align 有效（packed 无填充语义，spec §4）。
    {
        auto* unionType = node.name.empty() ? nullptr : typeCtx->getUnion(node.name);
        if (unionType) {
            unionType->annotations = node.annotations;
            for (const auto& ann : node.annotations) {
                if (ann.name == "align") unionType->forcedAlign = ctAlignArg(ann, *this);
                else if (ann.name == "repr") unionType->reprC = true;
                else if (ann.name == "deprecated") {
                    unionType->deprecated = true;
                    if (ann.args.size() == 1
                        && ann.args[0].kind == AnnotationArg::Kind::String) {
                        unionType->deprecatedMsg = ann.args[0].text;
                    }
                }
            }
        }
    }
    // Redef 轮: reuse an existing registration for named unions (mirrors the
    // struct branch) so the completion state is observable across visits.
    auto* unionType = node.name.empty() ? nullptr : typeCtx->getUnion(node.name);
    if (!unionType) {
        unionType = new UnionType(node.name);
        typeCtx->addUnion(node.name, unionType);
    }
    for (auto& member : node.members) {
        // P1-07 / AGG-04: union 不允许柔性数组。
        if (member.flexibleArray) {
            emitError("flexible array member is not allowed in a union", node);
            member.flexibleArray = false;
        }
        // 防重复（镜像 struct 分支的 getFieldType guard）——Parser 已预注册
        // 成员（含 bitWidth），此处不得二次追加。
        bool exists = false;
        for (auto& m : unionType->members) {
            if (m.name == member.name && m.bitWidth == member.bitWidth) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            unionType->addMember(member.name, member.type, member.bitWidth, member.annotations);
            if (member.flexibleArray && !unionType->members.empty())
                unionType->members.back().flexibleArray = true;
        }
    }
    // Redef 轮: duplicate definition of the same name (any kind) is E2004.
    if (!node.isForwardDecl && !node.name.empty()) {
        if (isTypeRedefined(node.name)) {
            emitError(DiagnosticCode::SemRedefinition, "redefinition of type '" + node.name + "'", node);
        }
        unionType->isComplete = true;
    }
    // AGG-11/DS4: nested types first (unions' nested types are public).
    std::string savedPath = classPathPrefix;
    classPathPrefix += mangleNamespaceName(
        node.bareName.empty() ? node.name : node.bareName) + "_";
    visitNestedTypeDecls(node.nestedTypes, nullptr);
    classPathPrefix = savedPath;
}

void SemanticAnalyzer::visit(EnumDeclAST& node) {
    // P1-05 / ANN-06: enum 注解验证。
    validateAnnotations(node.annotations, node, "enum");
    // Reuse the enum type registered by the parser (which carries the explicit
    // underlying type) so variable references and this declaration agree.
    EnumType* enumType = node.name.empty() ? nullptr : typeCtx->getEnum(node.name);
    if (!enumType) {
        enumType = new EnumType(node.name);
        if (!node.name.empty()) {
            typeCtx->addEnum(node.name, enumType);
        }
    }
    enumType->annotations = node.annotations;
    // Redef 轮: duplicate definition of the same name (any kind) is E2004.
    if (!node.isForwardDecl && !node.name.empty()) {
        if (isTypeRedefined(node.name)) {
            emitError(DiagnosticCode::SemRedefinition, "redefinition of type '" + node.name + "'", node);
        }
        enumType->isComplete = true;
    }
    // TYP-09/TYP-25: an explicit underlying type must be an integer type.
    if (node.underlyingType) {
        if (!isIntegerType(node.underlyingType) ||
            stripTypedef(node.underlyingType)->kind == TypeKind::Enum) {
            emitError("enum '" + node.name + "' underlying type must be an integer type", node);
        } else {
            enumType->underlyingType = node.underlyingType;
        }
    }
    enumType->values.clear();
    for (auto& val : node.values) {
        enumType->addValue(val.first, val.second);
        // Enumerators live in the enclosing (namespace) scope, so `A::Red`
        // resolves to the same key as `Red` used inside namespace A.
        // AGG-11/DS4: a class-nested enum's constants register under the
        // flattened enclosing-class path (`Outer::Red` -> `Outer_Red`); the
        // bare key is NOT registered, so `Red` stays unusable outside.
        std::string key = classPathPrefix.empty()
            ? scopedName(val.first)
            : scopedName(mangleNamespaceName(classPathPrefix + val.first));
        enumConstants[key] = {enumType, val.second};
    }
}

void SemanticAnalyzer::visit(ForwardDeclAST& node) {}

void SemanticAnalyzer::visit(DeclStmtAST& node) {
    if (node.decl) {
        if (auto* varDecl = dynamic_cast<VarDeclAST*>(node.decl.get())) {
            visit(*varDecl);
        } else if (auto* arrDecl = dynamic_cast<ArrayDeclAST*>(node.decl.get())) {
            visit(*arrDecl);
        } else if (auto* structDecl = dynamic_cast<StructDeclAST*>(node.decl.get())) {
            visit(*structDecl);
        } else if (auto* unionDecl = dynamic_cast<UnionDeclAST*>(node.decl.get())) {
            visit(*unionDecl);
        } else if (auto* enumDecl = dynamic_cast<EnumDeclAST*>(node.decl.get())) {
            visit(*enumDecl);
        } else if (auto* fwdDecl = dynamic_cast<ForwardDeclAST*>(node.decl.get())) {
            visit(*fwdDecl);
        } else if (auto* usingDecl = dynamic_cast<UsingDeclAST*>(node.decl.get())) {
            visit(*usingDecl);
        } else if (auto* fnDecl = dynamic_cast<FunctionDeclAST*>(node.decl.get())) {
            // SG-02: block-scope `extern` function prototype.
            visit(*fnDecl);
        } else if (auto* multi = dynamic_cast<MultiVarDeclAST*>(node.decl.get())) {
            for (auto& d : multi->decls) {
                if (auto* v = dynamic_cast<VarDeclAST*>(d.get())) visit(*v);
                else if (auto* a = dynamic_cast<ArrayDeclAST*>(d.get())) visit(*a);
                else if (auto* f = dynamic_cast<FunctionDeclAST*>(d.get())) visit(*f);
            }
        }
    }
}

void SemanticAnalyzer::visit(FunctionDeclAST& node) {
    // GEN-02: a local declaration or parameter shadowing a non-type template
    // parameter is rejected — value substitution is name-only and would
    // otherwise silently capture the template argument.
    for (const auto& shadowed : node.shadowedValueParams) {
        emitError("shadowing of template value parameter '" + shadowed +
                      "' is not allowed", node);
    }
    // CON-04: `thread_local` is a variable storage class — invalid on a function.
    if (node.isThreadLocal) {
        emitError(DiagnosticCode::SemInvalidThreadLocalOnFunction,
                  "thread_local cannot be applied to a function", node);
    }
    // P1-05 / ANN-06: 函数与参数注解验证。
    validateAnnotations(node.annotations, node, "function");
    for (auto& p : node.params) {
        validateAnnotations(p->annotations, node, "parameter", p->type);
    }
    // P1-05 / ANN-05: deprecated 函数与 nonnull 形参记录（使用点查询）。
    for (const auto& ann : node.annotations) {
        if (ann.name == "deprecated") {
            std::string msg;
            if (ann.args.size() == 1 && ann.args[0].kind == AnnotationArg::Kind::String) {
                msg = ann.args[0].text;
            }
            m_deprecatedFuncs.insert(node.name);
            m_deprecatedMsgs[node.name] = msg;
        }
    }
    {
        std::vector<std::pair<size_t, std::string>> nn;
        for (size_t i = 0; i < node.params.size(); ++i) {
            for (const auto& ann : node.params[i]->annotations) {
                if (ann.name == "nonnull") nn.push_back({i, node.params[i]->name});
            }
        }
        if (!nn.empty()) m_nonnullParams[node.name] = std::move(nn);
    }
    // Keep the pre-mangling spelling for out-of-class static member lookup.
    const std::string oocOriginalName = node.name;
    // Namespace members are registered/emitted under a mangled key.
    // C-linkage extern functions keep their plain symbol name even inside a
    // namespace (`std::sqrt` links to libm's `sqrt`): skip the prefix only
    // for the extern declaration itself (F1 — a same-named non-extern user
    // function must keep its namespace mangling).
    if (!node.isExtern) {
        node.name = scopedName(node.name);
    }
    // P1-03 / GEN-03: 返回类型与参数类型中的实例占位先解析。
    node.returnType = resolveTypeInstance(node.returnType, node);
    for (auto& p : node.params) p->type = resolveTypeInstance(p->type, node);

    // PAR-03: out-of-class method definition `RetType Class::method(...) {...}`.
    // Bind it to the method declared in the class body: the class must exist,
    // the method must be declared, and the signature must match. The implicit
    // `this` is inserted exactly like an in-class definition, so both the
    // definition and the call site mangle against {Class*, params...}.
    ClassType* oocClass = nullptr;
    ClassType* oocSavedClass = currentClass;
    if (!node.className.empty()) {
        oocClass = typeCtx->getClass(node.className);
        if (!oocClass) {
            emitError(DiagnosticCode::SemUnknownType,
                      "unknown class '" + node.className + "'", node);
            return;
        }

        // AGG-10: static methods have no instance entry in ClassType::methods;
        // they are desugared into staticMemberIndex as globals. Bind an
        // out-of-class definition to that existing symbol instead of inserting
        // an implicit `this` parameter.
        bool isStaticOoc = false;
        std::string staticKey;
        for (const auto& entry : staticMemberIndex) {
            if (entry.second.first == oocClass &&
                entry.second.second == oocOriginalName) {
                isStaticOoc = true;
                staticKey = entry.first;
                break;
            }
        }
        if (isStaticOoc) {
            node.isStatic = true;
            node.name = staticKey;
            node.className.clear();
            oocClass = nullptr;
        } else {
            FunctionType* declared = oocClass->getMethod(node.name);
            if (!declared) {
                emitError("method '" + node.name + "' is not declared in class '" +
                              node.className + "'", node);
                return;
            }
            bool sigOk = declared->returnType && node.returnType &&
                         typesEqual(declared->returnType, node.returnType) &&
                         declared->paramTypes.size() == node.params.size() + 1;
            if (sigOk) {
                for (size_t i = 0; i < node.params.size(); ++i) {
                    if (!typesEqual(declared->paramTypes[i + 1], node.params[i]->type)) {
                        sigOk = false;
                        break;
                    }
                }
            }
            if (!sigOk) {
                emitError("out-of-class definition of '" + node.className + "::" +
                              node.name + "' does not match the declaration in the class", node);
                return;
            }
            node.params.insert(node.params.begin(),
                std::make_unique<ParamDeclAST>("this",
                    new Type(TypeKind::Pointer, oocClass)));
            currentClass = oocClass;
        }
    }

    // P1-04 评审 I3: 返回/参数类型不得来自 compile_time.if 死分支。
    checkCtDeadBranchUse(node.returnType, node);
    for (auto& p : node.params) checkCtDeadBranchUse(p->type, node);
    // P1-05 评审 I4: 返回/参数类型弃用检查。
    checkDeprecatedTypeUse(node.returnType, node);
    for (auto& p : node.params) checkDeprecatedTypeUse(p->type, node);

    // Validate constexpr function constraints
    if (node.isConstexpr) {
        // Return type must be arithmetic (literal type)
        if (!isIntegerType(node.returnType) && !isFloatType(node.returnType)) {
            emitError("constexpr function '" + node.name + "' must have literal return type", node);
        }

        // All parameters must be arithmetic types
        for (auto& param : node.params) {
            if (!isIntegerType(param->type) && !isFloatType(param->type)) {
                emitError("constexpr function '" + node.name + "' parameter '" + param->name + "' must have literal type", node);
            }
        }
    }

    if (node.isConstexpr) {
        // Available to the compile-time evaluator (evaluateConstexpr).
        constexprFunctions[node.name] = &node;
    }

    // FUN-09: validate default parameters — only trailing params may carry a
    // default, and each default must be a compile-time constant. Count them for
    // overload resolution.
    {
        bool seenDefault = false;
        for (auto& param : node.params) {
            if (param->hasDefault) {
                seenDefault = true;
                auto v = evaluateConstexpr(param->defaultValue.get());
                if (!v) {
                    emitError("default argument must be a compile-time constant", *param);
                }
            } else if (seenDefault) {
                emitError("default arguments must be trailing (a parameter without a default cannot follow one with a default)", *param);
            }
        }
        node.defaultArgCount = 0;
        for (auto& param : node.params) {
            if (param->hasDefault) ++node.defaultArgCount;
        }
    }

    std::vector<Type*> paramTypes;
    for (auto& param : node.params) {
        paramTypes.push_back(param->type);
    }
    auto* funcType = new FunctionType(node.returnType, std::move(paramTypes), node.isVarArg);
    std::string signature = mangleFunction(node.name, funcType->paramTypes);
    Symbol* declaredSym = declareSymbol(node.name, funcType);
    if (!declaredSym) {
        // A matching declaration already exists. That is legal for repeated
        // prototypes or a prototype followed by its definition; only two
        // definitions of the same signature are an error.
        if (node.body && definedFunctions.count(signature)) {
            emitError(DiagnosticCode::SemRedefinition, "redefinition of function '" + node.name + "'", node);
        } else if (node.body) {
            definedFunctions.insert(signature);
        }
        // I4 (review): a redeclaration may carry default arguments the earlier
        // prototype lacked (prototype then definition-with-defaults). Refresh
        // the existing Symbol's decl back-pointer so OverloadSet reads the
        // newer defaultArgCount instead of the default-less first declaration.
        if (node.defaultArgCount > 0 || node.body) {
            if (OverloadSet* os = currentScope->lookupOverload(node.name)) {
                for (Symbol* cand : os->getCandidates()) {
                    if (cand->type->kind != TypeKind::Function) continue;
                    auto* ft = static_cast<FunctionType*>(cand->type);
                    if (ft->paramTypes.size() != funcType->paramTypes.size()) continue;
                    bool same = true;
                    for (size_t i = 0; i < ft->paramTypes.size(); ++i) {
                        if (!typesEqual(ft->paramTypes[i], funcType->paramTypes[i])) {
                            same = false;
                            break;
                        }
                    }
                    if (same && cand->decl != &node) {
                        cand->decl = &node;
                        break;
                    }
                }
            }
        }
    } else {
        declaredSym->decl = &node;
        if (node.body) definedFunctions.insert(signature);
    }

    FunctionDeclAST* prevFunc = currentFunction;
    currentFunction = &node;
    enterScope();

    for (auto& param : node.params) {
        if (!declare(param->name, param->type)) {
            emitError("redeclaration of parameter '" + param->name + "' in function '" + node.name + "'", *param);
        }
    }

    if (node.body) {
        visit(*node.body);
        checkInitialization(node);
    }

    exitScope();
    currentFunction = prevFunc;
    if (oocClass) currentClass = oocSavedClass;
}

void SemanticAnalyzer::visit(CompileTimeRepeatStmtAST& node) {
    auto count = evalCompileTime(node.count.get(), node);
    if (!count || count->type != ConstValue::INT) {
        emitError("compile_time.repeat count must be a compile-time integer", node);
        return;
    }
    if (count->intVal < 0 || count->intVal > 1024) {
        emitError("compile_time.repeat count must be between 0 and 1024", node);
        return;
    }
    node.ctResolved = true;
    node.iterations = static_cast<int>(count->intVal);
    ++m_ctRepeatDepth;
    if (node.body) visit(*node.body);
    --m_ctRepeatDepth;
}

void SemanticAnalyzer::visit(CompileTimeIfStmtAST& node) {
    auto cond = evalCompileTime(node.cond.get(), node);
    if (!cond) {
        emitError("compile_time.if condition must be a compile-time constant", node);
        return;
    }
    if (cond->type != ConstValue::INT) {
        emitError("compile_time.if condition must be a boolean", node);
        return;
    }
    node.ctResolved = true;
    node.selectedThen = cond->intVal != 0;
    CompoundStmtAST* live = node.selectedThen ? node.thenStmt.get() : node.elseStmt.get();
    if (live) visit(*live);
}

void SemanticAnalyzer::visit(CompileTimeIfDeclAST& node) {
    // P1-04 / CT-03: 条件求值与分支选择（诊断文案 spec §3 钉死）。
    auto cond = evalCompileTime(node.cond.get(), node);
    if (!cond) {
        emitError("compile_time.if condition must be a compile-time constant", node);
        return;
    }
    if (cond->type != ConstValue::INT) {
        emitError("compile_time.if condition must be a boolean", node);
        return;
    }
    node.ctResolved = true;
    node.selectedThen = cond->intVal != 0;

    auto& deadDecls = node.selectedThen ? node.elseDecls : node.thenDecls;
    auto& liveDecls = node.selectedThen ? node.thenDecls : node.elseDecls;
    auto collectTypeNames = [](std::vector<std::unique_ptr<DeclAST>>& ds) {
        std::unordered_set<std::string> names;
        for (auto& d : ds) {
            if (auto* s = dynamic_cast<StructDeclAST*>(d.get())) names.insert(s->name);
            else if (auto* u = dynamic_cast<UnionDeclAST*>(d.get())) names.insert(u->name);
            else if (auto* e = dynamic_cast<EnumDeclAST*>(d.get())) names.insert(e->name);
        }
        return names;
    };
    auto liveNames = collectTypeNames(liveDecls);
    // 未选分支 parse 期注册的类型占位须撤销/毒化（Review Focus 1：占位不得
    // 被活代码静默使用）。类型引用在 parse 期已绑定为指针，故除 TypeContext
    // 注销外还须毒化对象；活分支的 sema 注册创建新对象，天然解毒。两分支
    // 都声明的同名类型（变体选择）不支持——毒化使使用处得到明确诊断。
    for (auto& d : deadDecls) {
        if (auto* s = dynamic_cast<StructDeclAST*>(d.get())) {
            TypeContext& tc = TypeContext::instance();
            Type* poison = s->isClassDecl
                ? static_cast<Type*>(tc.getClass(s->name))
                : static_cast<Type*>(tc.getStruct(s->name));
            if (!liveNames.count(s->name)) {
                if (s->isClassDecl) tc.removeClass(s->name);
                else tc.removeStruct(s->name);
            }
            if (poison) poison->ctDeadBranch = true;
        } else if (auto* u = dynamic_cast<UnionDeclAST*>(d.get())) {
            TypeContext& tc = TypeContext::instance();
            Type* poison = tc.getUnion(u->name);
            if (!liveNames.count(u->name)) tc.removeUnion(u->name);
            if (poison) poison->ctDeadBranch = true;
        } else if (auto* e = dynamic_cast<EnumDeclAST*>(d.get())) {
            TypeContext& tc = TypeContext::instance();
            Type* poison = tc.getEnum(e->name);
            if (!liveNames.count(e->name)) tc.removeEnum(e->name);
            if (poison) poison->ctDeadBranch = true;
        } else if (auto* u2 = dynamic_cast<UsingDeclAST*>(d.get())) {
            if (Type* poison = TypeContext::instance().getTypedef(u2->name)) {
                poison->ctDeadBranch = true;
            }
        }
    }

    auto& decls = node.selectedThen ? node.thenDecls : node.elseDecls;
    for (auto& d : decls) {
        if (d) analyzeTopLevelDecl(*d);
    }
}

void SemanticAnalyzer::analyzeTopLevelDecl(DeclAST& decl) {
    // Visibility applies per declaration (MOD-05/06). `main` is always
    // externally visible (program entry point); an exported namespace makes
    // all of its members exported.
    bool savedExported = currentDeclExported;
    bool isEntry = false;
    if (auto* fn = dynamic_cast<FunctionDeclAST*>(&decl)) {
        if (fn->name == "main") isEntry = true;
    }
    currentDeclExported = decl.isExported || namespaceExported || isEntry;

    if (auto* funcDecl = dynamic_cast<FunctionDeclAST*>(&decl)) {
        visit(*funcDecl);
    } else if (auto* varDecl = dynamic_cast<VarDeclAST*>(&decl)) {
        visit(*varDecl);
    } else if (auto* arrDecl = dynamic_cast<ArrayDeclAST*>(&decl)) {
        visit(*arrDecl);
    } else if (auto* structDecl = dynamic_cast<StructDeclAST*>(&decl)) {
        visit(*structDecl);
    } else if (auto* unionDecl = dynamic_cast<UnionDeclAST*>(&decl)) {
        visit(*unionDecl);
    } else if (auto* enumDecl = dynamic_cast<EnumDeclAST*>(&decl)) {
        visit(*enumDecl);
    } else if (auto* fwdDecl = dynamic_cast<ForwardDeclAST*>(&decl)) {
        visit(*fwdDecl);
    } else if (auto* usingDecl = dynamic_cast<UsingDeclAST*>(&decl)) {
        visit(*usingDecl);
    } else if (auto* moduleDecl = dynamic_cast<ModuleDeclAST*>(&decl)) {
        visit(*moduleDecl);
    } else if (auto* nsDecl = dynamic_cast<NamespaceDeclAST*>(&decl)) {
        visit(*nsDecl);
    } else if (auto* tplDecl = dynamic_cast<TemplateDeclAST*>(&decl)) {
        // P1-03 / GEN-01: 模板定义只注册，不在此 sema（GEN-06 定义处不检查）。
        // Namespace context: type templates are already flattened by Parser
        // (`NsTpl_Box`), but function/alias template names remain bare until
        // sema would normally call scopedName. Register them under the same
        // flattened key so `NsTpl::id<int32>` resolves.
        std::string tplName;
        if (auto* fn = dynamic_cast<FunctionDeclAST*>(tplDecl->decl.get()))
            tplName = fn->name;
        else if (auto* st = dynamic_cast<StructDeclAST*>(tplDecl->decl.get()))
            tplName = st->name;
        else if (auto* ud = dynamic_cast<UsingDeclAST*>(tplDecl->decl.get()))
            tplName = ud->name;
        if (!namespacePrefix.empty() && !tplName.empty() &&
            tplName.rfind(namespacePrefix, 0) != 0) {
            tplName = namespacePrefix + tplName;
        }
        TemplateRegistry::instance().registerTemplate(tplDecl, tplName);
    } else if (auto* multi = dynamic_cast<MultiVarDeclAST*>(&decl)) {
        for (auto& d : multi->decls) {
            if (auto* v = dynamic_cast<VarDeclAST*>(d.get())) visit(*v);
            else if (auto* a = dynamic_cast<ArrayDeclAST*>(d.get())) visit(*a);
            else if (auto* f = dynamic_cast<FunctionDeclAST*>(d.get())) visit(*f);
        }
    } else if (auto* ctAssert = dynamic_cast<CompileTimeAssertDeclAST*>(&decl)) {
        // P1-04 / CT-02: 顶层 static_assert——内层 MethodCall 走钩子求值。
        if (auto* call = dynamic_cast<MethodCallExprAST*>(ctAssert->call.get())) {
            visit(*call);
        }
    } else if (auto* ctIf = dynamic_cast<CompileTimeIfDeclAST*>(&decl)) {
        // P1-04 / CT-03: 条件编译——选中分支原位展开进声明序列。
        visit(*ctIf);
    }

    currentDeclExported = savedExported;
}

void SemanticAnalyzer::visit(TranslationUnitAST& node) {
    Scope* savedScope = currentScope;
    std::string savedModule = currentModule;
    Scope* savedActive = activeModuleScope;

    for (auto& decl : node.declarations) {
        if (!decl) continue;
        // Switch to the owning module's scope for this declaration group.
        enterModuleContext(decl->moduleName);
        analyzeTopLevelDecl(*decl);
    }

    currentScope = savedScope;
    currentModule = savedModule;
    activeModuleScope = savedActive;
}

void SemanticAnalyzer::visit(NamespaceDeclAST& node) {
    std::string saved = namespacePrefix;
    bool savedExported = namespaceExported;
    if (node.isExported) {
        namespaceExported = true;
    }
    std::string segment = mangleNamespaceName(node.name);
    if (!segment.empty()) {
        namespacePrefix += segment + "_";
    }
    for (auto& decl : node.declarations) {
        if (decl) {
            analyzeTopLevelDecl(*decl);
        }
    }
    namespacePrefix = saved;
    namespaceExported = savedExported;
}

void SemanticAnalyzer::analyze(TranslationUnitAST& ast) {
    errors.clear();
    visit(ast);

    // P1-03 / GEN-05: 克隆期嵌套使用点（模板体内 `Box<f64>` 字段）产生的
    // 待检实例——补 visit 并全部追加到翻译单元尾部供 codegen 出码。惰性
    // 保证：从未使用的模板不产生任何实例或符号。
    auto& pending = TemplateRegistry::instance().pendingInstances();
    while (!pending.empty()) {
        std::unique_ptr<DeclAST> decl = std::move(pending.front());
        pending.pop_front();
        if (auto* st = dynamic_cast<StructDeclAST*>(decl.get())) {
            if (!m_visitedInstances.count(st->name)) {
                m_visitedInstances.insert(st->name);
                visit(*st);
            }
        } else if (auto* fn = dynamic_cast<FunctionDeclAST*>(decl.get())) {
            if (!m_visitedInstances.count(fn->name)) {
                m_visitedInstances.insert(fn->name);
                visit(*fn);
            }
        }
        ast.declarations.push_back(std::move(decl));
    }
}

// P1-03 / GEN-03: 解析类型树中的 TypeInstance 占位——递归解析嵌套实参、
// 触发实例化、visit 实例 decl（补全 TypeContext 中的实例类型），原地把
// 占位节点替换为具体类型。sema 结束后 TypeInstance 不复存在。
Type* SemanticAnalyzer::resolveTypeInstance(Type* t, ASTNode& at) {
    if (!t) return nullptr;
    switch (t->kind) {
        case TypeKind::Pointer:
            t->base = resolveTypeInstance(t->base, at);
            return t;
        case TypeKind::Array: {
            auto* arr = static_cast<ArrayType*>(t);
            arr->elementType = resolveTypeInstance(arr->elementType, at);
            if (!arr->sizeParam.empty()) {
                emitError("unbound non-type template parameter '" + arr->sizeParam +
                              "' outside template body",
                          at);
                arr->sizeParam.clear();
            }
            return t;
        }
        case TypeKind::Slice: {
            auto* s = static_cast<SliceType*>(t);
            s->elementType = resolveTypeInstance(s->elementType, at);
            return t;
        }
        case TypeKind::Optional: {
            auto* o = static_cast<OptionalType*>(t);
            o->elementType = resolveTypeInstance(o->elementType, at);
            return t;
        }
        case TypeKind::Result: {
            auto* r = static_cast<ResultType*>(t);
            r->successType = resolveTypeInstance(r->successType, at);
            r->errorType = resolveTypeInstance(r->errorType, at);
            return t;
        }
        case TypeKind::Typedef: {
            auto* td = static_cast<TypedefType*>(t);
            td->aliasedType = resolveTypeInstance(td->aliasedType, at);
            return t;
        }
        case TypeKind::TypeInstance: {
            auto* use = static_cast<TypeInstanceType*>(t);
            // 评审 I2：真实 parser→sema 路径上的深度上限（嵌套实参在此自底
            // 向上解析，Registry 的克隆链计不到它们）。单出口：递归深度
            // 由 lambda 外的计数器守护。
            ++m_useDepth;
            if (m_useDepth > kMaxTemplateUseDepth) {
                emitError("template instantiation depth limit exceeded (64)", at);
                --m_useDepth;
                return nullptr;
            }
            Type* result = resolveTypeInstanceUse(use, at);
            --m_useDepth;
            return result;
        }
        case TypeKind::Struct: {
            auto* st = static_cast<StructType*>(t);
            if (st->isTemplatePattern) {                emitError("use of template '" + st->name + "' requires template arguments", at);
                return nullptr;
            }
            return t;
        }
        case TypeKind::Class: {
            auto* ct = static_cast<ClassType*>(t);
            if (ct->isTemplatePattern) {
                emitError("use of template '" + ct->name + "' requires template arguments", at);
                return nullptr;
            }
            return t;
        }
        default:
            return t;
    }
}

// P1-03 / INH-05: 模板基类实参拼写 → 类型。按裸名与 scoped 名在各类型表
// 中查找；CRTP 场景下派生类自身的占位 ClassType 已由 getOrCreateClass 建。
Type* SemanticAnalyzer::resolveTypeByName(const std::string& name) {
    if (name.find("::") == std::string::npos && name.find('.') == std::string::npos) {
        if (Type* local = typeCtx->getLocalTypedef(name)) return local;
    }
    auto tryOne = [&](const std::string& n) -> Type* {
        if (Type* t = typeCtx->getClass(n)) return t;
        if (Type* t = typeCtx->getStruct(n)) return t;
        if (Type* t = typeCtx->getTypedef(n)) return t;
        if (Type* t = typeCtx->getEnum(n)) return t;
        if (Type* t = typeCtx->getUnion(n)) return t;
        return nullptr;
    };
    if (Type* t = tryOne(name)) return t;
    std::string scoped = scopedName(name);
    if (scoped != name) return tryOne(scoped);
    return nullptr;
}

// 别名展开产物中的实例类型：若其实例 decl 已克隆但未 visit，立即补齐
// （字段访问要求布局当场完整）。
// P1-03 / GEN-03: TypeInstance 占位的解析主体（深度计数在调用方）。
Type* SemanticAnalyzer::resolveTypeInstanceUse(TypeInstanceType* use, ASTNode& at) {
    // 嵌套实例实参先解析（Box<Box<i32>> 的内层先行完成）。
    for (auto*& arg : use->typeArgs) arg = resolveTypeInstance(arg, at);

    auto& reg = TemplateRegistry::instance();
    std::string name =
        TemplateRegistry::instanceName(use->templateName, use->typeArgs, use->valueArgs);

    // 别名模板：展开目标类型（键缓存防重复展开），产物中的实例
    // 类型立即补 visit——字段访问要求布局当场完整。
    auto* tpl = reg.find(use->templateName);
    if (tpl && dynamic_cast<UsingDeclAST*>(tpl->decl.get())) {
        if (m_aliasCache.count(name)) return m_aliasCache[name];
        std::unordered_map<std::string, Type*> typeArgMap;
        std::unordered_map<std::string, long long> valueArgMap;
        size_t ti = 0, vi = 0;
        for (auto& p : tpl->params) {
            if (p.isType) {
                if (ti < use->typeArgs.size()) typeArgMap[p.name] = use->typeArgs[ti++];
            } else {
                if (vi < use->valueArgs.size()) valueArgMap[p.name] = use->valueArgs[vi++];
            }
        }
        Type* expanded = TemplateInstantiator(typeArgMap, valueArgMap)
                             .rewrite(
                                 static_cast<UsingDeclAST*>(tpl->decl.get())->aliasedType);
        m_aliasCache[name] = expanded;
        ensureInstanceVisited(expanded, at);
        return expanded;
    }

    auto* decl = reg.instantiateClass(use->templateName, use->typeArgs, use->valueArgs);
    if (!decl) {
        emitError("cannot instantiate template '" + use->templateName + "'", at);
        return nullptr;
    }

    // 同一实例只 visit 一次（去重键已在 Registry 保证 decl 唯一）。
    if (!m_visitedInstances.count(name)) {
        m_visitedInstances.insert(name);
        m_instStack.push_back(use->templateName + "<...>");
        visit(*static_cast<StructDeclAST*>(decl));
        m_instStack.pop_back();
    }

    if (auto* ct = typeCtx->getClass(name)) return ct;
    return typeCtx->getStruct(name);
}

void SemanticAnalyzer::ensureInstanceVisited(Type* t, ASTNode& at) {
    if (!t) return;
    std::string name;
    if (t->kind == TypeKind::Struct) name = static_cast<StructType*>(t)->name;
    else if (t->kind == TypeKind::Class) name = static_cast<ClassType*>(t)->name;
    else return;
    if (name.find('$') == std::string::npos) return;
    auto* decl = TemplateRegistry::instance().instanceDeclFor(name);
    if (decl && !m_visitedInstances.count(name)) {
        m_visitedInstances.insert(name);
        m_instStack.push_back(name);
        visit(*static_cast<StructDeclAST*>(decl));
        m_instStack.pop_back();
    }
}

// P1-03 / INH-05 / GEN-09: 基类实例惰性方法的首次调用触发——this 插入与
// body 检查在此进行（与 class 分支的方法循环一致）。
void SemanticAnalyzer::visitLazyMethodsOf(const std::string& className) {
    auto it = m_pendingLazyMethods.find(className);
    if (it == m_pendingLazyMethods.end()) return;
    auto pending = std::move(it->second);
    m_pendingLazyMethods.erase(it);
    m_lazyInstanceMethods.erase(className);

    auto* classType = typeCtx->getClass(className);
    if (!classType) return;
    ClassType* savedClass = currentClass;
    currentClass = classType;
    for (auto* method : pending) {
        if (method->isStatic) {
            m_inStaticMethod = true;
            visit(*method);
            m_inStaticMethod = false;
        } else {
            auto thisType = new Type(TypeKind::Pointer, classType);
            method->params.insert(method->params.begin(),
                                  std::make_unique<ParamDeclAST>("this", thisType));
            visit(*method);
        }
    }
    currentClass = savedClass;
}

// CRTP：模板基类实例的字段若为派生类值语义自嵌套（`D next`），在实例化点
// 拒绝——类型尺寸无限。
// 评审 I3 + INH-05：类/结构体字段值语义自引用（`A x` / CRTP 的 `D next`）
// ——类型尺寸无限，诊断而非编译器崩溃。模板实例化中沿用原文案。
void SemanticAnalyzer::checkInstanceFieldComplete(StructDeclAST& node) {
    for (auto& field : node.fields) {
        Type* ft = field.type;
        while (ft && ft->kind == TypeKind::Typedef) {
            ft = static_cast<TypedefType*>(ft)->aliasedType;
        }
        if (!ft || (ft->kind != TypeKind::Class && ft->kind != TypeKind::Struct)) continue;
        std::string fname = ft->kind == TypeKind::Class
                                ? static_cast<ClassType*>(ft)->name
                                : static_cast<StructType*>(ft)->name;
        if (std::find(m_definingStack.begin(), m_definingStack.end(), fname) !=
            m_definingStack.end()) {
            if (!m_instStack.empty()) {
                emitError("recursive instantiation of template '" + m_instStack.back() +
                              "': field '" + field.name + "' has value type '" + fname + "'",
                          node);
            } else {
                emitError("field '" + field.name + "' of '" + fname +
                              "' cannot have its own type by value",
                          node);
            }
            return;
        }
    }
}

// 最终评审 C1：输出约束是否为内存类（m/o/V）。剥离 =/+（前缀）与 &（早破坏）
// 后扫描约束码；命中 m/o/V 即内存类（GCC 内存约束，含 Um 等多字母内存码）。
// `{寄存器}` 限定与 `|` 多选不展开——含内存字母即拒绝。寄存器类字母
// （r/q/Q/R/S/T/A/a-d/f/x/y/k/j/e/v 等）不含这三个字母，不受影响。
static bool hasMemoryClassOutputConstraint(const std::string& c) {
    size_t i = 0;
    while (i < c.size() && (c[i] == '=' || c[i] == '+' || c[i] == '&')) ++i;
    for (; i < c.size() && c[i] != '{' && c[i] != '|'; ++i) {
        if (c[i] == 'm' || c[i] == 'o' || c[i] == 'V') return true;
    }
    return false;
}

// P1-08 / DEC-09: inline asm —— 节点恒为 void 值（D3），outputs 必须是左值标量，
// inputs 必须是标量，约束串非空；模板 `%` 后只能是 `%`（转义）或数字（操作数
// 引用）。约束/模板的语义合法性交由 LLVM 后端（D2）。
void SemanticAnalyzer::visit(AsmExprAST& node) {
    node.type = typeCtx->getVoid();
    node.isLValue = false;

    auto checkOperand = [&](AsmOperand& op, bool mustBeLValue, const char* what) -> Type* {
        if (op.constraint.empty()) {
            emitError(DiagnosticCode::SemAsmOperand,
                      std::string("empty asm ") + what + " constraint", node);
        }
        Type* t = getExprType(*op.expr);
        if (mustBeLValue && !op.expr->isLValue) {
            emitError(DiagnosticCode::SemAsmOperand,
                      "asm output operand must be an lvalue", node);
        }
        if (t && !isScalarType(t)) {
            emitError(DiagnosticCode::SemAsmOperand,
                      std::string("asm ") + what + " operand must be a scalar type", node);
        }
        return t;
    };

    node.outputTypes.clear();
    for (auto& op : node.outputs) {
        node.outputTypes.push_back(checkOperand(op, true, "output"));
        // 最终评审 C1：本编译器的输出模型 = LLVM call 返回值，LLVM 21 对非间接
        // 内存类输出约束（m/o/V）会走 SelectionDAGBuilder 崩溃路径（SEGV，实证
        // rc=139）。sema 拒绝内存类输出，防 GCC 合法源码击穿编译器；寄存器输出
        // （r/q/Q/...）与只读内存输入（m）不受影响。见 spec §5 注释。
        if (hasMemoryClassOutputConstraint(op.constraint)) {
            emitError(DiagnosticCode::SemAsmOperand,
                      "asm memory-class output constraint is not supported "
                      "(register-class outputs only)", node);
        }
    }
    node.inputTypes.clear();
    for (auto& op : node.inputs) {
        node.inputTypes.push_back(checkOperand(op, false, "input"));
    }

    // SG-19: collect `[name]` operands; indices follow LLVM/GCC order
    // (outputs first, then inputs).
    std::unordered_map<std::string, unsigned> namedOperands;
    auto registerName = [&](const AsmOperand& op, unsigned index) {
        if (op.name.empty()) return;
        if (!namedOperands.emplace(op.name, index).second) {
            emitError(DiagnosticCode::SemAsmOperand,
                      "duplicate asm operand name '" + op.name + "'", node);
        }
    };
    for (unsigned i = 0; i < node.outputs.size(); ++i) {
        registerName(node.outputs[i], i);
    }
    for (unsigned i = 0; i < node.inputs.size(); ++i) {
        registerName(node.inputs[i], static_cast<unsigned>(node.outputs.size()) + i);
    }

    const std::string& tpl = node.asmTemplate;
    for (size_t i = 0; i < tpl.size(); ++i) {
        if (tpl[i] != '%') continue;
        if (i + 1 >= tpl.size()) {
            emitError(DiagnosticCode::SemAsmTemplate,
                      "invalid asm template: trailing '%' must be '%%' or a digit reference", node);
            break;
        }
        const char next = tpl[i + 1];
        if (next == '%' || (next >= '0' && next <= '9')) {
            ++i; // 已确认的一对（转义或操作数引用）整体跳过
            continue;
        }
        if (next == '[') {
            const size_t close = tpl.find(']', i + 2);
            if (close == std::string::npos) {
                emitError(DiagnosticCode::SemAsmTemplate,
                          "invalid asm template: missing ']' after operand name", node);
                break;
            }
            const std::string name = tpl.substr(i + 2, close - i - 2);
            if (namedOperands.find(name) == namedOperands.end()) {
                emitError(DiagnosticCode::SemAsmTemplate,
                          "unknown asm operand name '" + name + "'", node);
            }
            i = close;
            continue;
        }
        emitError(DiagnosticCode::SemAsmTemplate,
                  "invalid asm template: '%' must be followed by '%', a digit, or '[name]'", node);
        break;
    }
}

void SemanticAnalyzer::visit(ExprAST& expr) {
    if (auto* e = dynamic_cast<NumberExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<BoolLiteralExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<FloatExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<CharExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<StringExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<VariableExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<BinaryExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<UnaryExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<CallExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<AssignmentExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<TernaryExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<CastExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<AsmExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<CommaExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<PostfixIncDecExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<ArrayAccessExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<RangeExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<MemberAccessExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<SizeofExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<OffsetOfExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<MethodCallExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<SizeofExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<InitializerListExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<CompoundLiteralExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<LambdaExprAST*>(&expr)) { visit(*e); return; }
    if (auto* e = dynamic_cast<LambdaCallExprAST*>(&expr)) { visit(*e); return; }
}

void SemanticAnalyzer::visit(StmtAST& stmt) {
    if (auto* s = dynamic_cast<CompoundStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<ExprStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<ReturnStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<IfStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<WhileStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<DoWhileStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<ForStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<SwitchStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<BreakStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<ContinueStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<NullStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<DeclStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<DeferStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<CompileTimeIfStmtAST*>(&stmt)) { visit(*s); return; }
    if (auto* s = dynamic_cast<CompileTimeRepeatStmtAST*>(&stmt)) { visit(*s); return; }
}

bool SemanticAnalyzer::isTypeRedefined(const std::string& name) const {
    // Redef 轮: all four type kinds share one name namespace — a completed
    // type of any kind blocks a (re)definition of the same name.
    if (auto* t = typeCtx->getClass(name); t && t->isComplete) return true;
    if (auto* t = typeCtx->getStruct(name); t && t->isComplete) return true;
    if (auto* t = typeCtx->getUnion(name); t && t->isComplete) return true;
    if (auto* t = typeCtx->getEnum(name); t && t->isComplete) return true;
    return false;
}

bool SemanticAnalyzer::hasCircularInheritance(const std::string& className, const std::string& baseClass) const {
    std::vector<std::string> chain;
    chain.push_back(className);

    std::string current = baseClass;
    while (!current.empty()) {
        if (std::find(chain.begin(), chain.end(), current) != chain.end()) {
            return true;
        }
        chain.push_back(current);

        // INH-01 评审 C1: walk BOTH maps — a struct chain must be followed
        // too, otherwise a redefinition-shaped cycle is accepted silently.
        std::string next;
        if (auto* type = typeCtx->getClass(current)) {
            next = type->baseClass;
        } else if (auto* stype = typeCtx->getStruct(current)) {
            next = stype->baseClass;
        }
        current = next;
    }

    return false;
}

// PAR-12 / INH-04: 沿基类链判定 derived 是否为 base 的派生（含 Struct/Class
// 双形态，与 hasCircularInheritance 的 walk 同构）。仅公有继承（INH-01）。
bool SemanticAnalyzer::isDerivedFrom(Type* derived, Type* base) const {
    derived = stripTypedef(derived);
    base = stripTypedef(base);
    if (!derived || !base) return false;
    if (derived->kind != TypeKind::Struct && derived->kind != TypeKind::Class) return false;
    if (base->kind != TypeKind::Struct && base->kind != TypeKind::Class) return false;
    std::string current;
    if (derived->kind == TypeKind::Class) current = static_cast<ClassType*>(derived)->baseClass;
    else current = static_cast<StructType*>(derived)->baseClass;
    std::string target = base->kind == TypeKind::Class
        ? static_cast<ClassType*>(base)->name
        : static_cast<StructType*>(base)->name;
    int guard = 0;
    while (!current.empty() && guard++ < 128) {
        if (current == target) return true;
        std::string next;
        if (auto* t = typeCtx->getClass(current)) next = t->baseClass;
        else if (auto* st = typeCtx->getStruct(current)) next = st->baseClass;
        current = next;
    }
    return false;
}

// INH-06: 派生类方法内经 this-> 访问基类 protected 成员放行——当前类与
// 定义类相同，或（level 为 protected 且）当前类是定义类的派生类。
// private 仅定义类内可访问（派生类不放开）。
bool SemanticAnalyzer::canAccessMember(ClassType* definingClass, AccessLevel level) const {
    if (!definingClass) return false;
    if (level == AccessLevel::Public) return true;
    if (currentClass == definingClass) return true;
    if (!currentClass || level != AccessLevel::Protected) return false;
    return isDerivedFrom(currentClass, definingClass);
}

Symbol* SemanticAnalyzer::resolveMethod(ClassType* classType, const std::string& methodName,
                                        const std::vector<Type*>& argTypes,
                                        ClassType** defining, int depth) {
    auto defaultCountFor = [&](const std::string& name) -> size_t {
        auto it = m_methodDefaultCounts.find(classType->name + "::" + name);
        return it != m_methodDefaultCounts.end() ? static_cast<size_t>(it->second) : 0;
    };
    // FUN-09: argCount viable in [minArgs, fixed-1(this)].
    auto viableArity = [](size_t fixed, size_t minArgs, size_t argc) {
        return argc >= minArgs && argc < fixed;
    };

    for (auto& method : classType->methods) {
        if (method.first == methodName) {
            size_t fixed = method.second->paramTypes.size(); // 含 this
            size_t defaultCount = defaultCountFor(methodName);
            size_t minArgs = fixed > 1 + defaultCount ? fixed - 1 - defaultCount : 0;
            if (!viableArity(fixed, minArgs, argTypes.size())) continue;
            bool match = true;
            for (size_t i = 0; i < argTypes.size(); ++i) {
                if (method.second->paramTypes[i + 1] != argTypes[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                Symbol* sym = new Symbol(methodName, method.second);
                if (defining) *defining = classType;
                return sym;
            }
        }
    }

    // Phase 2: decay/implicit conversions — first candidate (declaration
    // order) where every argument converts to the declared parameter type
    // (conversionRank >= 0). A null argument type disqualifies the candidate
    // (the error was already reported when resolving the argument).
    for (auto& method : classType->methods) {
        if (method.first != methodName) continue;
        auto* funcType = method.second;
        size_t fixed = funcType->paramTypes.size(); // 含 this
        size_t defaultCount = defaultCountFor(methodName);
        size_t minArgs = fixed > 1 + defaultCount ? fixed - 1 - defaultCount : 0;
        if (!viableArity(fixed, minArgs, argTypes.size())) continue;
        bool match = true;
        for (size_t i = 0; i < argTypes.size(); ++i) {
            if (!argTypes[i] ||
                conversionRank(argTypes[i], funcType->paramTypes[i + 1]) < 0) {
                match = false;
                break;
            }
        }
        if (match) {
            Symbol* sym = new Symbol(methodName, funcType);
            if (defining) *defining = classType;
            return sym;
        }
    }

    if (!classType->baseClass.empty() && depth < 64) {
        auto* baseType = typeCtx->getClass(classType->baseClass);
        if (baseType) {
            Symbol* result = resolveMethod(baseType, methodName, argTypes, defining, depth + 1);
            if (result) return result;
        }
    }

    return nullptr;
}

bool SemanticAnalyzer::isMethodCall(ExprAST& expr) {
    auto* memberAccess = dynamic_cast<MemberAccessExprAST*>(&expr);
    if (!memberAccess) return false;

    Type* objType = memberAccess->object->type;
    if (!objType) return false;

    if (memberAccess->accessKind == MemberAccessKind::Arrow) {
        if (objType->kind == TypeKind::Pointer && objType->base &&
            objType->base->kind == TypeKind::Class) {
            return true;
        }
    } else {
        if (objType->kind == TypeKind::Class) {
            return true;
        }
    }

    return false;
}

// 新增AST节点的visit方法实现
void SemanticAnalyzer::visit(UsingDeclAST& node) {
    // PAR-24: using 是唯一别名形式——注解目标校验、弃用折叠与内联 enum
    // 枚举量注册都与原 typedef 语义对齐。
    validateAnnotations(node.annotations, node, "using");

    // `[[deprecated]] using T = ...;` —— 折叠到别名类型，使用点告警。
    auto markDeprecated = [&](Type* td) {
        for (const auto& ann : node.annotations) {
            if (ann.name == "deprecated") {
                td->deprecated = true;
                if (ann.args.size() == 1 &&
                    ann.args[0].kind == AnnotationArg::Kind::String) {
                    td->deprecatedMsg = ann.args[0].text;
                }
            }
        }
    };

    if (node.isBlockScope) {
        // Block aliases are type-only; they never declare a value symbol.
        Type* storedAlias = typeCtx->addLocalTypedef(node.bareName, node.aliasedType);
        if (node.aliasWrapper) markDeprecated(node.aliasWrapper);
        if (storedAlias && storedAlias != node.aliasWrapper) markDeprecated(storedAlias);
        return;
    }

    // 解析期已登记过一次别名；sema 再登记会产生新的 TypedefType 包装。
    // 已经解析完的声明（如 `T x;`）持有解析期包装，弃用标志必须同时落到
    // 两份包装上，否则使用点看不到 deprecated。
    Type* parseTimeAlias = typeCtx->getTypedef(node.name);
    typeCtx->addTypedef(node.name, node.aliasedType);
    Type* storedAlias = typeCtx->getTypedef(node.name);
    if (parseTimeAlias) markDeprecated(parseTimeAlias);
    if (storedAlias && storedAlias != parseTimeAlias) markDeprecated(storedAlias);

    // `using E = enum { A, B };` 的枚举量在别名作用域可见（TYP-25 对齐）。
    if (Type* t = stripTypedef(node.aliasedType); t && t->kind == TypeKind::Enum) {
        auto* enumType = static_cast<EnumType*>(t);
        for (auto& val : enumType->values) {
            enumConstants[scopedName(val.first)] = {enumType, val.second};
        }
    }

    declare(node.name, node.aliasedType);
}

void SemanticAnalyzer::visit(ModuleDeclAST& node) {
    // P1-05 / ANN-06: 模块注解验证（本轮仅解析记录语义，不做目标限制）。
    validateAnnotations(node.annotations, node, "module");
}

void SemanticAnalyzer::visit(DeferStmtAST& node) {
    // defer 语句：检查表达式是否有效
    if (node.callExpr) {
        visit(*node.callExpr);
    }
}

// ===== SEM-01/02: definite-assignment analysis =====

void SemanticAnalyzer::initRead(const std::string& name, const ASTNode& node,
                                std::unordered_set<std::string>& state) {
    if (!initLocals || !initLocals->count(name)) return;   // not a local of this function
    if (state.count(name)) return;                         // definitely assigned
    if (initWarned.count(name)) return;                    // warn once per variable
    initWarned.insert(name);
    emitWarning(DiagnosticCode::WarnUninitializedVariable,
                "variable '" + name + "' may be used before it is initialized", node);
}

void SemanticAnalyzer::collectLocalNames(StmtAST* stmt, std::unordered_set<std::string>& out) {
    if (!stmt) return;
    if (auto* block = dynamic_cast<CompoundStmtAST*>(stmt)) {
        for (auto& s : block->stmts) collectLocalNames(s.get(), out);
        return;
    }
    if (auto* ds = dynamic_cast<DeclStmtAST*>(stmt)) {
        if (auto* v = dynamic_cast<VarDeclAST*>(ds->decl.get())) {
            out.insert(v->name);
        } else if (auto* a = dynamic_cast<ArrayDeclAST*>(ds->decl.get())) {
            out.insert(a->name);
        } else if (auto* m = dynamic_cast<MultiVarDeclAST*>(ds->decl.get())) {
            for (auto& d : m->decls) {
                if (auto* v = dynamic_cast<VarDeclAST*>(d.get())) out.insert(v->name);
                else if (auto* a = dynamic_cast<ArrayDeclAST*>(d.get())) out.insert(a->name);
            }
        }
        return;
    }
    if (auto* ifs = dynamic_cast<IfStmtAST*>(stmt)) {
        collectLocalNames(ifs->thenStmt.get(), out);
        collectLocalNames(ifs->elseStmt.get(), out);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmtAST*>(stmt)) { collectLocalNames(w->body.get(), out); return; }
    if (auto* d = dynamic_cast<DoWhileStmtAST*>(stmt)) { collectLocalNames(d->body.get(), out); return; }
    if (auto* f = dynamic_cast<ForStmtAST*>(stmt)) {
        collectLocalNames(f->init.get(), out);
        collectLocalNames(f->body.get(), out);
        return;
    }
    if (auto* sw = dynamic_cast<SwitchStmtAST*>(stmt)) {
        for (auto& c : sw->cases) collectLocalNames(c.get(), out);
        return;
    }
}

void SemanticAnalyzer::initWalkExpr(ExprAST* expr, std::unordered_set<std::string>& state) {
    if (!expr) return;

    if (auto* var = dynamic_cast<VariableExprAST*>(expr)) {
        initRead(var->name, *var, state);
        return;
    }
    if (auto* bin = dynamic_cast<BinaryExprAST*>(expr)) {
        initWalkExpr(bin->left.get(), state);
        initWalkExpr(bin->right.get(), state);
        return;
    }
    if (auto* un = dynamic_cast<UnaryExprAST*>(expr)) {
        if (un->op == UnaryOp::AddressOf) {
            // Taking an address does not read the object; the pointee may be
            // written through it, so treat it as initialized from here on.
            if (auto* v = dynamic_cast<VariableExprAST*>(un->operand.get())) {
                state.insert(v->name);
            } else {
                initWalkExpr(un->operand.get(), state);
            }
            return;
        }
        if ((un->op == UnaryOp::PreInc || un->op == UnaryOp::PreDec)) {
            if (auto* v = dynamic_cast<VariableExprAST*>(un->operand.get())) {
                initRead(v->name, *v, state);
                state.insert(v->name);
                return;
            }
        }
        initWalkExpr(un->operand.get(), state);
        return;
    }
    if (auto* asn = dynamic_cast<AssignmentExprAST*>(expr)) {
        initWalkExpr(asn->rhs.get(), state);
        ExprAST* target = asn->lhs.get();
        if (asn->op != AssignOp::Assign && target) {
            // Compound assignment reads the target first.
            if (auto* v = dynamic_cast<VariableExprAST*>(target)) {
                initRead(v->name, *v, state);
            } else {
                initWalkExpr(target, state);
            }
        }
        // Record the write. For member/index targets, mark the base variable.
        ExprAST* base = target;
        while (auto* ma = dynamic_cast<MemberAccessExprAST*>(base)) base = ma->object.get();
        while (auto* aa = dynamic_cast<ArrayAccessExprAST*>(base)) base = aa->array.get();
        if (auto* v = dynamic_cast<VariableExprAST*>(base)) {
            state.insert(v->name);
        } else if (target && asn->op == AssignOp::Assign) {
            initWalkExpr(target, state);
        }
        return;
    }
    if (auto* post = dynamic_cast<PostfixIncDecExprAST*>(expr)) {
        if (auto* v = dynamic_cast<VariableExprAST*>(post->operand.get())) {
            initRead(v->name, *v, state);
            state.insert(v->name);
            return;
        }
        initWalkExpr(post->operand.get(), state);
        return;
    }
    if (auto* tern = dynamic_cast<TernaryExprAST*>(expr)) {
        initWalkExpr(tern->cond.get(), state);
        auto thenState = state;
        auto elseState = state;
        initWalkExpr(tern->then.get(), thenState);
        initWalkExpr(tern->elseExpr.get(), elseState);
        std::unordered_set<std::string> merged;
        for (const auto& n : thenState) {
            if (elseState.count(n)) merged.insert(n);
        }
        state = std::move(merged);
        return;
    }
    if (auto* cast = dynamic_cast<CastExprAST*>(expr)) {
        initWalkExpr(cast->expr.get(), state);
        return;
    }
    if (auto* comma = dynamic_cast<CommaExprAST*>(expr)) {
        initWalkExpr(comma->left.get(), state);
        initWalkExpr(comma->right.get(), state);
        return;
    }
    if (auto* arr = dynamic_cast<ArrayAccessExprAST*>(expr)) {
        initWalkExpr(arr->array.get(), state);
        initWalkExpr(arr->index.get(), state);
        return;
    }
    if (auto* range = dynamic_cast<RangeExprAST*>(expr)) {
        initWalkExpr(range->object.get(), state);
        initWalkExpr(range->begin.get(), state);
        initWalkExpr(range->end.get(), state);
        return;
    }
    if (auto* mem = dynamic_cast<MemberAccessExprAST*>(expr)) {
        initWalkExpr(mem->object.get(), state);
        return;
    }
    if (auto* call = dynamic_cast<CallExprAST*>(expr)) {
        if (call->isIndirect) {
            initRead(call->callee, *call, state);  // function-pointer value
        }
        for (auto& a : call->args) initWalkExpr(a.get(), state);
        return;
    }
    if (auto* mc = dynamic_cast<MethodCallExprAST*>(expr)) {
        initWalkExpr(mc->object.get(), state);
        for (auto& a : mc->args) initWalkExpr(a.get(), state);
        return;
    }
    if (auto* il = dynamic_cast<InitializerListExprAST*>(expr)) {
        for (auto& e : il->initializers) initWalkExpr(e.get(), state);
        return;
    }
    // SizeofExprAST is unevaluated; number/string/char literals have no effect.
}

void SemanticAnalyzer::initWalkStmt(StmtAST* stmt, std::unordered_set<std::string>& state) {
    if (!stmt) return;

    if (auto* block = dynamic_cast<CompoundStmtAST*>(stmt)) {
        for (auto& s : block->stmts) initWalkStmt(s.get(), state);
        return;
    }
    if (auto* es = dynamic_cast<ExprStmtAST*>(stmt)) {
        initWalkExpr(es->expr.get(), state);
        return;
    }
    if (auto* ds = dynamic_cast<DeclStmtAST*>(stmt)) {
        auto handle = [&](DeclAST* d) {
            // Only an initializer makes a local definitely assigned; a bare
            // declaration leaves it may-be-uninitialized.
            if (auto* v = dynamic_cast<VarDeclAST*>(d)) {
                if (v->initExpr) {
                    initWalkExpr(v->initExpr.get(), state);
                    state.insert(v->name);
                }
            } else if (auto* a = dynamic_cast<ArrayDeclAST*>(d)) {
                if (a->initExpr) {
                    initWalkExpr(a->initExpr.get(), state);
                    state.insert(a->name);
                }
            }
        };
        if (auto* m = dynamic_cast<MultiVarDeclAST*>(ds->decl.get())) {
            for (auto& d : m->decls) handle(d.get());
        } else {
            handle(ds->decl.get());
        }
        return;
    }
    if (auto* ifs = dynamic_cast<IfStmtAST*>(stmt)) {
        initWalkExpr(ifs->cond.get(), state);
        auto thenState = state;
        auto elseState = state;
        initWalkStmt(ifs->thenStmt.get(), thenState);
        initWalkStmt(ifs->elseStmt.get(), elseState);
        // Join: keep only variables assigned on both paths (a variable first
        // assigned on both branches becomes definitely assigned).
        std::unordered_set<std::string> merged;
        for (const auto& n : thenState) {
            if (elseState.count(n)) merged.insert(n);
        }
        state = std::move(merged);
        return;
    }
    if (auto* w = dynamic_cast<WhileStmtAST*>(stmt)) {
        initWalkExpr(w->cond.get(), state);
        auto bodyState = state;                 // loop may run zero times
        initWalkStmt(w->body.get(), bodyState);
        return;
    }
    if (auto* d = dynamic_cast<DoWhileStmtAST*>(stmt)) {
        auto bodyState = state;                 // body runs at least once
        initWalkStmt(d->body.get(), bodyState);
        initWalkExpr(d->cond.get(), bodyState);
        state = bodyState;
        return;
    }
    if (auto* f = dynamic_cast<ForStmtAST*>(stmt)) {
        if (f->init) initWalkStmt(f->init.get(), state);
        if (f->cond) initWalkExpr(f->cond.get(), state);
        auto loopState = state;
        initWalkStmt(f->body.get(), loopState);
        if (f->inc) initWalkExpr(f->inc.get(), loopState);
        return;                                 // exiting the loop is possible with 0 iterations
    }
    if (auto* sw = dynamic_cast<SwitchStmtAST*>(stmt)) {
        initWalkExpr(sw->cond.get(), state);
        for (auto& c : sw->cases) {
            auto caseState = state;
            initWalkStmt(c.get(), caseState);
        }
        return;                                 // conservative: no case may run
    }
    if (auto* ret = dynamic_cast<ReturnStmtAST*>(stmt)) {
        initWalkExpr(ret->value.get(), state);
        return;
    }
    // Break/continue/goto/null: no effect on initialization state.
}

void SemanticAnalyzer::checkInitialization(FunctionDeclAST& node) {
    if (!node.body) return;

    std::unordered_set<std::string> locals;
    collectLocalNames(node.body.get(), locals);
    for (auto& p : node.params) locals.insert(p->name);
    if (locals.empty()) return;

    initLocals = &locals;
    initWarned.clear();

    std::unordered_set<std::string> state;
    for (auto& p : node.params) state.insert(p->name);   // parameters are initialized

    initWalkStmt(node.body.get(), state);

    initLocals = nullptr;
}
