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

/// @note QueueWaterSplash — ワールド座標に水しぶきパーティクルバーストをキューに積む。
/// @note PhysicsSystem / IKSystem は Scene を直接操作しないため、キュー経由で委譲する。
void QueueWaterSplash(const math::Vector3& worldPos, float intensity);

class Scene;
/// @brief 積まれた水しぶきを GameObject として生成し、鳴り終わったものを破棄する。
/// @pre 描画中に呼ばないこと。生成・破棄はコンポーネント配列を詰め替えるので、
/// @note 同じフレームの後続パスが握っているエミッターを別物にすり替える。
/// @note WaterSystem::Update が毎フレーム呼ぶ。しぶきはシーン内の名前と
/// @note runtimeGenerated で見分けるので、Play 停止やシーン遷移をまたいで別シーンを消さない。
void UpdateWaterSplashes(Scene& scene);

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
