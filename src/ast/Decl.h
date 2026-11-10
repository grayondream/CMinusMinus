#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <memory>
#include "Stmt.h"
#include "Annotation.h"
#include "CtValue.h"

// PAR-08: aggregate initializer emitters shared with compound-literal codegen
// (Expr.cpp). Definitions live in Decl.cpp (see the forward decls there).
class CodegenContext;
class InitializerListExprAST;
namespace llvm {
class Value;
class Constant;
}
void emitAggregateInitializer(CodegenContext& ctx, llvm::Value* dest,
                              Type* type, InitializerListExprAST* init);
// PAR-08: slice value backed by a fresh stack anonymous array (shared by slice
// variable declarations and slice compound literals).
llvm::Value* emitSliceLiteral(CodegenContext& ctx, Type* sliceType,
                              InitializerListExprAST* init);
llvm::Constant* buildAggregateConstant(CodegenContext& ctx, Type* type,
                                       InitializerListExprAST* init);

struct FoldedValue {
    enum Type { INT, DOUBLE, CHAR } type;
    union { int intVal; double doubleVal; char charVal; };
};

class DeclAST : public ASTNode {
public:
    // Module visibility (P0-04 / MOD-05/06). `moduleName` is the owning module
    // (empty for legacy/anonymous units whose declarations are always visible).
    // A declaration of a participating module is visible outside it only when
    // marked `export`/`public` (`isExported`). See docs/spec/modules.md §4.
    bool isExported = false;
    std::string moduleName;

    // P1-05 / ANN-01: 声明头注解（`[[...]]`，可叠加）。
    std::vector<Annotation> annotations;

    virtual llvm::Value* codegen(CodegenContext& ctx) = 0;
};

class VarDeclAST : public DeclAST {
public:
    std::string name;
    Type* type;
    std::unique_ptr<ExprAST> initExpr;
    bool isCompileTime = false;
    std::optional<FoldedValue> foldedValue;
    // Constexpr container value; codegen materializes it as immutable
    // constant arrays + a constant container struct.
    std::shared_ptr<CtContainer> foldedContainer;
    // P1-05 / ANN-03: [[align(N)]] 折叠值（sema 写入，codegen setAlignment）。
    uint64_t declAlign = 0;
    // CON-04: `thread_local` storage class — emitted as an LLVM thread-local
    // global (per-thread copy) for file/namespace scope.
    bool isThreadLocal = false;
    // Parser-side source-order snapshot for MOD-03: the declared aggregate had
    // no definition at this point (a later definition must not silence it).
    bool typeIncompleteAtParse = false;
    // C static local: function-scoped declaration with static storage duration.
    bool isStaticLocal = false;
    // C block-scope `extern`: refers to an existing/global symbol; no local
    // storage is allocated for it.
    bool isExternLocal = false;
    // C++-style out-of-class static data definition:
    // `int32 S::x = 1;` — sema binds it to the in-class static member so the
    // definition does not emit a second global.
    bool isOutOfClassStatic = false;
    std::string staticClass;
    std::string staticMember;

