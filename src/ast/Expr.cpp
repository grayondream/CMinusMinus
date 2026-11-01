#include "Expr.h"
#include "Type.h"
#include "Symbol.h"
#include "LayoutBuilder.h"
#include "codegen/CodegenContext.h"
#include "support/Log.h"
#include "Mangle.h"
#include "ast/Decl.h"
#include "ast/Stmt.h"
#include "sema/ReflectionRegistry.h"

#include "llvm/IR/InlineAsm.h"

#include <unordered_map>

// P1-06: builtin str/string lowering (defined at file bottom).
static llvm::Value* codegenBuiltinMethod(CodegenContext& ctx, MethodCallExprAST& node);
static llvm::Value* codegenStringMethod(CodegenContext& ctx, MethodCallExprAST& node);
static llvm::Value* codegenStrFromC(CodegenContext& ctx, CallExprAST& node);
// CT-07: compile_time reflection lowering (defined at file bottom).
static llvm::Value* codegenReflection(CodegenContext& ctx, MethodCallExprAST& node);

// TYP-12: build the {ptr, len} view over a statically-sized array (zero-copy;
// the elements are never duplicated). Array operands already yield the
// address of their first element.
static Type* stripTypedefsT12(Type* t) {
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    return t;
}

// I4 (D6): string 按值形参——活值实参深拷贝到临时后传值（形参独立缓冲，
// callee destroy 不影响调用方）；新鲜实参（string.new / 返回 string 的调用）
// 直接传（移动）。判定与拷贝 helper 供三处实参组装共用。
static bool isStringType(Type* t) {
    t = stripTypedefsT12(t);
    return t && t->kind == TypeKind::String;
}

static llvm::Value* emitStringArgCopy(CodegenContext& ctx, llvm::Value* argVal,
                                      Type* paramType) {
    auto& builder = ctx.getBuilder();
    llvm::Type* llvmTy = ctx.getLLVMType(paramType);
    llvm::Value* tmp = builder.CreateAlloca(llvmTy, nullptr, "strarg");
    auto savedIP = builder.saveIP();
    llvm::Function* copyFn = ctx.getStringCopyFn();
    builder.restoreIP(savedIP);
    builder.CreateCall(copyFn, {tmp, argVal});
    return builder.CreateLoad(llvmTy, tmp, "strarg.v");
}

// TYP-12: strip typedefs, then report whether the value is an array being
// passed where a slice view is expected.
static bool isArrayToSliceArg(Type* argType, Type* paramType) {
    argType = stripTypedefsT12(argType);
    paramType = stripTypedefsT12(paramType);
    return argType && paramType && argType->kind == TypeKind::Array &&
           paramType->kind == TypeKind::Slice;
}

static llvm::Value* emitLoad(CodegenContext& ctx, llvm::Value* ptr, Type* astType = nullptr) {
    return ctx.loadValue(ptr, astType);
}

// P1-04 / CT-12: compile_time 常量节点直接出 llvm::Constant（零运行时指令）。
// 非常量节点返回 nullptr（走普通路径）；ctHandled 但无值（static_assert 等）
// 出被丢弃的 i32 0。必须在任何子表达式出码之前调用——compile_time 根标识符
// 未声明，子树出码会失败。
static llvm::Value* ctConstantOrNull(CodegenContext& ctx, ExprAST& node) {
    if (node.ctInt && node.type) {
        llvm::Type* ty = ctx.getLLVMType(node.type);
        if (auto* intTy = llvm::dyn_cast<llvm::IntegerType>(ty)) {
            return llvm::ConstantInt::get(intTy, static_cast<uint64_t>(*node.ctInt));
        }
    }
    if (node.ctFloat && node.type) {
        return llvm::ConstantFP::get(ctx.getLLVMType(node.type), *node.ctFloat);
    }
    if (node.ctHandled) {
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
    }
    return nullptr;
}

// P1-07 / CG-09: unified read point — dereference an lvalue to its value.
// Bit-field lvalues return the storage-unit address from codegen, so they are
// read via loadBitFieldValue (RMW-aware) instead of loading the whole unit.
// Non-lvalues are returned unchanged.
static llvm::Value* readLValue(CodegenContext& ctx, ExprAST& e, llvm::Value* v) {
    if (!v) return nullptr;
    if (!e.isLValue) return v;
    if (e.isBitFieldAccess()) {
        auto& ma = static_cast<MemberAccessExprAST&>(e);
        return ctx.loadBitFieldValue(v, ma.bitOffset, ma.bitWidth, e.type, ma.unitBits);
    }
    return ctx.loadValue(v, e.type);
}

// Evaluate an expression node as a value: lvalues are dereferenced, rvalues are
// used as-is. A pointer-typed rvalue (string literal, call result, function
// designator) is already a value and must not be loaded from.
static llvm::Value* emitRValue(CodegenContext& ctx, ExprAST& expr, llvm::Value* v) {
    if (!v) return nullptr;
    return readLValue(ctx, expr, v);
}

// C default argument promotions for variadic calls: float -> double and integer
// types narrower than int -> int (sign- or zero-extended per signedness).
// `enum` uses its explicit underlying type. Other types are returned unchanged.
static llvm::Value* promoteVarArg(CodegenContext& ctx, llvm::Value* v, Type* t) {
    if (!v || !t) return v;
    while (t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    if (!t) return v;
    llvm::LLVMContext& c = ctx.getContext();
    auto& builder = ctx.getBuilder();
    llvm::Type* i32 = llvm::Type::getInt32Ty(c);

    switch (t->kind) {
        case TypeKind::Bool:
            return v->getType()->isIntegerTy(1) ? builder.CreateZExt(v, i32, "promote") : v;
        case TypeKind::Char:
        case TypeKind::Int8:
        case TypeKind::Int16:
            return builder.CreateSExt(v, i32, "promote");
        case TypeKind::UInt8:
        case TypeKind::UInt16:
            return builder.CreateZExt(v, i32, "promote");
        case TypeKind::Enum: {
            auto* et = static_cast<EnumType*>(t);
            return et->underlyingType ? promoteVarArg(ctx, v, et->underlyingType) : v;
        }
        case TypeKind::Float32:
            return ctx.castValue(v, llvm::Type::getDoubleTy(c));
        // TYP-04 / C23 default argument promotions: _Float16 promotes to
        // double in variadic calls; __float128 is passed unchanged (the
        // x86-64 ABI has native quad support).
        case TypeKind::Float16:
            return ctx.castValue(v, llvm::Type::getDoubleTy(c));
        default:
            return v;
    }
}

// Apply C's default argument promotions so a print argument matches its
// compile-time chosen printf conversion.
static llvm::Value* promotePrintArg(CodegenContext& ctx, llvm::Value* v, PrintArgKind kind) {
    if (!v) return nullptr;
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();
    llvm::Type* ty = v->getType();

    switch (kind) {
        case PrintArgKind::Bool: {
            llvm::Value* cond = v;
            if (ty->isIntegerTy() && ty->getIntegerBitWidth() != 1) {
                cond = builder.CreateICmpNE(v, llvm::ConstantInt::get(ty, 0), "boolcond");
            }
            llvm::Value* yes = builder.CreateGlobalString("true", ".boolstr");
            llvm::Value* no = builder.CreateGlobalString("false", ".boolstr");
            return builder.CreateSelect(cond, yes, no, "boolstr");
        }
        case PrintArgKind::Char:
        case PrintArgKind::Int32:
            if (ty->isIntegerTy(32)) return v;
            if (ty->isIntegerTy(1)) return builder.CreateZExt(v, llvm::Type::getInt32Ty(c), "promo");
            return builder.CreateSExtOrTrunc(v, llvm::Type::getInt32Ty(c), "promo");
        case PrintArgKind::UInt32:
            if (ty->isIntegerTy(32)) return v;
            return builder.CreateZExtOrTrunc(v, llvm::Type::getInt32Ty(c), "promo");
        case PrintArgKind::Int64:
            if (ty->isIntegerTy(64)) return v;
            return builder.CreateSExtOrTrunc(v, llvm::Type::getInt64Ty(c), "promo");
        case PrintArgKind::UInt64:
            if (ty->isIntegerTy(64)) return v;
            return builder.CreateZExtOrTrunc(v, llvm::Type::getInt64Ty(c), "promo");
        case PrintArgKind::Float:
            if (ty->isDoubleTy()) return v;
            // TYP-04: float16 promotes losslessly; float128 truncates to
            // double precision for printing.
            if (ty->isFloatTy() || ty->isHalfTy())
                return builder.CreateFPExt(v, llvm::Type::getDoubleTy(c), "promo");
            if (ty->isFP128Ty())
                return builder.CreateFPTrunc(v, llvm::Type::getDoubleTy(c), "promo");
            return v;
        case PrintArgKind::CString:
        case PrintArgKind::Pointer:
        case PrintArgKind::ToString:
        default:
            return v;
    }
}

// P1-09 (FMT-08): render a placeholder value into a padded, NUL-terminated
// stack buffer for specs printf cannot express (centering or a custom fill
// character). Returns the buffer to pass through printf's "%s". When outLen
// is non-null it receives the padded byte count (max(rendered, width)).
static llvm::Value* emitPaddedValue(CodegenContext& ctx, const PrintSpec& spec,
                                    PrintArgKind kind, llvm::Value* v,
                                    llvm::Value** outLen = nullptr) {
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i32Ty = llvm::Type::getInt32Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);

    // 1) Render the bare value (no width/fill/align) into a fixed buffer.
    //    Bounds: ints/binary <= 64 bytes; floats <= ~311 digits + precision.
    PrintSpec bare = spec;
    bare.align = 0;
    bare.fill = ' ';
    bare.width = 0;
    llvm::Value* srcPtr = nullptr;
    llvm::Value* len = nullptr;
    if (kind == PrintArgKind::Str) {
        // str/string values carry their own bytes; copy straight from the view.
        srcPtr = builder.CreateExtractValue(v, 0, "pad.ptr");
        len = builder.CreateExtractValue(v, 1, "pad.len");
    } else {
        llvm::Value* promoted = promotePrintArg(ctx, v, kind);
        if (!promoted) return nullptr;
        std::string bareConv, err;
        bool needsRender = false;
        if (!specToPrintfConversion(kind, bare, bareConv, needsRender, err)) {
            bareConv = "%s";
        }
        const int bufSize = 512 + (spec.precision > 0 ? spec.precision : 0);
        llvm::Value* buf1 = builder.CreateAlloca(
            i8Ty, llvm::ConstantInt::get(i32Ty, bufSize), "pad.buf");
        llvm::Value* fmt = builder.CreateGlobalString(bareConv, ".padfmt");
        llvm::FunctionCallee snprintfFn = ctx.getModule().getOrInsertFunction(
            "snprintf",
            llvm::FunctionType::get(
                i32Ty, {llvm::PointerType::get(c, 0), i64Ty,
                        llvm::PointerType::get(c, 0)},
                true));
        llvm::Value* ret = builder.CreateCall(
            snprintfFn.getFunctionType(), snprintfFn.getCallee(),
            {buf1, llvm::ConstantInt::get(i64Ty, bufSize), fmt, promoted});
        len = builder.CreateZExt(ret, i64Ty, "pad.len");
        // Clamp defensively: a negative snprintf return means encoding failure,
        // a would-be length beyond the buffer means truncation — the padded
        // content never exceeds what snprintf actually wrote into buf1.
        len = builder.CreateSelect(
            builder.CreateICmpSLT(ret, llvm::ConstantInt::get(i32Ty, 0)),
            llvm::ConstantInt::get(i64Ty, 0),
            builder.CreateSelect(
                builder.CreateICmpUGT(
                    len, llvm::ConstantInt::get(i64Ty, bufSize - 1)),
                llvm::ConstantInt::get(i64Ty, bufSize - 1), len, "pad.len.hi"),
            "pad.len.clamp");
        srcPtr = buf1;
    }

    // 2) Pad into a runtime-sized buffer: memset(fill) + memcpy at offset.
    llvm::Value* widthV =
        llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(spec.width));
    llvm::Value* total = builder.CreateSelect(
        builder.CreateICmpULT(len, widthV), widthV, len, "pad.total");
    llvm::Value* buf2 = builder.CreateAlloca(
        i8Ty, builder.CreateAdd(total, llvm::ConstantInt::get(i64Ty, 1)),
        "pad.out");
    builder.CreateMemSet(buf2, llvm::ConstantInt::get(i8Ty, spec.fill), total,
                         std::nullopt);
    llvm::Value* off = llvm::ConstantInt::get(i64Ty, 0);
    if (spec.align == '^') {
        off = builder.CreateLShr(builder.CreateSub(total, len),
                                 llvm::ConstantInt::get(i64Ty, 1), "pad.off");
    } else if (spec.align == '>') {
        off = builder.CreateSub(total, len, "pad.off");
    }
    llvm::Value* dst =
        builder.CreateGEP(i8Ty, buf2, off, "pad.dst");
    builder.CreateMemCpy(dst, std::nullopt, srcPtr, std::nullopt, len);
    builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0),
                        builder.CreateGEP(i8Ty, buf2, total, "pad.nul"));
    if (outLen) *outLen = total;
    return buf2;
}

llvm::Value* NumberExprAST::codegen(CodegenContext& ctx) {
    // LEX-15: emit the integer constant at the literal's own width and
    // signedness (from sema), instead of always truncating to i32.
    llvm::Type* ty = type ? ctx.getLLVMType(type)
                          : llvm::Type::getInt32Ty(ctx.getContext());
    unsigned bits = ty->getIntegerBitWidth();

    bool isSigned = true;
    if (type) {
        switch (type->kind) {
            case TypeKind::UInt8:
            case TypeKind::UInt16:
            case TypeKind::UInt32:
            case TypeKind::UInt64:
            case TypeKind::UInt128:
            case TypeKind::USize:
                isSigned = false;
                break;
            default:
                break;
        }
    }
    if (valueHi != 0) {
        // LEX-17: 128 位字面量按 {lo, hi} 两字构造 APInt。
        llvm::APInt ap(bits, llvm::ArrayRef<uint64_t>(
            {static_cast<uint64_t>(value), static_cast<uint64_t>(valueHi)}));
        return llvm::ConstantInt::get(ctx.getContext(), ap);
    }
    return llvm::ConstantInt::get(
        ctx.getContext(), llvm::APInt(bits, static_cast<uint64_t>(value), isSigned));
}

// Found issue: `true`/`false` 是 bool 字面量——发射 i1 常量；整数上下文
// （int32 x = true 等）经 castValue 的 ZExt 提升路径。
llvm::Value* BoolLiteralExprAST::codegen(CodegenContext& ctx) {
    return llvm::ConstantInt::get(ctx.getContext(), llvm::APInt(1, value ? 1 : 0));
}

llvm::Value* FloatExprAST::codegen(CodegenContext& ctx) {
    // LEX-15: pick the APFloat semantics from the literal's fixed-width type.
    // Build at double precision, then convert to the target semantics.
    llvm::APFloat ap(value);
    if (type) {
        const llvm::fltSemantics* sem = nullptr;
        switch (type->kind) {
            case TypeKind::Float16:  sem = &llvm::APFloat::IEEEhalf();   break;
            case TypeKind::Float32:  sem = &llvm::APFloat::IEEEsingle(); break;
            case TypeKind::Float64:  sem = &llvm::APFloat::IEEEdouble(); break;
            case TypeKind::Float128: sem = &llvm::APFloat::IEEEquad();   break;
            default: break;
        }
        if (sem && sem != &ap.getSemantics()) {
            bool losesInfo = false;
            ap.convert(*sem, llvm::APFloat::rmNearestTiesToEven, &losesInfo);
        }
    }
    return llvm::ConstantFP::get(ctx.getContext(), ap);
}

llvm::Value* CharExprAST::codegen(CodegenContext& ctx) {
    // P1-06: char is signed on x86-64 — a byte literal >= 0x80 must be
    // truncated to 8 bits, not sign-extended into the APInt (assert crash).
    return llvm::ConstantInt::get(ctx.getContext(),
        llvm::APInt(8, static_cast<unsigned char>(value)));
}

llvm::Value* StringExprAST::codegen(CodegenContext& ctx) {
    // TYP-05: u8/u16/u32-prefixed literals are fixed-width arrays — build a
    // static global `[N x elemTy]` and return the decayed pointer (element 0).
    if (prefix != StringLiteralPrefix::None) {
        llvm::LLVMContext& c = ctx.getContext();
        llvm::Type* elemTy = nullptr;
        switch (prefix) {
            case StringLiteralPrefix::U8:
                elemTy = llvm::Type::getInt8Ty(c);
                break;
            case StringLiteralPrefix::U16:
                elemTy = llvm::Type::getInt16Ty(c);
                break;
            case StringLiteralPrefix::U32:
                elemTy = llvm::Type::getInt32Ty(c);
                break;
            default:
                break;
        }
        auto* arrTy = llvm::ArrayType::get(elemTy, value.size());
        std::vector<llvm::Constant*> elems;
        elems.reserve(value.size());
        for (unsigned char ch : value) {
            elems.push_back(llvm::ConstantInt::get(elemTy, ch));
        }
        auto* arrConst = llvm::ConstantArray::get(arrTy, elems);
        auto* gv = new llvm::GlobalVariable(ctx.getModule(), arrTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, arrConst, ".u8str");
        return ctx.getBuilder().CreateConstInBoundsGEP2_32(arrTy, gv, 0, 0, "u8str.ptr");
    }
    // P1-06 (FMT-03): literal is a `str` — {ptr, len} of a static global
    // (CreateGlobalString product); non-owning, globally alive (DEC-20).
    llvm::LLVMContext& c = ctx.getContext();
    auto* strTy = llvm::StructType::getTypeByName(c, "str");
    if (!strTy) {
        strTy = llvm::StructType::create(c,
            {llvm::PointerType::get(c, 0), llvm::Type::getInt64Ty(c)}, "str");
    }
    // The builder has no insertion block while materializing a global
    // initializer; pass the module explicitly so CreateGlobalString does not
    // dereference a null insertion block.
    llvm::Value* ptr = ctx.getBuilder().CreateGlobalString(
        value, ".str", /*AddressSpace=*/0, &ctx.getModule());
    return llvm::ConstantStruct::get(strTy,
        {llvm::dyn_cast<llvm::Constant>(ptr),
         llvm::ConstantInt::get(llvm::Type::getInt64Ty(c), value.size())});
}

llvm::Value* VariableExprAST::codegen(CodegenContext& ctx) {
    // Enumerator: materialize the compile-time integer constant.
    if (isEnumConstant) {
        llvm::Type* ty = type ? ctx.getLLVMType(type)
                              : llvm::Type::getInt32Ty(ctx.getContext());
        return llvm::ConstantInt::get(ty, static_cast<uint64_t>(enumValue), /*isSigned=*/true);
    }

    // A function name used as a value yields the function itself.
    if (isFunctionRef) {
        if (llvm::Function* fn = ctx.getModule().getFunction(resolvedFunctionName)) {
            return fn;
        }
        LOGE("unknown function: {}", resolvedFunctionName);
        return nullptr;
    }

    Symbol* sym = ctx.currentScope()->lookup(name);
    if (!sym) {
        LOGE("unknown variable: {}", name);
        return nullptr;
    }
    return ctx.lookupVariableAddr(name);
}

