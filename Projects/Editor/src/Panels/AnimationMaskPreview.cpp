/// @file    AnimationMaskPreview.cpp
/// @brief   Avatar Mask の IPreviewPanel 実装。
/// @author  Hasegawa Jin
/// @date    2026-08-19
#include <Editor/Panels/AnimationMaskPreview.hpp>
#include <Editor/Panels/AnimationPreview.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace fbzz::editor {

namespace {

std::string NormalizeExtension(std::string_view extension)
{
    std::string normalized(extension);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return normalized;
}

} // namespace

bool AnimationMaskPreview::Supports(std::string_view extension) const
{
    const std::string normalized = NormalizeExtension(extension);
    return normalized == ".mask" || normalized == ".maskpreset";
}

bool AnimationMaskPreview::DrawPreview(EditorContext& ctx,
                                       std::string_view assetPath,
                                       float previewHeight)
{
    asset::AvatarMaskAsset mask;
    const std::string normalizedPath = NormalizeAssetPath(std::string(assetPath));
    const std::string resolvedPath = asset::AssetManager::ResolveAssetPath(normalizedPath);
    if (!asset::LoadAvatarMaskAsset(
            resolvedPath.empty() ? normalizedPath : resolvedPath, mask)) {
        ImGui::TextDisabled("Unable to load Avatar Mask.");
        return false;
    }
    return DrawPreview(ctx, normalizedPath, mask, previewHeight);
}

bool AnimationMaskPreview::DrawPreview(EditorContext& ctx,
                                       std::string_view assetPath,
                                       const asset::AvatarMaskAsset& mask,
                                       float previewHeight)
{
    if (mask.skeletonSourcePath.empty()) {
        ImGui::TextDisabled("Skeleton Source に FBX を指定してください。");
        return false;
    }

    return DrawAnimationMaskPreviewWidget(
        ctx, mask.skeletonSourcePath, assetPath, previewHeight, &mask);
}

} // namespace fbzz::editor
