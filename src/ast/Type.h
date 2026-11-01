#pragma once
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

#include "Annotation.h"

class ExprAST;

// PAR-04/SEM-04/DEC-01: member access levels. `class` members default to
// Private, `struct` members to Public (unrecorded members fall back to
// Public, which keeps the struct path unchanged).
enum class AccessLevel {
    Public,
    Private,
    Protected, // equivalent to Private until INH (inheritance) lands
};

enum class TypeKind {
    Void,
    Char,
    Bool,
    Pointer,
    Array,
    Struct,
    Class,
    Union,
    Enum,
    Function,
    Typedef,
    // 新增类型
    Int8,
    Int16,
    Int32,
    Int64,
    Int128,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    UInt128,
    ISize,
    USize,
    Float32,
    Float64,
    Float16,
    Float128,
    Slice,
    Optional,
    Result,
    // P1-06 (FMT-01/02): str = 不拥有的 UTF-8 字节视图 {ptr, len}；string =
    // 动态字符串 {ptr, len, capacity}（C 语义显式 new/destroy，DEC-20）。
    Str,
    String,
    // P1-03 / GEN: 模板类型参数占位与模板使用占位。TypeVar 仅合法存在于
    // 模板体 AST 内；TypeInstance 是 parse 期 `Name<args>` 的占位，sema 在
    // 使用点解析为具体实例类型后不再残留。
    TypeVar,
    TypeInstance,
    // Review 2.6: parser placeholder for `auto name = init;`; sema replaces
    // it with the initializer's type before codegen.
    Auto,
};

class Type {
public:
    ~Type() = default;
    Type(const TypeKind kind, Type* base = nullptr)
        : kind(kind), base(base) {}
public:
    TypeKind kind{};
    Type* base{};
    bool isVolatile{};
    bool isConst{};
    // CON-01: `atomic T` — integer scalar qualifier (cloned type, never set on
    // the shared singletons). Only direct load/store and the atomicrmw subset
    // are supported (see spec §2.6).
    bool isAtomic{};
    // PAR-06: `restrict` — pointer-postfix qualifier, syntactic only (no
    // alias analysis; semantics deferred to MEM-04).
    bool isRestrict{};
    // P1-04 / CT-03: 该类型仅在 compile_time.if 未选中分支被声明（parse 期
    // 占位毒化）。类型引用在 parse 期绑定指针，仅靠 TypeContext 注销无法
    // 阻断使用；sema 在变量声明处拒绝毒化类型（活分支的 sema 注册创建新
    // 对象，天然解毒）。
    bool ctDeadBranch{false};
    // P1-05 / ANN-05: [[deprecated]] 类型（sema 折叠；使用点 W3004）。
    bool deprecated{false};
    std::string deprecatedMsg;
    // CTX-01: source annotations retained on the type for compile_time
    // attribute queries. The folded flags above remain the semantic source of
    // truth for existing code; this vector preserves names/arguments.
    std::vector<Annotation> annotations;
};

class ArrayType : public Type {
public:
    Type* elementType;
    int size;
    // P1-03 / GEN-02: 非空时 size 是占位值（0），实际长度在实例化时由该
    // 命名的非类型模板参数给出（`T data[N]`）。具体化后副本须清空本字段。
    std::string sizeParam;

    ArrayType(Type* elem, int sz)
        : Type(TypeKind::Array), elementType(elem), size(sz) {}
};

// P1-07 / AGG-06: 位域布局元组——(存储单元 LLVM 字段索引, 单元内位偏移, 位宽,
// 存储单元总位宽)。unitBits 供 RMW 助手构造加载/掩码类型：
//   - 非 packed：基类型位宽（int32→32，bool→8，经 bitFieldStorageType）；
//   - packed：所在位流区域字段（[N x i8]）总位宽 N*8（如 a:3;:0;b:3 → 40）；
//   - union：对齐字段（alignType）位宽，全体位域成员统一。
// LayoutBuilder 物化时填充进 StructType/ClassType/UnionType::bitFields。
struct BitFieldLayout {
    uint32_t unitIndex;
    uint32_t bitOffset;
    uint32_t bitWidth;
    uint32_t unitBits;
};

