/// @file    GeometryPassHelpers.cpp
/// @brief   ジオメトリパス共有ヘルパー関数。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "GeometryPasses.hpp"
#include <Graphics/Renderer/BuiltinShader.hpp>
#include <Engine/Renderer/IPipelineState.hpp>
#include <Engine/Renderer/RenderVisibility.hpp>
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/MaterialParamBinding.hpp"
#include "Engine/Asset/StreamedTextureResolver.hpp"
#include "Engine/Asset/TexDescSerializer.hpp"
#include "Engine/Asset/TextureAsset.hpp"
#include "Engine/Core/Logger.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/AnimatorComponent.hpp"
#include "Engine/Scene/Components/MaterialComponent.hpp"
#include "Engine/Scene/Components/SkinnedMeshRenderer.hpp"
#include "Engine/Scene/Components/WeatherComponent.hpp"
#include "Engine/Renderer/Material.hpp"
#include "Engine/Renderer/IShader.hpp"
#include "Engine/Renderer/ITexture.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Util/FileSystem.hpp"
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace fbzz::scene {

namespace {

/// @note 束縛の規則そのものは `Engine/Asset/MaterialParamBinding.hpp` が持つ。UI のように「同じ .mat 形式を、違う頂点入力とパスで描く」側が増えると、無名名前空間に置いた実装は写経するしかなくなり写した先だけが追従しなくなるため、規則は 1 箇所に置く。
using asset::ApplyMaterialAssetParams;
using asset::ApplyMaterialParamOverrides;
using asset::FindMaterialParam;
using asset::InitDefaultMaterialParams;
constexpr auto& kTextureSlotNames = asset::kMaterialTextureSlotNames;

/// @note Reuse bounded per-thread byte capacity; nested calls own a local vector and never alias the outer Sprite/texture snapshot.
constexpr size_t MATERIAL_PARAMETER_SCRATCH_MAX_CAPACITY = 4096;
struct MaterialParameterScratchStorage {
    std::vector<uint8_t> bytes;
    bool borrowed = false;
};
MaterialParameterScratchStorage& MaterialParameterScratch()
{
    thread_local MaterialParameterScratchStorage storage;
    return storage;
}

class OwnedMaterialParameterSnapshot {
public:
    explicit OwnedMaterialParameterSnapshot(const std::vector<uint8_t>& values)
    {
        auto& storage = MaterialParameterScratch();
        if (!storage.borrowed && values.size() <= MATERIAL_PARAMETER_SCRATCH_MAX_CAPACITY) {
            storage.borrowed = true; m_storage = &storage; m_bytes = &storage.bytes;
        }
        m_bytes->assign(values.begin(), values.end());
        if (m_storage && m_bytes->capacity() > MATERIAL_PARAMETER_SCRATCH_MAX_CAPACITY) {
            m_local.swap(*m_bytes); m_bytes = &m_local;
        }
    }
    ~OwnedMaterialParameterSnapshot()
    {
        if (m_storage) { m_storage->bytes.clear(); m_storage->borrowed = false; }
    }
    OwnedMaterialParameterSnapshot(const OwnedMaterialParameterSnapshot&) = delete;
    OwnedMaterialParameterSnapshot& operator=(const OwnedMaterialParameterSnapshot&) = delete;
    std::vector<uint8_t>& Bytes() { return *m_bytes; }
private:
    std::vector<uint8_t> m_local;
    std::vector<uint8_t>* m_bytes = &m_local;
    MaterialParameterScratchStorage* m_storage = nullptr;
};

/// @brief albedo に Sprite サブアセットが指定された場合、Sprite 矩形を標準 UV 変換へ合成する。
/// @note GPU Texture は atlas 全体を共有するため、3D Material で個別 Sprite を使うには頂点 UV を矩形の scale/offset へ写像する必要がある。
/// @note 共有 .mat でなく実効 albedo 参照を受け取る。textureOverrides で GameObject 単位に albedo を差し替えると、共有 .mat 側を見ていては矩形が元のスプライトのまま残り「別のコマを指定したのに絵が変わらない」という原因不明の壊れ方をする。表情・目パチのようにコマをランタイムで切り替える用途はこの経路しか通らないため実効値で解決する。
void ApplyAlbedoSpriteUv(std::string_view albedoReference,
                         const renderer::ShaderDescriptor& desc,
                         renderer::ResourceManager& resources,
                         std::vector<uint8_t>& paramData)
{
    if (albedoReference.empty()) return;

    std::string texturePath;
    std::string spriteToken;
    if (!asset::ParseSpriteReference(albedoReference, texturePath, spriteToken)) return;

    /// @note 矩形の取り出しは ResolveSpriteReference が持つ (ID / 名前 / 暗黙 Single と
    /// @note .meta のキャッシュまで含めて 1 箇所)。ここは UV への合成だけを受け持つ。
    /// @note 矩形はピクセル単位なので元画像の寸法で換算する。品質段で縮小して常駐していても GPU 実体の寸法は使わない。
    asset::StreamedTextureResolver& resolver = asset::StreamedTextureResolver::Engine();
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    if (const asset::TextureAsset* textureAsset = resolver.ResolveAsset(resources, texturePath)) {
        sourceWidth = textureAsset->sourceWidth;
        sourceHeight = textureAsset->sourceHeight;
        if ((sourceWidth == 0 || sourceHeight == 0) && resources.Get(textureAsset->gpuHandle)) {
            sourceWidth = resources.Get(textureAsset->gpuHandle)->GetWidth();
            sourceHeight = resources.Get(textureAsset->gpuHandle)->GetHeight();
        }
    } else if (resolver.GetMissPolicy() == asset::StreamedTextureResolver::MissPolicy::Synchronous) {
        const std::string absoluteTexturePath = asset::AssetManager::ResolveAssetPath(texturePath);
        if (const renderer::ITexture* texture = resources.Get(resources.LoadTexture(absoluteTexturePath))) {
            sourceWidth = texture->GetWidth();
            sourceHeight = texture->GetHeight();
        }
    }
    if (sourceWidth == 0 || sourceHeight == 0) return;

    const asset::ResolvedSprite resolved = asset::ResolveSpriteReference(
        albedoReference,
        static_cast<float>(sourceWidth),
        static_cast<float>(sourceHeight));
    if (!resolved.resolved) return;

    /// @note 矩形が画像からはみ出していても UV は画像内へ収める。
    /// @note はみ出した分を素通しすると Clamp サンプリングで端の 1 列が伸びる。
    struct { float scaleX, scaleY, offsetX, offsetY; } transform{
        std::clamp(resolved.uvMax.x - resolved.uvMin.x, 0.0f, 1.0f),
        std::clamp(resolved.uvMax.y - resolved.uvMin.y, 0.0f, 1.0f),
        std::clamp(resolved.uvMin.x, 0.0f, 1.0f),
        std::clamp(resolved.uvMin.y, 0.0f, 1.0f),
    };

    const auto* tilingVar = desc.FindVar("uvTiling");
    const auto* offsetVar = desc.FindVar("uvOffset");
    if (tilingVar == nullptr || offsetVar == nullptr
        || tilingVar->varType != renderer::ShaderVarType::Float || tilingVar->columns < 2
        || offsetVar->varType != renderer::ShaderVarType::Float || offsetVar->columns < 2
        || tilingVar->offset + 2u * sizeof(float) > paramData.size()
        || offsetVar->offset + 2u * sizeof(float) > paramData.size())
        return;

    float tiling[2] = { 1.0f, 1.0f };
    float offset[2] = { 0.0f, 0.0f };
    std::memcpy(tiling, paramData.data() + tilingVar->offset, sizeof(tiling));
    std::memcpy(offset, paramData.data() + offsetVar->offset, sizeof(offset));
    tiling[0] *= transform.scaleX;
    tiling[1] *= transform.scaleY;
    offset[0] = offset[0] * transform.scaleX + transform.offsetX;
    offset[1] = offset[1] * transform.scaleY + transform.offsetY;
    std::memcpy(paramData.data() + tilingVar->offset, tiling, sizeof(tiling));
    std::memcpy(paramData.data() + offsetVar->offset, offset, sizeof(offset));
}

}

AnimatorComponent* FindAnimator(GameObject& go)
{
    /// @note Skinned submesh はモデル構造により複数階層下へ配置されるため、直親だけで打ち切らない。Animator を見失うと bind pose 用 CB へフォールバックし、Trail の初期位置もずれる。
    for (GameObject* current = &go; current; current = current->GetParent())
        if (auto* animator = current->GetComponent<AnimatorComponent>())
            return animator;
    return nullptr;
}

const char* GetFallbackMaterialPath(bool skinned)
{
    return skinned ? renderer::ERROR_SKINNED_SHADER : renderer::ERROR_SURFACE_SHADER;
}

void LogSkinnedSurfaceFallbackWarningOnce(std::string_view shaderPath)
{
    static std::unordered_set<std::string> s_warnedSurfaceOnSkinned;
    std::string path(shaderPath);
    if (path.empty()) path = "<empty>";
    if (!s_warnedSurfaceOnSkinned.insert(path).second) return;
    FBZZ_LOG_WARN("SkinnedMeshRenderer has a Surface shader assigned: %s -> using %s. Use shaders under Skinned/.",
                  path.c_str(),
                  GetFallbackMaterialPath(true));
}

renderer::Material* GetFallbackMaterial(renderer::ResourceManager& resources, bool skinned)
{
    return resources.GetErrorMaterial(skinned);
}

namespace material_detail {

/// @note Immutable pre-streaming derivation; these bytes never include Sprite UV, texture residency/masks/bindless indices or a GPU buffer.
struct EffectiveMaterialTemplate {
    MaterialSlot::AuthoringSnapshotPtr slot;
    asset::MaterialAsset::AuthoringSnapshotPtr asset;
    asset::AssetHandle<asset::MaterialAsset> assetHandle;
    uint64_t resourceOwner = 0, resetVersion = 0, shaderVersion = 0;
    renderer::ResourceHandle<renderer::ShaderTag> shader;
    std::shared_ptr<const renderer::ShaderDescriptor> descriptor;
    std::vector<uint8_t> bytes;
    std::array<std::string, asset::kMaterialTextureSlotNames.size()> texturePaths;
    bool skinned = false;
    renderer::ShaderVertexContract vertex;
};

/// @note Only this helper reads/writes the two private retained entries; publication compares the entry observed before deriving.
class EffectiveMaterialCache {
public:
    static std::shared_ptr<const EffectiveMaterialTemplate> Read(const MaterialSlot& slot, bool skinned)
    {
        return slot.m_effectiveTemplates[skinned ? 1u : 0u];
    }
    static void Publish(MaterialSlot& slot, bool skinned,
        const std::shared_ptr<const EffectiveMaterialTemplate>& expected,
        std::shared_ptr<const EffectiveMaterialTemplate> value)
    {
        auto& current = slot.m_effectiveTemplates[skinned ? 1u : 0u];
        if (current == expected && slot.ReadAuthoring() == value->slot && slot.GetMaterialAsset() == value->assetHandle)
            current = std::move(value);
    }
};

} /// @note namespace material_detail

namespace {

bool MaterialShaderLayoutEqual(const renderer::ShaderDescriptor& lhs, const renderer::ShaderDescriptor& rhs)
{
    if (&lhs == &rhs) return true;
    if (lhs.cbufferSize != rhs.cbufferSize || lhs.textureMaskOffset != rhs.textureMaskOffset
        || lhs.vars.size() != rhs.vars.size() || lhs.postProcessVars.size() != rhs.postProcessVars.size()
        || lhs.textures.size() != rhs.textures.size()) return false;
    const auto varsEqual = [](const auto& a, const auto& b) {
        for (size_t i = 0; i < a.size(); ++i) {
            const auto& x = a[i]; const auto& y = b[i];
            if (x.name != y.name || x.offset != y.offset || x.size != y.size || x.rows != y.rows
                || x.columns != y.columns || x.varClass != y.varClass || x.varType != y.varType
                || x.elements != y.elements || x.arrayStride != y.arrayStride || x.matrixStride != y.matrixStride
                || x.rowMajor != y.rowMajor || x.unsupportedReason != y.unsupportedReason) return false;
        }
        return true;
    };
    if (!varsEqual(lhs.vars, rhs.vars) || !varsEqual(lhs.postProcessVars, rhs.postProcessVars)) return false;
    for (size_t i = 0; i < lhs.textures.size(); ++i) {
        const auto& a = lhs.textures[i]; const auto& b = rhs.textures[i];
        if (a.name != b.name || a.slot != b.slot || a.constantOffset != b.constantOffset) return false;
    }
    return true;
}

bool EffectiveMaterialWitnessMatches(const material_detail::EffectiveMaterialTemplate& value,
    const MaterialSlot::AuthoringSnapshotPtr& slot, const asset::MaterialAsset::AuthoringSnapshotPtr& material,
    asset::AssetHandle<asset::MaterialAsset> handle, renderer::ResourceManager& resources,
    renderer::ResourceHandle<renderer::ShaderTag> shader, const renderer::ShaderDescriptor& descriptor, bool skinned)
{
    return value.slot == slot && value.asset == material && value.assetHandle == handle
        && value.resourceOwner == resources.GetInstanceId() && value.resetVersion == resources.GetResetVersion()
        && value.shaderVersion == resources.GetShaderVersion() && value.shader == shader && value.skinned == skinned
        && value.descriptor && MaterialShaderLayoutEqual(*value.descriptor, descriptor);
}

bool EffectiveMaterialInputsCurrent(const MaterialComponent& component, size_t slotIndex,
    const material_detail::EffectiveMaterialTemplate& value, const renderer::ResourceManager& resources)
{
    const auto& currentSlot = component.SlotAt(slotIndex);
    const auto current = currentSlot.ReadAuthoring();
    /// @note A same-owner edit does not mutate the captured call snapshot; only publication must require its exact current revision.
    return current && current->OwnerToken() == value.slot->OwnerToken()
        && resources.GetInstanceId() == value.resourceOwner && resources.GetResetVersion() == value.resetVersion
        && resources.GetShaderVersion() == value.shaderVersion && resources.GetShaderCapabilities(value.shader).vertex == value.vertex;
}

bool EffectiveMaterialLayoutCurrent(const material_detail::EffectiveMaterialTemplate& value,
    renderer::ResourceManager& resources)
{
    const auto* shader = resources.Get(value.shader);
    const bool matches = shader && value.descriptor && MaterialShaderLayoutEqual(*value.descriptor, shader->GetDescriptor());
    return matches && resources.GetInstanceId() == value.resourceOwner && resources.GetResetVersion() == value.resetVersion
        && resources.GetShaderVersion() == value.shaderVersion;
}

} /// @note namespace

/// @pre The component remains alive for this call. Slot-vector edits are re-resolved after resource callbacks; no slot reference crosses them.
static renderer::Material* SyncMaterialSlotImpl(MaterialComponent& component, size_t slotIndex,
    renderer::ResourceManager& resources, bool preferSkinnedFallback, float screenPixels,
    const PreparedMaterialParams* prepared = nullptr)
{
    const auto useErrorMaterial = [&](const char* reason) {
        static std::unordered_set<std::string> warned;
        const std::string reference = component.SlotAt(slotIndex).GetMaterialPath().empty()
            ? "<unassigned>" : component.SlotAt(slotIndex).GetMaterialPath();
        if (warned.insert(reference + "|" + reason).second)
            FBZZ_LOG_WARN("Material '%s': %s -> using %s.", reference.c_str(), reason,
                GetFallbackMaterialPath(preferSkinnedFallback));
        return resources.GetErrorMaterial(preferSkinnedFallback);
    };
    /// @note Reentrant edits get a fresh derivation. Continually mutating custom callbacks fail closed after this bounded retry.
    for (size_t attempt = 0; attempt < 2; ++attempt) {
        if (!component.enabled || !component.SlotAt(slotIndex).visible) return nullptr;
        if (!component.EnsureMaterialAsset(slotIndex)) {
            if (attempt + 1u < 2u) continue;
            return useErrorMaterial("material load failed or selected slot changed");
        }
        if (!component.enabled || !component.SlotAt(slotIndex).visible) return nullptr;
        const auto slotSnapshot = component.SlotAt(slotIndex).ReadAuthoring();
        const auto handle = component.SlotAt(slotIndex).GetMaterialAsset();
        const auto assetSnapshot = asset::ReadMaterialAuthoring(handle);
        if (!slotSnapshot || !assetSnapshot) return useErrorMaterial("material load failed");
        const auto& slotData = slotSnapshot->Data(); const auto& assetData = assetSnapshot->Data();
        if (assetData.shaderPath.empty()) return useErrorMaterial("empty shader path");
        if (assetData.renderPath != asset::RenderPath::Auto) return useErrorMaterial("material is not declared for mesh rendering");
        if (!preferSkinnedFallback && assetData.meshType == asset::MeshType::Skinned)
            return useErrorMaterial("skinned material assigned to a static mesh");
        if (preferSkinnedFallback && assetData.meshType == asset::MeshType::Surface) {
            LogSkinnedSurfaceFallbackWarningOnce(assetData.shaderPath); return resources.GetErrorMaterial(true);
        }

        const uint64_t owner = resources.GetInstanceId(), reset = resources.GetResetVersion(), shaderVersion = resources.GetShaderVersion();
        const bool preparationCandidate = prepared && prepared->source == assetSnapshot
            && prepared->resourceOwner == owner && prepared->resetVersion == reset && prepared->shaderVersion == shaderVersion
            && prepared->shaderPath == assetData.shaderPath && prepared->shader.IsValid() && prepared->descriptor;
        const auto preparedVertex = preparationCandidate ? resources.GetShaderCapabilities(prepared->shader).vertex
            : renderer::ShaderVertexContract{};
        const auto* preparedNative = preparationCandidate ? resources.Get(prepared->shader) : nullptr;
        const bool preparationLayoutMatches = preparedNative && MaterialShaderLayoutEqual(*prepared->descriptor, preparedNative->GetDescriptor());
        const bool preparedReady = preparationCandidate && preparationLayoutMatches && prepared->standardDescriptorVerified
            && (preparedVertex == renderer::ShaderVertexContract::STANDARD_SURFACE || preparedVertex == renderer::ShaderVertexContract::STANDARD_SKINNED)
            && prepared->bytes.size() == prepared->descriptor->cbufferSize;
        const bool preparationMatches = preparationCandidate && preparationLayoutMatches && (preparedReady || preparedNative);
        const auto shaderHandle = preparationMatches ? prepared->shader : resources.LoadShader(assetData.shaderPath);
        const auto* shader = preparedReady ? nullptr : resources.Get(shaderHandle);
        if (!preparedReady && !shader) return useErrorMaterial("shader load failed");
        const auto vertex = preparedReady ? preparedVertex : resources.GetShaderCapabilities(shaderHandle).vertex;
        if ((preferSkinnedFallback && vertex == renderer::ShaderVertexContract::STANDARD_SURFACE)
            || (!preferSkinnedFallback && vertex == renderer::ShaderVertexContract::STANDARD_SKINNED))
            return useErrorMaterial("shader vertex contract does not match mesh");
        /// @note Unknown/native custom descriptor access is copied before callbacks and bypasses persistent reuse.
        auto descriptor = preparedReady ? prepared->descriptor
            : std::make_shared<const renderer::ShaderDescriptor>(shader->GetDescriptor());
        if (resources.GetInstanceId() != owner || resources.GetResetVersion() != reset || resources.GetShaderVersion() != shaderVersion
            || component.SlotAt(slotIndex).ReadAuthoring() != slotSnapshot || component.SlotAt(slotIndex).GetMaterialAsset() != handle
            || asset::ReadMaterialAuthoring(handle) != assetSnapshot) continue;

        const bool cacheable = vertex == renderer::ShaderVertexContract::STANDARD_SURFACE
            || vertex == renderer::ShaderVertexContract::STANDARD_SKINNED;
        const auto previous = material_detail::EffectiveMaterialCache::Read(component.SlotAt(slotIndex), preferSkinnedFallback);
        std::shared_ptr<const material_detail::EffectiveMaterialTemplate> effective;
        if (cacheable && previous && EffectiveMaterialWitnessMatches(*previous, slotSnapshot, assetSnapshot, handle,
            resources, shaderHandle, *descriptor, preferSkinnedFallback)) effective = previous;
        else {
            material_detail::EffectiveMaterialTemplate derived;
            derived.slot = slotSnapshot; derived.asset = assetSnapshot; derived.assetHandle = handle;
            derived.resourceOwner = owner; derived.resetVersion = reset; derived.shaderVersion = shaderVersion;
            derived.shader = shaderHandle; derived.descriptor = std::move(descriptor); derived.skinned = preferSkinnedFallback; derived.vertex = vertex;
            if (preparationMatches && MaterialShaderLayoutEqual(*prepared->descriptor, *derived.descriptor)
                && prepared->bytes.size() == derived.descriptor->cbufferSize) derived.bytes = prepared->bytes;
            else {
                derived.bytes.assign(derived.descriptor->cbufferSize, 0u);
                InitDefaultMaterialParams(*derived.descriptor, derived.bytes);
                ApplyMaterialAssetParams(assetData, *derived.descriptor, derived.bytes);
            }
            ApplyMaterialParamOverrides(slotData.presentationParamDefaults, *derived.descriptor, derived.bytes);
            ApplyMaterialParamOverrides(slotData.paramOverrides, *derived.descriptor, derived.bytes);
            asset::ApplyMaterialIntegerOverrides(slotData.integerParamOverrides, *derived.descriptor, derived.bytes);
            for (size_t i = 0; i < derived.texturePaths.size(); ++i) {
                const auto* key = kTextureSlotNames[i];
                if (const auto found = assetData.textures.find(key); found != assetData.textures.end()) derived.texturePaths[i] = found->second;
                if (const auto found = slotData.presentationTextureDefaults.find(key); found != slotData.presentationTextureDefaults.end()) derived.texturePaths[i] = found->second;
                if (const auto found = slotData.textureOverrides.find(key); found != slotData.textureOverrides.end()) derived.texturePaths[i] = found->second;
            }
            effective = std::make_shared<const material_detail::EffectiveMaterialTemplate>(std::move(derived));
        }
        if (!EffectiveMaterialInputsCurrent(component, slotIndex, *effective, resources)
            || !EffectiveMaterialLayoutCurrent(*effective, resources)) continue;

        /// @note Own per-call CPU results rather than a runtime Material reference across Sprite/texture callbacks.
        OwnedMaterialParameterSnapshot viewParameterSnapshot(effective->bytes);
        auto& viewBytes = viewParameterSnapshot.Bytes();
        std::array<renderer::ResourceHandle<renderer::TextureTag>, asset::kMaterialTextureSlotNames.size()> viewTextures{};
        ApplyAlbedoSpriteUv(effective->texturePaths[0], *effective->descriptor, resources, viewBytes);
        if (!EffectiveMaterialInputsCurrent(component, slotIndex, *effective, resources)) continue;
        auto& resolver = asset::StreamedTextureResolver::Engine();
        for (size_t i = 0; i < effective->texturePaths.size(); ++i) {
            /// @note Empty paths call no resolver or callback; the final check still covers the complete texture pass.
            if (effective->texturePaths[i].empty()) continue;
            viewTextures[i] = resolver.ResolveGpu(resources, effective->texturePaths[i], screenPixels);
            if (!EffectiveMaterialInputsCurrent(component, slotIndex, *effective, resources)) break;
        }
        if (!EffectiveMaterialInputsCurrent(component, slotIndex, *effective, resources)) continue;
        /// @note Re-resolve after callbacks, preserving a nested Sync's unique GPU owner and the existing native Upload lifetime contract.
        const auto& runtimeTextures = component.SlotAt(slotIndex).runtimeTextureOverrides;
        for (size_t i = 0; i < viewTextures.size(); ++i) {
            const auto found = runtimeTextures.find(kTextureSlotNames[i]);
            if (found == runtimeTextures.end()) continue;
            const auto& binding = found->second;
            if (binding.resourceOwner == resources.GetInstanceId() && binding.resetVersion == resources.GetResetVersion()
                && resources.Get(binding.texture)) viewTextures[i] = binding.texture;
        }
        auto& target = component.SlotAt(slotIndex).material;
        if (!target) target = std::make_unique<renderer::Material>();
        auto* material = target.get();
        material->shaderPath = assetData.shaderPath; material->dielectric = assetData.dielectric;
        material->shader = shaderHandle; material->paramData.assign(viewBytes.begin(), viewBytes.end());
        material->textures.assign(viewTextures.begin(), viewTextures.end());
        if (!EffectiveMaterialLayoutCurrent(*effective, resources)) continue;
        material->Upload(resources, *effective->descriptor);
        if (!EffectiveMaterialLayoutCurrent(*effective, resources)) continue;
        if (!EffectiveMaterialInputsCurrent(component, slotIndex, *effective, resources)) continue;
        if (effective->descriptor->cbufferSize > 0 && !resources.Get(material->paramsBuffer))
            return useErrorMaterial("material constant buffer unavailable");
        if (!EffectiveMaterialInputsCurrent(component, slotIndex, *effective, resources)) continue;
        if (cacheable && component.SlotAt(slotIndex).ReadAuthoring() == slotSnapshot
            && component.SlotAt(slotIndex).GetMaterialAsset() == handle && asset::ReadMaterialAuthoring(handle) == assetSnapshot)
            material_detail::EffectiveMaterialCache::Publish(component.SlotAt(slotIndex), preferSkinnedFallback, previous, effective);
        return component.SlotAt(slotIndex).material.get();
    }
    return useErrorMaterial("authoring or resource changed during synchronization");
}

renderer::Material* SyncMaterial(MaterialComponent& mc, renderer::ResourceManager& resources,
    bool preferSkinnedFallback, float screenPixels)
{
    return SyncMaterialSlotImpl(mc, 0, resources, preferSkinnedFallback, screenPixels);
}

renderer::Material* SyncMaterialSlot(MaterialComponent& mc, size_t slotIndex,
    renderer::ResourceManager& resources, bool preferSkinnedFallback, float screenPixels)
{
    return SyncMaterialSlotImpl(mc, slotIndex, resources, preferSkinnedFallback, screenPixels);
}

renderer::Material* SyncMaterialSlotPrepared(MaterialComponent& mc, size_t slotIndex,
    renderer::ResourceManager& resources, bool preferSkinnedFallback, float screenPixels,
    const PreparedMaterialParams* prepared)
{
    return SyncMaterialSlotImpl(mc, slotIndex, resources, preferSkinnedFallback, screenPixels, prepared);
}

float EstimateScreenPixels(const RenderPassContext& ctx, const GameObject& go, const renderer::Mesh& mesh)
{
    if (ctx.cullProjScaleY <= 0.0f || ctx.height == 0 || mesh.boundsRadius <= 0.0f) return 0.0f;
    const WorldBounds bounds = ComputeWorldBounds(go.transform, mesh);
    /// @note 投影半径 (NDC) = r * P[1][1] / 距離。NDC の縦 [-1, 1] が height px なので直径は r*P11/d*height。
    /// @note カメラが球の中へ入ったら画面いっぱいとして扱う。
    const float distance = (bounds.center - ctx.camera.m_position).Length();
    if (!ctx.cullOrthographic && distance <= bounds.radius) return static_cast<float>(ctx.height);
    const float ndcRadius = ctx.cullOrthographic
        ? bounds.radius * ctx.cullProjScaleY
        : bounds.radius * ctx.cullProjScaleY / distance;
    return ndcRadius * static_cast<float>(ctx.height);
}

renderer::ResourceHandle<renderer::PipelineStateTag> GetOrCreateMaterialPSO(
    renderer::ResourceManager& resources,
    const MaterialSlot&        slot,
    bool                      wireframe)
{
    return GetOrCreateMaterialPSO(resources, slot.GetBlendMode(), slot.IsDoubleSided(),
                                  slot.GetDepthBias(), slot.GetDepthBiasSlope(), wireframe);
}

bool ShouldRenderGameObject(const GameObject& go, fbzz::LayerMask mask)
{
    return go.activeInHierarchy() && fbzz::Layer::Contains(mask, go.layer);
}

/// @note かつてここに IsForwardOnly があった。GBuffer に入れるかどうかの判断は
/// @note ResolveGeometryRoute (Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp) が唯一の正本。
/// @note 判定が 2 つあると片方だけ古くなり、物が二重に描かれるか黙って消える。
/// @see Docs/design/pipeline-boundary.md

bool IsSurfaceMaterial(const MaterialSlot& mc)
{
    const auto snapshot = asset::ReadMaterialAuthoring(mc.GetMaterialAsset());
    const auto* a = snapshot ? &snapshot->Data() : nullptr;
    if (a) {
        if (a->meshType == asset::MeshType::Surface) return true;
        if (a->meshType == asset::MeshType::Skinned) return false;
        if (a->shaderPath.empty()) return false;
    }
    return false;
}

/// @name カリング ヘルパー

bool IsReliableOccluder(const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius <= 0.0f) return false;

    /// @note 立方体で 0.577、球で 1.0。板・壁・棒はこれを大きく下回る。
    /// @note 0.40 は「立方体は通し、厚み比 1:4 を超える扁平は落とす」あたりの線。
    constexpr float kMinFillRatio = 0.40f;
    const float minExtent = std::min({ mesh.boundsExtents.x,
                                       mesh.boundsExtents.y,
                                       mesh.boundsExtents.z });
    return minExtent / mesh.boundsRadius >= kMinFillRatio;
}

