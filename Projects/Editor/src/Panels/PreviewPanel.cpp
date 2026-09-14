/// @file    PreviewPanel.cpp
/// @brief   Animation / Material の共通ルーター。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// WHY: 各アセット種別の描画実装は分離し、ここでは拡張子判定とパネルのライフサイクルだけを扱う。
/// WHY .vfx を扱わないか: 専用の隔離プレビューを作らず Prefab 編集モードで開く判断
/// (Docs/design/vfx-prefab.md §8.2)。入口は Inspector の .vfx 分岐が持つ。
#include <Editor/Panels/PreviewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/PreviewPanelRenderers.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
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
           normalized == ".mask" || normalized == ".maskpreset";
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
    if (extension == ".mat" || extension == ".mask" || extension == ".maskpreset") {
        const float previewHeight =
            (std::max)(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing(), 120.0f);
        if (!DrawPreview(ctx, ctx.selectedAssetPath, previewHeight))
            widgets::EmptyState(icons::Or(icons::kView, nullptr),
                                LOCT("Nothing to preview"),
                                LOCT("Select a material, model, animation or avatar mask."));
        return;
    }

    DrawAnimationPreviewPanelContent(ctx);
}

} // namespace fbzz::editor
