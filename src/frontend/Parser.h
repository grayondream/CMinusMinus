#pragma once

#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include "frontend/Token.h"
#include "ast/Decl.h"
#include "sema/Diagnostic.h"

class Parser{
public:
    Parser(const std::vector<Token>& tokens) : m_tokens(std::move(tokens)) {
        m_constScopes.emplace_back();
    }

    std::unique_ptr<TranslationUnitAST> parse();
    const std::vector<Diagnostic>& getErrors() const;

private:
    void error(const std::string& msg, const Token& token);
    void error(DiagnosticCode code, const std::string& msg, const Token& token);
    void errorUnexpected(const std::string& expected);
    void errorUnexpectedEOF(const std::string& expected);
    bool expect(TokenType type, const std::string& msg);
    std::string tokenTypeName(TokenType type) const;
    bool eof() const;

    std::optional<Token> match(TokenType type);

    std::optional<Token> peek() const;

    std::optional<Token> advance();

    bool check(TokenType type) const;

    bool isTypeStart() const;

    // INF-03 / P0-06: stamp an AST node with the source position of the token it
    // originates from, so semantic diagnostics carry file:line:col. A null
    // `node`/`tok` is ignored.
    void applyLocation(ASTNode* node, const Token* tok);

    Type* parseType();
    Type* parseTypeId();
    // SG-01: one template argument may be a type or an integer const-expr.
    // `argName` is set for a direct non-type template parameter reference.
    bool parseTemplateArgument(Type*& argType, long long& argValue,
                               std::string& argName,
                               std::shared_ptr<ExprAST>& argExpr);
    int m_templateArgDepth = 0;
    // A speculative `name<...>(` probe rolls back to a comparison; diagnostics
    // from that probe are suppressed (SG-01).
    bool m_templateArgSpeculative = false;
    std::unique_ptr<UsingDeclAST> parseUsingAliasDecl(bool member);

    // PAR-24: legacy alias forms (typedef/type) and block/member-scope using.
    bool isRemovedTypeAliasStart() const;
    bool isLocalUsingStart() const;
    void rejectRemovedTypeAlias();
    void skipPastSemicolon();

    Type* parseBaseType();

    // P1-03 / GEN-01 / PAR-21: `template<...>` 声明（函数/struct/class/别名）。
    std::unique_ptr<DeclAST> parseTemplateDecl();

    // P1-05 / ANN-01: 注解序列解析（光标处无 `[[` 返回空）与判定。
    bool isAnnotationStart() const;
    std::vector<Annotation> parseAnnotations();
    // P1-05 / ANN-01: `[[...]] module ...;` 的模块注解中转（ModuleDeclAST
    // 在 parse() 尾部统一创建）。
    std::vector<Annotation> m_pendingModuleAnnotations;

    // P1-04 / CT-01: `compile_time.if` / `compile_time.static_assert` 顶层声明。
    // `compile_time` 非关键字；仅成员名 ∈ {if, static_assert} 的根链在此特判。
    bool isCompileTimeDeclStart() const;
    // `compile_time` as a declaration modifier (`compile_time T x;`,
    // `compile_time T f()`) — distinct from the `compile_time.if` namespace.
    bool isCompileTimeModifierStart() const;
    bool consumeCompileTimeSpecifier(bool& isCompileTime);
    std::unique_ptr<DeclAST> parseCompileTimeIf();
    std::unique_ptr<StmtAST> parseCompileTimeIfStmt();
    std::unique_ptr<StmtAST> parseCompileTimeRepeatStmt();
    std::unique_ptr<DeclAST> parseCompileTimeAssert();

    // 模板参数作用域栈：模板体 parse 期间，参数名解析为 TypeVarType /
    // 非类型参数（数组长度处）。GEN-01。
    struct TemplateScope {
        std::unordered_set<std::string> typeParams;
        std::unordered_set<std::string> valueParams;
    };
    std::vector<TemplateScope> m_templateScopes;

