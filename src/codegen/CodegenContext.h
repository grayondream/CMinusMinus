#pragma once

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "ast/Symbol.h"
#include <map>

class Type;
class ExprAST;

class CodegenContext {
public:
    CodegenContext();

    llvm::LLVMContext& getContext() { return *context; }
    llvm::IRBuilder<>& getBuilder() { return builder; }
    llvm::Module& getModule() { return *module; }
    llvm::DIBuilder& getDIBuilder() { return *diBuilder; }
    llvm::DIFile* getDIFile() { return diFile; }

    std::unique_ptr<llvm::Module> takeModule() { return std::move(module); }

    // Releasing the context (e.g. into LLJIT, which destroys it after compiling
    // the module) would leave the IRBuilder's current DebugLoc pointing at
    // freed debug metadata — ~IRBuilder then use-after-frees in
    // MetadataTracking::untrack. Detach the debug location first.
    std::unique_ptr<llvm::LLVMContext> takeContext() {
        builder.SetCurrentDebugLocation(llvm::DebugLoc());
        return std::move(context);
    }

    void pushScope();
    void popScope();
    Scope* currentScope();
    bool isGlobalScope() const;

    llvm::Value* lookupVariable(const std::string& name);
    llvm::Value* lookupVariableAddr(const std::string& name);
    void declareVariable(const std::string& name, llvm::Value* alloca, Type* type);

    // Load the value denoted by an lvalue pointer. Arrays decay to a pointer to
    // their first element instead of being loaded by value.
    llvm::Value* loadValue(llvm::Value* ptr, Type* type);

    // P1-07 / CG-09: bit-field storage-unit read/write (read-modify-write).
    // unitAddr points at the integer storage unit (materialized LLVM field);
    // the field spans [bitOffset, bitOffset+bitWidth) of that unit, counted
    // from the LSB. unitBits is the storage unit's total bit width (base-type
    // width for plain fields, region width for packed, alignType width for
    // unions — recorded in BitFieldLayout at materialization). loadBitFieldValue
    // returns the field as a base-type value (bool → i1; signed → sext;
    // unsigned → zext). storeBitFieldValue clears the field bits, truncates+
    // shifts the value in, and stores (no atomicity).
    llvm::Value* loadBitFieldValue(llvm::Value* unitAddr, uint64_t bitOffset,
                                   uint32_t bitWidth, Type* fieldType,
                                   uint32_t unitBits);
    void storeBitFieldValue(llvm::Value* unitAddr, uint64_t bitOffset,
                            uint32_t bitWidth, Type* fieldType, llvm::Value* val,
                            uint32_t unitBits);

    llvm::Type* getLLVMType(Type* type);

    // CON-01: atomic memory is accessed through an integer carrier so bool,
    // float and pointer atomics can use LLVM's integer atomic instructions.
    // Returns nullptr for unsupported widths (e.g. 16-byte atomics).
    llvm::Type* getAtomicCarrierType(Type* type, unsigned& bits);
    unsigned getAtomicAlignBits(Type* type);
    llvm::Value* valueToAtomicCarrier(llvm::Value* value, Type* type,
                                      llvm::Type* carrier);
    llvm::Value* valueFromAtomicCarrier(llvm::Value* value, Type* type,
                                        llvm::Type* carrier);
    unsigned getAtomicByteSize(Type* type);
    unsigned getPointeeByteSize(Type* pointerType);

    void setSourceFile(const std::string& file);
    void emitFunctionDebug(llvm::Function* func, Type* returnType,
                           const std::string& name, unsigned line);
    void emitVariableDebug(llvm::AllocaInst* alloca, const std::string& name,
                           Type* type, unsigned line);
    void setDebugLocation(unsigned line, unsigned col = 0);
    void finalizeDebugInfo();

    void pushBreakBlock(llvm::BasicBlock* bb);
    void popBreakBlock();
    llvm::BasicBlock* getBreakBlock() const;

    void pushContinueBlock(llvm::BasicBlock* bb);
    void popContinueBlock();
    llvm::BasicBlock* getContinueBlock() const;


    // Defer statement support. Deferred expressions are registered while a
    // compound statement is generated and emitted when its scope exits, or
    // before a return/break/continue that leaves the scope.
    void pushDeferScope();
    void popDeferScope();       // emit and drop the innermost scope
    void discardDeferScope();   // drop the innermost scope without emitting
    void addDefer(ExprAST* expr);
    // Review 2.5: owning locals are released when their lexical scope exits.
    void addStringCleanup(llvm::Value* addr, Type* type);
    void emitDefersFrom(size_t depth);  // emit scopes [size-1 .. depth], innermost first
    void emitAllDefers();
    size_t getBreakDeferBoundary() const;
    size_t getContinueDeferBoundary() const;

    llvm::Value* coerceToBool(llvm::Value* val);
    // Review 2.4: Optional/Result conditions use their valid/ok flag.
    llvm::Value* coerceToBool(llvm::Value* val, Type* astType);

    // Insert an implicit conversion of `val` to `targetLLVMType`, following the
    // usual C arithmetic conversion rules. Returns `val` unchanged when no
    // conversion is needed or possible.
    llvm::Value* castValue(llvm::Value* val, llvm::Type* targetLLVMType);

    // Signedness-aware variant (TYP-22): `fromAST` supplies the source type so
    // that widening an unsigned integer uses `zext` (and int->float uses
    // `uitofp`) instead of the signed variants.
    llvm::Value* castValue(llvm::Value* val, Type* fromAST, llvm::Type* targetLLVMType);

    // TYP-12: build the {ptr, len} slice view over a statically-sized array
    // (zero-copy; the elements are never duplicated).
    llvm::Value* emitArrayToSliceDecay(ArrayType* arrayType, llvm::Value* arrayAddr);

