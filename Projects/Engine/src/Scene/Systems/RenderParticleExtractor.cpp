/// @file    RenderParticleExtractor.cpp
/// @brief   パーティクルの GPU ディスパッチと描画用スナップショット。
/// @author  Hasegawa Jin
/// @date    2026-06-18

#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderEntitySlotMask.hpp"
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include "RenderPasses/Geometry/FlowFieldGpu.hpp"
#include <Engine/Scene/Systems/ParticleOverdrawStats.hpp>
#include <Engine/Scene/Systems/ParticleSimulationRuntime.hpp>
#include "Engine/Scene/Scene.hpp"
#include <Engine/Scene/SceneRenderResources.hpp>
#include <Graphics/Renderer/OwnedResource.hpp>
#include "ParticleSimulationHelpers.hpp"
#include "ParticleRandom.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/ParticleEmitter.hpp"
#include "Engine/Scene/Fields/FlowField.hpp"
#include "Engine/Scene/Components/ParticleGpuSimulation.hpp"
#include "Engine/Scene/Components/MeshRenderer.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/MaterialParamBinding.hpp"
#include "Fluid/VectorFieldAsset.hpp"
#include "Engine/Asset/VelocityFieldAtlas.hpp"
#include <Engine/Scene/Components/ParticleEmitterSpace.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include "Engine/Renderer/ShaderDescriptor.hpp"
#include "Engine/Asset/Model.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/ComputeCall.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/DynamicBufferPool.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include <Math/MathUtils.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {
using namespace particle_runtime_detail;
using namespace particle_simulation_detail;

std::unordered_set<std::string> g_warnedParticleMaterials;

bool WarnParticleMaterialOnce(const std::string& path)
{
    return g_warnedParticleMaterials.insert(path).second;
}

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
    uint64_t m_owner = 0, m_epoch = 0, m_pass = 0;
    std::unordered_map<std::string, ParticleMaterialBinding> m_materials;
};
using ParticleCacheList = std::vector<std::unique_ptr<ParticleMaterialCache>>;
std::unordered_map<uint64_t, ParticleCacheList> g_particleCaches;
ParticleMaterialCache& AcquireParticleCache(Scene& scene, renderer::ResourceManager& resources)
{
    auto& caches = g_particleCaches[scene.GetRenderSceneGeneration()];
    std::erase_if(caches, [](const auto& cache) {
        auto* owner = renderer::ResourceManager::FindOwner(cache->m_owner);
        return !owner || owner->GetResetVersion() != cache->m_epoch;
    });
    for (auto& cache : caches) if (cache->m_owner == resources.GetInstanceId()) return *cache;
    auto cache = std::make_unique<ParticleMaterialCache>();
    cache->m_owner = resources.GetInstanceId(); cache->m_epoch = resources.GetResetVersion();
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
    if (binding.resolvedPass == cache.m_pass)
        return binding.resolvedOk ? binding.paramsCB.Get() : renderer::ResourceHandle<renderer::ConstantBufferTag>{};
    binding.resolvedPass = cache.m_pass;
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

/// @brief 陰影の受け口。b3 (平行光・環境光) と 6-way の Negative (t3) は常に、点光源は .mat が求めたときだけ。
/// @brief 点光源は DeferredPasses の Lighting と同じ束縛: b9 + t29/t30 (Legacy では束縛しない。b9 が未束縛だと
/// @brief clusterLightMode = Legacy として読まれ、b3 の固定長配列へ落ちる) と、影・Cookie の b12 + t28/t31。
void EnsureParticleTexture(ParticleEmitter& emitter, renderer::ResourceManager& resources, ParticleMaterialCache& cache)
{
    PrepareParticleSimulationInputs(emitter);
    /// @note 見た目のテクスチャは 3 枚とも .mat の [textures] から来る。
    /// @note albedo = 素材 / normal = 歪みベクトル専用マップ / tex5 = Motion Vector アトラス
    /// @note 未設定なら既定 .mat へ落とす。1x1 白は alpha=1 なので、そのまま描くと粒子が
    /// @note 「不透明な四角」になり、素材の付け忘れが最も分かりにくい形で表に出る。
    /// @note 既定 .mat が無いプロジェクトでは `Load<MaterialAsset>` が失敗し、従来どおり白へ落ちる。
    const bool usingFallback = emitter.settings.materialPath.empty();
    const std::string resolvedMaterial =
        usingFallback ? std::string(PARTICLE_FALLBACK_MATERIAL) : emitter.settings.materialPath;
    {
        const bool matChanged = (emitter.runtime.loadedMaterialPath != resolvedMaterial);
        if (matChanged) {
            emitter.runtime.loadedMaterialPath = resolvedMaterial;
            /// @note .mat の変更でテクスチャも再ロードさせる
            emitter.runtime.loadedTexturePath.clear();
        }
        const auto matHandle = asset::AssetManager::Load<asset::MaterialAsset>(resolvedMaterial);
        if (const auto matAuthoringSnapshot = asset::ReadMaterialAuthoring(matHandle); matAuthoringSnapshot) {
            const auto* mat = &matAuthoringSnapshot->Data();
            const std::string& resolvedTex = ParticleMaterialTexture(*mat, "albedo");
            {
                emitter.runtime.texture = LoadParticleTextureOrWhite(resources, resolvedTex);
                emitter.runtime.loadedTexturePath = resolvedTex;
                emitter.runtime.textureIsSrgb = IsEffectTextureSrgb(resolvedTex);
            }
            /// @note 歪みベクトル専用マップ (normal)。未設定なら無効ハンドルのままにして、
            /// @note シェーダー側は effectsFlags を見て albedo の RG へ縮退する。
            const std::string& distortionTex = ParticleMaterialTexture(*mat, "normal");
            {
                emitter.runtime.distortionTexture = distortionTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, distortionTex);
                emitter.runtime.loadedDistortionTexturePath = distortionTex;
            }
            /// @note Motion Vector アトラス (tex5)。
            const std::string& motionTex = ParticleMaterialTexture(*mat, "tex5");
            {
                emitter.runtime.motionVectorTexture = motionTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, motionTex);
                emitter.runtime.loadedMotionVectorTexturePath = motionTex;
            }
            /// @note 6 方向ライトマップの Negative (emissive)。sixWayMaps のときだけ読む。
            static const std::string kNoTexture;
            const std::string& sixWayTex = mat->particle.sixWayMaps ? ParticleMaterialTexture(*mat, "emissive")
                                                                    : kNoTexture;
            {
                emitter.runtime.sixWayNegativeTexture = sixWayTex.empty()
                    ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                    : LoadParticleTextureOrWhite(resources, sixWayTex);
                emitter.runtime.loadedSixWayNegativeTexturePath = sixWayTex;
            }
            const std::string& sixWayColorTex = mat->particle.sixWayMaps
                ? ParticleMaterialTexture(*mat, "six_way_color") : kNoTexture;
            emitter.runtime.sixWayAlbedoColorTexture = sixWayColorTex.empty()
                ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                : LoadParticleTextureOrWhite(resources, sixWayColorTex);
            emitter.runtime.loadedSixWayAlbedoColorTexturePath = sixWayColorTex;
            const std::string& sixWayEmissionTex = mat->particle.sixWayMaps
                ? ParticleMaterialTexture(*mat, "six_way_emission") : kNoTexture;
            emitter.runtime.sixWayEmissionColorTexture = sixWayEmissionTex.empty()
                ? renderer::ResourceHandle<renderer::TextureTag>::Null()
                : LoadParticleTextureOrWhite(resources, sixWayEmissionTex);
            emitter.runtime.loadedSixWayEmissionColorTexturePath = sixWayEmissionTex;
            /// @note .mat が shader を指定していれば描画シェーダーを差し替える。
            /// @note 既定 .mat では見ない — 未設定にした瞬間に描画が化けるのは避ける (blendMode と同じ)。
            /// @note render_path を要求するのは、頂点が ParticleVertex (92B)・b2 が
            /// @note ParticleRenderConstants で、メッシュ用シェーダーを割り当てると
            /// @note «クラッシュせず静かに壊れた絵» になるため。UI / Decal と同じ判断。
            const bool declaredForParticles = (mat->renderPath == asset::RenderPath::Particle);
            if (!usingFallback && !declaredForParticles && !mat->shaderPath.empty()
                && WarnParticleMaterialOnce(resolvedMaterial)) {
                FBZZ_LOG_WARN("Particle material '%s' declares shader '%s' but is not declared for "
                              "particles (render_path must be \"particle\") -> ignoring the shader "
                              "and drawing with the built-in particle shader.",
                              resolvedMaterial.c_str(), mat->shaderPath.c_str());
            }
            const std::string shaderPath =
                (usingFallback || !declaredForParticles) ? std::string{} : mat->shaderPath;
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
                    && WarnParticleMaterialOnce(resolvedMaterial + "|shader|" + shaderPath)) {
                    FBZZ_LOG_WARN("Particle material '%s' references shader '%s' but it failed to "
                                  "load (not compiled?). Falling back to the built-in particle "
                                  "shader; a material without an albedo texture will draw as "
                                  "white quads.",
                                  resolvedMaterial.c_str(), shaderPath.c_str());
                }
            }
            /// @note カスタムシェーダーが MaterialConstants を宣言していれば .mat の [params] を流す。
            /// @note 毎フレーム引き直すのは編集結果を即座に絵へ出すため (解決は .mat 単位で 1 回)。
            emitter.runtime.materialParamsCB = ResolveParticleMaterialParams(
                cache, resources, resolvedMaterial, *mat, emitter.runtime.customShader);
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