llvm::Value* BinaryExprAST::codegen(CodegenContext& ctx) {
    if (auto* ct = ctConstantOrNull(ctx, *this)) return ct;
    // Check if this is an overloaded operator call
    if (!mangledCallee.empty()) {
        llvm::Value* lhs = left->codegen(ctx);
        llvm::Value* rhs = right->codegen(ctx);
        if (!lhs || !rhs) return nullptr;

        // Member-form operator: pass the left object address as implicit this.
        if (isMethodOperator) {
            auto& builder = ctx.getBuilder();
            llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
            if (!calleeFn) {
                llvm::Type* retLLVM = type ? ctx.getLLVMType(type)
                                           : llvm::Type::getVoidTy(ctx.getContext());
                std::vector<llvm::Type*> paramLLVM;
                for (auto* pt : resolvedParamTypes) paramLLVM.push_back(ctx.getLLVMType(pt));
                auto* fnType = llvm::FunctionType::get(retLLVM, paramLLVM, false);
                calleeFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                                  mangledCallee, ctx.getModule());
            }
            if (right->isLValue) rhs = readLValue(ctx, *right, rhs);
            if (resolvedParamTypes.size() > 1)
                rhs = ctx.castValue(rhs, right->type,
                                    ctx.getLLVMType(resolvedParamTypes[1]));
            return builder.CreateCall(calleeFn, {lhs, rhs}, "mopcalltmp");
        }

        lhs = emitRValue(ctx, *left, lhs);
        rhs = emitRValue(ctx, *right, rhs);

        llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
        if (!calleeFn) {
            LOGE("unknown operator function: {}", mangledCallee);
            return nullptr;
        }
        return ctx.getBuilder().CreateCall(calleeFn, {lhs, rhs}, "opcalltmp");
    }

    // P1-09: `&&`/`||` are short-circuit logical operators with C truthiness
    // (value != 0 via coerceToBool) — not bitwise And/Or, which mis-evaluated
    // high-bit truthy values and never skipped the RHS.
    if (op == BinaryOp::And || op == BinaryOp::Or) {
        auto& scBuilder = ctx.getBuilder();
        llvm::LLVMContext& c = ctx.getContext();
        auto evalTruthy = [&](ExprAST& e) -> llvm::Value* {
            llvm::Value* v = e.codegen(ctx);
            if (!v) return nullptr;
            v = emitRValue(ctx, e, v);
            return ctx.coerceToBool(v, e.type);
        };
        llvm::Value* lhsV = evalTruthy(*left);
        if (!lhsV) return nullptr;
        llvm::Function* fn = scBuilder.GetInsertBlock()->getParent();
        llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(c, "log.rhs", fn);
        llvm::BasicBlock* shortBB = llvm::BasicBlock::Create(c, "log.short", fn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(c, "log.merge", fn);
        // &&: false short-circuits to false; ||: true short-circuits to true.
        scBuilder.CreateCondBr(lhsV,
            op == BinaryOp::And ? rhsBB : shortBB,
            op == BinaryOp::And ? shortBB : rhsBB);
        scBuilder.SetInsertPoint(shortBB);
        llvm::Value* shortVal = llvm::ConstantInt::get(
            llvm::Type::getInt1Ty(c), op == BinaryOp::Or);
        scBuilder.CreateBr(mergeBB);
        scBuilder.SetInsertPoint(rhsBB);
        llvm::Value* rhsV = evalTruthy(*right);
        if (!rhsV) return nullptr;
        // The RHS may itself create blocks (nested &&/||, str ==/!= memcmp
        // structure, ternary) — the PHI incoming block must be where its
        // evaluation ENDED, not the block it started in.
        llvm::BasicBlock* rhsEnd = scBuilder.GetInsertBlock();
        scBuilder.CreateBr(mergeBB);
        scBuilder.SetInsertPoint(mergeBB);
        auto* phi = scBuilder.CreatePHI(llvm::Type::getInt1Ty(c), 2, "log.val");
        phi->addIncoming(shortVal, shortBB);
        phi->addIncoming(rhsV, rhsEnd);
        return phi;
    }

    llvm::Value* lhs = left->codegen(ctx);
    llvm::Value* rhs = right->codegen(ctx);
    if (!lhs || !rhs) return nullptr;

    lhs = emitRValue(ctx, *left, lhs);
    rhs = emitRValue(ctx, *right, rhs);

    auto& builder = ctx.getBuilder();

    // P1-06 (FMT-01): str == / != compare bytes — length first, then memcmp
    // over the (equal) length; never read past either view.
    {
        Type* lTy = left->type;
        Type* rTy = right->type;
        auto isStrT = [](Type* t) {
            while (t && t->kind == TypeKind::Typedef)
                t = static_cast<TypedefType*>(t)->aliasedType;
            return t && t->kind == TypeKind::Str;
        };
        if ((op == BinaryOp::Eq || op == BinaryOp::NotEq) &&
            isStrT(lTy) && isStrT(rTy) &&
            lhs->getType()->isStructTy() && rhs->getType()->isStructTy()) {
            llvm::LLVMContext& c = ctx.getContext();
            llvm::Value* lenL = builder.CreateExtractValue(lhs, 1, "strlen.l");
            llvm::Value* lenR = builder.CreateExtractValue(rhs, 1, "strlen.r");
            llvm::Value* lenEq = builder.CreateICmpEQ(lenL, lenR, "strlen.eq");
            // Lengths differ → Eq false / Ne true, and memcmp must NOT run
            // (select does not short-circuit: it would read past the shorter
            // view — final review I1). Branch instead.
            llvm::Function* fn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* condBB = builder.GetInsertBlock();
            llvm::BasicBlock* cmpBB = llvm::BasicBlock::Create(c, "str.cmp", fn);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(c, "str.done", fn);
            builder.CreateCondBr(lenEq, cmpBB, doneBB);

            builder.SetInsertPoint(cmpBB);
            llvm::Value* ptrL = builder.CreateExtractValue(lhs, 0, "strptr.l");
            llvm::Value* ptrR = builder.CreateExtractValue(rhs, 0, "strptr.r");
            llvm::FunctionType* memcmpTy = llvm::FunctionType::get(
                llvm::Type::getInt32Ty(c),
                {llvm::PointerType::get(c, 0), llvm::PointerType::get(c, 0),
                 llvm::Type::getInt64Ty(c)},
                false);
            llvm::FunctionCallee memcmpFn =
                ctx.getModule().getOrInsertFunction("memcmp", memcmpTy);
            llvm::Value* bytesEq = builder.CreateICmpEQ(
                builder.CreateCall(memcmpFn, {ptrL, ptrR, lenL}, "memcmp"),
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(c), 0), "bytes.eq");
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            auto* eq = builder.CreatePHI(llvm::Type::getInt1Ty(c), 2, "str.eq");
            eq->addIncoming(llvm::ConstantInt::getFalse(c), condBB);
            eq->addIncoming(bytesEq, cmpBB);
            return op == BinaryOp::Eq ? static_cast<llvm::Value*>(eq)
                                      : builder.CreateNot(eq, "str.ne");
        }
    }

    // Pointer arithmetic (ptr +/- int, int + ptr) must use GEP, not add/sub.
    auto pointerLike = [](Type* t) {
        return t && (t->kind == TypeKind::Pointer || t->kind == TypeKind::Array);
    };
    const bool leftPtr = pointerLike(left->type);
    const bool rightPtr = pointerLike(right->type);
    if ((op == BinaryOp::Add || op == BinaryOp::Sub) && (leftPtr != rightPtr)) {
        llvm::Value* ptrVal = leftPtr ? lhs : rhs;
        llvm::Value* idxVal = leftPtr ? rhs : lhs;
        Type* ptrType = leftPtr ? left->type : right->type;
        Type* pointee = nullptr;
        if (ptrType->kind == TypeKind::Pointer) {
            pointee = ptrType->base;
        } else if (ptrType->kind == TypeKind::Array) {
            pointee = static_cast<ArrayType*>(ptrType)->elementType;
        }
        llvm::Type* elemTy = pointee ? ctx.getLLVMType(pointee)
                                     : llvm::Type::getInt8Ty(ctx.getContext());
        if (!elemTy) elemTy = llvm::Type::getInt8Ty(ctx.getContext());
        if (op == BinaryOp::Sub) {
            idxVal = builder.CreateNeg(idxVal, "negidx");
        }
        return builder.CreateGEP(elemTy, ptrVal, idxVal, "ptradd");
    }

    // Pointer vs integer comparison (e.g. `p == null`, `0 != p`): compare as
    // integers of pointer width. `opType` below would otherwise be the pointer
    // type and leave the integer operand unconverted (ICmp type assert), while
    // converting to the integer's own width could truncate the address.
    if (leftPtr != rightPtr) {
        llvm::Value* intVal = leftPtr ? rhs : lhs;
        llvm::Value* ptrVal = leftPtr ? lhs : rhs;
        if (intVal->getType()->isIntegerTy()) {
            llvm::Type* ptrIntTy =
                ctx.getModule().getDataLayout().getIntPtrType(ctx.getContext());
            llvm::Value* ptrAsInt = builder.CreatePtrToInt(ptrVal, ptrIntTy, "ptrtoint");
            llvm::Value* intWide = ctx.castValue(intVal, ptrIntTy);
            if (leftPtr) {
                lhs = ptrAsInt;
                rhs = intWide;
            } else {
                lhs = intWide;
                rhs = ptrAsInt;
            }
        }
    }

    // Coerce both operands to a common arithmetic type (TYP-22). The AST types
    // drive integer promotion, the usual arithmetic conversions and signedness;
    // the opcode selection (sdiv/udiv, icmp slt/ult, ashr/lshr) follows suit.
    Type* leftAst = left->type;
    Type* rightAst = right->type;
    const bool isShift = (op == BinaryOp::LShift || op == BinaryOp::RShift);
    Type* resultAstTy = nullptr;
    if (leftAst && rightAst && isArithmeticType(leftAst) && isArithmeticType(rightAst)) {
        // Shifts yield the promoted left operand; other operators the usual
        // arithmetic common type.
        resultAstTy = isShift ? promoteArithmeticType(leftAst)
                              : usualArithmeticType(leftAst, rightAst);
    }

    llvm::Type* lhsTy = lhs->getType();
    llvm::Type* rhsTy = rhs->getType();
    llvm::Type* opType = lhsTy;
    if (resultAstTy) {
        opType = ctx.getLLVMType(resultAstTy);
    } else if (lhsTy->isFloatingPointTy() || rhsTy->isFloatingPointTy()) {
        if (lhsTy->isFloatingPointTy() && rhsTy->isFloatingPointTy()) {
            opType = (lhsTy->getFPMantissaWidth() >= rhsTy->getFPMantissaWidth()) ? lhsTy : rhsTy;
        } else if (lhsTy->isFloatingPointTy()) {
            opType = lhsTy;
        } else {
            opType = rhsTy;
        }
    } else if (lhsTy->isIntegerTy() && rhsTy->isIntegerTy()) {
        opType = (lhsTy->getIntegerBitWidth() >= rhsTy->getIntegerBitWidth()) ? lhsTy : rhsTy;
    }
    lhs = ctx.castValue(lhs, leftAst, opType);
    rhs = ctx.castValue(rhs, rightAst, opType);
    bool isFloat = opType->isFloatingPointTy();
    // Address comparisons and unsigned common types use unsigned predicates.
    bool unsignedOp = (leftPtr || rightPtr) ||
                      (resultAstTy && isUnsignedArithmeticType(resultAstTy));

    switch (op) {
        case BinaryOp::Add:    return isFloat ? builder.CreateFAdd(lhs, rhs, "addtmp")
                                              : builder.CreateAdd(lhs, rhs, "addtmp");
        case BinaryOp::Sub: {
            // 指针差 `p - q`：LLVM CreatePtrDiff 按元素计数（ptrdiff）。
            if (lhs->getType()->isPointerTy() && rhs->getType()->isPointerTy()) {
                Type* leftAstT = stripTypedefsT12(leftAst);
                llvm::Type* elemTy = llvm::Type::getInt8Ty(ctx.getContext());
                if (leftAstT && leftAstT->kind == TypeKind::Pointer && leftAstT->base)
                    elemTy = ctx.getLLVMType(leftAstT->base);
                if (!elemTy) elemTy = llvm::Type::getInt8Ty(ctx.getContext());
                return builder.CreatePtrDiff(elemTy, lhs, rhs, "ptrdiff");
            }
            return isFloat ? builder.CreateFSub(lhs, rhs, "subtmp")
                           : builder.CreateSub(lhs, rhs, "subtmp");
        }
        case BinaryOp::Mul:    return isFloat ? builder.CreateFMul(lhs, rhs, "multmp")
                                              : builder.CreateMul(lhs, rhs, "multmp");
        case BinaryOp::Div:    return isFloat ? builder.CreateFDiv(lhs, rhs, "divtmp")
                                              : (unsignedOp ? builder.CreateUDiv(lhs, rhs, "divtmp")
                                                            : builder.CreateSDiv(lhs, rhs, "divtmp"));
        case BinaryOp::Mod:    return isFloat ? builder.CreateFRem(lhs, rhs, "modtmp")
                                              : (unsignedOp ? builder.CreateURem(lhs, rhs, "modtmp")
                                                            : builder.CreateSRem(lhs, rhs, "modtmp"));
        case BinaryOp::Eq:     return isFloat ? builder.CreateFCmpOEQ(lhs, rhs, "eqtmp")
                                              : builder.CreateICmpEQ(lhs, rhs, "eqtmp");
        case BinaryOp::NotEq:  return isFloat ? builder.CreateFCmpONE(lhs, rhs, "netmp")
                                              : builder.CreateICmpNE(lhs, rhs, "netmp");
        case BinaryOp::Lt:     return isFloat ? builder.CreateFCmpOLT(lhs, rhs, "lttmp")
                                              : (unsignedOp ? builder.CreateICmpULT(lhs, rhs, "lttmp")
                                                            : builder.CreateICmpSLT(lhs, rhs, "lttmp"));
        case BinaryOp::Gt:     return isFloat ? builder.CreateFCmpOGT(lhs, rhs, "gttmp")
                                              : (unsignedOp ? builder.CreateICmpUGT(lhs, rhs, "gttmp")
                                                            : builder.CreateICmpSGT(lhs, rhs, "gttmp"));
        case BinaryOp::Le:     return isFloat ? builder.CreateFCmpOLE(lhs, rhs, "letmp")
                                              : (unsignedOp ? builder.CreateICmpULE(lhs, rhs, "letmp")
                                                            : builder.CreateICmpSLE(lhs, rhs, "letmp"));
        case BinaryOp::Ge:     return isFloat ? builder.CreateFCmpOGE(lhs, rhs, "getmp")
                                              : (unsignedOp ? builder.CreateICmpUGE(lhs, rhs, "getmp")
                                                            : builder.CreateICmpSGE(lhs, rhs, "getmp"));
        case BinaryOp::And:    return builder.CreateAnd(lhs, rhs, "andtmp");
        case BinaryOp::Or:     return builder.CreateOr(lhs, rhs, "ortmp");
        case BinaryOp::BitAnd: return builder.CreateAnd(lhs, rhs, "bitandtmp");
        case BinaryOp::BitOr:  return builder.CreateOr(lhs, rhs, "bitortmp");
        case BinaryOp::BitXor: return builder.CreateXor(lhs, rhs, "bitxortmp");
        case BinaryOp::LShift: return builder.CreateShl(lhs, rhs, "lshifttmp");
        case BinaryOp::RShift: return unsignedOp ? builder.CreateLShr(lhs, rhs, "rshifttmp")
                                                 : builder.CreateAShr(lhs, rhs, "rshifttmp");
        default:
            LOGE("invalid binary operator");
            return nullptr;
    }
}

llvm::Value* UnaryExprAST::codegen(CodegenContext& ctx) {
    if (auto* ct = ctConstantOrNull(ctx, *this)) return ct;
    // SEM-17: overloaded unary operator — emit a call to the resolved function.
    if (!mangledCallee.empty()) {
        llvm::Value* arg = operand->codegen(ctx);
        if (!arg) return nullptr;
        if (isMethodOperator) {
            llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
            if (!calleeFn) {
                llvm::Type* retLLVM = type ? ctx.getLLVMType(type)
                                           : llvm::Type::getVoidTy(ctx.getContext());
                std::vector<llvm::Type*> paramLLVM;
                for (auto* pt : resolvedParamTypes) paramLLVM.push_back(ctx.getLLVMType(pt));
                auto* fnType = llvm::FunctionType::get(retLLVM, paramLLVM, false);
                calleeFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                                  mangledCallee, ctx.getModule());
            }
            return ctx.getBuilder().CreateCall(calleeFn, {arg}, "umopcalltmp");
        }
        llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
        if (!calleeFn) {
            LOGE("unknown unary operator function: {}", mangledCallee);
            return nullptr;
        }
        arg = emitRValue(ctx, *operand, arg);
        return ctx.getBuilder().CreateCall(calleeFn, {arg}, "uopcalltmp");
    }
    llvm::Value* v = operand->codegen(ctx);
    if (!v) return nullptr;

    auto& builder = ctx.getBuilder();

    switch (op) {
        case UnaryOp::Plus:      return emitRValue(ctx, *operand, v);
        case UnaryOp::Minus: {
            llvm::Value* operandVal = emitRValue(ctx, *operand, v);
            // Floating-point negation must use fneg; integer CreateNeg would
            // expand to `0 - x`, which is not selectable for constants here.
            if (operandVal->getType()->isFloatingPointTy()) {
                return builder.CreateFNeg(operandVal, "negtmp");
            }
            return builder.CreateNeg(operandVal, "negtmp");
        }
        case UnaryOp::Not: {
            // Logical negation: normalize to i1 first, so `!x` is not bitwise
            // complement and `!p` (pointer) is well-typed.
            llvm::Value* operandVal = emitRValue(ctx, *operand, v);
            return builder.CreateNot(ctx.coerceToBool(operandVal, operand->type), "nottmp");
        }
        case UnaryOp::BitNot:    return builder.CreateNot(emitRValue(ctx, *operand, v), "bitnottmp");
        case UnaryOp::Deref: {
            // `*p` denotes the pointee as an lvalue: yield its address and let
            // consumers load it, mirroring how variable references work.
            llvm::Value* ptrVal = emitRValue(ctx, *operand, v);
            Type* opType = stripTypedefsT12(operand->type);
            if (opType && opType->kind == TypeKind::Pointer) {
                return ptrVal;
            }
            // Fallback when the operand type is unknown/lowering cannot tell.
            llvm::Type* pointeeType = ctx.getLLVMType(opType ? opType->base : nullptr);
            if (!pointeeType) pointeeType = llvm::Type::getInt8Ty(ctx.getContext());
            return builder.CreateLoad(pointeeType, ptrVal, "dereftmp");
        }
        case UnaryOp::PreInc:
        case UnaryOp::PreDec: {
            // P1-07 / CG-09: bit-field operand — RMW: read → ±1 → write back.
            if (operand->isBitFieldAccess()) {
                auto* ma = static_cast<MemberAccessExprAST*>(operand.get());
                Type* ft = stripTypedefsT12(operand->type);
                llvm::Value* oldVal = ctx.loadBitFieldValue(v, ma->bitOffset, ma->bitWidth, ft, ma->unitBits);
                llvm::Value* one = llvm::ConstantInt::get(oldVal->getType(), 1);
                llvm::Value* newVal = (op == UnaryOp::PreInc)
                    ? builder.CreateAdd(oldVal, one, "preinc")
                    : builder.CreateSub(oldVal, one, "predec");
                ctx.storeBitFieldValue(v, ma->bitOffset, ma->bitWidth, ft, newVal, ma->unitBits);
                return newVal;
            }
            // CON-01: atomic operand — atomicrmw add/sub 1 (seq_cst).
            if (operand->type && stripTypedefsT12(operand->type)->isAtomic) {
                Type* ot = stripTypedefsT12(operand->type);
                unsigned bits = 0;
                llvm::Type* carrier = ctx.getAtomicCarrierType(ot, bits);
                if (carrier) {
                    llvm::Value* one = llvm::ConstantInt::get(carrier, 1);
                    llvm::Value* rmw = builder.CreateAtomicRMW(
                        op == UnaryOp::PreInc ? llvm::AtomicRMWInst::Add
                                              : llvm::AtomicRMWInst::Sub,
                        v, one, llvm::Align(bits / 8),
                        llvm::AtomicOrdering::SequentiallyConsistent);
                    llvm::Value* next =
                        op == UnaryOp::PreInc
                            ? builder.CreateAdd(rmw, one, "preinc")
                            : builder.CreateSub(rmw, one, "predec");
                    return ctx.valueFromAtomicCarrier(next, ot, carrier);
                }
            }
            // The operand is an lvalue; update it in place and yield the new
            // value.
            llvm::Type* loadType = operand->type
                ? ctx.getLLVMType(operand->type)
                : llvm::Type::getInt32Ty(ctx.getContext());
            if (!loadType) loadType = llvm::Type::getInt32Ty(ctx.getContext());
            llvm::Value* oldVal = builder.CreateLoad(loadType, v, "preold");

            llvm::Value* newVal = nullptr;
            if (loadType->isPointerTy()) {
                llvm::Type* elemTy = (operand->type && operand->type->base)
                    ? ctx.getLLVMType(operand->type->base)
                    : llvm::Type::getInt8Ty(ctx.getContext());
                if (!elemTy) elemTy = llvm::Type::getInt8Ty(ctx.getContext());
                llvm::Value* step = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(ctx.getContext()),
                    op == UnaryOp::PreInc ? 1 : -1);
                newVal = builder.CreateGEP(elemTy, oldVal, step, "preptr");
            } else {
                llvm::Value* one = llvm::ConstantInt::get(loadType, 1);
                newVal = (op == UnaryOp::PreInc)
                    ? builder.CreateAdd(oldVal, one, "preinc")
                    : builder.CreateSub(oldVal, one, "predec");
            }
            builder.CreateStore(newVal, v);
            return newVal;
        }
        case UnaryOp::AddressOf: return v;
        default:
            LOGE("invalid unary operator");
            return nullptr;
    }
}

// Emit `dprintf(2, fmt, extra...)` then `abort()` (STD-01 / STD-27 / DEC-21).
// `fmt` points at a fixed format string built at compile time (never user data,
// so `%` in a path or message cannot corrupt the call); `extra` are varargs.
static void emitDprintfAndAbort(CodegenContext& ctx, llvm::Value* fmt,
                                const std::vector<llvm::Value*>& extra) {
    llvm::LLVMContext& c = ctx.getContext();
    auto& builder = ctx.getBuilder();
    llvm::Type* i32 = llvm::Type::getInt32Ty(c);
    llvm::Type* ptr = llvm::PointerType::get(c, 0);

    llvm::FunctionType* dprintfTy = llvm::FunctionType::get(i32, {i32, ptr}, true);
    llvm::FunctionCallee dprintfFn = ctx.getModule().getOrInsertFunction("dprintf", dprintfTy);

    std::vector<llvm::Value*> callArgs;
    callArgs.push_back(llvm::ConstantInt::get(i32, 2)); // fd 2 = stderr
    callArgs.push_back(fmt);
    callArgs.insert(callArgs.end(), extra.begin(), extra.end());
    builder.CreateCall(dprintfTy, dprintfFn.getCallee(), callArgs);

    llvm::FunctionType* abortTy = llvm::FunctionType::get(llvm::Type::getVoidTy(c), false);
    llvm::FunctionCallee abortFn = ctx.getModule().getOrInsertFunction("abort", abortTy);
    builder.CreateCall(abortTy, abortFn.getCallee(), {});
}

// "<file>:<line>" for the current node, or "" when unknown.
static std::string nodeSourcePrefix(const ASTNode& node) {
    if (node.sourceFile.empty()) return "";
    return node.sourceFile + ":" + std::to_string(node.sourceLine);
}

static llvm::AtomicOrdering toLLVMAtomicOrder(int order) {
    switch (order) {
        case 0: return llvm::AtomicOrdering::Monotonic;
        case 1: return llvm::AtomicOrdering::Acquire; // consume -> acquire
        case 2: return llvm::AtomicOrdering::Acquire;
        case 3: return llvm::AtomicOrdering::Release;
        case 4: return llvm::AtomicOrdering::AcquireRelease;
        default: return llvm::AtomicOrdering::SequentiallyConsistent;
    }
}

// Lower one internal __smc_atomic_* call. Sema has already checked arity,
// pointer/value types and memory-order constants.
static llvm::Value* emitAtomicBuiltin(CodegenContext& ctx, CallExprAST& node) {
    auto& builder = ctx.getBuilder();
    auto evalArg = [&](size_t index) -> llvm::Value* {
        llvm::Value* v = node.args[index]->codegen(ctx);
        if (!v) return nullptr;
        return emitRValue(ctx, *node.args[index], v);
    };

    if (node.atomicBuiltin == AtomicBuiltin::ThreadFence ||
        node.atomicBuiltin == AtomicBuiltin::SignalFence) {
        llvm::AtomicOrdering order = toLLVMAtomicOrder(node.atomicOrder);
        if (node.atomicBuiltin == AtomicBuiltin::SignalFence) {
            builder.CreateFence(order, llvm::SyncScope::SingleThread);
        } else {
            builder.CreateFence(order);
        }
        return nullptr;
    }

    Type* pointerType = node.args[0]->type;
    while (pointerType && pointerType->kind == TypeKind::Typedef) {
        pointerType = static_cast<TypedefType*>(pointerType)->aliasedType;
    }
    Type* valueType =
        pointerType && pointerType->kind == TypeKind::Pointer
            ? pointerType->base
            : nullptr;
    while (valueType && valueType->kind == TypeKind::Typedef) {
        valueType = static_cast<TypedefType*>(valueType)->aliasedType;
    }
    unsigned bits = 0;
    llvm::Type* carrier = ctx.getAtomicCarrierType(valueType, bits);
    if (!valueType || !carrier) return nullptr;
    llvm::Align align(bits / 8);
    llvm::AtomicOrdering order = toLLVMAtomicOrder(node.atomicOrder);

    llvm::Value* ptr = evalArg(0);
    if (!ptr) return nullptr;

    auto castToValueType = [&](llvm::Value* v, size_t argIndex) {
        if (!v) return v;
        return ctx.castValue(v, node.args[argIndex]->type,
                             ctx.getLLVMType(valueType));
    };

    switch (node.atomicBuiltin) {
        case AtomicBuiltin::Load: {
            llvm::LoadInst* load = builder.CreateLoad(carrier, ptr, "atomic.load");
            load->setAtomic(order);
            load->setAlignment(align);
            return ctx.valueFromAtomicCarrier(load, valueType, carrier);
        }
        case AtomicBuiltin::Store: {
            llvm::Value* value = castToValueType(evalArg(1), 1);
            if (!value) return nullptr;
            llvm::Value* carrierValue =
                ctx.valueToAtomicCarrier(value, valueType, carrier);
            llvm::StoreInst* store =
                builder.CreateStore(carrierValue, ptr);
            store->setAtomic(order);
            store->setAlignment(align);
            return nullptr;
        }
        case AtomicBuiltin::Exchange: {
            llvm::Value* value = castToValueType(evalArg(1), 1);
            if (!value) return nullptr;
            llvm::Value* carrierValue =
                ctx.valueToAtomicCarrier(value, valueType, carrier);
            llvm::Value* old = builder.CreateAtomicRMW(
                llvm::AtomicRMWInst::Xchg, ptr, carrierValue,
                llvm::MaybeAlign(bits / 8), order);
            return ctx.valueFromAtomicCarrier(old, valueType, carrier);
        }
        case AtomicBuiltin::CompareExchangeWeak:
        case AtomicBuiltin::CompareExchangeStrong: {
            llvm::Value* expectedPtr = evalArg(1);
            llvm::Value* expectedValue =
                expectedPtr ? ctx.loadValue(expectedPtr, valueType) : nullptr;
            llvm::Value* desired = castToValueType(evalArg(2), 2);
            if (!expectedPtr || !expectedValue || !desired) return nullptr;
            llvm::Value* expectedCarrier =
                ctx.valueToAtomicCarrier(expectedValue, valueType, carrier);
            llvm::Value* desiredCarrier =
                ctx.valueToAtomicCarrier(desired, valueType, carrier);
            llvm::AtomicOrdering success = toLLVMAtomicOrder(node.atomicOrder);
            llvm::AtomicOrdering failure =
                toLLVMAtomicOrder(node.atomicFailureOrder);
            llvm::Value* result = builder.CreateAtomicCmpXchg(
                ptr, expectedCarrier, desiredCarrier,
                llvm::MaybeAlign(bits / 8), success, failure);
            llvm::Value* old =
                builder.CreateExtractValue(result, 0, "atomic.cas.old");
            llvm::Value* ok =
                builder.CreateExtractValue(result, 1, "atomic.cas.ok");

            llvm::Function* fn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* failBB =
                llvm::BasicBlock::Create(ctx.getContext(), "atomic.cas.fail", fn);
            llvm::BasicBlock* contBB =
                llvm::BasicBlock::Create(ctx.getContext(), "atomic.cas.cont", fn);
            builder.CreateCondBr(ok, contBB, failBB);
            builder.SetInsertPoint(failBB);
            llvm::Value* oldValue =
                ctx.valueFromAtomicCarrier(old, valueType, carrier);
            builder.CreateStore(oldValue, expectedPtr);
            builder.CreateBr(contBB);
            builder.SetInsertPoint(contBB);
            return ok;
        }
        case AtomicBuiltin::FetchAdd:
        case AtomicBuiltin::FetchSub:
        case AtomicBuiltin::FetchAnd:
        case AtomicBuiltin::FetchOr:
        case AtomicBuiltin::FetchXor: {
            bool isPointer = valueType->kind == TypeKind::Pointer;
            llvm::Value* operand = nullptr;
            if (isPointer) {
                llvm::Value* delta = evalArg(1);
                if (!delta) return nullptr;
                delta = ctx.castValue(delta, node.args[1]->type,
                                      llvm::Type::getInt64Ty(ctx.getContext()));
                unsigned elemSize = ctx.getPointeeByteSize(valueType);
                operand = builder.CreateMul(
                    delta, llvm::ConstantInt::get(
                               llvm::Type::getInt64Ty(ctx.getContext()),
                               elemSize),
                    "atomic.ptr.step");
            } else {
                llvm::Value* value = castToValueType(evalArg(1), 1);
                if (!value) return nullptr;
                operand = ctx.valueToAtomicCarrier(value, valueType, carrier);
            }
            llvm::AtomicRMWInst::BinOp op = llvm::AtomicRMWInst::Add;
            switch (node.atomicBuiltin) {
                case AtomicBuiltin::FetchAdd: op = llvm::AtomicRMWInst::Add; break;
                case AtomicBuiltin::FetchSub: op = llvm::AtomicRMWInst::Sub; break;
                case AtomicBuiltin::FetchAnd: op = llvm::AtomicRMWInst::And; break;
                case AtomicBuiltin::FetchOr:  op = llvm::AtomicRMWInst::Or; break;
                default:                      op = llvm::AtomicRMWInst::Xor; break;
            }
            llvm::Value* old = builder.CreateAtomicRMW(
                op, ptr, operand, llvm::MaybeAlign(bits / 8), order);
            return ctx.valueFromAtomicCarrier(old, valueType, carrier);
        }
        default:
            return nullptr;
    }
}

