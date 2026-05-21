// FBZZ Engine
// MeshRenderer.hpp | fbzz::scene
// メッシュとマテリアルを持つ描画コンポーネント
#pragma once
#include <memory>
#include <string>

namespace fbzz::renderer {
struct Mesh;     // Mesh.hpp で struct 定義のため struct で前方宣言
class  Material;
} // namespace fbzz::renderer

namespace fbzz::scene {

struct MeshRenderer {
    std::shared_ptr<renderer::Mesh>     mesh;
    std::shared_ptr<renderer::Material> material;
    bool enabled = true;

    // シリアライズ用パス (ランタイムでは未使用)
    // "primitive:cube" / "primitive:sphere" / "primitive:plane" / "models/test.fbx"
    std::string meshPath;
    std::string shaderPath;     // "assets/shaders/Material/PBR.hlsl"
    std::string albedoTexPath;  // "" = テクスチャなし
    std::string normalTexPath;
};

} // namespace fbzz::scene
