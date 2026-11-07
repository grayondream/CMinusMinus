#include "CodegenContext.h"
#include "ast/Type.h"
#include "ast/LayoutBuilder.h"
#include "ast/Mangle.h"
#include "ast/Expr.h"
#include "ast/Symbol.h"
#include "support/Log.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Metadata.h"

// P1-07 / CG-09: 位域读取符号扩展判定——无符号整型族 zext，其余（含 isize）
// sext。
static bool bitFieldIsUnsigned(Type* t) {
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::UInt8:
        case TypeKind::UInt16:
        case TypeKind::UInt32:
        case TypeKind::UInt64:
        case TypeKind::UInt128:
        case TypeKind::USize:
            return true;
        default:
            return false;
    }
}

CodegenContext::CodegenContext()
    : context(std::make_unique<llvm::LLVMContext>()),
      builder(*context),
      module(std::make_unique<llvm::Module>("cmm", *context)),
      diBuilder(std::make_unique<llvm::DIBuilder>(*module)) {
    pushScope();
}

void CodegenContext::setSourceFile(const std::string& file) {
    sourceFileName = file;
    diFile = diBuilder->createFile(file, ".");
    diCompileUnit = diBuilder->createCompileUnit(
        llvm::dwarf::DW_LANG_C, diFile, "Cmm Compiler",
        false, "", 0);
    diCurrentScope = diFile;
}

llvm::DIType* getDIType(CodegenContext& ctx, Type* type) {
    if (!type) return nullptr;

    auto& builder = ctx.getDIBuilder();

    switch (type->kind) {
        case TypeKind::Void:
            return nullptr;
        case TypeKind::Char:
            return builder.createBasicType("char", 8, llvm::dwarf::DW_ATE_signed_char);
        case TypeKind::Bool:
            return builder.createBasicType("bool", 1, llvm::dwarf::DW_ATE_boolean);
        // 新增整数类型
        case TypeKind::Int8:
            return builder.createBasicType("int8", 8, llvm::dwarf::DW_ATE_signed);
        case TypeKind::Int16:
            return builder.createBasicType("int16", 16, llvm::dwarf::DW_ATE_signed);
        case TypeKind::Int32:
            return builder.createBasicType("int32", 32, llvm::dwarf::DW_ATE_signed);
        case TypeKind::Int64:
            return builder.createBasicType("int64", 64, llvm::dwarf::DW_ATE_signed);
        case TypeKind::Int128:
            return builder.createBasicType("int128", 128, llvm::dwarf::DW_ATE_signed);
        case TypeKind::UInt8:
            return builder.createBasicType("uint8", 8, llvm::dwarf::DW_ATE_unsigned);
        case TypeKind::UInt16:
            return builder.createBasicType("uint16", 16, llvm::dwarf::DW_ATE_unsigned);
        case TypeKind::UInt32:
            return builder.createBasicType("uint32", 32, llvm::dwarf::DW_ATE_unsigned);
        case TypeKind::UInt64:
            return builder.createBasicType("uint64", 64, llvm::dwarf::DW_ATE_unsigned);
        case TypeKind::UInt128:
            return builder.createBasicType("uint128", 128, llvm::dwarf::DW_ATE_unsigned);
        case TypeKind::ISize:
            return builder.createBasicType("isize", 64, llvm::dwarf::DW_ATE_signed);
        case TypeKind::USize:
            return builder.createBasicType("usize", 64, llvm::dwarf::DW_ATE_unsigned);
        case TypeKind::Float32:
            return builder.createBasicType("float32", 32, llvm::dwarf::DW_ATE_float);
        case TypeKind::Float64:
            return builder.createBasicType("float64", 64, llvm::dwarf::DW_ATE_float);
        case TypeKind::Pointer: {
            auto* pointee = getDIType(ctx, type->base);
            if (!pointee) {
                auto* opaque = builder.createUnspecifiedType("void");
                return builder.createPointerType(opaque, 64);
            }
            return builder.createPointerType(pointee, 64);
        }
        default:
            return builder.createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
    }
}

void CodegenContext::emitFunctionDebug(llvm::Function* func, Type* returnType,
                                       const std::string& name, unsigned line) {
    if (!diBuilder || !diFile) return;

    auto* retDIType = getDIType(*this, returnType);

    std::vector<llvm::Metadata*> paramTypes;
    if (retDIType) paramTypes.push_back(retDIType);

    auto* typeArray = llvm::MDTuple::get(*context, paramTypes);
    auto* subroutineType = diBuilder->createSubroutineType(
        llvm::DITypeRefArray(typeArray));

    auto* sp = diBuilder->createFunction(
        diFile, name, name, diFile,
        line, subroutineType,
        line, llvm::DINode::FlagZero,
        llvm::DISubprogram::SPFlagDefinition);

    func->setSubprogram(sp);
    diCurrentScope = sp;
}

void CodegenContext::emitVariableDebug(llvm::AllocaInst* alloca, const std::string& name,
                                       Type* type, unsigned line) {
    if (!diBuilder || !diFile || !diCurrentScope) return;

    auto* diType = getDIType(*this, type);
    if (!diType) return;

    diBuilder->createAutoVariable(
        diCurrentScope, name, diFile, line, diType);
}

void CodegenContext::setDebugLocation(unsigned line, unsigned col) {
    if (!diCurrentScope) return;
    auto* dl = llvm::DILocation::get(*context, line, col, diCurrentScope);
    builder.SetCurrentDebugLocation(dl);
}

void CodegenContext::finalizeDebugInfo() {
    if (diBuilder) {
        diBuilder->finalize();
    }
}

void CodegenContext::pushScope() {
    Scope* parent = scopes.empty() ? nullptr : scopes.back().get();
    scopes.push_back(std::make_unique<Scope>(parent));
}

void CodegenContext::popScope() {
    if (scopes.size() > 1) {
        scopes.pop_back();
    }
}

Scope* CodegenContext::currentScope() {
    return scopes.back().get();
}

bool CodegenContext::isGlobalScope() const {
    return scopes.size() == 1;
}

llvm::Value* CodegenContext::lookupVariable(const std::string& name) {
    Symbol* sym = currentScope()->lookup(name);
    if (!sym || !sym->value) return nullptr;
    return builder.CreateLoad(getLLVMType(sym->type), sym->value, name);
}

llvm::Value* CodegenContext::lookupVariableAddr(const std::string& name) {
    Symbol* sym = currentScope()->lookup(name);
    if (!sym || !sym->value) return nullptr;
    return sym->value;
}

llvm::Value* CodegenContext::loadValue(llvm::Value* ptr, Type* type) {
    if (!ptr) return nullptr;
    if (!ptr->getType()->isPointerTy()) return ptr;

    Type* t = type;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }

    // Arrays decay to a pointer to their first element in value contexts.
    if (t && t->kind == TypeKind::Array) {
        llvm::Type* arrTy = getLLVMType(t);
        if (!arrTy) return ptr;
        auto* i64 = llvm::Type::getInt64Ty(*context);
        return builder.CreateInBoundsGEP(
            arrTy, ptr,
            {llvm::ConstantInt::get(i64, 0), llvm::ConstantInt::get(i64, 0)},
            "decay");
    }

    // CON-01: atomic-typed lvalues load through an integer carrier with the
    // natural alignment of that carrier. This both fixes i64 fields (which
    // previously emitted `align 4` and fell back to `__atomic_load`) and
    // avoids invalid LLVM atomic `i1` loads for bool.
    if (t && t->isAtomic) {
        unsigned bits = 0;
        llvm::Type* carrier = getAtomicCarrierType(t, bits);
        if (carrier) {
            llvm::Value* addr = ptr;
            llvm::Type* carrierPtr = llvm::PointerType::get(*context, 0);
            if (addr->getType() != carrierPtr) {
                addr = builder.CreateBitCast(addr, carrierPtr, "atomic.addr");
            }
            llvm::LoadInst* load =
                builder.CreateLoad(carrier, addr, "atomic.load");
            load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
            load->setAlignment(llvm::Align(bits / 8));
            return valueFromAtomicCarrier(load, t, carrier);
        }
    }

    llvm::Type* loadType = t ? getLLVMType(t) : llvm::Type::getInt32Ty(*context);
    if (!loadType) return ptr;
    llvm::LoadInst* load = builder.CreateLoad(loadType, ptr, "loadtmp");
    if (t && t->isAtomic) {
        load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    }
    return load;
}

llvm::Value* CodegenContext::loadBitFieldValue(llvm::Value* unitAddr,
                                               uint64_t bitOffset,
                                               uint32_t bitWidth,
                                               Type* fieldType,
                                               uint32_t unitBits) {
    if (!unitAddr) return nullptr;
    llvm::Type* unit = llvm::Type::getIntNTy(*context, unitBits);
    Type* ft = fieldType;
    while (ft && ft->kind == TypeKind::Typedef) {
        ft = static_cast<TypedefType*>(ft)->aliasedType;
    }
    llvm::Value* raw = builder.CreateLoad(unit, unitAddr, "bf.load");
    if (bitOffset != 0) {
        raw = builder.CreateLShr(raw,
                                 llvm::ConstantInt::get(unit, bitOffset),
                                 "bf.shr");
    }
    if (ft && ft->kind == TypeKind::Bool) {
        return builder.CreateTrunc(raw, llvm::Type::getInt1Ty(*context), "bf.bool");
    }
    llvm::Value* narrow =
        builder.CreateTrunc(raw, llvm::Type::getIntNTy(*context, bitWidth), "bf.trunc");
    // 结果按基类型位宽扩展（非 packed 下与单元位宽一致；packed 区域单元可更宽）。
    llvm::Type* baseLLVM = getLLVMType(fieldType);
    if (bitFieldIsUnsigned(ft)) {
        return builder.CreateZExt(narrow, baseLLVM, "bf.zext");
    }
    return builder.CreateSExt(narrow, baseLLVM, "bf.sext");
}

void CodegenContext::storeBitFieldValue(llvm::Value* unitAddr,
                                        uint64_t bitOffset,
                                        uint32_t bitWidth,
                                        Type* fieldType,
                                        llvm::Value* val,
                                        uint32_t unitBits) {
    if (!unitAddr || !val) return;
    llvm::Type* unit = llvm::Type::getIntNTy(*context, unitBits);
    // 值扩展/截断到单元位宽（packed 区域单元可宽于基类型）。
    const unsigned valBits = val->getType()->getIntegerBitWidth();
    if (valBits < unitBits) {
        val = builder.CreateZExt(val, unit, "bf.wid");
    } else if (valBits > unitBits) {
        val = builder.CreateTrunc(val, unit, "bf.narrow");
    }
    llvm::Value* old = builder.CreateLoad(unit, unitAddr, "bf.old");
    // wMask = 低 bitWidth 位全 1；fieldMask = wMask << bitOffset。
    llvm::Value* wMask = llvm::ConstantInt::get(
        unit, llvm::APInt::getLowBitsSet(unitBits, bitWidth));
    llvm::Value* fieldMask = builder.CreateShl(
        wMask, llvm::ConstantInt::get(unit, bitOffset), "bf.fmask");
    llvm::Value* cleared = builder.CreateAnd(old, builder.CreateNot(fieldMask), "bf.clear");
    llvm::Value* v = builder.CreateAnd(val, wMask, "bf.vtrunc");
    v = builder.CreateShl(v, llvm::ConstantInt::get(unit, bitOffset), "bf.vshl");
    builder.CreateStore(builder.CreateOr(cleared, v, "bf.stored"), unitAddr);
}

void CodegenContext::declareVariable(const std::string& name, llvm::Value* alloca, Type* type) {
    currentScope()->declare(name, new Symbol(name, type, alloca));
}

void CodegenContext::pushBreakBlock(llvm::BasicBlock* bb) {
    breakBlocks.push_back(bb);
    breakDeferBoundaries.push_back(deferScopes.size());
}

void CodegenContext::popBreakBlock() {
    if (!breakBlocks.empty()) {
        breakBlocks.pop_back();
    }
    if (!breakDeferBoundaries.empty()) {
        breakDeferBoundaries.pop_back();
    }
}

llvm::BasicBlock* CodegenContext::getBreakBlock() const {
    if (breakBlocks.empty()) return nullptr;
    return breakBlocks.back();
}

void CodegenContext::pushContinueBlock(llvm::BasicBlock* bb) {
    continueBlocks.push_back(bb);
    continueDeferBoundaries.push_back(deferScopes.size());
}

void CodegenContext::popContinueBlock() {
    if (!continueBlocks.empty()) {
        continueBlocks.pop_back();
    }
    if (!continueDeferBoundaries.empty()) {
        continueDeferBoundaries.pop_back();
    }
}

llvm::BasicBlock* CodegenContext::getContinueBlock() const {
    if (continueBlocks.empty()) return nullptr;
    return continueBlocks.back();
}

void CodegenContext::pushDeferScope() {
    deferScopes.emplace_back();
    cleanupScopes.emplace_back();
}