llvm::Value* CallExprAST::codegen(CodegenContext& ctx) {
    if (atomicBuiltin != AtomicBuiltin::None) {
        return emitAtomicBuiltin(ctx, *this);
    }
    // General expression callee: evaluate the function pointer expression.
    if (isExprCallee) {
        llvm::Value* fnPtr = calleeExpr ? calleeExpr->codegen(ctx) : nullptr;
        if (!fnPtr) return nullptr;
        if (calleeExpr->isLValue) fnPtr = readLValue(ctx, *calleeExpr, fnPtr);
        std::vector<llvm::Value*> callArgs;
        for (size_t i = 0; i < args.size(); ++i) {
            llvm::Value* v = args[i]->codegen(ctx);
            if (!v) return nullptr;
            if (args[i]->isLValue) v = readLValue(ctx, *args[i], v);
            if (i < resolvedParamTypes.size())
                v = ctx.castValue(v, args[i]->type, ctx.getLLVMType(resolvedParamTypes[i]));
            callArgs.push_back(v);
        }
        std::vector<llvm::Type*> paramLLVMTypes;
        for (auto* t : resolvedParamTypes) paramLLVMTypes.push_back(ctx.getLLVMType(t));
        llvm::Type* retLLVM = type ? ctx.getLLVMType(type)
                                   : llvm::Type::getVoidTy(ctx.getContext());
        auto* fnType = llvm::FunctionType::get(retLLVM, paramLLVMTypes, false);
        if (retLLVM->isVoidTy())
            return ctx.getBuilder().CreateCall(fnType, fnPtr, callArgs);
        return ctx.getBuilder().CreateCall(fnType, fnPtr, callArgs, "icalltmp");
    }
    // Review 2.6: captured-lambda closure call — pass the closure address as
    // the hidden first argument.
    if (isClosureCall) {
        llvm::Value* closureAddr = ctx.lookupVariableAddr(callee);
        if (!closureAddr) return nullptr;
        llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
        if (!calleeFn) return nullptr;
        std::vector<llvm::Value*> callArgs{closureAddr};
        for (size_t i = 0; i < args.size(); ++i) {
            llvm::Value* v = args[i]->codegen(ctx);
            if (!v) return nullptr;
            if (args[i]->isLValue) v = readLValue(ctx, *args[i], v);
            if (i < resolvedParamTypes.size())
                v = ctx.castValue(v, args[i]->type,
                                  ctx.getLLVMType(resolvedParamTypes[i]));
            callArgs.push_back(v);
        }
        if (type && type->kind != TypeKind::Void)
            return ctx.getBuilder().CreateCall(calleeFn, callArgs, "closuretmp");
        ctx.getBuilder().CreateCall(calleeFn, callArgs);
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.getContext()), 0);
    }

    // SEM-17: `obj(args)` through an overloaded `operator()` — call with the
    // object as the first argument.
    if (calleeExpr) {
        llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
        if (!calleeFn) {
            LOGE("unknown operator() function: {}", mangledCallee);
            return nullptr;
        }
        std::vector<llvm::Value*> callArgs;
        llvm::Value* obj = calleeExpr->codegen(ctx);
        if (!obj) return nullptr;
        if (!memberOperator) obj = emitRValue(ctx, *calleeExpr, obj);
        callArgs.push_back(obj);
        for (auto& arg : args) {
            llvm::Value* v = arg->codegen(ctx);
            if (!v) return nullptr;
            callArgs.push_back(emitRValue(ctx, *arg, v));
        }
        return ctx.getBuilder().CreateCall(calleeFn, callArgs, "opcalltmp");
    }
    // P1-06 (FMT-04): builtin str_from_c — strlen + {ptr, len} assembly.
    if (isStrFromC) {
        return codegenStrFromC(ctx, *this);
    }
    // Builtin `assert(cond)`: on a false condition, report the call site and
    // abort; on success, fall through. Never silently recovers.
    if (isAssert) {
        llvm::LLVMContext& c = ctx.getContext();
        auto& builder = ctx.getBuilder();
        llvm::Value* cond = args[0]->codegen(ctx);
        if (!cond) return nullptr;
        if (args[0]->isLValue) cond = readLValue(ctx, *args[0], cond);
        cond = ctx.coerceToBool(cond, args[0]->type);
        if (!cond) return nullptr;

        llvm::Function* fn = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(c, "assert.fail", fn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "assert.ok", fn);
        builder.CreateCondBr(cond, okBB, failBB);

        builder.SetInsertPoint(failBB);
        std::string prefix = nodeSourcePrefix(*this);
        std::string msg = prefix.empty() ? "assertion failed\n"
                                         : prefix + ": assertion failed\n";
        llvm::Value* fmt = builder.CreateGlobalString("%s", ".assertfmt");
        llvm::Value* text = builder.CreateGlobalString(msg, ".assertmsg");
        emitDprintfAndAbort(ctx, fmt, {text});
        builder.CreateUnreachable();

        builder.SetInsertPoint(okBB);
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(c), 0);
    }

    // Builtin `panic(msg)`: report the call site and the user message, then
    // abort. The block is terminated; a dead continuation keeps later codegen
    // total (and is unreachable).
    if (isPanic) {
        llvm::LLVMContext& c = ctx.getContext();
        auto& builder = ctx.getBuilder();
        llvm::Value* userMsg = args[0]->codegen(ctx);
        if (!userMsg) return nullptr;
        if (args[0]->isLValue) userMsg = readLValue(ctx, *args[0], userMsg);
        if (!userMsg) return nullptr;
        // P1-06 (FMT-03): a str message is viewed as its char* bytes.
        if (args[0]->type && args[0]->type->kind == TypeKind::Str) {
            userMsg = ctx.castValue(userMsg, args[0]->type,
                                    llvm::PointerType::get(c, 0));
        } else if (!userMsg->getType()->isPointerTy()) {
            userMsg = ctx.castValue(userMsg, llvm::PointerType::get(c, 0));
        }

        llvm::Function* fn = builder.GetInsertBlock()->getParent();
        std::string prefix = nodeSourcePrefix(*this);
        std::string fmtStr = prefix.empty() ? "panic: %s\n" : prefix + ": panic: %s\n";
        llvm::Value* fmt = builder.CreateGlobalString(fmtStr, ".panicfmt");
        emitDprintfAndAbort(ctx, fmt, {userMsg});
        builder.CreateUnreachable();

        llvm::BasicBlock* contBB = llvm::BasicBlock::Create(c, "panic.cont", fn);
        builder.SetInsertPoint(contBB);
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(c), 0);
    }

    // Builtin `print`/`println`: emit a call to the C library's printf with a
    // compile-time-built conversion string.
    if (isPrint) {
        llvm::LLVMContext& c = ctx.getContext();
        auto& builder = ctx.getBuilder();

        llvm::Value* format = builder.CreateGlobalString(printCFormat, ".printfmt");
        llvm::FunctionType* printfTy = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(c), {llvm::PointerType::get(c, 0)}, true);
        llvm::FunctionCallee printfFn = ctx.getModule().getOrInsertFunction("printf", printfTy);

        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(format);
        for (size_t k = 1; k < args.size(); ++k) {
            llvm::Value* v = args[k]->codegen(ctx);
            if (!v) return nullptr;
            if (args[k]->isLValue) v = readLValue(ctx, *args[k], v);
            if (k - 1 < printArgKinds.size()) {
                // P1-09 (FMT-08): specs printf cannot express (centering or a
                // custom fill) pre-render the value into a padded buffer.
                const bool renderSlot =
                    k - 1 < printSpecs.size() &&
                    (printSpecs[k - 1].align == '^' ||
                     (printSpecs[k - 1].fill != ' ' && printSpecs[k - 1].fill != '\0'));
                if (renderSlot) {
                    llvm::Value* padded = emitPaddedValue(
                        ctx, printSpecs[k - 1], printArgKinds[k - 1], v);
                    if (!padded) return nullptr;
                    callArgs.push_back(padded);
                    continue;
                }
                // P1-06 (FMT-03): str prints via `%.*s` — TWO printf
                // arguments ((int)len, ptr); push both and skip the loop's
                // single-value promote.
                if (printArgKinds[k - 1] == PrintArgKind::Str) {
                    llvm::Value* len = builder.CreateExtractValue(v, 1, "printfmt.len");
                    llvm::Value* ptr = builder.CreateExtractValue(v, 0, "printfmt.ptr");
                    callArgs.push_back(builder.CreateTrunc(
                        len, llvm::Type::getInt32Ty(c), "printfmt.len32"));
                    callArgs.push_back(ptr);
                    continue;
                }
                v = promotePrintArg(ctx, v, printArgKinds[k - 1]);
            }
            if (!v) return nullptr;
            callArgs.push_back(v);
        }
        return builder.CreateCall(printfTy, printfFn.getCallee(), callArgs);
    }

    // P1-09 (FMT-07/12): builtin `format(fmt, args...) -> string`. Literal
    // formats assemble straight-line (global segments + snprintf/padded slot
    // buffers, one malloc, no realloc); dynamic formats render each argument
    // with its default conversion into chunks and let smc.format.dyn scan.
    if (isFormat) {
        llvm::LLVMContext& c = ctx.getContext();
        auto& builder = ctx.getBuilder();
        auto* i8Ty = llvm::Type::getInt8Ty(c);
        auto* i32Ty = llvm::Type::getInt32Ty(c);
        auto* i64Ty = llvm::Type::getInt64Ty(c);
        auto* ptrTy = llvm::PointerType::get(c, 0);
        auto* strLLVM = ctx.getLLVMType(TypeContext::instance().getStringType());

        auto mallocFn = ctx.getModule().getOrInsertFunction(
            "malloc", llvm::FunctionType::get(ptrTy, {i64Ty}, false));
        auto memcpyFn = ctx.getModule().getOrInsertFunction(
            "memcpy",
            llvm::FunctionType::get(llvm::Type::getVoidTy(c),
                                    {ptrTy, ptrTy, i64Ty}, false));
        auto snprintfFn = ctx.getModule().getOrInsertFunction(
            "snprintf",
            llvm::FunctionType::get(i32Ty, {ptrTy, i64Ty, ptrTy}, true));

        // Render one scalar with `conv` into a stack buffer -> {buf, len}.
        auto renderScalar = [&](llvm::Value* v, PrintArgKind kind,
                                const std::string& conv,
                                int bufSize) -> std::pair<llvm::Value*, llvm::Value*> {
            llvm::Value* promoted = promotePrintArg(ctx, v, kind);
            // NUL-terminated char* classes (bool/to_string/char*) chunk
            // directly by strlen — no fixed-buffer truncation, no copy.
            // (Only the plain "%s" conversion; width/alignment variants go
            // through snprintf with the clamp below.)
            if (conv == "%s" && promoted->getType()->isPointerTy()) {
                auto strlenFn = ctx.getModule().getOrInsertFunction(
                    "strlen", llvm::FunctionType::get(i64Ty, {ptrTy}, false));
                llvm::Value* slen =
                    builder.CreateCall(strlenFn, {promoted}, "fmt.slen");
                return {promoted, slen};
            }
            llvm::Value* buf = builder.CreateAlloca(
                i8Ty, llvm::ConstantInt::get(i32Ty, bufSize), "fmt.buf");
            llvm::Value* cv = builder.CreateGlobalString(conv, ".fmt.cv");
            llvm::Value* ret = builder.CreateCall(
                snprintfFn.getFunctionType(), snprintfFn.getCallee(),
                {buf, llvm::ConstantInt::get(i64Ty, bufSize), cv, promoted});
            // Clamp: snprintf returns the WOULD-BE length on truncation; the
            // chunk length must never exceed the buffer it read from.
            llvm::Value* len = builder.CreateSExt(ret, i64Ty, "fmt.len");
            len = builder.CreateSelect(
                builder.CreateICmpSGT(
                    len, llvm::ConstantInt::get(i64Ty, bufSize - 1)),
                llvm::ConstantInt::get(i64Ty, bufSize - 1), len, "fmt.len.clamp");
            return {buf, len};
        };
        auto defaultConvText = [](PrintArgKind kind) {
            switch (kind) {
                case PrintArgKind::Int32:   return "d";
                case PrintArgKind::Int64:   return "lld";
                case PrintArgKind::UInt32:  return "u";
                case PrintArgKind::UInt64:  return "llu";
                case PrintArgKind::Char:    return "c";
                case PrintArgKind::Float:   return "g";
                case PrintArgKind::Pointer: return "p";
                default:                    return "s";
            }
        };

        if (!formatLiteral.empty()) {
            std::vector<std::pair<llvm::Value*, llvm::Value*>> chunks;
            std::string lit;
            size_t argK = 1; // args[0] is the format string itself
            size_t slotIdx = 0;
            auto flushLit = [&]() {
                if (lit.empty()) return;
                llvm::Value* g = builder.CreateGlobalString(lit, ".fmt.seg");
                chunks.emplace_back(g, llvm::ConstantInt::get(i64Ty, lit.size()));
                lit.clear();
            };
            for (size_t i = 0; i < formatLiteral.size();) {
                const char ch = formatLiteral[i];
                if (ch == '{') {
                    if (i + 1 < formatLiteral.size() && formatLiteral[i + 1] == '{') {
                        lit += '{';
                        i += 2;
                        continue;
                    }
                    ++i; // consume '{'
                    if (i < formatLiteral.size() && formatLiteral[i] == ':') {
                        ++i;
                        while (i < formatLiteral.size() && formatLiteral[i] != '}') ++i;
                    }
                    while (i < formatLiteral.size() && formatLiteral[i] != '}') ++i;
                    ++i; // consume '}'
                    flushLit();
                    if (argK >= args.size() || slotIdx >= printArgKinds.size()) break;
                    llvm::Value* v = args[argK]->codegen(ctx);
                    if (!v) return nullptr;
                    if (args[argK]->isLValue) v = readLValue(ctx, *args[argK], v);
                    const PrintArgKind kind = printArgKinds[slotIdx];
                    const PrintSpec& spec = slotIdx < formatSpecs.size()
                                                ? formatSpecs[slotIdx]
                                                : PrintSpec{};
                    const bool renderSlot =
                        spec.align == '^' ||
                        (spec.fill != ' ' && spec.fill != '\0');
                    if (renderSlot) {
                        llvm::Value* len = nullptr;
                        llvm::Value* padded =
                            emitPaddedValue(ctx, spec, kind, v, &len);
                        if (!padded) return nullptr;
                        chunks.emplace_back(padded, len);
                    } else if (kind == PrintArgKind::Str) {
                        // Zero-copy byte fidelity (NUL-safe): the chunk IS
                        // the argument's bytes.
                        chunks.emplace_back(
                            builder.CreateExtractValue(v, 0, "fmt.sptr"),
                            builder.CreateExtractValue(v, 1, "fmt.slen"));
                    } else {
                        std::string conv, err;
                        bool needsRender = false;
                        if (!specToPrintfConversion(kind, spec, conv,
                                                    needsRender, err)) {
                            conv = "%s";
                        }
                        const int bufSize = 344 + spec.width +
                                            (spec.precision > 0 ? spec.precision : 0);
                        auto rendered = renderScalar(v, kind, conv, bufSize);
                        chunks.emplace_back(rendered.first, rendered.second);
                    }
                    ++argK;
                    ++slotIdx;
                } else if (ch == '}') {
                    if (i + 1 < formatLiteral.size() && formatLiteral[i + 1] == '}') {
                        lit += '}';
                        i += 2;
                        continue;
                    }
                    lit += '}';
                    ++i;
                } else {
                    lit += ch;
                    ++i;
                }
            }
            flushLit();

            llvm::Value* total = llvm::ConstantInt::get(i64Ty, 0);
            for (auto& chunk : chunks)
                total = builder.CreateAdd(total, chunk.second, "fmt.t");
            llvm::Value* total1 = builder.CreateAdd(
                total, llvm::ConstantInt::get(i64Ty, 1), "fmt.total1");
            llvm::Value* cap = builder.CreateSelect(
                builder.CreateICmpUGT(total1, llvm::ConstantInt::get(i64Ty, 64)),
                total1, llvm::ConstantInt::get(i64Ty, 64), "fmt.cap");
            llvm::Value* start = builder.CreateCall(mallocFn, {cap}, "fmt.ptr");
            // NOTE: never chain on memcpy's return — LLVM lowers calls to the
            // memcpy libfunc to an intrinsic DAG node whose return value is
            // not guaranteed. Advance the destination by explicit offsets.
            llvm::Value* off = llvm::ConstantInt::get(i64Ty, 0);
            for (auto& chunk : chunks) {
                llvm::Value* dst =
                    builder.CreateGEP(i8Ty, start, off, "fmt.dst");
                builder.CreateCall(memcpyFn, {dst, chunk.first, chunk.second});
                off = builder.CreateAdd(off, chunk.second, "fmt.off");
            }
            llvm::Value* end = builder.CreateGEP(i8Ty, start, total, "fmt.end");
            builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0), end);
            llvm::Value* out = llvm::Constant::getNullValue(strLLVM);
            out = builder.CreateInsertValue(out, start, {0});
            out = builder.CreateInsertValue(out, total, {1});
            return builder.CreateInsertValue(out, cap, {2});
        }

        // Dynamic format string: pre-render every argument with its default
        // conversion, then let smc.format.dyn interleave (FMT-09 contract).
        llvm::Value* fmtV = args[0]->codegen(ctx);
        if (!fmtV) return nullptr;
        if (args[0]->isLValue) fmtV = readLValue(ctx, *args[0], fmtV);
        llvm::Value* fptr = builder.CreateExtractValue(fmtV, 0, "fmt.fptr");
        llvm::Value* flen = builder.CreateExtractValue(fmtV, 1, "fmt.flen");

        const size_t n = args.size() - 1;
        auto* arrLen = llvm::ConstantInt::get(i32Ty, n > 0 ? n : 1);
        auto* ptrArr = builder.CreateAlloca(ptrTy, arrLen, "fmt.ptrs");
        auto* lenArr = builder.CreateAlloca(i64Ty, arrLen, "fmt.lens");
        for (size_t k = 1; k < args.size(); ++k) {
            const PrintArgKind kind = k - 1 < printArgKinds.size()
                                          ? printArgKinds[k - 1]
                                          : PrintArgKind::Int32;
            llvm::Value* v = args[k]->codegen(ctx);
            if (!v) return nullptr;
            if (args[k]->isLValue) v = readLValue(ctx, *args[k], v);
            llvm::Value* cptr;
            llvm::Value* clen;
            if (kind == PrintArgKind::Str) {
                cptr = builder.CreateExtractValue(v, 0, "fmt.cptr");
                clen = builder.CreateExtractValue(v, 1, "fmt.clen");
            } else {
                auto rendered = renderScalar(
                    v, kind, std::string("%") + defaultConvText(kind), 64);
                cptr = rendered.first;
                clen = rendered.second;
            }
            builder.CreateStore(cptr, builder.CreateGEP(ptrTy, ptrArr,
                llvm::ConstantInt::get(i64Ty, k - 1), "fmt.pp"));
            builder.CreateStore(clen, builder.CreateGEP(i64Ty, lenArr,
                llvm::ConstantInt::get(i64Ty, k - 1), "fmt.lp"));
        }
        auto savedIP = builder.saveIP();
        llvm::Function* dynFn = ctx.getFormatDynFn();
        builder.restoreIP(savedIP);
        llvm::Value* res = builder.CreateCall(
            dynFn, {fptr, flen, ptrArr, lenArr,
                    llvm::ConstantInt::get(i64Ty, n)});
        llvm::Value* out = llvm::Constant::getNullValue(strLLVM);
        out = builder.CreateInsertValue(
            out, builder.CreateExtractValue(res, 0, "fmt.rptr"), {0});
        out = builder.CreateInsertValue(
            out, builder.CreateExtractValue(res, 1, "fmt.rlen"), {1});
        return builder.CreateInsertValue(
            out, builder.CreateExtractValue(res, 2, "fmt.rcap"), {2});
    }

    // P1-09 (STD-10): builtin `split(s, sep) -> []str` — malloc'd str-view
    // array + {ptr, count} slice. A dynamic empty separator panics here (the
    // literal case is rejected at sema).
    if (isSplit || isSplitDestroy) {
        llvm::LLVMContext& c = ctx.getContext();
        auto& builder = ctx.getBuilder();
        auto* i64Ty = llvm::Type::getInt64Ty(c);
        auto* ptrTy = llvm::PointerType::get(c, 0);

        if (isSplitDestroy) {
            auto freeFn = ctx.getModule().getOrInsertFunction(
                "free", llvm::FunctionType::get(llvm::Type::getVoidTy(c),
                                                {ptrTy}, false));
            llvm::Value* v = args[0]->codegen(ctx);
            if (!v) return nullptr;
            if (args[0]->isLValue) v = readLValue(ctx, *args[0], v);
            llvm::Value* arr = builder.CreateExtractValue(v, 0, "split.arr");
            return builder.CreateCall(freeFn, {arr});
        }

        auto freeFn = ctx.getModule().getOrInsertFunction(
            "free", llvm::FunctionType::get(llvm::Type::getVoidTy(c),
                                            {ptrTy}, false));
        llvm::Value* vals[2];
        for (int k = 0; k < 2; ++k) {
            llvm::Value* v = args[k]->codegen(ctx);
            if (!v) return nullptr;
            if (args[k]->isLValue) v = readLValue(ctx, *args[k], v);
            vals[k] = v;
        }
        // Both str and string carry {ptr, len} in fields 0/1.
        llvm::Value* sPtr = builder.CreateExtractValue(vals[0], 0, "split.sptr");
        llvm::Value* sLen = builder.CreateExtractValue(vals[0], 1, "split.slen");
        llvm::Value* sepPtr = builder.CreateExtractValue(vals[1], 0, "split.seppt");
        llvm::Value* sepLen = builder.CreateExtractValue(vals[1], 1, "split.sepln");

        llvm::Function* fn = builder.GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "split.ok", fn);
        llvm::BasicBlock* badBB = llvm::BasicBlock::Create(c, "split.bad", fn);
        auto* empty = builder.CreateICmpEQ(
            sepLen, llvm::ConstantInt::get(i64Ty, 0), "split.sep.empty");
        builder.CreateCondBr(empty, badBB, okBB);
        builder.SetInsertPoint(badBB);
        {
            std::string prefix = nodeSourcePrefix(*this);
            std::string fmtStr = prefix.empty()
                ? "panic: split: empty separator\n"
                : prefix + ": panic: split: empty separator\n";
            llvm::Value* fmt = builder.CreateGlobalString(fmtStr, ".splitfmt");
            emitDprintfAndAbort(ctx, fmt, {});
            builder.CreateUnreachable();
        }

        builder.SetInsertPoint(okBB);
        auto savedIP = builder.saveIP();
        llvm::Function* splitHelper = ctx.getSplitFn();
        builder.restoreIP(savedIP);
        auto* cntAddr = builder.CreateAlloca(i64Ty, nullptr, "split.cnt");
        llvm::Value* arr =
            builder.CreateCall(splitHelper, {sPtr, sLen, sepPtr, sepLen, cntAddr},
                               "split.arr");
        llvm::Value* cnt = builder.CreateLoad(i64Ty, cntAddr, "split.n");
        llvm::Value* out = llvm::Constant::getNullValue(
            ctx.getLLVMType(TypeContext::instance().getSliceType(
                TypeContext::instance().getStrType())));
        out = builder.CreateInsertValue(out, arr, {0});
        return builder.CreateInsertValue(out, cnt, {1});
    }

    // Indirect call through a function-pointer variable.
    if (isIndirect) {
        llvm::Value* fpAddr = ctx.lookupVariableAddr(callee);
        if (!fpAddr) {
            LOGE("unknown function pointer: {}", callee);
            return nullptr;
        }
        llvm::Value* fnPtr = ctx.getBuilder().CreateLoad(
            llvm::PointerType::get(ctx.getContext(), 0), fpAddr, "fnptr");

        std::vector<llvm::Value*> argsV;
        for (size_t i = 0; i < args.size(); ++i) {
            llvm::Value* argVal = args[i]->codegen(ctx);
            if (!argVal) return nullptr;
            if (args[i]->isLValue) argVal = readLValue(ctx, *args[i], argVal);
            if (i < resolvedParamTypes.size()) {
                argVal = ctx.castValue(argVal, args[i]->type,
                                       ctx.getLLVMType(resolvedParamTypes[i]));
            }
            // I4 (D6): string 按值形参（间接调用路径）——活值实参深拷贝。
            if (i < resolvedParamTypes.size() &&
                isStringType(resolvedParamTypes[i]) &&
                !ctx.isFreshStringValue(*args[i])) {
                argVal = emitStringArgCopy(ctx, argVal, resolvedParamTypes[i]);
            }
            argsV.push_back(argVal);
        }

        std::vector<llvm::Type*> paramLLVMTypes;
        for (auto* t : resolvedParamTypes) paramLLVMTypes.push_back(ctx.getLLVMType(t));
        llvm::Type* retLLVM = type ? ctx.getLLVMType(type)
                                   : llvm::Type::getVoidTy(ctx.getContext());
        auto* fnType = llvm::FunctionType::get(retLLVM, paramLLVMTypes, false);
        if (retLLVM->isVoidTy()) {
            return ctx.getBuilder().CreateCall(fnType, fnPtr, argsV);
        }
        return ctx.getBuilder().CreateCall(fnType, fnPtr, argsV, "icalltmp");
    }

    // Prefer the overload chosen by semantic analysis so that implicit
    // conversions (e.g. int8 -> int) resolve to the right symbol.
    std::vector<Type*> argTypes;
    if (!resolvedParamTypes.empty()) {
        argTypes = resolvedParamTypes;
    } else {
        for (auto& arg : args) {
            argTypes.push_back(arg->type);
        }
    }
    std::string mangledName = mangleFunction(callee, argTypes);
    
    llvm::Function* calleeFn = ctx.getModule().getFunction(mangledName);
    if (!calleeFn) {
        // P1-03 / GEN-05: 惰性前向声明——模板实例函数等定义可能在本调用点
        // 之后出码；按调用点已知签名建声明，定义侧复用同名 llvm::Function。
        llvm::Type* retLLVM = type ? ctx.getLLVMType(type) : nullptr;
        if (!retLLVM) {
            LOGE("unknown function: {}", callee);
            return nullptr;
        }
        std::vector<llvm::Type*> paramLLVM;
        for (auto* pt : argTypes) paramLLVM.push_back(ctx.getLLVMType(pt));
        auto* fnType = llvm::FunctionType::get(retLLVM, paramLLVM, false);
        calleeFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                          mangledName, ctx.getModule());
    }

    std::vector<llvm::Value*> argsV;
    for (size_t i = 0; i < args.size(); ++i) {
        llvm::Value* argVal = args[i]->codegen(ctx);
        if (!argVal) return nullptr;
        if (args[i]->isLValue) {
            argVal = readLValue(ctx, *args[i], argVal);
        }
        if (i < resolvedParamTypes.size()) {
            if (isArrayToSliceArg(args[i]->type, resolvedParamTypes[i])) {
                // TYP-12: array -> slice view; no element copy.
                argVal = ctx.emitArrayToSliceDecay(
                    static_cast<ArrayType*>(stripTypedefsT12(args[i]->type)),
                    argVal);
            } else {
                argVal = ctx.castValue(argVal, args[i]->type,
                                       ctx.getLLVMType(resolvedParamTypes[i]));
            }
        } else if (calleeFn->isVarArg()) {
            // C default argument promotions for the variadic tail.
            argVal = promoteVarArg(ctx, argVal, args[i]->type);
        }
        // I4 (D6): string 按值形参——活值实参深拷贝（形参独立缓冲，callee
        // destroy 不影响调用方）；新鲜实参直接传（移动）。
        if (i < resolvedParamTypes.size() &&
            isStringType(resolvedParamTypes[i]) &&
            !ctx.isFreshStringValue(*args[i])) {
            argVal = emitStringArgCopy(ctx, argVal, resolvedParamTypes[i]);
        }
        argsV.push_back(argVal);
    }

    // FUN-09: append default-argument constants for trailing params not
    // supplied at the call site.
    if (!resolvedParamTypes.empty() && args.size() < resolvedParamTypes.size()) {
        size_t defIdx = 0;
        for (size_t i = args.size(); i < resolvedParamTypes.size(); ++i, ++defIdx) {
            llvm::Type* paramLLVM = ctx.getLLVMType(resolvedParamTypes[i]);
            llvm::Value* defVal = nullptr;
            if (defIdx < defaultArgValues.size()) {
                const auto& dv = defaultArgValues[defIdx];
                // C4 review fix: the folded default may be an integer while the
                // parameter is floating (e.g. `float64 x = 5`) — build the
                // constant with the parameter's LLVM type (ConstantInt for
                // integer params, ConstantFP for float params, converting
                // int/char defaults to a float value when the param is FP).
                if (paramLLVM->isFloatingPointTy()) {
                    double v = dv.isInt ? static_cast<double>(dv.intVal)
                             : dv.isChar ? static_cast<double>(dv.charVal)
                             : dv.isDouble ? dv.doubleVal : 0.0;
                    defVal = llvm::ConstantFP::get(paramLLVM, v);
                } else if (dv.isInt) {
                    defVal = llvm::ConstantInt::get(paramLLVM, static_cast<uint64_t>(dv.intVal), /*isSigned=*/true);
                } else if (dv.isDouble) {
                    // Integer param with a double default — truncate.
                    defVal = llvm::ConstantInt::get(paramLLVM, static_cast<uint64_t>(dv.doubleVal), /*isSigned=*/true);
                } else if (dv.isChar) {
                    defVal = llvm::ConstantInt::get(paramLLVM, static_cast<uint64_t>(dv.charVal), /*isSigned=*/true);
                }
            }
            if (!defVal) {
                LOGE("missing default argument value for param {}", i);
                return nullptr;
            }
            argsV.push_back(defVal);
        }
    }

    if (calleeFn->isVarArg()) {
        if (argsV.size() < calleeFn->arg_size()) {
            LOGE("function {} expects at least {} args, got {}", callee, calleeFn->arg_size(), argsV.size());
            return nullptr;
        }
    } else if (argsV.size() != calleeFn->arg_size()) {
        LOGE("function {} expects {} args, got {}", callee, calleeFn->arg_size(), argsV.size());
        return nullptr;
    }

    if (calleeFn->getReturnType()->isVoidTy()) {
        return ctx.getBuilder().CreateCall(calleeFn, argsV);
    }
    return ctx.getBuilder().CreateCall(calleeFn, argsV, "calltmp");
}

