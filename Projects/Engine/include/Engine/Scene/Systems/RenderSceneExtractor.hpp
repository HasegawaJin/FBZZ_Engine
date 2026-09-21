/// @file    RenderSceneExtractor.hpp
/// @brief   Scene から描画候補とスキニング資源を値へ抽出する境界。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#pragma once
#include <Engine/Renderer/RenderScene.hpp>

namespace fbzz::scene {
class Scene;
struct RenderPassContext;

/// @note 更新・スキニング完了後に呼ぶ。GPU 操作と材質ロードを行わず、全レイヤーの候補を抽出する。
[[nodiscard]] renderer::RenderScene ExtractRenderSceneGeometry(
    Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette = {});

/// @note 材質・テクスチャを解決してから const としてパスへ公開する。GPU スキニング完了後に呼ぶ。
void ExtractRenderScene(RenderPassContext& ctx);

} /// @note namespace fbzz::scene
