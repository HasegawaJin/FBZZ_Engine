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
#include <unordered_map>
#include <vector>

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
        , paramOverrides(o.paramOverrides)
        , textureOverrides(o.textureOverrides)
        , propertyNameCache(o.propertyNameCache)
        , hasBlendModeOverride(o.hasBlendModeOverride)
        , blendModeOverride(o.blendModeOverride)
        , hasDoubleSidedOverride(o.hasDoubleSidedOverride)
        , doubleSidedOverride(o.doubleSidedOverride)
        , hasRenderQueueOverride(o.hasRenderQueueOverride)
        , renderQueueOverride(o.renderQueueOverride)
    {}
    MaterialComponent& operator=(const MaterialComponent& o)
    {
        if (this != &o) {
            material     = o.material ? std::make_unique<renderer::Material>(*o.material) : nullptr;
            materialAsset = o.materialAsset;
            enabled      = o.enabled;
            materialPath = o.materialPath;
            paramOverrides = o.paramOverrides;
            textureOverrides = o.textureOverrides;
            propertyNameCache = o.propertyNameCache;
            propertyValidationCache.clear();
            propertyValidationDescriptor = nullptr;
            hasBlendModeOverride = o.hasBlendModeOverride;
            blendModeOverride = o.blendModeOverride;
            hasDoubleSidedOverride = o.hasDoubleSidedOverride;
            doubleSidedOverride = o.doubleSidedOverride;
            hasRenderQueueOverride = o.hasRenderQueueOverride;
            renderQueueOverride = o.renderQueueOverride;
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

    // オブジェクトごとのパラメータ上書き (シェーダー変数名 → float 値配列)。
    // WHY: MaterialAsset はパス単位で共有されるため、そこへ書くと同じ .mat を使う全インスタンスへ
    //      波及する。ここに積むと SyncMaterial が共有アセット適用後に「この GO 専用」で上書きするので、
    //      ディゾルブ量や色などをインスタンス単位でアニメーションできる。ランタイム専用 (非シリアライズ)。
    std::unordered_map<std::string, std::vector<float>> paramOverrides;
    // Textureと描画状態も共有.matを変更せず、GameObject単位で上書きする。
    std::unordered_map<std::string, std::string> textureOverrides;
    // PropertyId のhashから実名を引き、毎フレームの文字列生成と線形検索を避ける。
    // hash衝突時は呼び出し側が実名を照合して上書きする。
    std::unordered_map<uint64_t, std::string> propertyNameCache;
    // bitはMaterialInstanceのPropertyKindごとのShader reflection検証済み状態。
    std::unordered_map<uint64_t, uint8_t> propertyValidationCache;
    // Shader descriptorがhot reloadで差し替わったら検証cacheを破棄する非所有識別子。
    const void* propertyValidationDescriptor = nullptr;
    bool hasBlendModeOverride = false;
    renderer::BlendMode blendModeOverride = renderer::BlendMode::OPAQUE_BLEND;
    bool hasDoubleSidedOverride = false;
    bool doubleSidedOverride = false;
    bool hasRenderQueueOverride = false;
    int32_t renderQueueOverride = renderer::RenderQueue::GEOMETRY;

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
        if (hasBlendModeOverride) return blendModeOverride;
        const auto* a = asset::AssetManager::GetMaterial(materialAsset);
        return a ? a->blendMode : renderer::BlendMode::OPAQUE_BLEND;
    }

    bool IsDoubleSided() const
    {
        if (hasDoubleSidedOverride) return doubleSidedOverride;
        const auto* a = asset::AssetManager::GetMaterial(materialAsset);
        return a ? a->doubleSided : false;
    }

    int32_t GetRenderQueue() const
    {
        if (hasRenderQueueOverride) return renderQueueOverride;
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
