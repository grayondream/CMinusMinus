#include "sema/CompileTimeEvaluator.h"

#include "ast/Expr.h"
#include "ast/LayoutBuilder.h"
#include "ast/Decl.h"
#include "ast/Type.h"
#include "ast/Symbol.h"
#include "sema/SemanticAnalyzer.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_set>

using ConstValue = CompileTimeEvaluator::ConstValue;

// P1-04 / CT-04: host 目标映射（spec §4）：triple → os/arch/cpu 查询值。
static std::string ctTargetOs() {
    std::string triple = llvm::sys::getDefaultTargetTriple();
    if (triple.find("linux") != std::string::npos) return "linux";
    if (triple.find("darwin") != std::string::npos
        || triple.find("apple") != std::string::npos) return "macos";
    if (triple.find("windows") != std::string::npos
        || triple.find("mingw") != std::string::npos) return "windows";
    return "unknown";
}

static std::string ctTargetArch() {
    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    return triple.getArchName().str();
}

static std::string ctTargetTriple() {
    return llvm::sys::getDefaultTargetTriple();
}

static std::string ctEndianName(const llvm::DataLayout& dl) {
    return dl.isLittleEndian() ? "little" : "big";
}

// CTX-01: semver comparison for compile_time.version.at_least.
static bool ctVersionAtLeast(const std::string& requested) {
    unsigned want[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3 && pos <= requested.size(); ++i) {
        size_t dot = requested.find('.', pos);
        std::string part = requested.substr(pos, dot == std::string::npos
                                                     ? std::string::npos
                                                     : dot - pos);
        if (part.empty()) return false;
        try {
            want[i] = static_cast<unsigned>(std::stoul(part));
        } catch (...) {
            return false;
        }
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    constexpr unsigned have[3] = {1, 0, 0};
    for (int i = 0; i < 3; ++i) {
        if (have[i] != want[i]) return have[i] > want[i];
    }
    return true;
}

CompileTimeEvaluator::CompileTimeEvaluator(SemanticAnalyzer& sema)
    : m_sema(sema) {
    // P1-04 / CT-06: 独立 LLVMContext/Module 承载布局查询（DataLayout 与
    // codegen 的 host 目标一致——默认布局即 64-bit LP64，与 x86_64/aarch64 匹配）。
    m_llvmCtx = std::make_unique<llvm::LLVMContext>();
    m_module = std::make_unique<llvm::Module>("compile_time_layout", *m_llvmCtx);
}

CompileTimeEvaluator::~CompileTimeEvaluator() = default;

std::optional<ConstValue> CompileTimeEvaluator::eval(ExprAST* expr, ASTNode& at) {
    if (!expr) {
        return std::nullopt;
    }
    if (m_depth >= kMaxDepth) {
        if (!m_depthReported) {
            m_sema.emitError("compile_time evaluation depth limit exceeded (64)", at);
            m_depthReported = true;
        }
        return std::nullopt;
    }

    struct DepthGuard {
        CompileTimeEvaluator& ev;
        ~DepthGuard() {
            if (--ev.m_depth == 0) {
                ev.m_depthReported = false;
            }
        }
    } guard{*this};
    ++m_depth;

    if (auto* num = dynamic_cast<NumberExprAST*>(expr)) {
        if (num->valueHi != 0) {
            // 128 位字面量：编译期求值/折叠仍按 64 位 ConstValue 承载——
            // 高 64 位无法无损表示，拒绝并给出明确诊断（运行时值不受影响）。
            m_sema.emitError("128-bit integer literal is not supported in a constant expression", *expr);
            return std::nullopt;
        }
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

    // Found issue: bool 字面量按整数（0/1）参与编译期求值。
    if (auto* bl = dynamic_cast<BoolLiteralExprAST*>(expr)) {
        return ctInt(bl->value ? 1 : 0, 1, true, true);
    }

    if (auto* flt = dynamic_cast<FloatExprAST*>(expr)) {
        ConstValue cv;
        cv.type = ConstValue::DOUBLE;
        cv.doubleVal = flt->value;
        return cv;
    }

    if (auto* chr = dynamic_cast<CharExprAST*>(expr)) {
        ConstValue cv;
        cv.type = ConstValue::CHAR;
        cv.charVal = chr->value;
        return cv;
    }

    if (auto* str = dynamic_cast<StringExprAST*>(expr)) {
        ConstValue cv;
        cv.type = ConstValue::STR;
        cv.strVal = str->value;
        return cv;
    }

    if (auto* unary = dynamic_cast<UnaryExprAST*>(expr)) {
        auto operand = eval(unary->operand.get(), at);
        if (!operand) {
            return std::nullopt;
        }
        if (operand->type == ConstValue::INT || operand->type == ConstValue::CHAR) {
            const bool isChar = operand->type == ConstValue::CHAR;
            const long long raw = isChar ? operand->charVal : operand->intVal;
            const unsigned opBits = isChar ? 8 : operand->bits;
            const bool opUnsigned = isChar ? false : operand->isUnsigned;
            switch (unary->op) {
                case UnaryOp::Plus:
                    return *operand;
                case UnaryOp::Minus:
                    return ctNormalizeInt(static_cast<uint64_t>(-static_cast<uint64_t>(raw)),
                                          opBits, opUnsigned);
                case UnaryOp::Not:
                    return ctInt(!ctTruthy(*operand) ? 1 : 0, 1, true, true);
                case UnaryOp::BitNot:
                    return ctNormalizeInt(~static_cast<uint64_t>(raw),
                                          opBits, opUnsigned);
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

    if (auto* bin = dynamic_cast<BinaryExprAST*>(expr)) {
        auto left = eval(bin->left.get(), at);
        if (!left) {
            return std::nullopt;
        }
        // Short-circuit && / || so a side-effectful or non-constant RHS is
        // only evaluated when the result depends on it.
        if (bin->op == BinaryOp::And || bin->op == BinaryOp::Or) {
            const bool truth = ctTruthy(*left);
            if ((bin->op == BinaryOp::And && !truth) ||
                (bin->op == BinaryOp::Or && truth)) {
                return ctInt(truth ? 1 : 0, 1, true, true);
            }
        }
        auto right = eval(bin->right.get(), at);
        if (!right) {
            return std::nullopt;
        }
        return evalBinary(*left, *right, static_cast<int>(bin->op), bin->type);
    }

    if (auto* ternary = dynamic_cast<TernaryExprAST*>(expr)) {
        auto cond = eval(ternary->cond.get(), at);
        if (!cond) {
            return std::nullopt;
        }
        return ctTruthy(*cond) ? eval(ternary->then.get(), at)
                               : eval(ternary->elseExpr.get(), at);
    }

    // constexpr 变量（DEC-05：共享 compileTimeValues）；constexpr 函数调用
    // 委托既有解释内核（evalConstexprCall/evalConstexprStmt，含 ConstEnv）。
    if (auto* var = dynamic_cast<VariableExprAST*>(expr)) {
        const auto& values = m_sema.getCompileTimeValues();
        auto it = values.find(var->name);
        if (it != values.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    if (auto* call = dynamic_cast<CallExprAST*>(expr)) {
        // P1-04 评审 I1: 实参由本求值器折叠（可能含 CT 根），再以已折叠
        // 实参解释 constexpr 函数体——避免与 evaluateConstexpr 的整树
        // CT 委托互递归。
        std::vector<ConstValue> argValues;
        for (auto& a : call->args) {
            auto v = eval(a.get(), at);
            if (!v) return std::nullopt;
            argValues.push_back(*v);
        }
        return m_sema.evalConstexprCallCT(*call, argValues);
    }

    // P1-04 / CT-04/05: compile_time 成员链值（target.os/arch/cpu、
    // build.debug/optimize/version）。未知成员诊断在此发出（spec §3 钉死）。
    if (auto* ma = dynamic_cast<MemberAccessExprAST*>(expr)) {
        if (m_sema.isCompileTimeRoot(ma->object.get())) {
            return evalCTMemberChain(*ma, at);
        }
        return std::nullopt;
    }

    // P1-04 / CT-01: compile_time 成员调用（static_assert/size_of 族/if）
    // —— CT-02（T5）接管 if/static_assert；此处实现布局查询（CT-06）。
    if (auto* mc = dynamic_cast<MethodCallExprAST*>(expr)) {
        if (m_sema.isCompileTimeRoot(mc->object.get())) {
            return evalQuery(*mc, at);
        }
        return std::nullopt;
    }

    if (auto* init = dynamic_cast<InitializerListExprAST*>(expr)) {
        Type* t = init->type;
        while (t && t->kind == TypeKind::Typedef)
            t = static_cast<TypedefType*>(t)->aliasedType;
        if (!t) return std::nullopt;
        auto agg = std::make_shared<CtAggregate>();
        agg->type = t;
        auto append = [&](Type* elemType, ExprAST* source) -> bool {
            if (source) {
                if (auto* nested = dynamic_cast<InitializerListExprAST*>(source)) {
                    nested->type = elemType;
                }
                auto v = eval(source, at);
                if (!v) return false;
                agg->elements.push_back(ctConvertValue(*v, elemType));
            } else {
                agg->elements.push_back(ctDefaultValue(elemType));
            }
            return true;
        };
        if (t->kind == TypeKind::Array) {
            auto* arr = static_cast<ArrayType*>(t);
            const int n = arr->size > 0 ? arr->size : 0;
            for (int i = 0; i < n; ++i) {
                ExprAST* source = static_cast<std::size_t>(i) < init->initializers.size()
                                      ? init->initializers[i].get()
                                      : nullptr;
                if (!append(arr->elementType, source)) return std::nullopt;
            }
            agg->totalElements = static_cast<std::size_t>(n);
        } else if (t->kind == TypeKind::Struct || t->kind == TypeKind::Class) {
            const std::vector<FieldInfo>* fields =
                t->kind == TypeKind::Struct
                    ? &static_cast<StructType*>(t)->fields
                    : &static_cast<ClassType*>(t)->fields;
            for (std::size_t i = 0; i < fields->size(); ++i) {
                ExprAST* source = i < init->initializers.size()
                                      ? init->initializers[i].get()
                                      : nullptr;
                if (!append((*fields)[i].type, source)) return std::nullopt;
            }
            agg->totalElements = fields->size();
        } else {
            return std::nullopt;
        }
        CtValue out;
        out.type = CtValue::AGGREGATE;
        out.aggregate = std::move(agg);
        return out;
    }

    return std::nullopt; // 普通非常量节点：静默失败（spec §2 钉死）
}

std::optional<ConstValue>
CompileTimeEvaluator::evalBinary(ConstValue& left, ConstValue& right, int op,
                                 ::Type* resultType) {
    return ctBinary(left, right, op, resultType);
}

std::optional<ConstValue>
CompileTimeEvaluator::evalCTMemberChain(MemberAccessExprAST& node, ASTNode& at) {
    // 根链成员名序列：compile_time.target.os → [target, os]。
    std::vector<std::string> members;
    {
        std::vector<MemberAccessExprAST*> chain;
        ExprAST* cur = &node;
        while (auto* ma = dynamic_cast<MemberAccessExprAST*>(cur)) {
            chain.push_back(ma);
            cur = ma->object.get();
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            members.push_back((*it)->memberName);
        }
    }

    auto unknown = [&](const std::string& name) {
        m_sema.emitError("unknown compile_time member '" + name + "'", at);
        return std::optional<ConstValue>{};
    };

    // CTX-01: source-location chains. These are compile-time-only string/int
    // values; runtime escape is diagnosed by sema.
    if (members.size() == 1) {
        const std::string& m = members[0];
        if (m == "file") {
            ConstValue cv;
            cv.type = ConstValue::STR;
            cv.strVal = at.sourceFile;
            return cv;
        }
        if (m == "line" || m == "column") {
            ConstValue cv;
            cv.type = ConstValue::INT;
            cv.intVal = m == "line" ? at.sourceLine : at.sourceColumn;
            return cv;
        }
        if (m == "function") {
            ConstValue cv;
            cv.type = ConstValue::STR;
            cv.strVal = m_sema.currentFunction ? m_sema.currentFunction->name : "";
            return cv;
        }
    }

    if (members[0] == "target") {
        if (members.size() == 1) return unknown("target"); // 裸命名空间
        if (members.size() > 2) return unknown(members[2]);
        ConstValue cv;
        const std::string& m = members[1];
        if (m == "pointer_width") {
            cv.type = ConstValue::INT;
            cv.intVal = static_cast<long long>(
                m_module->getDataLayout().getPointerSizeInBits());
            return cv;
        }
        cv.type = ConstValue::STR;
        if (m == "os") {
            cv.strVal = ctTargetOs();
        } else if (m == "arch") {
            cv.strVal = ctTargetArch();
        } else if (m == "cpu") {
            cv.strVal = llvm::sys::getHostCPUName().str();
        } else if (m == "endian") {
            cv.strVal = ctEndianName(m_module->getDataLayout());
        } else if (m == "triple") {
            cv.strVal = ctTargetTriple();
        } else {
            return unknown(m);
        }
        return cv;
    }

    if (members[0] == "build") {
        if (members.size() == 1) return unknown("build");
        if (members.size() > 2) return unknown(members[2]);
        const std::string& m = members[1];
        ConstValue cv;
        if (m == "debug") {
            cv.type = ConstValue::INT;
            cv.intVal = m_sema.m_ctBuildDebug ? 1 : 0;
            return cv;
        }
        if (m == "optimize") {
            cv.type = ConstValue::STR;
            cv.strVal = m_sema.m_ctBuildOptimize;
            return cv;
        }
        if (m == "version") {
            cv.type = ConstValue::STR;
            cv.strVal = "1.0.0";
            return cv;
        }
        if (m == "mode") {
            cv.type = ConstValue::STR;
            cv.strVal = m_sema.m_ctBuildDebug ? "debug" : "release";
            return cv;
        }
        if (m == "compiler") {
            cv.type = ConstValue::STR;
            cv.strVal = "cmm";
            return cv;
        }
        if (m == "host_os") {
            cv.type = ConstValue::STR;
            cv.strVal = ctTargetOs();
            return cv;
        }
        if (m == "host_arch") {
            cv.type = ConstValue::STR;
            cv.strVal = ctTargetArch();
            return cv;
        }
        return unknown(m);
    }

    if (members[0] == "version") {
        if (members.size() == 1) return unknown("version");
        if (members.size() > 2) return unknown(members[2]);
        ConstValue cv;
        cv.type = ConstValue::INT;
        if (members[1] == "major") cv.intVal = 1;
        else if (members[1] == "minor") cv.intVal = 0;
        else if (members[1] == "patch") cv.intVal = 0;
        else return unknown(members[1]);
        return cv;
    }

    return unknown(members[0]);
}

// P1-04 / CT-06: Type* → llvm::Type（布局规则与 CodegenContext::getLLVMType
// 一致——基类子对象占槽 0；union 以最大对齐成员 + 填充布局）。
llvm::Type* CompileTimeEvaluator::toLLVMType(Type* t) {
    if (!t) return nullptr;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    if (!t) return nullptr;
    auto cached = m_typeCache.find(t);
    if (cached != m_typeCache.end()) return cached->second;

    llvm::LLVMContext& ctx = *m_llvmCtx;
    llvm::Type* result = nullptr;
    switch (t->kind) {
        case TypeKind::Char:    result = llvm::Type::getInt8Ty(ctx); break;
        case TypeKind::Bool:    result = llvm::Type::getInt1Ty(ctx); break;
        case TypeKind::Int8:    result = llvm::Type::getInt8Ty(ctx); break;
        case TypeKind::Int16:   result = llvm::Type::getInt16Ty(ctx); break;
        case TypeKind::Int32:   result = llvm::Type::getInt32Ty(ctx); break;
        case TypeKind::Int64:   result = llvm::Type::getInt64Ty(ctx); break;
        case TypeKind::Int128:  result = llvm::Type::getInt128Ty(ctx); break;
        case TypeKind::UInt8:   result = llvm::Type::getInt8Ty(ctx); break;
        case TypeKind::UInt16:  result = llvm::Type::getInt16Ty(ctx); break;
        case TypeKind::UInt32:  result = llvm::Type::getInt32Ty(ctx); break;
        case TypeKind::UInt64:  result = llvm::Type::getInt64Ty(ctx); break;
        case TypeKind::UInt128: result = llvm::Type::getInt128Ty(ctx); break;
        case TypeKind::ISize:   result = llvm::Type::getInt64Ty(ctx); break;
        case TypeKind::USize:   result = llvm::Type::getInt64Ty(ctx); break;
        case TypeKind::Float32: result = llvm::Type::getFloatTy(ctx); break;
        case TypeKind::Float64: result = llvm::Type::getDoubleTy(ctx); break;
        case TypeKind::Float16: result = llvm::Type::getHalfTy(ctx); break;
        case TypeKind::Float128:result = llvm::Type::getFP128Ty(ctx); break;
        case TypeKind::Pointer: result = llvm::PointerType::get(ctx, 0); break;
        case TypeKind::Array: {
            // P1-05 / ANN-03: 数组字段（含 LayoutBuilder 的 i8 padding 伪字段）
            // ——缺失会使 padding 被 conv 跳过、布局自检/查询错位。
            auto* at = static_cast<ArrayType*>(t);
            if (llvm::Type* elem = toLLVMType(at->elementType)) {
                result = llvm::ArrayType::get(elem, at->size);
            }
            break;
        }
        case TypeKind::Enum: {
            auto* et = static_cast<EnumType*>(t);
            result = et->underlyingType ? toLLVMType(et->underlyingType)
                                        : llvm::Type::getInt32Ty(ctx);
            break;
        }
        case TypeKind::Struct:
        case TypeKind::Class: {
            // P1-05 / ANN: 布局单源化（LayoutBuilder；基类槽 0、packed/align）。
            // Class 不是 StructType 子类——name/isPacked 必须按 kind 分支读取，
            // 不能 static_cast<StructType*> 跨类（成员偏移不同，读垃圾值 UB）。
            std::string aggName;
            bool aggPacked = false;
            if (t->kind == TypeKind::Struct) {
                aggName = static_cast<StructType*>(t)->name;
                aggPacked = static_cast<StructType*>(t)->isPacked;
            } else {
                aggName = static_cast<ClassType*>(t)->name;
                aggPacked = static_cast<ClassType*>(t)->isPacked;
            }
            llvm::StructType* llvmSt = llvm::StructType::getTypeByName(ctx, aggName);
            if (!llvmSt) {
                llvmSt = llvm::StructType::create(ctx, aggName);
            }
            m_typeCache[t] = llvmSt;
            if (!llvmSt->isOpaque()) {
                result = llvmSt;
                break;
            }
            auto LR = LayoutBuilder::buildAggregate(t, ctx, m_module->getDataLayout(),
                [this](Type* tt) { return toLLVMType(tt); },
                [this](const std::string& n) -> Type* { return m_sema.resolveTypeByName(n); });
            llvmSt->setBody(LR.fieldTypes(), aggPacked);
            result = llvmSt;
            break;
        }
        case TypeKind::Union: {
            // P1-05 / ANN: 布局单源化（LayoutBuilder）。
            auto* ut = static_cast<UnionType*>(t);
            llvm::StructType* st = llvm::StructType::getTypeByName(ctx, ut->name);
            if (!st) {
                st = llvm::StructType::create(ctx, ut->name);
            }
            m_typeCache[t] = st;
            if (!st->isOpaque()) {
                result = st;
                break;
            }
            auto LR = LayoutBuilder::buildUnion(ut, ctx, m_module->getDataLayout(),
                [this](Type* tt) { return toLLVMType(tt); });
            st->setBody(LR.fieldTypes());
            result = st;
            break;
        }
        case TypeKind::Str: {
            // P1-06 (FMT-01): 布局与 CodegenContext::getLLVMType 一致——
            // {ptr, len}，size_of(str) = 16。
            auto* st = llvm::StructType::getTypeByName(ctx, "str");
            result = st ? st
                        : llvm::StructType::create(ctx,
                              {llvm::PointerType::get(ctx, 0),
                               llvm::Type::getInt64Ty(ctx)},
                              "str");
            break;
        }
        case TypeKind::String: {
            // P1-06 (FMT-02): {ptr, len, capacity}，size_of(string) = 24
            // （string 本身不参与 constexpr，但 size_of 布局查询合法）。
            auto* st = llvm::StructType::getTypeByName(ctx, "string");
            result = st ? st
                        : llvm::StructType::create(ctx,
                              {llvm::PointerType::get(ctx, 0),
                               llvm::Type::getInt64Ty(ctx),
                               llvm::Type::getInt64Ty(ctx)},
                              "string");
            break;
        }
        default:
            return nullptr; // Optional/Slice 等未支持布局——诊断路径
    }
    m_typeCache[t] = result;
    return result;
}

Type* CompileTimeEvaluator::stripTypedef(Type* t) {
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    return t;
}

Type* CompileTimeEvaluator::builtinTypeByName(const std::string& name) {
    auto& tc = TypeContext::instance();
    if (name == "void") return tc.getVoid();
    if (name == "bool") return tc.getBool();
    if (name == "char") return tc.getChar();
    if (name == "int8") return tc.getInt8();
    if (name == "int16") return tc.getInt16();
    if (name == "int32") return tc.getInt32();
    if (name == "int64") return tc.getInt64();
    if (name == "int128") return tc.getInt128();
    if (name == "uint8") return tc.getUInt8();
    if (name == "uint16") return tc.getUInt16();
    if (name == "uint32") return tc.getUInt32();
    if (name == "uint64") return tc.getUInt64();
    if (name == "uint128") return tc.getUInt128();
    if (name == "isize") return tc.getISize();
    if (name == "usize") return tc.getUSize();
    if (name == "float16") return tc.getFloat16();
    if (name == "float32") return tc.getFloat32();
    if (name == "float64") return tc.getFloat64();
    if (name == "float128") return tc.getFloat128();
    if (name == "string") return tc.getStringType();
    if (name == "string_view") return tc.getStrType();
    if (name == "FieldInfo") return tc.getFieldInfoType();
    if (name == "Attribute") return tc.getAttributeType();
    return nullptr;
}

Type* CompileTimeEvaluator::resolveTypeArg(ExprAST* arg, ASTNode& at,
                                           std::string* nameOut) {
    if (!arg) {
        m_sema.emitError("compile_time query requires a type argument", at);
        return nullptr;
    }
    Type* t = nullptr;
    std::string name;
    if (auto* sized = dynamic_cast<SizeofExprAST*>(arg)) {
        t = sized->sizeofType;
    } else if (auto* var = dynamic_cast<VariableExprAST*>(arg)) {
        name = var->name;
        t = builtinTypeByName(name);
        if (!t) t = m_sema.resolveTypeByName(name);
        // `size_of(value)` / `align_of(value)`: fall back to the expression's
        // type when the identifier is not a type name.
        if (!t) t = m_sema.getExprType(*arg);
    } else {
        // `size_of(array_of(int32, 4))`: evaluate a compile-time type value.
        auto v = eval(arg, at);
        if (v && v->type == ConstValue::TYPE) {
            t = v->typeVal;
        } else {
            // `size_of(a + b)` / `type_name_of(expr)`.
            t = m_sema.getExprType(*arg);
        }
    }
    if (nameOut) *nameOut = name;
    if (!t) {
        m_sema.emitError("unknown type '" + (name.empty() ? "(unnamed)" : name) +
                             "' in compile_time expression",
                         at);
    }
    return t;
}

bool CompileTimeEvaluator::isAggregate(Type* t) const {
    t = const_cast<CompileTimeEvaluator*>(this)->stripTypedef(t);
    return t && (t->kind == TypeKind::Struct || t->kind == TypeKind::Class ||
                 t->kind == TypeKind::Union);
}

bool CompileTimeEvaluator::isCompleteAggregate(Type* t) const {
    t = const_cast<CompileTimeEvaluator*>(this)->stripTypedef(t);
    if (!t) return false;
    if (t->kind == TypeKind::Struct) return static_cast<StructType*>(t)->isComplete;
    if (t->kind == TypeKind::Class) return static_cast<ClassType*>(t)->isComplete;
    if (t->kind == TypeKind::Union) return static_cast<UnionType*>(t)->isComplete;
    return true;
}

bool CompileTimeEvaluator::typesSame(Type* a, Type* b) {
    a = stripTypedef(a);
    b = stripTypedef(b);
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind == TypeKind::TypeInstance && b->kind == TypeKind::TypeInstance) {
        auto* ai = static_cast<TypeInstanceType*>(a);
        auto* bi = static_cast<TypeInstanceType*>(b);
        if (ai->templateName != bi->templateName) return false;
        if (ai->typeArgs.size() != bi->typeArgs.size()) return false;
        for (size_t i = 0; i < ai->typeArgs.size(); ++i)
            if (!typesSame(ai->typeArgs[i], bi->typeArgs[i])) return false;
        if (ai->valueArgs != bi->valueArgs) return false;
        return true;
    }
    if (a->kind != b->kind) return false;
    return typesEqual(a, b);
}

bool CompileTimeEvaluator::isIntegerKind(Type* t) const {
    t = const_cast<CompileTimeEvaluator*>(this)->stripTypedef(t);
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::Char:
        case TypeKind::Bool:
        case TypeKind::Int8:
        case TypeKind::Int16:
        case TypeKind::Int32:
        case TypeKind::Int64:
        case TypeKind::Int128:
        case TypeKind::UInt8:
        case TypeKind::UInt16:
        case TypeKind::UInt32:
        case TypeKind::UInt64:
        case TypeKind::UInt128:
        case TypeKind::ISize:
        case TypeKind::USize:
        case TypeKind::Enum:
            return true;
        default:
            return false;
    }
}

bool CompileTimeEvaluator::isFloatKind(Type* t) const {
    t = const_cast<CompileTimeEvaluator*>(this)->stripTypedef(t);
    if (!t) return false;
    return t->kind == TypeKind::Float16 || t->kind == TypeKind::Float32 ||
           t->kind == TypeKind::Float64 || t->kind == TypeKind::Float128;
}

int CompileTimeEvaluator::integerBitWidth(Type* t) const {
    t = const_cast<CompileTimeEvaluator*>(this)->stripTypedef(t);
    if (!t) return 0;
    switch (t->kind) {
        case TypeKind::Bool: return 8; // storage width; value range is 0..1
        case TypeKind::Char:
        case TypeKind::Int8:
        case TypeKind::UInt8: return 8;
        case TypeKind::Int16:
        case TypeKind::UInt16: return 16;
        case TypeKind::Int32:
        case TypeKind::UInt32: return 32;
        case TypeKind::Int64:
        case TypeKind::UInt64:
        case TypeKind::ISize:
        case TypeKind::USize: return 64;
        case TypeKind::Int128:
        case TypeKind::UInt128: return 128;
        case TypeKind::Enum: {
            auto* et = static_cast<EnumType*>(t);
            return et->underlyingType ? integerBitWidth(et->underlyingType) : 32;
        }
        default: return 0;
    }
}

const LayoutResult* CompileTimeEvaluator::layoutFor(Type* t) {
    t = stripTypedef(t);
    if (!t || !isAggregate(t)) return nullptr;
    // Never cache a layout for an incomplete aggregate: sema may complete the
    // same Type* later in the TU, and a cached zero/partial layout would poison
    // every subsequent query.
    if (!isCompleteAggregate(t)) return nullptr;
    auto& cache = t->kind == TypeKind::Union ? m_unionLayoutCache : m_layoutCache;
    auto it = cache.find(t);
    if (it != cache.end()) return &it->second;
    LayoutResult lr;
    if (t->kind == TypeKind::Union) {
        lr = LayoutBuilder::buildUnion(static_cast<UnionType*>(t), *m_llvmCtx,
                                       m_module->getDataLayout(),
                                       [this](Type* tt) { return toLLVMType(tt); });
    } else {
        lr = LayoutBuilder::buildAggregate(t, *m_llvmCtx, m_module->getDataLayout(),
                                           [this](Type* tt) { return toLLVMType(tt); },
                                           [this](const std::string& n) -> Type* {
                                               return m_sema.resolveTypeByName(n);
                                           });
    }
    auto inserted = cache.emplace(t, std::move(lr));
    return &inserted.first->second;
}

std::vector<FieldInfo> CompileTimeEvaluator::collectFields(Type* t, bool includeBase) {
    std::vector<FieldInfo> out;
    t = stripTypedef(t);
    if (!t) return out;
    if (t->kind == TypeKind::Union) {
        for (auto& m : static_cast<UnionType*>(t)->members)
            if (!m.name.empty()) out.push_back(m);
        return out;
    }
    if (t->kind != TypeKind::Struct && t->kind != TypeKind::Class) return out;
    if (includeBase) {
        Type* base = t->base;
        std::string baseName;
        if (t->kind == TypeKind::Struct) baseName = static_cast<StructType*>(t)->baseClass;
        else baseName = static_cast<ClassType*>(t)->baseClass;
        if (!base && !baseName.empty()) base = m_sema.resolveTypeByName(baseName);
        if (base) {
            auto baseFields = collectFields(base, true);
            out.insert(out.end(), baseFields.begin(), baseFields.end());
        }
    }
    if (t->kind == TypeKind::Struct) {
        auto* st = static_cast<StructType*>(t);
        const auto& src = st->semanticFields.empty() ? st->fields : st->semanticFields;
        for (auto& f : src)
            if (!f.name.empty()) out.push_back(f);
    } else {
        auto* ct = static_cast<ClassType*>(t);
        const auto& src = ct->semanticFields.empty() ? ct->fields : ct->semanticFields;
        for (auto& f : src)
            if (!f.name.empty()) out.push_back(f);
    }
    return out;
}

std::optional<FieldInfo> CompileTimeEvaluator::findField(Type* t,
                                                         const std::string& name,
                                                         bool includeBase) {
    auto fields = collectFields(t, includeBase);
    for (auto& f : fields)
        if (f.name == name) return f;
    return std::nullopt;
}

std::optional<uint64_t> CompileTimeEvaluator::fieldOffsetOf(Type* t,
                                                           const std::string& name) {
    t = stripTypedef(t);
    if (!t) return std::nullopt;
    if (t->kind == TypeKind::Union) {
        auto* ut = static_cast<UnionType*>(t);
        for (auto& m : ut->members)
            if (m.name == name) return 0;
        return std::nullopt;
    }
    if (t->kind != TypeKind::Struct && t->kind != TypeKind::Class) return std::nullopt;
    auto* lr = layoutFor(t);
    if (lr) {
        auto it = lr->fieldOffsets.find(name);
        if (it != lr->fieldOffsets.end()) return it->second;
    }
    Type* base = t->base;
    std::string baseName;
    if (t->kind == TypeKind::Struct) baseName = static_cast<StructType*>(t)->baseClass;
    else baseName = static_cast<ClassType*>(t)->baseClass;
    if (!base && !baseName.empty()) base = m_sema.resolveTypeByName(baseName);
    if (base) return fieldOffsetOf(base, name);
    return std::nullopt;
}

const BitFieldLayout* CompileTimeEvaluator::bitFieldOf(Type* t,
                                                       const std::string& name) {
    t = stripTypedef(t);
    if (!t) return nullptr;
    if (t->kind == TypeKind::Struct) {
        auto* st = static_cast<StructType*>(t);
        auto it = st->bitFields.find(name);
        if (it != st->bitFields.end()) return &it->second;
    } else if (t->kind == TypeKind::Class) {
        auto* ct = static_cast<ClassType*>(t);
        auto it = ct->bitFields.find(name);
        if (it != ct->bitFields.end()) return &it->second;
    } else if (t->kind == TypeKind::Union) {
        auto* ut = static_cast<UnionType*>(t);
        auto it = ut->bitFields.find(name);
        if (it != ut->bitFields.end()) return &it->second;
    }
    Type* base = t->base;
    std::string baseName;
    if (t->kind == TypeKind::Struct) baseName = static_cast<StructType*>(t)->baseClass;
    else if (t->kind == TypeKind::Class) baseName = static_cast<ClassType*>(t)->baseClass;
    if (!base && !baseName.empty()) base = m_sema.resolveTypeByName(baseName);
    return base ? bitFieldOf(base, name) : nullptr;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalAnnotationArg(const Annotation& ann, size_t index,
                                        ASTNode& at) {
    if (index >= ann.args.size()) {
        m_sema.emitError("annotation '" + ann.name + "' has no argument " +
                             std::to_string(index),
                         at);
        return std::nullopt;
    }
    const auto& arg = ann.args[index];
    switch (arg.kind) {
        case AnnotationArg::Kind::Expr: {
            if (!arg.expr) return std::nullopt;
            return eval(arg.expr.get(), at);
        }
        case AnnotationArg::Kind::Type: {
            ConstValue cv;
            cv.type = ConstValue::STR;
            cv.strVal = m_sema.typeToString(arg.type);
            return cv;
        }
        case AnnotationArg::Kind::Ident:
        case AnnotationArg::Kind::String: {
            ConstValue cv;
            cv.type = ConstValue::STR;
            cv.strVal = arg.text;
            return cv;
        }
    }
    return std::nullopt;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalTypeQuery(MethodCallExprAST& node, ASTNode& at) {
    Type* t = nullptr;
    if (!node.args.empty()) t = resolveTypeArg(node.args[0].get(), at);
    if (!t) return std::nullopt;
    Type* raw = stripTypedef(t);

    auto intResult = [](long long v, bool isUnsigned = false) {
        ConstValue cv;
        cv.type = ConstValue::INT;
        cv.intVal = v;
        cv.isUnsigned = isUnsigned;
        return cv;
    };

    const std::string& m = node.methodName;
    if (m == "is_same") {
        Type* other = node.args.size() > 1 ? resolveTypeArg(node.args[1].get(), at) : nullptr;
        if (!other) {
            m_sema.emitError("compile_time.is_same requires two type arguments", at);
            return std::nullopt;
        }
        return intResult(typesSame(t, other) ? 1 : 0);
    }
    if (m == "is_integer") return intResult(isIntegerKind(raw) ? 1 : 0);
    if (m == "is_float") return intResult(isFloatKind(raw) ? 1 : 0);
    if (m == "is_bool") return intResult(raw && raw->kind == TypeKind::Bool ? 1 : 0);
    if (m == "is_pointer") return intResult(raw && raw->kind == TypeKind::Pointer ? 1 : 0);
    if (m == "is_array") return intResult(raw && raw->kind == TypeKind::Array ? 1 : 0);
    if (m == "is_aggregate") return intResult(isAggregate(raw) ? 1 : 0);
    if (m == "is_class") return intResult(raw && raw->kind == TypeKind::Class ? 1 : 0);
    if (m == "is_struct") return intResult(raw && raw->kind == TypeKind::Struct ? 1 : 0);
    if (m == "is_union") return intResult(raw && raw->kind == TypeKind::Union ? 1 : 0);
    if (m == "is_enum") return intResult(raw && raw->kind == TypeKind::Enum ? 1 : 0);
    if (m == "is_void") return intResult(raw && raw->kind == TypeKind::Void ? 1 : 0);
    if (m == "is_signed") {
        if (isIntegerKind(raw)) {
            if (raw->kind == TypeKind::Enum) {
                auto* et = static_cast<EnumType*>(raw);
                Type* underlying = et->underlyingType ? et->underlyingType
                                                     : TypeContext::instance().getInt32();
                return intResult(isUnsignedIntegerType(underlying) ? 0 : 1);
            }
            return intResult(isUnsignedIntegerType(raw) ? 0 : 1);
        }
        if (isFloatKind(raw)) return intResult(1);
        return intResult(0);
    }

    if (m == "bit_width") {
        if (isIntegerKind(raw)) return intResult(integerBitWidth(raw));
        if (isFloatKind(raw)) {
            switch (raw->kind) {
                case TypeKind::Float16: return intResult(16);
                case TypeKind::Float32: return intResult(32);
                case TypeKind::Float64: return intResult(64);
                case TypeKind::Float128: return intResult(128);
                default: break;
            }
        }
        m_sema.emitError("compile_time.bit_width requires an integer or float type", at);
        return std::nullopt;
    }

    if (m == "int_min" || m == "int_max" || m == "uint_max") {
        if (!isIntegerKind(raw)) {
            m_sema.emitError("compile_time." + m + " requires an integer type", at);
            return std::nullopt;
        }
        const bool isBool = raw->kind == TypeKind::Bool;
        const bool isUnsigned = isBool || isUnsignedIntegerType(raw);
        const int bits = integerBitWidth(raw);
        if (bits > 64) {
            m_sema.emitError("128-bit numeric limits are not representable in a "
                             "compile-time integer constant",
                             at);
            return std::nullopt;
        }
        if (m == "uint_max" && !isUnsigned) {
            m_sema.emitError("compile_time.uint_max requires an unsigned integer type", at);
            return std::nullopt;
        }
        if (isBool) {
            if (m == "int_min") return intResult(0);
            return intResult(1, true);
        }
        if (m == "int_min") {
            if (isUnsigned) return intResult(0, true);
            if (bits == 64) return intResult(std::numeric_limits<long long>::min());
            return intResult(-(1LL << (bits - 1)));
        }
        if (isUnsigned) {
            if (bits == 64) return intResult(-1, true); // 0xFFFF...FFFF
            return intResult(static_cast<long long>((1ULL << bits) - 1), true);
        }
        if (bits == 64) return intResult(std::numeric_limits<long long>::max());
        return intResult((1LL << (bits - 1)) - 1);
    }

    if (m == "float_epsilon" || m == "float_max" || m == "float_digits") {
        if (!isFloatKind(raw)) {
            m_sema.emitError("compile_time." + m + " requires a float type", at);
            return std::nullopt;
        }
        ConstValue cv;
        if (m == "float_digits") {
            cv.type = ConstValue::INT;
            switch (raw->kind) {
                case TypeKind::Float16: cv.intVal = 11; break;
                case TypeKind::Float32: cv.intVal = 24; break;
                case TypeKind::Float64: cv.intVal = 53; break;
                case TypeKind::Float128: cv.intVal = 113; break;
                default: cv.intVal = 0; break;
            }
            return cv;
        }
        cv.type = ConstValue::DOUBLE;
        switch (raw->kind) {
            case TypeKind::Float16:
                cv.doubleVal = m == "float_epsilon" ? 0.0009765625 : 65504.0;
                break;
            case TypeKind::Float32:
                cv.doubleVal = m == "float_epsilon"
                                   ? static_cast<double>(std::numeric_limits<float>::epsilon())
                                   : static_cast<double>(std::numeric_limits<float>::max());
                break;
            case TypeKind::Float64:
                cv.doubleVal = m == "float_epsilon"
                                   ? std::numeric_limits<double>::epsilon()
                                   : std::numeric_limits<double>::max();
                break;
            case TypeKind::Float128:
                cv.doubleVal = m == "float_epsilon"
                                   ? static_cast<double>(std::numeric_limits<long double>::epsilon())
                                   : static_cast<double>(std::numeric_limits<long double>::max());
                break;
            default:
                return std::nullopt;
        }
        return cv;
    }

    if (m == "is_convertible" || m == "is_assignable") {
        Type* from = node.args.size() > 0 ? resolveTypeArg(node.args[0].get(), at) : nullptr;
        Type* to = node.args.size() > 1 ? resolveTypeArg(node.args[1].get(), at) : nullptr;
        if (!from || !to) {
            m_sema.emitError("compile_time." + m + " requires two type arguments", at);
            return std::nullopt;
        }
        if (typesSame(from, to)) return intResult(1);
        Type* fromRaw = stripTypedef(from);
        Type* toRaw = stripTypedef(to);
        if (fromRaw->kind == TypeKind::TypeInstance ||
            toRaw->kind == TypeKind::TypeInstance)
            return intResult(0);
        return intResult(conversionRank(from, to) >= 0 ? 1 : 0);
    }
    if (m == "template_name_of" || m == "template_arg_count" ||
        m == "template_arg_name") {
        if (!raw || raw->kind != TypeKind::TypeInstance) {
            m_sema.emitError("compile_time." + m + " requires a template instance type", at);
            return std::nullopt;
        }
        auto* inst = static_cast<TypeInstanceType*>(raw);
        if (m == "template_name_of") {
            ConstValue cv;
            cv.type = ConstValue::STR;
            cv.strVal = inst->templateName;
            return cv;
        }
        if (m == "template_arg_count") {
            return intResult(static_cast<long long>(inst->typeArgs.size() +
                                                    inst->valueArgs.size()));
        }
        auto idx = node.args.size() > 1 ? eval(node.args[1].get(), at) : std::nullopt;
        if (!idx || idx->type != ConstValue::INT || idx->intVal < 0) {
            m_sema.emitError("compile_time.template_arg_name requires an index", at);
            return std::nullopt;
        }
        const size_t i = static_cast<size_t>(idx->intVal);
        ConstValue cv;
        cv.type = ConstValue::STR;
        if (i < inst->typeArgs.size()) {
            cv.strVal = m_sema.typeToString(inst->typeArgs[i]);
        } else if (i - inst->typeArgs.size() < inst->valueArgNames.size()) {
            cv.strVal = inst->valueArgNames[i - inst->typeArgs.size()];
        } else if (i - inst->typeArgs.size() < inst->valueArgs.size()) {
            cv.strVal = std::to_string(inst->valueArgs[i - inst->typeArgs.size()]);
        } else {
            m_sema.emitError("compile_time.template_arg_name index out of range", at);
            return std::nullopt;
        }
        return cv;
    }

    if (m == "type_name_of") {
        ConstValue cv;
        cv.type = ConstValue::STR;
        cv.strVal = m_sema.typeToString(t);
        return cv;
    }
    if (m == "type_kind_of") {
        ConstValue cv;
        cv.type = ConstValue::STR;
        switch (raw ? raw->kind : TypeKind::Void) {
            case TypeKind::Void: cv.strVal = "void"; break;
            case TypeKind::Bool: cv.strVal = "bool"; break;
            case TypeKind::Char: cv.strVal = "char"; break;
            case TypeKind::Int8:
            case TypeKind::Int16:
            case TypeKind::Int32:
            case TypeKind::Int64:
            case TypeKind::Int128:
            case TypeKind::UInt8:
            case TypeKind::UInt16:
            case TypeKind::UInt32:
            case TypeKind::UInt64:
            case TypeKind::UInt128:
            case TypeKind::ISize:
            case TypeKind::USize:
                cv.strVal = "integer";
                break;
            case TypeKind::Float16:
            case TypeKind::Float32:
            case TypeKind::Float64:
            case TypeKind::Float128:
                cv.strVal = "float";
                break;
            case TypeKind::Pointer: cv.strVal = "pointer"; break;
            case TypeKind::Array: cv.strVal = "array"; break;
            case TypeKind::Struct: cv.strVal = "struct"; break;
            case TypeKind::Class: cv.strVal = "class"; break;
            case TypeKind::Union: cv.strVal = "union"; break;
            case TypeKind::Enum: cv.strVal = "enum"; break;
            case TypeKind::Function: cv.strVal = "function"; break;
            case TypeKind::Str: cv.strVal = "string_view"; break;
            case TypeKind::String: cv.strVal = "string"; break;
            case TypeKind::Slice: cv.strVal = "slice"; break;
            case TypeKind::Optional: cv.strVal = "optional"; break;
            case TypeKind::Result: cv.strVal = "result"; break;
            default: cv.strVal = "unknown"; break;
        }
        return cv;
    }
    if (m == "layout_size_of" || m == "layout_align_of") {
        auto* lr = layoutFor(raw);
        if (lr) return intResult(static_cast<long long>(
            m == "layout_size_of" ? lr->size : lr->align));
        llvm::Type* lt = toLLVMType(raw);
        if (!lt) {
            m_sema.emitError("unknown type in compile_time." + m + " expression", at);
            return std::nullopt;
        }
        return intResult(static_cast<long long>(
            m == "layout_size_of"
                ? m_module->getDataLayout().getTypeAllocSize(lt)
                : m_module->getDataLayout().getABITypeAlign(lt).value()));
    }

    m_sema.emitError("unknown compile_time type query '" + m + "'", at);
    return std::nullopt;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalFieldQuery(MethodCallExprAST& node, ASTNode& at) {
    Type* t = node.args.empty() ? nullptr : resolveTypeArg(node.args[0].get(), at);
    if (!t) return std::nullopt;
    Type* raw = stripTypedef(t);
    if (!isAggregate(raw)) {
        m_sema.emitError("compile_time." + node.methodName +
                             " requires an aggregate type",
                         at);
        return std::nullopt;
    }
    if (!isCompleteAggregate(raw)) {
        m_sema.emitError("incomplete type '" + m_sema.typeToString(t) +
                             "' in compile_time." + node.methodName,
                         at);
        return std::nullopt;
    }
    // Ensure LayoutBuilder has materialized bitfield metadata before any
    // field_is_bitfield/field_offset_of query reads it.
    (void)layoutFor(raw);

    auto intResult = [](long long v) {
        ConstValue cv;
        cv.type = ConstValue::INT;
        cv.intVal = v;
        return cv;
    };
    auto strResult = [](std::string s) {
        ConstValue cv;
        cv.type = ConstValue::STR;
        cv.strVal = std::move(s);
        return cv;
    };
    auto stringArg = [&](size_t idx) -> std::optional<std::string> {
        if (idx >= node.args.size()) return std::nullopt;
        if (auto* s = dynamic_cast<StringExprAST*>(node.args[idx].get())) return s->value;
        if (auto* v = dynamic_cast<VariableExprAST*>(node.args[idx].get())) return v->name;
        return std::nullopt;
    };
    auto intArg = [&](size_t idx) -> std::optional<long long> {
        if (idx >= node.args.size()) return std::nullopt;
        auto v = eval(node.args[idx].get(), at);
        if (!v || v->type != ConstValue::INT) return std::nullopt;
        return v->intVal;
    };

    const std::string& m = node.methodName;
    auto fields = collectFields(raw, true);

    if (m == "field_count_of") return intResult(static_cast<long long>(fields.size()));
    if (m == "field_name_at") {
        auto idx = intArg(1);
        if (!idx || *idx < 0 || static_cast<size_t>(*idx) >= fields.size()) {
            m_sema.emitError("compile_time.field_name_at index out of range", at);
            return std::nullopt;
        }
        return strResult(fields[static_cast<size_t>(*idx)].name);
    }

    auto nameOpt = stringArg(1);
    if (!nameOpt) {
        m_sema.emitError("compile_time." + m + " requires a field name", at);
        return std::nullopt;
    }
    const std::string& fieldName = *nameOpt;
    auto fieldOpt = findField(raw, fieldName, true);
    const FieldInfo* field = fieldOpt ? &*fieldOpt : nullptr;
    if (m == "has_field") return intResult(field ? 1 : 0);
    if (!field) {
        m_sema.emitError("unknown field '" + fieldName + "' in compile_time." + m,
                         at);
        return std::nullopt;
    }

    if (m == "field_type_name_of") return strResult(m_sema.typeToString(field->type));
    if (m == "field_size_of") {
        llvm::Type* lt = toLLVMType(field->type);
        if (!lt) {
            m_sema.emitError("cannot compute size of field '" + fieldName + "'", at);
            return std::nullopt;
        }
        return intResult(static_cast<long long>(
            m_module->getDataLayout().getTypeAllocSize(lt)));
    }
    if (m == "field_offset_of") {
        const BitFieldLayout* bf = bitFieldOf(raw, fieldName);
        if (bf) {
            m_sema.emitError("cannot take offset of a bit-field", at);
            return std::nullopt;
        }
        auto off = fieldOffsetOf(raw, fieldName);
        if (!off) {
            m_sema.emitError("unknown field '" + fieldName + "' in compile_time.field_offset_of",
                             at);
            return std::nullopt;
        }
        return intResult(static_cast<long long>(*off));
    }
    if (m == "field_is_bitfield") return intResult(bitFieldOf(raw, fieldName) ? 1 : 0);
    if (m == "field_bit_width_of") {
        const BitFieldLayout* bf = bitFieldOf(raw, fieldName);
        return intResult(bf ? static_cast<long long>(bf->bitWidth) : 0);
    }

    if (m == "field_annotation_count_of")
        return intResult(static_cast<long long>(field->annotations.size()));
    if (m == "field_annotation_name_at") {
        auto idx = intArg(2);
        if (!idx || *idx < 0 || static_cast<size_t>(*idx) >= field->annotations.size()) {
            m_sema.emitError("compile_time.field_annotation_name_at index out of range", at);
            return std::nullopt;
        }
        return strResult(field->annotations[static_cast<size_t>(*idx)].name);
    }
    if (m == "field_annotation_arg_of") {
        auto annName = stringArg(2);
        auto idx = intArg(3);
        if (!annName || !idx || *idx < 0) {
            m_sema.emitError("compile_time.field_annotation_arg_of requires "
                             "(type, field, annotation, index)",
                             at);
            return std::nullopt;
        }
        for (const auto& ann : field->annotations) {
            if (ann.name == *annName) return evalAnnotationArg(ann, static_cast<size_t>(*idx), at);
        }
        m_sema.emitError("unknown annotation '" + *annName + "' on field '" +
                             fieldName + "'",
                         at);
        return std::nullopt;
    }

    m_sema.emitError("unknown compile_time field query '" + m + "'", at);
    return std::nullopt;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalTypeValueQuery(MethodCallExprAST& node, ASTNode& at) {
    Type* t = node.args.empty() ? nullptr : resolveTypeArg(node.args[0].get(), at);
    if (!t) return std::nullopt;
    ConstValue cv;
    cv.type = ConstValue::TYPE;
    if (node.methodName == "type_of") {
        cv.typeVal = t;
        return cv;
    }
    if (node.methodName == "pointer_to") {
        cv.typeVal = new Type(TypeKind::Pointer, t);
        return cv;
    }
    if (node.methodName == "array_of") {
        if (node.args.size() != 2) {
            m_sema.emitError("compile_time.array_of requires (type, count)", at);
            return std::nullopt;
        }
        auto n = eval(node.args[1].get(), at);
        if (!n || n->type != ConstValue::INT || n->intVal < 0 ||
            n->intVal > std::numeric_limits<int>::max()) {
            m_sema.emitError("compile_time.array_of count must be a non-negative integer", at);
            return std::nullopt;
        }
        cv.typeVal = new ArrayType(t, static_cast<int>(n->intVal));
        return cv;
    }
    if (node.methodName == "const_of") {
        Type* raw = stripTypedef(t);
        // `const` is not observable by the current type predicates; for
        // aggregates/arrays/enums return the original type instead of
        // constructing a bare `Type(kind)` that every consumer would cast to
        // the wrong subclass (UB).
        switch (raw->kind) {
            case TypeKind::Char:
            case TypeKind::Bool:
            case TypeKind::Int8:
            case TypeKind::Int16:
            case TypeKind::Int32:
            case TypeKind::Int64:
            case TypeKind::Int128:
            case TypeKind::UInt8:
            case TypeKind::UInt16:
            case TypeKind::UInt32:
            case TypeKind::UInt64:
            case TypeKind::UInt128:
            case TypeKind::ISize:
            case TypeKind::USize:
            case TypeKind::Float16:
            case TypeKind::Float32:
            case TypeKind::Float64:
            case TypeKind::Float128: {
                auto* c = new Type(raw->kind);
                c->isConst = true;
                cv.typeVal = c;
                return cv;
            }
            case TypeKind::Pointer: {
                auto* p = new Type(TypeKind::Pointer, raw->base);
                p->isConst = true;
                cv.typeVal = p;
                return cv;
            }
            default:
                cv.typeVal = t;
                return cv;
        }
    }
    m_sema.emitError("unknown compile_time type-value query '" + node.methodName + "'", at);
    return std::nullopt;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalEnumQuery(MethodCallExprAST& node, ASTNode& at) {
    Type* t = node.args.empty() ? nullptr : resolveTypeArg(node.args[0].get(), at);
    if (!t) return std::nullopt;
    Type* raw = stripTypedef(t);
    if (!raw || raw->kind != TypeKind::Enum) {
        m_sema.emitError("compile_time." + node.methodName + " requires an enum type", at);
        return std::nullopt;
    }
    auto* et = static_cast<EnumType*>(raw);
    auto intResult = [](long long v) {
        ConstValue cv;
        cv.type = ConstValue::INT;
        cv.intVal = v;
        return cv;
    };
    auto strResult = [](std::string s) {
        ConstValue cv;
        cv.type = ConstValue::STR;
        cv.strVal = std::move(s);
        return cv;
    };
    auto intArg = [&](size_t idx) -> std::optional<long long> {
        if (idx >= node.args.size()) return std::nullopt;
        auto v = eval(node.args[idx].get(), at);
        if (!v || v->type != ConstValue::INT) return std::nullopt;
        return v->intVal;
    };
    auto strArg = [&](size_t idx) -> std::optional<std::string> {
        if (idx >= node.args.size()) return std::nullopt;
        auto* s = dynamic_cast<StringExprAST*>(node.args[idx].get());
        return s ? std::optional<std::string>(s->value) : std::nullopt;
    };

    const std::string& m = node.methodName;
    if (m == "enum_count_of") return intResult(static_cast<long long>(et->values.size()));
    if (m == "enum_name_at") {
        auto idx = intArg(1);
        if (!idx || *idx < 0 || static_cast<size_t>(*idx) >= et->values.size()) {
            m_sema.emitError("compile_time.enum_name_at index out of range", at);
            return std::nullopt;
        }
        return strResult(et->values[static_cast<size_t>(*idx)].first);
    }
    if (m == "enum_value_at") {
        auto idx = intArg(1);
        if (!idx || *idx < 0 || static_cast<size_t>(*idx) >= et->values.size()) {
            m_sema.emitError("compile_time.enum_value_at index out of range", at);
            return std::nullopt;
        }
        return intResult(et->values[static_cast<size_t>(*idx)].second);
    }
    if (m == "enum_value_of") {
        auto name = strArg(1);
        if (!name) {
            m_sema.emitError("compile_time.enum_value_of requires a name", at);
            return std::nullopt;
        }
        for (auto& v : et->values)
            if (v.first == *name) return intResult(v.second);
        m_sema.emitError("unknown enum value '" + *name + "' in '" + et->name + "'", at);
        return std::nullopt;
    }
    if (m == "enum_name_of") {
        auto value = intArg(1);
        if (!value) {
            m_sema.emitError("compile_time.enum_name_of requires an integer value", at);
            return std::nullopt;
        }
        for (auto& v : et->values)
            if (v.second == *value) return strResult(v.first);
        m_sema.emitError("no enumerator with value " + std::to_string(*value) +
                             " in '" + et->name + "'",
                         at);
        return std::nullopt;
    }
    if (m == "enum_underlying_name") {
        Type* underlying = et->underlyingType ? et->underlyingType
                                               : TypeContext::instance().getInt32();
        return strResult(m_sema.typeToString(underlying));
    }
    m_sema.emitError("unknown compile_time enum query '" + m + "'", at);
    return std::nullopt;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalClassQuery(MethodCallExprAST& node, ASTNode& at) {
    Type* t = node.args.empty() ? nullptr : resolveTypeArg(node.args[0].get(), at);
    if (!t) return std::nullopt;
    Type* raw = stripTypedef(t);
    auto intResult = [](long long v) {
        ConstValue cv;
        cv.type = ConstValue::INT;
        cv.intVal = v;
        return cv;
    };
    auto strResult = [](std::string s) {
        ConstValue cv;
        cv.type = ConstValue::STR;
        cv.strVal = std::move(s);
        return cv;
    };
    auto strArg = [&](size_t idx) -> std::optional<std::string> {
        if (idx >= node.args.size()) return std::nullopt;
        if (auto* s = dynamic_cast<StringExprAST*>(node.args[idx].get())) return s->value;
        if (auto* v = dynamic_cast<VariableExprAST*>(node.args[idx].get())) return v->name;
        return std::nullopt;
    };
    auto intArg = [&](size_t idx) -> std::optional<long long> {
        if (idx >= node.args.size()) return std::nullopt;
        auto v = eval(node.args[idx].get(), at);
        if (!v || v->type != ConstValue::INT) return std::nullopt;
        return v->intVal;
    };

    const std::string& m = node.methodName;
    if (m == "is_base_of") {
        Type* base = node.args.size() > 0 ? resolveTypeArg(node.args[0].get(), at) : nullptr;
        Type* derived = node.args.size() > 1 ? resolveTypeArg(node.args[1].get(), at) : nullptr;
        if (!base || !derived) {
            m_sema.emitError("compile_time.is_base_of requires two type arguments", at);
            return std::nullopt;
        }
        return intResult(m_sema.isDerivedFrom(derived, base) ? 1 : 0);
    }

    if (!raw || (raw->kind != TypeKind::Class && raw->kind != TypeKind::Struct)) {
        m_sema.emitError("compile_time." + m + " requires a class or struct type", at);
        return std::nullopt;
    }

    if (m == "base_name_of") {
        std::string baseName;
        if (raw->kind == TypeKind::Class) baseName = static_cast<ClassType*>(raw)->baseClass;
        else baseName = static_cast<StructType*>(raw)->baseClass;
        return strResult(baseName);
    }

    // Methods: base chain first, then the most-derived class. `ClassType` is
    // the only type that stores methods (structs with methods are routed to
    // ClassType by sema).
    std::vector<std::pair<std::string, FunctionType*>> methods;
    std::function<void(Type*)> collect = [&](Type* cur) {
        cur = stripTypedef(cur);
        if (!cur || cur->kind != TypeKind::Class) return;
        auto* ct = static_cast<ClassType*>(cur);
        Type* base = ct->base;
        if (!base && !ct->baseClass.empty()) base = m_sema.resolveTypeByName(ct->baseClass);
        if (base) collect(base);
        for (auto& meth : ct->methods) methods.push_back(meth);
    };
    collect(raw);

    if (m == "method_count_of") return intResult(static_cast<long long>(methods.size()));
    if (m == "method_name_at") {
        auto idx = intArg(1);
        if (!idx || *idx < 0 || static_cast<size_t>(*idx) >= methods.size()) {
            m_sema.emitError("compile_time.method_name_at index out of range", at);
            return std::nullopt;
        }
        return strResult(methods[static_cast<size_t>(*idx)].first);
    }

    auto name = strArg(1);
    if (!name) {
        m_sema.emitError("compile_time." + m + " requires a method name", at);
        return std::nullopt;
    }
    FunctionType* fn = nullptr;
    for (auto& meth : methods) {
        if (meth.first == *name) {
            fn = meth.second;
            break;
        }
    }
    if (m == "has_method") return intResult(fn ? 1 : 0);
    if (!fn) {
        m_sema.emitError("unknown method '" + *name + "' in compile_time." + m, at);
        return std::nullopt;
    }
    if (m == "method_param_count") {
        const size_t count = fn->paramTypes.empty() ? 0 : fn->paramTypes.size() - 1;
        return intResult(static_cast<long long>(count));
    }
    if (m == "method_param_type_name") {
        auto idx = intArg(2);
        if (!idx || *idx < 0 || static_cast<size_t>(*idx) + 1 >= fn->paramTypes.size()) {
            m_sema.emitError("compile_time.method_param_type_name index out of range", at);
            return std::nullopt;
        }
        return strResult(m_sema.typeToString(fn->paramTypes[static_cast<size_t>(*idx) + 1]));
    }
    if (m == "method_return_type_name") return strResult(m_sema.typeToString(fn->returnType));
    m_sema.emitError("unknown compile_time class query '" + m + "'", at);
    return std::nullopt;
}

static std::string ctValueAsString(const ConstValue& v) {
    switch (v.type) {
        case ConstValue::INT:
            if (v.isBool) return v.intVal ? "true" : "false";
            return std::to_string(v.intVal);
        case ConstValue::DOUBLE: {
            std::ostringstream os;
            os << v.doubleVal;
            return os.str();
        }
        case ConstValue::CHAR:
            return std::string(1, v.charVal);
        case ConstValue::STR:
            return v.strVal;
        case ConstValue::TYPE:
            return "<type>";
    }
    return "";
}

std::optional<ConstValue>
CompileTimeEvaluator::evalFormat(MethodCallExprAST& node, ASTNode& at) {
    if (node.methodName == "to_string") {
        if (node.args.size() != 1) {
            m_sema.emitError("compile_time.to_string requires exactly one argument", at);
            return std::nullopt;
        }
        auto v = eval(node.args[0].get(), at);
        if (!v) {
            m_sema.emitError("compile_time.to_string argument must be a compile-time constant", at);
            return std::nullopt;
        }
        ConstValue cv;
        cv.type = ConstValue::STR;
        cv.strVal = ctValueAsString(*v);
        return cv;
    }

    if (node.args.empty()) {
        m_sema.emitError("compile_time.format requires a format string", at);
        return std::nullopt;
    }
    auto fmtVal = eval(node.args[0].get(), at);
    if (!fmtVal || fmtVal->type != ConstValue::STR) {
        m_sema.emitError("compile_time.format requires a compile-time string literal", at);
        return std::nullopt;
    }
    std::vector<ConstValue> args;
    for (size_t i = 1; i < node.args.size(); ++i) {
        auto v = eval(node.args[i].get(), at);
        if (!v) {
            m_sema.emitError("compile_time.format arguments must be compile-time constants", at);
            return std::nullopt;
        }
        args.push_back(*v);
    }

    const std::string& fmt = fmtVal->strVal;
    std::string out;
    size_t nextArg = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        if (i + 1 >= fmt.size()) {
            m_sema.emitError("compile_time.format: trailing '%'", at);
            return std::nullopt;
        }
        const char spec = fmt[++i];
        if (spec == '%') {
            out.push_back('%');
            continue;
        }
        if (nextArg >= args.size()) {
            m_sema.emitError("compile_time.format: not enough arguments", at);
            return std::nullopt;
        }
        const ConstValue& v = args[nextArg++];
        switch (spec) {
            case 'd':
            case 'i':
                if (v.type != ConstValue::INT) {
                    m_sema.emitError("compile_time.format: %d expects an integer", at);
                    return std::nullopt;
                }
                out += std::to_string(v.intVal);
                break;
            case 'u':
                if (v.type != ConstValue::INT) {
                    m_sema.emitError("compile_time.format: %u expects an integer", at);
                    return std::nullopt;
                }
                out += std::to_string(static_cast<unsigned long long>(v.intVal));
                break;
            case 'x':
            case 'X': {
                if (v.type != ConstValue::INT) {
                    m_sema.emitError("compile_time.format: %x expects an integer", at);
                    return std::nullopt;
                }
                std::ostringstream os;
                os << std::hex << (spec == 'X' ? std::uppercase : std::nouppercase)
                   << static_cast<unsigned long long>(v.intVal);
                out += os.str();
                break;
            }
            case 's':
                if (v.type != ConstValue::STR) {
                    m_sema.emitError("compile_time.format: %s expects a string", at);
                    return std::nullopt;
                }
                out += v.strVal;
                break;
            case 'c':
                if (v.type == ConstValue::CHAR) out.push_back(v.charVal);
                else if (v.type == ConstValue::INT) out.push_back(static_cast<char>(v.intVal));
                else {
                    m_sema.emitError("compile_time.format: %c expects a character", at);
                    return std::nullopt;
                }
                break;
            case 'f': {
                if (v.type != ConstValue::DOUBLE) {
                    m_sema.emitError("compile_time.format: %f expects a float", at);
                    return std::nullopt;
                }
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%f", v.doubleVal);
                out += buf;
                break;
            }
            default:
                m_sema.emitError(std::string("compile_time.format: unsupported conversion '%") +
                                     spec + "'",
                                 at);
                return std::nullopt;
        }
    }
    if (nextArg != args.size()) {
        m_sema.emitError("compile_time.format: too many arguments", at);
        return std::nullopt;
    }
    ConstValue cv;
    cv.type = ConstValue::STR;
    cv.strVal = std::move(out);
    return cv;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalBuildCall(MethodCallExprAST& node, ASTNode& at) {
    auto strArg = [&](size_t idx) -> std::optional<std::string> {
        if (idx >= node.args.size()) return std::nullopt;
        auto v = eval(node.args[idx].get(), at);
        if (!v || v->type != ConstValue::STR) return std::nullopt;
        return v->strVal;
    };
    ConstValue cv;
    if (node.methodName == "is" && node.args.size() == 1) {
        auto arg = strArg(0);
        if (!arg) {
            m_sema.emitError("compile_time.target.is requires a string literal", at);
            return std::nullopt;
        }
        cv.type = ConstValue::INT;
        cv.intVal = ctTargetOs() == *arg ? 1 : 0;
        return cv;
    }
    if (node.methodName == "has_feature" && node.args.size() == 1) {
        auto arg = strArg(0);
        if (!arg) {
            m_sema.emitError("compile_time.target.has_feature requires a string literal", at);
            return std::nullopt;
        }
        static const auto features = [] {
            return llvm::sys::getHostCPUFeatures();
        }();
        cv.type = ConstValue::INT;
        auto it = features.find(*arg);
        cv.intVal = (it != features.end() && it->second) ? 1 : 0;
        return cv;
    }
    if (node.methodName == "at_least" && node.args.size() == 1) {
        auto arg = strArg(0);
        if (!arg) {
            m_sema.emitError("compile_time.version.at_least requires a string literal", at);
            return std::nullopt;
        }
        cv.type = ConstValue::INT;
        cv.intVal = ctVersionAtLeast(*arg) ? 1 : 0;
        return cv;
    }
    m_sema.emitError("unknown compile_time call '" + node.methodName + "'", at);
    return std::nullopt;
}

std::optional<ConstValue>
CompileTimeEvaluator::evalQuery(MethodCallExprAST& node, ASTNode& at) {
    const std::string& m = node.methodName;
    if (m == "static_assert" || m == "if") return std::nullopt;
    if (m == "size_of" || m == "align_of" || m == "offset_of" ||
        m == "bit_size_of" || m == "padding_of" || m == "is_packed") {
        return evalLayoutQuery(node, at);
    }
    if (m == "is_same" || m == "is_integer" || m == "is_float" || m == "is_bool" ||
        m == "is_pointer" || m == "is_array" || m == "is_aggregate" ||
        m == "is_class" || m == "is_struct" || m == "is_union" || m == "is_enum" ||
        m == "is_void" || m == "is_signed" || m == "bit_width" ||
        m == "int_min" || m == "int_max" || m == "uint_max" ||
        m == "float_epsilon" || m == "float_max" || m == "float_digits" ||
        m == "type_name_of" || m == "type_kind_of" ||
        m == "layout_size_of" || m == "layout_align_of" ||
        m == "is_convertible" || m == "is_assignable" ||
        m == "template_name_of" || m == "template_arg_count" ||
        m == "template_arg_name") {
        return evalTypeQuery(node, at);
    }
    if (m == "type_of" || m == "array_of" || m == "pointer_to" || m == "const_of") {
        return evalTypeValueQuery(node, at);
    }
    if (m == "has_field" || m == "field_count_of" || m == "field_name_at" ||
        m == "field_type_name_of" || m == "field_size_of" ||
        m == "field_offset_of" || m == "field_is_bitfield" ||
        m == "field_bit_width_of" || m == "field_annotation_count_of" ||
        m == "field_annotation_name_at" || m == "field_annotation_arg_of") {
        return evalFieldQuery(node, at);
    }
    if (m == "has_attribute" || m == "attribute_count_of" ||
        m == "attribute_name_at" || m == "attribute_arg_of") {
        Type* t = node.args.empty() ? nullptr : resolveTypeArg(node.args[0].get(), at);
        if (!t) return std::nullopt;
        Type* raw = stripTypedef(t);
        const auto& attrs = !raw->annotations.empty() ? raw->annotations : t->annotations;
        auto intResult = [](long long v) {
            ConstValue cv;
            cv.type = ConstValue::INT;
            cv.intVal = v;
            return cv;
        };
        if (m == "has_attribute") {
            auto* name = node.args.size() > 1
                             ? dynamic_cast<StringExprAST*>(node.args[1].get())
                             : nullptr;
            if (!name) {
                m_sema.emitError("compile_time.has_attribute requires an attribute name", at);
                return std::nullopt;
            }
            for (const auto& ann : attrs)
                if (ann.name == name->value) return intResult(1);
            return intResult(0);
        }
        if (m == "attribute_count_of") return intResult(static_cast<long long>(attrs.size()));
        if (m == "attribute_name_at") {
            auto idx = node.args.size() > 1 ? eval(node.args[1].get(), at) : std::nullopt;
            if (!idx || idx->type != ConstValue::INT || idx->intVal < 0 ||
                static_cast<size_t>(idx->intVal) >= attrs.size()) {
                m_sema.emitError("compile_time.attribute_name_at index out of range", at);
                return std::nullopt;
            }
            ConstValue cv;
            cv.type = ConstValue::STR;
            cv.strVal = attrs[static_cast<size_t>(idx->intVal)].name;
            return cv;
        }
        if (m == "attribute_arg_of") {
            auto* name = node.args.size() > 1
                             ? dynamic_cast<StringExprAST*>(node.args[1].get())
                             : nullptr;
            auto idx = node.args.size() > 2 ? eval(node.args[2].get(), at) : std::nullopt;
            if (!name || !idx || idx->type != ConstValue::INT || idx->intVal < 0) {
                m_sema.emitError("compile_time.attribute_arg_of requires "
                                 "(type, attribute, index)",
                                 at);
                return std::nullopt;
            }
            for (const auto& ann : attrs)
                if (ann.name == name->value)
                    return evalAnnotationArg(ann, static_cast<size_t>(idx->intVal), at);
            m_sema.emitError("unknown annotation '" + name->value + "'", at);
            return std::nullopt;
        }
    }
    if (m == "enum_count_of" || m == "enum_name_at" || m == "enum_value_at" ||
        m == "enum_value_of" || m == "enum_name_of" || m == "enum_underlying_name") {
        return evalEnumQuery(node, at);
    }
    if (m == "method_count_of" || m == "has_method" || m == "method_name_at" ||
        m == "method_param_count" || m == "method_param_type_name" ||
        m == "method_return_type_name" || m == "base_name_of" ||
        m == "is_base_of") {
        return evalClassQuery(node, at);
    }
    if (m == "format" || m == "to_string") {
        return evalFormat(node, at);
    }
    if (m == "name_of") {
        Type* t = node.args.empty() ? nullptr : resolveTypeArg(node.args[0].get(), at);
        if (!t) return std::nullopt;
        ConstValue cv;
        cv.type = ConstValue::STR;
        cv.strVal = m_sema.typeToString(t);
        return cv;
    }
    if (m == "fields_of" || m == "attributes_of") {
        m_sema.emitError("compile_time." + m +
                             " returns a runtime reflection table and cannot be "
                             "used in a compile-time expression",
                         at);
        return std::nullopt;
    }
    if (m == "is" || m == "has_feature" || m == "at_least") {
        return evalBuildCall(node, at);
    }
    m_sema.emitError("unknown compile_time member '" + m + "'", at);
    return std::nullopt;
}

// P1-04 / CT-06: size_of(T)/align_of(T)/offset_of(T, "f") —— 结果 usize.
std::optional<ConstValue>
CompileTimeEvaluator::evalLayoutQuery(MethodCallExprAST& node, ASTNode& at) {
    Type* queriedType = node.args.empty() ? nullptr
                                         : resolveTypeArg(node.args[0].get(), at);
    if (!queriedType) return std::nullopt;
    Type* raw = stripTypedef(queriedType);
    if (isAggregate(raw) && !isCompleteAggregate(raw)) {
        m_sema.emitError("incomplete type '" + m_sema.typeToString(queriedType) +
                             "' in compile_time query",
                         at);
        return std::nullopt;
    }
    const std::string& m = node.methodName;
    const llvm::DataLayout& dl = m_module->getDataLayout();
    ConstValue cv;
    cv.type = ConstValue::INT;

    auto intResult = [](long long v) {
        ConstValue out;
        out.type = ConstValue::INT;
        out.intVal = v;
        return out;
    };

    if (m == "is_packed") {
        bool packed = false;
        if (raw->kind == TypeKind::Struct)
            packed = static_cast<StructType*>(raw)->isPacked;
        else if (raw->kind == TypeKind::Class)
            packed = static_cast<ClassType*>(raw)->isPacked;
        else if (raw->kind == TypeKind::Union)
            packed = static_cast<UnionType*>(raw)->isPacked;
        return intResult(packed ? 1 : 0);
    }

    if (m == "bit_size_of") {
        auto* lr = layoutFor(raw);
        if (lr) return intResult(static_cast<long long>(lr->size * 8));
        llvm::Type* lt = toLLVMType(raw);
        if (!lt) {
            m_sema.emitError("unknown type in compile_time.bit_size_of expression", at);
            return std::nullopt;
        }
        return intResult(static_cast<long long>(dl.getTypeAllocSizeInBits(lt)));
    }

    if (m == "padding_of") {
        auto* lr = layoutFor(raw);
        if (!lr) return intResult(0);
        if (raw->kind == TypeKind::Union) {
            uint64_t maxMember = 0;
            for (auto& member : static_cast<UnionType*>(raw)->members) {
                if (llvm::Type* lt = toLLVMType(member.type))
                    maxMember = std::max<uint64_t>(maxMember, dl.getTypeAllocSize(lt));
            }
            return intResult(static_cast<long long>(lr->size > maxMember ? lr->size - maxMember : 0));
        }
        // Field spans (base fields included) plus bitfield storage-unit spans.
        // Padding is the sum of the gaps between consecutive spans plus the
        // tail. Bitfield storage units are real occupied bytes, not padding.
        struct Span { uint64_t offset; uint64_t size; };
        std::vector<Span> spans;
        for (auto& f : collectFields(raw, true)) {
            auto off = fieldOffsetOf(raw, f.name);
            llvm::Type* lt = toLLVMType(f.type);
            if (!off || !lt) continue;
            spans.push_back({*off, dl.getTypeAllocSize(lt)});
        }
        std::unordered_set<uint32_t> unitIndices;
        auto addBitfieldUnits = [&](auto* agg) {
            for (auto& entry : agg->bitFields) {
                const uint32_t unit = entry.second.unitIndex;
                if (!unitIndices.insert(unit).second) continue;
                if (unit >= lr->fields.size()) continue;
                spans.push_back({lr->fields[unit].offset,
                                 dl.getTypeAllocSize(lr->fields[unit].type)});
            }
        };
        if (raw->kind == TypeKind::Struct)
            addBitfieldUnits(static_cast<StructType*>(raw));
        else if (raw->kind == TypeKind::Class)
            addBitfieldUnits(static_cast<ClassType*>(raw));
        std::sort(spans.begin(), spans.end(),
                  [](const Span& a, const Span& b) { return a.offset < b.offset; });
        uint64_t cursor = 0;
        uint64_t padding = 0;
        for (auto& s : spans) {
            if (s.offset > cursor) padding += s.offset - cursor;
            cursor = std::max(cursor, s.offset + s.size);
        }
        if (lr->size > cursor) padding += lr->size - cursor;
        return intResult(static_cast<long long>(padding));
    }

    if (m == "offset_of") {
        if (node.args.size() != 2) {
            m_sema.emitError("compile_time.offset_of requires (type, field)", at);
            return std::nullopt;
        }
        std::string path;
        if (auto* field = dynamic_cast<StringExprAST*>(node.args[1].get())) {
            path = field->value;
        } else if (auto* var = dynamic_cast<VariableExprAST*>(node.args[1].get())) {
            path = var->name;
        } else {
            m_sema.emitError("compile_time.offset_of field must be a string literal or identifier", at);
            return std::nullopt;
        }
        // Materialize bitfield metadata/offsets before walking the path.
        (void)layoutFor(raw);
        // Nested paths (a.b.c) walk one segment at a time.
        Type* cursor = raw;
        uint64_t total = 0;
        size_t pos = 0;
        while (pos <= path.size()) {
            size_t dot = path.find('.', pos);
            std::string seg = path.substr(pos, dot == std::string::npos
                                                  ? std::string::npos : dot - pos);
            if (seg.empty()) {
                m_sema.emitError("empty field path in compile_time.offset_of", at);
                return std::nullopt;
            }
            auto f = findField(cursor, seg, true);
            if (!f) {
                m_sema.emitError("unknown field '" + seg + "' in compile_time.offset_of expression", at);
                return std::nullopt;
            }
            if (bitFieldOf(cursor, seg)) {
                m_sema.emitError("cannot take offset of a bit-field", at);
                return std::nullopt;
            }
            auto off = fieldOffsetOf(cursor, seg);
            if (!off) {
                m_sema.emitError("unknown field '" + seg + "' in compile_time.offset_of expression", at);
                return std::nullopt;
            }
            total += *off;
            cursor = stripTypedef(f->type);
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
        return intResult(static_cast<long long>(total));
    }

    auto* lr = layoutFor(raw);
    if (lr) {
        if (m == "size_of") return intResult(static_cast<long long>(lr->size));
        if (m == "align_of") return intResult(static_cast<long long>(lr->align));
    }
    llvm::Type* lt = toLLVMType(raw);
    if (!lt) {
        m_sema.emitError("unknown type in compile_time." + m + " expression", at);
        return std::nullopt;
    }
    if (m == "size_of") return intResult(static_cast<long long>(dl.getTypeAllocSize(lt)));
    if (m == "align_of") return intResult(static_cast<long long>(dl.getABITypeAlign(lt).value()));
    m_sema.emitError("unknown compile_time layout query '" + m + "'", at);
    return std::nullopt;
}

// CT-07: fields_of — named (non-padding) fields with offset/size, from the
// LayoutBuilder (single source with codegen / offset_of). Padding pseudo-fields
// (name=="") and the base sub-object (slot 0, no name) are excluded.
std::vector<CompileTimeEvaluator::ReflectField>
CompileTimeEvaluator::evalReflectFields(Type* t, ASTNode& at) {
    std::vector<ReflectField> out;
    Type* raw = stripTypedef(t);
    if (!raw || !isAggregate(raw)) return out;

    auto fields = collectFields(raw, true);
    const llvm::DataLayout& dl = m_module->getDataLayout();
    for (auto& f : fields) {
        ReflectField rf;
        rf.name = f.name;
        if (auto off = fieldOffsetOf(raw, f.name)) rf.offset = *off;
        if (llvm::Type* lt = toLLVMType(f.type))
            rf.size = static_cast<size_t>(dl.getTypeAllocSize(lt));
        out.push_back(std::move(rf));
    }
    return out;
}