class StructType : public Type {
public:
    std::string name;
    // INH-01: single public inheritance — base sub-object occupies field
    // slot 0 in the LLVM layout (mirrors ClassType::baseClass/base).
    std::string baseClass;
    Type* base{};
    // INH-01: completion tracking (mirrors ClassType::isComplete).
    bool isComplete{};
    // Parser-side source-order snapshot: a definition body was parsed at this
    // point. Unlike isComplete (sema order), a later definition must not
    // retroactively make an earlier by-value use complete.
    bool parsedDefinition{};
    // P1-03 / GEN: 模板体 parse 经普通声明路径注册的占位类型——裸模板名
    // （无实参）使用时由 sema 诊断。
    bool isTemplatePattern = false;
    // P1-05 / ANN-02/03: 布局注解折叠（sema 写入，LayoutBuilder 消费）。
    bool isPacked = false;
    bool reprC = false;
    uint64_t forcedAlign = 0;
    std::unordered_map<std::string, uint64_t> fieldAligns;
    std::unordered_map<std::string, bool> fieldDeprecated;
    std::unordered_map<std::string, std::string> fieldDeprecatedMsg;
    // LayoutBuilder 首次构造时插入的 padding 伪字段（name=""）已落盘。
    bool layoutMaterialized = false;
    std::vector<FieldInfo> fields;
    // P1-07 / PAR-10: 物化前原始字段序快照（LayoutBuilder 首次物化时填充；
    // 常量构造/位域查表按语义字段消费，物化伪字段不占语义位）。
    std::vector<FieldInfo> semanticFields;
    // P1-07 / AGG-06: 位域布局表（LayoutBuilder 物化时填充）——字段名 → 布局。
    std::unordered_map<std::string, BitFieldLayout> bitFields;
    // Review 2.6: captured-lambda closure metadata. A closure value is a
    // normal struct; these fields let a variable of that type be called.
    bool isClosure = false;
    std::string closureCallName;
    std::vector<Type*> closureParamTypes;
    Type* closureReturnType = nullptr;

    StructType(const std::string& n)
        : Type(TypeKind::Struct), name(n) {}

    Type* getFieldType(const std::string& fieldName) const {
        for (auto& f : fields) {
            if (f.name == fieldName) return f.type;
        }
        return nullptr;
    }

    void addField(const std::string& fieldName, Type* fieldType, int bitWidth = 0,
                  std::vector<Annotation> annotations = {}) {
        fields.push_back(FieldInfo{fieldName, fieldType, std::move(annotations), bitWidth});
    }
};

class UnionType : public Type {
public:
    std::string name;
    // P1-05 / ANN-02/03: 布局注解折叠（packed 对 union 无填充语义，仅 align 有效）。
    bool isPacked = false;
    bool reprC = false;
    uint64_t forcedAlign = 0;
    std::vector<FieldInfo> members;
    // P1-07 / AGG-06: 位域布局表（union 成员共享偏移 0，unitIndex 恒 0）。
    std::unordered_map<std::string, BitFieldLayout> bitFields;
    // Redef 轮: completion tracking (mirrors ClassType::isComplete) — a
    // forward declaration leaves the placeholder incomplete.
    bool isComplete{};
    bool parsedDefinition{};

    UnionType(const std::string& n)
        : Type(TypeKind::Union), name(n) {}

    void addMember(const std::string& memberName, Type* memberType, int bitWidth = 0,
                   std::vector<Annotation> annotations = {}) {
        members.push_back(FieldInfo{memberName, memberType, std::move(annotations), bitWidth});
    }
};

class EnumType : public Type {
public:
    std::string name;
    std::vector<std::pair<std::string, int>> values;
    // Explicit underlying type (`enum E : u8`); null means the default (int).
    Type* underlyingType = nullptr;
    // Redef 轮: completion tracking (mirrors ClassType::isComplete).
    bool isComplete{};

    EnumType(const std::string& n)
        : Type(TypeKind::Enum), name(n) {}

    void addValue(const std::string& valueName, int val) {
        values.push_back({valueName, val});
    }
};

class FunctionType : public Type {
public:
    Type* returnType;
    std::vector<Type*> paramTypes;
    bool isVarArg;

