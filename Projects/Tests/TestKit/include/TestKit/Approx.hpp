/// @file    Approx.hpp
/// @brief   数学型の近似比較マクロ。float を EXPECT_EQ で比べないための入口。
/// @author  Hasegawa Jin
/// @date    2026-09-02
///
/// 素の EXPECT_NEAR を成分ごとに並べると、失敗しても «どの成分が» «どれだけ» ずれたかが
/// 1 行ずつ分断されて読めない。ここでは述語フォーマッタにして、期待値・実測値・
/// 最大誤差を 1 つのメッセージにまとめる。
#pragma once

#include <TestKit/Tolerance.hpp>

#include <gtest/gtest.h>

namespace fbzz::math {
struct Vector2;
struct Vector3;
struct Vector4;
struct Quaternion;
struct Matrix3;
struct Matrix4;
} // namespace fbzz::math

namespace fbzz::testkit {

::testing::AssertionResult CompareVector2(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Vector2& actual,
                                          const math::Vector2& expected,
                                          float tolerance);

::testing::AssertionResult CompareVector3(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Vector3& actual,
                                          const math::Vector3& expected,
                                          float tolerance);

::testing::AssertionResult CompareVector4(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Vector4& actual,
                                          const math::Vector4& expected,
                                          float tolerance);

/// q と -q は同じ回転なので、符号を揃えてから成分を比べる。
/// 失敗メッセージには回転角の差 (度) も出す — 成分の差より «どれだけ回っているか» が読める。
::testing::AssertionResult CompareQuaternion(const char* actualExpr,
                                             const char* expectedExpr,
                                             const char* toleranceExpr,
                                             const math::Quaternion& actual,
                                             const math::Quaternion& expected,
                                             float tolerance);

::testing::AssertionResult CompareMatrix3(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Matrix3& actual,
                                          const math::Matrix3& expected,
                                          float tolerance);

::testing::AssertionResult CompareMatrix4(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Matrix4& actual,
                                          const math::Matrix4& expected,
                                          float tolerance);

/// 単位ベクトルであることの確認。正規化の結果を «方向が合っている» とは別に検査する。
::testing::AssertionResult CheckUnitLength(const char* actualExpr,
                                           const char* toleranceExpr,
                                           const math::Vector3& actual,
                                           float tolerance);

} // namespace fbzz::testkit

#define EXPECT_VEC2_NEAR(actual, expected, tolerance) \
    EXPECT_PRED_FORMAT3(::fbzz::testkit::CompareVector2, actual, expected, tolerance)
#define ASSERT_VEC2_NEAR(actual, expected, tolerance) \
    ASSERT_PRED_FORMAT3(::fbzz::testkit::CompareVector2, actual, expected, tolerance)

#define EXPECT_VEC3_NEAR(actual, expected, tolerance) \
    EXPECT_PRED_FORMAT3(::fbzz::testkit::CompareVector3, actual, expected, tolerance)
#define ASSERT_VEC3_NEAR(actual, expected, tolerance) \
    ASSERT_PRED_FORMAT3(::fbzz::testkit::CompareVector3, actual, expected, tolerance)

#define EXPECT_VEC4_NEAR(actual, expected, tolerance) \
    EXPECT_PRED_FORMAT3(::fbzz::testkit::CompareVector4, actual, expected, tolerance)
#define ASSERT_VEC4_NEAR(actual, expected, tolerance) \
    ASSERT_PRED_FORMAT3(::fbzz::testkit::CompareVector4, actual, expected, tolerance)

#define EXPECT_QUAT_NEAR(actual, expected, tolerance) \
    EXPECT_PRED_FORMAT3(::fbzz::testkit::CompareQuaternion, actual, expected, tolerance)
#define ASSERT_QUAT_NEAR(actual, expected, tolerance) \
    ASSERT_PRED_FORMAT3(::fbzz::testkit::CompareQuaternion, actual, expected, tolerance)

#define EXPECT_MAT3_NEAR(actual, expected, tolerance) \
    EXPECT_PRED_FORMAT3(::fbzz::testkit::CompareMatrix3, actual, expected, tolerance)
#define ASSERT_MAT3_NEAR(actual, expected, tolerance) \
    ASSERT_PRED_FORMAT3(::fbzz::testkit::CompareMatrix3, actual, expected, tolerance)

#define EXPECT_MAT4_NEAR(actual, expected, tolerance) \
    EXPECT_PRED_FORMAT3(::fbzz::testkit::CompareMatrix4, actual, expected, tolerance)
#define ASSERT_MAT4_NEAR(actual, expected, tolerance) \
    ASSERT_PRED_FORMAT3(::fbzz::testkit::CompareMatrix4, actual, expected, tolerance)

#define EXPECT_UNIT_LENGTH(actual, tolerance) \
    EXPECT_PRED_FORMAT2(::fbzz::testkit::CheckUnitLength, actual, tolerance)
#define ASSERT_UNIT_LENGTH(actual, tolerance) \
    ASSERT_PRED_FORMAT2(::fbzz::testkit::CheckUnitLength, actual, tolerance)
