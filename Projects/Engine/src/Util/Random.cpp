/// @file    Random.cpp
/// @brief   擬似乱数ユーティリティ実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// mt19937 を使い、数値範囲と単位円・単位球のサンプリングを提供する。
/// SetSeed で再現可能な乱数列にできる。
#include <Engine/Util/Random.hpp>
#include <random>
#include <cmath>

namespace fbzz::util {

namespace {

std::mt19937 s_rng{ std::random_device{}() };

} // namespace

static constexpr float TWO_PI = 3.14159265358979323846f * 2.0f;

float Random::Value()
{
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(s_rng);
}

float Random::Range(float min, float max)
{
    return std::uniform_real_distribution<float>(min, max)(s_rng);
}

int Random::Range(int min, int max)
{
    return std::uniform_int_distribution<int>(min, max)(s_rng);
}

math::Vector2 Random::InsideUnitCircle()
{
    /// @note rejection sampling — 平均 π/4 ≈ 1.27 試行で収束
    while (true) {
        float x = Range(-1.0f, 1.0f);
        float y = Range(-1.0f, 1.0f);
        if (x * x + y * y <= 1.0f)
            return { x, y };
    }
}

math::Vector2 Random::OnUnitCircle()
{
    float angle = Value() * TWO_PI;
    return { std::cos(angle), std::sin(angle) };
}

math::Vector3 Random::InsideUnitSphere()
{
    /// @note rejection sampling — 平均 π/6 の逆数 ≈ 1.91 試行で収束
    while (true) {
        float x = Range(-1.0f, 1.0f);
        float y = Range(-1.0f, 1.0f);
        float z = Range(-1.0f, 1.0f);
        if (x * x + y * y + z * z <= 1.0f)
            return { x, y, z };
    }
}

math::Vector3 Random::OnUnitSphere()
{
    /// @note Marsaglia (1972) — 一様分布が保証される
    float theta = Value() * TWO_PI;
    float phi   = std::acos(1.0f - 2.0f * Value());
    float sinPhi = std::sin(phi);
    return {
        sinPhi * std::cos(theta),
        sinPhi * std::sin(theta),
        std::cos(phi)
    };
}

void Random::SetSeed(unsigned int seed)
{
    s_rng.seed(seed);
}

/// @name RandomStream
/// サンプリング手法は Random と同じものを、自前の状態に対して行う。

math::Vector2 RandomStream::InsideUnitCircle()
{
    while (true) {
        float x = Range(-1.0f, 1.0f);
        float y = Range(-1.0f, 1.0f);
        if (x * x + y * y <= 1.0f)
            return { x, y };
    }
}

math::Vector2 RandomStream::OnUnitCircle()
{
    float angle = Value() * TWO_PI;
    return { std::cos(angle), std::sin(angle) };
}

math::Vector3 RandomStream::InsideUnitSphere()
{
    while (true) {
        float x = Range(-1.0f, 1.0f);
        float y = Range(-1.0f, 1.0f);
        float z = Range(-1.0f, 1.0f);
        if (x * x + y * y + z * z <= 1.0f)
            return { x, y, z };
    }
}

math::Vector3 RandomStream::OnUnitSphere()
{
    /// @note Marsaglia (1972) — 一様分布が保証される
    float theta = Value() * TWO_PI;
    float phi   = std::acos(1.0f - 2.0f * Value());
    float sinPhi = std::sin(phi);
    return {
        sinPhi * std::cos(theta),
        sinPhi * std::sin(theta),
        std::cos(phi)
    };
}

} // namespace fbzz::util
