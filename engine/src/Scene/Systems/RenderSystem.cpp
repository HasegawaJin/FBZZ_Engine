// FBZZ Engine
// RenderSystem.cpp | fbzz::scene
// MeshRenderer + Transform を走査し、DrawCall を発行する
// Mesh / Material / LightSystem は Step 5-3 (Renderer 追加) 後に実装する
#include "engine/Scene/Systems/RenderSystem.hpp"
#include "engine/Scene/Scene.hpp"

namespace fbzz::scene {

void RenderSystem(Scene& /*scene*/,
                  renderer::IRenderer& /*renderer*/,
                  const renderer::Camera& /*camera*/,
                  const renderer::LightSystem& /*lights*/) {
    // Mesh / Material / LightSystem 追加後に実装する
    // for (auto [tf, mr] : scene.View<Transform, MeshRenderer>()) {
    //     if (!mr.enabled || !mr.mesh || !mr.material) continue;
    //     DrawCall dc;
    //     dc.vertexBuffer = mr.mesh->vertexBuffer;
    //     dc.indexBuffer  = mr.mesh->indexBuffer;
    //     dc.indexCount   = mr.mesh->indexCount;
    //     dc.shader       = mr.material->shader;
    //     renderer.Submit(dc);
    // }
}

} // namespace fbzz::scene