void CodegenContext::addDefer(ExprAST* expr) {
    if (expr && !deferScopes.empty()) {
        deferScopes.back().push_back(expr);
    }
}

void CodegenContext::addStringCleanup(llvm::Value* addr, Type* type) {
    if (addr && type && !cleanupScopes.empty())
        cleanupScopes.back().emplace_back(addr, type);
}

void CodegenContext::emitDefersFrom(size_t depth) {
    if (depth > deferScopes.size()) return;
    for (size_t i = deferScopes.size(); i > depth; --i) {
        auto& scope = deferScopes[i - 1];
        for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
            if (*it) (*it)->codegen(*this);
        }
        if (i - 1 < cleanupScopes.size()) {
            auto& cleanups = cleanupScopes[i - 1];
            for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) {
                emitStringValueRelease(it->first, it->second);
            }
        }
    }
}

void CodegenContext::popDeferScope() {
    if (deferScopes.empty()) return;
    emitDefersFrom(deferScopes.size() - 1);
    deferScopes.pop_back();
    if (!cleanupScopes.empty()) cleanupScopes.pop_back();
}

void CodegenContext::discardDeferScope() {
    if (!deferScopes.empty()) {
        deferScopes.pop_back();
    }
    if (!cleanupScopes.empty()) {
        cleanupScopes.pop_back();
    }
}

void CodegenContext::emitAllDefers() {
    emitDefersFrom(0);
}

size_t CodegenContext::getBreakDeferBoundary() const {
    if (breakDeferBoundaries.empty()) return 0;
    return breakDeferBoundaries.back();
}

size_t CodegenContext::getContinueDeferBoundary() const {
    if (continueDeferBoundaries.empty()) return 0;
    return continueDeferBoundaries.back();
}

llvm::Value* CodegenContext::coerceToBool(llvm::Value* val, Type* astType) {
    if (!val) return nullptr;
    Type* t = astType;
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    if (t && (t->kind == TypeKind::Optional || t->kind == TypeKind::Result)) {
        if (val->getType()->isStructTy())
            return builder.CreateExtractValue(val, {0}, "optres.bool");
        return coerceToBool(val);
    }
    return coerceToBool(val);
}

llvm::Value* CodegenContext::coerceToBool(llvm::Value* val) {
    if (!val) return nullptr;
    if (val->getType()->isIntegerTy(1)) return val;
    if (val->getType()->isIntegerTy()) {
        return builder.CreateICmpNE(val,
            llvm::ConstantInt::get(val->getType(), 0), "tobool");
    }
    if (val->getType()->isFloatingPointTy()) {
        return builder.CreateFCmpUNE(val,
            llvm::ConstantFP::get(val->getType(), 0.0), "tobool");
    }
    if (val->getType()->isPointerTy()) {
        // Compare against the null pointer. (ptrtoint to i1 would test only the
        // low address bit, so any aligned non-null pointer would look false.)
        auto* ptrTy = llvm::cast<llvm::PointerType>(val->getType());
        return builder.CreateICmpNE(val, llvm::ConstantPointerNull::get(ptrTy), "tobool");
    }
    return val;
}

llvm::Value* CodegenContext::castValue(llvm::Value* val, llvm::Type* targetLLVMType) {
    if (!val || !targetLLVMType) return val;
    llvm::Type* srcType = val->getType();
    if (srcType == targetLLVMType) return val;

    if (srcType->isIntegerTy() && targetLLVMType->isIntegerTy()) {
        unsigned srcBits = srcType->getIntegerBitWidth();
        unsigned dstBits = targetLLVMType->getIntegerBitWidth();
        // A 1-bit integer (bool) must be zero-extended so that `true` becomes 1.
        if (srcBits == 1 && dstBits > 1) return builder.CreateZExt(val, targetLLVMType, "zexttmp");
        if (srcBits < dstBits) return builder.CreateSExt(val, targetLLVMType, "sexttmp");
        if (srcBits > dstBits) return builder.CreateTrunc(val, targetLLVMType, "trunctmp");
        return val;
    }

    if (srcType->isIntegerTy() && targetLLVMType->isFloatingPointTy()) {
        return builder.CreateSIToFP(val, targetLLVMType, "sitofptmp");
    }
    if (srcType->isFloatingPointTy() && targetLLVMType->isIntegerTy()) {
        return builder.CreateFPToSI(val, targetLLVMType, "fptositmp");
    }
    if (srcType->isFloatingPointTy() && targetLLVMType->isFloatingPointTy()) {
        unsigned srcWidth = srcType->getFPMantissaWidth();
        unsigned dstWidth = targetLLVMType->getFPMantissaWidth();
        if (srcWidth < dstWidth) return builder.CreateFPExt(val, targetLLVMType, "fpexttmp");
        if (srcWidth > dstWidth) return builder.CreateFPTrunc(val, targetLLVMType, "fptrunctmp");
        return val;
    }

    if (srcType->isPointerTy() && targetLLVMType->isPointerTy()) {
        return builder.CreateBitCast(val, targetLLVMType, "bitcasttmp");
    }

    // Null-pointer constant / address conversions (TYP-24). Without these a
    // `T* p = null;` or pointer/integer comparison produced mismatched types.
    if (srcType->isIntegerTy() && targetLLVMType->isPointerTy()) {
        return builder.CreateIntToPtr(val, targetLLVMType, "inttoptr");
    }
    if (srcType->isPointerTy() && targetLLVMType->isIntegerTy()) {
        return builder.CreatePtrToInt(val, targetLLVMType, "ptrtoint");
    }

    return val;
}

llvm::Value* CodegenContext::castValue(llvm::Value* val, Type* fromAST, llvm::Type* targetLLVMType) {
    if (!val || !targetLLVMType || !fromAST) return castValue(val, targetLLVMType);
    llvm::Type* srcType = val->getType();
    if (srcType == targetLLVMType) return val;

    const bool fromUnsigned = isUnsignedIntegerType(fromAST);

    if (srcType->isIntegerTy() && targetLLVMType->isIntegerTy()) {
        unsigned srcBits = srcType->getIntegerBitWidth();
        unsigned dstBits = targetLLVMType->getIntegerBitWidth();
        // A 1-bit integer (bool) must be zero-extended so that `true` becomes 1.
        if (srcBits == 1 && dstBits > 1) return builder.CreateZExt(val, targetLLVMType, "zexttmp");
        if (srcBits < dstBits) {
            return fromUnsigned ? builder.CreateZExt(val, targetLLVMType, "zexttmp")
                                : builder.CreateSExt(val, targetLLVMType, "sexttmp");
        }
        if (srcBits > dstBits) return builder.CreateTrunc(val, targetLLVMType, "trunctmp");
        return val;
    }

    if (srcType->isIntegerTy() && targetLLVMType->isFloatingPointTy()) {
        return fromUnsigned ? builder.CreateUIToFP(val, targetLLVMType, "uitofptmp")
                            : builder.CreateSIToFP(val, targetLLVMType, "sitofptmp");
    }

    // P1-06 (FMT-02): string -> str implicit view — project {ptr, len} out
    // of the {ptr, len, cap} header.
    if (fromAST->kind == TypeKind::String && targetLLVMType->isStructTy()) {
        llvm::Value* ptr = builder.CreateExtractValue(val, 0, "view.ptr");
        llvm::Value* len = builder.CreateExtractValue(val, 1, "view.len");
        llvm::Value* v = llvm::Constant::getNullValue(targetLLVMType);
        v = builder.CreateInsertValue(v, ptr, {0});
        return builder.CreateInsertValue(v, len, {1});
    }

    // P1-06: string -> char* direct byte view — extract the pointer field.
    if (fromAST->kind == TypeKind::String && targetLLVMType->isPointerTy()) {
        llvm::Value* ptr = builder.CreateExtractValue(val, 0, "strview.ptr");
        return castValue(ptr, targetLLVMType);
    }

    // P1-06 (FMT-03): str -> char* implicit byte view — extract the pointer
    // field (the length is dropped; the buffer is guaranteed NUL-free UTF-8,
    // callers needing C-string semantics own that conversion).
    if (fromAST->kind == TypeKind::Str && targetLLVMType->isPointerTy()) {
        llvm::Value* ptr = builder.CreateExtractValue(val, 0, "strview.ptr");
        return castValue(ptr, targetLLVMType);
    }

    // P1-09 (INH slice fix): assigning a derived class VALUE to a base class
    // keeps only the base sub-object (field 0 of the derived layout). The
    // former whole-value store overflowed the destination by the derived tail
    // — latent stack corruption that surfaced once short-circuit && changed
    // the frame layout.
    if (fromAST->kind == TypeKind::Class && srcType->isStructTy() &&
        targetLLVMType->isStructTy()) {
        llvm::Value* cur = val;
        auto* srcStruct = llvm::cast<llvm::StructType>(srcType);
        while (srcStruct->getNumContainedTypes() > 0) {
            llvm::Type* first = srcStruct->getStructElementType(0);
            if (!first->isStructTy()) break;
            cur = builder.CreateExtractValue(cur, {0}, "base.slice");
            if (first == targetLLVMType) return cur;
            srcStruct = llvm::cast<llvm::StructType>(first);
        }
    }

    // Float -> int, float widening/narrowing and pointer conversions do not
    // depend on the source signedness; reuse the generic implementation.
    return castValue(val, targetLLVMType);
}

llvm::Value* CodegenContext::emitArrayToSliceDecay(ArrayType* arrayType,
                                                   llvm::Value* arrayAddr) {
    // TYP-12: build the {ptr, len} view over a statically-sized array
    // (zero-copy; the elements are never duplicated). Array operands already
    // yield the address of their first element.
    auto& builder = getBuilder();
    llvm::Type* sliceLLVM = getLLVMType(
        TypeContext::instance().getSliceType(arrayType->elementType));
    llvm::Value* v = llvm::Constant::getNullValue(sliceLLVM);
    v = builder.CreateInsertValue(v, arrayAddr, {0});
    return builder.CreateInsertValue(
        v, llvm::ConstantInt::get(llvm::Type::getInt64Ty(getContext()),
                                  static_cast<uint64_t>(arrayType->size)), {1});
}

// P1-02 评审 C1: injective layout-identity key. The LLVM named-struct name
// IS the layout identity (getTypeByName reuse), so it must never collide:
// length-prefixed components + '.' separators (user identifiers cannot
// contain '.'). Covers: two enums with different underlying types, the
// Result<My_Err, x> / Result<My, Err_x> ambiguity, and user structs named
// like the old "Optional_int32" prefix.
static std::string layoutKindName(TypeKind k) {
    switch (k) {
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::Char: return "char";
        case TypeKind::Int8: return "i8";
        case TypeKind::Int16: return "i16";
        case TypeKind::Int32: return "i32";
        case TypeKind::Int64: return "i64";
        case TypeKind::Int128: return "i128";
        case TypeKind::UInt8: return "u8";
        case TypeKind::UInt16: return "u16";
        case TypeKind::UInt32: return "u32";
        case TypeKind::UInt64: return "u64";
        case TypeKind::UInt128: return "u128";
        case TypeKind::ISize: return "isize";
        case TypeKind::USize: return "usize";
        case TypeKind::Float16: return "f16";
        case TypeKind::Float32: return "f32";
        case TypeKind::Float64: return "f64";
        case TypeKind::Float128: return "f128";
        default: return "unk";
    }
}

static std::string layoutKey(Type* t) {
    if (!t) return "unk";
    switch (t->kind) {
        case TypeKind::Typedef:
            return layoutKey(static_cast<TypedefType*>(t)->aliasedType);
        case TypeKind::Pointer:
            return "P" + layoutKey(t->base);
        case TypeKind::Array: {
            auto* at = static_cast<ArrayType*>(t);
            return "A" + std::to_string(at->size) + "." + layoutKey(at->elementType);
        }
        case TypeKind::Slice:
            return "L" + layoutKey(static_cast<SliceType*>(t)->elementType);
        case TypeKind::Str:
            return "STR";
        case TypeKind::String:
            return "STRING";
        case TypeKind::Optional:
            return "O" + layoutKey(static_cast<OptionalType*>(t)->elementType);
        case TypeKind::Result: {
            auto* r = static_cast<ResultType*>(t);
            return "R" + layoutKey(r->successType) + "." + layoutKey(r->errorType);
        }
        case TypeKind::Struct: {
            auto* s = static_cast<StructType*>(t);
            return "S" + std::to_string(s->name.size()) + "." + s->name;
        }
        case TypeKind::Class: {
            auto* c = static_cast<ClassType*>(t);
            return "C" + std::to_string(c->name.size()) + "." + c->name;
        }
        case TypeKind::Union: {
            auto* u = static_cast<UnionType*>(t);
            return "U" + std::to_string(u->name.size()) + "." + u->name;
        }
        case TypeKind::Enum: {
            auto* e = static_cast<EnumType*>(t);
            std::string underlying =
                e->underlyingType ? layoutKindName(e->underlyingType->kind) : "i32";
            return "E" + std::to_string(e->name.size()) + "." + e->name + "." + underlying;
        }
        default:
            return layoutKindName(t->kind);
    }
}

