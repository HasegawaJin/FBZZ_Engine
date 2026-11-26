/// @file    Approx.cpp
/// @brief   数学型の近似比較述語の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Assertions/Approx.hpp>

#include <TestKit/Output/Print.hpp>

#include <Math/LinearAlgebra/Matrix3.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector2.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Math/LinearAlgebra/Vector4.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>
#include <utility>

namespace fbzz::testkit {

namespace {

struct ComponentDiff {
    float worst = 0.0f; ///< 許容差と比べる最大の絶対差。非有限の成分があればその値。
    std::string name; ///< 最大差または最初の非有限値を検出した成分の表示名。

    bool IsWithin(float tolerance) const
    {
        return std::isfinite(worst) && std::isfinite(tolerance) && worst <= tolerance;
    }
};

ComponentDiff WorstOf(std::initializer_list<std::pair<const char*, float>> diffs)
{
    ComponentDiff result;
    for (const auto& [name, diff] : diffs) {
        const float magnitude = std::fabs(diff);
        if (!std::isfinite(magnitude)) {
            return {magnitude, name};
        }
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
        for (int column = 0; column < N; ++column) {
            const float magnitude = std::fabs(actual[row][column] - expected[row][column]);
            const bool magnitudeIsFinite = std::isfinite(magnitude);

            // 非有限値は許容差と比較できないため、最初に見つけた成分を失敗理由として返す。
            if (!magnitudeIsFinite || magnitude > result.worst) {
                result.worst = magnitude;
                result.name = "m[" + std::to_string(row) + "][" + std::to_string(column) + "]";
                if (!magnitudeIsFinite) {
                    return result;
                }
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
    if (diff.IsWithin(tolerance)) {
        return ::testing::AssertionSuccess();
    }
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
    if (diff.IsWithin(tolerance)) {
        return ::testing::AssertionSuccess();
    }
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
    if (diff.IsWithin(tolerance)) {
        return ::testing::AssertionSuccess();
    }
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
    // q と -q は同じ回転を表すため、成分差を求める前に符号を揃える。
    const float sign = (math::Quaternion::Dot(actual, expected) < 0.0f) ? -1.0f : 1.0f;
    const math::Quaternion aligned{expected.x * sign, expected.y * sign,
                                   expected.z * sign, expected.w * sign};

    const ComponentDiff diff = WorstOf({{"x", actual.x - aligned.x},
                                        {"y", actual.y - aligned.y},
                                        {"z", actual.z - aligned.z},
                                        {"w", actual.w - aligned.w}});
    if (diff.IsWithin(tolerance)) {
        return ::testing::AssertionSuccess();
    }

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
    if (diff.IsWithin(tolerance)) {
        return ::testing::AssertionSuccess();
    }
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
    if (diff.IsWithin(tolerance)) {
        return ::testing::AssertionSuccess();
    }
    return Failed(actualExpr, expectedExpr, toleranceExpr,
                  Describe(actual), Describe(expected), diff, tolerance);
}

::testing::AssertionResult CheckUnitLength(const char* actualExpr,
                                           const char* toleranceExpr,
                                           const math::Vector3& actual,
                                           float tolerance)
{
    const float length = actual.Length();
    if (std::isfinite(length) && std::isfinite(tolerance) && std::fabs(length - 1.0f) <= tolerance) {
        return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure()
           << "\n  " << actualExpr << " = " << Describe(actual)
           << "\n  長さ = " << length << " (期待 1.0, 許容 " << toleranceExpr << " = "
           << tolerance << ")";
}

} // namespace fbzz::testkit
