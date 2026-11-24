/// @file    RenderParticleMaterials.cpp
/// @brief   粒子材質・シェーダー定数・効果テクスチャの解決とキャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include "RenderParticleExtraction.hpp"
#include "../../../Scene/Systems/VFX/ParticleSimulationHelpers.hpp"
#include <Engine/Rendering/Extraction/Effects/RenderParticleExtractor.hpp>
#include <Engine/Rendering/Passes/RenderPassContext.hpp>
#include <Engine/Scene/Components/Particles/ParticleEmitter.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/VFX/ParticleSimulationRuntime.hpp>
#include <Engine/Scene/Components/Particles/ParticleColorSpace.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Material/MaterialAsset.hpp>
#include <Engine/Asset/Material/MaterialParamBinding.hpp>
#include <Engine/Asset/Streaming/StreamedTextureResolver.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>
#include <Graphics/Renderer/Resources/ResourceManager.hpp>
#include <Graphics/Renderer/Resources/OwnedResource.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene::particle_extraction_detail {

/// @brief カスタムシェーダーが宣言した MaterialConstants (b2) 1 本ぶんの解決結果。
/// @brief 解決にはシェーダーのロードとリフレクションが要るので .mat 単位でキャッシュする。
/// @brief 値で持つのは下のキャッシュの要素としてしか存在しないから (unordered_map はノード単位で
/// @brief 確保するので rehash しても要素のアドレスは動かない)。
struct ParticleMaterialBinding {
    renderer::ShaderDescriptor                            descriptor;
    renderer::OwnedResource<renderer::ConstantBufferTag> paramsCB;
    std::vector<uint8_t>                                  paramData;
    /// @brief paramsCB を確保したときのサイズ。シェーダーのホットリロードで MaterialConstants の
    /// @brief 大きさが変わったら作り直す必要がある (古い容量のまま書くと末尾が落ちる)。
    uint32_t                                              paramsCBSize = 0;
    /// @brief このパス呼び出しで既に値を適用したか。.mat の編集を絵へ出しつつ、
    /// @brief 同じ .mat を共有するエミッターぶん解決をやり直さないための通番。
    uint64_t                                              resolvedPass = 0;
    bool                                                  resolvedOk   = false;
};

struct ParticleMaterialCache {
    uint64_t m_resourceOwner = 0, m_resetVersion = 0, m_passSerial = 0;
    std::unordered_map<std::string, ParticleMaterialBinding> m_materials;
};