/// @brief GPU シミュレーションへ渡す力場とスポーンの StructuredBuffer。
/// @note エミッターに 1 本ずつ持たせず、シミュレーションのたびにプールから借りる。DX12 の
/// @note 読み取り専用 StructuredBuffer は Upload ヒープへの直 memcpy のため、1 本を毎フレーム
/// @note 書き直すと GPU がまだ読んでいる前フレームの内容を上書きする (詳細は `DynamicBufferPool.hpp`)。
renderer::DynamicStructuredBufferPool g_gpuForcePool;
renderer::DynamicStructuredBufferPool g_gpuSpawnPool;

/// @brief 力場バッファを借りて今フレームの内容を書き込む。
void AcquireGpuForceBuffer(ParticleEmitter& emitter,
                           const std::vector<GpuFlowField>& forces,
                           renderer::ResourceManager& resources)
{
    /// @note 0 本でも 1 要素は借りる。要素数 0 の StructuredBuffer は作れず、
    /// @note 未束縛の SRV は DX12 で null ディスクリプタの次元不一致になる。
    const size_t required = (std::max)(forces.size(), size_t{ 1 });
    emitter.runtime.gpuForceBuffer =
        g_gpuForcePool.Acquire(resources, required, static_cast<uint32_t>(sizeof(GpuFlowField)));
    if (!forces.empty() && emitter.runtime.gpuForceBuffer.IsValid()) {
        resources.Update(emitter.runtime.gpuForceBuffer, forces.data(),
                         forces.size() * sizeof(GpuFlowField));
    }
}

bool ShouldSortGpuParticles(const ParticleEmitter& emitter, const RenderPassHandles& handles)
{
    return emitter.settings.sortMode != ParticleSortMode::None
        && handles.particleGpuSortKeysCS.IsValid()
        && handles.particleGpuSortStepCS.IsValid()
        && handles.particleGpuSortLocalCS.IsValid();
}

