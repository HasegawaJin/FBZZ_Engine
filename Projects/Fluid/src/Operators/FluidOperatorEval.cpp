/// @file    FluidOperatorEval.cpp
/// @brief   発生源・外力・障害物の時刻による動き、量、稼働判定。
/// @author  Hasegawa Jin
/// @date    2026-09-12

#include <Fluid/Operators/FluidOperatorEval.hpp>
#include <algorithm>

namespace fbzz::fluid {

namespace {

[[nodiscard]] bool InWindow(bool enabled, float startTime, float duration, float time)
{
    if (!enabled || time < startTime) return false;
    return duration <= 0.0f || time < startTime + duration;
}

FluidOperatorPose ApplyOperatorMotion(const math::Vector3& center, const FluidMotion& motion, float time)
{
    const FluidMotionSample sample = SampleFluidMotion(motion, time);
    return { center + sample.offset,
             motion.inheritVelocity ? sample.velocity : math::Vector3{ 0.0f, 0.0f, 0.0f } };
}

} /// @note namespace

/// @note FluidGpuCommon.hlsli と演算の順序を揃え、CPU と GPU の丸めの差を避ける。
FluidMotionSample SampleFluidMotion(const FluidMotion& motion, float time)
{
    FluidMotionSample sample{ { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    const std::vector<FluidMotionKey>& keys = motion.keys;
    if (keys.empty()) return sample;
    if (time <= keys.front().time) {
        sample.offset = keys.front().offset;
        return sample;
    }
    if (time >= keys.back().time) {
        sample.offset = keys.back().offset;
        return sample;
    }
    for (std::size_t keyIndex = 0; keyIndex + 1 < keys.size(); ++keyIndex) {
        const FluidMotionKey& startKey = keys[keyIndex];
        const FluidMotionKey& endKey = keys[keyIndex + 1];
        if (time >= endKey.time) continue;
        const float keyDuration = endKey.time - startKey.time;
        if (keyDuration < 1.0e-6f) {
            sample.offset = startKey.offset;
            return sample;
        }
        const float elapsedFraction = (time - startKey.time) / keyDuration;
        const math::Vector3 offsetDelta = endKey.offset - startKey.offset;
        sample.offset = startKey.offset + offsetDelta * elapsedFraction;
        sample.velocity = offsetDelta * (1.0f / keyDuration);
        return sample;
    }
    sample.offset = keys.back().offset;
    return sample;
}

float SampleFluidAmount(const FluidAmount& amount, float time)
{
    const std::vector<FluidAmountKey>& keys = amount.keys;
    if (keys.empty()) return 1.0f;
    if (time <= keys.front().time) return keys.front().scale;
    if (time >= keys.back().time) return keys.back().scale;
    for (std::size_t keyIndex = 0; keyIndex + 1 < keys.size(); ++keyIndex) {
        const FluidAmountKey& startKey = keys[keyIndex];
        const FluidAmountKey& endKey = keys[keyIndex + 1];
        if (time >= endKey.time) continue;
        const float keyDuration = endKey.time - startKey.time;
        if (keyDuration < 1.0e-6f) return startKey.scale;
        return startKey.scale + (endKey.scale - startKey.scale) * ((time - startKey.time) / keyDuration);
    }
    return keys.back().scale;
}

FluidOperatorPose PoseFluidSource(const FluidSource& source, float time)
{
    return ApplyOperatorMotion(source.center, source.motion, time);
}

FluidOperatorPose PoseFluidForce(const FluidForce& force, float time)
{
    return ApplyOperatorMotion(force.center, force.motion, time);
}

float FluidSourceAmount(const FluidSource& source, float time)
{
    /// @note 発生源だけ 0 で止める理由: 掛かる先は density/temperature/fuel で、負にすると «質量を引く»
    ///       ことになり、散逸・移流・色の鍵 (mass/max(carrier,ε))・黒体放射 (T^4) が符号を想定していない。
    ///       «煙を削る» は倍率の裏口でなく別の部品で設計する。力側は strength が元々負を許すので止めない。
    return (std::max)(SampleFluidAmount(source.amount, time), 0.0f);
}

float FluidForceAmount(const FluidForce& force, float time)
{
    return SampleFluidAmount(force.amount, time);
}

bool FluidSourceEmitting(const FluidSource& source, float time)
{
    return InWindow(source.enabled, source.startTime, source.duration, time);
}

bool FluidForceActive(const FluidForce& force, float time)
{
    return InWindow(force.enabled, force.startTime, force.duration, time);
}

FluidOperatorPose PoseFluidCollider(const FluidCollider& collider, float time)
{
    return ApplyOperatorMotion(collider.center, collider.motion, time);
}

bool FluidColliderActive(const FluidCollider& collider, float time)
{
    return InWindow(collider.enabled, collider.startTime, collider.duration, time);
}

} /// @note namespace fbzz::fluid
