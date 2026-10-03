/// @file    RenderSceneExtractor.hpp
/// @brief   Scene から描画候補とスキニング資源を値へ抽出する境界。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Engine/Renderer/RenderScene.hpp>
#include <memory>

namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::scene {
class Scene;
struct RenderPassContext;
/// @brief 同じフレームの複数ビューでメッシュ候補だけを共有する一時領域。
/// @note 呼び出し元がフレーム末尾で破棄し、描画中に Scene を変更するスクリプトがある場合は使わない。
struct RenderFrameGeometryCache {
    const Scene* scene = nullptr;
    const renderer::ResourceManager* resources = nullptr;
    uint64_t frameStamp = 0;
    uint64_t resetVersion = 0;
    bool rayTracingInputs = false;
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette;
    std::shared_ptr<const renderer::RenderScene> geometry;
};
/// @return 品質要求用の投影直径 [px]。境界・射影が不明なら 0 (最高品質)。
[[nodiscard]] float EstimateRenderTexturePixels(const renderer::RenderObject& object,
    const renderer::RenderMeshItem& item, const math::Vector3& cameraPosition,
    float projectionScaleY, uint32_t height, bool orthographic);

/// @note 更新・スキニング完了後に呼ぶ。GPU 操作と材質ロードを行わず、全レイヤーの候補を抽出する。
/// @param rayTracingInputs 実験 RT の実行を許可したビューだけ true。既定では RT 専用の LOD 収集を行わない。
[[nodiscard]] renderer::RenderScene ExtractRenderSceneGeometry(
    Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette = {}, bool rayTracingInputs = false);

/// @note 材質・テクスチャを解決してから const としてパスへ公開する。GPU スキニング完了後に呼ぶ。
void ExtractRenderScene(RenderPassContext& ctx, RenderFrameGeometryCache* frameGeometry = nullptr);

} /// @note namespace fbzz::scene