llvm::Type* CodegenContext::getLLVMType(Type* type) {
    if (!type) return llvm::Type::getVoidTy(*context);

    switch (type->kind) {
        case TypeKind::Char:   return llvm::Type::getInt8Ty(*context);
        case TypeKind::Void:   return llvm::Type::getVoidTy(*context);
        case TypeKind::Bool:   return llvm::Type::getInt1Ty(*context);
        // 新增整数类型
        case TypeKind::Int8:   return llvm::Type::getInt8Ty(*context);
        case TypeKind::Int16:  return llvm::Type::getInt16Ty(*context);
        case TypeKind::Int32:  return llvm::Type::getInt32Ty(*context);
        case TypeKind::Int64:  return llvm::Type::getInt64Ty(*context);
        case TypeKind::Int128: return llvm::Type::getInt128Ty(*context);
        case TypeKind::UInt8:  return llvm::Type::getInt8Ty(*context);
        case TypeKind::UInt16: return llvm::Type::getInt16Ty(*context);
        case TypeKind::UInt32: return llvm::Type::getInt32Ty(*context);
        case TypeKind::UInt64: return llvm::Type::getInt64Ty(*context);
        case TypeKind::UInt128:return llvm::Type::getInt128Ty(*context);
        case TypeKind::ISize:  return llvm::Type::getInt64Ty(*context);
        case TypeKind::USize:  return llvm::Type::getInt64Ty(*context);
        case TypeKind::Float32:return llvm::Type::getFloatTy(*context);
        case TypeKind::Float64:return llvm::Type::getDoubleTy(*context);
        case TypeKind::Float16:return llvm::Type::getHalfTy(*context);
        case TypeKind::Float128:return llvm::Type::getFP128Ty(*context);
        case TypeKind::Pointer:
            // Opaque pointers: do NOT recurse into the pointee. Besides being
            // unnecessary, resolving a self-referential `Node*` field would
            // re-enter the aggregate layout and overflow the stack.
            return llvm::PointerType::get(*context, 0);
        case TypeKind::Struct: {
            auto* st = static_cast<StructType*>(type);
            // Reuse existing struct type if one with this name already exists
            if (auto* existing = llvm::StructType::getTypeByName(*context, st->name)) {
                return existing;
            }
            // P1-05 / ANN: 布局单源化（LayoutBuilder；基类槽 0）。
            auto LR = LayoutBuilder::buildAggregate(st, *context, module->getDataLayout(),
                [this](Type* t) { return getLLVMType(t); },
                [](const std::string& n) -> Type* {
                    TypeContext& tc = TypeContext::instance();
                    if (auto* s = tc.getStruct(n)) return s;
                    if (auto* c = tc.getClass(n)) return c;
                    if (auto* u = tc.getUnion(n)) return u;
                    return nullptr;
                });
            std::vector<llvm::Type*> fieldTypes;
            for (auto& f : LR.fields) fieldTypes.push_back(f.type);
            return llvm::StructType::create(*context, fieldTypes, st->name, LR.isPacked);
        }
        case TypeKind::Union: {
            // P1-05 / ANN: 布局单源化（LayoutBuilder；最大成员块 + 填充）。
            auto* ut = static_cast<UnionType*>(type);
            if (auto* existing = llvm::StructType::getTypeByName(*context, ut->name)) {
                return existing;
            }
            auto LR = LayoutBuilder::buildUnion(ut, *context, module->getDataLayout(),
                [this](Type* t) { return getLLVMType(t); });
            std::vector<llvm::Type*> fields;
            for (auto& f : LR.fields) fields.push_back(f.type);
            return llvm::StructType::create(*context, fields, ut->name);
        }
        case TypeKind::Class: {
            auto* ct = static_cast<ClassType*>(type);
            // Reuse existing struct type if one with this name already exists
            if (auto* existing = llvm::StructType::getTypeByName(*context, ct->name)) {
                return existing;
            }
            // P1-05 / ANN: 布局单源化（LayoutBuilder；基类槽 0）。
            auto LR = LayoutBuilder::buildAggregate(ct, *context, module->getDataLayout(),
                [this](Type* t) { return getLLVMType(t); },
                [](const std::string& n) -> Type* {
                    TypeContext& tc = TypeContext::instance();
                    if (auto* s = tc.getStruct(n)) return s;
                    if (auto* c = tc.getClass(n)) return c;
                    if (auto* u = tc.getUnion(n)) return u;
                    return nullptr;
                });
            std::vector<llvm::Type*> fieldTypes;
            for (auto& f : LR.fields) fieldTypes.push_back(f.type);
            return llvm::StructType::create(*context, fieldTypes, ct->name, LR.isPacked);
        }
        case TypeKind::Array: {
            auto* at = static_cast<ArrayType*>(type);
            return llvm::ArrayType::get(getLLVMType(at->elementType), at->size);
        }
        case TypeKind::Enum: {
            // TYP-09/TYP-25: use the explicit underlying type when present.
            auto* et = static_cast<EnumType*>(type);
            if (et->underlyingType) {
                return getLLVMType(et->underlyingType);
            }
            return llvm::Type::getInt32Ty(*context);
        }
        case TypeKind::Typedef: {
            auto* td = static_cast<TypedefType*>(type);
            return getLLVMType(td->aliasedType);
        }
        // 新增类型
        case TypeKind::Slice: {
            // TYP-12: all slices share ONE canonical named struct {ptr, i64};
            // the element type does not affect the LLVM shape. Per-call
            // create() split the type into "Slice", "Slice.0", ... breaking
            // type identity when slices cross function boundaries.
            auto* st = llvm::StructType::getTypeByName(*context, "Slice");
            return st ? st
                      : llvm::StructType::create(*context,
                            {llvm::PointerType::get(*context, 0),
                             llvm::Type::getInt64Ty(*context)},
                            "Slice");
        }
        case TypeKind::Str: {
            // P1-06 (FMT-01): str shares ONE canonical named struct
            // {ptr, i64} — the UTF-8 byte view (same shape as Slice).
            auto* st = llvm::StructType::getTypeByName(*context, "str");
            return st ? st
                      : llvm::StructType::create(*context,
                            {llvm::PointerType::get(*context, 0),
                             llvm::Type::getInt64Ty(*context)},
                            "str");
        }
        case TypeKind::String: {
            // P1-06 (FMT-02): string — canonical named struct {ptr, len, cap}.
            auto* st = llvm::StructType::getTypeByName(*context, "string");
            return st ? st
                      : llvm::StructType::create(*context,
                            {llvm::PointerType::get(*context, 0),
                             llvm::Type::getInt64Ty(*context),
                             llvm::Type::getInt64Ty(*context)},
                            "string");
        }
        case TypeKind::Optional: {
            // P1-02 (TYP-13) DS4: { i1 valid, T value } — 判别标志在前。
            // 原实现误转换 SliceType* 且字段序为 {T, i1}。
            auto* optType = static_cast<OptionalType*>(type);
            // 评审 C1: injective identity key (layoutKey) — 用户可控的名字
            // （枚举底层类型、struct 名下划线）不得影响布局身份。
            std::string name = "opt." + layoutKey(optType->elementType);
            if (auto* st = llvm::StructType::getTypeByName(*context, name))
                return st; // idempotent: named struct identity is unique
            std::vector<llvm::Type*> fieldTypes;
            fieldTypes.push_back(llvm::Type::getInt1Ty(*context));    // valid
            fieldTypes.push_back(getLLVMType(optType->elementType));  // value
            return llvm::StructType::create(*context, fieldTypes, name);
        }
        case TypeKind::Result: {
            // P1-02 (TYP-14) DS4: { i1 ok, T value, E error } — 原实现无
            // 判别标志（DEC-03 裁决随此轮）。
            auto* resultType = static_cast<ResultType*>(type);
            std::string name = "res." + layoutKey(resultType->successType) +
                               "." + layoutKey(resultType->errorType);
            if (auto* st = llvm::StructType::getTypeByName(*context, name))
                return st; // idempotent: named struct identity is unique
            std::vector<llvm::Type*> fieldTypes;
            fieldTypes.push_back(llvm::Type::getInt1Ty(*context));     // ok
            fieldTypes.push_back(getLLVMType(resultType->successType)); // value
            fieldTypes.push_back(getLLVMType(resultType->errorType));   // error
            return llvm::StructType::create(*context, fieldTypes, name);
        }
        default:               return llvm::Type::getInt32Ty(*context);
    }
}

llvm::Type* CodegenContext::getAtomicCarrierType(Type* type, unsigned& bits) {
    bits = 0;
    Type* t = type;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    if (!t) return nullptr;
    auto iN = [this](unsigned width) -> llvm::Type* {
        return llvm::Type::getIntNTy(*context, width);
    };
    switch (t->kind) {
        case TypeKind::Bool:
        case TypeKind::Char:
        case TypeKind::Int8:
        case TypeKind::UInt8:
            bits = 8;
            return iN(8);
        case TypeKind::Int16:
        case TypeKind::UInt16:
        case TypeKind::Float16:
            bits = 16;
            return iN(16);
        case TypeKind::Int32:
        case TypeKind::UInt32:
        case TypeKind::Float32:
            bits = 32;
            return iN(32);
        case TypeKind::Int64:
        case TypeKind::UInt64:
        case TypeKind::Float64:
        case TypeKind::ISize:
        case TypeKind::USize:
        case TypeKind::Pointer:
            bits = 64;
            return iN(64);
        case TypeKind::Enum: {
            llvm::Type* underlying = getLLVMType(t);
            if (underlying && underlying->isIntegerTy()) {
                unsigned width = underlying->getIntegerBitWidth();
                if (width == 8 || width == 16 || width == 32 || width == 64) {
                    bits = width;
                    return iN(width);
                }
            }
            return nullptr;
        }
        default:
            return nullptr;
    }
}

unsigned CodegenContext::getAtomicAlignBits(Type* type) {
    unsigned bits = 0;
    getAtomicCarrierType(type, bits);
    return bits;
}

llvm::Value* CodegenContext::valueToAtomicCarrier(llvm::Value* value,
                                                   Type* type,
                                                   llvm::Type* carrier) {
    if (!value || !carrier) return value;
    if (value->getType() == carrier) return value;
    Type* t = type;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    llvm::Type* orig = t ? getLLVMType(t) : value->getType();
    if (orig && orig->isIntegerTy(1) && value->getType()->isIntegerTy(1)) {
        return builder.CreateZExt(value, carrier, "atomic.bool");
    }
    if (value->getType()->isPointerTy()) {
        return builder.CreatePtrToInt(value, carrier, "atomic.ptr");
    }
    if (value->getType()->isFloatingPointTy()) {
        return builder.CreateBitCast(value, carrier, "atomic.fp");
    }
    if (value->getType()->isIntegerTy() && carrier->isIntegerTy()) {
        unsigned from = value->getType()->getIntegerBitWidth();
        unsigned to = carrier->getIntegerBitWidth();
        if (from == to) return builder.CreateBitCast(value, carrier, "atomic.int");
        if (from < to) return builder.CreateZExt(value, carrier, "atomic.zext");
        return builder.CreateTrunc(value, carrier, "atomic.trunc");
    }
    return builder.CreateBitCast(value, carrier, "atomic.cast");
}

llvm::Value* CodegenContext::valueFromAtomicCarrier(llvm::Value* value,
                                                     Type* type,
                                                     llvm::Type* carrier) {
    if (!value) return value;
    Type* t = type;
    while (t && t->kind == TypeKind::Typedef) {
        t = static_cast<TypedefType*>(t)->aliasedType;
    }
    llvm::Type* orig = t ? getLLVMType(t) : value->getType();
    if (!orig || orig == value->getType()) return value;
    if (orig->isIntegerTy(1)) {
        return builder.CreateTrunc(value, orig, "atomic.bool");
    }
    if (orig->isPointerTy()) {
        return builder.CreateIntToPtr(value, orig, "atomic.ptr");
    }
    if (orig->isFloatingPointTy()) {
        return builder.CreateBitCast(value, orig, "atomic.fp");
    }
    if (orig->isIntegerTy() && value->getType()->isIntegerTy()) {
        unsigned from = value->getType()->getIntegerBitWidth();
        unsigned to = orig->getIntegerBitWidth();
        if (from == to) return builder.CreateBitCast(value, orig, "atomic.int");
        if (from > to) return builder.CreateTrunc(value, orig, "atomic.trunc");
        return builder.CreateZExt(value, orig, "atomic.zext");
    }
    if (carrier && carrier == value->getType()) {
        return builder.CreateBitCast(value, orig, "atomic.cast");
    }
    return builder.CreateBitCast(value, orig, "atomic.cast");
}

unsigned CodegenContext::getAtomicByteSize(Type* type) {
    unsigned bits = 0;
    getAtomicCarrierType(type, bits);
    return bits / 8;
}

