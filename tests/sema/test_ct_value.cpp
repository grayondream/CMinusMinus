// CtValue: exact compile-time integer semantics shared by the constexpr
// interpreter and the compile_time query evaluator.
#include <gtest/gtest.h>

#include <cstdint>

#include "ast/CtValue.h"
#include "ast/Expr.h"

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
