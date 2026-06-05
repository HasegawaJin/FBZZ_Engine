// FBZZ Engine
// RenderPasses/GeometryPasses.hpp | fbzz::scene
// ジオメトリ描画パスの宣言とインラインヘルパー
#pragma once
#include <Engine/Scene/Systems/RenderPassContext.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include "Engine/Scene/Transform.hpp"
#include <Math/Frustum.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
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

// HDR レンダーターゲットのクリアカラー。Forward / Deferred 両パスで共有する。
inline constexpr math::Vector4 kHdrClearColor = { 0.005f, 0.005f, 0.02f, 1.0f };

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

// ── カリング ヘルパー ────────────────────────────────────────────────────────

// ワールド空間バウンディング球 (カリング用)
struct WorldBounds {
    math::Vector3 center;
    float         radius;
};

// メッシュのローカルバウンディング球をワールド空間に変換する。
// boundsRadius が 0 のメッシュ (ComputeBounds 未実行) は半径 0 を返す。
WorldBounds ComputeWorldBounds(const Transform& tf, const renderer::Mesh& mesh);

// バウンディング球が視錐台と交差するかを判定する。
// false → フラスタム外確定 → 描画スキップ可能。
bool IsVisibleInFrustum(const math::Frustum& frustum,
                        const Transform& tf,
                        const renderer::Mesh& mesh);

// Deferred GBuffer に書き込めないエフェクト系シェーダーかどうかを判定する。
// RimLight / Toon / Subsurface / Anisotropic / Dissolve / Unlit は
// 標準 GBuffer (albedo / normal / roughness / metallic) に収まらない
// 独自ライティング計算を持つため、Deferred でも Forward パスで描画する。
bool IsForwardOnlyShader(std::string_view shaderPath);

} // namespace fbzz::scene
