/// @file    SplineCurve.cpp
/// @brief   Numeric Catmull-Rom position, tangent, and length evaluation.
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/SplineCurve.hpp>

#include <algorithm>
#include <cmath>

namespace fbzz::scene {
namespace {

float Clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

math::Vector3 Multiply(const math::Vector3& a, const math::Vector3& b)
{
    return { a.x * b.x, a.y * b.y, a.z * b.z };
}

struct CurveSegment {
    const math::Vector3* p0 = nullptr;
    const math::Vector3* p1 = nullptr;
    const math::Vector3* p2 = nullptr;
    const math::Vector3* p3 = nullptr;
    float t = 0.0f;
    int segmentCount = 0;
};

CurveSegment GetSegment(std::span<const math::Vector3> points, bool closed, float normalizedPosition)
{
    const int count = static_cast<int>(points.size());
    if (count < 2)
        return {};

    const int segmentCount = closed ? count : count - 1;
    const float scaled = Clamp01(normalizedPosition) * static_cast<float>(segmentCount);
    const int segment = std::min(static_cast<int>(scaled), segmentCount - 1);
    const float t = scaled - static_cast<float>(segment);
    const auto point = [points, closed, count](int index) -> const math::Vector3& {
        if (closed) {
            index = (index % count + count) % count;
            return points[static_cast<std::size_t>(index)];
        }
        return points[static_cast<std::size_t>(std::clamp(index, 0, count - 1))];
    };
    return { &point(segment - 1), &point(segment), &point(segment + 1), &point(segment + 2),
        t, segmentCount };
}

} /// @note namespace

math::Vector3 EvaluateSplineCurve(std::span<const math::Vector3> points, bool closed,
    float normalizedPosition)
{
    if (points.empty())
        return math::Vector3::ZERO;
    if (points.size() == 1)
        return points.front();

    const CurveSegment segment = GetSegment(points, closed, normalizedPosition);
    const math::Vector3& p0 = *segment.p0;
    const math::Vector3& p1 = *segment.p1;
    const math::Vector3& p2 = *segment.p2;
    const math::Vector3& p3 = *segment.p3;
    const float t = segment.t;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (p1 * 2.0f + (p2 - p0) * t
        + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2
        + (-p0 + p1 * 3.0f - p2 * 3.0f + p3) * t3) * 0.5f;
}

math::Vector3 EvaluateSplineCurveTangent(std::span<const math::Vector3> points, bool closed,
    float normalizedPosition)
{
    if (points.size() < 2)
        return math::Vector3::ZERO;

    const CurveSegment segment = GetSegment(points, closed, normalizedPosition);
    const math::Vector3& p0 = *segment.p0;
    const math::Vector3& p1 = *segment.p1;
    const math::Vector3& p2 = *segment.p2;
    const math::Vector3& p3 = *segment.p3;
    const float t = segment.t;
    const float t2 = t * t;
    const math::Vector3 derivative = ((p2 - p0)
        + (p0 * 4.0f - p1 * 10.0f + p2 * 8.0f - p3 * 2.0f) * t
        + (-p0 * 3.0f + p1 * 9.0f - p2 * 9.0f + p3 * 3.0f) * t2) * 0.5f;
    return derivative * static_cast<float>(segment.segmentCount);
}

float EstimateSplineCurveLength(std::span<const math::Vector3> points, bool closed,
    const math::Vector3& worldScale)
{
    constexpr int SAMPLES = 32;
    float length = 0.0f;
    math::Vector3 previous = EvaluateSplineCurve(points, closed, 0.0f);
    for (int index = 1; index <= SAMPLES; ++index) {
        const math::Vector3 current = EvaluateSplineCurve(points, closed,
            static_cast<float>(index) / static_cast<float>(SAMPLES));
        length += Multiply(current - previous, worldScale).Length();
        previous = current;
    }
    return std::max(length, 0.0001f);
}

} /// @note namespace fbzz::scene