llvm::Value* MethodCallExprAST::codegen(CodegenContext& ctx) {
    // CT-07: compile_time reflection — name_of/fields_of/attributes_of.
    // Must run before ctConstantOrNull (which would fold the ctHandled flag
    // into a default int constant).
    if (reflectKind != ReflectionKind::None) {
        return codegenReflection(ctx, *this);
    }
    if (auto* ct = ctConstantOrNull(ctx, *this)) return ct;
    // P1-06: builtin str/string methods lower through their own path.
    if (builtinMethod != BuiltinMethod::None) {
        return codegenBuiltinMethod(ctx, *this);
    }
    // PAR-05: indirect call through a function-pointer aggregate field.
    if (isFunctionPointerCall && fieldCallee) {
        llvm::Value* fnPtr = fieldCallee->codegen(ctx);
        if (!fnPtr) return nullptr;
        if (fieldCallee->isLValue) fnPtr = readLValue(ctx, *fieldCallee, fnPtr);
        std::vector<llvm::Value*> callArgs;
        for (size_t i = 0; i < args.size(); ++i) {
            llvm::Value* v = args[i]->codegen(ctx);
            if (!v) return nullptr;
            if (args[i]->isLValue) v = readLValue(ctx, *args[i], v);
            if (i < resolvedParamTypes.size())
                v = ctx.castValue(v, args[i]->type,
                                  ctx.getLLVMType(resolvedParamTypes[i]));
            callArgs.push_back(v);
        }
        std::vector<llvm::Type*> paramLLVM;
        for (auto* pt : resolvedParamTypes) paramLLVM.push_back(ctx.getLLVMType(pt));
        llvm::Type* retLLVM = type ? ctx.getLLVMType(type)
                                   : llvm::Type::getVoidTy(ctx.getContext());
        auto* fnType = llvm::FunctionType::get(retLLVM, paramLLVM, false);
        if (retLLVM->isVoidTy())
            return ctx.getBuilder().CreateCall(fnType, fnPtr, callArgs);
        return ctx.getBuilder().CreateCall(fnType, fnPtr, callArgs, "fieldcalltmp");
    }
    llvm::Value* objVal = object->codegen(ctx);
    if (!objVal) return nullptr;

    // P1-03 / PAR-17: 对象是指针类型的左值变量（`D* other` 作 `other->area()`
    // 的对象）——先解引用取出对象指针；类实例变量（alloca 即对象地址）不变。
    {
        Type* objT = object->type;
        while (objT && objT->kind == TypeKind::Typedef)
            objT = static_cast<TypedefType*>(objT)->aliasedType;
        if (objT && objT->kind == TypeKind::Pointer && object->isLValue) {
            auto& builder = ctx.getBuilder();
            objVal = builder.CreateLoad(llvm::PointerType::get(ctx.getContext(), 0), objVal,
                                        "objderef");
        }
    }

    // Get the object's type (should be a pointer to a class type)
    llvm::Type* objLLVMType = objVal->getType();
    if (!objLLVMType->isPointerTy()) return nullptr;

    // Determine the class type name for method mangling
    std::string className;
    Type* objType = object->type;
    if (objType && objType->kind == TypeKind::Pointer && objType->base) {
        objType = objType->base;
    }
    ClassType* classType = nullptr;
    if (objType && objType->kind == TypeKind::Class) {
        classType = static_cast<ClassType*>(objType);
        className = classType->name;
    } else if (objType && objType->kind == TypeKind::Struct) {
        auto* structType = static_cast<StructType*>(objType);
        className = structType->name;
    }

    // Prefer the parameter types chosen by semantic analysis (incl. this) so
    // decayed arguments mangle against the definition site. Mirror of
    // CallExprAST::codegen.
    std::vector<Type*> argTypes;
    if (!resolvedParamTypes.empty()) {
        argTypes = resolvedParamTypes;
    } else {
        Type* thisType = new Type(TypeKind::Pointer, objType);
        argTypes.push_back(thisType);
        for (auto& arg : args) {
            argTypes.push_back(arg->type);
        }
    }

    std::string mangledName = !mangledCallee.empty()
        ? mangledCallee
        : mangleFunction(methodName, argTypes);

    llvm::Function* calleeFn = ctx.getModule().getFunction(mangledName);
    if (!calleeFn) {
        // P1-03 / INH-05: 惰性前向声明（基类实例方法体在派生类方法之后出码
        // 等场景）；定义侧复用同名 llvm::Function。
        if (resolvedParamTypes.empty() || !type) {
            LOGE("unknown method: {}", mangledName);
            return nullptr;
        }
        llvm::Type* retLLVM = ctx.getLLVMType(type);
        std::vector<llvm::Type*> paramLLVM;
        for (auto* pt : argTypes) paramLLVM.push_back(ctx.getLLVMType(pt));
        auto* fnType = llvm::FunctionType::get(retLLVM, paramLLVM, false);
        calleeFn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                          mangledName, ctx.getModule());
    }

    std::vector<llvm::Value*> argsV;
    // Pass object pointer as the this argument
    argsV.push_back(objVal);
    for (size_t i = 0; i < args.size(); ++i) {
        llvm::Value* argVal = args[i]->codegen(ctx);
        if (!argVal) return nullptr;
        if (args[i]->isLValue) {
            argVal = readLValue(ctx, *args[i], argVal);
        }
        // resolvedParamTypes[0] is this — explicit argument i maps to [i+1].
        if (i + 1 < resolvedParamTypes.size()) {
            if (isArrayToSliceArg(args[i]->type, resolvedParamTypes[i + 1])) {
                // TYP-12: array -> slice view; no element copy.
                argVal = ctx.emitArrayToSliceDecay(
                    static_cast<ArrayType*>(stripTypedefsT12(args[i]->type)),
                    argVal);
            } else {
                argVal = ctx.castValue(argVal, args[i]->type,
                                       ctx.getLLVMType(resolvedParamTypes[i + 1]));
            }
        }
        // I4 (D6): string 按值形参（方法调用路径）——活值实参深拷贝。
        if (i + 1 < resolvedParamTypes.size() &&
            isStringType(resolvedParamTypes[i + 1]) &&
            !ctx.isFreshStringValue(*args[i])) {
            argVal = emitStringArgCopy(ctx, argVal, resolvedParamTypes[i + 1]);
        }
        argsV.push_back(argVal);
    }

    // FUN-09: append default-argument constants for trailing params not
    // supplied at the call site (index 0 of resolvedParamTypes is `this`).
    if (!resolvedParamTypes.empty() && args.size() + 1 < resolvedParamTypes.size()) {
        size_t defIdx = 0;
        for (size_t i = args.size() + 1; i < resolvedParamTypes.size(); ++i, ++defIdx) {
            llvm::Type* paramLLVM = ctx.getLLVMType(resolvedParamTypes[i]);
            llvm::Value* defVal = nullptr;
            if (defIdx < defaultArgValues.size()) {
                const auto& dv = defaultArgValues[defIdx];
                // C4 review fix (mirror of CallExprAST): FP param with an int/
                // char default must produce a float constant.
                if (paramLLVM->isFloatingPointTy()) {
                    double v = dv.isInt ? static_cast<double>(dv.intVal)
                             : dv.isChar ? static_cast<double>(dv.charVal)
                             : dv.isDouble ? dv.doubleVal : 0.0;
                    defVal = llvm::ConstantFP::get(paramLLVM, v);
                } else if (dv.isInt) {
                    defVal = llvm::ConstantInt::get(paramLLVM, static_cast<uint64_t>(dv.intVal), /*isSigned=*/true);
                } else if (dv.isDouble) {
                    defVal = llvm::ConstantInt::get(paramLLVM, static_cast<uint64_t>(dv.doubleVal), /*isSigned=*/true);
                } else if (dv.isChar) {
                    defVal = llvm::ConstantInt::get(paramLLVM, static_cast<uint64_t>(dv.charVal), /*isSigned=*/true);
                }
            }
            if (!defVal) {
                LOGE("missing default argument value for method param {}", i);
                return nullptr;
            }
            argsV.push_back(defVal);
        }
    }

    if (argsV.size() != calleeFn->arg_size()) {
        LOGE("method {}.{} expects {} args, got {}", className, methodName, calleeFn->arg_size(), argsV.size());
        return nullptr;
    }

    if (calleeFn->getReturnType()->isVoidTy()) {
        return ctx.getBuilder().CreateCall(calleeFn, argsV);
    }
    return ctx.getBuilder().CreateCall(calleeFn, argsV, "calltmp");
}

