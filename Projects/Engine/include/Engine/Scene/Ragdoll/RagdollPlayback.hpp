/// @file    RagdollPlayback.hpp
/// @brief   物理・復帰・ブレンドが共有する固定刻みと連続な終了遷移。
/// @author  Hasegawa Jin
/// @date    2026-09-13
#pragma once
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <Math/MathUtils.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

inline constexpr float RAGDOLL_FIXED_STEP = 1.0f / 120.0f;

/// 過負荷時の残り時間は捨てず次回へ持ち越す。物理と各タイマーは返した回数だけ進める。
inline int AccumulateRagdollSteps(double& remaining, float dt)
{
    if (std::isfinite(dt) && dt > 0.0f) remaining += dt;
    const int count = static_cast<int>(std::min(16.0,
        std::floor((remaining + 1.0e-9) / RAGDOLL_FIXED_STEP)));
    remaining = std::max(0.0, remaining - count * static_cast<double>(RAGDOLL_FIXED_STEP));
    return count;
}

inline void BeginRagdollBlendOut(RagdollComponent& ragdoll)
{
    ragdoll.endRequested = false;
    ragdoll.phaseStartWeight = math::Clamp01(ragdoll.weight);
    ragdoll.phase = RagdollPhase::BlendOut;
    ragdoll.phaseTimer = 0.0f;
}

inline float AdvanceRagdollPhase(RagdollComponent& ragdoll, float dt)
{
    const float ceiling = math::Clamp01(ragdoll.activationWeight);

    switch (ragdoll.phase) {
    case RagdollPhase::BlendIn: {
        ragdoll.phaseTimer += dt;
        const float duration = std::max(ragdoll.blendIn, 0.0f);
        if (duration <= 0.0f || ragdoll.phaseTimer >= duration) {
            ragdoll.phase = RagdollPhase::Hold;
            ragdoll.phaseTimer = 0.0f;
            ragdoll.weight = ceiling;
        } else {
            ragdoll.weight = math::Lerp(ragdoll.phaseStartWeight, ceiling,
                                       math::Clamp01(ragdoll.phaseTimer / duration));
        }
        break;
    }
    case RagdollPhase::Hold: {
        ragdoll.weight = ceiling;
        if (ragdoll.holdRemaining > 0.0f) {
            ragdoll.holdRemaining -= dt;
            if (ragdoll.holdRemaining <= 0.0f) ragdoll.endRequested = true;
        }
        break;
    }
    case RagdollPhase::BlendOut: {
        ragdoll.phaseTimer += dt;
        const float duration = std::max(ragdoll.blendOut, 0.0f);
        if (duration <= 0.0f || ragdoll.phaseTimer >= duration) {
            ragdoll.phase  = RagdollPhase::Idle;
            ragdoll.weight = 0.0f;
            ragdoll.phaseTimer = 0.0f;
        } else {
            ragdoll.weight = ragdoll.phaseStartWeight * (1.0f - math::Clamp01(ragdoll.phaseTimer / duration));
        }
        break;
    }
    case RagdollPhase::Idle:
    default:
        ragdoll.weight = 0.0f;
        break;
    }
    return math::Clamp01(ragdoll.weight);
}

} // namespace fbzz::scene