    // Namespace-qualified type names (PAR-22): consume `A::B::name`, returning
    // the joined name ("" when no identifier is present).
    std::string parseQualifiedTypeName();

    // Symbol-table key for a type declared at the current namespace scope.
    std::string qualifyTypeDeclName(const std::string& name) const;

    // Resolve a (possibly qualified) type name, trying the enclosing namespace
    // prefix before the global name. nullptr when not a known named type.
    Type* lookupNamedType(const std::string& name) const;

    // Turn `T name` into `T name[N]` when an array suffix follows a member
    // declarator (used for struct/class/union members).
    Type* parseMemberArraySuffix(Type* base, bool* outFlexibleArray = nullptr);

    // Parse one bracketed array dimension as a compile-time integer constant.
    // `hasSize=false` means the brackets were empty (only valid for the first
    // dimension / flexible array member). The caller still consumes `]`.
    bool parseArrayDimension(int& size, bool& hasSize);

    // Parse-time constant values for enum constants and const/constexpr
    // integer variables, used by array dimensions and bit-field widths.
    bool lookupConstValue(const std::string& name, long long& value) const;
    void registerConstValue(const std::string& name, long long value);
    std::vector<std::unordered_map<std::string, long long>> m_constScopes;

    // P1-07 / PAR-10: parse a bit-field width after the caller consumed ':':
    //   width = parseExpr(2); evalIntConstExpr must succeed (constant expr).
    // Returns false (with a diagnostic) when the width is not a constant
    // expression; on success `width` holds the evaluated value.
    bool parseBitFieldWidth(int& width);

    std::unique_ptr<DeclAST> parseDeclaration();

    // The raw declaration parser; `parseDeclaration` wraps it to stamp source
    // locations on the result (INF-03 / P0-06).
    std::unique_ptr<DeclAST> parseDeclarationImpl();

    std::unique_ptr<DeclAST> parseDeclarationAsType();

    // Continue a declaration whose base type was already parsed. Handles the
    // attached forms after a record definition: `s`, `*p`, `f()`, `*f()`,
    // `(*fp)(...)`, `(*p)[N]`.
    std::unique_ptr<DeclAST> parseDeclarationFromBaseType(Type* base,
                                                          bool isCompileTime = false);

    std::unique_ptr<FunctionDeclAST> parseFunctionDecl(Type* returnType, const std::string& name,
                                                       bool isCompileTime = false,
                                                       bool allowComma = false);

    std::unique_ptr<DeclAST> parseVariableDecl(Type* type, const std::string& name, bool isCompileTime = false);

    std::unique_ptr<DeclAST> parseVariableDeclList(Type* type, const std::string& firstName, bool isCompileTime = false);

    // Parse the parameter list of a function-pointer declarator, after the
    // closing ')' of "(*name)"; returns the pointed-to FunctionType.
    Type* parseFunctionPointerType(Type* returnType);

    // Parse a parenthesized pointer declarator tail after its base type:
    //   (* [name] [suffix]) [suffix]
    // Covers pointer-to-function, pointer-to-array, array-of-function-pointer,
    // and function-returning-function-pointer. `outIsFunction` reports whether
    // the declared entity itself is a function (first declarator op), and
    // `outFunctionParams` receives that function's named parameters.
    Type* parseFunctionPointerDeclarator(Type* returnType, std::string& outName,
                                         bool requireName,
                                         bool* outIsFunction = nullptr,
                                         std::vector<std::unique_ptr<ParamDeclAST>>* outFunctionParams = nullptr);

    // Parse an abstract function-type suffix: `( paramTypes )`.
    Type* parseAbstractFunctionSuffix(Type* returnType);

    std::unique_ptr<ParamDeclAST> parseParamDecl();

    // PAR-05 / AGG: one aggregate member declarator after its base type.
    // Handles `name`, `*name`, `(*name)(params)`, `(*name)[N]`, array
    // suffixes and bit-fields; the caller owns the comma/semicolon loop.
    struct MemberFieldDecl {
        std::string name;
        Type* type = nullptr;
        int bitWidth = 0;
        bool flexibleArray = false;
    };
    bool parseMemberFieldDecl(Type* baseType, MemberFieldDecl& out);