unsigned CodegenContext::getPointeeByteSize(Type* pointerType) {
    Type* p = pointerType;
    while (p && p->kind == TypeKind::Typedef) {
        p = static_cast<TypedefType*>(p)->aliasedType;
    }
    if (!p || p->kind != TypeKind::Pointer || !p->base) return 1;
    llvm::Type* pointee = getLLVMType(p->base);
    if (!pointee || !pointee->isSized()) return 1;
    return static_cast<unsigned>(
        module->getDataLayout().getTypeAllocSize(pointee).getFixedValue());
}

// ===== P1-06 (FMT-01/04): synthesized UTF-8 stepping helpers =====
// RFC 3629 subset shared with sema (support/Utf8, T5): reject overlong
// encodings, surrogates U+D800..DFFF, values above U+10FFFF, and malformed
// continuation bytes. Internal linkage; synthesized once per module.

llvm::Function* CodegenContext::getUtf8CharLenAtFn() {
    if (utf8CharLenAtFn) return utf8CharLenAtFn;
    llvm::LLVMContext& c = *context;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getInt64Ty(c),
        {llvm::PointerType::get(c, 0), llvm::Type::getInt64Ty(c)}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.utf8.char_len_at", module.get());
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
    builder.SetInsertPoint(entry);

    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    llvm::Value* p = fn->getArg(0);
    llvm::Value* idx = fn->getArg(1);
    llvm::Value* bytePtr = builder.CreateGEP(i8Ty, p, idx, "seq.p");
    llvm::Value* lead = builder.CreateLoad(i8Ty, bytePtr, "seq.lead");
    auto* m2 = builder.CreateAnd(lead, llvm::ConstantInt::get(i8Ty, 0xE0));
    auto* m3 = builder.CreateAnd(lead, llvm::ConstantInt::get(i8Ty, 0xF0));
    auto* m4 = builder.CreateAnd(lead, llvm::ConstantInt::get(i8Ty, 0xF8));
    auto* is1 = builder.CreateICmpULT(
        lead, llvm::ConstantInt::get(i8Ty, 0x80), "lead.ascii");
    auto* is2 = builder.CreateICmpEQ(m2, llvm::ConstantInt::get(i8Ty, 0xC0), "lead.c2");
    auto* is3 = builder.CreateICmpEQ(m3, llvm::ConstantInt::get(i8Ty, 0xE0), "lead.e0");
    auto* is4 = builder.CreateICmpEQ(m4, llvm::ConstantInt::get(i8Ty, 0xF0), "lead.f0");
    auto* w1 = llvm::ConstantInt::get(i8Ty, 1);
    auto* w2 = llvm::ConstantInt::get(i8Ty, 2);
    auto* w3 = llvm::ConstantInt::get(i8Ty, 3);
    auto* w4 = llvm::ConstantInt::get(i8Ty, 4);
    auto* w0 = llvm::ConstantInt::get(i8Ty, 0);
    auto* sel4 = builder.CreateSelect(is4, w4, w0);
    auto* sel3 = builder.CreateSelect(is3, w3, sel4);
    auto* sel2 = builder.CreateSelect(is2, w2, sel3);
    auto* width8 = builder.CreateSelect(is1, w1, sel2, "seq.width");
    builder.CreateRet(builder.CreateZExt(width8, i64Ty));
    utf8CharLenAtFn = fn;
    return fn;
}

llvm::Function* CodegenContext::getUtf8CharCountFn() {
    if (utf8CharCountFn) return utf8CharCountFn;
    llvm::LLVMContext& c = *context;
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* fnTy = llvm::FunctionType::get(i64Ty,
        {llvm::PointerType::get(c, 0), i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.utf8.char_count", module.get());
    // Synthesize the callee FIRST — it re-points the shared builder.
    llvm::Function* lenFn = getUtf8CharLenAtFn();

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
    llvm::BasicBlock* loop = llvm::BasicBlock::Create(c, "loop", fn);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(c, "body", fn);
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(c, "exit", fn);

    builder.SetInsertPoint(entry);
    builder.CreateBr(loop);
    builder.SetInsertPoint(loop);
    auto* iPhi = builder.CreatePHI(i64Ty, 2, "i");
    auto* nPhi = builder.CreatePHI(i64Ty, 2, "n");
    auto* done = builder.CreateICmpUGE(iPhi, fn->getArg(1), "scan.done");
    builder.CreateCondBr(done, exit, body);

    builder.SetInsertPoint(body);
    auto* width = builder.CreateCall(lenFn, {fn->getArg(0), iPhi}, "scan.width");
    // Defensive: an invalid lead byte (width 0, unreachable on validated
    // views) must still advance, or the loop would not terminate.
    auto* step = builder.CreateSelect(
        builder.CreateICmpEQ(width, llvm::ConstantInt::get(i64Ty, 0)),
        llvm::ConstantInt::get(i64Ty, 1), width, "scan.step");
    auto* iNext = builder.CreateAdd(iPhi, step, "scan.inext");
    auto* nNext = builder.CreateAdd(nPhi, llvm::ConstantInt::get(i64Ty, 1), "scan.nnext");
    iPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entry);
    nPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entry);
    iPhi->addIncoming(iNext, body);
    nPhi->addIncoming(nNext, body);
    builder.CreateBr(loop);

    builder.SetInsertPoint(exit);
    builder.CreateRet(nPhi);
    utf8CharCountFn = fn;
    return fn;
}


