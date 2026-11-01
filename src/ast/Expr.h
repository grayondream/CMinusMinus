#pragma once

#include <string>
#include <memory>
#include <vector>
#include "Type.h"
#include "PrintFormat.h"
#include "support/LiteralKind.h"

// PAR-11: lambda 成员引用 Decl.h/Stmt.h 中的类型——前向声明避免循环包含
// （Decl.h → Stmt.h → Expr.h）。unique_ptr 到不完整类型在头文件合法，
// 析构在使用处（各 .cpp 全量包含）实例化。
class ParamDeclAST;
class CompoundStmtAST;

// FUN-09: a folded compile-time default argument value, carried on the call
// node from sema to codegen (avoids a sema-layer dependency from ast/).
struct DefaultArgValue {
    bool isInt = false;
    long long intVal = 0;
    bool isDouble = false;
    double doubleVal = 0;
    bool isChar = false;
    char charVal = 0;
};

#include "llvm/IR/Value.h"

class CodegenContext;

enum class BinaryOp {
    Invalid,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Eq,
    NotEq,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
    BitAnd,
    BitOr,
    BitXor,
    LShift,
    RShift,
};

enum class UnaryOp {
    Plus,
    Minus,
    Not,
    BitNot,
    Deref,
    AddressOf,
    PreInc,
    PreDec,
    Sizeof,
};

enum class AssignOp {
    Assign,
    AddAssign,
    SubAssign,
    MulAssign,
    DivAssign,
    ModAssign,
    BitAndAssign,
    BitOrAssign,
    BitXorAssign,
    LShiftAssign,
    RShiftAssign,
};

enum class MemberAccessKind {
    Dot,
    Arrow,
};

class ASTNode {
public:
    std::string sourceFile;
    int sourceLine{0};
    int sourceColumn{0};

    virtual ~ASTNode() = default;

    void setLocation(const std::string& file, int line, int col = 0) {
        sourceFile = file;
        sourceLine = line;
        sourceColumn = col;
    }
};

class ExprAST : public ASTNode {
public:
    Type* type{nullptr};
    bool isLValue{false};

    // P1-04 / CT-12: compile_time 求值结果（sema 写入；codegen 直接出
    // llvm::Constant，零运行时指令）。STR 不落这些字段——类型系统无 str，
    // STR 节点 type=nullptr 永不达 codegen。
    bool ctHandled = false;
    std::optional<long long> ctInt;
    std::optional<double> ctFloat;

    virtual llvm::Value* codegen(CodegenContext& ctx) = 0;
    // P1-07 / CG-09: bit-field member access predicate (only MemberAccessExprAST
    // overrides). codegen uses it to route reads through readLValue.
    virtual bool isBitFieldAccess() const { return false; }
};