WorldBounds ComputeWorldBounds(const Transform& tf, const renderer::Mesh& mesh, float padding)
{
    const math::Matrix4 world = tf.GetWorldMatrix();
    return ComputeWorldBounds(world, ComputeWorldBoundsScale(world), mesh, padding);
}

float ComputeWorldBoundsScale(const math::Matrix4& world)
{
    /// @note Maximum axis norm keeps spheres conservative under nonuniform and negative world scale.
    const float sx = std::sqrt(world.m[0][0]*world.m[0][0] + world.m[1][0]*world.m[1][0] + world.m[2][0]*world.m[2][0]);
    const float sy = std::sqrt(world.m[0][1]*world.m[0][1] + world.m[1][1]*world.m[1][1] + world.m[2][1]*world.m[2][1]);
    const float sz = std::sqrt(world.m[0][2]*world.m[0][2] + world.m[1][2]*world.m[1][2] + world.m[2][2]*world.m[2][2]);
    return std::max({ sx, sy, sz });
}

WorldBounds ComputeWorldBounds(const math::Matrix4& world, float maximumScale,
                               const renderer::Mesh& mesh, float padding)
{

    /// @note ローカル空間バウンディング球中心をワールド空間に変換する。
    /// @note 行列は列ベクトル規則 (M * v) なので:
    /// @note wx = m[0][0]*bx + m[0][1]*by + m[0][2]*bz + m[0][3]
    const float bx = mesh.boundsCenter.x;
    const float by = mesh.boundsCenter.y;
    const float bz = mesh.boundsCenter.z;
    const math::Vector3 worldCenter = {
        world.m[0][0]*bx + world.m[0][1]*by + world.m[0][2]*bz + world.m[0][3],
        world.m[1][0]*bx + world.m[1][1]*by + world.m[1][2]*bz + world.m[1][3],
        world.m[2][0]*bx + world.m[2][1]*by + world.m[2][2]*bz + world.m[2][3],
    };

    return { worldCenter, mesh.boundsRadius * maximumScale + (padding > 0.0f ? padding : 0.0f) };
}

