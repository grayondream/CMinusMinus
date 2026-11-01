#pragma once

// P1-04 / CT-06 / CT-14 / DEC-05: 编译期求值器。
// 与 constexpr 机制并存且共享内核（DEC-05 裁决）：constexpr 函数调用委托
// SemanticAnalyzer::evaluateConstexpr（其函数解释种子已存在）；compile_time
// 成员（target/build/size_of 族）由本类求值。ConstValue 权威定义在此，
// SemanticAnalyzer 以 using 别名引用，既有引用点不改动。

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <unordered_map>

#include "ast/LayoutBuilder.h"
#include "ast/Type.h"

class SemanticAnalyzer;
class ExprAST;
class ASTNode;
class MemberAccessExprAST;
class MethodCallExprAST;
class Type;
namespace llvm {
class LLVMContext;
class Module;
class Type;
}

class CompileTimeEvaluator {
public:
    struct ConstValue {
        enum Type { INT, DOUBLE, CHAR, STR, TYPE } type;
        // LEX-15: integer constants keep the full 64-bit literal range.
        long long intVal = 0;
        // CTX-01: unsigned integer constants keep their signedness so sema can
        // map e.g. uint_max(uint64) to the uint64 type instead of int64.
        bool isUnsigned = false;
        // CTX-01: bool literal provenance for compile_time.to_string.
        bool isBool = false;
        double doubleVal = 0;
        char charVal = 0;
        std::string strVal; // STR 时有效（union 不能放 std::string）
        ::Type* typeVal = nullptr; // TYPE 时有效（编译期类型值，不逃逸到运行时）
    };

    // CT-07: one named (non-padding) field's layout, for fields_of.
    struct ReflectField {
        std::string name;
        size_t offset = 0;
        size_t size = 0;
    };

    explicit CompileTimeEvaluator(SemanticAnalyzer& sema);
    ~CompileTimeEvaluator(); // llvm 类型仅前向声明，析构在 .cpp 定义

    // 求值表达式。不可常量折叠 → nullopt 且【不】发诊断（由调用方按场景发
    // 钉死文案）；compile_time 特有错误（未知成员/未知类型/深度超限）→
    // 诊断 + nullopt。`at` 供诊断定位。
    std::optional<ConstValue> eval(ExprAST* expr, ASTNode& at);

    // CT-07: fields_of 的字段布局（聚合类型：字段名/偏移/大小）。
    // 供 sema 的 analyzeReflectionCall 调用（LayoutBuilder 单一来源）。
    std::vector<ReflectField> evalReflectFields(Type* t, ASTNode& at);

private:
    // 递归深度上限（防编译期爆栈）；超限诊断一次（钉死文案，spec §3）。
    static constexpr int kMaxDepth = 64;

    std::optional<ConstValue> evalBinary(ConstValue& left, ConstValue& right, int op);
    // compile_time.<target|build>.<member> 值（CT-04/05）。
    std::optional<ConstValue> evalCTMemberChain(MemberAccessExprAST& node, ASTNode& at);
    // size_of/align_of/offset_of（CT-06 布局查询）。
    std::optional<ConstValue> evalLayoutQuery(MethodCallExprAST& node, ASTNode& at);

    // CTX-01: generalized compile_time query dispatch. All scalar/string
    // results stay in ConstValue and never reach codegen as runtime calls.
    std::optional<ConstValue> evalQuery(MethodCallExprAST& node, ASTNode& at);
    std::optional<ConstValue> evalTypeQuery(MethodCallExprAST& node, ASTNode& at);
    std::optional<ConstValue> evalFieldQuery(MethodCallExprAST& node, ASTNode& at);
    std::optional<ConstValue> evalEnumQuery(MethodCallExprAST& node, ASTNode& at);
    std::optional<ConstValue> evalClassQuery(MethodCallExprAST& node, ASTNode& at);
    std::optional<ConstValue> evalTypeValueQuery(MethodCallExprAST& node, ASTNode& at);
    std::optional<ConstValue> evalBuildCall(MethodCallExprAST& node, ASTNode& at);
    std::optional<ConstValue> evalFormat(MethodCallExprAST& node, ASTNode& at);

    // Resolve a type argument carried by the parser as SizeofExprAST, by a
    // user type name, or by a built-in scalar type spelling.
    Type* resolveTypeArg(ExprAST* arg, ASTNode& at, std::string* nameOut = nullptr);
    Type* builtinTypeByName(const std::string& name);
    Type* stripTypedef(Type* t);
    bool isAggregate(Type* t) const;
    bool isCompleteAggregate(Type* t) const;
    bool isIntegerKind(Type* t) const;
    bool isFloatKind(Type* t) const;
    int integerBitWidth(Type* t) const;
    bool typesSame(Type* a, Type* b);

    // Cached LayoutBuilder result for aggregates (memoized per Type* — repeated
    // size_of/field queries must not re-run layout).
    const LayoutResult* layoutFor(Type* t);
    std::vector<FieldInfo> collectFields(Type* t, bool includeBase = true);
    std::optional<FieldInfo> findField(Type* t, const std::string& name,
                                       bool includeBase = true);
    std::optional<uint64_t> fieldOffsetOf(Type* t, const std::string& name);
    const BitFieldLayout* bitFieldOf(Type* t, const std::string& name);

    // Evaluate an annotation argument ([[align(8)]], [[deprecated("msg")]], ...).
    std::optional<ConstValue> evalAnnotationArg(const Annotation& ann, size_t index,
                                                ASTNode& at);

    // Type* → llvm::Type（递归 + 缓存；布局规则与 CodegenContext::getLLVMType 一致，
    // 基类子对象占槽 0）。
    llvm::Type* toLLVMType(Type* t);

    SemanticAnalyzer& m_sema;
    int m_depth = 0;
    bool m_depthReported = false;
    // 布局查询承载（独立于 codegen 的 LLVMContext）。
    std::unique_ptr<llvm::LLVMContext> m_llvmCtx;
    std::unique_ptr<llvm::Module> m_module;
    std::unordered_map<Type*, llvm::Type*> m_typeCache;
    std::unordered_map<Type*, LayoutResult> m_layoutCache;
    std::unordered_map<Type*, LayoutResult> m_unionLayoutCache;
};