llvm::Value* AssignmentExprAST::codegen(CodegenContext& ctx) {
    // P1-07 / CG-09: bit-field LHS — codegen returns the storage-unit address
    // (and records bitOffset); read-modify-write the field, not the unit.
    if (lhs->isBitFieldAccess()) {
        auto* lma = static_cast<MemberAccessExprAST*>(lhs.get());
        llvm::Value* unitAddr = lhs->codegen(ctx);
        llvm::Value* rhsVal = rhs->codegen(ctx);
        if (!unitAddr || !rhsVal) return nullptr;
        auto& builder = ctx.getBuilder();
        llvm::Value* result = readLValue(ctx, *rhs, rhsVal);
        Type* ft = stripTypedefsT12(lhs->type);
        if (llvm::Type* dest = ctx.getLLVMType(ft)) {
            result = ctx.castValue(result, rhs->type, dest);
        }
        if (op != AssignOp::Assign) {
            // 复合赋值：先读当前位域值（基础类型宽度），运算后 RMW 写回。
            llvm::Value* cur = ctx.loadBitFieldValue(unitAddr, lma->bitOffset, lma->bitWidth, ft, lma->unitBits);
            switch (op) {
                case AssignOp::AddAssign:   result = builder.CreateAdd(cur, result, "addassign"); break;
                case AssignOp::SubAssign:   result = builder.CreateSub(cur, result, "subassign"); break;
                case AssignOp::MulAssign:   result = builder.CreateMul(cur, result, "mulassign"); break;
                case AssignOp::DivAssign:   result = builder.CreateSDiv(cur, result, "divassign"); break;
                case AssignOp::ModAssign:   result = builder.CreateSRem(cur, result, "modassign"); break;
                case AssignOp::BitAndAssign: result = builder.CreateAnd(cur, result, "bandassign"); break;
                case AssignOp::BitOrAssign:  result = builder.CreateOr(cur, result, "borassign"); break;
                case AssignOp::BitXorAssign: result = builder.CreateXor(cur, result, "bxorassign"); break;
                case AssignOp::LShiftAssign: result = builder.CreateShl(cur, result, "lshiftassign"); break;
                case AssignOp::RShiftAssign: result = builder.CreateAShr(cur, result, "rshiftassign"); break;
                default: break;
            }
        }
        ctx.storeBitFieldValue(unitAddr, lma->bitOffset, lma->bitWidth, ft, result, lma->unitBits);
        return result;
    }

    llvm::Value* lhsVal = lhs->codegen(ctx);
    llvm::Value* rhsVal = rhs->codegen(ctx);
    if (!lhsVal || !rhsVal) return nullptr;

    auto& builder = ctx.getBuilder();
    // Only load the RHS when it denotes a location; values such as function
    // designators or '&x' are already the stored value.
    llvm::Value* result = readLValue(ctx, *rhs, rhsVal);

    // TYP-12: array RHS decays to a slice view when assigned to a slice LHS.
    {
        Type* lt = stripTypedefsT12(lhs->type);
        Type* rt = stripTypedefsT12(rhs->type);
        if (lt && rt && lt->kind == TypeKind::Slice && rt->kind == TypeKind::Array &&
            op == AssignOp::Assign) {
            result = ctx.emitArrayToSliceDecay(static_cast<ArrayType*>(rt), result);
        } else if (lhs->type) {
            result = ctx.castValue(result, rhs->type, ctx.getLLVMType(lhs->type));
        }
    }

    if (op != AssignOp::Assign) {
        // CON-01: atomic LHS compound assignment → atomicrmw (seq_cst).
        if (lhs->type && stripTypedefsT12(lhs->type)->isAtomic) {
            Type* lt = stripTypedefsT12(lhs->type);
            unsigned bits = 0;
            llvm::Type* carrier = ctx.getAtomicCarrierType(lt, bits);
            if (carrier) {
                llvm::AtomicRMWInst::BinOp rmwOp;
                switch (op) {
                    case AssignOp::AddAssign:    rmwOp = llvm::AtomicRMWInst::Add; break;
                    case AssignOp::SubAssign:    rmwOp = llvm::AtomicRMWInst::Sub; break;
                    case AssignOp::BitAndAssign: rmwOp = llvm::AtomicRMWInst::And; break;
                    case AssignOp::BitOrAssign:  rmwOp = llvm::AtomicRMWInst::Or; break;
                    case AssignOp::BitXorAssign: rmwOp = llvm::AtomicRMWInst::Xor; break;
                    default: rmwOp = llvm::AtomicRMWInst::Xchg; break; // unreachable (sema rejects)
                }
                llvm::Value* operand = ctx.valueToAtomicCarrier(result, lt, carrier);
                llvm::Value* rmw = builder.CreateAtomicRMW(
                    rmwOp, lhsVal, operand, llvm::Align(bits / 8),
                    llvm::AtomicOrdering::SequentiallyConsistent);
                return ctx.valueFromAtomicCarrier(rmw, lt, carrier);
            }
        }
        // 复合赋值 lhs 必为可写左值（位域 lhs 已在函数顶部早退）；无条件取值
        // 保持既有行为（测试手工构造 AST 时 isLValue 可能未置位）。
        llvm::Value* loadedLhs = ctx.loadValue(lhsVal, lhs->type);
        // 浮点操作数须走 F 系指令——CreateAdd 等按整数位模式运算，浮点复合
        // 赋值（fd *= 2.0）会得到垃圾值（1.5*2.0 → 0）。无符号走 U 系
        // （udiv/urem/lshr），否则高位值被当有符号（0x80000000 >>= 4 变算术移）。
        const bool fpLhs = loadedLhs->getType()->isFloatingPointTy();
        const bool unsignedLhs =
            lhs->type && isUnsignedArithmeticType(stripTypedefsT12(lhs->type));
        switch (op) {
            case AssignOp::AddAssign:
                result = fpLhs ? builder.CreateFAdd(loadedLhs, result, "addassign")
                               : builder.CreateAdd(loadedLhs, result, "addassign");
                break;
            case AssignOp::SubAssign:
                result = fpLhs ? builder.CreateFSub(loadedLhs, result, "subassign")
                               : builder.CreateSub(loadedLhs, result, "subassign");
                break;
            case AssignOp::MulAssign:
                result = fpLhs ? builder.CreateFMul(loadedLhs, result, "mulassign")
                               : builder.CreateMul(loadedLhs, result, "mulassign");
                break;
            case AssignOp::DivAssign:
                result = fpLhs ? builder.CreateFDiv(loadedLhs, result, "divassign")
                               : (unsignedLhs ? builder.CreateUDiv(loadedLhs, result, "divassign")
                                              : builder.CreateSDiv(loadedLhs, result, "divassign"));
                break;
            case AssignOp::ModAssign:
                result = fpLhs ? builder.CreateFRem(loadedLhs, result, "modassign")
                               : (unsignedLhs ? builder.CreateURem(loadedLhs, result, "modassign")
                                              : builder.CreateSRem(loadedLhs, result, "modassign"));
                break;
            case AssignOp::BitAndAssign: result = builder.CreateAnd(loadedLhs, result, "bandassign"); break;
            case AssignOp::BitOrAssign:  result = builder.CreateOr(loadedLhs, result, "borassign"); break;
            case AssignOp::BitXorAssign: result = builder.CreateXor(loadedLhs, result, "bxorassign"); break;
            case AssignOp::LShiftAssign: result = builder.CreateShl(loadedLhs, result, "lshiftassign"); break;
            case AssignOp::RShiftAssign:
                result = unsignedLhs ? builder.CreateLShr(loadedLhs, result, "rshiftassign")
                                     : builder.CreateAShr(loadedLhs, result, "rshiftassign");
                break;
            default: break;
        }
    }

    // I4 (D7): string 赋值——活值深拷贝 / 新鲜值移动，旧缓冲自动释放，
    // 消除 I4「值拷贝别名双 free」。替代下方 :1334 的裸 store。
    {
        Type* lt = stripTypedefsT12(lhs->type);
        if (lt && lt->kind == TypeKind::String && op == AssignOp::Assign) {
            if (ctx.isFreshStringValue(*rhs)) {
                // 新鲜值（string.new / 返回 string 的调用）→ 移动：free 旧缓冲
                // （null 守卫，防御未初始化/零化局部）后裸 store。
                llvm::Function* func = builder.GetInsertBlock()->getParent();
                llvm::BasicBlock* freeBB = llvm::BasicBlock::Create(
                    ctx.getContext(), "asn.free", func);
                llvm::BasicBlock* storeBB = llvm::BasicBlock::Create(
                    ctx.getContext(), "asn.store", func);
                llvm::Value* dstVal = ctx.loadValue(lhsVal, lhs->type);
                llvm::Value* oldPtr = builder.CreateExtractValue(dstVal, 0, "asn.oldptr");
                llvm::Value* nullPtr = llvm::Constant::getNullValue(oldPtr->getType());
                builder.CreateCondBr(builder.CreateICmpNE(oldPtr, nullPtr, "asn.hasold?"),
                                     freeBB, storeBB);
                builder.SetInsertPoint(freeBB);
                auto freeFn = ctx.getModule().getOrInsertFunction(
                    "free", llvm::FunctionType::get(
                        llvm::Type::getVoidTy(ctx.getContext()), {oldPtr->getType()}, false));
                builder.CreateCall(freeFn, {oldPtr});
                builder.CreateBr(storeBB);
                builder.SetInsertPoint(storeBB);
                builder.CreateStore(result, lhsVal);
            } else {
                // 活值 → 深拷贝赋值（自赋值守卫 + free 旧缓冲 + 拷贝）。
                auto savedIP = builder.saveIP();
                llvm::Function* assignFn = ctx.getStringAssignFn();
                builder.restoreIP(savedIP);
                builder.CreateCall(assignFn, {lhsVal, result});
            }
            return result;
        }
        // I4 (D7): 聚合含 string 字段 → 字段感知赋值（string 字段 free 旧 + 拷贝）。
        if (lt && op == AssignOp::Assign &&
            (lt->kind == TypeKind::Struct || lt->kind == TypeKind::Class) &&
            ctx.aggregateHasStringField(lt)) {
            ctx.emitAggregateStringCopy(lhsVal, result, lt, /*assign=*/true);
            return result;
        }
    }

    // CON-01: atomic-typed lvalues store through the same integer carrier as
    // loads, with natural alignment (i64 fields need align 8).
    if (lhs->type && stripTypedefsT12(lhs->type)->isAtomic) {
        Type* lt = stripTypedefsT12(lhs->type);
        unsigned bits = 0;
        llvm::Type* carrier = ctx.getAtomicCarrierType(lt, bits);
        if (carrier) {
            llvm::Value* storeVal =
                ctx.valueToAtomicCarrier(result, lt, carrier);
            llvm::StoreInst* store = builder.CreateStore(storeVal, lhsVal);
            store->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
            store->setAlignment(llvm::Align(bits / 8));
            return result;
        }
    }

    llvm::StoreInst* store = builder.CreateStore(result, lhsVal);
    return result;
}

llvm::Value* TernaryExprAST::codegen(CodegenContext& ctx) {
    if (auto* ct = ctConstantOrNull(ctx, *this)) return ct;
    llvm::Value* condVal = cond->codegen(ctx);
    if (!condVal) return nullptr;
    if (cond->isLValue && cond->type && cond->type->kind != TypeKind::Array) {
        condVal = readLValue(ctx, *cond, condVal);
    }
    condVal = ctx.coerceToBool(condVal, cond->type);

    auto& builder = ctx.getBuilder();
    llvm::Function* func = builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(ctx.getContext(), "ternary.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(ctx.getContext(), "ternary.else", func);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(ctx.getContext(), "ternary.merge", func);

    builder.CreateCondBr(condVal, thenBB, elseBB);

    // 根因修复：lvalue 分支 codegen 产生的是地址——直接喂 phi 会把地址当
    // 值（标量被截断、指针存变量地址、聚合为垃圾）。分支须先取值（数组
    // 除外：数组 lvalue 不按值装入，与条件路径同一处理）。取值/转换在分支
    // 块内完成——merge 块顶只允许 phi。
    Type* common = type;
    llvm::Type* commonLLVM = common ? ctx.getLLVMType(common) : nullptr;
    auto finishBranch = [&ctx, commonLLVM](ExprAST& branch, llvm::Value* val) -> llvm::Value* {
        if (branch.isLValue && branch.type && branch.type->kind != TypeKind::Array) {
            val = readLValue(ctx, branch, val);
        }
        if (val && branch.type) {
            if (llvm::Type* dest = commonLLVM) {
                if (val->getType() != dest) val = ctx.castValue(val, branch.type, dest);
            }
        }
        return val;
    };

    builder.SetInsertPoint(thenBB);
    llvm::Value* thenVal = then->codegen(ctx);
    if (!thenVal) return nullptr;
    thenVal = finishBranch(*then, thenVal);
    if (!thenVal) return nullptr;
    // 嵌套三元：内层表达式会切换插入点，phi 的前驱必须记实际终结块。
    llvm::BasicBlock* thenPred = builder.GetInsertBlock();
    builder.CreateBr(mergeBB);

    builder.SetInsertPoint(elseBB);
    llvm::Value* elseVal = elseExpr->codegen(ctx);
    if (!elseVal) return nullptr;
    elseVal = finishBranch(*elseExpr, elseVal);
    if (!elseVal) return nullptr;
    llvm::BasicBlock* elsePred = builder.GetInsertBlock();
    builder.CreateBr(mergeBB);

    builder.SetInsertPoint(mergeBB);
    llvm::PHINode* phi = builder.CreatePHI(thenVal->getType(), 2, "ternarytmp");
    phi->addIncoming(thenVal, thenPred);
    phi->addIncoming(elseVal, elsePred);
    return phi;
}

// P1-08 / CG-10: inline asm → llvm::InlineAsm。
//
// LLVM 21 的 inline asm 约束语义与 GCC 有三处关键差异（实证于 llc + LLVM 源码
// InlineAsm.cpp Parse/verify、InlineAsm.h ConstraintPrefix）：
//   1. 输出必须映射到 call 返回值：单输出 → 返回类型即该标量；多输出 →
//      StructType（按约束顺序）；无输出 → void。plan 原案的「给 `=r` 传左值
//      地址」被 LLVM 拒绝（"number of input constraints does not match"）。
//   2. 约束前缀只认 `~`/`=`/`!`，无 GCC 的 `+`：`+r` 会被当纯输入（且多出
//      无意义的 `+` code）。读-写操作数翻译为 `=r`（输出）＋ 追加在全部普通
//      输入之后的 tied 输入（数字约束 = 该输出在 outputs 中的下标），使 GCC
//      模板编号（%N）与 LLVM `$N` 完全一致（tied 槽不被 GCC 模板引用）。
//   3. 模板转义不同：LLVM 只认 `$N`（操作数引用）与 `$$`（字面 `$`），`%`
//      不做任何处理。GCC 模板翻译：`$`→`$$`、`%%`→`%`、`%N`→`$N`（顺序为
//      先转义字面 `$`，再 `%%`，最后 `%N`，避免把新引入的 `$N` 再次转义）。
llvm::Value* AsmExprAST::codegen(CodegenContext& ctx) {
    llvm::LLVMContext& context = ctx.getContext();
    llvm::IRBuilder<>& builder = ctx.getBuilder();

    const unsigned nOut = static_cast<unsigned>(outputs.size());
    const unsigned nIn = static_cast<unsigned>(inputs.size());

    // 1. 返回类型：0 → void；1 → 该标量；≥2 → StructType。
    std::vector<llvm::Type*> retTys;
    retTys.reserve(nOut);
    for (Type* t : outputTypes) retTys.push_back(ctx.getLLVMType(t));
    llvm::Type* retTy = llvm::Type::getVoidTy(context);
    if (retTys.size() == 1) {
        retTy = retTys[0];
    } else if (retTys.size() > 1) {
        retTy = llvm::StructType::get(context, retTys);
    }

    // 2. 约束串（LLVM 方言）：outputs（`+`→`=`）＋ inputs ＋ 追加的 tied 输入
    //    ＋ clobbers（`~{name}`）。tied 输入排在所有普通输入之后。
    std::vector<int> rwIndices;  // 读-写输出在 outputs 中的下标（升序）
    std::vector<std::string> cons;
    cons.reserve(nOut + nIn + nOut);
    for (unsigned i = 0; i < nOut; ++i) {
        std::string c = outputs[i].constraint;
        if (!c.empty() && c[0] == '+') {
            c[0] = '=';  // '+r' → '=r'
            rwIndices.push_back(static_cast<int>(i));
        }
        cons.push_back(std::move(c));
    }
    for (unsigned i = 0; i < nIn; ++i) cons.push_back(inputs[i].constraint);
    for (int idx : rwIndices) cons.push_back(std::to_string(idx));
    for (const std::string& cl : clobbers) cons.push_back("~{" + cl + "}");
    std::string constraintStr;
    for (size_t i = 0; i < cons.size(); ++i) {
        if (i) constraintStr += ',';
        constraintStr += cons[i];
    }

    // SG-19: named operands are numbered outputs-first, then inputs.
    std::unordered_map<std::string, unsigned> namedOperands;
    for (unsigned i = 0; i < nOut; ++i) {
        if (!outputs[i].name.empty()) namedOperands[outputs[i].name] = i;
    }
    for (unsigned i = 0; i < nIn; ++i) {
        if (!inputs[i].name.empty()) namedOperands[inputs[i].name] = nOut + i;
    }

    // 3. 模板 GCC → LLVM 翻译。
    std::string tpl;
    tpl.reserve(asmTemplate.size());
    for (size_t i = 0; i < asmTemplate.size(); ++i) {
        const char ch = asmTemplate[i];
        if (ch == '$') {
            tpl += "$$";
        } else if (ch == '%') {
            const char nx = (i + 1 < asmTemplate.size()) ? asmTemplate[i + 1] : '\0';
            if (nx == '%') {
                tpl += '%';
                ++i;
            } else if (nx >= '0' && nx <= '9') {
                size_t j = i + 1;
                while (j < asmTemplate.size() && asmTemplate[j] >= '0' && asmTemplate[j] <= '9')
                    ++j;
                tpl += '$';
                tpl.append(asmTemplate, i + 1, j - i - 1);
                i = j - 1;
            } else if (nx == '[') {
                const size_t close = asmTemplate.find(']', i + 2);
                if (close != std::string::npos) {
                    const std::string name =
                        asmTemplate.substr(i + 2, close - i - 2);
                    auto it = namedOperands.find(name);
                    if (it != namedOperands.end()) {
                        tpl += '$';
                        tpl += std::to_string(it->second);
                    } else {
                        tpl += ch;  // sema E2024 已拒绝；防御性直通
                    }
                    i = close;
                } else {
                    tpl += ch;
                }
            } else {
                tpl += ch;  // sema E2024 已拒绝；防御性直通
            }
        } else {
            tpl += ch;
        }
    }

    // 4. 签名：参数 = 普通 inputs（值）＋ 读-写 outputs 初值（值）。
    std::vector<llvm::Type*> paramTys;
    paramTys.reserve(nIn + rwIndices.size());
    for (unsigned i = 0; i < nIn; ++i) paramTys.push_back(ctx.getLLVMType(inputTypes[i]));
    for (int idx : rwIndices) paramTys.push_back(ctx.getLLVMType(outputTypes[idx]));
    llvm::FunctionType* ft =
        llvm::FunctionType::get(retTy, paramTys, /*isVarArg=*/false);
    llvm::InlineAsm* ia = llvm::InlineAsm::get(
        ft, tpl, constraintStr,
        /*HasSideEffects=*/isVolatile || nOut == 0,
        /*IsAlignStack=*/false,
        llvm::InlineAsm::AD_ATT);

    // 5. 实参：先求所有 output 左值地址（求值一次，供 tied 输入与回写共用），
    //    再 inputs 值 ＋ 读-写输出初值。
    std::vector<llvm::Value*> outAddr(nOut);
    for (unsigned i = 0; i < nOut; ++i) {
        outAddr[i] = outputs[i].expr->codegen(ctx);
        if (!outAddr[i]) return nullptr;
    }
    std::vector<llvm::Value*> args;
    args.reserve(nIn + rwIndices.size());
    // 输入一律传值。注意：LLVM 对 `m`（内存）输入会把该值 spill 到栈临时并传
    // 其地址——只读模板语义与 GCC 一致，但模板若经 `m` 输入写内存，写的是副本
    // 而非原变量（GCC 写原变量）。与 spec §5 记录一致；要写回须用 `=`/`+` 输出。
    for (unsigned i = 0; i < nIn; ++i) {
        llvm::Value* v = inputs[i].expr->codegen(ctx);
        if (!v) return nullptr;
        args.push_back(emitRValue(ctx, *inputs[i].expr, v));
    }
    for (int idx : rwIndices) {
        llvm::Value* v = emitRValue(ctx, *outputs[idx].expr, outAddr[idx]);
        if (!v) return nullptr;
        args.push_back(v);
    }

    // 6. 调用 + 逐输出回写（结果丢弃，void 语义，D3）。
    llvm::Value* result = builder.CreateCall(ia, args);
    for (unsigned i = 0; i < nOut; ++i) {
        llvm::Value* outVal =
            (retTys.size() == 1) ? result : builder.CreateExtractValue(result, i, "asm.out");
        builder.CreateStore(outVal, outAddr[i]);
    }

    return nullptr;  // void 表达式
}

llvm::Value* CastExprAST::codegen(CodegenContext& ctx) {
    llvm::Value* val = expr->codegen(ctx);
    if (!val) return nullptr;

    llvm::Type* targetLLVMType = ctx.getLLVMType(castType);
    if (!targetLLVMType) return nullptr;

    if (expr->isLValue) {
        val = readLValue(ctx, *expr, val);
    }

    // `reinterpret_cast` reinterprets the bit pattern of same-width scalars
    // (notably float <-> int). Pointers and pointer<->integer go through
    // castValue (bitcast / ptrtoint / inttoptr); other pairs fall back to a
    // value conversion.
    if (castKind == CastKind::Reinterpret) {
        llvm::Type* src = val->getType();
        if (src == targetLLVMType) return val;
        const bool sameWidthScalars =
            !src->isPointerTy() && !targetLLVMType->isPointerTy() &&
            src->isSized() && targetLLVMType->isSized() &&
            src->getPrimitiveSizeInBits() == targetLLVMType->getPrimitiveSizeInBits() &&
            (src->isFloatingPointTy() || targetLLVMType->isFloatingPointTy());
        if (sameWidthScalars) {
            return ctx.getBuilder().CreateBitCast(val, targetLLVMType, "reinterpret");
        }
        return ctx.castValue(val, targetLLVMType);
    }

    return ctx.castValue(val, targetLLVMType);
}

llvm::Value* CommaExprAST::codegen(CodegenContext& ctx) {
    left->codegen(ctx);
    return right->codegen(ctx);
}

llvm::Value* PostfixIncDecExprAST::codegen(CodegenContext& ctx) {
    // SEM-17: overloaded postfix operator — call with (object, int32 dummy).
    if (!mangledCallee.empty()) {
        llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
        if (!calleeFn) {
            LOGE("unknown postfix operator function: {}", mangledCallee);
            return nullptr;
        }
        llvm::Value* obj = operand->codegen(ctx);
        if (!obj) return nullptr;
        obj = emitRValue(ctx, *operand, obj);
        llvm::Value* dummy = llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(ctx.getContext()), 0);
        return ctx.getBuilder().CreateCall(calleeFn, {obj, dummy}, "popcalltmp");
    }
    llvm::Value* addr = operand->codegen(ctx);
    if (!addr) return nullptr;

    auto& builder = ctx.getBuilder();
    // P1-07 / CG-09: bit-field operand — RMW, return the OLD value.
    if (operand->isBitFieldAccess()) {
        auto* ma = static_cast<MemberAccessExprAST*>(operand.get());
        Type* ft = stripTypedefsT12(operand->type);
        llvm::Value* oldVal = ctx.loadBitFieldValue(addr, ma->bitOffset, ma->bitWidth, ft, ma->unitBits);
        llvm::Value* one = llvm::ConstantInt::get(oldVal->getType(), 1);
        llvm::Value* newVal = isIncrement ? builder.CreateAdd(oldVal, one, "postinc")
                                          : builder.CreateSub(oldVal, one, "postdec");
        ctx.storeBitFieldValue(addr, ma->bitOffset, ma->bitWidth, ft, newVal, ma->unitBits);
        return oldVal;
    }
    // CON-01: atomic operand — atomicrmw add/sub 1; yield the OLD value.
    if (operand->type && stripTypedefsT12(operand->type)->isAtomic) {
        Type* ot = stripTypedefsT12(operand->type);
        unsigned bits = 0;
        llvm::Type* carrier = ctx.getAtomicCarrierType(ot, bits);
        if (carrier) {
            llvm::Value* one = llvm::ConstantInt::get(carrier, 1);
            llvm::Value* old = builder.CreateAtomicRMW(
                isIncrement ? llvm::AtomicRMWInst::Add : llvm::AtomicRMWInst::Sub,
                addr, one, llvm::Align(bits / 8),
                llvm::AtomicOrdering::SequentiallyConsistent);
            return ctx.valueFromAtomicCarrier(old, ot, carrier);
        }
    }
    llvm::Type* loadType = operand->type ? ctx.getLLVMType(operand->type) : llvm::Type::getInt32Ty(ctx.getContext());
    llvm::Value* oldVal = builder.CreateLoad(loadType, addr, "postold");
    llvm::Value* newVal = nullptr;
    if (loadType->isPointerTy()) {
        // 指针自增/自减按元素步进（GEP），不能 CreateAdd（opaque ptr 是字节
        // 算术）——否则 int32* ++ 只前进 1 字节，解引用读到错位值。
        llvm::Type* elemTy = (operand->type && operand->type->base)
            ? ctx.getLLVMType(operand->type->base)
            : llvm::Type::getInt8Ty(ctx.getContext());
        if (!elemTy) elemTy = llvm::Type::getInt8Ty(ctx.getContext());
        llvm::Value* step = llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx.getContext()), isIncrement ? 1 : -1);
        newVal = builder.CreateGEP(elemTy, oldVal, step, "postptr");
    } else {
        llvm::Value* one = llvm::ConstantInt::get(loadType, 1);
        newVal = isIncrement ? builder.CreateAdd(oldVal, one, "postinc")
                             : builder.CreateSub(oldVal, one, "postdec");
    }
    builder.CreateStore(newVal, addr);
    return oldVal;
}

llvm::Value* ArrayAccessExprAST::codegen(CodegenContext& ctx) {
    // SEM-17: overloaded `operator[]` — call with (object, index).
    if (!mangledCallee.empty()) {
        llvm::Function* calleeFn = ctx.getModule().getFunction(mangledCallee);
        if (!calleeFn) {
            LOGE("unknown operator[] function: {}", mangledCallee);
            return nullptr;
        }
        llvm::Value* obj = array->codegen(ctx);
        llvm::Value* idx = index->codegen(ctx);
        if (!obj || !idx) return nullptr;
        if (!memberOperator) obj = emitRValue(ctx, *array, obj);
        idx = emitRValue(ctx, *index, idx);
        return ctx.getBuilder().CreateCall(calleeFn, {obj, idx}, "idxcalltmp");
    }
    llvm::Value* arrVal = array->codegen(ctx);
    llvm::Value* idxVal = index->codegen(ctx);
    if (!arrVal || !idxVal) return nullptr;

    if (index->isLValue) {
        idxVal = readLValue(ctx, *index, idxVal);
    }

    // Determine the element type and, for a pointer operand, load the pointer
    // value (an array operand is already the address of its first element).
    Type* arrType = stripTypedefsT12(array->type);
    llvm::Type* elemTy = llvm::Type::getInt32Ty(ctx.getContext());
    if (arrType && arrType->kind == TypeKind::Array) {
        auto* at = static_cast<ArrayType*>(arrType);
        if (at->elementType) elemTy = ctx.getLLVMType(at->elementType);
    } else if (arrType && arrType->kind == TypeKind::Pointer) {
        if (arrType->base) elemTy = ctx.getLLVMType(arrType->base);
        arrVal = emitRValue(ctx, *array, arrVal);
    } else if (arrType && arrType->kind == TypeKind::Slice) {
        // TYP-12: load the {ptr, len} view, then index through its pointer.
        auto* st = static_cast<SliceType*>(arrType);
        auto* sliceTy = ctx.getLLVMType(arrType);
        auto& builder = ctx.getBuilder();
        if (array->isLValue) arrVal = builder.CreateLoad(sliceTy, arrVal);
        if (st->elementType) elemTy = ctx.getLLVMType(st->elementType);
        arrVal = builder.CreateExtractValue(arrVal, {0}, "slice.ptr");
    } else if (arrType && arrType->kind == TypeKind::String) {
        // P1-06 (FMT-02): load the {ptr, len, cap} header, index its buffer.
        auto* strTy = ctx.getLLVMType(arrType);
        auto& builder = ctx.getBuilder();
        if (array->isLValue) arrVal = builder.CreateLoad(strTy, arrVal);
        elemTy = llvm::Type::getInt8Ty(ctx.getContext());
        arrVal = builder.CreateExtractValue(arrVal, {0}, "string.ptr");
    } else if (arrType && arrType->kind == TypeKind::Str) {
        // P1-06 (FMT-01): load the {ptr, len} header, index its buffer.
        auto* strTy = ctx.getLLVMType(arrType);
        auto& builder = ctx.getBuilder();
        if (array->isLValue) arrVal = builder.CreateLoad(strTy, arrVal);
        elemTy = llvm::Type::getInt8Ty(ctx.getContext());
        arrVal = builder.CreateExtractValue(arrVal, {0}, "str.ptr");
    }

    auto& builder = ctx.getBuilder();
    return builder.CreateGEP(elemTy, arrVal, idxVal, "arrayidx");
}

