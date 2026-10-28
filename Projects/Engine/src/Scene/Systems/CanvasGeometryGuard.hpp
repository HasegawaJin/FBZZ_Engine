/// @file    CanvasGeometryGuard.hpp
/// @brief   Check the union of active Canvas subtrees without repeating geometry probes.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace fbzz::scene::canvas_geometry_detail {

/// @brief Check geometry presence over eligible Canvas subtrees using invocation-local scratch.
/// @pre The Scene hierarchy and components remain unchanged for this synchronous call.
/// @note Only completed geometry-free roots are memoized; the first geometry hit returns before any memo allocation.
/// @note UI measurement, layout and dragging can write transforms before the target-view filter.
/// @note The probe must only observe component presence; no activity, enabled or view filter applies to children.
template<typename GeometryProbe>
[[nodiscard]] bool CanShareCanvasFrameGeometry(Scene& scene, GeometryProbe&& hasGeometry)
{
    const auto canvasEntities = scene.GetEntities<UICanvas>();
    const bool deduplicate = canvasEntities.size() > 1;
    std::unordered_set<uint64_t> provenRoots;
    std::vector<GameObject*> pending;
    const auto key = [](const GameObject& object) {
        const EntityID id = object.GetID();
        return (static_cast<uint64_t>(id.generation) << 32) | static_cast<uint64_t>(id.index);
    };
    for (EntityID id : canvasEntities) {
        auto* canvas = scene.GetComponent<UICanvas>(id);
        auto* root = scene.GetGameObject(id);
        if (!canvas || !canvas->enabled || !root || !root->activeInHierarchy())
            continue;
        bool covered = false;
        if (!provenRoots.empty()) {
            for (auto* ancestor = root; ancestor; ancestor = ancestor->GetParent()) {
                if (provenRoots.contains(key(*ancestor))) { covered = true; break; }
            }
        }
        if (covered) continue;
        const bool skipProven = !provenRoots.empty();
        pending.push_back(root);
        while (!pending.empty()) {
            auto* object = pending.back();
            pending.pop_back();
            /// @note A previously checked child root proves its whole subtree; registration order need not be parent first.
            if (skipProven && provenRoots.contains(key(*object))) continue;
            if (hasGeometry(*object)) return false;
            for (int childIndex = 0; childIndex < object->GetChildCount(); ++childIndex)
                if (auto* child = object->GetChild(childIndex)) pending.push_back(child);
        }
        if (deduplicate) provenRoots.insert(key(*root));
    }
    return true;
}

/// @brief Preserve conservative renderer-component presence, including disabled or empty renderers.
[[nodiscard]] inline bool CanShareCanvasFrameGeometry(Scene& scene)
{
    return CanShareCanvasFrameGeometry(scene, [](GameObject& object) {
        return object.GetComponent<MeshRenderer>() || object.GetComponent<SkinnedMeshRenderer>();
    });
}

} /// @note namespace fbzz::scene::canvas_geometry_detail