bool ComputeSkinnedWorldBounds(const GameObject& go,
                               const SkinnedMeshRenderer& smr,
                               WorldBounds& outBounds,
                               float padding)
{
    if (!smr.model) return false;

    const Transform& tf = go.transform;

    /// @note 骨から作った球があればそちらを使う。下のループはバインドポーズの submesh バウンズを Renderer の Transform で運ぶだけで骨の移動を見ないため、Script が骨を数十 m 動かす構成では球がバインドポーズの場所に取り残され部位ごとに明滅する。
    /// @note Animator が持つのは骨の広がりだけで周囲のジオメトリは submesh のバインド半径ぶん外へ出るため、この Renderer が描く submesh の最大値を肉の厚みとして足す。
    /// @note GetComponent に const 版が無いので剥がす。ここは読むだけ。
    for (GameObject* current = const_cast<GameObject*>(&go); current;
         current = current->GetParent()) {
        const auto* animator = current->GetComponent<AnimatorComponent>();
        if (!animator) continue;
        if (animator->skinnedBoundsRadius <= 0.0f) break;

        float flesh = 0.0f;
        for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot)
            if (const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot))
                flesh = (std::max)(flesh, meshPtr->boundsRadius);

        /// @note 球は Animator の owner のローカル空間にある。運ぶのもその Transform。
        const math::Matrix4 world = current->transform.GetWorldMatrix();
        const math::Vector3& c = animator->skinnedBoundsCenter;
        outBounds.center = {
            world.m[0][0]*c.x + world.m[0][1]*c.y + world.m[0][2]*c.z + world.m[0][3],
            world.m[1][0]*c.x + world.m[1][1]*c.y + world.m[1][2]*c.z + world.m[1][3],
            world.m[2][0]*c.x + world.m[2][1]*c.y + world.m[2][2]*c.z + world.m[2][3],
        };
        const float maxScale = ComputeWorldBoundsScale(world);

        outBounds.radius = (animator->skinnedBoundsRadius + flesh) * maxScale;
        if (padding > 0.0f) outBounds.radius += padding;
        return true;
    }

    bool hasBounds = false;
    math::Vector3 weightedCenter = math::Vector3::ZERO;
    float totalWeight = 0.0f;
    math::Matrix4 world;
    float maximumScale = 0.0f;

    /// @note 各 submesh のワールド球を半径重みで平均し、最後に全 submesh を包む半径へ拡張する。毎フレーム CPU スキニングして厳密 bounds を取ると頂点数に比例して重いため、視錐台外の遠いキャラクターを安く除外できる保守的なバインドポーズ球を使う。
    /// @note この Renderer が描く submesh だけを包む。ノードごとに子 GameObject へ分けた構成ではモデル全体の球はどの子にとっても過大になり画面外の部位のぶんまで視錐台に残るため、カリングの単位は「実際に描くもの」に揃える。
    for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot) {
        const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot);
        if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
        if (!hasBounds) { world = tf.GetWorldMatrix(); maximumScale = ComputeWorldBoundsScale(world); }
        const WorldBounds bounds = ComputeWorldBounds(world, maximumScale, *meshPtr);
        const float weight = (std::max)(bounds.radius, 0.001f);
        weightedCenter = weightedCenter + bounds.center * weight;
        totalWeight += weight;
        hasBounds = true;
    }

    if (!hasBounds || totalWeight <= 0.0f) return false;

    outBounds.center = weightedCenter * (1.0f / totalWeight);
    outBounds.radius = 0.0f;
    /// @note この Renderer が描く submesh だけを包む。モデル全体で取ると、ノードごとに子 GameObject へ分けた構成では過大な球になり画面外の部位のぶんまで視錐台に残るため、カリングの単位は「実際に描くもの」に揃える。
    for (size_t slot = 0; slot < smr.SubmeshCount(); ++slot) {
        const renderer::Mesh* meshPtr = smr.SubmeshMesh(slot);
        if (!meshPtr || meshPtr->boundsRadius <= 0.0f) continue;
        const WorldBounds bounds = ComputeWorldBounds(world, maximumScale, *meshPtr);
        const math::Vector3 delta = bounds.center - outBounds.center;
        outBounds.radius = (std::max)(outBounds.radius, delta.Length() + bounds.radius);
    }

    /// @note 余白は submesh ごとではなく合成後の球へ 1 回だけ足す。ComputeWorldBounds へ渡して submesh 単位で足すと、合成時の「中心距離 + 半径」に padding が二重・三重で積み上がる。
    if (padding > 0.0f) outBounds.radius += padding;

    return true;
}

