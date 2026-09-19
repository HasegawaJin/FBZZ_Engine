/// @file    AnimationSampling.hpp
/// @brief   時刻付きキー列を 1 つの値へ落とすサンプリング関数群。
/// @author  Hasegawa Jin
/// @date    2026-08-26
///
/// AnimatorSystem (.anim) と SequenceSystem (.sequence) が共有する評価関数。
/// 実装を二重化すると補間結果がずれるため、ここに一本化する。
#pragma once

#include <Engine/Asset/AnimationClip.hpp>

#include <algorithm>
#include <vector>

namespace fbzz::asset {

[[nodiscard]] inline math::Vector3 SampleVectorKeys(const std::vector<VectorKey>& keys,
                                                    double ticks,
                                                    const math::Vector3& fallback,
                                                    AnimInterp interp = AnimInterp::Linear)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const VectorKey& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == AnimInterp::Step) return a.value;
    const double span = b.time - a.time;
    const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
    return math::Vector3::Lerp(a.value, b.value, t);
}

[[nodiscard]] inline math::Quaternion SampleQuaternionKeys(const std::vector<QuaternionKey>& keys,
                                                           double ticks,
                                                           const math::Quaternion& fallback,
                                                           AnimInterp interp = AnimInterp::Linear)
{
    if (keys.empty()) return fallback;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;

    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const QuaternionKey& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == AnimInterp::Step) return a.value;
    const double span = b.time - a.time;
    const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
    return math::Quaternion::Slerp(a.value, b.value, t).Normalized();
}

[[nodiscard]] inline float SampleFloatKeys(const std::vector<FloatKey>& keys,
                                           double ticks,
                                           AnimInterp interp)
{
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const FloatKey& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == AnimInterp::Step) return a.value;
    const double span = b.time - a.time;
    const float t = span > 0.0 ? static_cast<float>((ticks - a.time) / span) : 0.0f;
    if (interp != AnimInterp::Cubic) return a.value + (b.value - a.value) * t;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float duration = static_cast<float>(span);
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * a.value
         + (t3 - 2.0f * t2 + t) * a.outTangent * duration
         + (-2.0f * t3 + 3.0f * t2) * b.value
         + (t3 - t2) * b.inTangent * duration;
}

[[nodiscard]] inline math::Vector2 SampleVector2Keys(const std::vector<Vector2Key>& keys,
                                                     double ticks,
                                                     AnimInterp interp)
{
    if (keys.empty()) return math::Vector2::ZERO;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const Vector2Key& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == AnimInterp::Step) return a.value;
    const float t = static_cast<float>((ticks - a.time) / (b.time - a.time));
    return math::Vector2::Lerp(a.value, b.value, t);
}

[[nodiscard]] inline math::Vector4 SampleVector4Keys(const std::vector<Vector4Key>& keys,
                                                     double ticks,
                                                     AnimInterp interp)
{
    if (keys.empty()) return math::Vector4::ZERO;
    if (keys.size() == 1 || ticks <= keys.front().time) return keys.front().value;
    if (ticks >= keys.back().time) return keys.back().value;
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const Vector4Key& key) { return value < key.time; });
    const auto& b = *upper;
    const auto& a = *(upper - 1);
    if (interp == AnimInterp::Step) return a.value;
    const float t = static_cast<float>((ticks - a.time) / (b.time - a.time));
    return a.value + (b.value - a.value) * t;
}

/// 補間しない値 (int / bool) は直前のキーをそのまま返す。
template<typename Key>
[[nodiscard]] const Key& SampleDiscreteKey(const std::vector<Key>& keys, double ticks)
{
    const auto upper = std::upper_bound(keys.begin(), keys.end(), ticks,
        [](double value, const Key& key) { return value < key.time; });
    return upper == keys.begin() ? keys.front() : *(upper - 1);
}

} // namespace fbzz::asset
