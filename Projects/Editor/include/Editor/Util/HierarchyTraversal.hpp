/// @file    HierarchyTraversal.hpp
/// @brief   Hierarchy の表示巡回と Scene 構造の索引を保持する。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene { class GameObject; class Scene; }

namespace fbzz::editor {

/// @note Scene の構造編集は描画後へ遅延するため、1 回の巡回内では生存 Entity の index が一意。
struct HierarchyTraversal {
    using EntitySet = std::unordered_set<std::uint64_t>;

    static std::uint64_t EntityKey(scene::EntityID id)
    {
        return (static_cast<std::uint64_t>(id.generation) << 32) | id.index;
    }

    EntitySet visited;
    std::vector<scene::EntityID> roots;
    std::vector<scene::EntityID> recoveredRoots;
    std::unordered_map<const scene::GameObject*, int> hiddenCounts;
    std::unordered_map<const scene::GameObject*, bool> visibleChildren;
    EntitySet selected;
    EntitySet locked;
    EntitySet revealed;

    /// @note Scene 世代または構造が変わったときだけルート順と生成物の集計を失効させる。
    void BeginFrame(scene::Scene& scene, bool showGeneratedObjects);

    bool Visit(scene::EntityID id)
    {
        return id.IsValid() && visited.insert(EntityKey(id)).second;
    }

private:
    uint64_t m_sceneGeneration = 0;
    uint64_t m_revision = 0;
    bool m_showGeneratedObjects = false;
};

} // namespace fbzz::editor