llvm::Function* CodegenContext::getFormatDynFn() {
    if (formatDynFn) return formatDynFn;
    llvm::LLVMContext& c = *context;
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* ptrTy = llvm::PointerType::get(c, 0);
    auto* retTy = llvm::StructType::get(c, {ptrTy, i64Ty, i64Ty});
    auto* fnTy = llvm::FunctionType::get(
        retTy, {ptrTy, i64Ty, ptrTy, ptrTy, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.format.dyn", module.get());
    auto mallocFn = module->getOrInsertFunction(
        "malloc", llvm::FunctionType::get(ptrTy, {i64Ty}, false));

    // Args: fmt, fmtLen, chunkPtrs(i8**), chunkLens(i64*), n.
    auto* fmt = fn->getArg(0);
    auto* fmtLen = fn->getArg(1);
    auto* ptrs = fn->getArg(2);
    auto* lens = fn->getArg(3);
    auto* n = fn->getArg(4);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
    llvm::BasicBlock* sumLoop = llvm::BasicBlock::Create(c, "sum.loop", fn);
    llvm::BasicBlock* sumBody = llvm::BasicBlock::Create(c, "sum.body", fn);
    llvm::BasicBlock* alloc = llvm::BasicBlock::Create(c, "alloc", fn);
    llvm::BasicBlock* scanLoop = llvm::BasicBlock::Create(c, "scan.loop", fn);
    llvm::BasicBlock* scanBody = llvm::BasicBlock::Create(c, "scan.body", fn);
    llvm::BasicBlock* scanNext = llvm::BasicBlock::Create(c, "scan.next", fn);
    llvm::BasicBlock* openBrace = llvm::BasicBlock::Create(c, "open", fn);
    llvm::BasicBlock* checkEsc = llvm::BasicBlock::Create(c, "check.esc", fn);
    llvm::BasicBlock* escaped = llvm::BasicBlock::Create(c, "escaped", fn);
    llvm::BasicBlock* findEntry = llvm::BasicBlock::Create(c, "find.entry", fn);
    llvm::BasicBlock* findLoop = llvm::BasicBlock::Create(c, "find.loop", fn);
    llvm::BasicBlock* findBody = llvm::BasicBlock::Create(c, "find.body", fn);
    llvm::BasicBlock* consume = llvm::BasicBlock::Create(c, "consume", fn);
    llvm::BasicBlock* copyChunk = llvm::BasicBlock::Create(c, "copy.chunk", fn);
    llvm::BasicBlock* closeDone = llvm::BasicBlock::Create(c, "close.done", fn);
    llvm::BasicBlock* closeBrace = llvm::BasicBlock::Create(c, "close", fn);
    llvm::BasicBlock* closeDbl = llvm::BasicBlock::Create(c, "close.dbl", fn);
    llvm::BasicBlock* closeSingle = llvm::BasicBlock::Create(c, "close.single", fn);
    llvm::BasicBlock* copyByte = llvm::BasicBlock::Create(c, "copy", fn);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(c, "done", fn);

    auto byteAt = [&](llvm::Value* base, llvm::Value* idx) {
        return builder.CreateLoad(
            i8Ty, builder.CreateGEP(i8Ty, base, idx, "fd.p"), "fd.b");
    };

    // Pass 1: upper-bound capacity = fmtLen + sum(chunkLens).
    builder.SetInsertPoint(entry);
    builder.CreateBr(sumLoop);
    builder.SetInsertPoint(sumLoop);
    auto* siPhi = builder.CreatePHI(i64Ty, 2, "fd.si");
    auto* sumPhi = builder.CreatePHI(i64Ty, 2, "fd.sum");
    auto* sumDone = builder.CreateICmpUGE(siPhi, n, "fd.sum.done");
    builder.CreateCondBr(sumDone, alloc, sumBody);
    builder.SetInsertPoint(sumBody);
    auto* lVal = builder.CreateLoad(
        i64Ty, builder.CreateGEP(i64Ty, lens, siPhi, "fd.len.p"), "fd.len");
    auto* sumNext = builder.CreateAdd(sumPhi, lVal, "fd.sum.next");
    auto* siNext = builder.CreateAdd(siPhi, llvm::ConstantInt::get(i64Ty, 1), "fd.si.next");
    siPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entry);
    sumPhi->addIncoming(fmtLen, entry);
    siPhi->addIncoming(siNext, sumBody);
    sumPhi->addIncoming(sumNext, sumBody);
    builder.CreateBr(sumLoop);

    // Single malloc (FMT-12): capacity is a safe upper bound, no realloc.
    builder.SetInsertPoint(alloc);
    auto* cap = builder.CreateAdd(sumPhi, llvm::ConstantInt::get(i64Ty, 1), "fd.cap");
    auto* buf = builder.CreateCall(mallocFn, {cap}, "fd.buf");
    auto* dstAddr = builder.CreateAlloca(ptrTy, nullptr, "fd.dst.addr");
    auto* wriAddr = builder.CreateAlloca(i64Ty, nullptr, "fd.wri.addr");
    auto* fiAddr = builder.CreateAlloca(i64Ty, nullptr, "fd.fi.addr");
    auto* ciAddr = builder.CreateAlloca(i64Ty, nullptr, "fd.ci.addr");
    builder.CreateStore(buf, dstAddr);
    builder.CreateStore(llvm::ConstantInt::get(i64Ty, 0), wriAddr);
    builder.CreateStore(llvm::ConstantInt::get(i64Ty, 0), fiAddr);
    builder.CreateStore(llvm::ConstantInt::get(i64Ty, 0), ciAddr);
    builder.CreateBr(scanLoop);

    // Emit one literal byte, advance fi by `fiNext`, continue scanning.
    auto emitOne = [&](llvm::Value* ch, llvm::Value* fiNext) {
        auto* dst = builder.CreateLoad(ptrTy, dstAddr, "fd.dst");
        builder.CreateStore(ch, dst);
        builder.CreateStore(
            builder.CreateGEP(i8Ty, dst, llvm::ConstantInt::get(i64Ty, 1), "fd.dst.next"),
            dstAddr);
        builder.CreateStore(
            builder.CreateAdd(builder.CreateLoad(i64Ty, wriAddr, "fd.wri"),
                              llvm::ConstantInt::get(i64Ty, 1), "fd.wri.next"),
            wriAddr);
        builder.CreateStore(fiNext, fiAddr);
        builder.CreateBr(scanLoop);
    };

    builder.SetInsertPoint(scanLoop);
    auto* fi = builder.CreateLoad(i64Ty, fiAddr, "fd.fi");
    auto* more = builder.CreateICmpULT(fi, fmtLen, "fd.more");
    builder.CreateCondBr(more, scanBody, done);

    builder.SetInsertPoint(scanBody);
    auto* ch = byteAt(fmt, fi);
    auto* fi1 = builder.CreateAdd(fi, llvm::ConstantInt::get(i64Ty, 1), "fd.fi1");
    auto* isOpen = builder.CreateICmpEQ(ch, llvm::ConstantInt::get(i8Ty, '{'), "fd.isopen");
    builder.CreateCondBr(isOpen, openBrace, scanNext);
    builder.SetInsertPoint(scanNext);
    auto* isClose = builder.CreateICmpEQ(ch, llvm::ConstantInt::get(i8Ty, '}'), "fd.isclose");
    builder.CreateCondBr(isClose, closeBrace, copyByte);

    // "{{" escape.
    builder.SetInsertPoint(openBrace);
    auto* openHasNext = builder.CreateICmpULT(fi1, fmtLen, "fd.open.hasnext");
    builder.CreateCondBr(openHasNext, checkEsc, findEntry);
    builder.SetInsertPoint(checkEsc);
    auto* nextCh = byteAt(fmt, fi1);
    auto* isEsc = builder.CreateICmpEQ(nextCh, llvm::ConstantInt::get(i8Ty, '{'), "fd.isesc");
    builder.CreateCondBr(isEsc, escaped, findEntry);

    builder.SetInsertPoint(escaped);
    emitOne(ch, builder.CreateAdd(fi, llvm::ConstantInt::get(i64Ty, 2), "fd.fi.esc"));

    // Find the closing '}' (spec text inside dynamic placeholders is ignored).
    builder.SetInsertPoint(findEntry);
    builder.CreateBr(findLoop);
    builder.SetInsertPoint(findLoop);
    auto* jPhi = builder.CreatePHI(i64Ty, 2, "fd.j");
    auto* jMore = builder.CreateICmpULT(jPhi, fmtLen, "fd.j.more");
    // Unterminated '{': the scan stops (documented contract).
    builder.CreateCondBr(jMore, findBody, done);
    builder.SetInsertPoint(findBody);
    auto* jCh = byteAt(fmt, jPhi);
    auto* jIsClose = builder.CreateICmpEQ(jCh, llvm::ConstantInt::get(i8Ty, '}'), "fd.j.isclose");
    auto* jNext = builder.CreateAdd(jPhi, llvm::ConstantInt::get(i64Ty, 1), "fd.j.next");
    builder.CreateCondBr(jIsClose, consume, findLoop);
    jPhi->addIncoming(fi1, findEntry);
    jPhi->addIncoming(jNext, findBody);

    // Consume the next chunk as the placeholder's value (missing -> nothing).
    builder.SetInsertPoint(consume);
    auto* ci = builder.CreateLoad(i64Ty, ciAddr, "fd.ci");
    auto* hasChunk = builder.CreateICmpULT(ci, n, "fd.haschunk");
    builder.CreateCondBr(hasChunk, copyChunk, closeDone);

    builder.SetInsertPoint(copyChunk);
    {
        auto* cPtr = builder.CreateLoad(
            ptrTy, builder.CreateGEP(ptrTy, ptrs, ci, "fd.chunk.p"), "fd.chunk.ptr");
        auto* cLen = builder.CreateLoad(
            i64Ty, builder.CreateGEP(i64Ty, lens, ci, "fd.chunk.l"), "fd.chunk.len");
        auto* dstC = builder.CreateLoad(ptrTy, dstAddr, "fd.dst.c");
        builder.CreateMemCpy(dstC, std::nullopt, cPtr, std::nullopt, cLen);
        builder.CreateStore(
            builder.CreateGEP(i8Ty, dstC, cLen, "fd.dst.chunk"), dstAddr);
        builder.CreateStore(
            builder.CreateAdd(builder.CreateLoad(i64Ty, wriAddr, "fd.wri.c"),
                              cLen, "fd.wri.chunk"),
            wriAddr);
        builder.CreateStore(
            builder.CreateAdd(ci, llvm::ConstantInt::get(i64Ty, 1), "fd.ci.next"),
            ciAddr);
        builder.CreateBr(closeDone);
    }

    builder.SetInsertPoint(closeDone);
    builder.CreateStore(
        builder.CreateAdd(jPhi, llvm::ConstantInt::get(i64Ty, 1), "fd.fi.consumed"),
        fiAddr);
    builder.CreateBr(scanLoop);

    // Single '}' in a dynamic format is a literal (contract: unvalidated).
    builder.SetInsertPoint(closeBrace);
    auto* closeHasNext = builder.CreateICmpULT(fi1, fmtLen, "fd.close.hasnext");
    builder.CreateCondBr(closeHasNext, closeDbl, closeSingle);
    builder.SetInsertPoint(closeDbl);
    {
        auto* nextC = byteAt(fmt, fi1);
        auto* isDbl = builder.CreateICmpEQ(nextC, llvm::ConstantInt::get(i8Ty, '}'), "fd.close.isdbl");
        auto* step = builder.CreateSelect(isDbl,
            llvm::ConstantInt::get(i64Ty, 2), llvm::ConstantInt::get(i64Ty, 1), "fd.close.step");
        auto* fiNext = builder.CreateAdd(fi, step, "fd.fi.close");
        emitOne(ch, fiNext);
    }
    builder.SetInsertPoint(closeSingle);
    emitOne(ch, fi1);

    // Ordinary byte: copy through.
    builder.SetInsertPoint(copyByte);
    emitOne(ch, fi1);

    builder.SetInsertPoint(done);
    {
        auto* wriD = builder.CreateLoad(i64Ty, wriAddr, "fd.wri.final");
        builder.CreateStore(
            llvm::ConstantInt::get(i8Ty, 0),
            builder.CreateGEP(i8Ty, buf, wriD, "fd.end"));
        llvm::Value* v = llvm::Constant::getNullValue(retTy);
        v = builder.CreateInsertValue(v, buf, {0});
        v = builder.CreateInsertValue(v, wriD, {1});
        v = builder.CreateInsertValue(v, cap, {2});
        builder.CreateRet(v);
    }
    formatDynFn = fn;
    return fn;
}


llvm::Function* CodegenContext::getUtf8ValidateFn() {
    if (utf8ValidateFn) return utf8ValidateFn;
    llvm::LLVMContext& c = *context;
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* i1Ty = llvm::Type::getInt1Ty(c);
    auto* fnTy = llvm::FunctionType::get(i1Ty,
        {llvm::PointerType::get(c, 0), i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.utf8.validate", module.get());
    auto* p = fn->getArg(0);
    auto* len = fn->getArg(1);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
    llvm::BasicBlock* loop = llvm::BasicBlock::Create(c, "loop", fn);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(c, "body", fn);
    llvm::BasicBlock* chk2 = llvm::BasicBlock::Create(c, "chk2", fn);
    llvm::BasicBlock* chk3 = llvm::BasicBlock::Create(c, "chk3", fn);
    llvm::BasicBlock* chk4 = llvm::BasicBlock::Create(c, "chk4", fn);
    llvm::BasicBlock* contInit = llvm::BasicBlock::Create(c, "cont.init", fn);
    llvm::BasicBlock* contBody = llvm::BasicBlock::Create(c, "cont.body", fn);
    llvm::BasicBlock* contCheck = llvm::BasicBlock::Create(c, "cont.check", fn);
    llvm::BasicBlock* contDone = llvm::BasicBlock::Create(c, "cont.done", fn);
    llvm::BasicBlock* step1 = llvm::BasicBlock::Create(c, "step1", fn);
    llvm::BasicBlock* bad = llvm::BasicBlock::Create(c, "bad", fn);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(c, "ok", fn);

    builder.SetInsertPoint(entry);
    builder.CreateBr(loop);

    // loop: dispatch on the lead byte.
    builder.SetInsertPoint(loop);
    auto* iPhi = builder.CreatePHI(i64Ty, 3, "i");
    auto* scanDone = builder.CreateICmpUGE(iPhi, len, "scan.done");
    builder.CreateCondBr(scanDone, ok, body);

    // body: the scan is in-bounds here — load the lead and decode.
    builder.SetInsertPoint(body);
    auto* bytePtr = builder.CreateGEP(i8Ty, p, iPhi, "seq.p");
    auto* lead = builder.CreateLoad(i8Ty, bytePtr, "seq.lead");
    auto* ascii = builder.CreateICmpULT(lead, llvm::ConstantInt::get(i8Ty, 0x80), "lead.ascii");

    // Lead-shape masks must precede the terminator — chk2/chk3/chk4 use them.
    auto* m2 = builder.CreateAnd(lead, llvm::ConstantInt::get(i8Ty, 0xE0));
    auto* m3 = builder.CreateAnd(lead, llvm::ConstantInt::get(i8Ty, 0xF0));
    auto* m4 = builder.CreateAnd(lead, llvm::ConstantInt::get(i8Ty, 0xF8));
    auto* is2 = builder.CreateICmpEQ(m2, llvm::ConstantInt::get(i8Ty, 0xC0));
    auto* is3 = builder.CreateICmpEQ(m3, llvm::ConstantInt::get(i8Ty, 0xE0));
    auto* is4 = builder.CreateICmpEQ(m4, llvm::ConstantInt::get(i8Ty, 0xF0));
    builder.CreateCondBr(ascii, step1, chk2);

    builder.SetInsertPoint(chk2);
    builder.CreateCondBr(is2, contInit, chk3);
    builder.SetInsertPoint(chk3);
    builder.CreateCondBr(is3, contInit, chk4);
    builder.SetInsertPoint(chk4);
    builder.CreateCondBr(is4, contInit, bad);

    // cont.init: k = 1; then the per-continuation-byte loop.
    builder.SetInsertPoint(contInit);
    // width recomputed from the lead's shape (2/3/4; one of is2/is3/is4 held).
    auto* w2 = builder.CreateSelect(is2, llvm::ConstantInt::get(i64Ty, 2),
                                    llvm::ConstantInt::get(i64Ty, 0), "v.w2");
    auto* w23 = builder.CreateSelect(is3, llvm::ConstantInt::get(i64Ty, 3), w2, "v.w23");
    auto* width = builder.CreateSelect(is4, llvm::ConstantInt::get(i64Ty, 4), w23, "v.width");
    builder.CreateBr(contBody);

    builder.SetInsertPoint(contBody);
    auto* kPhi = builder.CreatePHI(i64Ty, 2, "v.k");
    auto* offset = builder.CreateAdd(iPhi, kPhi, "v.off");
    auto* have = builder.CreateICmpULT(offset, len, "v.have");
    auto* bp = builder.CreateGEP(i8Ty, p, offset, "v.bp");
    auto* b = builder.CreateLoad(i8Ty, bp, "v.b");
    auto* inCont = builder.CreateAnd(
        builder.CreateICmpUGE(b, llvm::ConstantInt::get(i8Ty, 0x80)),
        builder.CreateICmpULE(b, llvm::ConstantInt::get(i8Ty, 0xBF)), "v.incont");
    // First byte carries the RFC 3629 boundary constraints (E0/ED/F0/F4).
    auto* kIs1 = builder.CreateICmpEQ(kPhi, llvm::ConstantInt::get(i64Ty, 1), "v.kis1");
    auto* e0ok = builder.CreateOr(builder.CreateICmpNE(lead, llvm::ConstantInt::get(i8Ty, 0xE0)),
        builder.CreateICmpUGE(b, llvm::ConstantInt::get(i8Ty, 0xA0)));
    auto* edok = builder.CreateOr(builder.CreateICmpNE(lead, llvm::ConstantInt::get(i8Ty, 0xED)),
        builder.CreateICmpULT(b, llvm::ConstantInt::get(i8Ty, 0xA0)));
    auto* f0ok = builder.CreateOr(builder.CreateICmpNE(lead, llvm::ConstantInt::get(i8Ty, 0xF0)),
        builder.CreateICmpUGE(b, llvm::ConstantInt::get(i8Ty, 0x90)));
    auto* f4ok = builder.CreateOr(builder.CreateICmpNE(lead, llvm::ConstantInt::get(i8Ty, 0xF4)),
        builder.CreateICmpULE(b, llvm::ConstantInt::get(i8Ty, 0x8F)));
    // C0/C1 are 2-byte overlong prefixes; F5..F7 encode > U+10FFFF.
    auto* c2ok = builder.CreateOr(builder.CreateNot(is2),
        builder.CreateICmpUGE(lead, llvm::ConstantInt::get(i8Ty, 0xC2)));
    auto* f4bound = builder.CreateOr(builder.CreateNot(is4),
        builder.CreateICmpULE(lead, llvm::ConstantInt::get(i8Ty, 0xF4)));
    auto* firstOk = builder.CreateOr(
        builder.CreateNot(kIs1),
        builder.CreateAnd(
            builder.CreateAnd(e0ok, edok),
            builder.CreateAnd(f0ok, builder.CreateAnd(f4ok, builder.CreateAnd(c2ok, f4bound)))),
        "v.firstok");
    auto* byteOk = builder.CreateAnd(builder.CreateAnd(have, inCont), firstOk, "v.byteok");
    builder.CreateCondBr(byteOk, contCheck, bad);

    builder.SetInsertPoint(contCheck);
    auto* kNext = builder.CreateAdd(kPhi, llvm::ConstantInt::get(i64Ty, 1), "v.knext");
    auto* more = builder.CreateICmpULT(kNext, width, "v.more");
    builder.CreateCondBr(more, contBody, contDone);

    builder.SetInsertPoint(contDone);
    auto* iNext = builder.CreateAdd(iPhi, width, "v.inext");
    builder.CreateBr(loop);

    // ASCII fast path: advance by one byte.
    builder.SetInsertPoint(step1);
    auto* iNext1 = builder.CreateAdd(iPhi, llvm::ConstantInt::get(i64Ty, 1), "v.inext1");
    builder.CreateBr(loop);

    builder.SetInsertPoint(ok);
    builder.CreateRet(llvm::ConstantInt::getTrue(c));
    builder.SetInsertPoint(bad);
    builder.CreateRet(llvm::ConstantInt::getFalse(c));

    iPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entry);
    iPhi->addIncoming(iNext1, step1);
    iPhi->addIncoming(iNext, contDone);
    kPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 1), contInit);
    kPhi->addIncoming(kNext, contCheck);

    utf8ValidateFn = fn;
    return fn;
}

