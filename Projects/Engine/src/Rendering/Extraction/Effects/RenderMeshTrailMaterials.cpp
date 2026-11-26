/// @file    RenderMeshTrailMaterials.cpp
/// @brief   メッシュ残像の材質・テクスチャ・定数バッファ。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderMeshTrailExtraction.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderMeshTrailExtractor.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Material/MaterialParamBinding.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Resources/OwnedResource.hpp>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene::mesh_trail_extraction_detail {

/// @brief `.mat` が宣言したカスタムシェーダーと [params] の解決結果。
/// @note `.mat` 単位でキャッシュする。解決にはロードとリフレクションが要り、部位ごとに
/// @note 同じ `.mat` を張る残像を実体ごとに引くと 1 フレームでその回数ぶん繰り返す。
struct TrailMaterialBinding {
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    std::string                                           loadedShaderPath;
    renderer::ShaderDescriptor                            descriptor;
    renderer::ResourceHandle<renderer::ConstantBufferTag> paramsCB;
    std::vector<uint8_t>                                  paramData;
    /// @note paramsCB を確保したときのサイズ。ホットリロードで MaterialConstants の
    /// @note 大きさが変わったら作り直す (古い容量のまま書くと末尾が落ちる)。
    uint32_t                                              paramsCBSize = 0;
    asset::MeshType                                       meshType     = asset::MeshType::Any;
    uint64_t                                              resolvedPass = 0;
};

struct TrailMaterialCache {
    TrailMaterialCache() = default;
    TrailMaterialCache(const TrailMaterialCache&) = delete;
    TrailMaterialCache& operator=(const TrailMaterialCache&) = delete;
    uint64_t m_resourceOwner = 0, m_resetVersion = 0, m_passSerial = 0;
    std::unordered_map<std::string, TrailMaterialBinding> m_materials;
    std::unordered_set<std::string> m_warnedMaterials;
    ~TrailMaterialCache()
    {
        auto* resources = renderer::ResourceManager::FindOwner(m_resourceOwner);
        if (!resources || resources->GetResetVersion() != m_resetVersion) {
            return;
        }
        for (auto& [path, binding] : m_materials) {
            resources->Release(binding.paramsCB);
        }
    }
};

namespace {

/// @brief MeshTrailConstants (cbuffer b6) の C++ ミラー。
/// @note b2 ではなく b6: シェーダーリフレクションが "MaterialConstants" を b2 に探すため、
/// @note 占有すると残像材質だけ `.mat` の [params] を持てなくなる (Decal/Particle と同様。`Binding.hlsli` 参照)。
struct MeshTrailCB {
    math::Vector4 trailColor;
};

static_assert(sizeof(MeshTrailCB) == 16, "MeshTrailCB layout mismatch");

/// @note 個体に保持したバッファは現在の描画先ではなく、記録した作成元へ返す。
void ReleaseOwnedBuffer(renderer::ResourceHandle<renderer::ConstantBufferTag>& handle)
{
    renderer::ReleaseOwnedResource(handle);
    handle = {};
}

using TrailMaterialCaches = std::vector<std::unique_ptr<TrailMaterialCache>>;
std::unordered_map<uint64_t, TrailMaterialCaches> s_meshTrailCaches;

TrailMaterialCache& AcquireTrailCache(Scene& scene, renderer::ResourceManager& resources)
{
    auto& caches = s_meshTrailCaches[scene.GetRenderSceneGeneration()];
    std::erase_if(caches, [](const auto& cache) {
        auto* owner = renderer::ResourceManager::FindOwner(cache->m_resourceOwner);
        return !owner || owner->GetResetVersion() != cache->m_resetVersion;
    });
    for (auto& cache : caches)
        if (cache->m_resourceOwner == resources.GetInstanceId()) {
            return *cache;
        }
    auto cache = std::make_unique<TrailMaterialCache>();
    cache->m_resourceOwner = resources.GetInstanceId();
    cache->m_resetVersion = resources.GetResetVersion();
    caches.push_back(std::move(cache));
    return *caches.back();
}

bool WarnTrailMaterialOnce(TrailMaterialCache& cache, const std::string& path)
{
    return cache.m_warnedMaterials.insert(path).second;
}

/// @brief `.mat` の shader と [params] を解決する。
/// @return カスタムシェーダーが無ければ nullptr。
/// @note render_path = "trail" を要求する: 残像の b6 は MeshTrailConstants、頂点は
/// @note (Skinned なら) ボーン付きで来る。メッシュ用の `.mat` を割り当てると「静かに
/// @note 壊れた絵」になるため、UI/Decal/Particle と同じ判断で宣言を要求する。

bool TrailShaderMatchesMesh(const TrailMaterialBinding* binding, bool skinned)
{
    if (!binding || !binding->shader.IsValid()) {
        return false;
    }
    return skinned ? (binding->meshType != asset::MeshType::Surface)
                   : (binding->meshType != asset::MeshType::Skinned);
}

} // namespace