ParticleRenderCB BuildParticleRenderConstants(ParticleEmitter& emitter,
                                   renderer::ResourceManager& resources,
                                   int maxParticles,
                                   std::uint32_t screenWidth,
                                   std::uint32_t screenHeight,
                                   bool gpuSortEnabled = false)
{
    if (!resources.Get(emitter.runtime.renderCB)) {
        renderer::ReleaseOwnedResource(emitter.runtime.renderCB);
        emitter.runtime.renderCB = resources.CreateConstantBuffer(sizeof(ParticleRenderCB));
    }
    ParticleRenderCB cb{};
    cb.renderMode = static_cast<uint32_t>(emitter.settings.renderMode);
    cb.stretchedVelocityScale = (std::max)(emitter.settings.stretchedVelocityScale, 0.0f);
    cb.stretchedLengthScale = (std::max)(emitter.settings.stretchedLengthScale, 0.0f);
    cb.softParticleFadeDistance = (std::max)(emitter.runtime.material.softParticleFadeDistance, 0.001f);
    cb.softParticles = emitter.runtime.material.softParticles ? 1u : 0u;
    cb.maxParticles = static_cast<uint32_t>((std::max)(maxParticles, 0));
    /// @note ビット割り当ては Rendering/ParticleCommon.hlsli の FBZZ_PFX_* / FBZZ_PALPHA_* と一致させること。
    cb.effectsFlags = (emitter.runtime.material.distortion ? kParticleFxDistortion : 0u)
        | (emitter.runtime.material.sixWayLighting ? kParticleFxSixWay : 0u)
        | (emitter.runtime.material.flipbook.motionVectorFlipbook && emitter.runtime.motionVectorTexture.IsValid()
               ? kParticleFxMotionVector : 0u)
        | (emitter.runtime.material.receiveShadows ? kParticleFxReceiveShadow : 0u)
        | (emitter.runtime.material.volumetric ? kParticleFxVolumetric : 0u)
        | (emitter.runtime.resolvedBlend == ParticleBlendMode::Premultiplied
               ? kParticleFxPremultiplied : 0u)
        | (emitter.runtime.textureIsSrgb ? kParticleFxSrgbTexture : 0u)
        | (emitter.runtime.distortionTexture.IsValid() ? kParticleFxDistortionMap : 0u)
        | (emitter.runtime.material.punctualLighting ? kParticleFxPunctual : 0u)
        | (emitter.runtime.resolvedBlend == ParticleBlendMode::Additive ? kParticleFxAdditive : 0u)
        | (emitter.runtime.material.sixWayMaps && emitter.runtime.sixWayNegativeTexture.IsValid()
               ? kParticleFxSixWayMaps : 0u)
        | (emitter.runtime.material.sixWayMaps && emitter.runtime.sixWayAlbedoColorTexture.IsValid()
               && emitter.runtime.sixWayEmissionColorTexture.IsValid() ? kParticleFxSixWayColorMaps : 0u)
        | ((static_cast<std::uint32_t>(emitter.runtime.material.alphaSource) & kParticleAlphaMask)
               << kParticleAlphaShift);
    cb.distortionStrength = (std::max)(emitter.runtime.material.distortionStrength, 0.0f);
    cb.distortionChromatic = (std::max)(emitter.runtime.material.distortionChromatic, 0.0f);
    cb.lightingStrength = (std::max)(emitter.runtime.material.lightingStrength, 0.0f);
    cb.smokeWrap = std::clamp(emitter.runtime.material.smokeWrap, 0.0f, 1.0f);
    cb.smokeTransmission = (std::max)(emitter.runtime.material.smokeTransmission, 0.0f);
    /// @note 0 以下だと pow が発散する。シェーダー側でも下限を切るが、値の意味をここで固定する。
    cb.smokeBackScatterPower = (std::max)(emitter.runtime.material.smokeBackScatterPower, 0.1f);
    cb.tintColor = emitter.runtime.materialTint;
    cb.emissiveScale = (std::max)(emitter.runtime.material.emissiveScale, 0.0f);
    const math::Vector3& sixWayEmission = emitter.runtime.material.sixWayEmissionColor;
    cb.sixWayEmission = { (std::max)(sixWayEmission.x, 0.0f), (std::max)(sixWayEmission.y, 0.0f),
                          (std::max)(sixWayEmission.z, 0.0f),
                          (std::max)(emitter.runtime.material.sixWayEmissionScale, 0.0f) };
    cb.motionVectorStrength = (std::max)(emitter.runtime.material.flipbook.motionVectorStrength, 0.0f);
    /// @note 歪みの画面UV算出に使う。0 だとUVが右下隅へ張り付いて屈折が出ない。
    cb.screenWidth = static_cast<float>((std::max)(screenWidth, 1u));
    cb.screenHeight = static_cast<float>((std::max)(screenHeight, 1u));
    /// @note 0 以下だと粒子が潰れて一切見えなくなるため、下限で切って「消える」事故を防ぐ。
    cb.sizeAxisScaleX = (std::max)(emitter.settings.sizeAxisScale.x, 0.0001f);
    cb.sizeAxisScaleY = (std::max)(emitter.settings.sizeAxisScale.y, 0.0001f);
    cb.shadowStrength = std::clamp(emitter.runtime.material.shadowStrength, 0.0f, 1.0f);
    /// @note ステップ数はピクセルあたりのループ回数に直結する。上限を切って
    /// @note 設定ミスで GPU が張り付くのを防ぐ。
    cb.volumetricSteps = static_cast<std::uint32_t>(std::clamp(emitter.runtime.material.volumetricSteps, 1, 64));
    cb.volumetricDensity = (std::max)(emitter.runtime.material.volumetricDensity, 0.0f);
    /// @note g = ±1 は位相関数が発散するため内側へ寄せる。
    cb.volumetricAnisotropy = std::clamp(emitter.runtime.material.volumetricAnisotropy, -0.95f, 0.95f);
    cb.volumetricNoiseScale = (std::max)(emitter.runtime.material.volumetricNoiseScale, 0.0f);
    /// @note GPU 経路の VS がソート済み index (t15) を経由するかどうか。CPU 経路では常に 0。
    cb.gpuSortEnabled = gpuSortEnabled ? 1u : 0u;
    /// @note 自己影。密度バッファ (t9) が無いフレームでも 0 なら参照しないので安全。
    cb.selfShadowStrength = (std::max)(emitter.runtime.material.selfShadowStrength, 0.0f);
    /// @note カメラ距離フェード。負値だけ潰す。near >= far (既定の 0 / 0 を含む) は
    /// @note シェーダー側が «無効» として扱うので、ここで組み替えたり警告したりはしない。
    cb.cameraFadeNear = (std::max)(emitter.runtime.material.cameraFadeNear, 0.0f);
    cb.cameraFadeFar = (std::max)(emitter.runtime.material.cameraFadeFar, 0.0f);
    return cb;
}
/// @brief scopeRoot 以下を名前で深さ優先探索する。VFX Graph の生成物は同名が複数存在しうるため、
/// @brief 「自分と同じエフェクトに属する Emitter」だけを候補にするための限定探索。
void PrepareGpuEmitter(ParticleEmitter&                     emitter,
                    const Transform&                     tf,
                    const AnimatorComponent*             animator,
                    float                                dt,
                    float                                time,
                    bool                                 canEmit,
                    const std::vector<ActiveFlowField>& flowFields,
                    renderer::ResourceHandle<renderer::TextureTag> sceneColor,
                    const MeshRenderer*                  meshParticleRenderer,
                    RenderPassContext&                   ctx, renderer::RenderParticleInput& input)
{
    auto& resources = ctx.resources;
    auto& renderer  = ctx.renderer;
    auto& h         = ctx.handles;

    if (!h.particleGpuSimCS.IsValid() || !h.particleGpuShader.IsValid()) return;

    const int maxP = (std::max)(emitter.settings.maxParticles, 1);

    const bool invalidResources = emitter.runtime.gpuInitialized
        && (emitter.runtime.gpuResetVersion != resources.GetResetVersion()
            || !resources.Get(emitter.runtime.gpuParticleBuffer) || !resources.Get(emitter.runtime.gpuEmitterCB));
    if (emitter.runtime.gpuClearPending || invalidResources
        || (emitter.runtime.gpuInitialized && emitter.runtime.gpuCapacity != static_cast<uint32_t>(maxP))) {
        ReleaseParticleSimulationGpuResources(emitter);
        emitter.runtime.gpuClearPending = false;
    }

    /// @note バッファ未作成なら初期化 (要素ゼロで確保し CS が age>=lifetime で無視する)
    if (!emitter.runtime.gpuInitialized)
    {
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = 0.0f;
        emitter.runtime.flowBoundsRadius = 0.0f;
        emitter.runtime.flowSpeedBound = 0.0f;
        std::vector<GpuParticle> init(static_cast<size_t>(maxP));
        /// @note 全粒子を「死亡済み」で初期化
        for (auto& p : init) p.age = p.lifetime = 1.0f;
        emitter.runtime.gpuParticleBuffer = resources.CreateRWStructuredBuffer(
            init.data(), static_cast<uint32_t>(maxP), sizeof(GpuParticle));

        emitter.runtime.gpuEmitterCB = resources.CreateConstantBuffer(sizeof(GpuParticleEmitterCB));
        emitter.runtime.gpuWriteHead     = 0;
        emitter.runtime.gpuSpawnCount    = 0;
        emitter.runtime.gpuResetVersion  = resources.GetResetVersion();
        emitter.runtime.gpuCapacity      = static_cast<uint32_t>(maxP);
        emitter.runtime.gpuInitialized   = true;
    }

    const bool simulateThisFrame = emitter.runtime.lastGpuSimulationFrame != Time::frameCount;
    if (simulateThisFrame) {
        emitter.runtime.lastGpuSimulationFrame = Time::frameCount;
    /// @note SubEmitter からの «発火元つき» スポーンは GPU 経路では出せない (InitGpuSpawnEntry は
    /// @note 位置と速度の上書きを持たない)。捨てないと、GPU のエミッターを SubEmitter に指した
    /// @note 構成で古い発火位置が溜まり続ける。
    emitter.runtime.injectedSpawns.clear();
    /// @note 今フレームのスポーンエントリを構築
    std::vector<GpuSpawnEntry> spawns;
    if (canEmit || emitter.runtime.burstPending > 0)
    {
        const int burstCount = (std::max)(emitter.runtime.burstPending, 0);
        emitter.runtime.burstPending = 0;
        for (int i = 0; i < burstCount && static_cast<int>(spawns.size()) < maxP; ++i)
        {
            GpuSpawnEntry s;
            InitGpuSpawnEntry(s, emitter, tf, animator);
            spawns.push_back(s);
        }
        if (canEmit)
        {
            /// @note lodRateScale は CPU 経路と同じく発生レートへ掛ける。掛け忘れると
            /// @note 遠距離のエミッターが GPU のときだけ間引かれない。
            emitter.runtime.emitAccum += emitter.settings.emitRate * emitter.runtime.lodRateScale
                * EmitRateCurveScale(emitter) * dt;
            while (emitter.runtime.emitAccum >= 1.0f && static_cast<int>(spawns.size()) < maxP)
            {
                emitter.runtime.emitAccum -= 1.0f;
                GpuSpawnEntry s;
                InitGpuSpawnEntry(s, emitter, tf, animator);
                spawns.push_back(s);
            }
        }
    }
    else
    {
        emitter.runtime.emitAccum = 0.0f;
    }

    /// @note 前回の移動上限で膨らませた球を引き継ぎ、今回の実スポーンを追加する。
    if (emitter.runtime.flowMinimumAge >= emitter.runtime.flowMaxLifetime) {
        emitter.runtime.flowBoundsCenter = TransformEmitterPoint(tf, emitter.settings.emitPosition);
        emitter.runtime.flowBoundsRadius = 0.0f;
        emitter.runtime.flowSpeedBound = 0.0f;
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = 0.0f;
    }
    for (const auto& spawn : spawns) {
        emitter.runtime.flowBoundsRadius = (std::max)(emitter.runtime.flowBoundsRadius,
            (spawn.position - emitter.runtime.flowBoundsCenter).Length());
        emitter.runtime.flowSpeedBound = (std::max)(emitter.runtime.flowSpeedBound, spawn.velocity.Length());
        emitter.runtime.flowMinimumAge = 0.0f;
        emitter.runtime.flowMaxLifetime = (std::max)(emitter.runtime.flowMaxLifetime, spawn.lifetime);
    }
    emitter.runtime.gpuSpawnCount = static_cast<uint32_t>(spawns.size());

    /// @note スポーンバッファを借りて CPU → GPU 転送する。0 件でも 1 要素は借りる (t15 を空にしない)。
    emitter.runtime.gpuSpawnBuffer = g_gpuSpawnPool.Acquire(
        resources, (std::max)(spawns.size(), size_t{ 1 }), static_cast<uint32_t>(sizeof(GpuSpawnEntry)));
    if (!emitter.runtime.gpuSpawnBuffer.IsValid())
        emitter.runtime.gpuSpawnCount = 0;
    if (emitter.runtime.gpuSpawnCount > 0)
        resources.Update(emitter.runtime.gpuSpawnBuffer, spawns.data(),
                         emitter.runtime.gpuSpawnCount * sizeof(GpuSpawnEntry));

    /// @note CS 用定数バッファ更新
    GpuParticleEmitterCB cb{};
    cb.emitterPos      = TransformEmitterPoint(tf, emitter.settings.emitPosition);
    cb.deltaTime       = dt;
    /// @note 乱流・周回・放射は専用スロットではなく gFlowFields へ入る。
    /// @note 重力と結合係数は «媒質の運動ではない» のでエミッターの値をそのまま渡す。
    cb.gravity         = emitter.settings.gravity;
    cb.maxParticles    = static_cast<uint32_t>(maxP);
    cb.colorStart      = emitter.settings.colorStart;
    cb.colorEnd        = emitter.settings.colorEnd;
    cb.spawnCount      = emitter.runtime.gpuSpawnCount;
    cb.spawnOffset     = emitter.runtime.gpuWriteHead;
    cb.colorCurvePower = emitter.settings.colorCurvePower;
    cb.flowCoupling    = emitter.settings.flowCoupling;
    cb.sizeStart       = emitter.settings.sizeStart;
    cb.sizeEnd         = emitter.settings.sizeEnd;
    cb.sizeCurvePower  = emitter.settings.sizeCurvePower;
    {
        /// @note 丸めは CPU 経路・Inspector プレビューと同じ関数で行う (end = 0 を «最後まで» と読む等)。
        const asset::FlipbookFrameRange range =
            asset::ResolveFlipbookRange(emitter.runtime.material.flipbook);
        cb.spriteColumns    = static_cast<uint32_t>(range.columns);
        cb.spriteRows       = static_cast<uint32_t>(range.rows);
        cb.spriteStartFrame = static_cast<uint32_t>(range.first);
        cb.spriteEndFrame   = static_cast<uint32_t>(range.last);
    }
    cb.time           = time;
    /// @note 乱流は gParticleForces 側へ
    cb.noiseStrength  = 0.0f;
    cb.noiseFrequency = 0.5f;
    cb.noiseSpeed     = 1.0f;
    cb.flipbookMode = static_cast<uint32_t>(emitter.runtime.material.flipbook.flipbookMode);
    cb.flipbookFramesPerSecond = (std::max)(emitter.runtime.material.flipbook.flipbookFramesPerSecond, 0.0f);

    /// @note 内蔵の流れもシーンの場も同じ StructuredBuffer へ詰める。CS 側は届いた分を全部
    /// @note 合流させればよい。本数の上限は無いので «場が捨てられて粒子が流れない» は起きない。
    std::vector<ActiveFlowField> emitterForces;
    ResolveEmitterForces(emitter, tf, flowFields, emitterForces, true);
    float flowSpeedBound = 0.0f;
    for (const auto& field : emitterForces) flowSpeedBound += FlowSpeedBound(field);
    /// @note 緩和は凸結合なので、重力後の速さと合成流速の大きい方が次の速さの上限になる。
    emitter.runtime.flowSpeedBound = (std::max)(
        emitter.runtime.flowSpeedBound + emitter.settings.gravity.Length() * std::abs(dt), flowSpeedBound) * 1.00001f;
    /// @note 任意の速度カーブの上限が未確定ならカリングを無効化して粒子を取りこぼさない。
    if (emitter.settings.useVelocityCurve)
        emitter.runtime.flowBoundsRadius = std::numeric_limits<float>::infinity();
    else
        emitter.runtime.flowBoundsRadius += emitter.runtime.flowSpeedBound * std::abs(dt) + 1.0e-4f;
    /// @note GPU と同じ加算で最小年齢を進める。残り寿命の減算では丸めが違い、早く球を捨て得る。
    emitter.runtime.flowMinimumAge += dt;

    std::vector<GpuFlowField> gpuForces;
    gpuForces.reserve(emitterForces.size());
    for (const ActiveFlowField& f : emitterForces) gpuForces.push_back(PackGpuFlowField(f, ctx.resources));
    cb.flowFieldCount = static_cast<uint32_t>(gpuForces.size());
    AcquireGpuForceBuffer(emitter, gpuForces, resources);
    cb.curveFlags = {
        emitter.settings.useSizeCurve ? 1.0f : 0.0f,
        emitter.settings.useVelocityCurve ? 1.0f : 0.0f,
        emitter.settings.useColorGradient ? 1.0f : 0.0f,
        emitter.runtime.material.flipbook.flipbookFrameBlending ? 1.0f : 0.0f
    };
    /// @note カーブは float4 1 本へ 2 キー (time,value) ずつ詰める。有効キー数を超えた分は
    /// @note 最終キーで埋め、GPU 側が余分な区間を踏んでも CPU の Evaluate と同じ値になるようにする。
    const auto packCurve = [](const ParticleCurve& curve, size_t first) {
        const size_t last = static_cast<size_t>(
            (std::max)(1u, (std::min)(curve.keyCount, kMaxParticleCurveKeys)) - 1u);
        const auto& firstKey = curve.keys[(std::min)(first, last)];
        const auto& secondKey = curve.keys[(std::min)(first + 1u, last)];
        return math::Vector4{
            firstKey.time, firstKey.value, secondKey.time, secondKey.value
        };
    };
    const auto curveKeyCount = [](const ParticleCurve& curve) {
        return static_cast<float>(
            (std::max)(1u, (std::min)(curve.keyCount, kMaxParticleCurveKeys)));
    };
    cb.sizeCurveKeys01 = packCurve(emitter.settings.sizeCurve, 0);
    cb.sizeCurveKeys23 = packCurve(emitter.settings.sizeCurve, 2);
    cb.sizeCurveKeys45 = packCurve(emitter.settings.sizeCurve, 4);
    cb.sizeCurveKeys67 = packCurve(emitter.settings.sizeCurve, 6);
    cb.velocityCurveKeys01 = packCurve(emitter.settings.velocityCurve, 0);
    cb.velocityCurveKeys23 = packCurve(emitter.settings.velocityCurve, 2);
    cb.velocityCurveKeys45 = packCurve(emitter.settings.velocityCurve, 4);
    cb.velocityCurveKeys67 = packCurve(emitter.settings.velocityCurve, 6);
    /// @note 黒体モードを焼き込んだ実効グラデーションを渡す。CPU 経路も同じ runtimeGradient を見る。
    const auto& gradient = emitter.runtime.runtimeGradient;
    const size_t lastGradientKey = static_cast<size_t>(
        (std::max)(1u, (std::min)(gradient.keyCount, kMaxParticleCurveKeys)) - 1u);
    const auto gradientTime = [&](size_t index) {
        return gradient.keys[(std::min)(index, lastGradientKey)].time;
    };
    cb.gradientTimes  = { gradientTime(0), gradientTime(1), gradientTime(2), gradientTime(3) };
    cb.gradientTimes47 = { gradientTime(4), gradientTime(5), gradientTime(6), gradientTime(7) };
    for (size_t index = 0; index < 4; ++index) {
        cb.gradientColors[index] = gradient.keys[(std::min)(index, lastGradientKey)].color;
        cb.gradientColors47[index] =
            gradient.keys[(std::min)(index + 4, lastGradientKey)].color;
    }
    /// @note z = 補間色空間 (ParticleColorSpace)。EvaluateGradient8 がこれを見て CPU と同じ空間で混ぜる。
    cb.gradientMeta = { static_cast<float>(lastGradientKey + 1),
                        static_cast<float>(gradient.interpolation),
                        static_cast<float>(gradient.colorSpace), 0.0f };
    /// @note 深度コリジョンは深度バッファを screen space で引くので、それを焼いたのと同じ
    /// @note (TAA ジッター込みの) 行列で射影しないと当たり位置が半ピクセルずれる。
    cb.viewProjection = MakeJitteredViewProjection(ctx.camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    cb.screenWidth = static_cast<float>(ctx.width);
    cb.screenHeight = static_cast<float>(ctx.height);
    cb.depthThickness = (std::max)(0.001f, emitter.settings.collisionRadius * 0.002f);
    cb.depthBounciness = std::clamp(emitter.settings.collisionBounciness, 0.0f, 1.0f);
    cb.depthCollision = emitter.settings.collisionMode == ParticleCollisionMode::Depth ? 1u : 0u;
    cb.depthResponse = static_cast<std::uint32_t>(emitter.settings.collisionResponse);
    cb.depthDamping = std::clamp(emitter.settings.collisionDamping, 0.0f, 1.0f);
    /// @note over-lifetime モジュール (回転カーブ / drag カーブ / 周回・放射)
    cb.curveFlags2 = {
        emitter.settings.useRotationCurve ? 1.0f : 0.0f,
        emitter.settings.useDragCurve ? 1.0f : 0.0f,
        0.0f, 0.0f
    };
    cb.rotationCurveKeys01 = packCurve(emitter.settings.rotationCurve, 0);
    cb.rotationCurveKeys23 = packCurve(emitter.settings.rotationCurve, 2);
    cb.rotationCurveKeys45 = packCurve(emitter.settings.rotationCurve, 4);
    cb.rotationCurveKeys67 = packCurve(emitter.settings.rotationCurve, 6);
    cb.dragCurveKeys01 = packCurve(emitter.settings.dragCurve, 0);
    cb.dragCurveKeys23 = packCurve(emitter.settings.dragCurve, 2);
    cb.dragCurveKeys45 = packCurve(emitter.settings.dragCurve, 4);
    cb.dragCurveKeys67 = packCurve(emitter.settings.dragCurve, 6);
    /// @note 4 本のカーブの有効キー数と補間モード。GPU 側はこれを見て CPU と同じ区間を選ぶ。
    cb.curveKeyCounts = {
        curveKeyCount(emitter.settings.sizeCurve), curveKeyCount(emitter.settings.velocityCurve),
        curveKeyCount(emitter.settings.rotationCurve), curveKeyCount(emitter.settings.dragCurve)
    };
    cb.curveModes = {
        static_cast<float>(emitter.settings.sizeCurve.interpolation),
        static_cast<float>(emitter.settings.velocityCurve.interpolation),
        static_cast<float>(emitter.settings.rotationCurve.interpolation),
        static_cast<float>(emitter.settings.dragCurve.interpolation)
    };
    /// @note 周回と放射は gParticleForces (Vortex / Source) 側へ移った。CB の orbitalAxis /
    /// @note orbitalVelocity / radialVelocity は誰も書かない死んだ枠で、cb{} のゼロ初期化のまま
    /// @note GPU へ渡る (書いていた 0 と同じ値)。フィールド自体は残す ─ この CB は
    /// @note ParticleGpuSim.cs.hlsl と 1 バイト単位で対で、途中を削ると以降の全オフセットがずれる。
    cb.spriteRandomFlags = (emitter.runtime.material.flipbook.spriteRandomStartFrame ? 1u : 0u)
        | (emitter.runtime.material.flipbook.spriteRandomRow ? 2u : 0u);
    input.simulation = cb;
    input.simulate = true;

    /// @note リングバッファヘッドを進める
    emitter.runtime.gpuWriteHead = (emitter.runtime.gpuWriteHead + emitter.runtime.gpuSpawnCount)
                           % static_cast<uint32_t>(maxP);
    }
    if (ShouldSortGpuParticles(emitter, h)) {
        uint32_t padded = kParticleSortBlock;
        while (padded < static_cast<uint32_t>(maxP)) padded <<= 1;
        if (!emitter.runtime.gpuSortBuffer.IsValid() || emitter.runtime.gpuSortCapacity != padded) {
            if (emitter.runtime.gpuSortBuffer.IsValid()) resources.Release(emitter.runtime.gpuSortBuffer);
            /// @note 1 要素 = uint2 (key, particleIndex)。
            emitter.runtime.gpuSortBuffer = resources.CreateRWStructuredBuffer(
                nullptr, padded, static_cast<std::uint32_t>(sizeof(std::uint32_t) * 2u));
            emitter.runtime.gpuSortCapacity = padded;
        }
        if (!emitter.runtime.gpuSortCB.IsValid())
            emitter.runtime.gpuSortCB = resources.CreateConstantBuffer(sizeof(GpuParticleSortCB));
    } else if (emitter.runtime.gpuSortBuffer.IsValid()) {
        resources.Release(emitter.runtime.gpuSortBuffer);
        emitter.runtime.gpuSortBuffer = {}; emitter.runtime.gpuSortCapacity = 0;
    }
    input.velocityField = asset::VelocityFieldAtlas::Texture(resources);
    if (meshParticleRenderer && meshParticleRenderer->enabled && meshParticleRenderer->mesh && !meshParticleRenderer->mesh->isSkinned) {
        const auto& mesh = *meshParticleRenderer->mesh;
        input.meshVertices = mesh.vertexBuffer; input.meshIndices = mesh.indexBuffer;
        input.meshIndexCount = mesh.indexCount; input.meshVertexCount = mesh.vertexCount;
    }
}

} ///< @note namespace