llvm::Value* RangeExprAST::codegen(CodegenContext& ctx) {
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* i8Ty = llvm::Type::getInt8Ty(c);

    Type* objType = stripTypedefsT12(object->type);
    if (!objType) return nullptr;

    llvm::Value* basePtr = nullptr;
    llvm::Value* totalLen = nullptr;
    Type* elemType = nullptr;
    bool stringLike = false;

    if (objType->kind == TypeKind::Str || objType->kind == TypeKind::String) {
        llvm::Value* v = object->codegen(ctx);
        if (!v) return nullptr;
        if (object->isLValue) v = readLValue(ctx, *object, v);
        basePtr = builder.CreateExtractValue(v, 0, "range.ptr");
        totalLen = builder.CreateExtractValue(v, 1, "range.len");
        elemType = TypeContext::instance().getChar();
        stringLike = true;
    } else if (objType->kind == TypeKind::Slice) {
        llvm::Value* v = object->codegen(ctx);
        if (!v) return nullptr;
        if (object->isLValue) v = readLValue(ctx, *object, v);
        basePtr = builder.CreateExtractValue(v, 0, "range.ptr");
        totalLen = builder.CreateExtractValue(v, 1, "range.len");
        elemType = static_cast<SliceType*>(objType)->elementType;
    } else if (objType->kind == TypeKind::Array) {
        auto* at = static_cast<ArrayType*>(objType);
        llvm::Value* addr = object->codegen(ctx);
        if (!addr) return nullptr;
        if (!object->isLValue) {
            // Array expressions already denote a pointer to their first element.
            basePtr = addr;
        } else {
            llvm::Type* arrLLVM = ctx.getLLVMType(objType);
            basePtr = builder.CreateInBoundsGEP(
                arrLLVM, addr,
                {llvm::ConstantInt::get(i64Ty, 0),
                 llvm::ConstantInt::get(i64Ty, 0)},
                "range.base");
        }
        totalLen = llvm::ConstantInt::get(i64Ty, at->size);
        elemType = at->elementType;
    } else {
        return nullptr;
    }

    auto indexValue = [&](ExprAST* e, llvm::Value* fallback) -> llvm::Value* {
        if (!e) return fallback;
        llvm::Value* v = e->codegen(ctx);
        if (!v) return nullptr;
        if (e->isLValue) v = readLValue(ctx, *e, v);
        return builder.CreateSExtOrTrunc(v, i64Ty, "range.idx");
    };

    llvm::Value* beginV = indexValue(begin.get(), llvm::ConstantInt::get(i64Ty, 0));
    if (!beginV) return nullptr;
    llvm::Value* endV = indexValue(end.get(), totalLen);
    if (!endV) return nullptr;

    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "range.ok", fn);
    llvm::BasicBlock* badBB = llvm::BasicBlock::Create(c, "range.bad", fn);
    auto* neg = builder.CreateICmpSLT(beginV, llvm::ConstantInt::get(i64Ty, 0), "range.neg");
    auto* inv = builder.CreateICmpSLT(endV, beginV, "range.inv");
    auto* over = builder.CreateICmpSGT(endV, totalLen, "range.over");
    auto* bad = builder.CreateOr(neg, builder.CreateOr(inv, over), "range.oob");
    builder.CreateCondBr(bad, badBB, okBB);

    builder.SetInsertPoint(badBB);
    {
        std::string prefix = nodeSourcePrefix(*this);
        std::string msg = prefix.empty()
            ? "panic: range: out of bounds\n"
            : prefix + ": panic: range: out of bounds\n";
        llvm::Value* fmt = builder.CreateGlobalString(msg, ".rangefmt");
        emitDprintfAndAbort(ctx, fmt, {});
        builder.CreateUnreachable();
    }

    builder.SetInsertPoint(okBB);
    llvm::Type* elemLLVM = ctx.getLLVMType(elemType);
    llvm::Value* subPtr = builder.CreateGEP(elemLLVM, basePtr, beginV, "range.ptr");
    llvm::Value* subLen = builder.CreateSub(endV, beginV, "range.n");
    llvm::Type* resTy = ctx.getLLVMType(type);
    llvm::Value* res = llvm::Constant::getNullValue(resTy);
    res = builder.CreateInsertValue(res, subPtr, {0});
    res = builder.CreateInsertValue(res, subLen, {1});
    (void)stringLike;
    (void)i8Ty;
    return res;
}

// Emit a GEP for a class field, following the base-class chain if the member
// is inherited. The LLVM layout of a derived class is { %Base, ownFields... }.
static llvm::Value* emitClassFieldGEP(CodegenContext& ctx, Type* aggType,
                                      llvm::Value* objPtr, const std::string& memberName) {
    auto& builder = ctx.getBuilder();
    // INH-01: unified walk over ClassType and StructType inheritance chains —
    // the base sub-object occupies field slot 0 of every derived layout.
    // Depth-capped: a redefinition-shaped cycle must never hang the compiler
    // (评审 C1).
    Type* cur = aggType;
    llvm::Value* curPtr = objPtr;
    int walkDepth = 0;

    while (cur) {
        std::vector<FieldInfo>* fields = nullptr;
        std::string baseClassName;
        Type* baseType = nullptr;
        if (cur->kind == TypeKind::Class) {
            auto* classType = static_cast<ClassType*>(cur);
            fields = &classType->fields;
            baseClassName = classType->baseClass;
            baseType = classType->base;
        } else if (cur->kind == TypeKind::Struct) {
            auto* structType = static_cast<StructType*>(cur);
            fields = &structType->fields;
            baseClassName = structType->baseClass;
            baseType = structType->base;
        } else {
            break;
        }

        for (size_t i = 0; i < fields->size(); ++i) {
            if ((*fields)[i].name == memberName) {
                unsigned idx = static_cast<unsigned>(i);
                // Base sub-object occupies field index 0. 位域/注解聚合在物化时
                // 已把基类伪字段压入 fields（索引 0），此时不再 +1；未物化路径
                // 才需要（LLVM 结构体的字段 0 = 基类）。
                bool materialized = cur->kind == TypeKind::Class
                    ? static_cast<ClassType*>(cur)->layoutMaterialized
                    : static_cast<StructType*>(cur)->layoutMaterialized;
                if (!baseClassName.empty() && !materialized) idx += 1;
                llvm::Type* curLLVM = ctx.getLLVMType(cur);
                if (!curLLVM) return nullptr;
                return builder.CreateStructGEP(curLLVM, curPtr, idx, "member");
            }
        }

        if (baseClassName.empty()) break;
        if (!baseType || (baseType->kind != TypeKind::Class &&
                          baseType->kind != TypeKind::Struct)) {
            break;
        }
        llvm::Type* curLLVM = ctx.getLLVMType(cur);
        if (!curLLVM) break;
        curPtr = builder.CreateStructGEP(curLLVM, curPtr, 0, "base");
        cur = baseType;
        if (++walkDepth > 64) break;
    }

    return nullptr;
}

// P1-07 / CG-09: bit-field member GEP — walk the inheritance chain to the
// defining aggregate and GEP to the storage unit. `unitIndex` is the absolute
// LLVM field index recorded at materialization (base sub-object occupies slot
// 0, so no +1 adjustment is needed here). Records bitOffset on the node for
// the RMW read/write helpers.
static llvm::Value* emitBitFieldGEP(CodegenContext& ctx, Type* aggType,
                                    llvm::Value* objPtr, MemberAccessExprAST& node) {
    auto& builder = ctx.getBuilder();
    Type* cur = aggType;
    llvm::Value* curPtr = objPtr;
    int walkDepth = 0;
    while (cur) {
        std::unordered_map<std::string, BitFieldLayout>* bf = nullptr;
        std::string baseClassName;
        Type* baseType = nullptr;
        if (cur->kind == TypeKind::Class) {
            auto* ct = static_cast<ClassType*>(cur);
            bf = &ct->bitFields;
            baseClassName = ct->baseClass;
            baseType = ct->base;
        } else if (cur->kind == TypeKind::Struct) {
            auto* st = static_cast<StructType*>(cur);
            bf = &st->bitFields;
            baseClassName = st->baseClass;
            baseType = st->base;
        } else {
            break;
        }
        auto it = bf->find(node.memberName);
        if (it != bf->end()) {
            node.bitOffset = it->second.bitOffset;
            node.unitBits = it->second.unitBits;
            llvm::Type* curLLVM = ctx.getLLVMType(cur);
            if (!curLLVM) return nullptr;
            return builder.CreateStructGEP(curLLVM, curPtr, it->second.unitIndex, "bfunit");
        }
        if (baseClassName.empty()) break;
        if (!baseType || (baseType->kind != TypeKind::Class &&
                          baseType->kind != TypeKind::Struct)) {
            break;
        }
        llvm::Type* curLLVM = ctx.getLLVMType(cur);
        if (!curLLVM) break;
        curPtr = builder.CreateStructGEP(curLLVM, curPtr, 0, "base");
        cur = baseType;
        if (++walkDepth > 64) break;
    }
    return nullptr;
}

llvm::Value* MemberAccessExprAST::codegen(CodegenContext& ctx) {
    if (auto* ct = ctConstantOrNull(ctx, *this)) return ct;
    // TYP-12: slice `.len` extracts the length field of the {ptr, len} view.
    {
        Type* st = object->type;
        while (st && st->kind == TypeKind::Typedef)
            st = static_cast<TypedefType*>(st)->aliasedType;
        if (st && st->kind == TypeKind::Slice && memberName == "len") {
            llvm::Value* sliceVal = object->codegen(ctx);
            if (!sliceVal) return nullptr;
            auto& builder = ctx.getBuilder();
            if (object->isLValue)
                sliceVal = builder.CreateLoad(ctx.getLLVMType(st), sliceVal);
            return builder.CreateExtractValue(sliceVal, {1}, "slice.len");
        }
    }

    // Review 2.2: property-first sequence access. The method forms remain
    // accepted; both lower to the same fields.
    {
        Type* st = object->type;
        while (st && st->kind == TypeKind::Typedef)
            st = static_cast<TypedefType*>(st)->aliasedType;
        if (st && st->kind == TypeKind::Array && memberName == "len") {
            return llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx.getContext()),
                static_cast<ArrayType*>(st)->size);
        }
        if (st && (st->kind == TypeKind::Str || st->kind == TypeKind::String)) {
            bool isString = st->kind == TypeKind::String;
            if (memberName == "len" || memberName == "capacity" ||
                memberName == "char_count" || memberName == "data") {
                llvm::Value* v = object->codegen(ctx);
                if (!v) return nullptr;
                if (object->isLValue) v = readLValue(ctx, *object, v);
                auto& builder = ctx.getBuilder();
                if (memberName == "data")
                    return builder.CreateExtractValue(v, 0,
                        isString ? "string.data" : "str.data");
                if (memberName == "len")
                    return builder.CreateExtractValue(v, 1,
                        isString ? "string.len" : "str.len");
                if (memberName == "capacity")
                    return builder.CreateExtractValue(v, 2, "string.cap");
                // char_count: UTF-8 code-point scan (builder may be hijacked
                // while synthesizing the helper).
                auto savedIP = builder.saveIP();
                llvm::Function* cntFn = ctx.getUtf8CharCountFn();
                builder.restoreIP(savedIP);
                llvm::Value* ptr = builder.CreateExtractValue(v, 0, "seq.ptr");
                llvm::Value* len = builder.CreateExtractValue(v, 1, "seq.len");
                return builder.CreateCall(cntFn, {ptr, len}, "seq.chars");
            }
        }
    }

    // P1-02 (TYP-13/14): Optional/Result pseudo-fields. DS4 layout: flag
    // (i1) at index 0, value at 1, error at 2 (Result only). Lvalue access
    // yields the field address (writable); rvalue access extracts the field.
    {
        Type* st = object->type;
        while (st && st->kind == TypeKind::Typedef)
            st = static_cast<TypedefType*>(st)->aliasedType;
        if (st && (st->kind == TypeKind::Optional || st->kind == TypeKind::Result)) {
            long idx = -1;
            if (memberName == "valid" || memberName == "ok") idx = 0;
            else if (memberName == "value") idx = 1;
            else if (st->kind == TypeKind::Result && memberName == "error") idx = 2;
            if (idx < 0) return nullptr; // sema already diagnosed
            auto& builder = ctx.getBuilder();
            llvm::Value* agg = object->codegen(ctx);
            if (!agg) return nullptr;
            if (object->isLValue) {
                return builder.CreateStructGEP(ctx.getLLVMType(st), agg,
                                               (unsigned)idx, "optresmember");
            }
            // Rvalue object (call result etc.): the aggregate is already a
            // value — no load here (评审 I3: a spurious load on typedef'd
            // returns crashed the backend with "Do not know how to promote").
            return builder.CreateExtractValue(agg, {(unsigned)idx}, "optresmember");
        }
    }

    llvm::Value* objVal = object->codegen(ctx);
    if (!objVal) return nullptr;

    auto& builder = ctx.getBuilder();
    llvm::Type* objType = nullptr;
    unsigned fieldIndex = 0;

    // Typedefs to aggregates resolve to the underlying type for member access
    // (AGG-17): `typedef union U2 U2; U2 v; v.i` must behave like `U2 v`.
    auto stripTypedefsOf = [](Type* t) -> Type* {
        while (t && t->kind == TypeKind::Typedef) {
            t = static_cast<TypedefType*>(t)->aliasedType;
        }
        return t;
    };
    Type* valType = stripTypedefsOf(object->type);
    Type* valBase =
        (valType && valType->kind == TypeKind::Pointer) ? stripTypedefsOf(valType->base) : nullptr;

    // CG-05: a member of an rvalue aggregate (`make().x`) has no address in
    // the source program. Spill the aggregate to a temporary so the existing
    // GEP paths (inheritance/bit-fields) work, then read the selected field
    // back as a value. Semantically this is an xvalue, not an lvalue.
    bool spilledRvalue = false;
    if (!object->isLValue && valType &&
        (valType->kind == TypeKind::Struct || valType->kind == TypeKind::Class ||
         valType->kind == TypeKind::Union)) {
        llvm::Value* tmp = builder.CreateAlloca(objVal->getType(), nullptr, "agg.tmp");
        builder.CreateStore(objVal, tmp);
        objVal = tmp;
        spilledRvalue = true;
    }
    auto finishMember = [&](llvm::Value* addr) -> llvm::Value* {
        if (!spilledRvalue) return addr;
        if (isBitField) {
            return ctx.loadBitFieldValue(addr, bitOffset, bitWidth, type, unitBits);
        }
        return builder.CreateLoad(ctx.getLLVMType(type), addr, "member.val");
    };

    // Union members all live at offset 0: GEP to field 0 gives the address of
    // the requested member regardless of which member it is.
    if (valType && valType->kind == TypeKind::Union) {
        llvm::Type* unionLLVM = ctx.getLLVMType(valType);
        if (!unionLLVM) return nullptr;
        // P1-07: union 位域——单元即字段 0；记录 bitOffset（恒 0）与 unitBits 供 RMW。
        if (isBitField) {
            auto it = static_cast<UnionType*>(valType)->bitFields.find(memberName);
            if (it != static_cast<UnionType*>(valType)->bitFields.end()) {
                bitOffset = it->second.bitOffset;
                unitBits = it->second.unitBits;
            }
        }
        return finishMember(builder.CreateStructGEP(unionLLVM, objVal, 0, "unionmember"));
    }
    if (valType && valType->kind == TypeKind::Pointer &&
        valBase && valBase->kind == TypeKind::Union) {
        if (object->isLValue) {
            objVal = builder.CreateLoad(llvm::PointerType::get(ctx.getContext(), 0), objVal, "deref");
        }
        llvm::Type* unionLLVM = ctx.getLLVMType(valBase);
        if (!unionLLVM) return nullptr;
        // P1-07: union 位域——记录 bitOffset 与 unitBits 供 RMW。
        if (isBitField) {
            auto it = static_cast<UnionType*>(valBase)->bitFields.find(memberName);
            if (it != static_cast<UnionType*>(valBase)->bitFields.end()) {
                bitOffset = it->second.bitOffset;
                unitBits = it->second.unitBits;
            }
        }
        return builder.CreateStructGEP(unionLLVM, objVal, 0, "unionmember");
    }

    if (valType && valType->kind == TypeKind::Struct) {
        auto* structType = static_cast<StructType*>(valType);
        // P1-07: 位域成员 → 存储单元 GEP（先于名字扫描——物化后 fields
        // 无位域名，普通扫描会误 GEP 到字段 0）。
        if (isBitField) {
            if (auto* gep = emitBitFieldGEP(ctx, valType, objVal, *this)) {
                return finishMember(gep);
            }
        }
        // INH-01: chain-aware GEP first (inherited fields live in the base
        // sub-object); falls back to the local scan below.
        if (auto* gep = emitClassFieldGEP(ctx, valType, objVal, memberName)) {
            return finishMember(gep);
        }
        for (size_t i = 0; i < structType->fields.size(); ++i) {
            if (structType->fields[i].name == memberName) {
                fieldIndex = i;
                break;
            }
        }
        objType = ctx.getLLVMType(valType);
    } else if (valType && valType->kind == TypeKind::Class) {
        auto* classType = static_cast<ClassType*>(valType);
        // P1-07: 位域成员 → 存储单元 GEP（链式：定义类型可能为基类）。
        if (isBitField) {
            if (auto* gep = emitBitFieldGEP(ctx, classType, objVal, *this)) {
                return finishMember(gep);
            }
        }
        if (auto* gep = emitClassFieldGEP(ctx, classType, objVal, memberName)) {
            return finishMember(gep);
        }
        objType = ctx.getLLVMType(valType);
    } else if (valType && valType->kind == TypeKind::Pointer &&
               valBase && valBase->kind == TypeKind::Struct) {
        auto* structType = static_cast<StructType*>(valBase);
        // INH-01: chain-aware GEP first — inherited fields live in the base
        // sub-object behind the pointer.
        // Load the pointer unless the object already produced a pointer value
        // (a cast, a call result, `&x`, ...); only lvalue objects are addresses
        // of a variable that stores the pointer (MEM-10).
        if (object->isLValue) {
            objVal = builder.CreateLoad(llvm::PointerType::get(ctx.getContext(), 0), objVal, "deref");
        }
        // P1-07: 位域成员 → 存储单元 GEP。
        if (isBitField) {
            if (auto* gep = emitBitFieldGEP(ctx, valBase, objVal, *this)) {
                return gep;
            }
        }
        if (auto* gep = emitClassFieldGEP(ctx, valBase, objVal, memberName)) {
            return gep;
        }
        for (size_t i = 0; i < structType->fields.size(); ++i) {
            if (structType->fields[i].name == memberName) {
                fieldIndex = i;
                break;
            }
        }
        objType = ctx.getLLVMType(valBase);
    } else if (valType && valType->kind == TypeKind::Pointer &&
               valBase && valBase->kind == TypeKind::Class) {
        auto* classType = static_cast<ClassType*>(valBase);
        // Load the pointer unless the object already produced a pointer value.
        if (object->isLValue) {
            objVal = builder.CreateLoad(llvm::PointerType::get(ctx.getContext(), 0), objVal, "deref");
        }
        // P1-07: 位域成员 → 存储单元 GEP。
        if (isBitField) {
            if (auto* gep = emitBitFieldGEP(ctx, classType, objVal, *this)) {
                return gep;
            }
        }
        if (auto* gep = emitClassFieldGEP(ctx, classType, objVal, memberName)) {
            return gep;
        }
        objType = ctx.getLLVMType(valBase);
    } else if (valType && valType->kind == TypeKind::Pointer) {
        objType = ctx.getLLVMType(valBase);
    }

    if (!objType) {
        objType = objVal->getType();
    }

    return finishMember(builder.CreateStructGEP(objType, objVal, fieldIndex, "member"));
}

llvm::Value* SizeofExprAST::codegen(CodegenContext& ctx) {
    Type* operandType = sizeofType;
    if (!operandType && expr) operandType = expr->type;
    if (!operandType) return nullptr;

    llvm::Type* llvmType = ctx.getLLVMType(operandType);
    if (!llvmType) return nullptr;

    if (isAlignOf) {
        uint64_t align = ctx.getModule().getDataLayout().getABITypeAlign(llvmType).value();
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), align);
    }
    uint64_t size = ctx.getModule().getDataLayout().getTypeStoreSize(llvmType);
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), size);
}

