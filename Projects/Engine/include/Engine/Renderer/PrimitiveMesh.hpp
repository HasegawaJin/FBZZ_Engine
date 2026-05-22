// FBZZ Engine
// PrimitiveMesh.hpp | fbzz::renderer
// 手続き生成メッシュのファクトリ
#pragma once
#include <memory>
#include "Mesh.hpp"

namespace fbzz::renderer {

class ResourceManager;

class PrimitiveMesh {
public:
    static std::shared_ptr<Mesh> Cube    (ResourceManager& resources);
    static std::shared_ptr<Mesh> Sphere  (ResourceManager& resources, int segments = 16);
    static std::shared_ptr<Mesh> Plane   (ResourceManager& resources);
    static std::shared_ptr<Mesh> Cylinder(ResourceManager& resources, int segments = 16);
    static std::shared_ptr<Mesh> Cone    (ResourceManager& resources, int segments = 16);
    static std::shared_ptr<Mesh> Torus   (ResourceManager& resources, int segments = 24);
    static std::shared_ptr<Mesh> Capsule (ResourceManager& resources, int segments = 24);
};

} // namespace fbzz::renderer
