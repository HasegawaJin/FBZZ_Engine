/// @file    WaterRenderPass.hpp
/// @brief   WaterComponent を走査して水面メッシュ・泡・波紋を描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

// AddWaterRipple — 水面ローカル UV [0,1] に動的なリング波紋を追加する。
void AddWaterRipple(
    EntityID      waterEntity,
    math::Vector2 positionUV,
    float         amplitude = 0.5f,
    float         speed     = 0.25f,
    float         decayRate = 1.5f,
    float         waveWidth = 0.038f);

// QueueWaterSplash — ワールド座標に水しぶきパーティクルバーストをキューに積む。
// WHY: PhysicsSystem / IKSystem は Scene を直接操作しないため、キュー経由で委譲する。
void QueueWaterSplash(const math::Vector3& worldPos, float intensity);

// WorldToWaterUV — ワールド座標を水面 UV [0,1] に変換する。
inline math::Vector2 WorldToWaterUV(
    const math::Vector3&  worldPos,
    const WaterComponent& water,
    const Transform&      waterTransform)
{
    return {
        math::Clamp01((worldPos.x - waterTransform.worldPosition.x + water.extentX * 0.5f) / water.extentX),
        math::Clamp01((worldPos.z - waterTransform.worldPosition.z + water.extentZ * 0.5f) / water.extentZ)
    };
}

struct RenderPassContext;
void WaterSelectionMaskSystem(RenderPassContext& ctx);

// IRenderPass 実装 — RenderPipeline::AddPass<WaterRenderPass>() で登録する。
class WaterRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    std::vector<renderer::RenderGraph::ResourceAccess> DeclareAccesses(
        const RenderPassContext& ctx) const override;
    void Execute(RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
