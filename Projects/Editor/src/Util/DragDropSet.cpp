/// @file    DragDropSet.cpp
/// @brief   複数選択ドラッグの集合の記録と取り出し。
/// @author  Hasegawa Jin
/// @date    2026-09-17
#include <Editor/Util/DragDropSet.hpp>
#include <imgui.h>
#include <algorithm>

namespace fbzz::editor::dragdrop {

namespace {

/// @brief ドラッグ元は毎フレーム payload を出し直すので、記録も毎フレーム上書きされる。
/// @note 受け側のウィンドウがドラッグ元より先に描かれると記録は 1 フレーム前のものになる。
///       それより古い記録は終わったドラッグの残りとして捨てる。
struct EntityRecord {
    scene::EntityID              grabbed;
    std::vector<scene::EntityID> entities;
    int                          frame = -2;
};

struct AssetRecord {
    std::string              grabbed;
    std::vector<std::string> paths;
    int                      frame = -2;
};

EntityRecord g_entities;
AssetRecord  g_assets;

bool IsFresh(int frame)
{
    return ImGui::GetFrameCount() - frame <= 1;
}

} // namespace

void SetEntityDrag(scene::EntityID grabbed, const std::vector<scene::EntityID>& selection)
{
    g_entities.grabbed = grabbed;
    g_entities.frame   = ImGui::GetFrameCount();
    g_entities.entities.clear();
    g_entities.entities.push_back(grabbed);
    if (std::find(selection.begin(), selection.end(), grabbed) == selection.end()) return;
    for (scene::EntityID id : selection)
        if (id != grabbed) g_entities.entities.push_back(id);
}

std::vector<scene::EntityID> DraggedEntities(scene::EntityID payloadId)
{
    if (g_entities.grabbed != payloadId || !IsFresh(g_entities.frame)) return { payloadId };
    return g_entities.entities;
}

void SetAssetDrag(const std::string& grabbedPayload, std::vector<std::string> paths)
{
    g_assets.grabbed = grabbedPayload;
    g_assets.frame   = ImGui::GetFrameCount();
    g_assets.paths.clear();
    g_assets.paths.push_back(grabbedPayload);
    if (std::find(paths.begin(), paths.end(), grabbedPayload) == paths.end()) return;
    for (std::string& path : paths)
        if (path != grabbedPayload
            && std::find(g_assets.paths.begin(), g_assets.paths.end(), path) == g_assets.paths.end())
            g_assets.paths.push_back(std::move(path));
}

std::vector<std::string> DraggedAssetPaths(const std::string& payloadPath)
{
    if (g_assets.grabbed != payloadPath || !IsFresh(g_assets.frame)) return { payloadPath };
    return g_assets.paths;
}

} // namespace fbzz::editor::dragdrop
