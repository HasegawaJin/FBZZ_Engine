// FBZZ Engine
// SkinnedMeshRenderer.hpp | fbzz::scene
// Skinned mesh component: holds the Model (mesh + skeleton).
// AnimatorComponent on the same GameObject drives the bone matrices.
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/Model.hpp>
#include <memory>
#include <string>

namespace fbzz::scene {

struct SkinnedMeshRenderer {
    bool enabled = true;
    std::string modelPath;
    int meshIndex = 0;

    std::shared_ptr<asset::Model> model; // runtime, not serialized

    const char* GetTypeName() const { return "Skinned Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",   enabled);
        r.Field("modelPath", modelPath);
        r.Field("meshIndex", meshIndex);
    }
};

} // namespace fbzz::scene
