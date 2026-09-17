/// @file    FlowFieldSystem.hpp
/// @brief   シーンの FlowField を集めて Scene のフレームキャッシュを確定させる System。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#pragma once
#include <Engine/Core/Scheduler/ISystem.hpp>

namespace fbzz::scene {

/// 流れの場を «物理より前に» 1 回だけ確定させる。
///
/// @note キャッシュ自体は Scene::FlowFrame() が遅延更新するので、この System が
///       走らない経路 (マテリアルプレビュー / VFX Editor の別 Scene) でも場は消えない。
///       ここでやるのは «剛体が読む前に確定させる» ことだけ。
/// @note TransformPrePhysics は RunMode::SimOnly。編集中は TransformEditorPreview が
///       PreScript で回すのでワールド姿勢は確定している。順序ヒントは両方が走るときだけ効く。
class FlowFieldSystem final : public ISystem {
public:
    std::string_view Name()       const override { return "FlowFieldSystem"; }
    Phase            GetPhase()   const override { return Phase::PrePhysics; }
    RunMode          GetRunMode() const override { return RunMode::Always; }
    ComponentAccess  GetAccess()  const override;
    OrderingHints    GetOrder()   const override;
    void             Update(SystemContext& ctx) override;
};

} // namespace fbzz::scene
