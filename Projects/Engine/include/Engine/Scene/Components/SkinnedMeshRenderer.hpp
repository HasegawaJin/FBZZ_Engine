// FBZZ Engine
// SkinnedMeshRenderer.hpp | fbzz::scene
// Component that draws a skinned model and owns runtime bone entity links.
#pragma once

#include <Engine/Asset/Model.hpp>
#include <Engine/Scene/Entity.hpp>
#include <Engine/Scene/Script.hpp>
#include <string>
#include <vector>

namespace fbzz::scene {

struct SkinnedMeshRenderer {
    bool enabled = true;
    std::string modelPath;
    int meshIndex = -1; // -1 = 全 submesh, >=0 = 子 GO が担当する特定 submesh

    asset::Model* model = nullptr;

    // WHY: Transform is the common base for scene editing, animation and rigging.
    // SkeletonNode remains the asset-side lookup table; these EntityID values are
    // rebuilt at load/runtime and point at the visible Bone GameObjects.
    EntityID skeletonRootEntity = EntityID::INVALID;
    std::vector<EntityID> nodeEntities;

    const char* GetTypeName() const { return "Skinned Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",   enabled);
        r.Field("modelPath", modelPath);
        r.Field("meshIndex", meshIndex);
    }
};

} // namespace fbzz::scene