    FunctionType(Type* ret, std::vector<Type*> params, bool varArg = false)
        : Type(TypeKind::Function), returnType(ret), paramTypes(std::move(params)), isVarArg(varArg) {}
};

class TypedefType : public Type {
public:
    std::string name;
    Type* aliasedType;

    TypedefType(const std::string& n, Type* aliased)
        : Type(TypeKind::Typedef), name(n), aliasedType(aliased) {}
};

class SliceType : public Type {
public:
    Type* elementType;

    SliceType(Type* elem)
        : Type(TypeKind::Slice), elementType(elem) {}
};

class OptionalType : public Type {
public:
    Type* elementType;

    OptionalType(Type* elem)
        : Type(TypeKind::Optional), elementType(elem) {}
};

class ResultType : public Type {
public:
    Type* successType;
    Type* errorType;

    ResultType(Type* success, Type* error)
        : Type(TypeKind::Result), successType(success), errorType(error) {}
};

// P1-06 (FMT-01): str — 不拥有的 UTF-8 字节视图，布局 {ptr, len(字节)}。
class StrType : public Type {
public:
    StrType() : Type(TypeKind::Str) {}
};

// P1-06 (FMT-02): string — 动态字符串，布局 {ptr, len(字节), capacity}。
class StringType : public Type {
public:
    StringType() : Type(TypeKind::String) {}
};

// P1-03 / GEN-01: 模板类型参数占位（`template<typename T>` 中的 T）。
// 仅合法存在于模板体 AST；实例化（克隆替换）后不得残留。
class TypeVarType : public Type {
public:
    std::string name;

    explicit TypeVarType(const std::string& n)
        : Type(TypeKind::TypeVar), name(n) {}
};

// P1-03 / GEN-03: parse 期模板使用占位（`Box<i32>`、`Array<T, 8>`）。
// sema 在使用点触发实例化并把它解析为具体 ClassType；sema 结束后不存在。
class TypeInstanceType : public Type {
public:
    std::string templateName;
    std::vector<Type*> typeArgs;
    std::vector<long long> valueArgs;
    // SG-01: parallel symbolic value arguments, resolved during outer template
    // instantiation (e.g. `A<N>` / `A<N + 1>` inside a template body).
    std::vector<std::string> valueArgNames;
    std::vector<std::shared_ptr<ExprAST>> valueArgExprs;

    explicit TypeInstanceType(const std::string& n)
        : Type(TypeKind::TypeInstance), templateName(n) {}
};

class ClassType : public Type {
public:
    std::string name;
    std::vector<FieldInfo> fields;
    std::vector<std::pair<std::string, FunctionType*>> methods;
    std::string baseClass;

    // DEC-01: per-member access levels; absent -> Public (struct path).
    std::unordered_map<std::string, AccessLevel> memberAccess;

    // INH-01: false while only a forward declaration has been seen; sema sets
    // it when the definition is visited. Deriving from an incomplete class is
    // diagnosed.
    bool isComplete{};
    bool parsedDefinition{};

    // P1-03 / GEN: 模板体 parse 的占位注册（同 StructType::isTemplatePattern）。
    bool isTemplatePattern = false;

    // P1-05 / ANN-02/03: 布局注解折叠（sema 写入，LayoutBuilder 消费）。
    bool isPacked = false;
    bool reprC = false;
    uint64_t forcedAlign = 0;
    std::unordered_map<std::string, uint64_t> fieldAligns;
    std::unordered_map<std::string, bool> fieldDeprecated;
    std::unordered_map<std::string, std::string> fieldDeprecatedMsg;
    bool layoutMaterialized = false;
    // P1-07 / PAR-10: 物化前原始字段序快照（镜像 StructType）。
    std::vector<FieldInfo> semanticFields;
    // P1-07 / AGG-06: 位域布局表（镜像 StructType）。
    std::unordered_map<std::string, BitFieldLayout> bitFields;

    ClassType(const std::string& n)
        : Type(TypeKind::Class), name(n) {}

    void setMemberAccess(const std::string& memberName, AccessLevel level) {
        memberAccess[memberName] = level;
    }

