// CtValue: exact compile-time integer semantics shared by the compile_time
// interpreter and the compile_time query evaluator.
#include <gtest/gtest.h>

#include <cstdint>

#include "ast/CtValue.h"
#include "ast/Expr.h"
#include "ast/Type.h"

TEST(CtValueTest, Uint32Wraps) {
    auto v = ctBinary(ctInt(0xFFFFFFFFll, 32, true), ctInt(1, 32, true),
                      static_cast<int>(BinaryOp::Add), nullptr);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->intVal, 0);
    EXPECT_TRUE(v->isUnsigned);
    EXPECT_EQ(v->bits, 32u);
}

TEST(CtValueTest, Uint64KeepsHighBit) {
    long long bits = static_cast<long long>(0xFFFFFFFFFFFFFFFFull);
    auto v = ctBinary(ctInt(bits, 64, true), ctInt(1, 64, true),
                      static_cast<int>(BinaryOp::Add), nullptr);
    ASSERT_TRUE(v);
    EXPECT_EQ(static_cast<uint64_t>(v->intVal), 0ull);
    EXPECT_TRUE(v->isUnsigned);
    EXPECT_EQ(v->bits, 64u);
}

TEST(CtValueTest, ShiftSignedness) {
    auto logical = ctBinary(ctInt(-1, 32, true), ctInt(1, 32, true),
                            static_cast<int>(BinaryOp::RShift), nullptr);
    ASSERT_TRUE(logical);
    EXPECT_EQ(static_cast<uint32_t>(logical->intVal), 0x7FFFFFFFu);

    auto arith = ctBinary(ctInt(-2, 32, false), ctInt(1, 32, false),
                          static_cast<int>(BinaryOp::RShift), nullptr);
    ASSERT_TRUE(arith);
    EXPECT_EQ(arith->intVal, -1);
}

TEST(CtValueTest, Int8PromotesToInt) {
    // Usual arithmetic conversions promote int8 operands to int before the
    // addition, so the result is 128 (int32), not an int8 wraparound.
    auto v = ctBinary(ctInt(127, 8, false), ctInt(1, 8, false),
                      static_cast<int>(BinaryOp::Add), nullptr);
    ASSERT_TRUE(v);
    EXPECT_EQ(v->intVal, 128);
    EXPECT_EQ(v->bits, 32u);
}

namespace {

ClassType* makeContainerType(const std::string& name,
                             const std::vector<std::pair<std::string, Type*>>& fields) {
    auto* ct = new ClassType(name);
    for (const auto& [field, type] : fields) ct->addField(field, type);
    return ct;
}

} // namespace

TEST(CtValueTest, ContainerInfoDetectsStdContainers) {
    auto& tc = TypeContext::instance();
    Type* i32 = tc.getInt32();
    Type* i64 = tc.getInt64();
    Type* u32 = tc.getUInt32();
    Type* usize = tc.getUSize();
    Type* str = tc.getStrType();
    auto* ptrI32 = new Type(TypeKind::Pointer, i32);
    auto* ptrI64 = new Type(TypeKind::Pointer, i64);
    auto* ptrU32 = new Type(TypeKind::Pointer, u32);
    auto* ptrStr = new Type(TypeKind::Pointer, str);
    auto* u8Ptr = new Type(TypeKind::Pointer, tc.getUInt8());

    auto* vec = makeContainerType("std_Vec$int32",
                                  {{"data", ptrI32}, {"len", usize}, {"cap", usize}});
    auto vi = ctContainerInfo(vec);
    ASSERT_TRUE(vi);
    EXPECT_EQ(vi->kind, CtContainer::Vec);
    EXPECT_EQ(vi->elementType, i32);

    auto* deque = makeContainerType("std_Deque$int64",
                                    {{"data", ptrI64}, {"cap", usize},
                                     {"head", usize}, {"len", usize}});
    auto di = ctContainerInfo(deque);
    ASSERT_TRUE(di);
    EXPECT_EQ(di->kind, CtContainer::Deque);
    EXPECT_EQ(di->elementType, i64);

    auto* map = makeContainerType("std_HashMap$uint32$int32",
                                  {{"keys", ptrU32}, {"values", ptrI32},
                                   {"states", u8Ptr}, {"cap", usize}, {"len", usize},
                                   {"tombstones", usize}, {"hash_fn", nullptr},
                                   {"eq_fn", nullptr}});
    auto mi = ctContainerInfo(map);
    ASSERT_TRUE(mi);
    EXPECT_EQ(mi->kind, CtContainer::HashMap);
    EXPECT_EQ(mi->keyType, u32);
    EXPECT_EQ(mi->valueType, i32);

    auto* set = makeContainerType("std_HashSet$str",
                                  {{"keys", ptrStr}, {"states", u8Ptr},
                                   {"cap", usize}, {"len", usize},
                                   {"tombstones", usize}, {"hash_fn", nullptr},
                                   {"eq_fn", nullptr}});
    auto si = ctContainerInfo(set);
    ASSERT_TRUE(si);
    EXPECT_EQ(si->kind, CtContainer::HashSet);
    EXPECT_EQ(si->keyType, str);
}

TEST(CtValueTest, DefaultValues) {
    auto& tc = TypeContext::instance();
    auto i8 = ctDefaultValue(tc.getUInt8());
    EXPECT_EQ(i8.type, CtValue::INT);
    EXPECT_EQ(i8.intVal, 0);
    EXPECT_EQ(i8.bits, 8u);
    EXPECT_TRUE(i8.isUnsigned);

    auto b = ctDefaultValue(tc.getBool());
    EXPECT_EQ(b.type, CtValue::INT);
    EXPECT_EQ(b.intVal, 0);
    EXPECT_TRUE(b.isBool);

    auto f = ctDefaultValue(tc.getFloat32());
    EXPECT_EQ(f.type, CtValue::DOUBLE);
    EXPECT_EQ(f.doubleVal, 0.0);

    auto s = ctDefaultValue(tc.getStrType());
    EXPECT_EQ(s.type, CtValue::STR);
    EXPECT_TRUE(s.strVal.empty());
}

TEST(CtValueTest, CloneDeepCopiesContainers) {
    auto c = std::make_shared<CtContainer>();
    c->kind = CtContainer::Vec;
    c->cap = 2;
    c->len = 2;
    c->items = {ctInt(1, 32), ctInt(2, 32)};

    CtValue original;
    original.type = CtValue::CONTAINER;
    original.container = c;

    CtValue copy = ctClone(original);
    ASSERT_TRUE(copy.container);
    EXPECT_EQ(copy.container->items[0].intVal, 1);
    copy.container->items[0] = ctInt(99, 32);
    EXPECT_EQ(c->items[0].intVal, 1);
    EXPECT_NE(copy.container.get(), c.get());
}
