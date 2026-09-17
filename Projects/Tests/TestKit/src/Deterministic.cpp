/// @file    Deterministic.cpp
/// @brief   固定シード乱数の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <TestKit/Deterministic.hpp>

#include <Math/Quaternion.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

#include <cmath>

namespace fbzz::testkit {

namespace {
constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
} // namespace

DeterministicRng::DeterministicRng(std::uint64_t seed)
    : m_state(seed == 0 ? kDefaultSeed : seed)
{
}

std::uint32_t DeterministicRng::NextUInt()
{
    m_state ^= m_state >> 12;
    m_state ^= m_state << 25;
    m_state ^= m_state >> 27;
    return static_cast<std::uint32_t>((m_state * 0x2545F4914F6CDD1Dull) >> 32);
}

float DeterministicRng::NextFloat()
{
    /// @note 仮数部 24bit ぶんだけ使う。float に収まらない下位ビットを捨てることで
    ///       1.0 に丸め上がって [0,1) の約束が破れるのを防ぐ。
    return static_cast<float>(NextUInt() >> 8) * (1.0f / 16777216.0f);
}

float DeterministicRng::NextFloat(float minValue, float maxValue)
{
    return minValue + (maxValue - minValue) * NextFloat();
}

int DeterministicRng::NextInt(int minValue, int maxValue)
{
    if (maxValue <= minValue) return minValue;
    const std::uint32_t span = static_cast<std::uint32_t>(maxValue - minValue) + 1u;
    return minValue + static_cast<int>(NextUInt() % span);
}

math::Vector2 DeterministicRng::NextVector2(float minValue, float maxValue)
{
    const float x = NextFloat(minValue, maxValue);
    const float y = NextFloat(minValue, maxValue);
    return {x, y};
}

math::Vector3 DeterministicRng::NextVector3(float minValue, float maxValue)
{
    const float x = NextFloat(minValue, maxValue);
    const float y = NextFloat(minValue, maxValue);
    const float z = NextFloat(minValue, maxValue);
    return {x, y, z};
}

math::Vector3 DeterministicRng::NextUnitVector3()
{
    /// @note z を一様に取ってから方位角を振る (アルキメデスの定理)。
    ///       成分を個別に振って正規化すると、立方体の角方向が濃くなって一様にならない。
    const float z     = NextFloat(-1.0f, 1.0f);
    const float theta = NextFloat(0.0f, kTwoPi);
    const float rSq   = 1.0f - z * z;
    const float r     = std::sqrt(rSq > 0.0f ? rSq : 0.0f);
    return {r * std::cos(theta), r * std::sin(theta), z};
}

math::Quaternion DeterministicRng::NextRotation()
{
    /// @note Shoemake の方法。4 成分を個別に振って正規化するより回転が一様に散る。
    const float u1 = NextFloat();
    const float u2 = NextFloat(0.0f, kTwoPi);
    const float u3 = NextFloat(0.0f, kTwoPi);
    const float a  = std::sqrt(1.0f - u1);
    const float b  = std::sqrt(u1);
    return {a * std::sin(u2), a * std::cos(u2), b * std::sin(u3), b * std::cos(u3)};
}

} // namespace fbzz::testkit