    VarDeclAST(const std::string& n, Type* t, std::unique_ptr<ExprAST> init = nullptr, bool constexpr_ = false)
        : name(n), type(t), initExpr(std::move(init)), isCompileTime(constexpr_) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class ParamDeclAST : public ASTNode {
public:
    std::string name;
    Type* type;
    // P1-05 / ANN-01: 参数注解（[[nonnull]] 等）。
    std::vector<Annotation> annotations;
    // FUN-09: default parameter `T name = const-expr` (trailing params only).
    bool hasDefault = false;
    std::unique_ptr<ExprAST> defaultValue;

    ParamDeclAST(const std::string& n, Type* t)
        : name(n), type(t) {}
};

class FunctionDeclAST : public DeclAST {
public:
    std::string name;
    Type* returnType;
    std::vector<std::unique_ptr<ParamDeclAST>> params;
    std::unique_ptr<CompoundStmtAST> body;
    bool isCompileTime = false;
    bool isVarArg = false;
    // AGG-10: class-body `static` method — no `this` insertion; sema rewrites
    // the name to the desugared `Class_method` global symbol.
    bool isStatic = false;
    // PAR-03: out-of-class method definition `RetType Class::method(...) {...}`.
    // Non-empty = the qualifying class name; sema binds the body to the method
    // declared in that class (inserting `this` like an in-class definition).
    std::string className;
    // GEN-02: local declarations that shadow a non-type template parameter
    // (filled by template instantiation; sema rejects them).
    std::vector<std::string> shadowedValueParams;
    // C linkage: `extern` declaration — symbol name stays the plain C name
    // (unmangled, no namespace prefix), and the call site resolves through
    // the bare C name.
    bool isExtern = false;
    // CON-04: `thread_local` storage class is invalid on a function — set by
    // the parser so sema can diagnose SemInvalidThreadLocalOnFunction.
    bool isThreadLocal = false;
    // FUN-09: number of trailing parameters carrying a default value. Filled by
    // sema at declaration; overload resolution reads it via Symbol::decl.
    int defaultArgCount = 0;

    FunctionDeclAST(const std::string& n, Type* ret,
                    std::vector<std::unique_ptr<ParamDeclAST>>& parameters,
                    std::unique_ptr<CompoundStmtAST>& b, bool constexpr_ = false,
                    bool varArg = false)
        : name(n), returnType(ret), params(std::move(parameters)), body(std::move(b)),
          isCompileTime(constexpr_), isVarArg(varArg) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
    // P1-03 / GEN-05: 仅创建签名（无体）——TU codegen 预扫。
    llvm::Value* codegenPrototype(CodegenContext& ctx);
};

class DeclStmtAST : public StmtAST {
public:
    std::unique_ptr<DeclAST> decl;

