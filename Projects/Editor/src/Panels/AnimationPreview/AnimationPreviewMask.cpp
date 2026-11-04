/// @file    AnimationPreviewMask.cpp
/// @brief   Avatar Mask のウェイトをメッシュ色で見せる専用プレビュー。
/// @author  Hasegawa Jin
/// @date    2026-09-16

/// @note 通常の Animation Preview と GPU・カメラは共有するが、「対象は .mask とその骨格 FBX」
/// @note 「再生しない」「色は Weight」と入口も出口も別物。同じファイルに置くと、
/// @note どちらの都合で書かれた分岐かが読めなくなるため分ける。
#include <Engine/Asset/ModelSource.hpp>
#include "AnimationPreviewInternal.hpp"
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/AssetPath.hpp>
#include <Editor/Util/ImGuiWidgets.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace fbzz::editor {

using namespace fbzz::editor::animpreview;

namespace {
std::string g_panelModelPath;
std::string g_panelMaskPath;
std::string g_panelLastSelectedAsset;
} /// @note namespace

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
    const bool targetChanged = g_maskPreview.modelPath != target.modelPath
        || g_maskPreview.maskPath != normalizedMaskPath || g_state.target.modelPath != target.modelPath;
    if (targetChanged &&
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
    if (targetChanged) g_state.needsFraming = true;
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
    auto& modelPath = g_panelModelPath;
    auto& maskPath = g_panelMaskPath;
    auto& lastSelectedAsset = g_panelLastSelectedAsset;

    /// @note 外部からの指定は選択追従より強い。押した本人が「今これを見たい」と言っている。
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
        if (asset::IsModelSourcePath(selectedExt) || selectedExt == ".fzasset" || selectedExt == ".asset")
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
    /// @note スキンメッシュは「そのメッシュのノード」ではなく「頂点を動かすボーン」で色が決まる。
    ///       メッシュノードで引くと、マスク対象がアーマチュアの外にあるため常に 0 になっていた。
    ImGui::TextDisabled("Skinned mesh is colored by the mask weight of the bones that skin it.");
    widgets::AssetPathField("Model", modelPath, ".fbx,.fzasset,.asset,.obj,.gltf,.glb", ctx.projectRoot);
    widgets::AssetPathField("Mask", maskPath, ".mask,.maskpreset", ctx.projectRoot);

    /// @note ウェイトの読み方を色見本で示す。数値を出すより «赤 = 効かない» が一目で通る。
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
        ImGui::TextDisabled("モデルと .mask を指定してください。");
    }
}

void RegisterAnimationMaskPreviewOperators(OperatorRegistry& registry)
{
    for (const std::string_view action : {"inspect", "open", "configure"}) {
        EditorOperator op;
        op.id = "animation.mask.preview." + std::string(action);
        op.label = op.id;
        op.category = "Animation Preview";
        op.kind = action == "inspect" ? OpKind::Query : OpKind::Action;
        op.desc = "Avatar Mask専用プレビューのモデル・マスク・骨選択・カメラを指定する。角度はラジアン。";
        if (action == "open") {
            op.params = {
                {"mask", OpParamType::String, "Avatar Mask asset path", true, {}},
                {"model", OpParamType::String, "Skeleton model override", false, {}}
            };
        } else if (action == "configure") {
            op.params = {
                {"nodePath", OpParamType::String, "Selected bone path", false, {}},
                {"yaw", OpParamType::Float, "Camera yaw", false, {}},
                {"pitch", OpParamType::Float, "Camera pitch", false, {}},
                {"distance", OpParamType::Float, "Camera distance", false, {}}
            };
        }
        op.exec = [action](OpContext& context, const OpArgs& args) {
            if (action == "open") {
                const auto path = NormalizeAssetPath(args.GetString("mask"));
                const auto extension = util::StringUtils::ToLower(util::FileSystem::GetExtension(path));
                if (extension != ".mask" && extension != ".maskpreset") return OpResult::Err("BAD_ARG", "mask");
                asset::AvatarMaskAsset mask;
                const auto resolved = asset::AssetManager::ResolveAssetPath(path);
                if (!asset::LoadAvatarMaskAsset(resolved.empty() ? path : resolved, mask))
                    return OpResult::Err("MASK_NOT_FOUND", path);
                const auto model = NormalizeAssetPath(args.GetString("model", mask.skeletonSourcePath));
                PreviewTarget target;
                if (model.empty() || !ResolvePathTarget(model, target)) return OpResult::Err("MODEL_NOT_FOUND", model);
                g_panelModelPath = model;
                g_panelMaskPath = path;
                g_panelLastSelectedAsset = NormalizeAssetPath(context.ctx.selectedAssetPath);
                g_maskPreviewRequest.pending = false;
                ClearAnimationMaskPreviewSelection();
                g_maskPreview.modelPath = model;
                g_maskPreview.maskPath = path;
                target.modelPath = model;
                target.label = util::FileSystem::GetFilename(model);
                g_state.target = std::move(target);
                g_state.origin = TargetOrigin::Manual;
                g_state.time = 0.0f;
                g_state.playing = false;
                g_state.needsFraming = true;
            } else if (action == "configure") {
                const float yaw = args.GetFloat("yaw", g_state.yaw);
                const float pitch = args.GetFloat("pitch", g_state.pitch);
                const float distance = args.GetFloat("distance", g_state.distance);
                if (!std::isfinite(yaw) || !(pitch >= -1.35f && pitch <= 1.35f)
                    || !(distance >= 0.01f && distance <= 86400.0f))
                    return OpResult::Err("BAD_ARG", "yaw/pitch/distance");
                g_state.yaw = yaw;
                g_state.pitch = pitch;
                g_state.distance = distance;
                if (args.Has("distance")) g_state.needsFraming = false;
                if (args.Has("nodePath")) SetAnimationMaskPreviewSelection(args.GetString("nodePath"));
            }
            auto data = OpData::MakeObject();
            data.Set("mask", g_panelMaskPath);
            data.Set("model", g_panelModelPath);
            data.Set("nodePath", g_maskPreview.selectedNodePath);
            data.Set("yaw", g_state.yaw);
            data.Set("pitch", g_state.pitch);
            data.Set("distance", g_state.distance);
            return OpResult::Data(std::move(data));
        };
        registry.Register(std::move(op));
    }
}

} /// @note namespace fbzz::editor
