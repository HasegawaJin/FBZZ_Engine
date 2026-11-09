/// @file    HierarchyTraversal.hpp
/// @brief   Hierarchy の巡回状態をフレームごとに初期化し、作業領域の容量を再利用する。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene { class GameObject; }

namespace fbzz::editor {

/// @note Scene の構造編集は描画後へ遅延するため、1 回の巡回内では生存 Entity の index が一意。
struct HierarchyTraversal {
    using EntitySet = std::unordered_set<std::uint64_t>;

    static std::uint64_t EntityKey(scene::EntityID id)
    {
        return (static_cast<std::uint64_t>(id.generation) << 32) | id.index;
    }

    std::vector<bool> visited;
    std::unordered_map<const scene::GameObject*, int> hiddenCounts;
    EntitySet selected;
    EntitySet locked;
    EntitySet revealed;

    /// @brief 前フレームの値を破棄する。Scene の切替や同じ slot の再利用も次の巡回に持ち越さない。
    void BeginFrame(size_t gameObjectCount)
    {
        visited.assign(gameObjectCount, false);
        hiddenCounts.clear();
    }

    bool Contains(scene::EntityID id) const
    {
        return id.index < visited.size() && visited[id.index];
    }
    bool Visit(scene::EntityID id)
    {
        if (!id.IsValid() || Contains(id)) return false;
        if (id.index >= visited.size()) visited.resize(static_cast<size_t>(id.index) + 1);
        visited[id.index] = true;
        return true;
    }
};

} // namespace fbzz::editor
