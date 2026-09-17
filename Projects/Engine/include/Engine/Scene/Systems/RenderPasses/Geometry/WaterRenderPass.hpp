/// @file    WaterRenderPass.hpp
/// @brief   WaterComponent を走査して水面メッシュ・泡・波紋を描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

#include <Engine/Scene/Systems/RenderPasses/IRenderPass.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>
#include <cstdint>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {

/// 波紋テクスチャの一辺 [texel]。水面 1 枚につき 1 枚だけ張る。
inline constexpr uint32_t kWaterRippleTextureSize = 128;

/// さざ波タイル 1 枚と、その勾配を復号する係数。
struct WaterDetailNoise {
    renderer::ResourceHandle<renderer::TextureTag> texture;
    /// RG を [-1,1] へ戻したあとに掛ける係数。Water.hlsl の g_normalParams.z へ渡す。
    float derivativeScale = 1.0f;
    /// 1 / タイル 1 辺のノイズセル数。Water.hlsl の g_timeParams.z へ渡す。
    float invTileCells = 1.0f;
};

/// 全水面が共有するさざ波タイルを返す (初回に焼く)。
/// @note マテリアルプレビューも同じタイルを引く。別々に焼くとプレビューと本編で
///       さざ波の位相が食い違い、詰めた値がビューポートで再現しない。
/// @see Docs/design/water-waves.md
[[nodiscard]] const WaterDetailNoise& GetWaterDetailNoise(renderer::ResourceManager& resources);

/// @note 輪を積む入口は WaterSystem の EmitWaterRipple。正本は WaterComponent::ripples で、
///       このパスはそれを読んで波紋テクスチャを焼くだけになった (旧 AddWaterRipple は廃止)。

/// QueueWaterSplash — ワールド座標に水しぶきパーティクルバーストをキューに積む。
/// @note PhysicsSystem / IKSystem は Scene を直接操作しないため、キュー経由で委譲する。
void QueueWaterSplash(const math::Vector3& worldPos, float intensity);

class Scene;
/// @brief 積まれた水しぶきを GameObject として生成し、鳴り終わったものを破棄する。
/// @pre 描画中に呼ばないこと。生成・破棄はコンポーネント配列を詰め替えるので、
///      同じフレームの後続パスが握っているエミッターを別物にすり替える。
/// @note WaterSystem::Update が毎フレーム呼ぶ。しぶきはシーン内の名前と
///       runtimeGenerated で見分けるので、Play 停止やシーン遷移をまたいで別シーンを消さない。
void UpdateWaterSplashes(Scene& scene);

/// WorldToWaterUV — ワールド座標を水面 UV [0,1] に変換する。
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

/// IRenderPass 実装 — RenderPipeline::`AddPass<WaterRenderPass>()` で登録する。
class WaterRenderPass final : public IRenderPass {
public:
    std::string_view Name() const override;
    void Setup(PassBuilder& builder, const RenderPassContext& ctx) const override;
    void Execute(PassResources& resources, RenderPassContext& ctx) override;
};

} // namespace fbzz::scene