const TrailMaterialBinding* ResolveTrailMaterialBinding(
    TrailMaterialCache& cache, renderer::ResourceManager& resources, const std::string& materialPath)
{
    if (materialPath.empty()) {
        return nullptr;
    }

    TrailMaterialBinding& binding = cache.m_materials[materialPath];
    if (binding.resolvedPass == cache.m_passSerial) {
        return resources.Get(binding.shader) ? &binding : nullptr;
    }
    binding.resolvedPass = cache.m_passSerial;

    const auto materialHandle = asset::AssetManager::Load<asset::MaterialAsset>(materialPath);
    const auto materialSnapshot = asset::ReadMaterialAuthoring(materialHandle);

    const auto* authoring = materialSnapshot ? &materialSnapshot->Data() : nullptr;
    if (!authoring) {
        return nullptr;
    }

    const bool declaredForTrails = (authoring->renderPath == asset::RenderPath::Trail);
    if (!declaredForTrails && !authoring->shaderPath.empty()
        && WarnTrailMaterialOnce(cache, materialPath)) {
        FBZZ_LOG_WARN("Mesh trail material '%s' declares shader '%s' but is not declared for "
                      "trails (render_path must be \"trail\") -> ignoring the shader and "
                      "drawing with the built-in mesh trail shader.",
                      materialPath.c_str(), authoring->shaderPath.c_str());
    }
    const std::string shaderPath = declaredForTrails ? authoring->shaderPath : std::string{};
    binding.meshType = authoring->meshType;

    if (binding.loadedShaderPath != shaderPath || !resources.Get(binding.shader)) {
        binding.loadedShaderPath = shaderPath;
        binding.shader = shaderPath.empty()
            ? renderer::ResourceHandle<renderer::ShaderTag>{}
            : resources.LoadShader(shaderPath);
        // 黙って組み込みへ落ちると «前と同じ絵» が出るだけで、材質を書いた側からは
        // 「効いていない」としか見えない。コンパイル漏れが一番起きやすい。
        if (!shaderPath.empty() && !binding.shader.IsValid()
            && WarnTrailMaterialOnce(cache, materialPath + "|load")) {
            FBZZ_LOG_WARN("Mesh trail material '%s' references shader '%s' but it failed to load "
                          "(not compiled?). Falling back to the built-in mesh trail shader.",
                          materialPath.c_str(), shaderPath.c_str());
        }
    }
    if (!resources.Get(binding.shader)) {
        ReleaseOwnedBuffer(binding.paramsCB);
        binding.paramsCBSize = 0;
        return nullptr;
    }

    // MaterialConstants を宣言していないシェーダーは cbufferSize が 0 (= IsValid() が false)。
    // その場合 b2 へは何も束縛しない。束縛規則そのものは asset::MaterialParamBinding が持つ。
    binding.descriptor = {};
    if (auto* compiled = resources.Get(binding.shader)) {
        binding.descriptor = compiled->GetDescriptor();
    }
    if (!binding.descriptor.IsValid()) {
        ReleaseOwnedBuffer(binding.paramsCB);
        binding.paramsCBSize = 0;
        return &binding;
    }

    binding.paramData.assign(binding.descriptor.cbufferSize, uint8_t{ 0 });
    asset::InitDefaultMaterialParams(binding.descriptor, binding.paramData);
    asset::ApplyMaterialAssetParams(*authoring, binding.descriptor, binding.paramData);

    // IsValid() だけでは判定できない: このキャッシュはシーンの寿命もデバイスリセットも跨ぎ、
    // ハンドルの体裁は残るため、実体が居るかどうかで作り直しを判断する。
    const bool cbLive = resources.Get(binding.paramsCB) != nullptr;
    if (!cbLive) {
        binding.paramsCB = {};
    } else if (binding.paramsCBSize != binding.descriptor.cbufferSize) {
        resources.Release(binding.paramsCB);
        binding.paramsCB = {};
    }
    if (!binding.paramsCB.IsValid()) {
        binding.paramsCB = resources.CreateConstantBuffer(binding.descriptor.cbufferSize);
        binding.paramsCBSize = binding.descriptor.cbufferSize;
    }
    if (binding.paramsCB.IsValid()) {
        resources.Update(binding.paramsCB, binding.paramData.data(),
                         static_cast<uint32_t>(binding.paramData.size()));
    }
    return &binding;
}