void ExtractRayParticleSources(const Scene& scene, renderer::RenderScene& output,
    bool experimentalRayTracingEnabled)
{
    if (!experimentalRayTracingEnabled) return;
    for (EntityID id : scene.GetEntities<ParticleEmitter>()) {
        auto* go = scene.GetGameObject(id);
        const auto* emitter = go ? go->GetComponent<ParticleEmitter>() : nullptr;
        if (!go || !go->activeInHierarchy() || !emitter || !emitter->settings.enabled) continue;
        const bool gpu = CanUseGpuSimulation(emitter->settings, &emitter->runtime.material);
        /// @note Match Raster's CPU mesh-particle exclusion and zero-count rule, but never use the camera, LOD, budget, or visibleParticleCount as proof of no source.
        if (gpu ? emitter->settings.maxParticles <= 0
                : !emitter->settings.meshParticlePath.empty() || emitter->runtime.particles.empty()) continue;
        const uint32_t layerMask = go->layer >= 0 && go->layer < 32 ? uint32_t{1} << go->layer : 0;
        output.rayUnsupportedEffects.push_back({id.index, id.generation, layerMask,
            renderer::RenderRayUnsupportedEffect::PARTICLE});
    }
}

void ReleaseParticleRenderResources(Scene& scene)
{
    g_particleCaches.erase(scene.GetRenderSceneGeneration());
}