namespace {

using namespace particle_simulation_detail;

std::unordered_set<std::string> g_warnedParticleMaterials;

bool WarnParticleMaterialOnce(const std::string& path)
{
    return g_warnedParticleMaterials.insert(path).second;
}

using ParticleCacheList = std::vector<std::unique_ptr<ParticleMaterialCache>>;
std::unordered_map<uint64_t, ParticleCacheList> s_particleMaterialCaches;
ParticleMaterialCache& AcquireParticleCache(Scene& scene, renderer::ResourceManager& resources)
{
    auto& caches = s_particleMaterialCaches[scene.GetRenderSceneGeneration()];
    std::erase_if(caches, [](const auto& cache) {
        auto* owner = renderer::ResourceManager::FindOwner(cache->m_resourceOwner);
        return !owner || owner->GetResetVersion() != cache->m_resetVersion;
    });
    for (auto& cache : caches) if (cache->m_resourceOwner == resources.GetInstanceId()) return *cache;
    auto cache = std::make_unique<ParticleMaterialCache>();
    cache->m_resourceOwner = resources.GetInstanceId(); cache->m_resetVersion = resources.GetResetVersion();
    caches.push_back(std::move(cache));
    return *caches.back();
}

/// @brief .mat の [params] をカスタムシェーダーの MaterialConstants へ束縛し、b2 へ流す定数バッファを返す。
/// @brief 組み込みシェーダー (MaterialConstants を宣言しない) では無効ハンドルを返す。
/// @brief 束縛規則そのものは asset::MaterialParamBinding が持つ — メッシュ / UI / デカールと同じ経路。
renderer::ResourceHandle<renderer::ConstantBufferTag> ResolveParticleMaterialParams(
    ParticleMaterialCache& cache, renderer::ResourceManager& resources,
    const std::string& materialPath,
    const asset::MaterialAssetAuthoring& material,
    renderer::ResourceHandle<renderer::ShaderTag> shader)
{
    if (!shader.IsValid()) return {};

    ParticleMaterialBinding& binding = cache.m_materials[materialPath];
    if (binding.resolvedPass == cache.m_passSerial)
        return binding.resolvedOk ? binding.paramsCB.Get() : renderer::ResourceHandle<renderer::ConstantBufferTag>{};
    binding.resolvedPass = cache.m_passSerial;
    binding.resolvedOk   = false;

    /// @note 記述子は値で持つ。シェーダーはホットリロードで差し替わりうるので、
    /// @note ポインタで持つと解決時の中身と食い違う瞬間ができる。
    binding.descriptor = {};
    if (auto* compiled = resources.Get(shader))
        binding.descriptor = compiled->GetDescriptor();
    /// @note MaterialConstants を宣言していないシェーダーは cbufferSize が 0 (= IsValid() が false)。
    /// @note その場合 b2 は何も束縛しない — 組み込み Particle.hlsl がこれに当たる。
    if (!binding.descriptor.IsValid()) { binding.paramsCB.Reset(); binding.paramsCBSize = 0; return {}; }

    binding.paramData.assign(binding.descriptor.cbufferSize, uint8_t{ 0 });
    asset::InitDefaultMaterialParams(binding.descriptor, binding.paramData);
    asset::ApplyMaterialAssetParams(material, binding.descriptor, binding.paramData);

    /// @note IsValid() だけでは判定できない: このキャッシュはシーンの寿命もデバイスリセットも跨ぎ、
    /// @note ハンドルの体裁は残るため、実体が居るかどうかで作り直しを判断する。
    const bool cbLive = resources.Get(binding.paramsCB) != nullptr;
    if (!cbLive) {
        binding.paramsCB = {};
    } else if (binding.paramsCBSize != binding.descriptor.cbufferSize) {
        binding.paramsCB.Reset();
    }
    if (!binding.paramsCB.IsValid()) {
        binding.paramsCB = resources.CreateConstantBuffer(binding.descriptor.cbufferSize);
        binding.paramsCBSize = binding.descriptor.cbufferSize;
    }
    if (!binding.paramsCB.IsValid()) return {};
    resources.Update(binding.paramsCB, binding.paramData.data(),
                     static_cast<uint32_t>(binding.paramData.size()));

    binding.resolvedOk = true;
    return binding.paramsCB;
}

renderer::ResourceHandle<renderer::TextureTag> LoadParticleTextureOrWhite(
    renderer::ResourceManager& resources,
    const std::string& texturePath)
{
    /// @note Particle は色カーブだけでも成立する VFX なので、参照先テクスチャの欠落で
    /// @note DrawCall 全体を無効化せず、白テクスチャにフォールバックして色だけは表示する。
    if (!texturePath.empty()) {
        auto texture = asset::StreamedTextureResolver::Engine().ResolveGpu(resources, texturePath);
        if (texture.IsValid())
            return texture;
    }

    /// @note 白テクスチャは共有の 1 枚を返す。エミッターごとに作ると Despawn / Stop で
    /// @note 返却されないまま漏れる (LoadTexture 由来のハンドルを返すと共有キャッシュを壊すため)。
    return resources.GetWhiteTexture();
}

/// @brief .mat の [textures] から 1 スロット引く。未設定なら空文字列。
const std::string& ParticleMaterialTexture(const asset::MaterialAssetAuthoring& material, const char* slot)
{
    static const std::string kEmpty;
    const auto it = material.textures.find(slot);
    return (it != material.textures.end()) ? it->second : kEmpty;
}

} /// @note namespace

ParticleMaterialCache& BeginParticleMaterialPass(Scene& scene, renderer::ResourceManager& resources)
{
    auto& cache = AcquireParticleCache(scene, resources);
    ++cache.m_passSerial;
    return cache;
}

