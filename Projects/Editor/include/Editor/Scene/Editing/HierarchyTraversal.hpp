/// @file    HierarchyTraversal.hpp
/// @brief   Hierarchy の表示巡回と Scene 構造の索引を保持する。
/// @author  Hasegawa Jin
/// @date    2026-10-12
#pragma once
#include <Engine/Scene/References/Entity.hpp>
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

    EntitySet visited; ///< 表示巡回を済ませた EntityKey の集合。循環した枝で同じ行を二重に描かない。
    std::vector<scene::EntityID> roots; ///< Hierarchy の起点となる通常のルートエンティティ。
    std::vector<scene::EntityID> recoveredRoots; ///< ルートから辿れず、別の表示起点として回収したエンティティ。
    std::unordered_map<const scene::GameObject*, int> hiddenCounts; ///< 各オブジェクトの下で非表示にされている生成オブジェクトの件数。
    std::unordered_map<const scene::GameObject*, bool> visibleChildren; ///< 各オブジェクトに現在のフィルターで表示できる子がいるかの索引。
    EntitySet selected; ///< 現在の選択対象を EntityKey で引くための集合。
    EntitySet locked; ///< ロック状態にある対象を EntityKey で引くための集合。
    EntitySet revealed; ///< Hierarchy で表示を要求された分岐を EntityKey で引くための集合。

    /// @note Scene 世代または構造が変わったときだけルート順と生成物の集計を失効させる。
    void BeginFrame(scene::Scene& scene, bool showGeneratedObjects);

    bool Visit(scene::EntityID id)
    {
        return id.IsValid() && visited.insert(EntityKey(id)).second;
    }

private:
    uint64_t m_sceneGeneration = 0; ///< ルート順と生成物を数えたときのシーンの世代番号。
    uint64_t m_revision = 0; ///< ルート順と生成物を数えたときのシーン構造のリビジョン。
    bool m_showGeneratedObjects = false; ///< キャッシュを作成したときに生成オブジェクトの表示を許可していたか。
};

} // namespace fbzz::editor