    std::unique_ptr<StructDeclAST> parseStructDecl();
    std::unique_ptr<StructDeclAST> parseClassDecl();

    // PAR-05: struct/class share one record grammar; `isClass` selects the
    // keyword and the default member access (class=private, struct=public).
    std::unique_ptr<StructDeclAST> parseRecordDecl(bool isClass);

    // PAR-05: look ahead over the current record body to decide whether a
    // `struct` needs a ClassType placeholder before its members are parsed
    // (methods/operators/access sections). Needed for self-referential
    // signatures such as `S* self()` and `operator+(S)`.
    bool recordBodyLooksClassLike() const;

    std::unique_ptr<UnionDeclAST> parseUnionDecl();

    std::unique_ptr<EnumDeclAST> parseEnumDecl();


    std::unique_ptr<DeclAST> parseNamespaceDecl();

    // The raw namespace parser; `parseNamespaceDecl` wraps it to stamp source
    // locations on the result (INF-03 / P0-06).
    std::unique_ptr<DeclAST> parseNamespaceDeclImpl();

    std::unique_ptr<ReturnStmtAST> parseReturnStmt();

    std::unique_ptr<CompoundStmtAST> parseCompoundStmt();

    std::unique_ptr<StmtAST> parseStmt();

    std::unique_ptr<StmtAST> parseIfStmt();

    std::unique_ptr<StmtAST> parseWhileStmt();

    std::unique_ptr<StmtAST> parseDoWhileStmt();

    std::unique_ptr<StmtAST> parseForStmt();

    std::unique_ptr<StmtAST> parseSwitchStmt();

    std::unique_ptr<StmtAST> parseBreakStmt();

    std::unique_ptr<StmtAST> parseContinueStmt();

    // If the current token is '#', report that preprocessor directives are
    // unsupported (NG-02) and skip the rest of the line. Returns true if it did.
    bool rejectPreprocessorDirective();

    std::unique_ptr<StmtAST> parseExprStmt();

    std::unique_ptr<ExprAST> parseExpr(int minPrec = 0);

    std::unique_ptr<ExprAST> parsePrimary();

    // Raw parsers wrapped by `parsePrimary`/`parseUnary` to stamp source
    // locations on every expression node (INF-03 / P0-06).
    std::unique_ptr<ExprAST> parsePrimaryImpl();
    std::unique_ptr<ExprAST> parseUnaryImpl();

    // P1-08 / PAR-16: inline asm —— `asm` 已被 parseUnaryImpl 消费后调用。
    std::unique_ptr<ExprAST> parseAsmExpr();

    // PAR-11: lambda 表达式 `[captures](params) -> Ret { body }`。
    std::unique_ptr<ExprAST> parseLambdaExpr();

    std::unique_ptr<ExprAST> parseUnary();

    std::unique_ptr<ExprAST> parsePostfix(std::unique_ptr<ExprAST> lhs);

    int getPrecedence(TokenType op) const;

    bool isRightAssociative(TokenType op) const;

    BinaryOp tokenTypeToBinaryOp(TokenType type) const;

    AssignOp tokenTypeToAssignOp(TokenType type) const;

private:
    std::vector<Token> m_tokens;
    size_t m_currentTokenPos{0};
    std::vector<Diagnostic> m_errors;
    // Mangled prefix (trailing '_') active while parsing a namespace body, e.g.
    // "A_" inside `namespace A { ... }`.
    std::string m_typeNamespacePrefix;
    // AGG-11: register a nested type declaration into TypeContext, mirroring
    // what parseDeclaration does for the same kind at top level.
    void registerNestedDeclType(DeclAST* decl);
    // AGG-11: token index just past the '}' matching the '{' at lbracePos
    // (lbracePos itself when braces are unbalanced).
    size_t balancedBraceEnd(size_t lbracePos) const;
};