/// @brief 陰影の受け口。b3 (平行光・環境光) と 6-way の Negative (t3) は常に、点光源は .mat が求めたときだけ。
/// @brief 点光源は DeferredPasses の Lighting と同じ束縛: b9 + t29/t30 (Legacy では束縛しない。b9 が未束縛だと
/// @brief clusterLightMode = Legacy として読まれ、b3 の固定長配列へ落ちる) と、影・Cookie の b12 + t28/t31。
void PrepareParticleAppearance(ParticleEmitter& emitter, renderer::ResourceManager& resources, ParticleMaterialCache& cache)
{
    PrepareParticleSimulationInputs(emitter);
    /// @note 見た目のテクスチャは 3 枚とも .mat の [textures] から来る。
    /// @note albedo = 素材 / normal = 歪みベクトル専用マップ / tex5 = Motion Vector アトラス
    /// @note 未設定なら既定 .mat へ落とす。1x1 白は alpha=1 なので、そのまま描くと粒子が
    /// @note 「不透明な四角」になり、素材の付け忘れが最も分かりにくい形で表に出る。
    /// @note 既定 .mat が無いプロジェクトでは `Load<MaterialAsset>` が失敗し、従来どおり白へ落ちる。
    const bool usingFallback = emitter.settings.materialPath.empty();
    const std::string materialPath =
        usingFallback ? std::string(PARTICLE_FALLBACK_MATERIAL) : emitter.settings.materialPath;
    {
        const bool materialChanged = (emitter.runtime.loadedMaterialPath != materialPath);
        if (materialChanged) {
            emitter.runtime.loadedMaterialPath = materialPath;
            /// @note .mat の変更でテクスチャも再ロードさせる
            emitter.runtime.loadedTexturePath.clear();
        }
        const auto materialHandle = asset::AssetManager::Load<asset::MaterialAsset>(materialPath);
        if (const auto materialSnapshot = asset::ReadMaterialAuthoring(materialHandle); materialSnapshot) {
            const auto* authoring = &materialSnapshot->Data();
            const std::string& albedoPath = ParticleMaterialTexture(*authoring, "albedo");
            {
                emitter.runtime.texture = LoadParticleTextureOrWhite(resources, albedoPath);
                emitter.runtime.loadedTexturePath = albedoPath;
                emitter.runtime.textureIsSrgb = IsEffectTextureSrgb(albedoPath);
            }
            /// @note 歪みベクトル専用マップ (normal)。未設定なら無効ハンドルのままにして、
            /// @note シェーダー側は effectsFlags を見て albedo の RG へ縮退する。
            const std::string& distortionPath = ParticleMaterialTexture(*authoring, "normal");
            {
                emitter.runtime.distortionTexture = distortionPath.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, distortionPath);
                emitter.runtime.loadedDistortionTexturePath = distortionPath;
            }
            /// @note Motion Vector アトラス (tex5)。
            const std::string& motionVectorPath = ParticleMaterialTexture(*authoring, "tex5");
            {
                emitter.runtime.motionVectorTexture = motionVectorPath.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, motionVectorPath);
                emitter.runtime.loadedMotionVectorTexturePath = motionVectorPath;
            }
            /// @note 6 方向ライトマップの Negative (emissive)。sixWayMaps のときだけ読む。
            static const std::string kNoTexture;
            const std::string& sixWayTex = authoring->particle.sixWayMaps ? ParticleMaterialTexture(*authoring, "emissive")
                                                                    : kNoTexture;
            {
                emitter.runtime.sixWayNegativeTexture = sixWayTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, sixWayTex);
                emitter.runtime.loadedSixWayNegativeTexturePath = sixWayTex;
            }
            const std::string& sixWayColorTex = authoring->particle.sixWayMaps
                ? ParticleMaterialTexture(*authoring, "six_way_color") : kNoTexture;
            emitter.runtime.sixWayAlbedoColorTexture = sixWayColorTex.empty()
                ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                : LoadParticleTextureOrWhite(resources, sixWayColorTex);
            emitter.runtime.loadedSixWayAlbedoColorTexturePath = sixWayColorTex;
            const std::string& sixWayEmissionTex = authoring->particle.sixWayMaps
                ? ParticleMaterialTexture(*authoring, "six_way_emission") : kNoTexture;
            emitter.runtime.sixWayEmissionColorTexture = sixWayEmissionTex.empty()
                ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                : LoadParticleTextureOrWhite(resources, sixWayEmissionTex);
            emitter.runtime.loadedSixWayEmissionColorTexturePath = sixWayEmissionTex;
            /// @note .mat が shader を指定していれば描画シェーダーを差し替える。
            /// @note 既定 .mat では見ない — 未設定にした瞬間に描画が化けるのは避ける (blendMode と同じ)。
            /// @note render_path を要求するのは、頂点が ParticleVertex (92B)・b2 が
            /// @note ParticleRenderConstants で、メッシュ用シェーダーを割り当てると
            /// @note «クラッシュせず静かに壊れた絵» になるため。UI / Decal と同じ判断。
            const bool declaredForParticles = (authoring->renderPath == asset::RenderPath::Particle);
            if (!usingFallback && !declaredForParticles && !authoring->shaderPath.empty()
                && WarnParticleMaterialOnce(materialPath)) {
                FBZZ_LOG_WARN("Particle material '%s' declares shader '%s' but is not declared for "
                              "particles (render_path must be \"particle\") -> ignoring the shader "
                              "and drawing with the built-in particle shader.",
                              materialPath.c_str(), authoring->shaderPath.c_str());
            }
            const std::string shaderPath =
                (usingFallback || !declaredForParticles) ? std::string{} : authoring->shaderPath;
            if (emitter.runtime.loadedShaderPath != shaderPath
                || (!shaderPath.empty() && !resources.Get(emitter.runtime.customShader))) {
                emitter.runtime.loadedShaderPath = shaderPath;
                emitter.runtime.customShader = shaderPath.empty()
                    ? renderer::ResourceHandle<renderer::ShaderTag>{}
                    : resources.LoadShader(shaderPath);
                /// @note 黙って組み込みへ落ちると、テクスチャを持たない .mat では «白い四角» が並ぶ。
                /// @note 手続きシェーダーほどテクスチャを持たないので、一番起きやすい失敗が
                /// @note 一番分かりにくい絵になる。
                if (!shaderPath.empty() && !emitter.runtime.customShader.IsValid()
                    && WarnParticleMaterialOnce(materialPath + "|shader|" + shaderPath)) {
                    FBZZ_LOG_WARN("Particle material '%s' references shader '%s' but it failed to "
                                  "load (not compiled?). Falling back to the built-in particle "
                                  "shader; a material without an albedo texture will draw as "
                                  "white quads.",
                                  materialPath.c_str(), shaderPath.c_str());
                }
            }
            /// @note カスタムシェーダーが MaterialConstants を宣言していれば .mat の [params] を流す。
            /// @note 毎フレーム引き直すのは編集結果を即座に絵へ出すため (解決は .mat 単位で 1 回)。
            emitter.runtime.materialParamsCB = ResolveParticleMaterialParams(
                cache, resources, materialPath, *authoring, emitter.runtime.customShader);
            return;
        }
    }

    /// @note .mat を解決できなかった。1x1 白 + 既定の見た目で描き続ける
    /// @note (エフェクトが丸ごと消えるより、素材が付いていないと分かる方がよい)。
    emitter.runtime.distortionTexture = {};
    emitter.runtime.motionVectorTexture = {};
    emitter.runtime.sixWayNegativeTexture = {};
    /// @note 正常材質から読込失敗へ移った場合も、前材質のシェーダー・色・補助画像を残さない。
    emitter.runtime.customShader = {};
    emitter.runtime.loadedShaderPath.clear();
    emitter.runtime.materialParamsCB = {};
    emitter.runtime.sixWayAlbedoColorTexture = {};
    emitter.runtime.sixWayEmissionColorTexture = {};
    emitter.runtime.loadedDistortionTexturePath.clear();
    emitter.runtime.loadedMotionVectorTexturePath.clear();
    emitter.runtime.loadedSixWayNegativeTexturePath.clear();
    emitter.runtime.loadedSixWayAlbedoColorTexturePath.clear();
    emitter.runtime.loadedSixWayEmissionColorTexturePath.clear();
    {
        emitter.runtime.texture = LoadParticleTextureOrWhite(resources, {});
        emitter.runtime.loadedTexturePath.clear();
        /// @note 1x1 白フォールバック。リニアでも sRGB でも 1.0 は 1.0 なので変換しない。
        emitter.runtime.textureIsSrgb = false;
    }
}

void ReleaseParticleMaterialCaches(Scene& scene)
{
    s_particleMaterialCaches.erase(scene.GetRenderSceneGeneration());
}

} /// @note namespace fbzz::scene::particle_extraction_detail

namespace fbzz::scene {

void ReleaseParticleRenderResources(Scene& scene)
{
    particle_extraction_detail::ReleaseParticleMaterialCaches(scene);
}

} /// @note namespace fbzz::scene
