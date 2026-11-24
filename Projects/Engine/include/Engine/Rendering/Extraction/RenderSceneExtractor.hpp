/// @file    RenderSceneExtractor.hpp
/// @brief   Scene から描画候補とスキニング資源を値へ抽出する境界。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Engine/Api.hpp>
#include <Graphics/Scene/RenderScene.hpp>
#include <cstddef>
#include <memory>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene {
class Scene;
struct EntityID;
struct RenderPassContext;
/// @brief 同一フレームのビュー間で共有する、カメラや材質に依存しないメッシュ入力。
/// @note フレームごとに入力を照合し、変化のない静的メッシュの変換と境界は前フレームの計算値を再利用できる。
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
    float occluderRadius = 0;
    std::shared_ptr<const renderer::MeshletClusterData> clusterLod;
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
    /// @note 不変の姿勢は geometry が所有する。材質と選択状態は各ビューで求める。
    std::vector<RenderFrameGeometryItem> itemGeometry;
    /// @note 融合抽出の最初のビューが静的メッシュだけを扱うラスタライズなら、次のキャッシュヒットまで元データを保持し、安定した形状項目だけを読む。
    /// @note スキンドメッシュを含むビューとレイ描画では、ビューごとの更新前に形状行を退避する。
    std::shared_ptr<const renderer::RenderScene> deferredItemGeometry;
    /// @note どちらの形状表現も所有者と非ゼロの公開番号で照合し、所有者が不明または変化していれば再構築する。
    const renderer::RenderScene* itemGeometryOwner = nullptr;
    uint64_t itemGeometrySerial = 0;
    uint64_t staticGeometryRevision = 0;
    /// @note スキンドメッシュの半開区間は所有者と公開番号がともに一致するときだけ有効。不明なら全件を調べる。
    const renderer::RenderScene* skinnedObjectRangeOwner = nullptr;
    uint64_t skinnedObjectRangeSerial = 0;
    size_t skinnedObjectBegin = 0;
    size_t skinnedObjectEnd = 0;
};
/// @note シーンの更新・抽出スレッドで MeshRenderer への書き込み後に呼ぶ。コンポーネントへの参照を保持せずにキャッシュを無効化する。
/// @note 直接のフィールド更新も、次の形状構築時に現在の入力を照合して検出する。
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

}
