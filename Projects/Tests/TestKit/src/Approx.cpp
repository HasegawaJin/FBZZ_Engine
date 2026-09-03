/// @file    Approx.cpp
/// @brief   数学型の近似比較述語の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Approx.hpp>

#include <TestKit/Print.hpp>

#include <Math/Matrix3.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

namespace fbzz::testkit {

namespace {

struct ComponentDiff {
    float       worst = 0.0f;
    std::string name;
};

ComponentDiff WorstOf(std::initializer_list<std::pair<const char*, float>> diffs)
{
    ComponentDiff result;
    for (const auto& [name, diff] : diffs) {
        const float magnitude = std::fabs(diff);
        if (magnitude > result.worst) {
            result.worst = magnitude;
            result.name  = name;
        }
    }
    return result;
}

template <int N>
ComponentDiff WorstOfMatrix(const float (&actual)[N][N], const float (&expected)[N][N])
{
    ComponentDiff result;
    for (int row = 0; row < N; ++row) {
        for (int col = 0; col < N; ++col) {
            const float magnitude = std::fabs(actual[row][col] - expected[row][col]);
            if (magnitude > result.worst) {
                result.worst = magnitude;
                result.name  = "m[" + std::to_string(row) + "][" + std::to_string(col) + "]";
            }
        }
    }
    return result;
}

template <typename T>
std::string Describe(const T& value)
{
    return ::testing::PrintToString(value);
}

::testing::AssertionResult Failed(const char* actualExpr,
                                  const char* expectedExpr,
                                  const char* toleranceExpr,
                                  const std::string& actualText,
                                  const std::string& expectedText,
                                  const ComponentDiff& diff,
                                  float tolerance)
{
    return ::testing::AssertionFailure()
           << "\n  実測 " << actualExpr << "\n    = " << actualText
           << "\n  期待 " << expectedExpr << "\n    = " << expectedText
           << "\n  最大のずれ: " << diff.name << " で " << diff.worst
           << " (許容 " << toleranceExpr << " = " << tolerance << ")";
}

} // namespace

::testing::AssertionResult CompareVector2(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Vector2& actual,
                                          const math::Vector2& expected,
                                          float tolerance)
{
    const ComponentDiff diff = WorstOf({{"x", actual.x - expected.x},
                                        {"y", actual.y - expected.y}});
    if (diff.worst <= tolerance) return ::testing::AssertionSuccess();
    return Failed(actualExpr, expectedExpr, toleranceExpr,
                  Describe(actual), Describe(expected), diff, tolerance);
}

::testing::AssertionResult CompareVector3(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Vector3& actual,
                                          const math::Vector3& expected,
                                          float tolerance)
{
    const ComponentDiff diff = WorstOf({{"x", actual.x - expected.x},
                                        {"y", actual.y - expected.y},
                                        {"z", actual.z - expected.z}});
    if (diff.worst <= tolerance) return ::testing::AssertionSuccess();
    return Failed(actualExpr, expectedExpr, toleranceExpr,
                  Describe(actual), Describe(expected), diff, tolerance);
}

::testing::AssertionResult CompareVector4(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Vector4& actual,
                                          const math::Vector4& expected,
                                          float tolerance)
{
    const ComponentDiff diff = WorstOf({{"x", actual.x - expected.x},
                                        {"y", actual.y - expected.y},
                                        {"z", actual.z - expected.z},
                                        {"w", actual.w - expected.w}});
    if (diff.worst <= tolerance) return ::testing::AssertionSuccess();
    return Failed(actualExpr, expectedExpr, toleranceExpr,
                  Describe(actual), Describe(expected), diff, tolerance);
}

::testing::AssertionResult CompareQuaternion(const char* actualExpr,
                                             const char* expectedExpr,
                                             const char* toleranceExpr,
                                             const math::Quaternion& actual,
                                             const math::Quaternion& expected,
                                             float tolerance)
{
    // 符号を揃える。q と -q は同じ回転なので、成分を直に引くと «正しいのに落ちる»。
    const float sign = (math::Quaternion::Dot(actual, expected) < 0.0f) ? -1.0f : 1.0f;
    const math::Quaternion aligned{expected.x * sign, expected.y * sign,
                                   expected.z * sign, expected.w * sign};

    const ComponentDiff diff = WorstOf({{"x", actual.x - aligned.x},
                                        {"y", actual.y - aligned.y},
                                        {"z", actual.z - aligned.z},
                                        {"w", actual.w - aligned.w}});
    if (diff.worst <= tolerance) return ::testing::AssertionSuccess();

    const float clamped  = std::clamp(std::fabs(math::Quaternion::Dot(actual, expected)), 0.0f, 1.0f);
    const float angleDeg = 2.0f * std::acos(clamped) * 57.2957795f;
    return Failed(actualExpr, expectedExpr, toleranceExpr,
                  Describe(actual), Describe(expected), diff, tolerance)
           << "\n  回転角の差: " << angleDeg << " 度";
}

::testing::AssertionResult CompareMatrix3(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Matrix3& actual,
                                          const math::Matrix3& expected,
                                          float tolerance)
{
    const ComponentDiff diff = WorstOfMatrix(actual.m, expected.m);
    if (diff.worst <= tolerance) return ::testing::AssertionSuccess();
    return Failed(actualExpr, expectedExpr, toleranceExpr,
                  Describe(actual), Describe(expected), diff, tolerance);
}

::testing::AssertionResult CompareMatrix4(const char* actualExpr,
                                          const char* expectedExpr,
                                          const char* toleranceExpr,
                                          const math::Matrix4& actual,
                                          const math::Matrix4& expected,
                                          float tolerance)
{
    const ComponentDiff diff = WorstOfMatrix(actual.m, expected.m);
    if (diff.worst <= tolerance) return ::testing::AssertionSuccess();
    return Failed(actualExpr, expectedExpr, toleranceExpr,
                  Describe(actual), Describe(expected), diff, tolerance);
}

::testing::AssertionResult CheckUnitLength(const char* actualExpr,
                                           const char* toleranceExpr,
                                           const math::Vector3& actual,
                                           float tolerance)
{
    const float length = actual.Length();
    if (std::fabs(length - 1.0f) <= tolerance) return ::testing::AssertionSuccess();

    return ::testing::AssertionFailure()
           << "\n  " << actualExpr << " = " << Describe(actual)
           << "\n  長さ = " << length << " (期待 1.0, 許容 " << toleranceExpr << " = "
           << tolerance << ")";
}

} // namespace fbzz::testkit
