// FBZZ Engine
// RenderPasses/GeometryPasses.hpp | fbzz::scene
// ジオメトリ描画パスの宣言とインラインヘルパー
#pragma once
#include "RenderPassContext.hpp"
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Physics/Layer.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace fbzz::scene {

class  GameObject;
struct MaterialComponent;

// シャドウマップ解像度。RenderSystem の初期化と各パスで参照する。
constexpr uint32_t kShadowMapSize = 8192u;

// パーティクル最大描画数。RenderSystem の VB/IB 確保と ParticlePass で共有する。
constexpr int kMaxParticleDraw = 1000;

// パーティクルビルボード頂点。Particle.hlsl の ParticleVSIn と一致させること。
struct ParticleVertex {
    float center[3];  // POSITION   12 bytes
    float uv[2];      // TEXCOORD0   8 bytes
    float color[4];   // COLOR       16 bytes
    float size;       // TEXCOORD1    4 bytes
};                    // 40 bytes

// ---- パス宣言 ---------------------------------------------------------------
void ExecuteShadowPass                     (RenderPassContext& ctx);
void ExecuteForwardPasses                  (RenderPassContext& ctx);
void ExecuteGBufferPass                    (RenderPassContext& ctx);
void ExecuteDeferredDepthCopyPass          (RenderPassContext& ctx);
void ExecuteDeferredLightingPass           (RenderPassContext& ctx);
void ExecuteDeferredSkinnedForwardPass     (RenderPassContext& ctx);
void ExecuteDeferredForwardTransparentPass (RenderPassContext& ctx);
void ExecuteSkyPass                        (RenderPassContext& ctx);
void ExecuteParticlePass                   (RenderPassContext& ctx);

// ---- ヘルパー宣言 (定義は GeometryPassHelpers.cpp) -------------------------
renderer::Material* SyncMaterial(
    MaterialComponent& mc, renderer::ResourceManager& resources);

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    renderer::BlendMode        blend,
    bool                       doubleSided);

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask);
bool IsSurfaceMaterialShader(std::string_view path);

} // namespace fbzz::scene
