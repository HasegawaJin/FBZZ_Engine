// FBZZ Engine
// Model.hpp | fbzz::asset
// FBX and OBJ asset data: meshes, materials, and optional skeletal animation
#pragma once
#include <memory>
#include <vector>
#include "AnimationClip.hpp"
#include "Skeleton.hpp"

namespace fbzz::renderer { struct Mesh; class Material; }

namespace fbzz::asset {

struct Model {
    std::vector<std::shared_ptr<renderer::Mesh>>     meshes;
    std::vector<std::shared_ptr<renderer::Material>> materials;  // meshes[i] maps to materials[i]
    std::shared_ptr<Skeleton> skeleton;
    std::vector<AnimationClip> clips;
};

} // namespace fbzz::asset
