/// @file    CanvasWorklist.hpp
/// @brief   Collect current Canvas members in their original hierarchy traversal order.
/// @author  Hasegawa Jin
/// @date    2026-10-18
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <algorithm>
#include <vector>
#include <utility>
#include <iterator>

namespace fbzz::scene::ui_detail {
/// @note No Scene state survives this pure collection. Preserve the original DFS input order before UISystem applies its unchanged sortOrder comparator.
inline std::vector<EntityID> CollectCanvasEntities(Scene& scene)
{
    std::vector<EntityID> entities;
    for (const auto id : scene.GetEntities<UICanvas>()) {
        const auto* object = scene.GetGameObject(id);
        const auto* canvas = scene.GetComponent<UICanvas>(id);
        if (object && object->activeInHierarchy() && canvas && canvas->enabled) entities.push_back(id);
    }
    if (entities.size() < 2) return entities;

    const auto roots = scene.GetRootGameObjects();
    struct OrderedCanvas { EntityID entity; std::vector<int> path; };
    std::vector<OrderedCanvas> ordered;
    ordered.reserve(entities.size());
    for (const auto id : entities) {
        auto* object = scene.GetGameObject(id);
        OrderedCanvas entry{id, {}};
        while (object->GetParent()) {
            entry.path.push_back(object->GetSiblingIndex());
            object = object->GetParent();
        }
        const auto root = std::find(roots.begin(), roots.end(), object);
        if (root == roots.end()) continue;
        entry.path.push_back(static_cast<int>(std::distance(roots.begin(), root)));
        std::reverse(entry.path.begin(), entry.path.end());
        ordered.push_back(std::move(entry));
    }
    /// @note A parent key precedes its longer descendant key; root and child indices reproduce DFS even after reparenting or sibling edits.
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    entities.clear();
    for (const auto& entry : ordered) entities.push_back(entry.entity);
    return entities;
}
} /// @note namespace fbzz::scene::ui_detail