namespace {

/// @note このレイヤーに適用する描画距離 [m] を返す。0 は「距離カリングしない」。
float ResolveCullDistance(const RenderPassContext& ctx, int layer)
{
    if (ctx.hasLayerCullDistances) {
        const float perLayer = ctx.cullLayerDistances[layer & (kCullLayerCount - 1)];
        if (perLayer > 0.0f) return perLayer;
    }
    return ctx.cullMaxDistance;
}

renderer::RenderCullingView ExtractCullingView(const RenderPassContext& ctx)
{
    renderer::RenderCullingView view;
    view.position = ctx.camera.m_position;
    view.forward = ctx.cullCameraForward;
    view.frustum = ctx.frustumCullingEnabled ? ctx.cameraFrustum : nullptr;
    view.projectionScaleY = ctx.cullProjScaleY;
    view.smallObjectScreenHeight = ctx.smallObjectScreenHeight;
    view.distanceSpherical = ctx.cullDistanceSpherical;
    view.orthographic = ctx.cullOrthographic;
    return view;
}

bool TestBoundsVisible(RenderPassContext& ctx, const GameObject& go, const WorldBounds& bounds)
{
    const renderer::RenderCullingItem item{
        bounds.center, bounds.radius, ResolveCullDistance(ctx, go.layer)
    };
    switch (renderer::EvaluateVisibility(ExtractCullingView(ctx), item)) {
    case renderer::VisibilityResult::DISTANCE_CULLED:
        ++ctx.statsDistanceCulled;
        return false;
    case renderer::VisibilityResult::SMALL_OBJECT_CULLED:
        ++ctx.statsSmallObjectCulled;
        return false;
    case renderer::VisibilityResult::FRUSTUM_CULLED:
        ++ctx.statsFrustumCulled;
        return false;
    case renderer::VisibilityResult::VISIBLE:
        return true;
    }
    return true;
}

}

