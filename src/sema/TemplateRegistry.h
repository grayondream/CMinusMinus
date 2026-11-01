// P1-03 / GEN-05: 模板注册表——模板定义存储、实例化键去重、实例化状态机
// （NotInstantiated → Instantiating → Done）、递归检测与深度上限。
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/Decl.h"
#include "ast/Type.h"

class TemplateRegistry {
public:
    static TemplateRegistry& instance();

    // 注册模板定义（parse 完成后）。非所有权——模板定义由翻译单元持有。
    // 同名重复注册覆盖（单测重建用）。`qualifiedName` 由 sema 在命名空间
    // 上下文传入（`NsTpl_id`）；省略时使用声明自身的名字。
    void registerTemplate(TemplateDeclAST* decl,
                          const std::string& qualifiedName = {});
    TemplateDeclAST* find(const std::string& name) const;

    // 类模板实例化：克隆模板体、按实参重写、改名 mangled 拼写
    // （`Box$i32` / `Array$i32$8`），实例 decl 进入 pendingInstances 待 sema。
    // 命中缓存直接返回同一 decl 指针（去重）。
    StructDeclAST* instantiateClass(const std::string& name,
                                    const std::vector<Type*>& typeArgs,
                                    const std::vector<long long>& valueArgs);

    // 函数模板实例化：同上，typeArgs 为显式实参与推导结果并集。
    FunctionDeclAST* instantiateFunction(const std::string& name,
                                         const std::vector<Type*>& typeArgs,
                                         const std::vector<long long>& valueArgs,
                                         const std::vector<Type*>& deducedParamTypes);

    // TypeInstance 占位的解析入口（TemplateInstantiator::rewrite 调用）：
    // 命中 Instantiating 同名同参 = 递归实例化，报错；否则触发实例化并返回
    // 按实例名注册的 StructType 占位（sema visit 实例 decl 时补全）。
    Type* resolveInstance(TypeInstanceType* use,
                          const std::unordered_map<std::string, Type*>& outerTypeArgs,
                          const std::unordered_map<std::string, long long>& outerValueArgs);

    // 实例化状态（递归检测用）。key = 实例 mangled 名。
    bool isInstantiating(const std::string& key) const;

    // 已完成克隆的实例 decl（按实例名）。别名展开后补 visit 用。
    DeclAST* instanceDeclFor(const std::string& instanceName) const;

    // 克隆完成、待 sema 的实例 decl 队列。调用方 pop 后 visit，最终转交
    // codegen（追加到翻译单元）。
    std::deque<std::unique_ptr<DeclAST>>& pendingInstances() { return m_pending; }

    // 诊断（文案钉死见 spec）：`recursive instantiation of template '<名>'`、
    // `template instantiation depth limit exceeded (64)`。
    const std::vector<std::string>& getErrors() const { return m_errors; }
    void clearErrors() { m_errors.clear(); }

    // 仅测试用：清空实例缓存/状态/待检队列（单测进程内多 TU 隔离）。
    void resetForTesting() {
        m_states.clear();
        m_done.clear();
        m_pending.clear();
        m_errors.clear();
        m_depth = 0;
        TypeContext::instance().removeInstanceTypes();
    }

    // 命名空间限定名 -> TypeContext/Registry 使用的扁平键：
    // `std::Box` / `std.Box` -> `std_Box`（与 Parser 的 m_typeNamespacePrefix
    // 和 SemanticAnalyzer::mangleNamespaceName 保持一致）。
    static std::string canonicalName(const std::string& name);

    // 实例命名：`名 + 每个类型实参的 typeToMangled 拼写 + 非类型实参值`。
    // `name` 先经 canonicalName 归一，保证 `std::Box<T>` 与命名空间内的
    // `Box<T>` 命中同一实例。
    static std::string instanceName(const std::string& name,
                                    const std::vector<Type*>& typeArgs,
                                    const std::vector<long long>& valueArgs);

private:
    enum class State { NotInstantiated, Instantiating, Done };
    static constexpr int kMaxDepth = 64;

    TemplateDeclAST* bodyAsStructOrAlias(TemplateDeclAST* tpl) const { return tpl; }

    DeclAST* instantiateByKey(const std::string& key, const std::string& tplName,
                              const std::string& displayName,
                              const std::unordered_map<std::string, Type*>& typeArgMap,
                              const std::unordered_map<std::string, long long>& valueArgMap,
                              bool wantClass);

    std::unordered_map<std::string, TemplateDeclAST*> m_templates;
    std::map<std::string, State> m_states;
    std::unordered_map<std::string, DeclAST*> m_done;
    std::deque<std::unique_ptr<DeclAST>> m_pending;
    std::vector<std::string> m_errors;
    int m_depth = 0;
};