llvm::Function* CodegenContext::getStrFindFn(bool reverse) {
    llvm::Function*& cache = reverse ? strRFindFn : strFindFn;
    if (cache) return cache;
    llvm::LLVMContext& c = *context;
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* ptrTy = llvm::PointerType::get(c, 0);
    auto* fnTy = llvm::FunctionType::get(
        i64Ty, {ptrTy, i64Ty, ptrTy, i64Ty}, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        reverse ? "smc.str.rfind" : "smc.str.find", module.get());
    auto* hay = fn->getArg(0);
    auto* hayLen = fn->getArg(1);
    auto* needle = fn->getArg(2);
    auto* needleLen = fn->getArg(3);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
    llvm::BasicBlock* sizeCheck = llvm::BasicBlock::Create(c, "size.check", fn);
    llvm::BasicBlock* outerInit = llvm::BasicBlock::Create(c, "outer.init", fn);
    llvm::BasicBlock* outer = llvm::BasicBlock::Create(c, "outer", fn);
    llvm::BasicBlock* inner = llvm::BasicBlock::Create(c, "inner", fn);
    llvm::BasicBlock* innerBody = llvm::BasicBlock::Create(c, "inner.body", fn);
    llvm::BasicBlock* innerNext = llvm::BasicBlock::Create(c, "inner.next", fn);
    llvm::BasicBlock* matched = llvm::BasicBlock::Create(c, "matched", fn);
    llvm::BasicBlock* outerNext = llvm::BasicBlock::Create(c, "outer.next", fn);
    llvm::BasicBlock* retEmpty = llvm::BasicBlock::Create(c, "ret.empty", fn);
    llvm::BasicBlock* retMinus1 = llvm::BasicBlock::Create(c, "ret.m1", fn);
    llvm::BasicBlock* retMatch = llvm::BasicBlock::Create(c, "ret.match", fn);

    auto one = llvm::ConstantInt::get(i64Ty, 1);
    auto zero = llvm::ConstantInt::get(i64Ty, 0);
    auto minus1 = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(-1));

    builder.SetInsertPoint(entry);
    auto* isEmpty = builder.CreateICmpEQ(needleLen, zero, "sf.empty");
    builder.CreateCondBr(isEmpty, retEmpty, sizeCheck);
    builder.SetInsertPoint(sizeCheck);
    auto* tooBig = builder.CreateICmpUGT(needleLen, hayLen, "sf.toobig");
    builder.CreateCondBr(tooBig, retMinus1, outerInit);

    builder.SetInsertPoint(outerInit);
    auto* limit = builder.CreateSub(hayLen, needleLen, "sf.limit");
    llvm::Value* startIdx = reverse ? limit : zero;
    builder.CreateBr(outer);
    builder.SetInsertPoint(outer);
    auto* iPhi = builder.CreatePHI(i64Ty, 2, "sf.i");
    // Forward: scan i in [0, limit]; reverse: scan i in [limit, 0] descending.
    if (reverse) {
        auto* atFloor = builder.CreateICmpSLT(iPhi, zero, "sf.floor");
        builder.CreateCondBr(atFloor, retMinus1, inner);
    } else {
        auto* pastEnd = builder.CreateICmpUGT(iPhi, limit, "sf.pastend");
        builder.CreateCondBr(pastEnd, retMinus1, inner);
    }
    builder.SetInsertPoint(inner);
    auto* jPhi = builder.CreatePHI(i64Ty, 2, "sf.j");
    auto* jDone = builder.CreateICmpUGE(jPhi, needleLen, "sf.jdone");
    builder.CreateCondBr(jDone, matched, innerBody);
    builder.SetInsertPoint(innerBody);
    auto* ij = builder.CreateAdd(iPhi, jPhi, "sf.ij");
    auto* hByte = builder.CreateLoad(
        i8Ty, builder.CreateGEP(i8Ty, hay, ij, "sf.hp"), "sf.hb");
    auto* nByte = builder.CreateLoad(
        i8Ty, builder.CreateGEP(i8Ty, needle, jPhi, "sf.np"), "sf.nb");
    auto* eq = builder.CreateICmpEQ(hByte, nByte, "sf.eq");
    builder.CreateCondBr(eq, innerNext, outerNext);
    builder.SetInsertPoint(innerNext);
    auto* jNext = builder.CreateAdd(jPhi, one, "sf.jnext");
    jPhi->addIncoming(zero, outer);
    jPhi->addIncoming(jNext, innerNext);
    builder.CreateBr(inner);
    builder.SetInsertPoint(matched);
    builder.CreateBr(retMatch);
    builder.SetInsertPoint(outerNext);
    llvm::Value* iNext;
    if (reverse) {
        iNext = builder.CreateSub(iPhi, one, "sf.inext");
    } else {
        iNext = builder.CreateAdd(iPhi, one, "sf.inext");
    }
    iPhi->addIncoming(startIdx, outerInit);
    iPhi->addIncoming(iNext, outerNext);
    builder.CreateBr(outer);

    builder.SetInsertPoint(retEmpty);
    builder.CreateRet(reverse ? static_cast<llvm::Value*>(hayLen)
                              : static_cast<llvm::Value*>(zero));
    builder.SetInsertPoint(retMinus1);
    builder.CreateRet(minus1);
    builder.SetInsertPoint(retMatch);
    builder.CreateRet(iPhi);

    cache = fn;
    return fn;
}

llvm::Function* CodegenContext::getStrMatchAtFn() {
    if (strMatchAtFn) return strMatchAtFn;
    llvm::LLVMContext& c = *context;
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* ptrTy = llvm::PointerType::get(c, 0);
    auto* fnTy = llvm::FunctionType::get(
        llvm::Type::getInt1Ty(c), {ptrTy, ptrTy, i64Ty, i64Ty}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.str.matchat", module.get());
    auto* hay = fn->getArg(0);
    auto* needle = fn->getArg(1);
    auto* nlen = fn->getArg(2);
    auto* at = fn->getArg(3);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
    llvm::BasicBlock* loop = llvm::BasicBlock::Create(c, "loop", fn);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(c, "body", fn);
    llvm::BasicBlock* yes = llvm::BasicBlock::Create(c, "yes", fn);
    llvm::BasicBlock* no = llvm::BasicBlock::Create(c, "no", fn);

    builder.SetInsertPoint(entry);
    builder.CreateBr(loop);
    builder.SetInsertPoint(loop);
    auto* jPhi = builder.CreatePHI(i64Ty, 2, "ma.j");
    auto* done = builder.CreateICmpUGE(jPhi, nlen, "ma.done");
    builder.CreateCondBr(done, yes, body);
    builder.SetInsertPoint(body);
    auto* hb = builder.CreateLoad(i8Ty, builder.CreateGEP(
        i8Ty, hay, builder.CreateAdd(at, jPhi, "ma.atj"), "ma.hp"), "ma.hb");
    auto* nb = builder.CreateLoad(i8Ty, builder.CreateGEP(
        i8Ty, needle, jPhi, "ma.np"), "ma.nb");
    auto* eq = builder.CreateICmpEQ(hb, nb, "ma.eq");
    auto* jNext = builder.CreateAdd(jPhi, llvm::ConstantInt::get(i64Ty, 1), "ma.jnext");
    jPhi->addIncoming(llvm::ConstantInt::get(i64Ty, 0), entry);
    jPhi->addIncoming(jNext, body);
    builder.CreateCondBr(eq, loop, no);
    builder.SetInsertPoint(yes);
    builder.CreateRet(builder.getTrue());
    builder.SetInsertPoint(no);
    builder.CreateRet(builder.getFalse());

    strMatchAtFn = fn;
    return fn;
}

llvm::Function* CodegenContext::getSplitFn() {
    if (splitFn) return splitFn;
    llvm::LLVMContext& c = *context;
    auto* i8Ty = llvm::Type::getInt8Ty(c);
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* ptrTy = llvm::PointerType::get(c, 0);
    auto* fnTy = llvm::FunctionType::get(
        ptrTy, {ptrTy, i64Ty, ptrTy, i64Ty, llvm::PointerType::get(i64Ty, 0)},
        false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.str.split", module.get());
    auto mallocFn = module->getOrInsertFunction(
        "malloc", llvm::FunctionType::get(ptrTy, {i64Ty}, false));
    // Synthesize the callee FIRST (it re-points the shared builder).
    llvm::Function* matchAt = getStrMatchAtFn();

    auto* s = fn->getArg(0);
    auto* slen = fn->getArg(1);
    auto* sep = fn->getArg(2);
    auto* seplen = fn->getArg(3);
    auto* outCnt = fn->getArg(4);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "entry", fn);
    llvm::BasicBlock* countLoop = llvm::BasicBlock::Create(c, "count.loop", fn);
    llvm::BasicBlock* countBody = llvm::BasicBlock::Create(c, "count.body", fn);
    llvm::BasicBlock* countSep = llvm::BasicBlock::Create(c, "count.sep", fn);
    llvm::BasicBlock* countAdv = llvm::BasicBlock::Create(c, "count.adv", fn);
    llvm::BasicBlock* allocBB = llvm::BasicBlock::Create(c, "alloc", fn);
    llvm::BasicBlock* fillLoop = llvm::BasicBlock::Create(c, "fill.loop", fn);
    llvm::BasicBlock* fillBody = llvm::BasicBlock::Create(c, "fill.body", fn);
    llvm::BasicBlock* fillSep = llvm::BasicBlock::Create(c, "fill.sep", fn);
    llvm::BasicBlock* fillAdv = llvm::BasicBlock::Create(c, "fill.adv", fn);
    llvm::BasicBlock* fillTail = llvm::BasicBlock::Create(c, "fill.tail", fn);

    builder.SetInsertPoint(entry);
    auto* cntA = builder.CreateAlloca(i64Ty, nullptr, "sp.cnt");
    auto* iA = builder.CreateAlloca(i64Ty, nullptr, "sp.i");
    auto* psA = builder.CreateAlloca(i64Ty, nullptr, "sp.ps");
    auto* idxA = builder.CreateAlloca(i64Ty, nullptr, "sp.idx");
    auto* arrA = builder.CreateAlloca(ptrTy, nullptr, "sp.arr");
    auto* one = llvm::ConstantInt::get(i64Ty, 1);
    auto zero = llvm::ConstantInt::get(i64Ty, 0);
    auto sixteen = llvm::ConstantInt::get(i64Ty, 16);

    builder.CreateStore(one, cntA);
    builder.CreateStore(zero, iA);
    builder.CreateStore(zero, psA);
    builder.CreateStore(zero, idxA);
    builder.CreateBr(countLoop);

    // Pass 1: greedy non-overlapping separator count; parts = count + 1.
    builder.SetInsertPoint(countLoop);
    auto* ci = builder.CreateLoad(i64Ty, iA, "sp.ci");
    auto* ciNext = builder.CreateAdd(ci, seplen, "sp.cinext");
    auto* cmore = builder.CreateICmpULE(ciNext, slen, "sp.cmore");
    builder.CreateCondBr(cmore, countBody, allocBB);
    builder.SetInsertPoint(countBody);
    auto* chit = builder.CreateCall(matchAt, {s, sep, seplen, ci}, "sp.chit");
    builder.CreateCondBr(chit, countSep, countAdv);
    builder.SetInsertPoint(countSep);
    builder.CreateStore(
        builder.CreateAdd(builder.CreateLoad(i64Ty, cntA, "sp.ccnt"), one, "sp.cnt.next"),
        cntA);
    builder.CreateStore(ciNext, iA);
    builder.CreateBr(countLoop);
    builder.SetInsertPoint(countAdv);
    builder.CreateStore(builder.CreateAdd(ci, one, "sp.ci.adv"), iA);
    builder.CreateBr(countLoop);

    builder.SetInsertPoint(allocBB);
    auto* cnt = builder.CreateLoad(i64Ty, cntA, "sp.cnt.final");
    auto* bytes = builder.CreateMul(cnt, sixteen, "sp.bytes");
    auto* arr = builder.CreateCall(mallocFn, {bytes}, "sp.arr.v");
    builder.CreateStore(arr, arrA);
    builder.CreateStore(zero, iA);
    builder.CreateBr(fillLoop);

    // Pass 2: write each {ptr, len} view; parts span [partStart, sepStart).
    builder.SetInsertPoint(fillLoop);
    auto* fi = builder.CreateLoad(i64Ty, iA, "sp.fi");
    auto* fiNext = builder.CreateAdd(fi, seplen, "sp.finext");
    auto* fmore = builder.CreateICmpULE(fiNext, slen, "sp.fmore");
    builder.CreateCondBr(fmore, fillBody, fillTail);
    builder.SetInsertPoint(fillBody);
    auto* fhit = builder.CreateCall(matchAt, {s, sep, seplen, fi}, "sp.fhit");
    builder.CreateCondBr(fhit, fillSep, fillAdv);
    builder.SetInsertPoint(fillSep);
    {
        auto* idx = builder.CreateLoad(i64Ty, idxA, "sp.fidx");
        auto* ps = builder.CreateLoad(i64Ty, psA, "sp.fps");
        auto* arrV = builder.CreateLoad(ptrTy, arrA, "sp.farr");
        auto* slot = builder.CreateGEP(i8Ty, arrV,
            builder.CreateMul(idx, sixteen, "sp.slot.off"), "sp.slot");
        builder.CreateStore(builder.CreateGEP(i8Ty, s, ps, "sp.part.p"), slot);
        builder.CreateStore(builder.CreateSub(fi, ps, "sp.part.l"),
            builder.CreateGEP(i64Ty, slot, one, "sp.slot.l"));
        builder.CreateStore(builder.CreateAdd(idx, one, "sp.idx.next"), idxA);
        builder.CreateStore(fiNext, iA);
        builder.CreateStore(fiNext, psA);
        builder.CreateBr(fillLoop);
    }
    builder.SetInsertPoint(fillAdv);
    builder.CreateStore(builder.CreateAdd(fi, one, "sp.fi.adv"), iA);
    builder.CreateBr(fillLoop);

    builder.SetInsertPoint(fillTail);
    {
        auto* idx = builder.CreateLoad(i64Ty, idxA, "sp.tidx");
        auto* ps = builder.CreateLoad(i64Ty, psA, "sp.tps");
        auto* arrV = builder.CreateLoad(ptrTy, arrA, "sp.tarr");
        auto* slot = builder.CreateGEP(i8Ty, arrV,
            builder.CreateMul(idx, sixteen, "sp.tslot.off"), "sp.tslot");
        builder.CreateStore(builder.CreateGEP(i8Ty, s, ps, "sp.tpart.p"), slot);
        builder.CreateStore(builder.CreateSub(slen, ps, "sp.tpart.l"),
            builder.CreateGEP(i64Ty, slot, one, "sp.tslot.l"));
        builder.CreateStore(cnt, outCnt);
        builder.CreateRet(arrV);
    }

    splitFn = fn;
    return fn;
}

// I4 (D2): 新鲜 string 值——rvalue 且为返回 string 的调用（string.new / 函数
// 调用 / 方法调用）→ 移动（零拷贝）而非深拷贝。spec D2 拷贝模型 B：活值深拷贝、
// 新鲜值移动、return 特殊规则；无法判定 → 拷贝（默认安全）。
bool CodegenContext::isFreshStringValue(ExprAST& e) {
    if (e.isLValue) return false;
    if (!dynamic_cast<CallExprAST*>(&e) &&
        !dynamic_cast<MethodCallExprAST*>(&e))
        return false;
    Type* t = e.type;
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    return t && t->kind == TypeKind::String;
}

// I4 (D7): `void stringCopy({ptr,len,cap}* dst, {ptr,len,cap} src)` —
// 深拷贝：cap = max(len,1)，malloc(cap)+memcpy，写 dst 三字段。调用方须
// saveIP/restoreIP（惰性合成期间借用共享 builder）。
llvm::Function* CodegenContext::getStringCopyFn() {
    if (stringCopyFn) return stringCopyFn;
    llvm::LLVMContext& c = *context;
    auto* i64Ty = llvm::Type::getInt64Ty(c);
    auto* ptrTy = llvm::PointerType::get(c, 0);
    auto* strTy = getLLVMType(TypeContext::instance().getStringType());
    auto* voidTy = llvm::Type::getVoidTy(c);
    auto* fnTy = llvm::FunctionType::get(voidTy, {ptrTy, strTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.string.copy", module.get());
    auto* dst = fn->getArg(0);
    auto* src = fn->getArg(1);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "copy.entry", fn);
    builder.SetInsertPoint(entry);
    auto* sptr = builder.CreateExtractValue(src, 0, "copy.sptr");
    auto* slen = builder.CreateExtractValue(src, 1, "copy.slen");
    auto* one = llvm::ConstantInt::get(i64Ty, 1);
    auto* cap = builder.CreateAdd(slen, one, "copy.len1");
    cap = builder.CreateSelect(
        builder.CreateICmpUGT(cap, one, "copy.gt1"), cap, one, "copy.cap");
    auto mallocFn = module->getOrInsertFunction(
        "malloc", llvm::FunctionType::get(ptrTy, {i64Ty}, false));
    auto memcpyFn = module->getOrInsertFunction(
        "memcpy", llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy, i64Ty}, false));
    auto* nptr = builder.CreateCall(mallocFn, {cap}, "copy.nptr");
    builder.CreateCall(memcpyFn, {nptr, sptr, slen});
    auto* end = builder.CreateGEP(llvm::Type::getInt8Ty(c), nptr, slen, "copy.end");
    builder.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(c), 0), end);
    llvm::Value* v = llvm::Constant::getNullValue(strTy);
    v = builder.CreateInsertValue(v, nptr, {0});
    v = builder.CreateInsertValue(v, slen, {1});
    v = builder.CreateInsertValue(v, cap, {2});
    builder.CreateStore(v, dst);
    builder.CreateRetVoid();

    stringCopyFn = fn;
    return fn;
}

