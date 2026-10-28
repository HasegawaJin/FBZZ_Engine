/// @file    Random.cpp
/// @brief   擬似乱数ユーティリティ実装。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#include <Engine/Util/Random.hpp>
#include <random>
#include <cmath>

namespace fbzz::util {

namespace {

std::mt19937 s_rng{ std::random_device{}() };

constexpr float TWO_PI = 3.14159265358979323846f * 2.0f;

template<typename SampleRange>
math::Vector2 SampleInsideUnitCircle(const SampleRange& sampleRange)
{
    while (true) {
        float x = sampleRange(-1.0f, 1.0f);
        float y = sampleRange(-1.0f, 1.0f);
        if (x * x + y * y <= 1.0f)
            return { x, y };
    }
}

template<typename SampleValue>
math::Vector2 SampleOnUnitCircle(const SampleValue& sampleValue)
{
    float angle = sampleValue() * TWO_PI;
    return { std::cos(angle), std::sin(angle) };
}

template<typename SampleRange>
math::Vector3 SampleInsideUnitSphere(const SampleRange& sampleRange)
{
    while (true) {
        float x = sampleRange(-1.0f, 1.0f);
        float y = sampleRange(-1.0f, 1.0f);
        float z = sampleRange(-1.0f, 1.0f);
        if (x * x + y * y + z * z <= 1.0f)
            return { x, y, z };
    }
}

template<typename SampleValue>
math::Vector3 SampleOnUnitSphere(const SampleValue& sampleValue)
{
    float theta = sampleValue() * TWO_PI;
    float phi   = std::acos(1.0f - 2.0f * sampleValue());
    float sinPhi = std::sin(phi);
    return {
        sinPhi * std::cos(theta),
        sinPhi * std::sin(theta),
        std::cos(phi)
    };
}

} /// @note namespace

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
    return SampleInsideUnitCircle([](float min, float max) { return Range(min, max); });
}

math::Vector2 Random::OnUnitCircle()
{
    return SampleOnUnitCircle([] { return Value(); });
}

math::Vector3 Random::InsideUnitSphere()
{
    return SampleInsideUnitSphere([](float min, float max) { return Range(min, max); });
}

math::Vector3 Random::OnUnitSphere()
{
    return SampleOnUnitSphere([] { return Value(); });
}

void Random::SetSeed(unsigned int seed)
{
    s_rng.seed(seed);
}

math::Vector2 RandomStream::InsideUnitCircle()
{
    return SampleInsideUnitCircle([this](float min, float max) { return Range(min, max); });
}

math::Vector2 RandomStream::OnUnitCircle()
{
    return SampleOnUnitCircle([this] { return Value(); });
}

math::Vector3 RandomStream::InsideUnitSphere()
{
    return SampleInsideUnitSphere([this](float min, float max) { return Range(min, max); });
}

math::Vector3 RandomStream::OnUnitSphere()
{
    return SampleOnUnitSphere([this] { return Value(); });
}

} /// @note namespace fbzz::util
