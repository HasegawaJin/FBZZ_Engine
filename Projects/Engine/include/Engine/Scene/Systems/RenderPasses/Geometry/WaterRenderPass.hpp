/// @file    WaterRenderPass.hpp
/// @brief   WaterComponent を走査して水面メッシュ・泡・波紋を描画する IRenderPass 実装。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Passes/Geometry/WaterRenderPass.hpp>

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

/// @note 波紋テクスチャの一辺 [texel]。水面 1 枚につき 1 枚だけ張る。
inline constexpr uint32_t kWaterRippleTextureSize = 128;

using renderer::WaterDetailNoise;
using renderer::GetWaterDetailNoise;
using renderer::WaterSelectionMaskSystem;
using renderer::WaterRenderPass;
/// @note 輪を積む入口は WaterSystem の EmitWaterRipple。正本は WaterComponent::ripples で、
/// @note このパスはそれを読んで波紋テクスチャを焼くだけになった (旧 AddWaterRipple は廃止)。

class Scene;
struct EntityID;
/// @note Sceneと実効ownerの値だけを記録し、粒子生成はCleanupの構造変更境界へ委譲する。
void QueueWaterSplash(Scene& scene, EntityID owner, const math::Vector3& worldPos, float intensity);

/// @note WorldToWaterUV — ワールド座標を水面 UV [0,1] に変換する。
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

}
