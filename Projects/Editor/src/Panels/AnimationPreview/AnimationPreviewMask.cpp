/// @file    AnimationPreviewMask.cpp
/// @brief   Avatar Mask のウェイトをメッシュ色で見せる専用プレビュー。
/// @author  Hasegawa Jin
/// @date    2026-09-16
///
/// WHY 分けるか: 通常の Animation Preview と GPU・カメラは共有するが、
/// 「対象は .mask とその骨格 FBX」「再生しない」「色は Weight」と入口も出口も別物。
/// 同じファイルに置くと、どちらの都合で書かれた分岐かが読めなくなる。
#include "AnimationPreviewInternal.hpp"
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <string>
#include <utility>

namespace fbzz::editor {

using namespace fbzz::editor::animpreview;

void SetAnimationMaskPreviewSelection(std::string_view nodePath)
{
    g_maskPreview.selectedNodePath = std::string(nodePath);
    g_maskPreview.pendingSelectionPath = g_maskPreview.selectedNodePath;
}

bool ConsumeAnimationMaskPreviewSelection(std::string& nodePath)
{
    if (g_maskPreview.pendingSelectionPath.empty()) return false;
    nodePath = std::move(g_maskPreview.pendingSelectionPath);
    return true;
}

void ClearAnimationMaskPreviewSelection()
{
    g_maskPreview.selectedNodePath.clear();
    g_maskPreview.pendingSelectionPath.clear();
}

bool DrawAnimationMaskPreviewWidget(EditorContext& ctx,
                                    std::string_view modelPath,
                                    std::string_view maskPath,
                                    float previewHeight,
                                    const asset::AvatarMaskAsset* maskOverride)
{
    if (modelPath.empty() || maskPath.empty()) return false;

    PreviewTarget target;
    if (!ResolvePathTarget(std::string(modelPath), target)) return false;
    target.modelPath = NormalizeAssetPath(std::string(modelPath));
    target.label = util::FileSystem::GetFilename(target.modelPath);

    const std::string normalizedMaskPath = NormalizeAssetPath(std::string(maskPath));
    if ((g_maskPreview.modelPath != target.modelPath ||
         g_maskPreview.maskPath != normalizedMaskPath) &&
        g_maskPreview.pendingSelectionPath.empty()) {
        ClearAnimationMaskPreviewSelection();
    }
    g_maskPreview.active = true;
    g_maskPreview.modelPath = target.modelPath;
    g_maskPreview.maskPath = normalizedMaskPath;
    if (maskOverride) {
        g_maskPreview.mask = *maskOverride;
        g_maskPreview.loaded = true;
    } else {
        g_maskPreview.mask = asset::AvatarMaskAsset{};
        const std::string resolvedMask =
            asset::AssetManager::ResolveAssetPath(g_maskPreview.maskPath);
        g_maskPreview.loaded = asset::LoadAvatarMaskAsset(
            resolvedMask.empty() ? g_maskPreview.maskPath : resolvedMask, g_maskPreview.mask);
    }

    g_state.target = target;
    g_state.origin = TargetOrigin::Manual;
    g_state.time = 0.0f;
    g_state.playing = false;
    g_state.needsFraming = true;
    const bool previousShowMesh = g_state.showMesh;
    const bool previousShowBones = g_state.showBones;
    g_state.showMesh = true;
    g_state.showBones = true;
    const bool result = DrawAnimationPreviewWidget(ctx, previewHeight);
    g_state.showMesh = previousShowMesh;
    g_state.showBones = previousShowBones;
    g_maskPreview.active = false;
    g_maskPreview.loaded = false;
    return result;
}

void RequestAnimationMaskPreview(std::string_view maskPath, std::string_view modelPath)
{
    if (maskPath.empty()) return;
    g_maskPreviewRequest.maskPath = std::string(maskPath);
    g_maskPreviewRequest.modelPath = std::string(modelPath);
    g_maskPreviewRequest.pending = true;
}

void DrawAnimationMaskPreviewPanelContent(EditorContext& ctx)
{
    static std::string modelPath;
    static std::string maskPath;
    static std::string lastSelectedAsset;

    // 外部からの指定は選択追従より強い。押した本人が「今これを見たい」と言っている。
    if (g_maskPreviewRequest.pending) {
        g_maskPreviewRequest.pending = false;
        maskPath = NormalizeAssetPath(g_maskPreviewRequest.maskPath);
        lastSelectedAsset = maskPath;
        if (!g_maskPreviewRequest.modelPath.empty()) {
            modelPath = NormalizeAssetPath(g_maskPreviewRequest.modelPath);
        } else {
            asset::AvatarMaskAsset requested;
            const std::string resolved = asset::AssetManager::ResolveAssetPath(maskPath);
            if (asset::LoadAvatarMaskAsset(resolved.empty() ? maskPath : resolved, requested) &&
                !requested.skeletonSourcePath.empty())
                modelPath = requested.skeletonSourcePath;
        }
        ClearAnimationMaskPreviewSelection();
    }

    const std::string selected = NormalizeAssetPath(ctx.selectedAssetPath);
    const std::string selectedExt = util::StringUtils::ToLower(
        util::FileSystem::GetExtension(selected));
    if (selected != lastSelectedAsset) {
        lastSelectedAsset = selected;
        if (selectedExt == ".fbx" || selectedExt == ".fzasset" || selectedExt == ".asset")
            modelPath = selected;
        else if (selectedExt == ".mask" || selectedExt == ".maskpreset") {
            maskPath = selected;
            asset::AvatarMaskAsset selectedMask;
            const std::string resolved = asset::AssetManager::ResolveAssetPath(selected);
            if (asset::LoadAvatarMaskAsset(
                    resolved.empty() ? selected : resolved, selectedMask)) {
                if (!selectedMask.skeletonSourcePath.empty())
                    modelPath = selectedMask.skeletonSourcePath;
            }
        }
    }

    ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "Animation Mask Preview");
    // スキンメッシュは「そのメッシュのノード」ではなく「頂点を動かすボーン」で色が決まる。
    // メッシュノードで引くと、マスク対象がアーマチュアの外にあるため常に 0 になっていた。
    ImGui::TextDisabled("Skinned mesh is colored by the mask weight of the bones that skin it.");
    widgets::AssetPathField("FBX", modelPath, ".fbx,.fzasset,.asset", ctx.projectRoot);
    widgets::AssetPathField("Mask", maskPath, ".mask,.maskpreset", ctx.projectRoot);

    // ウェイトの読み方を色見本で示す。数値を出すより «赤 = 効かない» が一目で通る。
    const struct { float weight; const char* label; } legend[] = {
        { 0.0f, "0.0 Excluded" }, { 0.5f, "0.5 Blended" }, { 1.0f, "1.0 Included" }
    };
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, 14.0f);
        ImGui::TextColored(MaskWeightColor(legend[i].weight), "%s", legend[i].label);
    }

    const float previewHeight =
        (std::max)(ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.0f,
                   160.0f);
    if (!DrawAnimationMaskPreviewWidget(ctx, modelPath, maskPath, previewHeight)) {
        ImGui::TextDisabled("FBX と .mask を指定してください。");
    }
}

} // namespace fbzz::editor
