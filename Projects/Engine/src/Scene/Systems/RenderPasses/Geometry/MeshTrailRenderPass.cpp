/// @file    RenderPasses/Geometry/MeshTrailRenderPass.cpp
/// @brief   MeshTrailComponent の過去姿勢サンプリング、Skinned bone palette 保存、半透明 DrawCall 発行 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "Engine/Scene/Systems/RenderPasses/Geometry/MeshTrailRenderPass.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/MathUtils.hpp>
#include "GeometryPasses.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {

namespace {

// MeshTrailCB — MeshTrailConstants(cbuffer b6) の C++ ミラー。
//
// WHY b2 ではないか: シェーダーリフレクションは cbuffer 名 "MaterialConstants" を
//     b2 に探す。エンジンがそこを占有すると、残像材質だけ .mat の [params] を
//     1 つも持てない例外になる (Decal / Particle と同じ理由。Binding.hlsli 参照)。
struct MeshTrailCB {
    math::Vector4 trailColor;
};

// DrawCall::constantBuffers の添字 = レジスタ番号。b6 は残像パスでは空いている。
constexpr int kMeshTrailCBSlot = 6;

// SkinningCB — Common/Constants.hlsli の SkinningConstants と同じレイアウト。
struct SkinningCB {
    math::Matrix4 boneMatrices[asset::MAX_SKINNING_BONES];
};

static_assert(sizeof(MeshTrailCB) == 16, "MeshTrailCB layout mismatch");

// .mat が宣言したカスタムシェーダーと [params] の解決結果。
//
// WHY .mat 単位でキャッシュするか: 解決にはシェーダーのロードとリフレクションが要る。
//     プレイヤーの残像は部位ごとに 10 個以上の実体へ同じ .mat が張られるので、
//     実体ごとに引くと 1 フレームでその回数ぶん繰り返すことになる。
struct TrailMaterialBinding {
    renderer::ResourceHandle<renderer::ShaderTag>         shader;
    std::string                                           loadedShaderPath;
    renderer::ShaderDescriptor                            descriptor;
    renderer::ResourceHandle<renderer::ConstantBufferTag> paramsCB;
    std::vector<uint8_t>                                  paramData;
    // paramsCB を確保したときのサイズ。ホットリロードで MaterialConstants の
    // 大きさが変わったら作り直す (古い容量のまま書くと末尾が落ちる)。
    uint32_t                                              paramsCBSize = 0;
    asset::MeshType                                       meshType     = asset::MeshType::Any;
    uint64_t                                              resolvedPass = 0;
};

std::unordered_map<std::string, TrailMaterialBinding> g_trailMaterials;
std::unordered_set<std::string>                       g_warnedTrailMaterials;
// Execute の呼び出し通番。0 は「未解決」を表すため 1 から始める。
uint64_t                                              g_trailPassSerial = 0;
// ResourceManager::Reset() の世代。キャッシュはシェーダーと定数バッファのハンドルを
// 握るので、世代が変われば中身は失効している。
// WHY 返さずに捨てるか: Reset() は実体を破棄済みで、残っているのは失効したハンドルだけ。
uint32_t                                              g_resetVersion = 0;

bool WarnTrailMaterialOnce(const std::string& path)
{
    return g_warnedTrailMaterials.insert(path).second;
}

// .mat の shader と [params] を解決する。カスタムシェーダーが無ければ nullptr。
//
// WHY render_path = "trail" を要求するか: 残像の b6 は MeshTrailConstants、頂点は
//     (Skinned なら) ボーン付きで来る。メッシュ用の .mat を割り当てると、落ちずに
//     «静かに壊れた絵» になる。UI / Decal / Particle と同じ判断で宣言を要求する。
const TrailMaterialBinding* ResolveTrailMaterialBinding(
    renderer::ResourceManager& resources, const std::string& materialPath)
{
    if (materialPath.empty()) return nullptr;

    TrailMaterialBinding& binding = g_trailMaterials[materialPath];
    if (binding.resolvedPass == g_trailPassSerial)
        return binding.shader.IsValid() ? &binding : nullptr;
    binding.resolvedPass = g_trailPassSerial;

    const auto matHandle = asset::AssetManager::LoadMaterial(materialPath);
    const auto* mat = asset::AssetManager::GetMaterial(matHandle);
    if (!mat) return nullptr;

    const bool declaredForTrails = (mat->renderPath == asset::RenderPath::Trail);
    if (!declaredForTrails && !mat->shaderPath.empty()
        && WarnTrailMaterialOnce(materialPath)) {
        FBZZ_LOG_WARN("Mesh trail material '%s' declares shader '%s' but is not declared for "
                      "trails (render_path must be \"trail\") -> ignoring the shader and "
                      "drawing with the built-in mesh trail shader.",
                      materialPath.c_str(), mat->shaderPath.c_str());
    }
    const std::string shaderPath = declaredForTrails ? mat->shaderPath : std::string{};
    binding.meshType = mat->meshType;

    if (binding.loadedShaderPath != shaderPath) {
        binding.loadedShaderPath = shaderPath;
        binding.shader = shaderPath.empty()
            ? renderer::ResourceHandle<renderer::ShaderTag>{}
            : resources.LoadShader(shaderPath);
        // 黙って組み込みへ落ちると «前と同じ絵» が出るだけで、材質を書いた側からは
        // 「効いていない」としか見えない。コンパイル漏れが一番起きやすい。
        if (!shaderPath.empty() && !binding.shader.IsValid()
            && WarnTrailMaterialOnce(materialPath + "|load")) {
            FBZZ_LOG_WARN("Mesh trail material '%s' references shader '%s' but it failed to load "
                          "(not compiled?). Falling back to the built-in mesh trail shader.",
                          materialPath.c_str(), shaderPath.c_str());
        }
    }
    if (!binding.shader.IsValid()) return nullptr;

    // MaterialConstants を宣言していないシェーダーは cbufferSize が 0 (= IsValid() が false)。
    // その場合 b2 へは何も束縛しない。束縛規則そのものは asset::MaterialParamBinding が持つ。
    binding.descriptor = {};
    if (auto* compiled = resources.Get(binding.shader))
        binding.descriptor = compiled->GetDescriptor();
    if (!binding.descriptor.IsValid()) {
        binding.paramsCB = {};
        return &binding;
    }

    binding.paramData.assign(binding.descriptor.cbufferSize, uint8_t{ 0 });
    asset::InitDefaultMaterialParams(binding.descriptor, binding.paramData);
    asset::ApplyMaterialAssetParams(*mat, binding.descriptor, binding.paramData);

    // WHY IsValid() で足りないか: このキャッシュはシーンの寿命もデバイスリセットも跨ぐ。
    //     ハンドルの体裁は残るので、実体が居るかどうかで «作り直し» を判断する。
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

// カスタムシェーダーをこの描画へ当ててよいか。VS の頂点入力が食い違うと
// «落ちずに崩れた絵» になるので、.mat の mesh_type 宣言で弾く。
bool TrailShaderMatchesMesh(const TrailMaterialBinding* binding, bool skinned)
{
    if (!binding || !binding->shader.IsValid()) return false;
    return skinned ? (binding->meshType != asset::MeshType::Surface)
                   : (binding->meshType != asset::MeshType::Skinned);
}

float DistanceSq(const math::Vector3& a, const math::Vector3& b)
{
    return (a - b).LengthSq();
}

void ReleaseSampleResources(MeshTrailSample& sample, renderer::ResourceManager& resources)
{
    if (sample.skinningCB.IsValid()) {
        resources.Release(sample.skinningCB);
        sample.skinningCB = {};
    }
}

MeshTrailSample& SampleAt(MeshTrailComponent& trail, int logicalIndex)
{
    const int physicalIndex = (trail.sampleHead + logicalIndex) % trail.allocatedMaxSamples;
    return trail.samples[static_cast<size_t>(physicalIndex)];
}

const MeshTrailSample& SampleAt(const MeshTrailComponent& trail, int logicalIndex)
{
    const int physicalIndex = (trail.sampleHead + logicalIndex) % trail.allocatedMaxSamples;
    return trail.samples[static_cast<size_t>(physicalIndex)];
}

void EnsureSampleStorage(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    const int desiredMaxSamples = (std::max)(trail.maxSamples, 1);
    if (trail.allocatedMaxSamples == desiredMaxSamples &&
        trail.samples.size() == static_cast<size_t>(desiredMaxSamples))
        return;

    std::vector<MeshTrailSample> ordered;
    ordered.reserve(static_cast<size_t>((std::min)(trail.sampleCount, desiredMaxSamples)));
    if (trail.allocatedMaxSamples > 0 && !trail.samples.empty()) {
        const int keepCount = (std::min)(trail.sampleCount, desiredMaxSamples);
        const int discardCount = trail.sampleCount - keepCount;
        for (int i = 0; i < discardCount; ++i)
            ReleaseSampleResources(SampleAt(trail, i), resources);
        for (int i = discardCount; i < trail.sampleCount; ++i)
            ordered.push_back(std::move(SampleAt(trail, i)));
    }

    trail.samples.clear();
    trail.samples.resize(static_cast<size_t>(desiredMaxSamples));
    for (size_t i = 0; i < ordered.size(); ++i)
        trail.samples[i] = std::move(ordered[i]);
    trail.sampleHead = 0;
    trail.sampleCount = static_cast<int>(ordered.size());
    trail.allocatedMaxSamples = desiredMaxSamples;
}

void ClearSamples(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    for (int i = 0; i < trail.sampleCount; ++i) {
        auto& sample = SampleAt(trail, i);
        ReleaseSampleResources(sample, resources);
        sample = {};
    }
    trail.sampleHead = 0;
    trail.sampleCount = 0;
    trail.lastSampleTime = -1.0f;
}

void ExpireSamples(MeshTrailComponent& trail, renderer::ResourceManager& resources, float currentTime)
{
    const float duration = (std::max)(trail.duration, 0.01f);
    const float oldestAllowed = currentTime - duration;

    while (trail.sampleCount > 0 && SampleAt(trail, 0).timestamp < oldestAllowed) {
        auto& oldest = SampleAt(trail, 0);
        ReleaseSampleResources(oldest, resources);
        oldest = {};
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
        --trail.sampleCount;
    }

    trail.maxSamples = (std::max)(trail.maxSamples, 1);
    while (trail.sampleCount > trail.maxSamples) {
        auto& oldest = SampleAt(trail, 0);
        ReleaseSampleResources(oldest, resources);
        oldest = {};
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
        --trail.sampleCount;
    }
}

bool ShouldSample(const MeshTrailComponent& trail, const math::Vector3& position, float currentTime)
{
    const bool firstSample = trail.lastSampleTime < 0.0f;
    const bool timeReady = firstSample || currentTime - trail.lastSampleTime >= trail.sampleInterval;
    if (!timeReady)
        return false;

    if (trail.sampleCount == 0)
        return true;

    const float minDist = (std::max)(trail.minVertexDist, 0.0f);
    return DistanceSq(position, SampleAt(trail, trail.sampleCount - 1).position) >= minDist * minDist;
}

bool IsMeshIndexExcluded(const MeshTrailComponent& trail, int meshIndex)
{
    return std::find(trail.excludedMeshIndices.begin(), trail.excludedMeshIndices.end(), meshIndex)
        != trail.excludedMeshIndices.end();
}

void CaptureSample(GameObject& go, MeshTrailComponent& trail, renderer::ResourceManager& resources, float currentTime)
{
    MeshTrailSample sample{};
    sample.timestamp = currentTime;
            sample.position = go.transform.worldPosition;
            sample.world = go.transform.GetWorldMatrix();

    // 子SkinnedMeshRendererは親GameObjectのAnimatorを共有する。
    // WHY: 自GOだけを見るとbone paletteが空になり、武器残像がbind poseで描画されるため。
    if (auto* animator = FindAnimator(go)) {
        sample.boneMatrices = animator->boneMatrices;
        if (sample.boneMatrices.size() > asset::MAX_SKINNING_BONES)
            sample.boneMatrices.resize(asset::MAX_SKINNING_BONES);
    }

    if (trail.allocatedMaxSamples <= 0)
        return;

    if (trail.sampleCount == trail.allocatedMaxSamples) {
        MeshTrailSample& writeSlot = trail.samples[static_cast<size_t>(trail.sampleHead)];
        ReleaseSampleResources(writeSlot, resources);
        writeSlot = std::move(sample);
        trail.sampleHead = (trail.sampleHead + 1) % trail.allocatedMaxSamples;
    } else {
        const int writeIndex = (trail.sampleHead + trail.sampleCount) % trail.allocatedMaxSamples;
        trail.samples[static_cast<size_t>(writeIndex)] = std::move(sample);
        ++trail.sampleCount;
    }
    trail.lastSampleTime = currentTime;
}

void EnsureComponentResources(MeshTrailComponent& trail, renderer::ResourceManager& resources)
{
    trail.duration = (std::max)(trail.duration, 0.01f);
    trail.sampleInterval = (std::max)(trail.sampleInterval, 0.0f);
    trail.minVertexDist = (std::max)(trail.minVertexDist, 0.0f);
    trail.maxSamples = (std::max)(trail.maxSamples, 1);
    EnsureSampleStorage(trail, resources);

    if (!trail.meshTrailCB.IsValid())
        trail.meshTrailCB = resources.CreateConstantBuffer(sizeof(MeshTrailCB));

    // materialPath が設定されている場合: .mat の albedo テクスチャと doubleSided を優先する。
    if (!trail.materialPath.empty()) {
        const bool matChanged = (trail.loadedMaterialPath != trail.materialPath);
        if (matChanged) {
            trail.loadedMaterialPath = trail.materialPath;
            trail.loadedTexturePath.clear();
        }
        const auto matHandle = asset::AssetManager::LoadMaterial(trail.materialPath);
        if (const auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
            const auto it = mat->textures.find("albedo");
            const std::string& resolvedTex = (it != mat->textures.end()) ? it->second : std::string{};
            if (!trail.texture.IsValid() || trail.loadedTexturePath != resolvedTex) {
                if (resolvedTex.empty()) {
                    trail.texture = resources.GetWhiteTexture();
                } else {
                    trail.texture = resources.LoadTexture(resolvedTex);
                }
                trail.loadedTexturePath = resolvedTex;
            }
            trail.doubleSided = mat->doubleSided;
        }
    } else if (!trail.texture.IsValid()) {
        // 共有の 1 枚を借りる。実体ごとに作ると、その実体が畳まれたぶんだけ GPU に残る。
        trail.texture = resources.GetWhiteTexture();
        trail.loadedTexturePath.clear();
    }
}

renderer::ResourceHandle<renderer::ConstantBufferTag> EnsureSampleSkinningCB(
    MeshTrailSample& sample,
    renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::ConstantBufferTag> bindPoseCB)
{
    if (sample.boneMatrices.empty())
        return bindPoseCB;

    if (!sample.skinningCB.IsValid())
        sample.skinningCB = resources.CreateConstantBuffer(sizeof(SkinningCB));

    if (!sample.skinningCBDirty)
        return sample.skinningCB;

    SkinningCB cb{};
    for (int i = 0; i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = math::Matrix4::Identity();
    for (size_t i = 0; i < sample.boneMatrices.size() && i < asset::MAX_SKINNING_BONES; ++i)
        cb.boneMatrices[i] = sample.boneMatrices[i];
    resources.Update(sample.skinningCB, &cb, sizeof(cb));
    sample.skinningCBDirty = false;
    return sample.skinningCB;
}

math::Vector4 SampleColor(const MeshTrailComponent& trail, const MeshTrailSample& sample, float currentTime)
{
    const float duration = (std::max)(trail.duration, 0.01f);
    const float newestWeight = math::Clamp01(1.0f - (currentTime - sample.timestamp) / duration);
    return trail.colorEnd + (trail.colorStart - trail.colorEnd) * newestWeight;
}

void DrawStaticMeshSample(
    MeshTrailComponent& trail,
    MeshTrailSample& sample,
    MeshRenderer& mr,
    RenderPassContext& ctx,
    float currentTime,
    const TrailMaterialBinding* material)
{
    if (!mr.enabled || !mr.lodVisible || !mr.mesh || mr.mesh->isSkinned)
        return;
    if (!mr.mesh->vertexBuffer.IsValid() || !mr.mesh->indexBuffer.IsValid())
        return;

    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    PerObjectCB objData{};
    objData.world = sample.world;
    objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
    resources.Update(h.objectCB, &objData, sizeof(objData));

    MeshTrailCB cb{};
    cb.trailColor = SampleColor(trail, sample, currentTime);
    resources.Update(trail.meshTrailCB, &cb, sizeof(cb));

    renderer::DrawCall dc;
    dc.vertexBuffer = mr.mesh->vertexBuffer;
    dc.indexBuffer = mr.mesh->indexBuffer;
    dc.indexCount = mr.mesh->indexCount;
    dc.vertexCount = mr.mesh->vertexCount;
    const bool custom = TrailShaderMatchesMesh(material, false);
    dc.shader = custom ? material->shader : h.meshTrailShader;
    dc.pipelineState = trail.doubleSided ? h.meshTrailDoubleSidedPSO : h.meshTrailPSO;
    dc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
    dc.constantBuffers[0] = h.frameCB;
    dc.constantBuffers[1] = h.objectCB;
    // b2 は材質のもの。組み込みシェーダーは何も読まないので束縛しない。
    if (custom) dc.constantBuffers[2] = material->paramsCB;
    dc.constantBuffers[kMeshTrailCBSlot] = trail.meshTrailCB;
    dc.textures[0] = trail.texture;
    SubmitCounted(ctx, dc);
}

void DrawSkinnedMeshSample(
    MeshTrailComponent& trail,
    MeshTrailSample& sample,
    SkinnedMeshRenderer& smr,
    RenderPassContext& ctx,
    float currentTime,
    const TrailMaterialBinding* material)
{
    if (!smr.enabled || !smr.lodVisible || !smr.model)
        return;

    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    PerObjectCB objData{};
    objData.world = sample.world;
    objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(objData.world);
    resources.Update(h.objectCB, &objData, sizeof(objData));

    MeshTrailCB cb{};
    cb.trailColor = SampleColor(trail, sample, currentTime);
    resources.Update(trail.meshTrailCB, &cb, sizeof(cb));

    const auto skinCB = EnsureSampleSkinningCB(sample, resources, h.bindPoseSkinningCB);
    const bool custom = TrailShaderMatchesMesh(material, true);

    for (size_t meshIndex = 0; meshIndex < smr.SubmeshCount(); ++meshIndex) {
        // 対象の絞り込みは MeshTrailComponent::excludedMeshIndices で行う。
        // WHY 除外番号がローカルスロット番号か: 剣だけを残像化する、といった指定は
        //     「この Renderer の何番目か」で書くのが自然で、モデル全体の submesh 番号を
        //     知る必要がない。ノードごとに子 GO へ分けた構成では、剣の Renderer が
        //     持つ submesh は 1 個だけになり excludedMeshIndices すら不要になる。
        if (IsMeshIndexExcluded(trail, static_cast<int>(meshIndex)))
            continue;
        const renderer::Mesh* meshPtr = smr.SubmeshMesh(meshIndex);
        if (!meshPtr)
            continue;
        if (!meshPtr->vertexBuffer.IsValid() || !meshPtr->indexBuffer.IsValid())
            continue;

        renderer::DrawCall dc;
        dc.vertexBuffer = smr.ResolveSlotVertexBuffer(meshIndex, meshPtr->vertexBuffer);
        dc.indexBuffer = meshPtr->indexBuffer;
        dc.indexCount = meshPtr->indexCount;
        dc.vertexCount = meshPtr->vertexCount;
        dc.shader = custom ? material->shader : h.skinnedMeshTrailShader;
        dc.pipelineState = trail.doubleSided ? h.meshTrailDoubleSidedPSO : h.meshTrailPSO;
        dc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[1] = h.objectCB;
        // b2 は材質のもの。組み込みシェーダーは何も読まないので束縛しない。
        if (custom) dc.constantBuffers[2] = material->paramsCB;
        dc.constantBuffers[kMeshTrailCBSlot] = trail.meshTrailCB;
        dc.constantBuffers[7] = skinCB;
        dc.textures[0] = trail.texture;
        SubmitCounted(ctx, dc);
    }
}

} // namespace

// ─── IRenderPass ──────────────────────────────────────────────────────────────

std::string_view MeshTrailRenderPass::Name() const { return "MeshTrail"; }

void MeshTrailRenderPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

void MeshTrailRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    if (!h.meshTrailShader.IsValid() || !h.skinnedMeshTrailShader.IsValid())
        return;
    if (!h.meshTrailPSO.IsValid() || !h.meshTrailDoubleSidedPSO.IsValid())
        return;

    ctx.renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

    if (g_resetVersion != resources.GetResetVersion()) {
        g_resetVersion = resources.GetResetVersion();
        g_trailMaterials.clear();
        g_warnedTrailMaterials.clear();
    }

    const float currentTime = Time::time;
    // .mat の解決をこのパスで 1 回だけやり直すための通番。編集が次のフレームで
    // 絵へ出つつ、同じ .mat を共有する実体ぶん引き直さない。
    ++g_trailPassSerial;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask))
            continue;

        auto* trail = go.GetComponent<MeshTrailComponent>();
        if (!trail)
            continue;

        if (!trail->enabled && trail->clearOnDisable) {
            ClearSamples(*trail, resources);
            trail->clearRequested = false;
            continue;
        }

        if (trail->clearRequested) {
            ClearSamples(*trail, resources);
            trail->clearRequested = false;
        }

        const bool hasStaticMesh = go.GetComponent<MeshRenderer>() != nullptr;
        const bool hasSkinnedMesh = go.GetComponent<SkinnedMeshRenderer>() != nullptr;
        if (!hasStaticMesh && !hasSkinnedMesh)
            continue;

        EnsureComponentResources(*trail, resources);
        const TrailMaterialBinding* material =
            ResolveTrailMaterialBinding(resources, trail->materialPath);
        if (auto* emitter = go.GetComponent<ParticleEmitter>();
            emitter != nullptr && !emitter->settings.meshParticlePath.empty()) {
            auto* mesh = go.GetComponent<MeshRenderer>();
            if (mesh == nullptr) continue;
            const math::Vector4 savedStart = trail->colorStart;
            const math::Vector4 savedEnd = trail->colorEnd;
            const std::size_t particleCount = (std::min)(emitter->runtime.particles.size(),
                static_cast<std::size_t>((std::max)(emitter->runtime.visibleParticleCount, 0)));
            for (std::size_t particleIndex = 0; particleIndex < particleCount; ++particleIndex) {
                const Particle& particle = emitter->runtime.particles[particleIndex];
                math::Vector3 position = particle.position;
                if (emitter->settings.simulationSpace == ParticleSimulationSpace::Local) {
                    const math::Vector3 scaled{
        position.x * go.transform.worldScale.x,
        position.y * go.transform.worldScale.y,
        position.z * go.transform.worldScale.z };
    position = go.transform.worldPosition +
        go.transform.worldRotation * scaled;
                }
                MeshTrailSample sample;
                sample.timestamp = currentTime;
                sample.position = position;
                // Mesh Particle は billboard と違い 3 軸すべてを使えるため、
                // sizeAxisScale の z も反映する (billboard 経路は xy のみ)。
                sample.world = math::Matrix4::TRS(position,
                    math::Quaternion::FromAxisAngle(math::Vector3::FORWARD, particle.rotation),
                    { particle.size * emitter->settings.sizeAxisScale.x,
                      particle.size * emitter->settings.sizeAxisScale.y,
                      particle.size * emitter->settings.sizeAxisScale.z });
                // Particle::color はリニア、MeshTrail の色はオーサリング空間 (sRGB) と
                // 規約が違う。境界のここで戻して、MeshTrail 側の扱いは一切変えない。
                const math::Vector4 authored = ParticleLinearToSrgb(particle.color);
                trail->colorStart = authored;
                trail->colorEnd = authored;
                DrawStaticMeshSample(*trail, sample, *mesh, ctx, currentTime, material);
            }
            trail->colorStart = savedStart;
            trail->colorEnd = savedEnd;
            continue;
        }
        ExpireSamples(*trail, resources, currentTime);

        if (trail->enabled && ShouldSample(
            *trail, go.transform.worldPosition, currentTime))
            CaptureSample(go, *trail, resources, currentTime);

        ExpireSamples(*trail, resources, currentTime);

        for (int i = 0; i < trail->sampleCount; ++i) {
            auto& sample = SampleAt(*trail, i);
            if (auto* mr = go.GetComponent<MeshRenderer>())
                DrawStaticMeshSample(*trail, sample, *mr, ctx, currentTime, material);
            if (auto* smr = go.GetComponent<SkinnedMeshRenderer>())
                DrawSkinnedMeshSample(*trail, sample, *smr, ctx, currentTime, material);
        }
    }
}

} // namespace fbzz::scene
