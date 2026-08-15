// FBZZ Engine
// VFXSequenceExporter.cpp | fbzz::editor
// 決定論スクラブによる PNG 連番書き出しの実装
#include <Editor/VFXEditor/Services/VFXSequenceExporter.hpp>

#include <Editor/EditorContext.hpp>
#include <Engine/Asset/FlipbookAtlasBaker.hpp>
#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <Editor/VFXEditor/Services/VFXPreviewController.hpp>
#include <Engine/Renderer/IRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <cstdio>
#include <fstream>

namespace fbzz::editor {

void VFXSequenceExporter::Tick(EditorContext& ctx, VFXPreviewController& preview)
{
    if (!running) return;
    auto* previewObject = ctx.vfxPreviewScene != nullptr
        ? ctx.vfxPreviewScene->GetGameObject(preview.graphEntity) : nullptr;
    auto* instance = previewObject != nullptr
        ? previewObject->GetComponent<scene::VFXGraphComponent>() : nullptr;
    if (instance == nullptr || ctx.renderer == nullptr || ctx.resources == nullptr
        || !preview.renderTarget.IsValid()) {
        running = false;
        message = "プレビューが無いため中断しました";
        return;
    }
    if (frame >= total) {
        running = false;
        message = std::to_string(total) + " frames -> " + directory;
        if (bakeAtlas) {
            asset::FlipbookAtlasBakeSettings settings;
            settings.columns = atlasColumns;
            settings.outputPath =
                (std::filesystem::path(directory) / atlasFileName).string();
            settings.overwrite = overwriteAtlas;
            settings.framePaths.reserve(static_cast<std::size_t>(total));
            for (int index = 0; index < total; ++index) {
                char name[32]{};
                std::snprintf(name, sizeof(name), "frame_%04d.png", index);
                settings.framePaths.push_back(
                    (std::filesystem::path(directory) / name).string());
            }
            const asset::FlipbookAtlasBakeResult result =
                asset::BakeFlipbookAtlas(settings);
            message += result.success
                ? "\n" + result.message + " -> " + result.outputPath
                : "\nAtlas生成失敗: " + result.message;
        }
        return;
    }

    // scrub 要求を出したフレームの RenderTarget にはまだ前の時刻が入っている。
    // 1 フレーム置いてから読み戻さないと、全カットが 1 フレームずつ前へずれる。
    if (settleFrames > 0) {
        --settleFrames;
        return;
    }

    std::vector<std::uint8_t> png;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (ctx.renderer->CaptureRenderTargetToPng(preview.renderTarget, *ctx.resources, png, width, height)
        && !png.empty()) {
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%04d.png", frame);
        const std::filesystem::path destination =
            std::filesystem::path(directory) / name;
        std::ofstream file(destination, std::ios::binary);
        if (file) file.write(reinterpret_cast<const char*>(png.data()),
                             static_cast<std::streamsize>(png.size()));
        else {
            running = false;
            message = "書き込みに失敗しました: " + destination.string();
            return;
        }
    } else {
        running = false;
        message = "RenderTargetの読み戻しに失敗しました: frame "
            + std::to_string(frame);
        return;
    }

    ++frame;
    if (frame < total) {
        instance->editorScrubTime = static_cast<float>(frame)
            / static_cast<float>((std::max)(fps, 1));
        settleFrames = 1;
    }
}

} // namespace fbzz::editor
