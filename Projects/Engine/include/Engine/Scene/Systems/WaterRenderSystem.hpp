// FBZZ Engine
// WaterRenderSystem.hpp | fbzz::scene
// WaterComponent を走査して水面メッシュ・泡・波紋を描画するシステム
#pragma once

#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector3.hpp>

namespace fbzz::scene {

class Scene;

} // namespace fbzz::scene

namespace fbzz::renderer {

class IRenderer;
class Camera;
class ResourceManager;
struct ConstantBufferTag;
struct RenderSettings;

} // namespace fbzz::renderer

namespace fbzz::scene {

// WaterRenderSystem — 水面専用の透明描画パスを発行する。
// WHY: メッシュ再生成・Terrain 参照の泡マスク生成・動的波紋は複数コンポーネントを横断するため System に置く。
void WaterRenderSystem(
    Scene&                                        scene,
    renderer::IRenderer&                          renderer,
    renderer::ResourceManager&                    resources,
    const renderer::Camera&                       camera,
    renderer::ResourceHandle<renderer::RenderTargetTag> outputRT,
    renderer::ResourceHandle<renderer::TextureTag> sceneColor,
    float                                         elapsedTime,
    const renderer::RenderSettings*               settings = nullptr,
    renderer::ResourceHandle<renderer::ConstantBufferTag> lightCB = {},
    renderer::ResourceHandle<renderer::TextureTag> shadowDepthTexture = {},
    renderer::ResourceHandle<renderer::ConstantBufferTag> shadowCB = {});

// AddWaterRipple — 水面ローカル UV [0,1] に動的なリング波紋を追加する。
// WHAT: スクリプトや物理イベントから水面接触を通知し、描画時に CPU テクスチャへ焼き込む。
void AddWaterRipple(
    EntityID      waterEntity,
    math::Vector2 positionUV,
    float         amplitude = 0.5f,
    float         speed     = 0.25f,
    float         decayRate = 1.5f,
    float         waveWidth = 0.038f);

// QueueWaterSplash — ワールド座標に水しぶきパーティクルバーストをキューに積む。
// WHAT: Scene 参照不要。次フレームの WaterRenderSystem が ParticleEmitter GO を生成して消費する。
// WHY: PhysicsSystem / IKSystem は Scene を直接操作しないため、キュー経由で委譲する。
void QueueWaterSplash(const math::Vector3& worldPos, float intensity);

// WorldToWaterUV — ワールド座標を水面 UV [0,1] に変換する。
// WHAT: AddWaterRipple の positionUV 引数に渡す UV を計算するユーティリティ。
//       水面 Extent の外側の座標は [0,1] にクランプされる。
// WHY: スクリプトからリップルを生成するとき、ワールド座標→水面UV変換を毎回手書きしないよう
//      ここで提供する。PhysicsSystem の WaterBuoyancyVolume からも使用する。
inline math::Vector2 WorldToWaterUV(
    const math::Vector3&  worldPos,
    const WaterComponent& water,
    const Transform&      waterTransform)
{
    return {
        math::Clamp01((worldPos.x - waterTransform.position.x + water.extentX * 0.5f) / water.extentX),
        math::Clamp01((worldPos.z - waterTransform.position.z + water.extentZ * 0.5f) / water.extentZ)
    };
}

} // namespace fbzz::scene