// I4 (D7): `void stringAssign({ptr,len,cap}* dst, {ptr,len,cap} src)` —
// 赋值语义：自赋值守卫（dst.ptr == src.ptr 时什么都不做——free 会双 free、
// copy 会泄漏旧缓冲，值本就相同）；否则 dst.ptr != null 时 free 旧缓冲；
// 再深拷贝。让 `b = a` 后 a/b 各自独立、旧缓冲自动释放。
llvm::Function* CodegenContext::getStringAssignFn() {
    if (stringAssignFn) return stringAssignFn;
    llvm::LLVMContext& c = *context;
    auto* ptrTy = llvm::PointerType::get(c, 0);
    auto* strTy = getLLVMType(TypeContext::instance().getStringType());
    auto* voidTy = llvm::Type::getVoidTy(c);
    auto* fnTy = llvm::FunctionType::get(voidTy, {ptrTy, strTy}, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "smc.string.assign", module.get());
    auto* dst = fn->getArg(0);
    auto* src = fn->getArg(1);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(c, "asn.entry", fn);
    llvm::BasicBlock* selfBB = llvm::BasicBlock::Create(c, "asn.self", fn);
    llvm::BasicBlock* freeBB = llvm::BasicBlock::Create(c, "asn.free", fn);
    llvm::BasicBlock* doFreeBB = llvm::BasicBlock::Create(c, "asn.dofree", fn);
    llvm::BasicBlock* copyBB = llvm::BasicBlock::Create(c, "asn.copy", fn);
    builder.SetInsertPoint(entry);
    auto* dv = builder.CreateLoad(strTy, dst, "asn.dv");
    auto* dptr = builder.CreateExtractValue(dv, 0, "asn.dptr");
    auto* sptr = builder.CreateExtractValue(src, 0, "asn.sptr");
    builder.CreateCondBr(builder.CreateICmpEQ(dptr, sptr, "asn.self?"),
                         selfBB, freeBB);
    builder.SetInsertPoint(selfBB);
    builder.CreateRetVoid();
    builder.SetInsertPoint(freeBB);
    auto* nullPtr = llvm::Constant::getNullValue(ptrTy);
    builder.CreateCondBr(builder.CreateICmpNE(dptr, nullPtr, "asn.hasold?"),
                         doFreeBB, copyBB);
    builder.SetInsertPoint(doFreeBB);
    auto freeFn = module->getOrInsertFunction(
        "free", llvm::FunctionType::get(voidTy, {ptrTy}, false));
    builder.CreateCall(freeFn, {dptr});
    builder.CreateBr(copyBB);
    builder.SetInsertPoint(copyBB);
    // getStringCopyFn() 惰性合成期间会劫持共享 builder 的插入点——save/restore。
    auto savedIP = builder.saveIP();
    llvm::Function* copyFn = getStringCopyFn();
    builder.restoreIP(savedIP);
    builder.CreateCall(copyFn, {dst, src});
    builder.CreateRetVoid();

    stringAssignFn = fn;
    return fn;
}

// I4 (D7): 递归检查聚合是否（直接或间接）含 string 字段。数组/optional/result
// 等视为不敏感（整段原样拷贝，不深拷贝其中的 string——记录为已知限制）。
bool CodegenContext::aggregateHasStringField(Type* t) {
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::String:
            return true;
        case TypeKind::Struct:
            for (auto& f : static_cast<StructType*>(t)->fields)
                if (aggregateHasStringField(f.type)) return true;
            return false;
        case TypeKind::Class: {
            auto* ct = static_cast<ClassType*>(t);
            for (auto& f : ct->fields)
                if (aggregateHasStringField(f.type)) return true;
            return ct->base ? aggregateHasStringField(ct->base) : false;
        }
        // Found issue: 数组/optional/result 内含 string 也要字段感知
        //（否则整段浅拷贝 → 元素共享缓冲 → 双 free）。
        case TypeKind::Array:
            return aggregateHasStringField(static_cast<ArrayType*>(t)->elementType);
        case TypeKind::Optional:
            return aggregateHasStringField(static_cast<OptionalType*>(t)->elementType);
        case TypeKind::Result: {
            auto* rt = static_cast<ResultType*>(t);
            return aggregateHasStringField(rt->successType) ||
                   aggregateHasStringField(rt->errorType);
        }
        default:
            return false;
    }
}

// I4 (D7): 单值字段感知拷贝——string → copy/assign helper（fetch 时 save/restore
// 共享 builder）；含 string 聚合 → 递归；其余 → 裸 store。
void CodegenContext::emitStringValueCopy(llvm::Value* dstPtr, llvm::Value* srcVal,
                                         Type* t, bool assign) {
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    if (!t) return;
    switch (t->kind) {
        case TypeKind::String: {
            auto ip = builder.saveIP();
            llvm::Function* fn = assign ? getStringAssignFn() : getStringCopyFn();
            builder.restoreIP(ip);
            builder.CreateCall(fn, {dstPtr, srcVal});
            return;
        }
        case TypeKind::Struct:
        case TypeKind::Class:
        case TypeKind::Array:
        case TypeKind::Optional:
        case TypeKind::Result:
            if (aggregateHasStringField(t)) {
                emitAggregateStringCopy(dstPtr, srcVal, t, assign);
            } else {
                builder.CreateStore(srcVal, dstPtr);
            }
            return;
        default:
            builder.CreateStore(srcVal, dstPtr);
            return;
    }
}

