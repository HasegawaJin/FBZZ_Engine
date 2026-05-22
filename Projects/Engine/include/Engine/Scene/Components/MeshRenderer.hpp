// FBZZ Engine
// MeshRenderer.hpp | fbzz::scene
// メッシュとマテリアルを持つ描画コンポーネント
#pragma once
#include <Engine/Scene/Script.hpp>
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

    const char* GetTypeName() const { return "Mesh Renderer"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("meshPath", meshPath);
        r.Field("shaderPath", shaderPath);
        r.Field("albedoTexPath", albedoTexPath);
        r.Field("normalTexPath", normalTexPath);
    }
};

} // namespace fbzz::scene
