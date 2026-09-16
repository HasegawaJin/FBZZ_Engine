/// @file    AnimatorSystem.hpp
/// @brief   スケルタルアニメーションのサンプリングと GPU 転送。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

/// @brief AnimationClip を評価し、ボーン姿勢と SkinnedMeshRenderer 用の骨行列を更新する。
/// @note 頂点の変形は行わない。GPU スキニングは SkinningComputePass が boneMatrices を読んで行う。
class AnimatorSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "AnimatorSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    /// @note ctx.resources が nullptr のときは何もしない (定数バッファを作れないため)。
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
