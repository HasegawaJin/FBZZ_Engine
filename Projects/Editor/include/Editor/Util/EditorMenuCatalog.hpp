/// @file    EditorMenuCatalog.hpp
/// @brief   View のパネル分類と表示補助の共通投影。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once

#include <Editor/Panels/IPanel.hpp>
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fbzz::editor {

/// @note ウィンドウ名は docking と設定の ID。分類だけを変え、パネル側の保存契約は変えない。
inline std::string_view PanelMenuCategory(const IPanel& panel)
{
    const std::string_view name = panel.GetWindowName();
    if (name == "Scene" || name == "Game" || name == "UI" || name == "Scene Hierarchy"
        || name == "Inspector" || name == "Asset Browser" || name == "Console"
        || name == "Search" || name == "Undo History") return {};
    if (name == "Animation Graph" || name == "Animation Mask Preview" || name == "Sequence")
        return "Animation";
    if (name == "VFX Timeline" || name == "Fluid Editor" || name == "SFX Editor")
        return "Effects & Audio";
    if (name == "Preview" || name == "Sprite Editor" || name == "Dependency View"
        || name.starts_with("Asset Browser ")) return "Assets";
    if (name == "Navigation" || name == "Behavior Tree") return "AI & Navigation";
    if (name == "Analysis" || name == "Script Profiler" || name == "Memory Debug"
        || name == "Render Pass Viewer" || name == "Build Output" || name == "Script Requirements")
        return "Diagnostics";
    if (name == "Hotkey Editor" || name == "Project Settings" || name == "AI Settings")
        return "Settings";
    const char* category = panel.GetMenuCategory();
    return category && category[0] != '\0' ? std::string_view(category) : "Other Panels";
}

struct PanelMenuEntry {
    IPanel* panel = nullptr;
};

struct PanelMenuGroup {
    std::string category;
    std::vector<PanelMenuEntry> entries;
};

/// @note ImGui とネイティブメニューが同じ一覧を使う。非連続のカテゴリと将来のパネルも一度だけ列挙する。
inline std::vector<PanelMenuGroup> BuildPanelMenu(const std::vector<std::unique_ptr<IPanel>>& panels)
{
    std::vector<PanelMenuGroup> groups;
    for (const char* category : { "", "Animation", "Effects & Audio", "Assets", "AI & Navigation",
                                 "Diagnostics", "Settings", "Other Panels" })
        groups.push_back({category, {}});
    for (const auto& ownedPanel : panels) {
        IPanel* panel = ownedPanel.get();
        if (!panel || !panel->ShowInViewMenu()) continue;
        const std::string_view category = PanelMenuCategory(*panel);
        auto group = std::find_if(groups.begin(), groups.end(), [&category](const PanelMenuGroup& candidate) {
            return candidate.category == category;
        });
        if (group == groups.end()) {
            groups.push_back({std::string(category), {}});
            group = groups.end() - 1;
        }
        group->entries.push_back({panel});
    }
    const auto primaryOrder = [](const PanelMenuEntry& entry) {
        constexpr std::string_view names[] = { "Scene", "Game", "UI", "Scene Hierarchy", "Inspector",
                                              "Asset Browser", "Console", "Search", "Undo History" };
        return std::find(std::begin(names), std::end(names), entry.panel->GetWindowName()) - std::begin(names);
    };
    std::stable_sort(groups.front().entries.begin(), groups.front().entries.end(),
                     [&primaryOrder](const PanelMenuEntry& a, const PanelMenuEntry& b) {
                         return primaryOrder(a) < primaryOrder(b);
                     });
    std::erase_if(groups, [](const PanelMenuGroup& group) { return group.entries.empty(); });
    return groups;
}

struct EditorMenuAction {
    const char* operatorId;
    const char* label;
    const char* mode = nullptr;
};

inline constexpr EditorMenuAction COMMON_SCENE_OVERLAYS[] = {
    {"render.show_grid", "Grid"}, {"render.show_stats", "Stats"},
    {"render.show_scene_icons", "Scene Icons"}, {"render.show_script_gizmos", "Script Gizmos"},
};
inline constexpr EditorMenuAction PHYSICS_SCENE_OVERLAYS[] = {
    {"render.show_colliders", "Colliders"}, {"render.show_constraints", "Constraints"},
    {"render.show_rigid_bodies", "Rigid Bodies"}, {"render.show_physics_volumes", "Physics Volumes"},
    {"render.show_terrain_collision", "Terrain Collision"},
    {"render.show_physics_broadphase", "Broadphase Visualization"},
};
inline constexpr EditorMenuAction ANIMATION_SCENE_OVERLAYS[] = {
    {"render.show_skeleton", "Skeleton"}, {"render.skeleton_selected_only", "Skeleton: Selected Only"},
    {"render.show_ik", "IK Chains"}, {"render.show_spring_bones", "Spring Bones"},
    {"render.show_attachments", "Attachments"},
};
inline constexpr EditorMenuAction EFFECTS_SCENE_OVERLAYS[] = {
    {"render.show_vfx_gizmos", "VFX Emitters"}, {"render.show_vfx_paths", "VFX Paths"},
    {"render.show_flow_fields", "Flow Fields"}, {"render.show_flow_samples", "Flow Samples"},
    {"render.show_water_flow", "Water Flow"},
};
inline constexpr EditorMenuAction NAVIGATION_SCENE_OVERLAYS[] = {
    {"render.show_navmesh", "NavMesh"}, {"render.show_nav_sensors", "AI Sensors"},
};
inline constexpr EditorMenuAction RENDERING_SCENE_OVERLAYS[] = {
    {"render.show_render_bvh", "Shadow-query BVH"},
    {"render.show_light_range", "Light Range"}, {"render.show_decal_bounds", "Decal Bounds"},
    {"render.show_terrain_bounds", "Terrain Bounds"}, {"render.show_lod_bounds", "LOD Bounds"},
};
struct EditorMenuActionGroup {
    const char* category;
    std::span<const EditorMenuAction> actions;
};
inline constexpr EditorMenuActionGroup SCENE_OVERLAY_GROUPS[] = {
    {"", COMMON_SCENE_OVERLAYS}, {"Physics", PHYSICS_SCENE_OVERLAYS},
    {"Animation", ANIMATION_SCENE_OVERLAYS}, {"Effects", EFFECTS_SCENE_OVERLAYS},
    {"Navigation", NAVIGATION_SCENE_OVERLAYS}, {"Rendering", RENDERING_SCENE_OVERLAYS},
};
inline constexpr EditorMenuAction NAVMESH_DRAW_MODES[] = {
    {"render.set_navmesh_draw_mode", "Solid", "solid"},
    {"render.set_navmesh_draw_mode", "Transparent", "transparent"},
    {"render.set_navmesh_draw_mode", "Areas", "areas"},
    {"render.set_navmesh_draw_mode", "Portals", "portals"},
    {"render.set_navmesh_draw_mode", "Voxels", "voxels"},
};
inline constexpr EditorMenuAction SCENE_VIEW_MODES[] = {
    {"render.set_view_mode", "Lit", "lit"}, {"render.set_view_mode", "Unlit", "unlit"},
    {"render.set_view_mode", "Wireframe Lit", "wireframe_lit"},
    {"render.set_view_mode", "Wireframe Unlit", "wireframe_unlit"},
};
inline constexpr EditorMenuAction DEVELOPER_VIEW_MODES[] = {
    {"render.set_view_mode", "Ray Hit Distance", "ray_hit_distance"},
    {"render.set_view_mode", "Ray Geometric Normal", "ray_geometric_normal"},
    {"render.set_view_mode", "Ray Instance ID", "ray_instance_id"},
};

} /// @note namespace fbzz::editor