bool IsMeshVisible(RenderPassContext& ctx,
                   const GameObject& go,
                   const renderer::Mesh& mesh)
{
    if (mesh.boundsRadius <= 0.0f) return true;
    const WorldBounds bounds =
        ComputeWorldBounds(go.transform, mesh, ctx.cullingBoundsPadding);
    return TestBoundsVisible(ctx, go, bounds);
}

bool IsSkinnedVisible(RenderPassContext& ctx,
                      const GameObject& go,
                      const SkinnedMeshRenderer& smr)
{
    WorldBounds bounds{};
    /// @note bounds を作れない (CPU 頂点未生成など) 場合は安全側に倒して描く。
    if (!ComputeSkinnedWorldBounds(go, smr, bounds, ctx.cullingBoundsPadding))
        return true;
    return TestBoundsVisible(ctx, go, bounds);
}

bool IsWithinCullDistance(const RenderPassContext& ctx,
                          const GameObject& go,
                          const WorldBounds& bounds)
{
    return renderer::IsWithinDrawDistance(ExtractCullingView(ctx), {
        bounds.center, bounds.radius, ResolveCullDistance(ctx, go.layer)
    });
}

ActiveWeather FindActiveWeather(Scene& scene)
{
    ActiveWeather result;
    const auto entities = scene.GetEntities<WeatherComponent>();
    if (entities.empty()) return result;
    /// @note This callback-free query retains capacity only; component packing order must not change the first Scene-owned weather.
    thread_local std::vector<EntityID> candidates;
    candidates.assign(entities.begin(), entities.end());
    scene.SortEntitiesBySceneOrder(candidates);
    for (const auto entity : candidates) {
        auto* go = scene.GetGameObject(entity);
        if (!go || !go->activeInHierarchy()) continue;
        const auto* weather = go->GetComponent<WeatherComponent>();
        if (!weather || !weather->enabled) continue;
        result.wetness      = std::clamp(weather->wetness, 0.0f, 1.0f);
        result.darkening    = std::clamp(weather->darkening, 0.0f, 1.0f);
        result.puddleAmount = std::clamp(weather->puddleAmount, 0.0f, 1.0f);
        /// @note シーンに 1 つ想定。環境風と同じ扱い
        break;
    }
    candidates.clear();
    return result;
}

