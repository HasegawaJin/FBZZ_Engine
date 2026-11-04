/// @file    BoneBakeTypes.hpp
/// @brief   Animator が保持するベイク姿勢・状態の値型。評価処理には依存しない。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Asset/BakedBoneAnimation.hpp>
#include <Graphics/Renderer/ResourceHandle.hpp>
#include <cstdint>
#include <memory>

namespace fbzz::scene {
enum class BoneBakeMode : int { LEGACY = 0, CPU_BAKED = 1, GPU_SHARED = 2, GPU_POSE_SHARED = 3 };
enum class BoneBakeFallback : uint8_t {
    NONE, DISABLED, NEAR_CAMERA, CPU_BONES_REQUIRED, RIG_CONTROL,
    ROOT_MOTION, LAYERS_OR_MASK, MORPH, MUTABLE_CLIP, CLIP_COUNT,
    INVALID_SOURCE, INVALID_CLOCK, INCOMPATIBLE_LOD, GPU_RESOURCE, GPU_DISPATCH
};
FBZZ_ENGINE_API const char* BoneBakeFallbackName(BoneBakeFallback reason);

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
} /// @note namespace fbzz::scene
