/// @file    RenderSceneMaterialResolver.cpp
/// @brief   抽出中の材質能力、初期パラメーターと同期済みバインディングを共有する。
/// @author  Hasegawa Jin
/// @date    2026-11-21
#include "RenderSceneMaterialResolver.hpp"

#include <Core/Profiler/ProfileScope.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <algorithm>
#include <bit>
#include <utility>

namespace fbzz::scene::render_extraction_detail {

RenderMaterialResolver::RenderMaterialResolver(const renderer::ResourceManager* resources)
    : m_resources(resources), m_resourceOwner(resources ? resources->GetInstanceId() : 0),
      m_resetVersion(resources ? resources->GetResetVersion() : 0),
      m_shaderVersion(resources ? resources->GetShaderVersion() : 0),
      m_assetGeneration(asset::AssetManager::GetAssetGeneration()) {}

renderer::GeometryMaterialInput RenderMaterialResolver::ReadSlotCapabilities(const MaterialSlot& slot)
{
    const auto slotSnapshot = slot.ReadAuthoring();
    const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    return ExtractCapabilities(slotSnapshot->Data(), snapshot);
}

void RenderMaterialResolver::ApplySlotState(const MaterialSlot& slot, renderer::RenderMaterial& output)
{
    RefreshCapabilitiesEpoch();
    const auto slotSnapshot = slot.ReadAuthoring();
    const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    const auto& values = slotSnapshot->Data();
    const auto* material = snapshot ? &snapshot->Data() : nullptr;
    output.capabilities = ExtractCapabilities(values, snapshot);
    output.renderQueue = values.hasRenderQueueOverride ? values.renderQueueOverride
        : values.hasPresentationDefaults ? values.presentationRenderQueue
        : material ? material->renderQueue : renderer::RenderQueue::GEOMETRY;
    output.doubleSided = values.hasDoubleSidedOverride ? values.doubleSidedOverride
        : values.hasPresentationDefaults ? values.presentationDoubleSided
        : material && material->doubleSided;
    output.depthBias = material ? material->depthBias : 0;
    output.depthBiasSlope = material ? material->depthBiasSlope : 0.0f;
}

renderer::GeometryMaterialInput RenderMaterialResolver::ExtractCapabilities(const MaterialSlotAuthoring& slot,
    const asset::MaterialAsset::AuthoringSnapshotPtr& snapshot)
{
    const auto* material = snapshot ? &snapshot->Data() : nullptr;
    if (!material) return ExtractGeometryMaterial(slot, nullptr, false);
    const bool sharedRoute = slot.paramOverrides.empty() && !slot.hasBlendModeOverride;
    if (sharedRoute) {
        const auto cached = m_geometryDefaults.find(snapshot);
        if (cached != m_geometryDefaults.end()) return cached->second;
    }
    auto found = m_shaderCapabilities.find(material->shaderPath);
    if (found == m_shaderCapabilities.end()) {
        FBZZ_PROFILE_SCOPE("RenderSceneExtractor::ShaderCapabilities");
        found = m_shaderCapabilities.emplace(material->shaderPath,
            IsGBufferEquivalentShader(material->shaderPath)).first;
    }
    const auto input = ExtractGeometryMaterial(slot, material, found->second);
    if (sharedRoute) m_geometryDefaults.emplace(snapshot, input);
    return input;
}

void RenderMaterialResolver::RefreshCapabilitiesEpoch()
{
    const auto owner = m_resources ? m_resources->GetInstanceId() : 0;
    const auto reset = m_resources ? m_resources->GetResetVersion() : 0;
    const auto shader = m_resources ? m_resources->GetShaderVersion() : 0;
    const auto assets = asset::AssetManager::GetAssetGeneration();
    if (m_resourceOwner == owner && m_resetVersion == reset && m_shaderVersion == shader && m_assetGeneration == assets) return;
    m_resourceOwner = owner; m_resetVersion = reset; m_shaderVersion = shader;
    m_assetGeneration = assets;
    m_shaderCapabilities.clear(); m_geometryDefaults.clear();
}

const PreparedMaterialParams* RenderMaterialResolver::Prepare(MaterialComponent& component, size_t slotIndex,
    renderer::ResourceManager& resources, bool skinned)
{
    const auto& slot = component.SlotAt(slotIndex);
    const auto slotSnapshot = slot.ReadAuthoring();
    const auto handle = slot.GetMaterialAsset();
    const auto snapshot = asset::ReadMaterialAuthoring(handle);
    if (!snapshot) return nullptr;
    const auto& values = snapshot->Data();
    if (values.shaderPath.empty() || values.renderPath != asset::RenderPath::Auto
        || (skinned && values.meshType == asset::MeshType::Surface)
        || (!skinned && values.meshType == asset::MeshType::Skinned)) return nullptr;
    auto [found, inserted] = m_prepared.try_emplace(snapshot);
    auto& cached = found->second;
    if (inserted || cached.prepared.resourceOwner != resources.GetInstanceId()
        || cached.prepared.resetVersion != resources.GetResetVersion()
        || cached.prepared.shaderVersion != resources.GetShaderVersion()) {
        PreparedMaterialParams candidate;
        candidate.source = snapshot; candidate.shaderPath = values.shaderPath;
        candidate.resourceOwner = resources.GetInstanceId(); candidate.resetVersion = resources.GetResetVersion();
        candidate.shaderVersion = resources.GetShaderVersion();
        candidate.shader = resources.LoadShader(values.shaderPath);
        const auto* shader = resources.Get(candidate.shader);
        if (shader) {
            candidate.descriptor = std::make_shared<const renderer::ShaderDescriptor>(shader->GetDescriptor());
            const auto vertex = resources.GetShaderCapabilities(candidate.shader).vertex;
            candidate.standardDescriptorVerified = vertex == renderer::ShaderVertexContract::STANDARD_SURFACE
                || vertex == renderer::ShaderVertexContract::STANDARD_SKINNED;
            candidate.bytes.assign(candidate.descriptor->cbufferSize, 0u);
            asset::InitDefaultMaterialParams(*candidate.descriptor, candidate.bytes);
            asset::ApplyMaterialAssetParams(values, *candidate.descriptor, candidate.bytes);
        }
        /// @note シェーダーのロード中に起こる再入編集や資源世代の変更を再照合し、古いレイアウトの公開を防ぐ。
        if (candidate.resourceOwner != resources.GetInstanceId() || candidate.resetVersion != resources.GetResetVersion()
            || candidate.shaderVersion != resources.GetShaderVersion() || asset::ReadMaterialAuthoring(handle) != snapshot
            || component.SlotAt(slotIndex).ReadAuthoring() != slotSnapshot || component.SlotAt(slotIndex).GetMaterialAsset() != handle)
            return nullptr;
        cached.prepared = std::move(candidate); cached.packedVariants.clear(); cached.synchronized.clear();
        cached.copyPlan.reset();
        if (cached.prepared.standardDescriptorVerified && cached.prepared.descriptor)
            cached.copyPlan.emplace(*cached.prepared.descriptor);
        cached.hasTextureRequests = std::any_of(values.textures.begin(), values.textures.end(),
            [](const auto& entry) { return !entry.second.empty(); });
    }
    return cached.prepared.descriptor ? &cached.prepared : nullptr;
}

bool RenderMaterialResolver::CopySynchronized(const MaterialSlot& slot, renderer::ResourceManager& resources, bool skinned,
    float screenPixels, renderer::RenderMaterial& output, bool shareTexturedDefaults) const
{
    if (!CanShareSynchronization(slot)) return false;
    const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    const auto found = m_prepared.find(snapshot);
    if (found == m_prepared.end() || (found->second.hasTextureRequests && !shareTexturedDefaults)) return false;
    const uint32_t textureRequest = found->second.hasTextureRequests ? std::bit_cast<uint32_t>(screenPixels) : 0;
    for (const auto& entry : found->second.synchronized) {
        if (entry.skinned == skinned && entry.textureRequest == textureRequest
            && entry.shaderVersion == resources.GetShaderVersion()
            && entry.resetVersion == resources.GetResetVersion()) {
            output = entry.material;
            return true;
        }
    }
    return false;
}

void RenderMaterialResolver::RememberSynchronized(const MaterialSlot& slot, renderer::ResourceManager& resources, bool skinned,
    float screenPixels, const renderer::RenderMaterial& material, bool shareTexturedDefaults)
{
    if (!material.valid || !material.paramsBuffer.IsValid() || !CanShareSynchronization(slot)) return;
    const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    const auto found = m_prepared.find(snapshot);
    if (found == m_prepared.end() || material.shader != found->second.prepared.shader
        || (found->second.hasTextureRequests && !shareTexturedDefaults)) return;
    auto& cached = found->second;
    /// @note 上限を超える投影サイズは通常経路で処理し、キャッシュの都合で要求解像度を丸めない。
    constexpr size_t MAXIMUM_SYNCHRONIZED_VARIANTS = 32;
    if (cached.synchronized.size() >= MAXIMUM_SYNCHRONIZED_VARIANTS) return;
    cached.synchronized.push_back({material, resources.GetShaderVersion(), resources.GetResetVersion(),
        cached.hasTextureRequests ? std::bit_cast<uint32_t>(screenPixels) : 0, skinned});
}

bool RenderMaterialResolver::CopyPacked(const MaterialSlot& slot, const renderer::Material& source, renderer::RenderMaterial& output,
    const renderer::ResourceManager& resources, const GBufferCopyPlan*& copyPlan,
    const renderer::ShaderDescriptor*& layout)
{
    copyPlan = nullptr; layout = nullptr;
    const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    const auto found = m_prepared.find(snapshot);
    if (found == m_prepared.end()) return false;
    const auto shaderVersion = resources.GetShaderVersion();
    for (const auto& packed : found->second.packedVariants) {
        if (packed.shaderVersion != shaderVersion || packed.shader != source.shader || packed.sourceBytes != source.paramData) continue;
        output.gbufferParams = packed.params; output.directGBufferParams = packed.direct;
        return true;
    }
    const auto& cached = found->second;
    const auto& prepared = cached.prepared;
    const auto vertex = output.shaderCapabilities.vertex;
    if (cached.copyPlan && prepared.descriptor && prepared.shader == source.shader
        && prepared.resourceOwner == resources.GetInstanceId() && prepared.resetVersion == resources.GetResetVersion()
        && prepared.shaderVersion == shaderVersion
        && (vertex == renderer::ShaderVertexContract::STANDARD_SURFACE || vertex == renderer::ShaderVertexContract::STANDARD_SKINNED)) {
        /// @note ポインターはこの抽出のキャッシュを参照し、コールバックを挟まないレイアウト照合にだけ使う。
        copyPlan = &*cached.copyPlan; layout = prepared.descriptor.get();
    }
    return false;
}

void RenderMaterialResolver::RememberPacked(const MaterialSlot& slot, const renderer::Material& source, const renderer::RenderMaterial& output,
    uint64_t shaderVersion)
{
    const auto snapshot = asset::ReadMaterialAuthoring(slot.GetMaterialAsset());
    const auto found = m_prepared.find(snapshot);
    if (found == m_prepared.end()) return;
    auto& variants = found->second.packedVariants;
    /// @note 固有上書きが大量にあるassetは通常経路へ戻し、同値共有のための追加メモリをasset当たり32種類へ制限する。
    constexpr size_t MAXIMUM_PACKED_VARIANTS = 32;
    if (variants.size() >= MAXIMUM_PACKED_VARIANTS) return;
    variants.push_back({source.shader, shaderVersion, source.paramData, output.gbufferParams, output.directGBufferParams});
}

bool RenderMaterialResolver::CanShareSynchronization(const MaterialSlot& slot)
{
    const auto& values = slot.Authoring();
    return slot.runtimeTextureOverrides.empty() && values.paramOverrides.empty() && values.integerParamOverrides.empty() && values.textureOverrides.empty()
        && values.presentationParamDefaults.empty() && values.presentationTextureDefaults.empty()
        && !values.hasPresentationDefaults && !values.hasPresentationMaterialOverride
        && !values.hasBlendModeOverride && !values.hasDoubleSidedOverride && !values.hasRenderQueueOverride;
}

}