// I4 (D7): 字段感知聚合拷贝。dst=目标聚合地址，srcVal=源聚合值，aggType=聚合
// 类型。string 字段深拷贝（assign=true → 先 free 目标旧缓冲 + 拷贝）；其余
// 字段原样；嵌套聚合递归。helper 合成期间会劫持共享 builder——整体 save/restore。
// 支持 Struct/Class/Array/Optional/Result（found issue 扩展）。
void CodegenContext::emitAggregateStringCopy(llvm::Value* dst, llvm::Value* srcVal,
                                             Type* aggType, bool assign) {
    while (aggType && aggType->kind == TypeKind::Typedef)
        aggType = static_cast<TypedefType*>(aggType)->aliasedType;
    if (!aggType) {
        return;
    }
    llvm::LLVMContext& c = *context;
    // 注意：不 save/restore 外层插入点——Struct/Class/Array 不改变插入块；
    // Optional/Result 的收尾块（doneBB）是控制流汇合点，调用方必须从其继续。
    switch (aggType->kind) {
        case TypeKind::Struct:
        case TypeKind::Class: {
            llvm::Type* llvmTy = getLLVMType(aggType);
            std::vector<FieldInfo>* fields = nullptr;
            Type* baseType = nullptr;
            unsigned baseOffset = 0;
            if (aggType->kind == TypeKind::Struct) {
                fields = &static_cast<StructType*>(aggType)->fields;
            } else {
                auto* ct = static_cast<ClassType*>(aggType);
                fields = &ct->fields;
                baseType = ct->base;
                if (!ct->baseClass.empty()) baseOffset = 1;
            }
            // 基类子对象占 slot 0（INH-01），递归。
            if (baseType && aggregateHasStringField(baseType)) {
                llvm::Value* baseAddr = builder.CreateStructGEP(llvmTy, dst, 0, "agg.basep");
                llvm::Value* baseVal = builder.CreateExtractValue(srcVal, 0, "agg.base");
                emitAggregateStringCopy(baseAddr, baseVal, baseType, assign);
            }
            for (size_t i = 0; i < fields->size(); ++i) {
                unsigned idx = baseOffset + (unsigned)i;
                llvm::Value* faddr = builder.CreateStructGEP(llvmTy, dst, idx, "agg.fp");
                llvm::Value* fval = builder.CreateExtractValue(srcVal, idx, "agg.fv");
                emitStringValueCopy(faddr, fval, (*fields)[i].type, assign);
            }
            break;
        }
        case TypeKind::Array: {
            auto* arrTy = static_cast<ArrayType*>(aggType);
            llvm::Type* arrLLVM = getLLVMType(arrTy);
            auto* i32 = llvm::Type::getInt32Ty(c);
            for (int i = 0; i < arrTy->size; ++i) {
                llvm::Value* elemPtr = builder.CreateInBoundsGEP(
                    arrLLVM, dst,
                    {llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, i)},
                    "agg.arrp");
                llvm::Value* elemVal = builder.CreateExtractValue(srcVal, i, "agg.arrv");
                emitStringValueCopy(elemPtr, elemVal, arrTy->elementType, assign);
            }
            break;
        }
        case TypeKind::Optional: {
            // {i1 valid, T value} — valid 时深拷贝 value。
            auto* optTy = static_cast<OptionalType*>(aggType);
            llvm::Type* optLLVM = getLLVMType(optTy);
            llvm::Value* srcValid = builder.CreateExtractValue(srcVal, 0, "agg.opt.valid");
            llvm::Value* srcV = builder.CreateExtractValue(srcVal, 1, "agg.opt.v");
            llvm::Value* dstValidSlot = builder.CreateStructGEP(optLLVM, dst, 0, "agg.opt.vp");
            llvm::Value* dstVSlot = builder.CreateStructGEP(optLLVM, dst, 1, "agg.opt.vp.v");
            // 遗留缺陷修复：目标"变为失效"（srcValid=false）时 value 槽必须释放
            // 旧缓冲——先读旧 valid 位，旧有效则 free+置零槽位；否则旧缓冲泄漏
            // 且槽位残留（破坏"valid⟹value 含缓冲"不变量）。
            llvm::Value* oldValid = builder.CreateLoad(
                optLLVM->getStructElementType(0), dstValidSlot, "agg.opt.oldvalid");
            llvm::Function* curFn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(c, "agg.opt.body", curFn);
            llvm::BasicBlock* staleBB = llvm::BasicBlock::Create(c, "agg.opt.stale", curFn);
            llvm::BasicBlock* freeBB = llvm::BasicBlock::Create(c, "agg.opt.free", curFn);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(c, "agg.opt.done", curFn);
            builder.CreateCondBr(srcValid, bodyBB, staleBB);
            builder.SetInsertPoint(bodyBB);
            emitStringValueCopy(dstVSlot, srcV, optTy->elementType, assign);
            builder.CreateBr(doneBB);
            builder.SetInsertPoint(staleBB);
            builder.CreateCondBr(oldValid, freeBB, doneBB);
            builder.SetInsertPoint(freeBB);
            emitStringValueRelease(dstVSlot, optTy->elementType);
            builder.CreateBr(doneBB);
            builder.SetInsertPoint(doneBB);
            builder.CreateStore(srcValid, dstValidSlot);
            break;
        }
        case TypeKind::Result: {
            // {i1 ok, T value, E error} — ok 时拷贝 value、!ok 时拷贝 error。
            auto* rt = static_cast<ResultType*>(aggType);
            llvm::Type* resLLVM = getLLVMType(rt);
            llvm::Value* srcOk = builder.CreateExtractValue(srcVal, 0, "agg.res.ok");
            llvm::Value* srcV = builder.CreateExtractValue(srcVal, 1, "agg.res.v");
            llvm::Value* srcE = builder.CreateExtractValue(srcVal, 2, "agg.res.e");
            llvm::Value* dstOkSlot = builder.CreateStructGEP(resLLVM, dst, 0, "agg.res.okp");
            llvm::Value* dstVSlot = builder.CreateStructGEP(resLLVM, dst, 1, "agg.res.vp");
            llvm::Value* dstESlot = builder.CreateStructGEP(resLLVM, dst, 2, "agg.res.ep");
            // 遗留缺陷修复：变为失效的槽位须释放旧缓冲——ok→err 时旧 value 缓冲
            // （oldOk 为真）、err→ok 时旧 error 缓冲（oldOk 为假）free+置零。
            llvm::Value* oldOk = builder.CreateLoad(
                resLLVM->getStructElementType(0), dstOkSlot, "agg.res.oldok");
            llvm::Function* curFn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "agg.res.ok", curFn);
            llvm::BasicBlock* errBB = llvm::BasicBlock::Create(c, "agg.res.err", curFn);
            llvm::BasicBlock* freeErrBB = llvm::BasicBlock::Create(c, "agg.res.freeerr", curFn);
            llvm::BasicBlock* freeValBB = llvm::BasicBlock::Create(c, "agg.res.freeval", curFn);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(c, "agg.res.done", curFn);
            builder.CreateCondBr(srcOk, okBB, errBB);
            builder.SetInsertPoint(okBB);
            if (aggregateHasStringField(rt->successType))
                emitStringValueCopy(dstVSlot, srcV, rt->successType, assign);
            else
                builder.CreateStore(srcV, dstVSlot);
            builder.CreateCondBr(oldOk, doneBB, freeErrBB);
            builder.SetInsertPoint(freeErrBB);
            emitStringValueRelease(dstESlot, rt->errorType);
            builder.CreateBr(doneBB);
            builder.SetInsertPoint(errBB);
            if (aggregateHasStringField(rt->errorType))
                emitStringValueCopy(dstESlot, srcE, rt->errorType, assign);
            else
                builder.CreateStore(srcE, dstESlot);
            builder.CreateCondBr(oldOk, freeValBB, doneBB);
            builder.SetInsertPoint(freeValBB);
            emitStringValueRelease(dstVSlot, rt->successType);
            builder.CreateBr(doneBB);
            builder.SetInsertPoint(doneBB);
            builder.CreateStore(srcOk, dstOkSlot);
            break;
        }
        default:
            return;
    }
}

// 遗留缺陷修复（新遗留项）：单值槽位释放——string → free(null 守卫)+置零；
// 含 string 聚合 → 递归（emitAggregateStringRelease）；其余（无拥有资源）→
// 无操作。用于 optional/result 赋值时"变为失效"的字段槽：释放旧缓冲并置零，
// 维持"失效 ⟹ 槽位零化"不变量（否则后续赋值 helper 会对悬垂指针二次 free）。
void CodegenContext::emitStringValueRelease(llvm::Value* dstPtr, Type* t) {
    while (t && t->kind == TypeKind::Typedef)
        t = static_cast<TypedefType*>(t)->aliasedType;
    if (!t) return;
    switch (t->kind) {
        case TypeKind::String: {
            llvm::LLVMContext& c = *context;
            auto* ptrTy = llvm::PointerType::get(c, 0);
            auto* voidTy = llvm::Type::getVoidTy(c);
            llvm::Type* strTy = getLLVMType(t);
            llvm::Value* cur = builder.CreateLoad(strTy, dstPtr, "rel.dv");
            llvm::Value* dptr = builder.CreateExtractValue(cur, 0, "rel.dptr");
            llvm::Function* curFn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* doFreeBB = llvm::BasicBlock::Create(c, "rel.free", curFn);
            llvm::BasicBlock* zeroBB = llvm::BasicBlock::Create(c, "rel.zero", curFn);
            llvm::Value* nullPtr = llvm::Constant::getNullValue(ptrTy);
            builder.CreateCondBr(builder.CreateICmpNE(dptr, nullPtr, "rel.hasold?"),
                                 doFreeBB, zeroBB);
            builder.SetInsertPoint(doFreeBB);
            auto freeFn = module->getOrInsertFunction(
                "free", llvm::FunctionType::get(voidTy, {ptrTy}, false));
            builder.CreateCall(freeFn, {dptr});
            builder.CreateBr(zeroBB);
            builder.SetInsertPoint(zeroBB);
            builder.CreateStore(llvm::Constant::getNullValue(strTy), dstPtr);
            return;
        }
        case TypeKind::Struct:
        case TypeKind::Class:
        case TypeKind::Array:
        case TypeKind::Optional:
        case TypeKind::Result:
            if (aggregateHasStringField(t))
                emitAggregateStringRelease(dstPtr, t);
            return;
        default:
            return;
    }
}

// 遗留缺陷修复（新遗留项）：聚合槽位释放——镜像 emitAggregateStringCopy 的
// 递归结构，只做"释放拥有缓冲 + 置零"，不做拷贝。Struct/Class/Array/
// Optional/Result 全支持（失效槽可能承载嵌套聚合）。
void CodegenContext::emitAggregateStringRelease(llvm::Value* dst, Type* aggType) {
    while (aggType && aggType->kind == TypeKind::Typedef)
        aggType = static_cast<TypedefType*>(aggType)->aliasedType;
    if (!aggType) return;
    llvm::LLVMContext& c = *context;
    switch (aggType->kind) {
        case TypeKind::Struct:
        case TypeKind::Class: {
            llvm::Type* llvmTy = getLLVMType(aggType);
            std::vector<FieldInfo>* fields = nullptr;
            Type* baseType = nullptr;
            unsigned baseOffset = 0;
            if (aggType->kind == TypeKind::Struct) {
                fields = &static_cast<StructType*>(aggType)->fields;
            } else {
                auto* ct = static_cast<ClassType*>(aggType);
                fields = &ct->fields;
                baseType = ct->base;
                if (!ct->baseClass.empty()) baseOffset = 1;
            }
            if (baseType && aggregateHasStringField(baseType)) {
                llvm::Value* baseAddr = builder.CreateStructGEP(llvmTy, dst, 0, "rel.basep");
                emitAggregateStringRelease(baseAddr, baseType);
            }
            for (size_t i = 0; i < fields->size(); ++i) {
                unsigned idx = baseOffset + (unsigned)i;
                llvm::Value* faddr = builder.CreateStructGEP(llvmTy, dst, idx, "rel.fp");
                emitStringValueRelease(faddr, (*fields)[i].type);
            }
            break;
        }
        case TypeKind::Array: {
            auto* arrTy = static_cast<ArrayType*>(aggType);
            llvm::Type* arrLLVM = getLLVMType(arrTy);
            auto* i32 = llvm::Type::getInt32Ty(c);
            for (int i = 0; i < arrTy->size; ++i) {
                llvm::Value* elemPtr = builder.CreateInBoundsGEP(
                    arrLLVM, dst,
                    {llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, i)},
                    "rel.arrp");
                emitStringValueRelease(elemPtr, arrTy->elementType);
            }
            break;
        }
        case TypeKind::Optional: {
            auto* optTy = static_cast<OptionalType*>(aggType);
            llvm::Type* optLLVM = getLLVMType(optTy);
            llvm::Value* validSlot = builder.CreateStructGEP(optLLVM, dst, 0, "rel.opt.vp");
            llvm::Value* valSlot = builder.CreateStructGEP(optLLVM, dst, 1, "rel.opt.valp");
            llvm::Value* valid = builder.CreateLoad(
                optLLVM->getStructElementType(0), validSlot, "rel.opt.valid");
            llvm::Function* curFn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(c, "rel.opt.body", curFn);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(c, "rel.opt.done", curFn);
            builder.CreateCondBr(valid, bodyBB, doneBB);
            builder.SetInsertPoint(bodyBB);
            emitStringValueRelease(valSlot, optTy->elementType);
            builder.CreateBr(doneBB);
            builder.SetInsertPoint(doneBB);
            builder.CreateStore(llvm::ConstantInt::getFalse(c), validSlot);
            break;
        }
        case TypeKind::Result: {
            auto* rt = static_cast<ResultType*>(aggType);
            llvm::Type* resLLVM = getLLVMType(rt);
            llvm::Value* okSlot = builder.CreateStructGEP(resLLVM, dst, 0, "rel.res.okp");
            llvm::Value* valSlot = builder.CreateStructGEP(resLLVM, dst, 1, "rel.res.valp");
            llvm::Value* errSlot = builder.CreateStructGEP(resLLVM, dst, 2, "rel.res.errp");
            llvm::Value* ok = builder.CreateLoad(
                resLLVM->getStructElementType(0), okSlot, "rel.res.ok");
            llvm::Function* curFn = builder.GetInsertBlock()->getParent();
            llvm::BasicBlock* okBB = llvm::BasicBlock::Create(c, "rel.res.ok", curFn);
            llvm::BasicBlock* errBB = llvm::BasicBlock::Create(c, "rel.res.err", curFn);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(c, "rel.res.done", curFn);
            builder.CreateCondBr(ok, okBB, errBB);
            builder.SetInsertPoint(okBB);
            emitStringValueRelease(valSlot, rt->successType);
            builder.CreateBr(doneBB);
            builder.SetInsertPoint(errBB);
            emitStringValueRelease(errSlot, rt->errorType);
            builder.CreateBr(doneBB);
            builder.SetInsertPoint(doneBB);
            builder.CreateStore(llvm::ConstantInt::getFalse(c), okSlot);
            break;
        }
        default:
            return;
    }
}