class NumberExprAST : public ExprAST {
public:
    long long value;
    // P1-07 / LEX-17: 128 位整型字面量的高 64 位（≤64 位字面量恒 0）。value 为
    // 低 64 位（有符号位模式）；组合出 unsigned __int128 = (hi<<64)|lo。
    int64_t valueHi = 0;
    // LEX-15: suffix/default kind of the literal, used by sema/codegen to pick
    // the fixed-width type instead of a fixed `int`/`i32`.
    LiteralKind literalKind{LiteralKind::Int};
    explicit NumberExprAST(int val) : value(val) {}
    NumberExprAST(long long val, LiteralKind kind) : value(val), literalKind(kind) {}
    NumberExprAST(long long lo, long long hi, LiteralKind kind)
        : value(lo), valueHi(hi), literalKind(kind) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// Found issue（I4 review）: `true`/`false` 是布尔字面量（spec keywords.md），
// 此前被解析成 NumberExprAST(1)/(0)（int32）→ 重载解析匹配不上 bool 形参。
// 独立节点携带 bool 类型（i1），算术/赋值仍按整数族参与（bool 是 1 位整数）。
class BoolLiteralExprAST : public ExprAST {
public:
    bool value;
    explicit BoolLiteralExprAST(bool val) : value(val) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class FloatExprAST : public ExprAST {
public:
    double value;
    LiteralKind literalKind{LiteralKind::Float64};
    explicit FloatExprAST(double val) : value(val) {}
    FloatExprAST(double val, LiteralKind kind) : value(val), literalKind(kind) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class CharExprAST : public ExprAST {
public:
    char value;
    explicit CharExprAST(char val) : value(val) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class StringExprAST : public ExprAST {
public:
    std::string value;
    // TYP-05: literal prefix — None = plain `"..."` (string_view); U8/U16/U32
    // map to char8[]/char16[]/char32[].
    StringLiteralPrefix prefix{StringLiteralPrefix::None};
    explicit StringExprAST(const std::string& val) : value(val) {}
    explicit StringExprAST(const std::string& val, StringLiteralPrefix p)
        : value(val), prefix(p) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class VariableExprAST : public ExprAST {
public:
    std::string name;
    // Set by semantic analysis when the name refers to a function used as a
    // value: codegen then yields the function instead of a variable address.
    bool isFunctionRef = false;
    std::string resolvedFunctionName;
    // Set by semantic analysis when the name is an enumerator: codegen then
    // yields the integer constant instead of a variable address.
    bool isEnumConstant = false;
    int enumValue = 0;
    explicit VariableExprAST(const std::string& n) : name(n) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class BinaryExprAST : public ExprAST {
public:
    BinaryOp op;
    std::unique_ptr<ExprAST> left;
    std::unique_ptr<ExprAST> right;
    std::string mangledCallee;  // Set by sema for operator overloading
    // SEM-17: member-form operator call (`a + b` resolving to `a.operator+(b)`).
    bool isMethodOperator = false;
    std::vector<Type*> resolvedParamTypes;


    BinaryExprAST(BinaryOp oper, std::unique_ptr<ExprAST> l, std::unique_ptr<ExprAST> r)
        : op(oper), left(std::move(l)), right(std::move(r)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class UnaryExprAST : public ExprAST {
public:
    UnaryOp op;
    std::unique_ptr<ExprAST> operand;
    // SEM-17: unary operator overloading — set by sema when an aggregate
    // operand resolves to an `operator-`/`operator~`/`operator!`/`operator++`
    // (prefix) free function. codegen then emits a call instead of the
    // built-in instruction.
    std::string mangledCallee;
    bool isMethodOperator = false;
    std::vector<Type*> resolvedParamTypes;


    UnaryExprAST(UnaryOp oper, std::unique_ptr<ExprAST> expr)
        : op(oper), operand(std::move(expr)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// Internal LLVM-atomic lowering selected by sema for the `__smc_atomic_*`
// library intrinsics. These are not part of the user-facing surface.
enum class AtomicBuiltin {
    None, Load, Store, Exchange,
    CompareExchangeWeak, CompareExchangeStrong,
    FetchAdd, FetchSub, FetchAnd, FetchOr, FetchXor,
    ThreadFence, SignalFence
};

class CallExprAST : public ExprAST {
public:
    std::string callee;
    // SEM-17: `obj(args)` where `obj` is an aggregate object — sema moves the
    // object reference here and resolves `operator()`. When set, codegen calls
    // mangledCallee with the object as the first argument.
    std::unique_ptr<ExprAST> calleeExpr;
    std::string mangledCallee;
    std::vector<std::unique_ptr<ExprAST>> args;
    // P1-03 / GEN-03: 显式模板实参 `max<int32>(3, 4)`——类型与整型值按
    // 出现顺序分组；空且 hasTemplateArgs 时走纯推导。
    bool hasTemplateArgs = false;
    std::vector<Type*> explicitTemplateArgs;
    std::vector<long long> explicitTemplateValues;
    // SG-01: parallel to explicitTemplateValues; non-empty entry means the
    // argument is a symbolic template value parameter (`g<N>()`), while an
    // expression entry covers dependent arithmetic such as `g<N + 1>()`.
    std::vector<std::string> explicitTemplateValueNames;
    std::vector<std::shared_ptr<ExprAST>> explicitTemplateValueExprs;
    // Parameter types of the overload chosen by semantic analysis. Empty when
    // unresolved (e.g. codegen is run without semantic analysis).
    std::vector<Type*> resolvedParamTypes;
    // FUN-09: folded values for trailing default arguments (position i fills
    // resolvedParamTypes[args.size()+i]).
    std::vector<DefaultArgValue> defaultArgValues;
    // Set by semantic analysis for calls through a function-pointer variable.
    bool isIndirect = false;
    // Review 2.6: call through a captured-lambda closure value.
    bool isClosureCall = false;
    // SEM-17: member `operator()` resolved via a class method; codegen passes
    // the object address as the implicit `this` argument.
    bool memberOperator = false;
    // SEM-17: `(expr)(args)` where `expr` yields a function pointer — the
    // callee is an arbitrary expression (`arr[0](1)`, `get()(1)`).
    bool isExprCallee = false;
    // Internal atomic builtin selected by sema (see AtomicBuiltin).
    AtomicBuiltin atomicBuiltin = AtomicBuiltin::None;
    int atomicOrder = 5;
    int atomicFailureOrder = 5;
    // Set by semantic analysis when this is the builtin `print`/`println`.
    bool isPrint = false;
    bool printNewline = false;
    std::string printCFormat;                 // final printf format string
    std::vector<PrintArgKind> printArgKinds;  // one per `{}` slot
    // P1-09 (FMT-08): parsed spec per `{}` slot, parallel to printArgKinds.
    std::vector<PrintSpec> printSpecs;
    // P1-09 (FMT-07): builtin `format(fmt, args...)`. With a literal format
    // string, formatLiteral/formatSpecs carry the compile-time parse; a
    // dynamic str format leaves formatLiteral empty (runtime scan).
    bool isFormat = false;
    std::string formatLiteral;
    std::vector<PrintSpec> formatSpecs;
    // Set by semantic analysis when this is the builtin terminator `assert` or
    // `panic` (STD-01 / STD-27 / DEC-21). Lowered to a stderr message + abort(),
    // with the call site's file:line embedded at compile time.
    bool isAssert = false;
    bool isPanic = false;
    // P1-09 (STD-10): builtin `split(s, sep) -> []str` (zero-copy views over
    // malloc'd array; split_destroy frees it). Sema rejects a literal empty
    // separator at compile time.
    bool isSplit = false;
    bool isSplitDestroy = false;
    // P1-06 (FMT-04): builtin `str_from_c(char*) -> str` (semantics: strlen
    // over the NUL-terminated buffer; UTF-8 validation at literal points).
    bool isStrFromC = false;

    CallExprAST(const std::string& name, std::vector<std::unique_ptr<ExprAST>> arguments)
        : callee(name), args(std::move(arguments)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class AssignmentExprAST : public ExprAST {
public:
    AssignOp op;
    std::unique_ptr<ExprAST> lhs;
    std::unique_ptr<ExprAST> rhs;

    AssignmentExprAST(AssignOp oper, std::unique_ptr<ExprAST> left, std::unique_ptr<ExprAST> right)
        : op(oper), lhs(std::move(left)), rhs(std::move(right)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class TernaryExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> cond;
    std::unique_ptr<ExprAST> then;
    std::unique_ptr<ExprAST> elseExpr;

    TernaryExprAST(std::unique_ptr<ExprAST> condition,
                   std::unique_ptr<ExprAST> thenExpr,
                   std::unique_ptr<ExprAST> elseExpr)
        : cond(std::move(condition)), then(std::move(thenExpr)), elseExpr(std::move(elseExpr)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// How a cast was written. `CStyle` is the legacy `(T)x`; `Static`/`Reinterpret`
// are the explicit operators from LEX-11 / PAR-18 (DEC-18).
enum class CastKind {
    CStyle,
    Static,
    Reinterpret,
};

class CastExprAST : public ExprAST {
public:
    Type* castType;
    std::unique_ptr<ExprAST> expr;
    CastKind castKind;

    CastExprAST(Type* targetType, std::unique_ptr<ExprAST> e,
                CastKind kind = CastKind::CStyle)
        : castType(targetType), expr(std::move(e)), castKind(kind) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// P1-08 / PAR-16: 内联汇编操作数 —— 约束串 + 表达式（GCC extended asm 形态）。
struct AsmOperand {
    std::string constraint;
    // SG-19: optional GCC/C++ operand name (`[v] "=r"(x)`).
    std::string name;
    std::unique_ptr<ExprAST> expr;
};

// `asm [volatile] ("tpl" : outputs : inputs : clobbers)` —— 表达式节点，经
// ExprStmtAST 作语句出现；sema 置 type=void、isLValue=false（D3）。
class AsmExprAST : public ExprAST {
public:
    std::string asmTemplate;
    bool isVolatile;
    std::vector<AsmOperand> outputs;
    std::vector<AsmOperand> inputs;
    std::vector<std::string> clobbers;
    // sema 按序填充，codegen 用其构造 InlineAsm 签名。
    std::vector<Type*> outputTypes;
    std::vector<Type*> inputTypes;

    AsmExprAST(std::string tpl, bool vol, std::vector<AsmOperand> outs,
               std::vector<AsmOperand> ins, std::vector<std::string> clobs)
        : asmTemplate(std::move(tpl)), isVolatile(vol), outputs(std::move(outs)),
          inputs(std::move(ins)), clobbers(std::move(clobs)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class CommaExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> left;
    std::unique_ptr<ExprAST> right;

    CommaExprAST(std::unique_ptr<ExprAST> l, std::unique_ptr<ExprAST> r)
        : left(std::move(l)), right(std::move(r)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class PostfixIncDecExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> operand;
    bool isIncrement;
    // SEM-17: postfix operator overloading — set by sema when an aggregate
    // operand resolves to `operator++(V, int32)` / `operator--(V, int32)`.
    std::string mangledCallee;

    PostfixIncDecExprAST(std::unique_ptr<ExprAST> expr, bool inc)
        : operand(std::move(expr)), isIncrement(inc) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class ArrayAccessExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> array;
    std::unique_ptr<ExprAST> index;
    // SEM-17: `v[i]` on an aggregate object resolves to `operator[]`; sema
    // sets the mangled callee and codegen emits a call instead of a GEP.
    std::string mangledCallee;
    // Member operator[] needs the object address as the implicit `this`.
    bool memberOperator = false;

    ArrayAccessExprAST(std::unique_ptr<ExprAST> arr, std::unique_ptr<ExprAST> idx)
        : array(std::move(arr)), index(std::move(idx)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// Review 2.2: zero-copy range access `s[a:b]`. Either bound may be omitted;
// strings yield a string_view, arrays/slices yield a T[] view. Bounds are
// checked at runtime by codegen.
class RangeExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> object;
    std::unique_ptr<ExprAST> begin;
    std::unique_ptr<ExprAST> end;

    RangeExprAST(std::unique_ptr<ExprAST> obj,
                 std::unique_ptr<ExprAST> b,
                 std::unique_ptr<ExprAST> e)
        : object(std::move(obj)), begin(std::move(b)), end(std::move(e)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class MemberAccessExprAST : public ExprAST {
public:
    MemberAccessKind accessKind;
    std::unique_ptr<ExprAST> object;
    std::string memberName;
    // P1-07 / SEM-12: bit-field access metadata — sema sets isBitField/bitWidth
    // when the resolved field is a bit-field (isLValue stays true; codegen
    // returns the storage-unit address). codegen fills bitOffset from the
    // defining type's bitFields map.
    bool isBitField = false;
    uint32_t bitWidth = 0;
    uint64_t bitOffset = 0;
    // P1-07 / AGG-06: 存储单元总位宽（emitBitFieldGEP 从布局表填入）——
    // 供 loadBitFieldValue/storeBitFieldValue 构造加载/掩码类型（packed
    // 区域单元可为非基类型位宽，如 i40）。
    uint32_t unitBits = 0;
    bool isBitFieldAccess() const override { return isBitField; }


    MemberAccessExprAST(MemberAccessKind kind, std::unique_ptr<ExprAST> obj, const std::string& member)
        : accessKind(kind), object(std::move(obj)), memberName(member) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// P1-06 (FMT-01/02): builtin methods dispatched by sema, lowered by codegen.
enum class BuiltinMethod {
    None,
    // str views
    StrLen,
    StrCharCount,
    StrCharAt,
    StrCharLenAt,
    // P1-09 (STD-10): find/rfind/sub primitives
    StrFind,
    StrRFind,
    StrSub,
    StrData,
    StrCharCountProp,
    // Review 2.2: method alias for array/slice `.len`.
    SequenceLen,
    // string dynamic
    StringNew,
    StringDestroy,
    StringAppend,
    StringPush,
    StringLen,
    StringCapacity,
    StringData,
    StringCharCount,
    // Review 2.4: Optional/Result unpacking methods.
    OptionalIsSome,
    OptionalIsNone,
    OptionalUnwrap,
    OptionalValueOr,
    ResultIsOk,
    ResultIsErr,
    ResultUnwrap,
    ResultValueOr,
};

class MethodCallExprAST : public ExprAST {
public:
    std::unique_ptr<ExprAST> object;
    std::string methodName;
    std::vector<std::unique_ptr<ExprAST>> args;

    // Declared parameter types (incl. this at index 0) filled by sema so
    // codegen mangles against the definition site. Mirror of CallExprAST.
    std::vector<Type*> resolvedParamTypes;
    // Namespace-qualified method definition name chosen by sema. The
    // definition side applies the namespace prefix (`std_init`), while the
    // call-site spelling is bare (`init`); codegen must reuse this name.
    std::string mangledCallee;

    // FUN-09: folded values for trailing default arguments (position i fills
    // resolvedParamTypes[args.size()+1+i] — index 0 is `this`).
    std::vector<DefaultArgValue> defaultArgValues;

    // P1-06: set by sema when this is a builtin str/string method; codegen
    // dispatches on it before the class-method path.
    BuiltinMethod builtinMethod = BuiltinMethod::None;

    // PAR-05: `s.f(args)` where `f` is a function-pointer aggregate field.
    // Sema moves the object reference into `fieldCallee` and records the
    // pointee function signature in `resolvedParamTypes` (no implicit this).
    bool isFunctionPointerCall = false;
    std::unique_ptr<MemberAccessExprAST> fieldCallee;

    // CT-07: compile_time reflection call (name_of/fields_of/attributes_of).
    // Sema records data in ReflectionRegistry and sets the kind + target type;
    // codegen materializes the global array/slice and returns it.
    enum class ReflectionKind { None, NameOf, FieldsOf, AttributesOf };
    ReflectionKind reflectKind = ReflectionKind::None;
    Type* reflectType = nullptr;

    MethodCallExprAST(std::unique_ptr<ExprAST> obj, const std::string& method,
                      std::vector<std::unique_ptr<ExprAST>> arguments)
        : object(std::move(obj)), methodName(method), args(std::move(arguments)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

class SizeofExprAST : public ExprAST {
public:
    Type* sizeofType;
    std::unique_ptr<ExprAST> expr;
    // PAR-14: `alignof(T)` — codegen returns the ABI alignment, not the size.
    bool isAlignOf = false;

    SizeofExprAST(Type* type, std::unique_ptr<ExprAST> e = nullptr)
        : sizeofType(type), expr(std::move(e)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// PAR-14: `offsetof(T, member)` — layout query returning usize.
struct OffsetDesignator {
    bool isIndex = false;
    std::string field;
    int64_t index = 0;
    // SG-03: original constant-expression subscript; sema folds it into
    // `index`, so codegen keeps consuming the resolved value.
    std::unique_ptr<ExprAST> indexExpr;
};

class OffsetOfExprAST : public ExprAST {
public:
    // NOTE: named `aggregateType` (NOT `type`) — `type` would shadow the
    // inherited ExprAST::type (result type) and sema's `node.type = usize`
    // would clobber the aggregate operand (C3 review fix).
    Type* aggregateType;
    std::string field;
    std::vector<OffsetDesignator> path;
    OffsetOfExprAST(Type* t, std::string f) : aggregateType(t), field(std::move(f)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// PAR-13: 指定初始化器设计符。Field = `.name`；Index = `[expr]`（parser 存
// indexExpr，sema 常量折叠写回 index）。designators 与 initializers 平行。
struct Designator {
    enum class Kind { Field, Index } kind{Kind::Field};
    std::string field;             // Kind::Field
    int64_t index = 0;             // Kind::Index，sema 折叠后
    std::unique_ptr<ExprAST> indexExpr;  // Kind::Index，parser 原始表达式
};

class InitializerListExprAST : public ExprAST {
public:
    std::vector<std::unique_ptr<ExprAST>> initializers;
    // PAR-13: 与 initializers 平行的设计符链；空 vector = 位置式。
    std::vector<std::vector<Designator>> designators;
    // PAR-13 (Task 2 sema 填充)：每个元素展开后的显式语义目标链（非空），
    // codegen 直接消费，不再推导续位规则。
    std::vector<std::vector<Designator>> resolvedTargets;

    InitializerListExprAST(std::vector<std::unique_ptr<ExprAST>> initList)
        : initializers(std::move(initList)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// PAR-08: C99 compound literal `(type-name){ init-list }`. Block scope = stack
// object (automatic storage); file scope = static constant. Direct escape
// (& / return / store-to-global of array|slice forms) is rejected by sema.
class CompoundLiteralExprAST : public ExprAST {
public:
    Type* literalType = nullptr;           // the target type (sema-validated)
    std::unique_ptr<InitializerListExprAST> initList;

    CompoundLiteralExprAST(Type* type, std::unique_ptr<InitializerListExprAST> inits)
        : literalType(type), initList(std::move(inits)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// PAR-11/CG-11/DEC-10: lambda 表达式 `[captures](params) -> Ret { body }`。
// 无捕获 → 函数指针（零开销）；有捕获 → 匿名闭包 struct + 生成调用函数。
class LambdaExprAST : public ExprAST {
public:
    struct Capture {
        std::string name;
        bool byRef = false;  // true = `&name`；false = 按值
    };
    std::vector<Capture> captures;
    bool captureAll = false;  // `[=]` 全部按值
    std::vector<std::unique_ptr<ParamDeclAST>> params;
    Type* returnType{nullptr};  // 显式或 sema 推断
    bool hasExplicitReturnType = false;
    std::unique_ptr<CompoundStmtAST> body;
    // sema 填充：闭包 struct 名（有捕获）或空（无捕获 → 函数指针）。
    std::string closureName;
    Type* closureType{nullptr};
    // PAR-11: codegen 生成的调用函数符号名（供 LambdaCallExprAST 调用）。
    std::string lambdaFnName;

    LambdaExprAST() = default;
    // PAR-11: params/body 持有不完整类型（前向声明）——析构须在包含
    // Decl.h/Stmt.h 的翻译单元定义（Expr.cpp）。
    ~LambdaExprAST() override;
    llvm::Value* codegen(CodegenContext& ctx) override;
};

// PAR-11: 立即调用 `[captures](params){...}(args)` —— 后缀 `(` 直接作用于
// lambda 表达式。持 lambda 与实参，codegen 构造闭包/函数指针后调用。
class LambdaCallExprAST : public ExprAST {
public:
    std::unique_ptr<LambdaExprAST> lambda;
    std::vector<std::unique_ptr<ExprAST>> args;

    LambdaCallExprAST(std::unique_ptr<LambdaExprAST> lam,
                      std::vector<std::unique_ptr<ExprAST>> arguments)
        : lambda(std::move(lam)), args(std::move(arguments)) {}
    llvm::Value* codegen(CodegenContext& ctx) override;
};