/// @note カスタムシェーダーをこの描画へ当ててよいか。VS の頂点入力が食い違うと
/// @note «落ちずに崩れた絵» になるので、.mat の mesh_type 宣言で弾く。

TrailMaterialCache& BeginMeshTrailMaterialPass(Scene& scene, renderer::ResourceManager& resources)
{
    auto& cache = AcquireTrailCache(scene, resources);
    ++cache.m_passSerial;
    return cache;
}

void PrepareMeshTrailAppearance(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    if (!resources.Get(trail.meshTrailCB)) {
        trail.meshTrailCB.Reset();
        trail.meshTrailCB = resources.CreateConstantBuffer(sizeof(MeshTrailCB));
    }

    // materialPath が設定されている場合: .mat の albedo テクスチャと doubleSided を優先する。
    trail.texture = resources.GetWhiteTexture();
    if (!trail.materialPath.empty()) {
        const bool materialChanged = (trail.loadedMaterialPath != trail.materialPath);
        if (materialChanged) {
            trail.loadedMaterialPath = trail.materialPath;
            trail.loadedTexturePath.clear();
        }
        const auto materialHandle = asset::AssetManager::Load<asset::MaterialAsset>(trail.materialPath);
        if (const auto materialSnapshot = asset::ReadMaterialAuthoring(materialHandle); materialSnapshot) {
            const auto* authoring = &materialSnapshot->Data();
            const auto it = authoring->textures.find("albedo");
            const std::string& albedoPath = (it != authoring->textures.end()) ? it->second : std::string{};
            {
                if (albedoPath.empty()) {
                    trail.texture = resources.GetWhiteTexture();
                } else {
                    trail.texture = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, albedoPath);
                }
                trail.loadedTexturePath = albedoPath;
            }
            trail.doubleSided = authoring->doubleSided;
        }
    } else {
        // 共有の 1 枚を借りる。実体ごとに作ると、その実体が畳まれたぶんだけ GPU に残る。
        trail.texture = resources.GetWhiteTexture();
        trail.loadedTexturePath.clear();
    }
}

void ApplyMeshTrailMaterial(const TrailMaterialBinding* material, bool skinned, renderer::RenderMeshTrailInput& input)
{
    if (!TrailShaderMatchesMesh(material, skinned)) {
        return;
    }
    input.shader = material->shader;
    input.material = material->paramsCB;
}

void ClearMeshTrailMaterialCaches(Scene& scene)
{
    s_meshTrailCaches.erase(scene.GetRenderSceneGeneration());
}

} // namespace fbzz::scene::mesh_trail_extraction_detail

namespace fbzz::scene {

void ReleaseMeshTrailRenderResources(Scene& scene)
{
    mesh_trail_extraction_detail::ClearMeshTrailMaterialCaches(scene);
}

} // namespace fbzz::scene
