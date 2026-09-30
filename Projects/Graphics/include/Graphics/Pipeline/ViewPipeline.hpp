/// @file    ViewPipeline.hpp
/// @brief   Scene に依存しない描画構成とホスト拡張の挿入点。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#pragma once
#include <Graphics/Pipeline/RenderResources.hpp>
#include <Graphics/Pipeline/ResolvedRenderPlan.hpp>
#include <functional>
#include <vector>

namespace fbzz::renderer {
struct ViewPipelineOptions {
    ResolvedRenderPlan renderPlan;
    std::vector<uint32_t> customAfterOpaqueIndices;
    std::vector<uint32_t> customSceneHdrIndices;
    std::vector<uint32_t> customPostProcessIndices;
};
/// @note 登録中だけ同期的に呼ぶ。ホストは Scene の参照を自身のパスに閉じ込める。
/// @note パス順は従来の挿入点を維持する。段境界の変更は別途行う。
struct ViewPipelineExtensions {
    std::function<void()> begin;
    std::function<void()> setup;
    std::function<void(UserRenderPassInjectionPoint)> userPasses;
    std::function<void()> depthDebug;
    std::function<void()> selectionMask;
    std::function<void(PassResources&)> selectionOutline;
    std::function<void(const char*)> overlayDebug;
    std::function<void()> ui;
};
/// @pre context とビューは登録されたパスの実行終了まで生存すること。
/// @note options/extensions は保持しない。実行後は BeginBuild でホストのコールバックを破棄する。
/// @note 無効な Plan は登録しない。現段階で対応する実効モードは Raster のみ。
void BuildViewPipeline(RenderPipeline& pipeline, RenderPassContext& context,
    RenderViewResources& view, RenderSharedResources& shared,
    const ViewPipelineOptions& options, const ViewPipelineExtensions& extensions = {});
}
