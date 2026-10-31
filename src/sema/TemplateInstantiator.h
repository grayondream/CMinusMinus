// P1-03 / GEN-05: 模板实例化器——TypeVar → 实参 Type* 的重写 + 模板体 AST
// 深克隆。实例 decl 与手写同构声明完全等价，后续走既有 sema/codegen。
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "ast/Decl.h"
#include "ast/Expr.h"
#include "ast/Stmt.h"

class TemplateInstantiator {
public:
    TemplateInstantiator(const std::unordered_map<std::string, Type*>& typeArgs,
                         const std::unordered_map<std::string, long long>& valueArgs)
        : m_typeArgs(typeArgs), m_valueArgs(valueArgs) {}

    // 递归重写 Pointer/Array/Optional/Result/Slice/Typedef 内的 TypeVar 与
    // 数组长度占位；其余类型原样返回。TypeInstance 占位经 TemplateRegistry
    // 解析（嵌套实例化 / 递归检测 / 深度上限）。
    Type* rewrite(Type* t);

    // SG-01: evaluate a dependent integer expression (`N`, `N + 1`) against a
    // value-argument map.
    static bool evalIntExpr(const ExprAST* expr,
                            const std::unordered_map<std::string, long long>& values,
                            long long& out);

    // 深克隆声明（含函数体全部语句/表达式），克隆过程中完成类型重写。
    // 调用方负责在克隆体上改名（实例 mangled 名）。
    std::unique_ptr<DeclAST> cloneDecl(const DeclAST& decl);

    std::unique_ptr<ExprAST> cloneExpr(const ExprAST* expr);
    std::unique_ptr<StmtAST> cloneStmt(const StmtAST* stmt);

private:
    void copyLoc(ASTNode* dst, const ASTNode& src);
    std::unique_ptr<DeclAST> cloneDeclInternal(const DeclAST& decl);

    const std::unordered_map<std::string, Type*>& m_typeArgs;
    const std::unordered_map<std::string, long long>& m_valueArgs;
    // GEN-02: local declarations that shadow a non-type template parameter,
    // collected during cloning and attached to the cloned function decl so
    // sema can reject them (name-only value substitution would otherwise
    // silently capture the wrong `N`).
    std::vector<std::string> m_shadowedValueParams;
};
