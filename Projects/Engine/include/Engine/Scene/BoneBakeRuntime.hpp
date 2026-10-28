/// @file    BoneBakeRuntime.hpp
/// @brief   Per-individual baked clip state while immutable clip samples remain shared.
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Asset/BakedBoneAnimation.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <memory>
#include <span>
#include <string>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::asset { struct Model; struct AnimationClip; }
namespace fbzz::scene {
struct AnimatorComponent;

enum class BoneBakeMode : int { LEGACY = 0, CPU_BAKED = 1, GPU_SHARED = 2, GPU_POSE_SHARED = 3 };
enum class BoneBakeFallback : uint8_t {
    NONE, DISABLED, NEAR_CAMERA, CPU_BONES_REQUIRED, RIG_CONTROL,
    ROOT_MOTION, LAYERS_OR_MASK, MORPH, MUTABLE_CLIP, CLIP_COUNT,
    INVALID_SOURCE, INVALID_CLOCK, INCOMPATIBLE_LOD, GPU_RESOURCE, GPU_DISPATCH
};
const char* BoneBakeFallbackName(BoneBakeFallback reason);

/// @note Immutable clip references preserve the exact previous displayed pose across LOD holds and clip transitions.
struct BoneBakePoseState {
    std::shared_ptr<const asset::BakedBoneAnimation> source, target;
    asset::BakedBoneFrame frame, targetFrame;
    float blendWeight = 0.0f;
};
struct BoneBakeRuntime {
    BoneBakePoseState previous;
    renderer::ResourceHandle<renderer::StructuredBufferTag> gpuPalette, gpuPreviousPalette;
    uint32_t gpuOffset = 0, gpuPreviousOffset = 0;
    uint64_t gpuOwner = 0, gpuEpoch = 0, gpuFrame = UINT64_MAX;
    bool previousPoseValid = false;
    bool gpuReady = false, cpuDeferred = false, previousCpuDeferred = false;
    bool active = false;
    bool invalidateHistory = true;
    BoneBakeFallback fallback = BoneBakeFallback::DISABLED;
    BoneBakeFallback reportedFallback = BoneBakeFallback::DISABLED;
    asset::BoneBakeStatus bakeStatus = asset::BoneBakeStatus::OK;
    int generation = -1;
    float sampleRate = 0.0f;
    float radius = 0.0f;
    std::shared_ptr<const asset::BakedBoneAnimation> source, target;
    asset::BakedBoneFrame frame, targetFrame;
    float blendWeight = 0.0f;
    asset::BakedBoneWorkspace workspace;
};
struct BoneBakeClipInput {
    const asset::AnimationClip* clip = nullptr;
    double ticks = 0.0;
    float weight = 0.0f;
};

/// @note Asset loading/edit preparation warms shared derived caches before playback; unsupported clips remain ordinary Animator inputs.
void PrepareBoneBakeAssets(AnimatorComponent& animator, const asset::Model& model);

/// @note Uses only prepared immutable cache entries; a changed source, sample rate, generation or invalid clock requires a fresh pose.
/// @note The caller applies the distance LOD and excludes CPU bone consumers, controls and transitions before retaining the published pose.
bool CanHoldBoneBake(const AnimatorComponent& animator, const asset::Model& model,
                     std::span<const BoneBakeClipInput> clips);

/// @note Caller excludes CPU bone consumers, rig controls, masks, layers, morph and root motion before taking this path.
/// @note GPU_POSE_SHARED defers CPU palettes only after a successful GPU preparation; CPU bone readers must opt out.
/// @note Reconstructs deferred current/previous CPU palettes before a legacy consumer or a failed GPU preparation.
/// @return False invalidates previous history; validated immutable bake inputs normally cannot fail evaluation.
bool MaterializeBoneBakeCpu(AnimatorComponent& animator, renderer::ResourceManager& resources, bool current);

bool TryEvaluateBoneBake(AnimatorComponent& animator, const asset::Model& model,
                         std::span<const BoneBakeClipInput> clips);
}
