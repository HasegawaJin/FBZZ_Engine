// FBZZ Engine
// PrimitiveMesh.hpp | fbzz::renderer
// 手続き生成メッシュのファクトリ
#pragma once
#include <memory>
#include "Mesh.hpp"

namespace fbzz::renderer {

class IRenderer;

class PrimitiveMesh {
public:
    static std::shared_ptr<Mesh> Cube    (IRenderer& renderer);
    static std::shared_ptr<Mesh> Sphere  (IRenderer& renderer, int segments = 16);
    static std::shared_ptr<Mesh> Plane   (IRenderer& renderer);
    static std::shared_ptr<Mesh> Cylinder(IRenderer& renderer, int segments = 16);
    static std::shared_ptr<Mesh> Cone    (IRenderer& renderer, int segments = 16);
    static std::shared_ptr<Mesh> Torus   (IRenderer& renderer, int segments = 24);
    static std::shared_ptr<Mesh> Capsule (IRenderer& renderer, int segments = 24);
};

} // namespace fbzz::renderer
