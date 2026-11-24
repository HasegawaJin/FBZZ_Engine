/// @file    CanvasWorklist.hpp
/// @brief   現在の Canvas の要素をシーン階層の走査順で集める。
/// @author  Hasegawa Jin
/// @date    2026-10-18
#pragma once
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Components/UICanvas.hpp>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene::ui_detail {
/// @note 採集後にシーン状態を保持しない。UISystem が sortOrder でソートする前の深さ優先順を保つ。
inline std::vector<EntityID> CollectCanvasEntities(Scene& scene)
{
    std::vector<EntityID> entities;
    for (const auto id : scene.GetEntities<UICanvas>()) {
        const auto* object = scene.GetGameObject(id);
        const auto* canvas = scene.GetComponent<UICanvas>(id);
        if (object && object->activeInHierarchy() && canvas && canvas->enabled) entities.push_back(id);
    }
    if (entities.size() < 2) return entities;

    struct OrderedCanvas {
        EntityID entity;
        std::vector<int> path;
        const GameObject* root = nullptr;
        size_t rootOrder = SIZE_MAX;
    };
    std::vector<OrderedCanvas> ordered;
    ordered.reserve(entities.size());
    std::unordered_map<const GameObject*, size_t> rootOrders;
    for (const auto id : entities) {
        const auto* object = scene.GetGameObject(id);
        OrderedCanvas entry{id, {}};
        while (const auto* parent = object->GetParent()) {
            entry.path.push_back(object->GetSiblingIndex());
            object = parent;
        }
        entry.root = object;
        rootOrders.try_emplace(object, SIZE_MAX);
        std::reverse(entry.path.begin(), entry.path.end());
        ordered.push_back(std::move(entry));
    }
    /// @note 解決不能な親を持つ個体をルートに含めないよう、Scene のルート列から所属と順序を確定する。
    size_t rootOrder = 0;
    for (const auto* root : scene.GetRootGameObjects()) {
        if (const auto found = rootOrders.find(root); found != rootOrders.end()) found->second = rootOrder++;
    }
    for (auto& entry : ordered) entry.rootOrder = rootOrders.find(entry.root)->second;
    std::erase_if(ordered, [](const auto& entry) { return entry.rootOrder == SIZE_MAX; });
    /// @note 同じルートでは、親の短いキーを子孫のキーより前に置いて深さ優先順を再現する。
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
        if (a.rootOrder != b.rootOrder) return a.rootOrder < b.rootOrder;
        return a.path < b.path;
    });
    entities.clear();
    for (const auto& entry : ordered) entities.push_back(entry.entity);
    return entities;
}
}
