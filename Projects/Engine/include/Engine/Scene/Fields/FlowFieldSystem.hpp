/// @file    FlowFieldSystem.hpp
/// @brief   シーンの FlowField を集めて Scene のフレームキャッシュを確定させる System。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// @note 流れの場を «物理より前に» 1 回だけ確定させる。

/// @note キャッシュ自体は Scene::FlowFrame() が遅延更新するので、この System が
/// @note 走らない経路 (マテリアルプレビュー / VFX Editor の別 Scene) でも場は消えない。
/// @note ここでやるのは «剛体が読む前に確定させる» ことだけ。
/// @note TransformPrePhysics は RunMode::SimOnly。編集中は TransformEditorPreview が
/// @note PreScript で回すのでワールド姿勢は確定している。順序ヒントは両方が走るときだけ効く。
class FlowFieldSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FlowFieldSystem"; }
    Phase            GetPhase()   const override { return Phase::PrePhysics; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    FBZZ_ENGINE_API ComponentAccess  GetAccess()  const override;
    FBZZ_ENGINE_API OrderingHints    GetOrder()   const override;
    FBZZ_ENGINE_API void             Update(SystemContext& ctx) override;
};

} /// @note namespace fbzz::scene
