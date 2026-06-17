// FBZZ Engine
// PrefabSerializer.hpp | fbzz::editor
// Saves and instantiates GameObject hierarchies as .prefab assets
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::scene { class Scene; }

namespace fbzz::editor {

class PrefabSerializer {
public:
    static bool SaveSelection(const scene::Scene& scene,
                              const std::vector<scene::EntityID>& selectedEntities,
                              const std::string& path);

    static bool Instantiate(scene::Scene& scene,
                            const std::string& path,
                            std::vector<scene::EntityID>& outRootEntities);

    // Apply: インスタンスの現在状態をプレファブアセットに書き戻す。
    // WHY: インスタンスを編集してプレファブに反映する Unity の "Apply to Prefab" に相当する。
    //      go.prefabAssetPath が空の場合は失敗を返す。
    static bool Apply(const scene::Scene& scene, scene::EntityID rootEntity);

    // Revert: インスタンスをプレファブアセットの状態に戻す。
    // WHY: インスタンスへの変更を破棄して元の定義に揃える "Revert" に相当する。
    //      旧 GO 階層を Destroy し、同じ Transform/parent 位置に再インスタンス化する。
    //      outNewRoots に再生成された GO の EntityID が入る。
    static bool Revert(scene::Scene& scene,
                       scene::EntityID rootEntity,
                       std::vector<scene::EntityID>& outNewRoots);
};

} // namespace fbzz::editor
