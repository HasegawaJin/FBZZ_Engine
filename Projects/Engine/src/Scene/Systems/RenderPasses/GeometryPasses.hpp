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
void ExecuteMeshTrailPass                  (RenderPassContext& ctx);
void ExecuteTrailPass                      (RenderPassContext& ctx);
void ExecuteParticlePass                   (RenderPassContext& ctx);
void ExecuteDecalPass                      (RenderPassContext& ctx);

// ---- ヘルパー宣言 (定義は GeometryPassHelpers.cpp) -------------------------
renderer::Material* SyncMaterial(
    MaterialComponent& mc, renderer::ResourceManager& resources, bool preferSkinnedFallback = false);

renderer::Material* GetFallbackMaterial(
    renderer::ResourceManager& resources, bool skinned);

const char* GetFallbackMaterialPath(bool skinned);

void LogSkinnedSurfaceFallbackWarningOnce(std::string_view shaderPath);

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

// Deferred GBuffer に書き込めないエフェクト系シェーダーかどうかをシェーダーパス名で判定する。
// 後方互換のため残す。新規呼び出しは IsForwardOnly(MaterialComponent) を使うこと。
bool IsForwardOnlyShader(std::string_view shaderPath);

// fzmat の render_path フィールドを優先し、"auto" の場合は名前チェックにフォールバックする。
// WHY: カスタムシェーダーはエンジンコードを触らず render_path = "forward"/"deferred" で
//      自分のレンダーパスを制御できるようにするため。
bool IsForwardOnly(const MaterialComponent& mc);

// static mesh 専用シェーダー (Surface) かどうかをシェーダーパス名で判定する。
// 後方互換のため残す。新規呼び出しは IsSurfaceMaterial(MaterialComponent) を使うこと。
bool IsSurfaceMaterialShader(std::string_view shaderPath);

// fzmat の mesh_type フィールドを優先し、"any" の場合はパス名チェックにフォールバックする。
// WHY: カスタムシェーダーはエンジンコードを触らず mesh_type = "surface"/"skinned"/"any" で
//      対応するメッシュタイプを宣言できるようにするため。
bool IsSurfaceMaterial(const MaterialComponent& mc);

} // namespace fbzz::scene