    explicit DeclStmtAST(std::unique_ptr<DeclAST> d) : decl(std::move(d)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class TranslationUnitAST : public ASTNode {
public:
    std::vector<std::unique_ptr<DeclAST>> declarations;
    // Module/file names named by `import` at the top level. The driver resolves
    // and loads them, splicing their declarations in front of this unit's.
    std::vector<std::string> imports;
    // The module declared by `module NAME;` at the head of this unit, or empty
    // for a legacy/anonymous unit (MOD-04). Dotted, e.g. "std.core".
    std::string moduleName;

    explicit TranslationUnitAST(std::vector<std::unique_ptr<DeclAST>> decls)
        : declarations(std::move(decls)) {}
    llvm::Value* codegen(CodegenContext& ctx);
};

class ArrayDeclAST : public DeclAST {
public:
    std::string name;
    Type* elementType;
    int size;
    std::unique_ptr<ExprAST> initExpr;

    ArrayDeclAST(const std::string& n, Type* elemType, int sz, std::unique_ptr<ExprAST> init = nullptr)
        : name(n), elementType(elemType), size(sz), initExpr(std::move(init)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// A single declaration statement that declares several variables, e.g.
// `int a = 1, b = 2;`. Each element is a VarDeclAST or ArrayDeclAST.
class MultiVarDeclAST : public DeclAST {
public:
    std::vector<std::unique_ptr<DeclAST>> decls;

    explicit MultiVarDeclAST(std::vector<std::unique_ptr<DeclAST>> d)
        : decls(std::move(d)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class StructDeclAST : public DeclAST {
public:
    std::string name;
    std::vector<FieldInfo> fields;
    std::vector<std::unique_ptr<FunctionDeclAST>> methods;
    // AGG-10: static data members — parser-built VarDeclASTs. Never part of
    // `fields` (no object layout); sema rewrites the name to the desugared
    // `Class_member` global symbol and codegen emits them as globals.
    std::vector<std::unique_ptr<VarDeclAST>> staticMembers;
    // AGG-10: class name before qualifyTypeDeclName's namespace prefixing —
    // the desugared static-member symbol must combine the BARE class name
    // with sema's own namespace prefix (scopedName), matching what the fully
    // qualified access spelling flattens to.
    std::string bareName;
    std::string baseClass;

    // PAR-04/DEC-01: explicit access levels recorded by the parser. Both
    // struct and class record every member; absent entries mean the record
    // keyword's default (struct: Public, class: Private).
    std::unordered_map<std::string, AccessLevel> memberAccess;
    // PAR-05: true when the source body contains at least one access section.
    // A struct with access sections needs ClassType semantics even if it has no
    // methods; a pure data struct keeps StructType.
    bool hasAccessSections = false;
    // INH-01: true when this node came from a forward declaration (`class D;`)
    // — sema uses it to keep ClassType::isComplete accurate.
    bool isForwardDecl = false;
    // AGG-11: nested type declarations (enum/struct/class/union), in source
    // order. Registered under flat keys (`Outer_Inner`) at parse time.
    std::vector<std::unique_ptr<DeclAST>> nestedTypes;
    bool isClassDecl = false;
    // PAR-12: 内联变量声明 `class { ... } c;` / `struct S { ... } s;` ——
    // 类型定义与变量在同一声明里。变量挂在聚合 AST 上（sema/codegen 先处理
    // 类型再处理变量，字段/方法/访问段随类型注册，变量随后可见）。
    std::unique_ptr<DeclAST> attachedVars;

    StructDeclAST(const std::string& n, std::vector<FieldInfo> flds)
        : name(n), fields(std::move(flds)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
    void emitStaticMembers(CodegenContext& ctx);
};

// PAR-05: one predicate for "this record needs class semantics". A `struct`
// with methods or explicit access sections shares ClassType with `class`, while
// a pure data struct stays StructType. Use this everywhere instead of repeating
// the three-way condition (parser registration, sema routing, dead-branch
// cleanup) so the identity decision cannot drift.
inline bool isClassLikeRecord(const StructDeclAST& decl) {
    return decl.isClassDecl || !decl.methods.empty() || decl.hasAccessSections;
}

class UnionDeclAST : public DeclAST {
public:
    std::string name;
    std::vector<FieldInfo> members;
    // AGG-11: nested type declarations, in source order.
    std::vector<std::unique_ptr<DeclAST>> nestedTypes;
    // AGG-11: union name before qualifyTypeDeclName's namespace prefixing
    // (mirrors StructDeclAST::bareName).
    std::string bareName;
    // Redef 轮: true when from a forward declaration (`union U;`) — sema uses
    // it to keep UnionType::isComplete accurate (mirrors StructDeclAST).
    bool isForwardDecl = false;

    UnionDeclAST(const std::string& n, std::vector<FieldInfo> mems)
        : name(n), members(std::move(mems)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class EnumDeclAST : public DeclAST {
public:
    std::string name;
    std::vector<std::pair<std::string, int>> values;
    // Optional explicit underlying type (`enum E : u8`); null means default int.
    Type* underlyingType;
    // AGG-11: enum name before qualifyTypeDeclName's namespace prefixing
    // (mirrors StructDeclAST::bareName).
    std::string bareName;
    // Redef 轮: true when from a forward declaration (`enum E;`) — sema uses
    // it to keep EnumType::isComplete accurate (mirrors StructDeclAST).
    bool isForwardDecl = false;

    EnumDeclAST(const std::string& n, std::vector<std::pair<std::string, int>> vals,
                Type* underlying = nullptr)
        : name(n), values(std::move(vals)), underlyingType(underlying) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class ForwardDeclAST : public DeclAST {
public:
    std::string name;

    explicit ForwardDeclAST(const std::string& n) : name(n) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// P1-03 / GEN-01 / PAR-21: 模板声明——`template<...>` 修饰函数/struct/class/
// using 别名。定义处只 parse 不 sema；实例化由 sema 侧 TemplateRegistry 驱动。
class TemplateDeclAST : public DeclAST {
public:
    struct Param {
        std::string name;
        bool isType = true;
        // isType==false 时为该非类型参数的类型节点（如 usize），否则 nullptr。
        Type* nonTypeType = nullptr;
    };

    std::vector<Param> params;
    std::unique_ptr<DeclAST> decl; // FunctionDeclAST / StructDeclAST / UsingDeclAST
    bool isAlias = false;

    llvm::Value* codegen(CodegenContext& ctx) override;
};

// 新增AST节点
class UsingDeclAST : public DeclAST {
public:
    std::string name;
    Type* aliasedType;
    // PAR-24 scoping: original spelling for local/member aliases, whether this
    // is a block-scoped alias, and the parse-time wrapper (deprecation folding
    // must reach the type node held by already-parsed declarators).
    std::string bareName;
    bool isBlockScope = false;
    Type* aliasWrapper = nullptr;

    UsingDeclAST(const std::string& n, Type* aliased)
        : name(n), aliasedType(aliased) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class DeferStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> callExpr;

    explicit DeferStmtAST(std::unique_ptr<ExprAST> call)
        : callExpr(std::move(call)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// P1-04 / CT-03: function-body `compile_time.if (cond) { stmts } [else { stmts }]`.
// Top-level declarations use CompileTimeIfDeclAST; this statement form selects
// at sema time and emits only the live branch.
class CompileTimeIfStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> cond;
    std::unique_ptr<CompoundStmtAST> thenStmt;
    std::unique_ptr<CompoundStmtAST> elseStmt;
    bool ctResolved = false;
    bool selectedThen = true;
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// CTX-01 / E2: compile-time repeat — the body is emitted `iterations` times at
// codegen time; no runtime loop or counter is generated.
class CompileTimeRepeatStmtAST : public StmtAST {
public:
    std::unique_ptr<ExprAST> count;
    std::unique_ptr<CompoundStmtAST> body;
    bool ctResolved = false;
    int iterations = 0;
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class ModuleDeclAST : public DeclAST {
public:
    std::string name;
    std::vector<std::string> imports;
    std::vector<std::string> exports;

    ModuleDeclAST(const std::string& n, std::vector<std::string> imp, std::vector<std::string> exp)
        : name(n), imports(std::move(imp)), exports(std::move(exp)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// namespace Name { ... } (TODO PAR-22 / MOD-12). Members are code-generated as
// part of the enclosing translation unit; the semantic analyzer qualifies their
// names so they do not collide with the global namespace.
class NamespaceDeclAST : public DeclAST {
public:
    // Dotted/`::`-joined namespace name, e.g. "geometry.ops" or "geometry::ops".
    std::string name;
    std::vector<std::unique_ptr<DeclAST>> declarations;

    NamespaceDeclAST(const std::string& n, std::vector<std::unique_ptr<DeclAST>> decls)
        : name(n), declarations(std::move(decls)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// P1-04 / CT-03: 顶层 `compile_time.if (cond) { decls... } [else { decls... }]`。
// 两个分支语法均 parse（spec compile_time.md §3）；选择在 sema 期完成。
class CompileTimeIfDeclAST : public DeclAST {
public:
    std::unique_ptr<ExprAST> cond;
    std::vector<std::unique_ptr<DeclAST>> thenDecls;
    std::vector<std::unique_ptr<DeclAST>> elseDecls;
    // sema 求值条件后写入；codegen 只出选中分支（SEM-07 死代码消除）。
    bool ctResolved = false;
    bool selectedThen = true;
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// P1-04 / CT-02: 顶层 `compile_time.static_assert(...);` 的包装节点（call 为
// MethodCallExprAST 形态）。函数体内走普通表达式路径，无需包装。
class CompileTimeAssertDeclAST : public DeclAST {
public:
    std::unique_ptr<ExprAST> call;
    llvm::Value* codegen(CodegenContext& ctx) override;
};
