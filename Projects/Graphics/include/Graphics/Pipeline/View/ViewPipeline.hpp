/// @file    ViewPipeline.hpp
/// @brief   Scene に依存しない描画構成とホスト拡張の挿入点。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Api.hpp>
#include <Graphics/Pipeline/Resources/RenderResources.hpp>
#include <Graphics/Pipeline/View/ResolvedRenderPlan.hpp>
#include <functional>
#include <vector>

namespace fbzz::renderer {
struct ViewPipelineOptions {
    ResolvedRenderPlan renderPlan; ///< このビューの設定と利用可能な資源から決定した描画計画。
    std::vector<uint32_t> customAfterOpaqueIndices; ///< 不透明描画の後に挿入するカスタム効果の添字列。
    std::vector<uint32_t> customSceneHdrIndices; ///< シーンの HDR 色に適用するカスタム効果の添字列。
    std::vector<uint32_t> customPostProcessIndices; ///< ポストプロセス段階に挿入するカスタム効果の添字列。
};
/// @note 登録中だけ同期的に呼ぶ。ホストは Scene の参照を自身のパスに閉じ込める。
/// @note AfterOpaque は不透明合成後・半透明と水の前、AfterTransparent は VFX の合成後。
struct ViewPipelineExtensions {
    std::function<void()> begin; ///< ビューの描画パスの登録を開始するときに呼ぶフック。
    std::function<void()> setup; ///< 描画パス構築時にホスト側の設定を登録するフック。
    std::function<void(UserRenderPassInjectionPoint)> userPasses; ///< 指定した挿入位置にユーザーパスを登録するフック。
    std::function<void()> depthDebug; ///< 深度診断表示のパスを登録するフック。
    std::function<void()> selectionMask; ///< 選択された物体のマスク描画を登録するフック。
    std::function<void(PassResources&)> selectionOutline; ///< 選択マスクから輪郭線を合成する処理のフック。
    std::function<void(const char*)> overlayDebug; ///< 指定した描画段階に重ねる診断表示のフック。
    std::function<void()> ui; ///< ビューに重ねる UI の描画を登録するフック。
};
/// @pre context とビューは登録されたパスの実行終了まで生存すること。
/// @note options/extensions は保持しない。実行後は BeginBuild でホストのコールバックを破棄する。
    /// @note 無効な Plan は登録しない。Reference Path は Raster の照明・TAA を使用しない。
FBZZ_GRAPHICS_API void BuildViewPipeline(RenderPipeline& pipeline, RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared,
    const ViewPipelineOptions& options, const ViewPipelineExtensions& extensions = {});
}
