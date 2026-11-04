/// @file    RenderSceneExtractor.hpp
/// @brief   Scene から描画候補とスキニング資源を値へ抽出する境界。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Renderer/RenderScene.hpp>
#include <cstddef>
#include <memory>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene {
class Scene;
struct EntityID;
struct RenderPassContext;
/// @brief Camera-independent mesh inputs retained between views of one frame without material payloads.
/// @note Each frame validates current inputs; unchanged static transforms and bounds may reuse private value data across frames.
struct RenderFrameGeometryItem {
    uint32_t objectIndex = 0;
    uint32_t materialSlot = 0;
    uint32_t sourceSubmesh = 0;
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer, indexBuffer, skinningVertexBuffer, deformedVertexBuffer;
    std::shared_ptr<renderer::PreparedSkinningTicket> preparedSkinning;
    uint32_t preparedSkinningWrite = UINT32_MAX;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    uint64_t vertexContentVersion = 0;
    uint64_t indexContentVersion = 0;
    uint64_t deformedContentVersion = 0;
    uint32_t vertexStride = 0;
    uint32_t vertexPositionOffset = 0;
    bool reliableOccluder = false;
    math::Vector3 boundsCenter{};
    float boundsRadius = 0;
};
/// @brief 同じフレームの複数ビューでメッシュ候補だけを共有する一時領域。
/// @note 呼び出し元がフレーム末尾で破棄し、描画中に Scene を変更するスクリプトがある場合は使わない。
/// @note 退役後も各 snapshot が共通姿勢の所有権を保持する。材質・選択・変形入力はビューごとに解決する。
/// @see Docs/design/runtime-workset-optimization.md
struct RenderFrameGeometryCache {
    const Scene* scene = nullptr;
    const renderer::ResourceManager* resources = nullptr;
    uint64_t frameStamp = 0;
    uint64_t resetVersion = 0;
    bool rayTracingInputs = false;
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette;
    std::shared_ptr<const renderer::RenderScene> geometry;
    /// @note geometry owns immutable object poses; material/selection state never enters the compact rows.
    std::vector<RenderFrameGeometryItem> itemGeometry;
    /// @note In fused mode a static-only Raster first view owns the deferred source until a cache hit; only stable geometry fields are read.
    /// @note Mixed/skinned and Ray views capture compact rows eagerly before per-view mesh-input updates.
    std::shared_ptr<const renderer::RenderScene> deferredItemGeometry;
    /// @note Both item representations belong to this exact nonzero geometry publication; unknown/replaced owners rebuild.
    const renderer::RenderScene* itemGeometryOwner = nullptr;
    uint64_t itemGeometrySerial = 0;
    uint64_t staticGeometryRevision = 0;
    /// @note Only the matching immutable geometry owner certifies this half-open skinned-object range; unknown metadata keeps full discovery.
    const renderer::RenderScene* skinnedObjectRangeOwner = nullptr;
    uint64_t skinnedObjectRangeSerial = 0;
    size_t skinnedObjectBegin = 0;
    size_t skinnedObjectEnd = 0;
};
/// @note Call after a MeshRenderer write on the Scene update/extraction thread; invalidates retained geometry without retaining the component.
/// @note Direct field writes remain supported through current-input validation at the next geometry build.
/// @see Docs/design/static-mesh-render-reuse.md
FBZZ_ENGINE_API void NotifyMeshRendererChanged(Scene& scene, EntityID entity);
/// @return 品質要求用の投影直径 [px]。境界・射影が不明なら 0 (最高品質)。
[[nodiscard]] FBZZ_ENGINE_API float EstimateRenderTexturePixels(const renderer::RenderObject& object,
    const renderer::RenderMeshItem& item, const math::Vector3& cameraPosition,
    float projectionScaleY, uint32_t height, bool orthographic);

/// @note 更新・スキニング完了後に呼ぶ。GPU 操作と材質ロードを行わず、全レイヤーの候補を抽出する。
/// @param rayTracingInputs 実験 RT の実行を許可したビューだけ true。既定では RT 専用の LOD 収集を行わない。
[[nodiscard]] FBZZ_ENGINE_API renderer::RenderScene ExtractRenderSceneGeometry(
    Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette = {}, bool rayTracingInputs = false);

/// @note 材質・テクスチャを解決してから const としてパスへ公開する。GPU スキニング完了後に呼ぶ。
FBZZ_ENGINE_API void ExtractRenderScene(RenderPassContext& ctx, RenderFrameGeometryCache* frameGeometry = nullptr);

/// @note 成功済み ticket の参照だけを COW 完成し、旧 geometry/material と旧 snapshot を保持する。
/// @return 差替前の所有 snapshot。callback handler は返値を handler の外まで退役保持する。
[[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<const renderer::RenderScene> CompletePreparedSkinningSnapshot(RenderPassContext& ctx);

} /// @note namespace fbzz::scene
