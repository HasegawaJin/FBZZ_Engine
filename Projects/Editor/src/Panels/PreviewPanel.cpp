/// @file    PreviewPanel.cpp
/// @brief   Animation / Material の共通ルーター。
/// @author  Hasegawa Jin
/// @date    2026-08-19
///
/// 各アセット種別の描画実装は分離し、ここでは拡張子判定とパネルのライフサイクルだけを扱う。
/// @see Docs/design/vfx-prefab.md §8.2 .vfx は専用の隔離プレビューを作らず Prefab 編集モードで開く。
#include <Editor/Panels/PreviewPanel.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/PreviewPanelRenderers.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/EditorIcons.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/IcoImage.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Renderer/IImGuiRenderer.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
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
           normalized == ".mask" || normalized == ".maskpreset" || normalized == ".ico";
}

bool PreviewPanel::DrawPreview(EditorContext& ctx,
                               std::string_view assetPath,
                               float previewHeight)
{
    const std::string extension = LowerPreviewExtension(assetPath);
    if (extension == ".mat") {
        const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(NormalizeAssetPath(std::string(assetPath)));
        const auto* material = asset::AssetManager::Get<asset::MaterialAsset>(handle);
        return material != nullptr && m_materialPreview.Draw(ctx, *material, previewHeight);
    }
    if (extension == ".ico")
        return DrawIconPreview(ctx, assetPath, previewHeight);
    if (extension == ".mask" || extension == ".maskpreset")
        return m_animationMaskPreview.DrawPreview(ctx, assetPath, previewHeight);
    return DrawAnimationPreviewWidget(ctx, previewHeight);
}

bool PreviewPanel::DrawIconPreview(EditorContext& ctx, std::string_view assetPath, float previewHeight)
{
    if (!ctx.resources || !ctx.imguiRenderer) return false;

    const std::string path(assetPath);
    if (path != m_iconPreview.path) {
        if (m_iconPreview.texture.IsValid()) ctx.resources->Release(m_iconPreview.texture);
        m_iconPreview = {};
        m_iconPreview.path = path;
    }
    if (!m_iconPreview.resolved) {
        m_iconPreview.resolved = true;
        IcoImage image;
        if (DecodeIcoFile(util::FileSystem::PathFromUtf8(path), image, m_iconPreview.error) &&
            image.IsValid()) {
            m_iconPreview.width      = image.width;
            m_iconPreview.height     = image.height;
            m_iconPreview.frameCount = image.frameCount;
            m_iconPreview.texture    = ctx.resources->CreateTexture(
                image.rgba.data(), static_cast<uint32_t>(image.width),
                static_cast<uint32_t>(image.height));
        }
    }

    if (!m_iconPreview.texture.IsValid()) {
        ImGui::TextDisabled("%s", m_iconPreview.error.empty()
            ? "Cannot read this .ico" : m_iconPreview.error.c_str());
        return false;
    }

    void* rawId = ctx.imguiRenderer->GetImTextureID(m_iconPreview.texture, *ctx.resources);
    if (!rawId) return false;

    ImGui::TextDisabled("%d x %d  |  %d size%s in file",
                        m_iconPreview.width, m_iconPreview.height,
                        m_iconPreview.frameCount, m_iconPreview.frameCount == 1 ? "" : "s");

    const float side = (std::max)(
        (std::min)(ImGui::GetContentRegionAvail().x, previewHeight - ImGui::GetFrameHeight()),
        64.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({ side, side });

    /// @note アイコンは透明部分を持つのが前提なので、市松を敷いて «抜け» を見せる。
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    constexpr float kCell = 12.0f;
    drawList->AddRectFilled(origin, { origin.x + side, origin.y + side }, IM_COL32(58, 60, 66, 255), 4.0f);
    drawList->PushClipRect(origin, { origin.x + side, origin.y + side }, true);
    const int cells = static_cast<int>(std::ceil(side / kCell));
    for (int y = 0; y < cells; ++y) {
        for (int x = (y & 1); x < cells; x += 2) {
            const ImVec2 cellMin{ origin.x + x * kCell, origin.y + y * kCell };
            drawList->AddRectFilled(cellMin,
                { (std::min)(cellMin.x + kCell, origin.x + side),
                  (std::min)(cellMin.y + kCell, origin.y + side) },
                IM_COL32(40, 42, 47, 255));
        }
    }
    drawList->PopClipRect();
    drawList->AddImage(static_cast<ImTextureID>(std::bit_cast<std::uintptr_t>(rawId)),
                       { origin.x + 8.0f, origin.y + 8.0f },
                       { origin.x + side - 8.0f, origin.y + side - 8.0f });
    drawList->AddRect(origin, { origin.x + side, origin.y + side },
                      IM_COL32(95, 100, 112, 230), 4.0f, 0, 1.0f);
    return true;
}

/// Animation Preview の状態はパネルのメンバーではなくファイルスコープの共有状態なので、
/// ここでは往復の «口» を開けるだけにして、中身は AnimationPreview 側に閉じる。
void PreviewPanel::OnLoadSettings(const EditorSettings& settings)
{
    LoadAnimationPreviewSettings(settings);
}

void PreviewPanel::OnSaveSettings(EditorSettings& settings) const
{
    SaveAnimationPreviewSettings(settings);
}

void PreviewPanel::OnRenderContent(EditorContext& ctx)
{
    const std::string extension = LowerPreviewExtension(ctx.selectedAssetPath);
    if (extension == ".mat" || extension == ".mask" || extension == ".maskpreset" ||
        extension == ".ico") {
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