    // char_count: number of code points in [p, p+len);
    // char_len_at: sequence width at byte i (0 on an invalid lead byte);
    // validate: true when [p, p+len) is valid UTF-8 (used at char* -> str
    // conversion points).
    llvm::Function* getUtf8CharCountFn();
    llvm::Function* getUtf8CharLenAtFn();
    llvm::Function* getUtf8ValidateFn();

    // P1-09 (FMT-07): dynamic format() runtime scan. Walks the format string,
    // interleaving literal bytes with pre-rendered argument chunks
    // ({ptr,len} arrays). Escapes {{ / }}; a '{' consumes the next chunk
    // (spec text inside dynamic placeholders is ignored); an unterminated '{'
    // stops the scan. Returns the string layout {ptr, len, cap} with a single
    // malloc sized fmtLen + sum(chunkLens) + 1 (FMT-12: one buffer, no
    // realloc).
    llvm::Function* getFormatDynFn();

    // P1-09 (STD-10): naive byte scan of needle in hay. Returns the match
    // index as i64, -1 when absent. An empty needle yields 0 (forward) or
    // hayLen (reverse).
    llvm::Function* getStrFindFn(bool reverse);

    // P1-09 (STD-10): does sep match hay at byte offset `at`? (i1)
    llvm::Function* getStrMatchAtFn();

    // P1-09 (STD-10): greedy non-overlapping split. Returns a malloc'd array
    // of str views ({i8*, i64} = 16 bytes each) and writes the part count to
    // *outCnt. Assumes seplen > 0 (checked at the call site).
    llvm::Function* getSplitFn();

    // I4 (D2/D7): string 拥有型值语义 helper。string 是 {ptr, len, cap} 的
    // 拥有型值——赋值/初始化/传参/返回处深拷贝活值、移动新鲜值，从根上消除
    // I4「值拷贝别名双 free」。这些 helper 惰性合成（InternalLinkage），调用方
    // 须在合成前 saveIP/restoreIP（与 getStrFindFn 等同一约定）。
    //
    // isFreshStringValue: rvalue 且为返回 string 的调用（string.new / 函数调用
    // 等）→ 新鲜值，移动（零拷贝）而非深拷贝（spec D2 拷贝模型 B）。
    bool isFreshStringValue(ExprAST& e);
    // stringCopy: dst* ← 深拷贝 src（cap = max(len,1)，malloc+memcpy，写三字段）。
    llvm::Function* getStringCopyFn();
    // stringAssign: dst* ← src。自赋值守卫（dst.ptr == src.ptr 时什么都不做，
    // 不得 free 共享缓冲）；否则 dst.ptr != null 时 free 旧缓冲，再深拷贝。
    llvm::Function* getStringAssignFn();

    // I4 (D7, spec §3.1/§5): 聚合字段感知拷贝。聚合（Struct/Class）含 string
    // 字段时，整段拷贝会共享缓冲 → 双 free；须逐字段处理：string 字段深拷贝
    // （assign=true → getStringAssignFn：自赋值守卫 + free 旧 + 拷贝；false →
    // getStringCopyFn），其余字段原样 extract+store，嵌套聚合递归。
    bool aggregateHasStringField(Type* t);
    void emitAggregateStringCopy(llvm::Value* dst, llvm::Value* srcVal,
                                 Type* aggType, bool assign);
    // 单值字段感知拷贝（供 emitAggregateStringCopy 递归复用）：
    // string → copy/assign helper；含 string 聚合 → 递归；其余 → 裸 store。
    void emitStringValueCopy(llvm::Value* dstPtr, llvm::Value* srcVal,
                             Type* t, bool assign);
    // 聚合槽位释放（optional/result 赋值使某字段"变为失效"时用）：
    // 释放槽位内全部 string 缓冲并置零，维持"失效 ⟹ 槽位零化"不变量。
    // 镜像 emitAggregateStringCopy 的递归结构；非 string 承载 → 无操作。
    void emitStringValueRelease(llvm::Value* dstPtr, Type* t);
    void emitAggregateStringRelease(llvm::Value* dst, Type* aggType);

private:
    std::unique_ptr<llvm::LLVMContext> context;
    llvm::IRBuilder<> builder;
    std::unique_ptr<llvm::Module> module;
    std::vector<std::unique_ptr<Scope>> scopes;

    std::unique_ptr<llvm::DIBuilder> diBuilder;
    llvm::DIFile* diFile{nullptr};
    llvm::DICompileUnit* diCompileUnit{nullptr};
    llvm::DIScope* diCurrentScope{nullptr};
    std::string sourceFileName;

    std::vector<llvm::BasicBlock*> breakBlocks;
    std::vector<llvm::BasicBlock*> continueBlocks;

    std::vector<std::vector<ExprAST*>> deferScopes;
    std::vector<std::vector<std::pair<llvm::Value*, Type*>>> cleanupScopes;
    std::vector<size_t> breakDeferBoundaries;
    std::vector<size_t> continueDeferBoundaries;

    // P1-06: lazily synthesized UTF-8 helpers (single instance per module).
    llvm::Function* utf8CharLenAtFn{nullptr};
    llvm::Function* utf8CharCountFn{nullptr};
    llvm::Function* utf8ValidateFn{nullptr};
    llvm::Function* formatDynFn{nullptr};
    llvm::Function* strFindFn{nullptr};
    llvm::Function* strRFindFn{nullptr};
    llvm::Function* strMatchAtFn{nullptr};
    llvm::Function* splitFn{nullptr};
    // I4 (D7): lazily synthesized string ownership helpers.
    llvm::Function* stringCopyFn{nullptr};
    llvm::Function* stringAssignFn{nullptr};
};