void ExtractRenderParticles(RenderPassContext& ctx, renderer::RenderScene& output) {
    auto& resources = ctx.resources;
    auto& h = ctx.handles;
    auto& cache = AcquireParticleCache(ctx.scene, resources);
    ++cache.m_pass;
    if (!h.particleShader.IsValid() || !h.particleIB.IsValid()) return;
    const float dt = Time::deltaTime, time = Time::time;
    const auto flowSnapshot = ctx.scene.FlowFrame().fields;
    const std::vector<ActiveFlowField>& flowFields = *flowSnapshot;
    int particleBudget = ctx.settings.particleBudgetEnabled
        ? (std::max)(ctx.settings.particleBudget, 0) : 0;

    bool collectParticlePhases = false;
    if (profiler::Profiler::IsEnabled()) {
        const auto requestedFilter = profiler::Profiler::GetRequestedCaptureFilter();
        collectParticlePhases = requestedFilter.scope == "RenderSceneExtractor::Particles"
            && (requestedFilter.category.empty() || requestedFilter.category == "General");
    }
    std::optional<profiler::ProfileScope> phaseScope;
    /// @note Only the explicit parent filter opens bounded phase scopes; the applied frame filter still controls recording.
    const auto beginPhase = [&](const char* name) {
        if (!collectParticlePhases) return;
        phaseScope.reset();
        phaseScope.emplace(profiler::ProfilerMarker(name, "General"));
    };
    beginPhase("RenderSceneExtractor::Particles::CollectAndSort");
    /// @note 描画順を renderPriority → カメラ距離 (遠い順) で確定させる。GameObject の並び順の
    /// @note ままだと「炎の手前に煙」が保証されず、シーンを編集しただけで前後が入れ替わる。
    /// @note 距離はバウンズ更新前なので Transform 位置で近似する (順序決定には十分)。
    std::vector<GameObject*> sortedEmitters;
    {
        /// @note Collection calls no resource or gameplay callback; Scene order remains the stable-sort tie breaker.
        const auto emitters = ctx.scene.GetEntities<ParticleEmitter>();
        std::vector<EntityID> candidates(emitters.begin(), emitters.end());
        ctx.scene.SortEntitiesBySceneOrder(candidates);
        for (EntityID entity : candidates) {
            auto* owner = ctx.scene.GetGameObject(entity);
            if (!owner) continue;
            auto& candidate = *owner;
            if (!ShouldRenderGameObject(candidate, ctx.cullingMask)) continue;
            auto* candidateEmitter = candidate.GetComponent<ParticleEmitter>();
            if (candidateEmitter == nullptr || !candidateEmitter->settings.enabled) continue;
            sortedEmitters.push_back(&candidate);
        }
    }
    const math::Vector3 cameraPosition = ctx.camera.m_position;
    /// @note GetComponent は非 const 版しかないため、比較関数も非 const ポインタで受ける。
    std::stable_sort(sortedEmitters.begin(), sortedEmitters.end(),
        [cameraPosition](GameObject* a, GameObject* b) {
            const int priorityA = a->GetComponent<ParticleEmitter>()->settings.renderPriority;
            const int priorityB = b->GetComponent<ParticleEmitter>()->settings.renderPriority;
            if (priorityA != priorityB) return priorityA < priorityB;
            const math::Vector3 deltaA = a->transform.worldPosition - cameraPosition;
            const math::Vector3 deltaB = b->transform.worldPosition - cameraPosition;
            return math::Vector3::Dot(deltaA, deltaA) > math::Vector3::Dot(deltaB, deltaB);
        });

    beginPhase("RenderSceneExtractor::Particles::EmitterInputsAndResources");
    for (GameObject* emitterObject : sortedEmitters) {
        GameObject& go = *emitterObject;
        auto* emitter = go.GetComponent<ParticleEmitter>();
        const auto* animator = FindParticleAnimator(go);
        const Transform& tf = go.transform;
        ++ctx.statsParticleEmitters;

        /// @note VFX Editor のプレビュー速度 (一時停止 / スロー / 倍速)。ゲーム実行時は常に 1.0。
        const float dtEmitter = dt * emitter->GetEditorTimeScale(Time::frameCount);

        UpdateParticleBounds(*emitter, tf);
        const float cameraDistance = (emitter->runtime.boundsCenter - ctx.camera.m_position).Length();
        const float coverage = emitter->runtime.boundsRadius / (std::max)(cameraDistance, 0.001f);
        const bool frustumVisible = !ctx.frustumCullingEnabled || !ctx.cameraFrustum
            || ctx.cameraFrustum->IntersectsSphere(emitter->runtime.boundsCenter, emitter->runtime.boundsRadius);
        const bool coverageVisible = emitter->settings.culling.screenCoverageThreshold <= 0.0f
            || coverage >= emitter->settings.culling.screenCoverageThreshold;
        emitter->runtime.isCulledThisFrame = emitter->settings.culling.cullingEnabled && (!frustumVisible || !coverageVisible);
        if (emitter->runtime.isCulledThisFrame) {
            ++ctx.statsParticleCulled;
            emitter->runtime.visibleParticleCount = 0;
            continue;
        }
        if (emitter->settings.culling.lodEnabled && emitter->settings.culling.lodFarDistance > emitter->settings.culling.lodNearDistance) {
            const float alpha = Clamp01((cameraDistance - emitter->settings.culling.lodNearDistance)
                / (emitter->settings.culling.lodFarDistance - emitter->settings.culling.lodNearDistance));
            emitter->runtime.lodRateScale = emitter->settings.culling.lodNearRateScale
                + (emitter->settings.culling.lodFarRateScale - emitter->settings.culling.lodNearRateScale) * alpha;
        } else {
            emitter->runtime.lodRateScale = 1.0f;
        }
        EnsureParticleTexture(*emitter, resources, cache);
        /// @note Physics Query、Local Space、厳密な透過ソートはCPU側で解決し、見た目の正しさを優先する。
        const bool isGpuMode = CanUseGpuSimulation(emitter->settings, &emitter->runtime.material);

        const bool canEmit = emitter->runtime.emitThisFrame;

        renderer::RenderParticleInput input;
        input.layer = static_cast<uint32_t>(go.layer); input.position = tf.worldPosition;
        input.boundsCenter = emitter->runtime.boundsCenter;
        input.boundsRadius = emitter->runtime.boundsRadius;
        input.priority = emitter->settings.renderPriority; input.gpu = isGpuMode;
        input.settings.sortMode = emitter->settings.sortMode;
        input.settings.trail = emitter->settings.trail;
        input.settings.colorStart = emitter->settings.colorStart; input.settings.colorEnd = emitter->settings.colorEnd;
        input.settings.meshParticle = !emitter->settings.meshParticlePath.empty();
        input.settings.maxParticles = emitter->settings.maxParticles;
        int count = 0;
        if (isGpuMode) {
            const int requested = (std::max)(emitter->settings.maxParticles, 0);
            count = particleBudget > 0 ? (std::min)(requested, particleBudget) : requested;
            emitter->runtime.visibleParticleCount = count;
            if (particleBudget > 0) particleBudget -= count;
            if (requested > count) ++ctx.statsParticleBudgetDropped;
            if (count <= 0) continue;
            ctx.statsParticleVisible += count;
            input.constants = BuildParticleRenderConstants(*emitter, resources, count, ctx.width, ctx.height, ShouldSortGpuParticles(*emitter, h));
            const auto* meshRenderer = input.settings.meshParticle ? go.GetComponent<MeshRenderer>() : nullptr;
            PrepareGpuEmitter(*emitter, tf, animator, dtEmitter, time, canEmit, flowFields, {}, meshRenderer, ctx, input);
        } else {
            const int available = static_cast<int>(emitter->runtime.particles.size());
            const int budget = particleBudget > 0 ? (std::min)(available, particleBudget) : available;
            count = (std::min)(budget, kMaxParticleDraw);
            emitter->runtime.visibleParticleCount = count;
            if (particleBudget > 0) particleBudget -= count;
            if (available > count) ++ctx.statsParticleBudgetDropped;
            ctx.statsParticleVisible += count;
            if (count == 0 || input.settings.meshParticle) continue;
            input.constants = BuildParticleRenderConstants(*emitter, resources, count, ctx.width, ctx.height);
            input.runtime.particles = emitter->runtime.particles;
            if (emitter->settings.simulationSpace == ParticleSimulationSpace::Local) {
                for (auto& particle : input.runtime.particles) {
                    particle.position = TransformEmitterPoint(tf, particle.position);
                    particle.velocity = TransformEmitterVector(tf, particle.velocity);
                    for (auto& point : particle.trailPoints) point = TransformEmitterPoint(tf, point);
                }
            }
        }
        input.drawCount = count;
        if (emitter->settings.trail.trailRibbon && !resources.Get(emitter->runtime.trailRibbonCB)) {
            renderer::ReleaseOwnedResource(emitter->runtime.trailRibbonCB);
            emitter->runtime.trailRibbonCB = resources.CreateConstantBuffer(sizeof(TrailCB));
        }
        input.runtime.texture = emitter->runtime.texture;
        input.runtime.distortionTexture = emitter->runtime.distortionTexture;
        input.runtime.motionVectorTexture = emitter->runtime.motionVectorTexture;
        input.runtime.sixWayNegativeTexture = emitter->runtime.sixWayNegativeTexture;
        input.runtime.sixWayAlbedoColorTexture = emitter->runtime.sixWayAlbedoColorTexture;
        input.runtime.sixWayEmissionColorTexture = emitter->runtime.sixWayEmissionColorTexture;
        input.runtime.customShader = emitter->runtime.customShader;
        input.runtime.renderCB = emitter->runtime.renderCB;
        input.runtime.materialParamsCB = emitter->runtime.materialParamsCB;
        input.runtime.trailRibbonCB = emitter->runtime.trailRibbonCB;
        input.runtime.gpuEmitterCB = emitter->runtime.gpuEmitterCB;
        input.runtime.gpuSortCB = emitter->runtime.gpuSortCB;
        input.runtime.gpuParticleBuffer = emitter->runtime.gpuParticleBuffer;
        input.runtime.gpuSpawnBuffer = emitter->runtime.gpuSpawnBuffer;
        input.runtime.gpuForceBuffer = emitter->runtime.gpuForceBuffer;
        input.runtime.gpuSortBuffer = emitter->runtime.gpuSortBuffer;
        input.runtime.gpuSortCapacity = emitter->runtime.gpuSortCapacity;
        input.runtime.resolvedBlend = emitter->runtime.resolvedBlend;
        input.runtime.textureIsSrgb = emitter->runtime.textureIsSrgb;
        input.runtime.material.distortion = emitter->runtime.material.distortion;
        input.runtime.material.punctualLighting = emitter->runtime.material.punctualLighting;
        input.runtime.material.selfShadowStrength = emitter->runtime.material.selfShadowStrength;
        output.particles.push_back(std::move(input));
    }
    if (collectParticlePhases) phaseScope.reset();
    ExtractRayParticleSources(ctx.scene, output, ctx.experimentalRayTracingEnabled);
}
}
