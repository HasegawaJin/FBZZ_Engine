// FBZZ Engine
// VFXTemplateThumbnailBaker.cpp | fbzz::editor
// Template サムネイル PNG 一括生成の実装
#include <Editor/VFXEditor/Services/VFXTemplateThumbnailBaker.hpp>

#include <Editor/EditorContext.hpp>
#include <Editor/VFXEditor/Services/VFXPreviewController.hpp>
#include <Editor/VFXEditor/Services/VFXTemplateCatalog.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace fbzz::editor {

void VFXTemplateThumbnailBaker::Begin(std::vector<std::string> templatePaths,
                                      bool overwriteExisting)
{
    queue = std::move(templatePaths);
    overwrite = overwriteExisting;
    index = 0;
    baked = skipped = failed = 0;
    m_stage = 0;
    m_settleFrames = 0;
    running = !queue.empty();
    message = running ? "サムネイルを生成中..." : "対象の Template がありません";
}


void VFXTemplateThumbnailBaker::Cancel()
{
    if (!running) return;
    running = false;
    restoreRequested = true;
    message = "中断しました (" + std::to_string(baked) + " 件生成)";
}


void VFXTemplateThumbnailBaker::Tick(EditorContext& ctx, VFXPreviewController& preview)
{
    if (!running) return;
    namespace fs = std::filesystem;

    auto* previewObject = ctx.vfxPreviewScene != nullptr
        ? ctx.vfxPreviewScene->GetGameObject(preview.graphEntity) : nullptr;
    auto* instance = previewObject != nullptr
        ? previewObject->GetComponent<scene::VFXGraphComponent>() : nullptr;
    if (instance == nullptr || ctx.renderer == nullptr || ctx.resources == nullptr
        || !preview.renderTarget.IsValid()) {
        running = false;
        restoreRequested = true;
        message = "プレビューが無いため中断しました";
        return;
    }
    // 焼いている間もプレビューは描かれ続ける必要がある (RenderTarget を読むため)。
    preview.renderRequested = true;
    instance->editorPreviewFrame = Time::frameCount;

    if (index >= queue.size()) {
        running = false;
        restoreRequested = true;
        message = std::to_string(baked) + " 件生成 / " + std::to_string(skipped)
                + " 件スキップ / " + std::to_string(failed) + " 件失敗";
        return;
    }

    const std::string& templatePath = queue[index];
    const std::string outputPath = TemplateThumbnailPath(templatePath);

    if (m_stage == 0) {
        std::error_code errorCode;
        if (!overwrite && fs::is_regular_file(fs::path(outputPath), errorCode)) {
            ++skipped;
            ++index;
            return;
        }
        errorCode.clear();
        asset::VFXGraphAsset graph;
        if (!asset::ParseVFXGraphAsset(templatePath, graph, nullptr)) {
            ++failed;
            ++index;
            return;
        }
        // authoringGraph は「未保存グラフをプレビューへ流し込む」既存経路。
        // ここへ Template を差し込めば、ファイルを開き直さずに絵を作れる。
        const float scrubTime = asset::ResolveVFXThumbnailTime(graph);
        instance->authoringGraph = std::make_shared<const asset::VFXGraphAsset>(std::move(graph));
        instance->authoringDirtyNodeId = -1; // グラフ全体が別物なので作り直させる
        instance->authoringRevision = instance->authoringRevision + 1;
        instance->Restart();
        instance->editorScrubTime = scrubTime;
        // scrub 要求を出したフレームの RenderTarget にはまだ前の時刻が入っている。
        // 粒子の再シミュレートも挟むため、Sequence Exporter より 1 枚多く待つ。
        m_settleFrames = 2;
        m_stage = 1;
        return;
    }

    if (m_stage == 1) {
        if (m_settleFrames > 0) { --m_settleFrames; return; }
        m_stage = 2;
        return;
    }

    std::vector<std::uint8_t> png;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (ctx.renderer->CaptureRenderTargetToPng(preview.renderTarget, *ctx.resources,
                                               png, width, height)
        && !png.empty()) {
        std::error_code errorCode;
        fs::create_directories(fs::path(outputPath).parent_path(), errorCode);
        std::ofstream file(fs::path(outputPath), std::ios::binary);
        if (file) {
            file.write(reinterpret_cast<const char*>(png.data()),
                       static_cast<std::streamsize>(png.size()));
            ++baked;
        } else {
            ++failed;
        }
    } else {
        ++failed;
    }
    ++index;
    m_stage = 0;
    message = "サムネイルを生成中... (" + std::to_string(index) + "/"
            + std::to_string(queue.size()) + ")";
}

} // namespace fbzz::editor
