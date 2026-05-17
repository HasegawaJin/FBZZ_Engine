// FBZZ Engine
// MeshRenderer.hpp | fbzz::scene
// メッシュとマテリアルを持つ描画コンポーネント
#pragma once
#include <memory>

namespace fbzz::renderer {
class Mesh;
class Material;
} // namespace fbzz::renderer

namespace fbzz::scene {

struct MeshRenderer {
    std::shared_ptr<renderer::Mesh>     mesh;
    std::shared_ptr<renderer::Material> material;
    bool enabled = true;
};

} // namespace fbzz::scene
