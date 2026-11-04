/// @file    BoneBakeRuntime.hpp
/// @brief   ベイク済み骨アニメーションの評価・保持・CPU 姿勢復元。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Components/BoneBakeTypes.hpp>
#include <span>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset { struct Model; struct AnimationClip; }
namespace fbzz::scene {
struct AnimatorComponent;

struct BoneBakeClipInput {
    const asset::AnimationClip* clip = nullptr;
    double ticks = 0.0;
    float weight = 0.0f;
};

/// @note Asset loading/edit preparation warms shared derived caches before playback; unsupported clips remain ordinary Animator inputs.
FBZZ_ENGINE_API void PrepareBoneBakeAssets(AnimatorComponent& animator, const asset::Model& model);

/// @note Uses only prepared immutable cache entries; a changed source, sample rate, generation or invalid clock requires a fresh pose.
/// @note The caller applies the distance LOD and excludes CPU bone consumers, controls and transitions before retaining the published pose.
FBZZ_ENGINE_API bool CanHoldBoneBake(const AnimatorComponent& animator, const asset::Model& model,
                     std::span<const BoneBakeClipInput> clips);

/// @note Caller excludes CPU bone consumers, rig controls, masks, layers, morph and root motion before taking this path.
/// @note GPU_POSE_SHARED defers CPU palettes only after a successful GPU preparation; CPU bone readers must opt out.
/// @note Reconstructs deferred current/previous CPU palettes before a legacy consumer or a failed GPU preparation.
/// @return False invalidates previous history; validated immutable bake inputs normally cannot fail evaluation.
FBZZ_ENGINE_API bool MaterializeBoneBakeCpu(AnimatorComponent& animator, renderer::ResourceManager& resources, bool current);

FBZZ_ENGINE_API bool TryEvaluateBoneBake(AnimatorComponent& animator, const asset::Model& model,
                         std::span<const BoneBakeClipInput> clips);
}
