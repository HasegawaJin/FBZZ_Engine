// FBZZ Engine
// MaterialComponent.hpp | fbzz::scene
// GameObject が参照する .mat マテリアルアセットと GPU Material キャッシュ
#pragma once

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/RenderLayer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/Script.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace fbzz::scene {

struct MaterialComponent {
    std::unique_ptr<renderer::Material> material;

    MaterialComponent() = default;
    ~MaterialComponent() = default;

    MaterialComponent(const MaterialComponent& o)
        : material(o.material ? std::make_unique<renderer::Material>(*o.material) : nullptr)
        , materialAsset(o.materialAsset)
        , enabled(o.enabled)
        , materialPath(o.materialPath)
    {}
    MaterialComponent& operator=(const MaterialComponent& o)
    {
        if (this != &o) {
            material     = o.material ? std::make_unique<renderer::Material>(*o.material) : nullptr;
            materialAsset = o.materialAsset;
            enabled      = o.enabled;
            materialPath = o.materialPath;
        }
        return *this;
    }
    MaterialComponent(MaterialComponent&&)            = default;
    MaterialComponent& operator=(MaterialComponent&&) = default;
    renderer::ResourceHandle<renderer::MaterialAssetTag> materialAsset;
    bool enabled = true;

    // .mat の assets/ 相対パス。空文字は「マテリアル未割当」として RenderSystem が描画をスキップする。
    // WHY: MaterialComponent は参照だけを持ち、シェーダー・パラメータ・テクスチャは共有アセット側へ集約する。
    std::string materialPath;

    const char* GetTypeName() const { return "Material"; }

    // materialPath が設定されていれば AssetManager 経由で共有 MaterialAsset を解決する。
    // WHY: BlendMode / RenderQueue は描画キュー振り分け前に必要なため、SyncMaterial より前でも解決できるようにする。
    bool EnsureMaterialAsset()
    {
        if (!materialAsset.IsValid() && !materialPath.empty())
            materialAsset = asset::AssetManager::LoadMaterial(materialPath);
        return materialAsset.IsValid();
    }

    renderer::BlendMode GetBlendMode() const
    {
        const auto* a = asset::AssetManager::GetMaterial(materialAsset);
        return a ? a->blendMode : renderer::BlendMode::OPAQUE_BLEND;
    }

    bool IsDoubleSided() const
    {
        const auto* a = asset::AssetManager::GetMaterial(materialAsset);
        return a ? a->doubleSided : false;
    }

    int32_t GetRenderQueue() const
    {
        const auto* a = asset::AssetManager::GetMaterial(materialAsset);
        return a ? a->renderQueue : renderer::RenderQueue::GEOMETRY;
    }

    const std::string& GetShaderPath() const
    {
        static const std::string empty;
        const auto* a = asset::AssetManager::GetMaterial(materialAsset);
        return a ? a->shaderPath : empty;
    }

    asset::MeshType GetMeshType() const
    {
        const auto* a = asset::AssetManager::GetMaterial(materialAsset);
        return a ? a->meshType : asset::MeshType::Any;
    }

    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("materialPath", materialPath);
    }
};

} // namespace fbzz::scene
