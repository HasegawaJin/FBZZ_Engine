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
    uint32_t objectIndex = 0; ///< この描画項目が参照する RenderScene のオブジェクト配列の添字。
    uint32_t materialSlot = 0; ///< 描画に使う元コンポーネントの材質スロット番号。
    uint32_t sourceSubmesh = 0; ///< 元モデルのサブメッシュ番号。
    renderer::ResourceHandle<renderer::BufferTag> vertexBuffer; ///< ResourceManager が所有する描画用の元頂点バッファ。
    renderer::ResourceHandle<renderer::BufferTag> indexBuffer; ///< ResourceManager が所有する描画用インデックスバッファ。
    renderer::ResourceHandle<renderer::BufferTag> skinningVertexBuffer; ///< GPU スキニングへ渡す変形前の頂点バッファ。
    renderer::ResourceHandle<renderer::BufferTag> deformedVertexBuffer; ///< GPU スキニングが生成した変形済み頂点バッファ。
    std::shared_ptr<renderer::PreparedSkinningTicket> preparedSkinning; ///< 変形結果の公開を確認するスキニング処理の共有チケット。
    uint32_t preparedSkinningWrite = UINT32_MAX; ///< チケット内の書き込み結果番号。UINT32_MAX は未指定。
    uint32_t indexCount = 0; ///< 描画に使うインデックス要素数。
    uint32_t vertexCount = 0; ///< 描画に使う頂点要素数。
    uint64_t vertexContentVersion = 0; ///< 元頂点バッファの内容を照合する更新番号。
    uint64_t indexContentVersion = 0; ///< インデックスバッファの内容を照合する更新番号。
    uint64_t deformedContentVersion = 0; ///< 変形済み頂点バッファの内容を照合する更新番号。
    uint32_t vertexStride = 0; ///< 頂点バッファ内の隣接要素の間隔 [byte]。
    uint32_t vertexPositionOffset = 0; ///< 各頂点の先頭から位置属性までのオフセット [byte]。
    bool reliableOccluder = false; ///< 遮蔽の判定に使える形状を持つか。
    math::Vector3 boundsCenter{}; ///< カリング用境界球のワールド中心 [m]。
    float boundsRadius = 0; ///< カリング用境界球のワールド半径 [m]。
    float occluderRadius = 0; ///< 遮蔽用境界球のワールド半径 [m]。0 なら半径なし。
    std::shared_ptr<const renderer::MeshletClusterData> clusterLod; ///< 描画項目が参照する不変のメッシュレットクラスタ LOD データ。
};
/// @brief 同じフレームの複数ビューでメッシュ候補だけを共有する一時領域。
/// @note 呼び出し元がフレーム末尾で破棄し、描画中に Scene を変更するスクリプトがある場合は使わない。
/// @note 退役後も各 snapshot が共通姿勢の所有権を保持する。材質・選択・変形入力はビューごとに解決する。
/// @see Docs/design/runtime-workset-optimization.md
struct RenderFrameGeometryCache {
    const Scene* scene = nullptr; ///< キャッシュが対象とする Scene の非所有参照。
    const renderer::ResourceManager* resources = nullptr; ///< ハンドルの所有元を照合する資源管理器の非所有参照。
    uint64_t frameStamp = 0; ///< 同一フレームのビューに限定して再利用するための描画フレーム番号。
    uint64_t resetVersion = 0; ///< 資源管理器のリセット後に古いハンドルを再利用しないための世代番号。
    bool rayTracingInputs = false; ///< レイ描画専用のメッシュ入力を含めて抽出したか。
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette; ///< 骨変換を持たない場合に使う単位行列パレットの資源ハンドル。
    std::shared_ptr<const renderer::RenderScene> geometry; ///< ビュー間で共有する不変のジオメトリスナップショット。
    std::vector<RenderFrameGeometryItem> itemGeometry; ///< 不変の姿勢は geometry が所有する。材質と選択状態は各ビューで求める。
    /// @note 融合抽出の最初のビューが静的メッシュだけを扱うラスタライズなら、次のキャッシュヒットまで元データを保持し、安定した形状項目だけを読む。
    /// @note スキンドメッシュを含むビューとレイ描画では、ビューごとの更新前に形状行を退避する。
    std::shared_ptr<const renderer::RenderScene> deferredItemGeometry; ///< 形状行を遅延で取り出す場合に保持する元のスナップショット。
    const renderer::RenderScene* itemGeometryOwner = nullptr; ///< どちらの形状表現も所有者と非ゼロの公開番号で照合し、所有者が不明または変化していれば再構築する。
    uint64_t itemGeometrySerial = 0; ///< 形状行の元となったスナップショットの公開番号。0 なら未設定。
    uint64_t staticGeometryRevision = 0; ///< 静的メッシュ入力を構築した版。異なる版の抽出結果は再利用しない。
    const renderer::RenderScene* skinnedObjectRangeOwner = nullptr; ///< スキンドメッシュの半開区間は所有者と公開番号がともに一致するときだけ有効。不明なら全件を調べる。
    uint64_t skinnedObjectRangeSerial = 0; ///< スキンドオブジェクト範囲を得たスナップショットの公開番号。
    size_t skinnedObjectBegin = 0; ///< RenderScene のオブジェクト配列でスキンドメッシュが始まる添字。
    size_t skinnedObjectEnd = 0; ///< オブジェクト配列でスキンドメッシュの末尾の次を指す添字。
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
/// @param[in] rayTracingInputs 実験 RT の実行を許可したビューだけ true。既定では RT 専用の LOD 収集を行わない。
[[nodiscard]] FBZZ_ENGINE_API renderer::RenderScene ExtractRenderSceneGeometry(
    Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette = {}, bool rayTracingInputs = false);

/// @note 材質・テクスチャを解決してから const としてパスへ公開する。GPU スキニング完了後に呼ぶ。
FBZZ_ENGINE_API void ExtractRenderScene(RenderPassContext& ctx, RenderFrameGeometryCache* frameGeometry = nullptr);

/// @note 成功済み ticket の参照だけを COW 完成し、旧 geometry/material と旧 snapshot を保持する。
/// @return 差替前の所有 snapshot。callback handler は返値を handler の外まで退役保持する。
[[nodiscard]] FBZZ_ENGINE_API std::shared_ptr<const renderer::RenderScene> CompletePreparedSkinningSnapshot(RenderPassContext& ctx);

}
