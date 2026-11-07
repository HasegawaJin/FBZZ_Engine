/// @file    SceneForest.hpp
/// @brief   Checked publication of a prepared object forest into an existing Scene.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Scene/Entity.hpp>
#include <string>
#include <vector>

namespace fbzz::renderer { class ResourceManager; }

namespace fbzz::scene {
class Scene;

/// @note Worker-safe structural validation only; does not access a Scene, assets, Script factories or renderer resources.
/// @note Returns saved object identities in document order. Failure clears outIds and provides an error.
[[nodiscard]] FBZZ_ENGINE_API bool ValidateSceneForestDocument(const std::string& document,
    std::vector<std::string>& outIds, std::string& error);

/// @note Main-thread only. Saved identities and hierarchy ownership are validated before changing the destination.
/// @note Preparation is data-only; registered Script constructors and callbacks run once after the whole forest is attached.
/// @note Failure clears both outputs. Referenced terrain assets must load successfully; resources may be null for data-only meshes.
[[nodiscard]] FBZZ_ENGINE_API bool AppendSceneForest(Scene& destination,
    const std::string& document, const std::string& sourcePath, renderer::ResourceManager* resources,
    std::vector<EntityID>& outRoots, std::vector<EntityID>& outEntities, std::string& error);

} /// @note namespace fbzz::scene
