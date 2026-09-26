/// @file    RenderPassContext.hpp
/// @brief   Scene の抽出・拡張を Graphics の実行文脈へ接続する。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once
#include <Graphics/Pipeline/RenderPassContext.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Scene/CameraCullingSettings.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/ParticleCurve.hpp>
#include <Engine/Scene/Systems/RenderPasses/EnvironmentResources.hpp>
#include <Engine/Scene/Systems/RenderPasses/PassResources.hpp>
#include <Engine/Scene/Systems/RenderPasses/OcclusionCuller.hpp>
#include <Physics/Layer.hpp>
namespace fbzz::physics { class World; }
namespace fbzz::scene {
class Scene;
struct RenderSystemUIOptions;
struct RenderPassContext;
using renderer::UserRenderPassInjectionPoint;
using renderer::GpuFlowField;
using renderer::GpuParticleEmitterCB;
using renderer::PerFrameCB;
using renderer::PerObjectCB;
using renderer::ClusterLightMode;
using renderer::PunctualLightType;
using renderer::PunctualLightGPU;
using renderer::PunctualShadowConstantsCB;
using renderer::FroxelFogCB;
using renderer::FroxelFogViewState;
using renderer::AutoExposureCB;
using renderer::CookieBlitCB;
using renderer::ClusterConstantsCB;
using renderer::ShadowConstantsCB;
using renderer::AtmosphereCB;
using renderer::ProbeVolumeParamsCB;
using renderer::AdvancedGraphicsCB;
using renderer::PostProcCB;
using renderer::OutlineCB;
using renderer::ObjectMaskCB;
using renderer::DecalCB;
using renderer::DecalMaterialCB;
using renderer::DecalReceiverCB;
using renderer::ParticleVertex;
using renderer::ParticleRenderCB;
using renderer::TrailVertex;
using renderer::TrailCB;
using renderer::TerrainObjectCB;
using renderer::TerrainLayerGpu;
using renderer::WaterVertex;
using renderer::WaterCB;
using renderer::WaterEffectParams;
using renderer::RenderPassHandles;
using renderer::ShadowCascade;
using renderer::PunctualShadowView;
using renderer::LightCookieView;
using renderer::MakeJitteredProjection;
using renderer::MakeJitteredViewProjection;
using renderer::MakeCameraFrameCB;
using renderer::kClusterGridX;
using renderer::kClusterGridY;
using renderer::kClusterGridZ;
using renderer::kClusterCount;
using renderer::kMaxLightsPerCluster;
using renderer::kClusterStride;
using renderer::kMaxPunctualLights;
using renderer::kMaxPunctualShadows;
using renderer::kMaxLightCookies;
using renderer::kLightCookieTileSize;
using renderer::kLightCookieAtlasCols;
using renderer::kLightCookieAtlasRows;
using renderer::kLightCookieAtlasWidth;
using renderer::kLightCookieAtlasHeight;
using renderer::kMaxLegacyPunctualLights;
using renderer::kLegacySpotSlotBase;
using renderer::kMaxLegacyShapedLights;
using renderer::kLegacyShapedLightStride;
using renderer::kExposureHistogramBins;
using renderer::kBloomMipCount;
using renderer::MakeScreenPostProcCB;
using renderer::kDecalFlagReceiverFilter;
using renderer::kParticleFxDistortion;
using renderer::kParticleFxSixWay;
using renderer::kParticleFxMotionVector;
using renderer::kParticleFxReceiveShadow;
using renderer::kParticleFxVolumetric;
using renderer::kParticleFxPremultiplied;
using renderer::kParticleFxSrgbTexture;
using renderer::kParticleFxDistortionMap;
using renderer::kParticleFxPunctual;
using renderer::kParticleFxSixWayMaps;
using renderer::kParticleFxSixWayColorMaps;
using renderer::kParticleFxAdditive;
using renderer::kParticleAlphaShift;
using renderer::kParticleAlphaMask;
using renderer::kParticleConstantSlot;
using renderer::kTrailFlagSrgbTexture;
using renderer::kOutsideGraphAccesses;
using renderer::SubmitCounted;
using renderer::SubmitCountedShadow;
/// @note UserRenderPassDesc — Script / Scene が RenderGraph へ追加したい 1 パス分の宣言。
/// @note reads/writes は RenderGraph 上の論理リソース名、execute は実際の描画処理。execute は
/// @note RenderSystem が保持する RenderPassContext を渡して呼ぶため、Script 側は renderer/resources/handles を参照できる。
struct UserRenderPassDesc {
    std::string name;
    UserRenderPassInjectionPoint injectionPoint = UserRenderPassInjectionPoint::AfterTransparent;
    std::vector<renderer::RenderGraph::ResourceAccess> accesses;
    std::function<void(RenderPassContext&)> execute;
    bool allowCulling = true;
};


struct RenderPassContext : renderer::RenderPassContext {
    Scene& scene;
    const physics::World* physicsWorld = nullptr;
    const RenderSystemUIOptions* uiOptions = nullptr;
    RenderPassContext(Scene& source, renderer::IRenderer& device, renderer::ResourceManager& manager,
                      const renderer::Camera& view, const renderer::RenderSettings& options,
                      renderer::ResourceHandle<renderer::RenderTargetTag> target,
                      fbzz::LayerMask mask, renderer::RenderPassHandles& passHandles)
        : renderer::RenderPassContext{ {}, device, manager, view, options, target, mask, passHandles }, scene(source) {}
};
/// @note スキンメッシュ描画に使う b3 パレットを解決する。優先順位:
/// @note 1. AnimatorComponent が評価したパレット (アニメーション中)
/// @note 2. Model のリファレンスポーズ (無アニメ時の既定。Unity / Unreal と同じ考え方)
/// @note 3. 単位行列 (スケルトン未解決時のみ。本来は到達しない)
/// @note 以前は 2 が無く、AnimatorComponent が無いだけで単位行列パレットが使われていた。
/// @note 単位行列は頂点がモデル空間そのままのアセットでしか正しい姿勢にならず、ノード
/// @note 階層にバインド変換を持つアセット (Blender 由来など) は倒れて描画された。
inline renderer::ResourceHandle<renderer::ConstantBufferTag> ResolveSkinningCB(
    const renderer::ResourceHandle<renderer::ConstantBufferTag>& animatorPalette,
    const asset::Model* model,
    const renderer::ResourceHandle<renderer::ConstantBufferTag>& identityFallback)
{
    if (animatorPalette.IsValid()) return animatorPalette;
    if (model && model->referencePoseCB.IsValid()) return model->referencePoseCB;
    return identityFallback;
}


}
