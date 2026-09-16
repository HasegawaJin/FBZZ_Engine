/// @file    AnimatorSystem.hpp
/// @brief   スケルタルアニメーションのサンプリングと GPU 転送。
/// @author  Hasegawa Jin
/// @date    2026-05-24
///
/// AnimationClip を評価し、SkinnedMeshRenderer 用の骨行列を更新する。
/// ctx.resources が nullptr の場合はスキップ（Update() 内で guard）。
#pragma once
#include "Engine/Core/Scheduler/ISystem.hpp"

namespace fbzz::scene {

class AnimatorSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "AnimatorSystem"; }
    Phase            GetPhase()   const override { return Phase::LateUpdate; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
