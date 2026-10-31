// P1-03 / GEN-05: TemplateRegistry 实现。
#include "sema/TemplateRegistry.h"

#include <algorithm>

#include "ast/Mangle.h"
#include "sema/TemplateInstantiator.h"

TemplateRegistry& TemplateRegistry::instance() {
    static TemplateRegistry reg;
    return reg;
}

void TemplateRegistry::registerTemplate(TemplateDeclAST* decl) {
    std::string name;
    if (auto* fn = dynamic_cast<FunctionDeclAST*>(decl->decl.get())) {
        name = fn->name;
    } else if (auto* st = dynamic_cast<StructDeclAST*>(decl->decl.get())) {
        name = st->name;
    } else if (auto* ud = dynamic_cast<UsingDeclAST*>(decl->decl.get())) {
        name = ud->name;
    }
    m_templates[name] = decl;

    // 模板体 parse 走了普通声明路径，把类型名注册成了占位 StructType/
    // ClassType——撤销，保证裸模板名（无实参）在类型位置报错而非静默解析。
    TypeContext::instance().removeStruct(name);
    TypeContext::instance().removeClass(name);
}

TemplateDeclAST* TemplateRegistry::find(const std::string& name) const {
    auto it = m_templates.find(name);
    return it != m_templates.end() ? it->second : nullptr;
}

std::string TemplateRegistry::instanceName(const std::string& name,
                                           const std::vector<Type*>& typeArgs,
                                           const std::vector<long long>& valueArgs) {
    std::string s = name;
    for (auto* t : typeArgs)
        if (t) s += "$" + typeToMangled(t);
    for (auto v : valueArgs) s += "$" + std::to_string(v);
    return s;
}

bool TemplateRegistry::isInstantiating(const std::string& key) const {
    auto it = m_states.find(key);
    return it != m_states.end() && it->second == State::Instantiating;
}

DeclAST* TemplateRegistry::instanceDeclFor(const std::string& instanceName) const {
    auto it = m_done.find(instanceName);
    return it != m_done.end() ? it->second : nullptr;
}

DeclAST* TemplateRegistry::instantiateByKey(
    const std::string& key, const std::string& tplName, const std::string& displayName,
    const std::unordered_map<std::string, Type*>& typeArgMap,
    const std::unordered_map<std::string, long long>& valueArgMap, bool wantClass) {
    auto st = m_states.find(key);
    if (st != m_states.end() && st->second == State::Done) {
        return m_done[key];
    }
    if (st != m_states.end() && st->second == State::Instantiating) {
        // 文案用模板名（spec 钉死），键含实参拼写。
        m_errors.push_back("recursive instantiation of template '" + tplName + "'");
        return nullptr;
    }
    if (m_depth >= kMaxDepth) {
        m_errors.push_back("template instantiation depth limit exceeded (64)");
        return nullptr;
    }

    m_states[key] = State::Instantiating;
    m_depth++;

    auto cloned = TemplateInstantiator(typeArgMap, valueArgMap)
                      .cloneDecl(*m_templates[tplName]->decl);
    m_depth--;
    m_states[key] = State::Done;

    if (!cloned) {
        m_errors.push_back("internal: template instantiation failed for '" + key + "'");
        return nullptr;
    }

    // 实例 decl 改名为 mangled 拼写——后续 sema/codegen 将其当普通声明。
    if (wantClass) {
        static_cast<StructDeclAST*>(cloned.get())->name = displayName;
    } else {
        static_cast<FunctionDeclAST*>(cloned.get())->name = displayName;
    }

    DeclAST* raw = cloned.get();
    m_done[key] = raw;
    m_pending.push_back(std::move(cloned));
    return raw;
}

StructDeclAST* TemplateRegistry::instantiateClass(const std::string& name,
                                                  const std::vector<Type*>& typeArgs,
                                                  const std::vector<long long>& valueArgs) {
    auto* tpl = find(name);
    if (!tpl) return nullptr;
    if (!dynamic_cast<StructDeclAST*>(tpl->decl.get())) return nullptr;

    std::unordered_map<std::string, Type*> typeArgMap;
    std::unordered_map<std::string, long long> valueArgMap;
    size_t ti = 0, vi = 0;
    for (auto& p : tpl->params) {
        if (p.isType) {
            if (ti < typeArgs.size()) typeArgMap[p.name] = typeArgs[ti++];
        } else {
            if (vi < valueArgs.size()) valueArgMap[p.name] = valueArgs[vi++];
        }
    }

    std::string key = instanceName(name, typeArgs, valueArgs);
    return static_cast<StructDeclAST*>(instantiateByKey(key, name, key, typeArgMap,
                                                        valueArgMap, /*wantClass=*/true));
}

