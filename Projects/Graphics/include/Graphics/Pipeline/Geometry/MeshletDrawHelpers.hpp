/// @file    MeshletDrawHelpers.hpp
/// @brief   Eligibility and draw preparation for static meshlet geometry.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/Culling/GpuHiZ.hpp>

#include <Graphics/Renderer/Commands/DrawCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Math/LinearAlgebra/Matrix4.hpp>
#include <cstdint>

namespace fbzz::renderer {

/// @brief Pass-specific meshlet shader and eligibility rules.
enum class MeshletDrawPass : uint8_t {
    GBUFFER,
    SHADOW,
    SHADOW_MASKED,
};

/// @note The unjittered color-view projection keeps the screen-error decision stable across TAA samples.
struct MeshletClusterView {
    math::Matrix4 viewProjection; ///< ワールド座標をこのビューのクリップ座標に変換する行列。
    uint32_t width; ///< 投影面積の評価に使うビューポートの幅（画素）。
    uint32_t height; ///< 投影面積の評価に使うビューポートの高さ（画素）。
    /// @note Negative means the skinning palette cannot provide a conservative bound.
    float skinDisplacement = -1.0f;
};

/// @note Indexed instance commands provide the visible ID range without GPU readback.
struct MeshletClusterInstances {
    ResourceHandle<StructuredBufferTag> rows; ///< クラスター描画で参照するインスタンス変換のバッファ。
    ResourceHandle<StructuredBufferTag> ids; ///< インスタンスの選別結果から元の変換を引くための ID 列。
    ResourceHandle<StructuredBufferTag> indexedArguments; ///< 選別後の描画数が入るインデックス描画用の間接引数バッファ。
    uint32_t argumentIndex = UINT32_MAX; ///< このバッチが使う間接引数の位置。
    uint32_t capacity = 0; ///< このバッチに渡せるインスタンス数の上限。
};

/// @brief Checks whether a static standard-surface item can use the meshlet path.
/// @param[in] item 解決済みのメッシュとマテリアルのスナップショット。
/// @param[in] skinned 描画対象のオブジェクトがスキニングを使う場合は true。
/// @param[in] pass 描画パスの種類。GBuffer は標準の 60 バイトの頂点形式を使い、影パスは位置だけを読む。
/// @return False for unsupported, small, deformed, custom, or shadow alpha-cutout inputs.
[[nodiscard]] FBZZ_GRAPHICS_API bool IsMeshletDrawCandidate(const RenderMeshItem& item, bool skinned,
                                          MeshletDrawPass pass);

/// @note A clustered color surface needs a matching depth cut and must bypass the full-resolution Hi-Z seed.
[[nodiscard]] bool CanUseGBufferClusterLod(const RenderObject& object, const RenderMeshItem& item);

/// @brief Prepares a mesh-shader DrawCall while leaving the input unchanged on failure.
/// @param[in] viewProjection 描画先のカメラまたは影ビューに対応するビュー射影行列。
/// @param[in] world 全メッシュレットのローカル境界をワールド空間へ移す行列。
/// @param[in] forceMesh 経路を明示的に比較するとき、オブジェクト全体が可視でもメッシュシェーダーを使う。
/// @return True when inOut is a fully configured direct mesh-shader draw.
/// @note Flat meshlets require at least 25 percent estimated clipping unless forced. Cluster LOD also handles fully visible geometry.
/// @note Callers update b1 and submit immediately after success so the shared b13 upload and draw stay ordered.
/// @see https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html DispatchMesh groups and mesh output limits.
[[nodiscard]] FBZZ_GRAPHICS_API bool TryPrepareMeshletDraw(ResourceManager& resources, IRenderer& renderer,
                                         const math::Matrix4& viewProjection,
                                         const math::Matrix4& world,
                                         MeshletDrawPass pass, const RenderMeshItem& item,
                                         bool skinned, DrawCall& inOut,
                                         bool frustumCullingEnabled = true,
                                         bool forceMesh = false,
                                         const MeshletClusterView* clusterView = nullptr,
                                         const GpuHiZ* hierarchy = nullptr);

struct RenderPassContext;
[[nodiscard]] ResourceHandle<ConstantBufferTag> MeshletWindFrame(RenderPassContext& ctx);
FBZZ_GRAPHICS_API void SubmitMeshletDraw(RenderPassContext& ctx, const DrawCall& draw, MeshletDrawPass pass);
[[nodiscard]] bool TrySubmitInstancedClusterDraw(RenderPassContext& ctx, const math::Matrix4& rasterViewProjection,
    const MeshletClusterView& view, const RenderMeshItem& item, MeshletDrawPass pass,
    const DrawCall& indexedDraw, const MeshletClusterInstances& instances);

} // namespace fbzz::renderer
