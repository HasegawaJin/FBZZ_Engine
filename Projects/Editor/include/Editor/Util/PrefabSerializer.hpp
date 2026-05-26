// FBZZ Engine
// PrefabSerializer.hpp | fbzz::editor
// Saves and instantiates GameObject hierarchies as .fbzzprefab assets
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
};

} // namespace fbzz::editor