    AccessLevel memberAccessLevel(const std::string& memberName) const {
        auto it = memberAccess.find(memberName);
        return it == memberAccess.end() ? AccessLevel::Public : it->second;
    }

    void addField(const std::string& fieldName, Type* fieldType, int bitWidth = 0,
                  std::vector<Annotation> annotations = {}) {
        fields.push_back(FieldInfo{fieldName, fieldType, std::move(annotations), bitWidth});
    }

    void addMethod(const std::string& methodName, FunctionType* methodType) {
        methods.push_back({methodName, methodType});
    }

    Type* getFieldType(const std::string& fieldName) const {
        for (auto& f : fields) {
            if (f.name == fieldName) return f.type;
        }
        return nullptr;
    }

    FunctionType* getMethod(const std::string& methodName) const {
        for (auto& [name, type] : methods) {
            if (name == methodName) return type;
        }
        return nullptr;
    }
};

class TypeContext{
public:
    static TypeContext& instance();
    ~TypeContext();

    Type* getChar();
    Type* getVoid();

    void addTypedef(const std::string& name, Type* type);
    Type* getTypedef(const std::string& name) const;

    // PAR-24: block-scoped aliases live in a lexical stack; unqualified
    // lookups see the innermost binding, qualified names bypass it.
    void pushLocalTypedefScope();
    void popLocalTypedefScope();
    Type* addLocalTypedef(const std::string& name, Type* type);
    Type* getLocalTypedef(const std::string& name) const;

    void addStruct(const std::string& name, StructType* type);
    StructType* getStruct(const std::string& name) const;

    void addUnion(const std::string& name, UnionType* type);
    UnionType* getUnion(const std::string& name) const;

    void addEnum(const std::string& name, EnumType* type);
    EnumType* getEnum(const std::string& name) const;

    void addClass(const std::string& name, ClassType* type);
    ClassType* getClass(const std::string& name) const;
    ClassType* getOrCreateClass(const std::string& name);

    // PAR-12: 匿名聚合类型的唯一合成名 `__anon_<N>`（进程内单调）。
    std::string nextAnonId();

    // P1-03 / GEN: 模板体 parse 会经普通声明路径注册类型名（占位）——模板
    // 注册时须撤销，保证裸模板名（无实参）在类型位置不可解析。
    void removeStruct(const std::string& name);
    void removeClass(const std::string& name);
    // P1-04 / CT-03: compile_time.if 死分支占位撤销（同上先例）。
    void removeUnion(const std::string& name);
    void removeEnum(const std::string& name);

    // 仅测试用：清除所有模板实例类型（名字含 '$'）。
    void removeInstanceTypes();

    // 新增类型获取方法
    Type* getBool();
    Type* getInt8();
    Type* getInt16();
    Type* getInt32();
    Type* getInt64();
    Type* getInt128();
    Type* getUInt8();
    Type* getUInt16();
    Type* getUInt32();
    Type* getUInt64();
    Type* getUInt128();
    Type* getISize();
    Type* getUSize();
    Type* getFloat32();
    Type* getFloat64();
    Type* getFloat16();
    Type* getFloat128();
    SliceType* getSliceType(Type* elementType);
    OptionalType* getOptionalType(Type* elementType);
    ResultType* getResultType(Type* successType, Type* errorType);
    // P1-06 (FMT-01/02): monomorphic singletons (m_types keyed by TypeKind).
    StrType* getStrType();
    StringType* getStringType();
    // CT-07: reflection result types — named StructTypes with string_view/offset/size.
    StructType* getFieldInfoType();
    StructType* getAttributeType();

    // P1-03 / GEN-01: 模板类型参数占位，同名同指针（单射）。
    TypeVarType* getTypeVar(const std::string& name);

private:
    std::unordered_map<TypeKind, Type*> m_types;
    std::unordered_map<std::string, Type*> m_typedefs;
    std::unordered_map<std::string, StructType*> m_structs;
    std::unordered_map<std::string, UnionType*> m_unions;
    std::unordered_map<std::string, EnumType*> m_enums;
    std::unordered_map<std::string, ClassType*> m_classes;
    std::unordered_map<std::string, TypeVarType*> m_typeVars;
    std::vector<std::unordered_map<std::string, Type*>> m_localTypedefs;
};