bool IsEffectTextureSrgb(const std::string& texturePath)
{
    /// @note 1x1 白フォールバックなど参照が無い場合。1.0 はどちらの空間でも 1.0 なので変換しない。
    if (texturePath.empty()) return false;

    /// @note 毎フレーム呼ばれる経路 (Trail) があるため結果を持つ。.meta の更新時刻で無効化して、
    /// @note エディターで再インポートしたときに古い判定が残らないようにする。
    struct CachedSrgb {
        std::filesystem::file_time_type metaWriteTime{};
        bool srgb = true;
        bool resolved = false;
    };
    static std::unordered_map<std::string, CachedSrgb> s_cache;

    const std::string resolved = asset::AssetManager::ResolveAssetPath(texturePath);
    std::string sourcePath;
    if (!asset::TexDescSerializer::ResolveSourcePath(resolved, sourcePath)) return true;
    const std::string metaPath = sourcePath + ".meta";
    std::error_code ec;
    const auto metaWriteTime = std::filesystem::last_write_time(metaPath, ec);

    CachedSrgb& cached = s_cache[texturePath];
    if (cached.resolved && cached.metaWriteTime == metaWriteTime) return cached.srgb;
    cached.resolved = true;
    cached.metaWriteTime = metaWriteTime;

    asset::TextureAsset described;
    const asset::TexDescSerializer serializer;
    if (!ec && serializer.Load(metaPath, described)) {
        cached.srgb = described.settings.srgb;
        return cached.srgb;
    }
    /// @note サイドカーが無い素材はインポーターと同じ推定に従う (色テクスチャ = sRGB)。
    const auto slash = sourcePath.find_last_of("/\\");
    const std::string filename =
        slash == std::string::npos ? sourcePath : sourcePath.substr(slash + 1);
    cached.srgb = asset::DefaultSettingsForType(asset::GuessTextureType(filename)).srgb;
    return cached.srgb;
}

}
