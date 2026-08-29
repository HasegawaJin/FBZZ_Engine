/// @file    PreviewPanel.cpp
/// @brief   Animation / Material / VFX の共通ルーター。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY: 各アセット種別の描画実装は分離し、ここでは拡張子判定とパネルのライフサイクルだけを扱う。
#include <Editor/Panels/PreviewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/PreviewPanelRenderers.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace fbzz::editor {

namespace {

std::string LowerPreviewExtension(std::string_view path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string extension(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

} // namespace

bool PreviewPanel::Supports(std::string_view extension) const
{
    std::string normalized(extension);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized == ".anim" || normalized == ".animcontroller" ||
           normalized == ".fbx" || normalized == ".obj" || normalized == ".gltf" ||
           normalized == ".glb" || normalized == ".fzasset" || normalized == ".mat" ||
           normalized == ".vfx" || normalized == ".mask" || normalized == ".maskpreset";
}

bool PreviewPanel::DrawPreview(EditorContext& ctx,
                               std::string_view assetPath,
                               float previewHeight)
{
    const std::string extension = LowerPreviewExtension(assetPath);
    if (extension == ".mat") {
        const auto handle = asset::AssetManager::LoadMaterial(NormalizeAssetPath(std::string(assetPath)));
        const auto* material = asset::AssetManager::GetMaterial(handle);
        return material != nullptr && DrawMaterialPreviewWidget(ctx, *material, previewHeight);
    }
    if (extension == ".mask" || extension == ".maskpreset")
        return m_animationMaskPreview.DrawPreview(ctx, assetPath, previewHeight);
    return DrawAnimationPreviewWidget(ctx, previewHeight);
}

void PreviewPanel::OnRenderContent(EditorContext& ctx)
{
    const std::string extension = LowerPreviewExtension(ctx.selectedAssetPath);
    if (extension == ".mat" || extension == ".vfx" ||
        extension == ".mask" || extension == ".maskpreset") {
        const float previewHeight =
            (std::max)(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing(), 120.0f);
        if (!DrawPreview(ctx, ctx.selectedAssetPath, previewHeight))
            ImGui::TextDisabled("Select a supported asset to preview.");
        return;
    }

    DrawAnimationPreviewPanelContent(ctx);
}

} // namespace fbzz::editor