// PAR-14: `offsetof(T, member)` — field offset via LayoutBuilder (single source
// with compile_time.offset_of / codegen layout).
llvm::Value* OffsetOfExprAST::codegen(CodegenContext& ctx) {
    Type* t = stripTypedefsT12(aggregateType);
    if (!t || (t->kind != TypeKind::Struct && t->kind != TypeKind::Class &&
               t->kind != TypeKind::Union)) {
        LOGE("offsetof requires an aggregate type");
        return nullptr;
    }

    auto fieldTypeOf = [](Type* agg, const std::string& name) -> Type* {
        if (agg->kind == TypeKind::Struct) {
            auto* st = static_cast<StructType*>(agg);
            const auto& fields = st->semanticFields.empty() ? st->fields : st->semanticFields;
            for (auto& f : fields) if (f.name == name) return f.type;
        } else if (agg->kind == TypeKind::Class) {
            auto* ct = static_cast<ClassType*>(agg);
            const auto& fields = ct->semanticFields.empty() ? ct->fields : ct->semanticFields;
            for (auto& f : fields) if (f.name == name) return f.type;
        } else if (agg->kind == TypeKind::Union) {
            auto* ut = static_cast<UnionType*>(agg);
            for (auto& m : ut->members) if (m.name == name) return m.type;
        }
        return nullptr;
    };
    auto converter = [&](Type* tt) { return ctx.getLLVMType(tt); };
    auto resolver = [](const std::string&) -> Type* { return nullptr; };

    // SG-03: path may own subscript expressions (move-only), so inspect it
    // directly instead of copying the designator list.
    std::vector<OffsetDesignator> fallback;
    const std::vector<OffsetDesignator>* stepsPtr = &path;
    if (path.empty()) {
        OffsetDesignator d; d.field = field;
        fallback.push_back(std::move(d));
        stepsPtr = &fallback;
    }
    const std::vector<OffsetDesignator>& steps = *stepsPtr;

    uint64_t total = 0;
    Type* cur = t;
    for (const auto& step : steps) {
        cur = stripTypedefsT12(cur);
        if (!cur) {
            LOGE("offsetof path has an unresolved type");
            return nullptr;
        }
        if (step.isIndex) {
            if (cur->kind != TypeKind::Array) {
                LOGE("offsetof index applied to non-array type");
                return nullptr;
            }
            auto* at = static_cast<ArrayType*>(cur);
            llvm::Type* elemLLVM = ctx.getLLVMType(at->elementType);
            if (!elemLLVM) return nullptr;
            total += static_cast<uint64_t>(step.index) *
                     ctx.getModule().getDataLayout().getTypeAllocSize(elemLLVM);
            cur = at->elementType;
            continue;
        }

        if (cur->kind == TypeKind::Union) {
            // All union members alias offset 0.
            Type* ft = fieldTypeOf(cur, step.field);
            if (!ft) {
                LOGE("unknown field '{}' in offsetof", step.field);
                return nullptr;
            }
            cur = ft;
            continue;
        }
        if (cur->kind != TypeKind::Struct && cur->kind != TypeKind::Class) {
            LOGE("offsetof member '{}' applied to non-aggregate type", step.field);
            return nullptr;
        }
        auto LR = LayoutBuilder::buildAggregate(cur, ctx.getContext(),
            ctx.getModule().getDataLayout(), converter, resolver);
        auto it = LR.fieldOffsets.find(step.field);
        if (it == LR.fieldOffsets.end()) {
            LOGE("unknown field '{}' in offsetof", step.field);
            return nullptr;
        }
        total += it->second;
        Type* ft = fieldTypeOf(cur, step.field);
        if (!ft) {
            LOGE("unknown field type '{}' in offsetof", step.field);
            return nullptr;
        }
        cur = ft;
    }
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx.getContext()), total);
}

llvm::Value* InitializerListExprAST::codegen(CodegenContext& ctx) {
    if (initializers.empty()) return nullptr;
    return initializers.back()->codegen(ctx);
}

// PAR-08: compound literal `(T){...}`. Block scope → stack temp object
// (alloca + zero + aggregate init); file scope → static constant global.
// Array target yields the decayed pointer (lvalue→loadValue); struct yields
// the by-value aggregate (lvalue→readLValue loads it); scalar yields the value.
llvm::Value* CompoundLiteralExprAST::codegen(CodegenContext& ctx) {
    if (!literalType || !initList) return nullptr;
    Type* lt = literalType;
    while (lt && lt->kind == TypeKind::Typedef)
        lt = static_cast<TypedefType*>(lt)->aliasedType;
    llvm::LLVMContext& c = ctx.getContext();
    llvm::Type* llvmType = ctx.getLLVMType(literalType);
    if (!llvmType) return nullptr;

    // Slice target `(T[]){...}`: value view over a stack anonymous array
    // (shared with slice variable literals). At file scope the backing array
    // is a static constant and the view is a constant struct.
    if (lt && lt->kind == TypeKind::Slice) {
        auto* sl = static_cast<SliceType*>(lt);
        const size_t n = initList->initializers.size();
        if (n == 0) return llvm::Constant::getNullValue(llvmType);
        if (ctx.isGlobalScope()) {
            auto* arrType = new ArrayType(sl->elementType, (int)n);
            llvm::Type* arrLLVM = ctx.getLLVMType(arrType);
            if (!arrLLVM) return llvm::Constant::getNullValue(llvmType);
            llvm::Constant* arrConst = buildAggregateConstant(ctx, arrType, initList.get());
            if (!arrConst) arrConst = llvm::Constant::getNullValue(arrLLVM);
            auto* gv = new llvm::GlobalVariable(ctx.getModule(), arrLLVM, /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, arrConst, ".compound.slice");
            auto* i64 = llvm::Type::getInt64Ty(c);
            llvm::Constant* zero = llvm::ConstantInt::get(i64, 0);
            llvm::Constant* idxs[] = {zero, zero};
            llvm::Constant* ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
                arrLLVM, gv, llvm::ArrayRef<llvm::Constant*>(idxs, 2));
            return llvm::ConstantStruct::get(
                llvm::cast<llvm::StructType>(llvmType),
                {ptr, llvm::ConstantInt::get(i64, (uint64_t)n)});
        }
        return emitSliceLiteral(ctx, literalType, initList.get());
    }

    // Scalar target: value of the single initializer.
    if (lt && lt->kind != TypeKind::Array && lt->kind != TypeKind::Struct &&
        lt->kind != TypeKind::Class && lt->kind != TypeKind::Union &&
        lt->kind != TypeKind::Optional &&
        lt->kind != TypeKind::Result) {
        if (initList->initializers.empty()) {
            return llvm::Constant::getNullValue(llvmType);
        }
        llvm::Value* v = initList->initializers[0]->codegen(ctx);
        if (!v) return nullptr;
        if (initList->initializers[0]->isLValue) v = readLValue(ctx, *initList->initializers[0], v);
        return ctx.castValue(v, initList->initializers[0]->type, llvmType);
    }

    // File scope: the literal IS a static constant (no builder insertion point
    // exists in a global initializer — never emit loads here).
    if (ctx.isGlobalScope()) {
        llvm::Constant* initConst = buildAggregateConstant(ctx, literalType, initList.get());
        if (!initConst) initConst = llvm::Constant::getNullValue(llvmType);
        if (lt && lt->kind == TypeKind::Array) {
            // Array target in global scope: materialize a private constant
            // array first, then decay to a pointer to its element 0.
            auto* gv = new llvm::GlobalVariable(ctx.getModule(), llvmType, /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, initConst, ".compound");
            auto* i64 = llvm::Type::getInt64Ty(c);
            llvm::Constant* idx0 = llvm::ConstantInt::get(i64, 0);
            llvm::Constant* idxs[] = {idx0, idx0};
            return llvm::ConstantExpr::getInBoundsGetElementPtr(
                llvmType, gv, llvm::ArrayRef<llvm::Constant*>(idxs, 2));
        }
        // Struct/union: the constant value itself.
        return initConst;
    }

    // Block scope: stack temp.
    llvm::AllocaInst* alloca = ctx.getBuilder().CreateAlloca(llvmType, nullptr, "compound.tmp");
    // Zero the whole object first so unspecified fields stay zero.
    ctx.getBuilder().CreateStore(llvm::Constant::getNullValue(llvmType), alloca);
    emitAggregateInitializer(ctx, alloca, literalType, initList.get());
    if (lt && lt->kind == TypeKind::Array) {
        // Array target: decayed pointer (C semantics — the temporary's address).
        return alloca;
    }
    // Struct/union: return the loaded value (by-value).
    return ctx.getBuilder().CreateLoad(llvmType, alloca, "compound.val");
}

// PAR-11/CG-11: lambda codegen。
// 无捕获 → 生成静态函数并返回函数指针。
// 有捕获 → 生成闭包调用函数 `__lambda_<N>__call(Closure*, params...)`，
// body 内捕获名声明为从闭包加载的局部变量；表达式本身返回闭包 struct 值
// （立即调用由 LambdaCallExprAST 组装：栈上构造闭包 → 调用）。
LambdaExprAST::~LambdaExprAST() = default;

llvm::Value* LambdaExprAST::codegen(CodegenContext& ctx) {
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();

    std::vector<Type*> paramTypes;
    for (auto& p : params) paramTypes.push_back(p->type);
    Type* retType = returnType ? returnType : TypeContext::instance().getVoid();

    // 函数名：无捕获 `__lambda_<N>`，有捕获 `__lambda_<N>__call`。
    static int fnCounter = 0;
    if (lambdaFnName.empty()) {
        lambdaFnName = "__lambda_" + std::to_string(fnCounter++);
        if (closureType) lambdaFnName += "__call";
    }

    llvm::Function* fn = ctx.getModule().getFunction(lambdaFnName);
    if (!fn) {
        std::vector<llvm::Type*> argLLVM;
        if (closureType) argLLVM.push_back(ctx.getLLVMType(
            new Type(TypeKind::Pointer, closureType)));
        for (auto* t : paramTypes) argLLVM.push_back(ctx.getLLVMType(t));
        llvm::FunctionType* FT = llvm::FunctionType::get(
            ctx.getLLVMType(retType), argLLVM, false);
        fn = llvm::Function::Create(FT, llvm::Function::InternalLinkage,
                                    lambdaFnName, ctx.getModule());
    }
    if (fn->size() == 0) {
        // 嵌套生成函数体——保存调用方插入点，body 在 lambda 函数内生成后恢复。
        auto savedIP = builder.saveIP();
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
        builder.SetInsertPoint(entry);
        ctx.pushScope();

        // 有捕获：闭包首参，按捕获字段声明局部变量。
        if (closureType) {
            llvm::Value* closurePtr = fn->getArg(0);
            auto* st = static_cast<StructType*>(closureType);
            for (size_t i = 0; i < captures.size(); ++i) {
                auto& cap = captures[i];
                Type* capTy = st->fields[i].type;
                llvm::Value* fieldPtr = builder.CreateStructGEP(
                    ctx.getLLVMType(closureType), closurePtr, (unsigned)i,
                    "capgep." + cap.name);
                if (cap.byRef) {
                    // 按引用：字段是 `T*`。把该指针直接作为捕获名的地址声明，
                    // body 内读/写经此指针直达原对象（`a = 9` 写穿）。
                    llvm::Value* refPtr = builder.CreateLoad(
                        llvm::PointerType::get(c, 0), fieldPtr);
                    ctx.declareVariable(cap.name, refPtr,
                                        capTy->kind == TypeKind::Pointer ? capTy->base : capTy);
                } else {
                    // 按值：字段是拷贝，加载进局部 alloca。
                    llvm::AllocaInst* alloca = builder.CreateAlloca(
                        ctx.getLLVMType(capTy), nullptr, "cap." + cap.name);
                    llvm::Value* fieldVal = builder.CreateLoad(
                        ctx.getLLVMType(capTy), fieldPtr);
                    builder.CreateStore(fieldVal, alloca);
                    ctx.declareVariable(cap.name, alloca, capTy);
                }
            }
            for (size_t i = 0; i < params.size(); ++i) {
                llvm::Argument* arg = fn->getArg((unsigned)(i + 1));
                arg->setName(params[i]->name);
                llvm::AllocaInst* alloca = builder.CreateAlloca(
                    ctx.getLLVMType(params[i]->type), nullptr, params[i]->name);
                builder.CreateStore(arg, alloca);
                ctx.declareVariable(params[i]->name, alloca, params[i]->type);
            }
        } else {
            for (size_t i = 0; i < params.size(); ++i) {
                llvm::Argument* arg = fn->getArg((unsigned)i);
                arg->setName(params[i]->name);
                llvm::AllocaInst* alloca = builder.CreateAlloca(
                    ctx.getLLVMType(params[i]->type), nullptr, params[i]->name);
                builder.CreateStore(arg, alloca);
                ctx.declareVariable(params[i]->name, alloca, params[i]->type);
            }
        }

        if (body) body->codegen(ctx);
        if (!builder.GetInsertBlock()->getTerminator()) {
            if (retType->kind == TypeKind::Void) builder.CreateRetVoid();
            else builder.CreateUnreachable();
        }
        ctx.popScope();
        builder.restoreIP(savedIP);
    }

    // 无捕获：返回函数指针（bitcast 成指针）。
    if (!closureType) {
        return llvm::ConstantExpr::getBitCast(
            fn, llvm::PointerType::get(c, 0));
    }

    // 有捕获：表达式求值——栈上构造闭包并返回其值。
    llvm::AllocaInst* closureAlloca = builder.CreateAlloca(
        ctx.getLLVMType(closureType), nullptr, "lambda.closure");
    auto* st = static_cast<StructType*>(closureType);
    for (size_t i = 0; i < captures.size(); ++i) {
        auto& cap = captures[i];
        llvm::Value* fieldPtr = builder.CreateStructGEP(
            ctx.getLLVMType(closureType), closureAlloca, (unsigned)i, "capset." + cap.name);
        if (cap.byRef) {
            llvm::Value* addr = ctx.lookupVariableAddr(cap.name);
            if (!addr) return nullptr;
            builder.CreateStore(addr, fieldPtr);
        } else {
            llvm::Value* addr = ctx.lookupVariableAddr(cap.name);
            if (!addr) return nullptr;
            llvm::Value* val = builder.CreateLoad(
                ctx.getLLVMType(st->fields[i].type), addr);
            builder.CreateStore(val, fieldPtr);
        }
    }
    return builder.CreateLoad(ctx.getLLVMType(closureType), closureAlloca,
                              "lambda.closure.val");
}

// PAR-11: 立即调用——构造闭包（复用 lambda 表达式的构造逻辑）+ 调用生成函数。
llvm::Value* LambdaCallExprAST::codegen(CodegenContext& ctx) {
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();

    // 先生成 lambda（无捕获→函数指针；有捕获→闭包值 + 记录调用符号）。
    llvm::Value* lambdaVal = lambda->codegen(ctx);
    (void)lambdaVal;

    llvm::Function* callFn = ctx.getModule().getFunction(lambda->lambdaFnName);
    if (!callFn) return nullptr;

    std::vector<llvm::Value*> argsV;
    // 有捕获：闭包地址作首参（重新构造闭包——lambda 表达式刚构造的值已含捕获，
    // 但立即调用路径直接传地址更稳；捕获拷贝在 lambda codegen 里已完成）。
    if (lambda->closureType) {
        llvm::AllocaInst* closureAlloca = builder.CreateAlloca(
            ctx.getLLVMType(lambda->closureType), nullptr, "lambda.closure");
        auto* st = static_cast<StructType*>(lambda->closureType);
        for (size_t i = 0; i < lambda->captures.size(); ++i) {
            auto& cap = lambda->captures[i];
            llvm::Value* fieldPtr = builder.CreateStructGEP(
                ctx.getLLVMType(lambda->closureType), closureAlloca, (unsigned)i,
                "capset." + cap.name);
            if (cap.byRef) {
                llvm::Value* addr = ctx.lookupVariableAddr(cap.name);
                if (!addr) return nullptr;
                builder.CreateStore(addr, fieldPtr);
            } else {
                llvm::Value* addr = ctx.lookupVariableAddr(cap.name);
                if (!addr) return nullptr;
                llvm::Value* val = builder.CreateLoad(
                    ctx.getLLVMType(st->fields[i].type), addr);
                builder.CreateStore(val, fieldPtr);
            }
        }
        argsV.push_back(closureAlloca);
    }

    for (size_t i = 0; i < args.size(); ++i) {
        llvm::Value* argVal = args[i]->codegen(ctx);
        if (!argVal) return nullptr;
        if (args[i]->isLValue) argVal = readLValue(ctx, *args[i], argVal);
        if (i < lambda->params.size()) {
            argVal = ctx.castValue(argVal, args[i]->type,
                                   ctx.getLLVMType(lambda->params[i]->type));
        }
        argsV.push_back(argVal);
    }

    Type* retType = lambda->returnType;
    if (retType && retType->kind != TypeKind::Void) {
        return builder.CreateCall(callFn, argsV, "lambdacall");
    }
    builder.CreateCall(callFn, argsV);
    return llvm::ConstantInt::get(llvm::Type::getInt32Ty(c), 0);
}

// ===== P1-06 (FMT-01/02/04): builtin str/string method lowering =====

// Materialize the receiver as an rvalue `str`/`string` struct value.
static llvm::Value* builtinObjectValue(CodegenContext& ctx, MethodCallExprAST& node) {
    llvm::Value* objVal = node.object->codegen(ctx);
    if (!objVal) return nullptr;
    if (node.object->isLValue) objVal = readLValue(ctx, *node.object, objVal);
    return objVal;
}

static llvm::Value* codegenBuiltinMethod(CodegenContext& ctx, MethodCallExprAST& node) {
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);

    switch (node.builtinMethod) {
        case BuiltinMethod::SequenceLen: {
            Type* st = node.object->type;
            while (st && st->kind == TypeKind::Typedef)
                st = static_cast<TypedefType*>(st)->aliasedType;
            llvm::Value* obj = node.object->codegen(ctx);
            if (!obj) return nullptr;
            if (st && st->kind == TypeKind::Array)
                return llvm::ConstantInt::get(i64Ty,
                    static_cast<ArrayType*>(st)->size);
            if (node.object->isLValue) obj = readLValue(ctx, *node.object, obj);
            return builder.CreateExtractValue(obj, 1, "slice.len");
        }
        case BuiltinMethod::StrLen: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            return builder.CreateExtractValue(obj, 1, "str.len");
        }
        case BuiltinMethod::StrData: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            return builder.CreateExtractValue(obj, 0, "str.data");
        }
        case BuiltinMethod::StrCharCount: {
            // NOTE: getUtf8CharCountFn() may synthesize IR and re-point the
            // shared builder — save/restore the caller's insertion point.
            auto savedIP = builder.saveIP();
            llvm::Function* cntFn = ctx.getUtf8CharCountFn();
            builder.restoreIP(savedIP);
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* ptr = builder.CreateExtractValue(obj, 0, "str.ptr");
            llvm::Value* len = builder.CreateExtractValue(obj, 1, "str.len");
            return builder.CreateCall(cntFn, {ptr, len}, "str.count");
        }
        case BuiltinMethod::StrCharAt: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* ptr = builder.CreateExtractValue(obj, 0, "str.ptr");
            llvm::Value* idx = node.args[0]->codegen(ctx);
            if (!idx) return nullptr;
            if (node.args[0]->isLValue) idx = readLValue(ctx, *node.args[0], idx);
            idx = builder.CreateSExtOrTrunc(idx, i64Ty, "str.idx64");
            llvm::Value* bytePtr =
                builder.CreateGEP(i8Ty, ptr, idx, "str.bytep");
            return builder.CreateLoad(i8Ty, bytePtr, "str.byte");
        }
        case BuiltinMethod::StrCharLenAt: {
            // Same builder-hijack caveat as StrCharCount above.
            auto savedIP = builder.saveIP();
            llvm::Function* lenAtFn = ctx.getUtf8CharLenAtFn();
            builder.restoreIP(savedIP);
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* ptr = builder.CreateExtractValue(obj, 0, "str.ptr");
            llvm::Value* idx = node.args[0]->codegen(ctx);
            if (!idx) return nullptr;
            if (node.args[0]->isLValue) idx = readLValue(ctx, *node.args[0], idx);
            idx = builder.CreateSExtOrTrunc(idx, i64Ty, "str.idx64");
            return builder.CreateCall(lenAtFn, {ptr, idx}, "str.width");
        }
        case BuiltinMethod::StrFind:
        case BuiltinMethod::StrRFind: {
            // P1-09 (STD-10): needle scan over the byte view. The needle
            // accepts str or string (both carry {ptr, len} prefix fields).
            auto savedIP = builder.saveIP();
            llvm::Function* findFn =
                ctx.getStrFindFn(node.builtinMethod == BuiltinMethod::StrRFind);
            builder.restoreIP(savedIP);
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* ptr = builder.CreateExtractValue(obj, 0, "str.ptr");
            llvm::Value* len = builder.CreateExtractValue(obj, 1, "str.len");
            llvm::Value* nv = node.args[0]->codegen(ctx);
            if (!nv) return nullptr;
            if (node.args[0]->isLValue)
                nv = readLValue(ctx, *node.args[0], nv);
            Type* nArg = node.args[0]->type;
            while (nArg && nArg->kind == TypeKind::Typedef)
                nArg = static_cast<TypedefType*>(nArg)->aliasedType;
            if (nArg && nArg->kind == TypeKind::String) {
                // Project {ptr, len, cap} to the str view.
                llvm::Value* sv = llvm::Constant::getNullValue(
                    ctx.getLLVMType(TypeContext::instance().getStrType()));
                sv = builder.CreateInsertValue(
                    sv, builder.CreateExtractValue(nv, 0, "n.ptr"), {0});
                nv = builder.CreateInsertValue(
                    sv, builder.CreateExtractValue(nv, 1, "n.len"), {1});
            }
            llvm::Value* nptr = builder.CreateExtractValue(nv, 0, "n.sptr");
            llvm::Value* nlen = builder.CreateExtractValue(nv, 1, "n.slen");
            return builder.CreateCall(findFn, {ptr, len, nptr, nlen},
                                      node.builtinMethod == BuiltinMethod::StrFind
                                          ? "str.find"
                                          : "str.rfind");
        }
        case BuiltinMethod::StrSub: {
            // P1-09 (STD-10): zero-copy byte-range view. begin < 0 ||
            // end < begin || end > len aborts (str.sub: out of bounds).
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* ptr = builder.CreateExtractValue(obj, 0, "str.ptr");
            llvm::Value* len = builder.CreateExtractValue(obj, 1, "str.len");
            auto index64 = [&](ExprAST& arg) -> llvm::Value* {
                llvm::Value* v = arg.codegen(ctx);
                if (!v) return nullptr;
                if (arg.isLValue) v = readLValue(ctx, arg, v);
                return builder.CreateSExtOrTrunc(v, i64Ty, "sub.idx64");
            };
            llvm::Value* begin = index64(*node.args[0]);
            if (!begin) return nullptr;
            llvm::Value* end = index64(*node.args[1]);
            if (!end) return nullptr;

            llvm::Function* fn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "sub.ok", fn);
            llvm::BasicBlock* badBB = llvm::BasicBlock::Create(c, "sub.bad", fn);
            auto* neg = builder.CreateICmpSLT(begin, llvm::ConstantInt::get(i64Ty, 0), "sub.neg");
            auto* inverted = builder.CreateICmpSLT(end, begin, "sub.inv");
            auto* over = builder.CreateICmpSGT(end, len, "sub.over");
            auto* bad = builder.CreateOr(neg, builder.CreateOr(inverted, over), "sub.oob");
            builder.CreateCondBr(bad, badBB, okBB);

            builder.SetInsertPoint(badBB);
            {
                std::string prefix = nodeSourcePrefix(node);
                std::string fmtStr = prefix.empty()
                    ? "panic: str.sub: out of bounds\n"
                    : prefix + ": panic: str.sub: out of bounds\n";
                llvm::Value* fmt = builder.CreateGlobalString(fmtStr, ".subfmt");
                emitDprintfAndAbort(ctx, fmt, {});
                builder.CreateUnreachable();
            }

            builder.SetInsertPoint(okBB);
            llvm::Value* subPtr = builder.CreateGEP(i8Ty, ptr, begin, "sub.ptr");
            llvm::Value* subLen = builder.CreateSub(end, begin, "sub.len");
            llvm::Value* v = llvm::Constant::getNullValue(
                ctx.getLLVMType(TypeContext::instance().getStrType()));
            v = builder.CreateInsertValue(v, subPtr, {0});
            return builder.CreateInsertValue(v, subLen, {1});
        }
        case BuiltinMethod::OptionalIsSome:
        case BuiltinMethod::OptionalIsNone: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* flag = builder.CreateExtractValue(obj, {0}, "opt.valid");
            return node.builtinMethod == BuiltinMethod::OptionalIsSome
                ? flag : builder.CreateNot(flag, "opt.none");
        }
        case BuiltinMethod::OptionalValueOr: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* flag = builder.CreateExtractValue(obj, {0}, "opt.valid");
            llvm::Value* value = builder.CreateExtractValue(obj, {1}, "opt.value");
            llvm::Value* fallback = node.args[0]->codegen(ctx);
            if (!fallback) return nullptr;
            if (node.args[0]->isLValue)
                fallback = readLValue(ctx, *node.args[0], fallback);
            fallback = ctx.castValue(fallback, node.args[0]->type,
                                     ctx.getLLVMType(node.type));
            return builder.CreateSelect(flag, value, fallback, "opt.valueor");
        }
        case BuiltinMethod::OptionalUnwrap: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* flag = builder.CreateExtractValue(obj, {0}, "opt.valid");
            llvm::Value* value = builder.CreateExtractValue(obj, {1}, "opt.value");
            llvm::Function* fn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "opt.unwrap.ok", fn);
            llvm::BasicBlock* badBB = llvm::BasicBlock::Create(c, "opt.unwrap.bad", fn);
            builder.CreateCondBr(flag, okBB, badBB);
            builder.SetInsertPoint(badBB);
            {
                std::string prefix = nodeSourcePrefix(node);
                std::string msg = prefix.empty()
                    ? "panic: optional.unwrap: invalid value\n"
                    : prefix + ": panic: optional.unwrap: invalid value\n";
                llvm::Value* fmt = builder.CreateGlobalString(msg, ".optunwrapfmt");
                emitDprintfAndAbort(ctx, fmt, {});
                builder.CreateUnreachable();
            }
            builder.SetInsertPoint(okBB);
            return value;
        }
        case BuiltinMethod::ResultIsOk:
        case BuiltinMethod::ResultIsErr: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* flag = builder.CreateExtractValue(obj, {0}, "res.ok");
            return node.builtinMethod == BuiltinMethod::ResultIsOk
                ? flag : builder.CreateNot(flag, "res.err");
        }
        case BuiltinMethod::ResultValueOr: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* flag = builder.CreateExtractValue(obj, {0}, "res.ok");
            llvm::Value* value = builder.CreateExtractValue(obj, {1}, "res.value");
            llvm::Value* fallback = node.args[0]->codegen(ctx);
            if (!fallback) return nullptr;
            if (node.args[0]->isLValue)
                fallback = readLValue(ctx, *node.args[0], fallback);
            fallback = ctx.castValue(fallback, node.args[0]->type,
                                     ctx.getLLVMType(node.type));
            return builder.CreateSelect(flag, value, fallback, "res.valueor");
        }
        case BuiltinMethod::ResultUnwrap: {
            llvm::Value* obj = builtinObjectValue(ctx, node);
            if (!obj) return nullptr;
            llvm::Value* flag = builder.CreateExtractValue(obj, {0}, "res.ok");
            llvm::Value* value = builder.CreateExtractValue(obj, {1}, "res.value");
            llvm::Function* fn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "res.unwrap.ok", fn);
            llvm::BasicBlock* badBB = llvm::BasicBlock::Create(c, "res.unwrap.bad", fn);
            builder.CreateCondBr(flag, okBB, badBB);
            builder.SetInsertPoint(badBB);
            {
                std::string prefix = nodeSourcePrefix(node);
                std::string msg = prefix.empty()
                    ? "panic: result.unwrap: error value\n"
                    : prefix + ": panic: result.unwrap: error value\n";
                llvm::Value* fmt = builder.CreateGlobalString(msg, ".resunwrapfmt");
                emitDprintfAndAbort(ctx, fmt, {});
                builder.CreateUnreachable();
            }
            builder.SetInsertPoint(okBB);
            return value;
        }
        case BuiltinMethod::StringNew:
        case BuiltinMethod::StringDestroy:
        case BuiltinMethod::StringAppend:
        case BuiltinMethod::StringPush:
        case BuiltinMethod::StringLen:
        case BuiltinMethod::StringCapacity:
        case BuiltinMethod::StringData:
        case BuiltinMethod::StringCharCount:
            return codegenStringMethod(ctx, node);
        case BuiltinMethod::None:
            break;
    }
    return nullptr;
}

