// FBZZ Engine
// MaterialComponent.hpp | fbzz::scene
// マテリアル設定を GameObject に持たせるコンポーネント
// シェーダー・テクスチャ・係数を Renderer::Material へ反映する。
// Inspector / SceneSerializer で編集しやすい値型として保持する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <memory>
#include <string>

namespace fbzz::renderer {
class Material;
} // namespace fbzz::renderer

namespace fbzz::scene {

struct MaterialComponent {
    std::shared_ptr<renderer::Material> material;
    bool enabled = true;

    std::string shaderPath;
    std::string albedoTexPath;
    std::string normalTexPath;

    const char* GetTypeName() const { return "Material"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled",       enabled);
        r.Field("shaderPath",    shaderPath);
        r.Field("albedoTexPath", albedoTexPath);
        r.Field("normalTexPath", normalTexPath);
    }
};

} // namespace fbzz::scene