FunctionDeclAST* TemplateRegistry::instantiateFunction(
    const std::string& name, const std::vector<Type*>& typeArgs,
    const std::vector<long long>& valueArgs, const std::vector<Type*>& deducedParamTypes) {
    auto* tpl = find(name);
    if (!tpl) return nullptr;
    if (!dynamic_cast<FunctionDeclAST*>(tpl->decl.get())) return nullptr;

    std::unordered_map<std::string, Type*> typeArgMap;
    std::unordered_map<std::string, long long> valueArgMap;
    size_t ti = 0, vi = 0;
    for (auto& p : tpl->params) {
        if (p.isType) {
            if (ti < typeArgs.size()) typeArgMap[p.name] = typeArgs[ti++];
        } else {
            if (vi < valueArgs.size()) valueArgMap[p.name] = valueArgs[vi++];
        }
    }

    // 键 = 显示名（含显式/推导类型实参）+ 推导后参数类型拼写（计划钉死的
    // 三段式键）；decl 名不含推导参数段，符号唯一性由既有 mangleFunction
    // 追加参数类型保证。
    std::string key = instanceName(name, typeArgs, valueArgs);
    std::string display = key;
    for (auto* pt : deducedParamTypes)
        if (pt) key += "$" + typeToMangled(pt);

    return static_cast<FunctionDeclAST*>(instantiateByKey(key, name, display, typeArgMap,
                                                          valueArgMap, /*wantClass=*/false));
}

Type* TemplateRegistry::resolveInstance(
    TypeInstanceType* use, const std::unordered_map<std::string, Type*>& outerTypeArgs,
    const std::unordered_map<std::string, long long>& outerValueArgs) {
    // 深度预算与 instantiateClass 共享：嵌套使用点链（Box<Box<...>>）在
    // resolveInstance 递归中逐层展开，须同计入上限。
    if (m_depth >= kMaxDepth) {
        m_errors.push_back("template instantiation depth limit exceeded (64)");
        return nullptr;
    }
    m_depth++;

    // 先对实参做外层参数重写（`Box<T>` 内嵌 `Vec<T>` 等）。
    std::vector<Type*> args;
    for (auto* ta : use->typeArgs) {
        TemplateInstantiator inst(outerTypeArgs, outerValueArgs);
        args.push_back(inst.rewrite(ta));
    }
    m_depth--;

    // SG-01: symbolic value arguments (`A<N>`) resolve against the enclosing
    // instantiation's value bindings before the instance key is formed.
    std::vector<long long> values = use->valueArgs;
    const size_t symbolicCount = std::max(use->valueArgNames.size(),
                                           use->valueArgExprs.size());
    values.resize(std::max(values.size(), symbolicCount), 0);
    for (size_t i = 0; i < use->valueArgExprs.size(); ++i) {
        if (!use->valueArgExprs[i]) continue;
        long long evaluated = 0;
        if (!TemplateInstantiator::evalIntExpr(use->valueArgExprs[i].get(),
                                               outerValueArgs, evaluated)) {
            m_errors.push_back("cannot evaluate dependent template value argument");
            return nullptr;
        }
        values[i] = evaluated;
    }
    for (size_t i = 0; i < use->valueArgNames.size(); ++i) {
        const std::string& name = use->valueArgNames[i];
        if (name.empty()) continue;
        auto it = outerValueArgs.find(name);
        if (it == outerValueArgs.end()) {
            m_errors.push_back("unknown template value argument '" + name + "'");
            return nullptr;
        }
        values[i] = it->second;
    }

    std::string name = instanceName(use->templateName, args, values);

    // 按实例名注册 StructType 占位（INH-01 reuse-registration 模式：sema
    // visit 实例 decl 时补全字段并置 isComplete）。
    auto* placeholder = TypeContext::instance().getStruct(name);
    if (!placeholder) {
        placeholder = new StructType(name);
        TypeContext::instance().addStruct(name, placeholder);
    }

    DeclAST* inst = instantiateClass(use->templateName, args, values);
    if (!inst) return nullptr;
    return placeholder;
}