// ===== P1-06 (FMT-02): string memory/mutation lowering =====
// Mutating methods take the receiver's ADDRESS: an lvalue object already
// codegens to its storage address; an rvalue (e.g. `string.new("ab")`) is
// materialized into a temporary alloca first so chains keep working.

// Receiver address for a string method call.
static llvm::Value* builtinObjectAddr(CodegenContext& ctx, MethodCallExprAST& node) {
    auto& builder = ctx.getBuilder();
    llvm::Value* objVal = node.object->codegen(ctx);
    if (!objVal) return nullptr;
    if (node.object->isLValue) return objVal; // already the storage address
    llvm::Type* strTy = objVal->getType();
    llvm::Value* tmp = builder.CreateAlloca(strTy, nullptr, "string.tmp");
    builder.CreateStore(objVal, tmp);
    return tmp;
}

// str-typed argument value for append/new (String -> str projected here).
static llvm::Value* builtinStrArg(CodegenContext& ctx, ExprAST* arg) {
    auto& builder = ctx.getBuilder();
    llvm::Value* v = arg->codegen(ctx);
    if (!v) return nullptr;
    if (arg->isLValue) v = readLValue(ctx, *arg, v);
    Type* t = arg->type;
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    if (t && t->kind == TypeKind::String) {
        llvm::Value* ptr = builder.CreateExtractValue(v, 0, "arg.ptr");
        llvm::Value* len = builder.CreateExtractValue(v, 1, "arg.len");
        llvm::Value* sv = llvm::Constant::getNullValue(
            ctx.getLLVMType(TypeContext::instance().getStrType()));
        sv = builder.CreateInsertValue(sv, ptr, {0});
        return builder.CreateInsertValue(sv, len, {1});
    }
    return v;
}

static llvm::FunctionCallee declareLibcFn(CodegenContext& ctx, const char* name,
                                          llvm::Type* ret, llvm::ArrayRef<llvm::Type*> params) {
    llvm::LLVMContext& c = ctx.getContext();
    return ctx.getModule().getOrInsertFunction(
        name, llvm::FunctionType::get(ret, params, false));
}

static llvm::Value* codegenStringMethod(CodegenContext& ctx, MethodCallExprAST& node) {
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* ptrTy = llvm::PointerType::get(c, 0);
    auto* strLLVM = ctx.getLLVMType(TypeContext::instance().getStringType());

    switch (node.builtinMethod) {
        case BuiltinMethod::StringLen: {
            llvm::Value* addr = builtinObjectAddr(ctx, node);
            if (!addr) return nullptr;
            llvm::Value* v = builder.CreateLoad(strLLVM, addr, "string.v");
            return builder.CreateExtractValue(v, 1, "string.len");
        }
        case BuiltinMethod::StringCapacity: {
            llvm::Value* addr = builtinObjectAddr(ctx, node);
            if (!addr) return nullptr;
            llvm::Value* v = builder.CreateLoad(strLLVM, addr, "string.v");
            return builder.CreateExtractValue(v, 2, "string.cap");
        }
        case BuiltinMethod::StringData: {
            llvm::Value* addr = builtinObjectAddr(ctx, node);
            if (!addr) return nullptr;
            llvm::Value* v = builder.CreateLoad(strLLVM, addr, "string.v");
            return builder.CreateExtractValue(v, 0, "string.data");
        }
        case BuiltinMethod::StringCharCount: {
            llvm::Value* addr = builtinObjectAddr(ctx, node);
            if (!addr) return nullptr;
            llvm::Value* v = builder.CreateLoad(strLLVM, addr, "string.v");
            llvm::Value* ptr = builder.CreateExtractValue(v, 0, "string.ptr");
            llvm::Value* len = builder.CreateExtractValue(v, 1, "string.len");
            auto savedIP = builder.saveIP();
            llvm::Function* cntFn = ctx.getUtf8CharCountFn();
            builder.restoreIP(savedIP);
            return builder.CreateCall(cntFn, {ptr, len}, "string.chars");
        }
        case BuiltinMethod::StringNew: {
            // cap = max(len + 1, 1); the extra byte is the C terminator that
            // makes string.data safe to hand to libc APIs.
            auto savedIP = builder.saveIP();
            auto mallocFn = declareLibcFn(ctx, "malloc", ptrTy, {i64Ty});
            auto memcpyFn = declareLibcFn(ctx, "memcpy", ptrTy, {ptrTy, ptrTy, i64Ty});
            builder.restoreIP(savedIP);
            llvm::Value* s = builtinStrArg(ctx, node.args[0].get());
            if (!s) return nullptr;
            llvm::Value* sptr = builder.CreateExtractValue(s, 0, "new.sptr");
            llvm::Value* slen = builder.CreateExtractValue(s, 1, "new.slen");
            llvm::Value* one = llvm::ConstantInt::get(i64Ty, 1);
            llvm::Value* cap = builder.CreateAdd(slen, one, "new.len1");
            cap = builder.CreateSelect(
                builder.CreateICmpUGT(cap, one, "new.gt1"), cap, one, "new.cap");
            llvm::Value* ptr = builder.CreateCall(mallocFn, {cap}, "new.ptr");
            builder.CreateCall(memcpyFn, {ptr, sptr, slen});
            llvm::Value* end = builder.CreateGEP(i8Ty, ptr, slen, "new.end");
            builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0), end);
            llvm::Value* v = llvm::Constant::getNullValue(strLLVM);
            v = builder.CreateInsertValue(v, ptr, {0});
            v = builder.CreateInsertValue(v, slen, {1});
            return builder.CreateInsertValue(v, cap, {2});
        }
        case BuiltinMethod::StringDestroy: {
            auto savedIP = builder.saveIP();
            auto freeFn = declareLibcFn(ctx, "free", llvm::Type::getVoidTy(c), {ptrTy});
            builder.restoreIP(savedIP);
            llvm::Value* addr = builtinObjectAddr(ctx, node);
            if (!addr) return nullptr;
            llvm::Value* v = builder.CreateLoad(strLLVM, addr, "destroy.v");
            llvm::Value* ptr = builder.CreateExtractValue(v, 0, "destroy.ptr");
            builder.CreateCall(freeFn, {ptr});
            // Zero the header: a second destroy frees null (harmless) and
            // len()/capacity() observe the destruction.
            builder.CreateStore(llvm::Constant::getNullValue(strLLVM), addr);
            return llvm::Constant::getNullValue(llvm::Type::getInt32Ty(c));
        }
        case BuiltinMethod::StringAppend: {
            auto savedIP = builder.saveIP();
            auto reallocFn = declareLibcFn(ctx, "realloc", ptrTy, {ptrTy, i64Ty});
            auto memmoveFn = declareLibcFn(ctx, "memmove", ptrTy, {ptrTy, ptrTy, i64Ty});
            builder.restoreIP(savedIP);
            llvm::Value* addr = builtinObjectAddr(ctx, node);
            if (!addr) return nullptr;
            llvm::Value* s = builtinStrArg(ctx, node.args[0].get());
            if (!s) return nullptr;
            llvm::Value* v = builder.CreateLoad(strLLVM, addr, "append.v");
            llvm::Value* ptr = builder.CreateExtractValue(v, 0, "append.ptr");
            llvm::Value* len = builder.CreateExtractValue(v, 1, "append.len");
            llvm::Value* cap = builder.CreateExtractValue(v, 2, "append.cap");
            llvm::Value* sptr = builder.CreateExtractValue(s, 0, "append.sptr");
            llvm::Value* slen = builder.CreateExtractValue(s, 1, "append.slen");
            llvm::Value* need = builder.CreateAdd(len, slen, "append.need");
            // Reserve one byte for the implicit NUL terminator (libc interop).
            llvm::Value* need1 = builder.CreateAdd(
                need, llvm::ConstantInt::get(i64Ty, 1), "append.need1");

            llvm::Function* fn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* growBB = llvm::BasicBlock::Create(c, "append.grow", fn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(c, "append.merge", fn);
            llvm::Value* mustGrow = builder.CreateICmpUGT(need1, cap, "append.grow?");
            llvm::BasicBlock* condBB = builder.GetInsertBlock();
            builder.CreateCondBr(mustGrow, growBB, mergeBB);

            builder.SetInsertPoint(growBB);
            llvm::Value* doubled = builder.CreateMul(cap, llvm::ConstantInt::get(i64Ty, 2), "append.dbl");
            llvm::Value* grown = builder.CreateSelect(
                builder.CreateICmpUGT(doubled, need1, "append.dbl?"), doubled, need1, "append.cap2");
            llvm::Value* rptr = builder.CreateCall(reallocFn, {ptr, grown}, "append.rptr");
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            llvm::PHINode* ptr2 = builder.CreatePHI(ptrTy, 2, "append.ptr2");
            llvm::PHINode* cap2 = builder.CreatePHI(i64Ty, 2, "append.cap2");
            ptr2->addIncoming(ptr, condBB);
            cap2->addIncoming(cap, condBB);
            ptr2->addIncoming(rptr, growBB);
            cap2->addIncoming(grown, growBB);

            llvm::Value* dst = builder.CreateGEP(i8Ty, ptr2, len, "append.dst");
            // 终审 I3: the source view may alias the receiver's own buffer
            // (`s.append(s)`, or any view into it). After a moving realloc
            // the old pointer is dangling — re-project a self-aliased source
            // onto the (content-preserving) realloc result, and use memmove
            // for the (now possible) overlap with dst.
            llvm::Value* capEnd = builder.CreateGEP(i8Ty, ptr, cap, "append.cepend");
            llvm::Value* selfAlias = builder.CreateAnd(
                builder.CreateICmpUGE(sptr, ptr, "append.alias1"),
                builder.CreateICmpULT(sptr, capEnd, "append.alias2"), "append.alias");
            llvm::Value* src = builder.CreateSelect(selfAlias, ptr2, sptr, "append.src");
            builder.CreateCall(memmoveFn, {dst, src, slen});
            llvm::Value* end = builder.CreateGEP(i8Ty, ptr2, need, "append.end");
            builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0), end);
            llvm::Value* v2 = llvm::Constant::getNullValue(strLLVM);
            v2 = builder.CreateInsertValue(v2, ptr2, {0});
            v2 = builder.CreateInsertValue(v2, need, {1});
            v2 = builder.CreateInsertValue(v2, cap2, {2});
            builder.CreateStore(v2, addr);
            return llvm::Constant::getNullValue(llvm::Type::getInt32Ty(c));
        }
        case BuiltinMethod::StringPush: {
            auto savedIP = builder.saveIP();
            auto reallocFn = declareLibcFn(ctx, "realloc", ptrTy, {ptrTy, i64Ty});
            builder.restoreIP(savedIP);
            llvm::Value* addr = builtinObjectAddr(ctx, node);
            if (!addr) return nullptr;
            llvm::Value* ch = node.args[0]->codegen(ctx);
            if (!ch) return nullptr;
            if (node.args[0]->isLValue) ch = readLValue(ctx, *node.args[0], ch);
            ch = builder.CreateSExtOrTrunc(ch, i8Ty, "push.c8");

            llvm::Value* v = builder.CreateLoad(strLLVM, addr, "push.v");
            llvm::Value* ptr = builder.CreateExtractValue(v, 0, "push.ptr");
            llvm::Value* len = builder.CreateExtractValue(v, 1, "push.len");
            llvm::Value* cap = builder.CreateExtractValue(v, 2, "push.cap");
            llvm::Value* need = builder.CreateAdd(len, llvm::ConstantInt::get(i64Ty, 1), "push.need");
            llvm::Value* need1 = builder.CreateAdd(
                need, llvm::ConstantInt::get(i64Ty, 1), "push.need1");

            llvm::Function* fn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* growBB = llvm::BasicBlock::Create(c, "push.grow", fn);
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(c, "push.merge", fn);
            llvm::Value* mustGrow = builder.CreateICmpUGT(need1, cap, "push.grow?");
            llvm::BasicBlock* condBB = builder.GetInsertBlock();
            builder.CreateCondBr(mustGrow, growBB, mergeBB);

            builder.SetInsertPoint(growBB);
            llvm::Value* doubled = builder.CreateMul(cap, llvm::ConstantInt::get(i64Ty, 2), "push.dbl");
            llvm::Value* grown = builder.CreateSelect(
                builder.CreateICmpUGT(doubled, need1, "push.dbl?"), doubled, need1, "push.cap2");
            llvm::Value* rptr = builder.CreateCall(reallocFn, {ptr, grown}, "push.rptr");
            builder.CreateBr(mergeBB);

            builder.SetInsertPoint(mergeBB);
            llvm::PHINode* ptr2 = builder.CreatePHI(ptrTy, 2, "push.ptr2");
            llvm::PHINode* cap2 = builder.CreatePHI(i64Ty, 2, "push.cap2");
            ptr2->addIncoming(ptr, condBB);
            cap2->addIncoming(cap, condBB);
            ptr2->addIncoming(rptr, growBB);
            cap2->addIncoming(grown, growBB);

            llvm::Value* dst = builder.CreateGEP(i8Ty, ptr2, len, "push.dst");
            builder.CreateStore(ch, dst);
            llvm::Value* end = builder.CreateGEP(i8Ty, ptr2, need, "push.end");
            builder.CreateStore(llvm::ConstantInt::get(i8Ty, 0), end);
            llvm::Value* v2 = llvm::Constant::getNullValue(strLLVM);
            v2 = builder.CreateInsertValue(v2, ptr2, {0});
            v2 = builder.CreateInsertValue(v2, need, {1});
            v2 = builder.CreateInsertValue(v2, cap2, {2});
            builder.CreateStore(v2, addr);
            return llvm::Constant::getNullValue(llvm::Type::getInt32Ty(c));
        }
        default:
            return nullptr;
    }
}

// P1-06 (FMT-04): str_from_c — strlen over the buffer (or an identity view
// for a str argument), assembled into the canonical `str` struct.
static llvm::Value* codegenStrFromC(CodegenContext& ctx, CallExprAST& node) {
    auto& builder = ctx.getBuilder();
    llvm::LLVMContext& c = ctx.getContext();
    auto* i64Ty = llvm::Type::getInt64Ty(c);

    llvm::Value* arg = node.args[0]->codegen(ctx);
    if (!arg) return nullptr;
    if (node.args[0]->isLValue) arg = readLValue(ctx, *node.args[0], arg);

    Type* argTy = node.args[0]->type;
    while (argTy && argTy->kind == TypeKind::Typedef) {
        argTy = static_cast<TypedefType*>(argTy)->aliasedType;
    }
    if (argTy && argTy->kind == TypeKind::Str) {
        return arg; // identity view
    }

    // P1-06 (FMT-04): runtime buffers are validated (E2015's runtime
    // companion) — an invalid sequence panics, never silently mis-decodes.
    // Helper synthesis re-points the shared builder; save/restore around it.
    auto savedIP = builder.saveIP();
    auto* validateFn = ctx.getUtf8ValidateFn();
    auto* strlenTy = llvm::FunctionType::get(i64Ty, {llvm::PointerType::get(c, 0)}, false);
    llvm::FunctionCallee strlenFn =
        ctx.getModule().getOrInsertFunction("strlen", strlenTy);
    builder.restoreIP(savedIP);

    llvm::Value* len = builder.CreateCall(strlenFn, {arg}, "strfromc.len");
    llvm::Value* valid = builder.CreateCall(validateFn, {arg, len}, "strfromc.valid");

    llvm::Function* fn = builder.GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "utf8.ok", fn);
    llvm::BasicBlock* badBB = llvm::BasicBlock::Create(c, "utf8.bad", fn);
    builder.CreateCondBr(valid, okBB, badBB);

    builder.SetInsertPoint(badBB);
    {
        std::string prefix = nodeSourcePrefix(node);
        std::string fmtStr = prefix.empty()
            ? "panic: invalid UTF-8 sequence\n"
            : prefix + ": panic: invalid UTF-8 sequence\n";
        llvm::Value* fmt = builder.CreateGlobalString(fmtStr, ".utf8fmt");
        emitDprintfAndAbort(ctx, fmt, {});
        builder.CreateUnreachable();
    }

    builder.SetInsertPoint(okBB);
    llvm::Value* v = llvm::Constant::getNullValue(ctx.getLLVMType(TypeContext::instance().getStrType()));
    v = builder.CreateInsertValue(v, arg, {0});
    return builder.CreateInsertValue(v, len, {1});
}

// CT-07: compile_time reflection — name_of/fields_of/attributes_of. Sema has
// recorded the data in ReflectionRegistry (keyed by the target type's mangled
// name); this materializes the runtime value: a {ptr,len} string_view for
// name_of, or a {ptr,len} slice over a global constant array for the others.
static llvm::Value* codegenReflection(CodegenContext& ctx, MethodCallExprAST& node) {
    auto& registry = smc::ReflectionRegistry::instance();
    std::string key = typeToMangled(node.reflectType);
    llvm::LLVMContext& c = ctx.getContext();
    auto& builder = ctx.getBuilder();
    llvm::Type* i64 = llvm::Type::getInt64Ty(c);

    auto strViewType = [&]() -> llvm::StructType* {
        if (llvm::StructType* t = llvm::StructType::getTypeByName(c, "str")) return t;
        return llvm::StructType::create(c, {llvm::PointerType::get(c, 0), i64}, "str");
    };
    auto buildStrView = [&](const std::string& s) -> llvm::Constant* {
        llvm::Value* ptr = builder.CreateGlobalString(s, ".rflstr");
        return llvm::ConstantStruct::get(strViewType(),
            {llvm::dyn_cast<llvm::Constant>(ptr),
             llvm::ConstantInt::get(i64, s.size())});
    };

    if (node.reflectKind == MethodCallExprAST::ReflectionKind::NameOf) {
        std::string name = registry.getName(key);
        return buildStrView(name);
    }

    // fields_of / attributes_of — a global [N x Elem] array + {ptr, len} view.
    TypeContext& tc = TypeContext::instance();
    llvm::StructType* elemStruct = nullptr;
    llvm::Type* elemLLVM = nullptr;
    std::vector<llvm::Constant*> elems;

    if (node.reflectKind == MethodCallExprAST::ReflectionKind::FieldsOf) {
        elemLLVM = ctx.getLLVMType(tc.getFieldInfoType());
        elemStruct = llvm::dyn_cast<llvm::StructType>(elemLLVM);
        if (const auto* fields = registry.getFields(key)) {
            for (auto& f : *fields) {
                elems.push_back(llvm::ConstantStruct::get(elemStruct,
                    {buildStrView(f.name),
                     llvm::ConstantInt::get(i64, static_cast<uint64_t>(f.offset)),
                     llvm::ConstantInt::get(i64, static_cast<uint64_t>(f.size))}));
            }
        }
    } else { // AttributesOf
        elemLLVM = ctx.getLLVMType(tc.getAttributeType());
        elemStruct = llvm::dyn_cast<llvm::StructType>(elemLLVM);
        if (const auto* attrs = registry.getAttributes(key)) {
            for (auto& a : *attrs) {
                elems.push_back(llvm::ConstantStruct::get(elemStruct, {buildStrView(a.name)}));
            }
        }
    }

    llvm::ArrayType* arrTy = llvm::ArrayType::get(elemLLVM, elems.empty() ? 0 : elems.size());
    auto* garr = new llvm::GlobalVariable(ctx.getModule(), arrTy, /*isConstant=*/true,
        llvm::GlobalVariable::InternalLinkage,
        elems.empty() ? llvm::ConstantAggregateZero::get(arrTy)
                      : llvm::ConstantArray::get(arrTy, elems),
        ".rflarr");

    Type* sliceT = node.reflectKind == MethodCallExprAST::ReflectionKind::FieldsOf
                       ? tc.getSliceType(tc.getFieldInfoType())
                       : tc.getSliceType(tc.getAttributeType());
    llvm::Type* sliceLLVM = ctx.getLLVMType(sliceT);
    llvm::Value* v = llvm::Constant::getNullValue(sliceLLVM);
    v = builder.CreateInsertValue(v, garr, {0});
    return builder.CreateInsertValue(v, llvm::ConstantInt::get(i64, elems.size()), {1});
}
