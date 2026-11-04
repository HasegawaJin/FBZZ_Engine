/// @file    MaterialComponent.hpp
/// @brief   GameObject が参照する .mat マテリアルアセットと GPU Material キャッシュ。
/// @author  Hasegawa Jin
/// @date    2026-05-24
#pragma once

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/Material.hpp>
#include <Engine/Renderer/RenderLayer.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/Reflection.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <utility>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

/// @brief Owned mutable drafts are distinct from each published const slot snapshot.
struct MaterialSlotAuthoring {
    asset::AssetHandle<asset::MaterialAsset> materialAsset;
    std::string materialPath;
    std::unordered_map<std::string, std::vector<float>> paramOverrides;
    std::unordered_map<std::string, std::vector<int64_t>> integerParamOverrides;
    std::unordered_map<std::string, std::string> textureOverrides;
    bool hasBlendModeOverride = false;
    renderer::BlendMode blendModeOverride = renderer::BlendMode::OPAQUE_BLEND;
    bool hasDoubleSidedOverride = false;
    bool doubleSidedOverride = false;
    bool hasRenderQueueOverride = false;
    int32_t renderQueueOverride = renderer::RenderQueue::GEOMETRY;
    bool hasPresentationDefaults = false;
    bool hasPresentationMaterialOverride = false;
    std::unordered_map<std::string, std::vector<float>> presentationParamDefaults;
    std::unordered_map<std::string, std::string> presentationTextureDefaults;
    bool presentationDoubleSided = true;
    int32_t presentationRenderQueue = renderer::RenderQueue::TRANSPARENT_QUEUE;
    bool operator==(const MaterialSlotAuthoring& other) const
    {
        return materialAsset == other.materialAsset && materialPath == other.materialPath
            && asset::detail::MaterialAuthoringFloatMapsEqual(paramOverrides, other.paramOverrides)
            && integerParamOverrides == other.integerParamOverrides && textureOverrides == other.textureOverrides
            && hasBlendModeOverride == other.hasBlendModeOverride && blendModeOverride == other.blendModeOverride
            && hasDoubleSidedOverride == other.hasDoubleSidedOverride && doubleSidedOverride == other.doubleSidedOverride
            && hasRenderQueueOverride == other.hasRenderQueueOverride && renderQueueOverride == other.renderQueueOverride
            && hasPresentationDefaults == other.hasPresentationDefaults
            && hasPresentationMaterialOverride == other.hasPresentationMaterialOverride
            && asset::detail::MaterialAuthoringFloatMapsEqual(presentationParamDefaults, other.presentationParamDefaults)
            && presentationTextureDefaults == other.presentationTextureDefaults
            && presentationDoubleSided == other.presentationDoubleSided && presentationRenderQueue == other.presentationRenderQueue;
    }
};

namespace material_detail {
struct EffectiveMaterialTemplate;
class EffectiveMaterialCache;
}

/// @note Transient bindings never enter authoring snapshots or Scene copies; resource identity protects reset and handle reuse.
struct RuntimeMaterialTexture {
    renderer::ResourceHandle<renderer::TextureTag> texture;
    uint64_t resourceOwner = 0;
    uint64_t resetVersion = 0;
    std::string propertyName;
};

/// @note Slot authoring is private; runtime Material remains uniquely owned and is refreshed on every synchronization.
struct MaterialSlot {
public:
    using AuthoringState = asset::detail::MaterialAuthoringState<MaterialSlotAuthoring>;
    using AuthoringSnapshot = AuthoringState::Snapshot;
    using AuthoringSnapshotPtr = AuthoringState::SnapshotPtr;
    using AuthoringEdit = AuthoringState::Edit;
    /// @note Live selection/gating is evaluated on every Sync/SlotAt; it is deliberately outside the effective CPU template.
    bool visible = true;
    std::unique_ptr<renderer::Material> material;
    std::unordered_map<std::string, RuntimeMaterialTexture> runtimeTextureOverrides;
    std::unordered_map<uint64_t, std::string> propertyNameCache;
    std::unordered_map<uint64_t, uint8_t> propertyValidationCache;
    const void* propertyValidationDescriptor = nullptr;

    MaterialSlot() = default;
    ~MaterialSlot() = default;
    /// @note Copy keeps values but creates a new edit/cache identity and does not copy GPU ownership.
    MaterialSlot(const MaterialSlot& other)
        : visible(other.visible), material(other.material ? std::make_unique<renderer::Material>(other.material->CloneWithoutGpuResources()) : nullptr),
          propertyNameCache(other.propertyNameCache), m_authoring(other.m_authoring),
          m_resolvedAsset(other.GetMaterialAsset()), m_resolvedReferenceRevision(m_referenceRevision) {}
    MaterialSlot& operator=(const MaterialSlot& other)
    {
        if (this != &other) {
            visible = other.visible;
            runtimeTextureOverrides.clear();
            m_effectiveTemplates = {};
            m_authoring = other.m_authoring;
            m_referenceRevision = m_referenceRevision == (std::numeric_limits<uint64_t>::max)() ? 1u : m_referenceRevision + 1u;
            m_resolvedAsset = other.GetMaterialAsset();
            m_resolvedReferenceRevision = m_referenceRevision;
            propertyNameCache = other.propertyNameCache;
            propertyValidationCache.clear(); propertyValidationDescriptor = nullptr;
            material = other.material ? std::make_unique<renderer::Material>(other.material->CloneWithoutGpuResources()) : nullptr;
        }
        return *this;
    }
    MaterialSlot(MaterialSlot&& other) noexcept
        : visible(other.visible), material(std::move(other.material)), runtimeTextureOverrides(std::move(other.runtimeTextureOverrides)), propertyNameCache(std::move(other.propertyNameCache)),
          propertyValidationCache(std::move(other.propertyValidationCache)),
          propertyValidationDescriptor(std::exchange(other.propertyValidationDescriptor, nullptr)),
          m_authoring(std::move(other.m_authoring)),
          m_resolvedAsset(std::exchange(other.m_resolvedAsset, {})),
          m_referenceRevision(std::exchange(other.m_referenceRevision, 1u)),
          m_resolvedReferenceRevision(std::exchange(other.m_resolvedReferenceRevision, 0u)),
          m_effectiveTemplates(std::move(other.m_effectiveTemplates)) {}
    MaterialSlot& operator=(MaterialSlot&& other) noexcept
    {
        if (this != &other) {
            visible = other.visible;
            material = std::move(other.material);
            runtimeTextureOverrides = std::move(other.runtimeTextureOverrides);
            propertyNameCache = std::move(other.propertyNameCache);
            propertyValidationCache = std::move(other.propertyValidationCache);
            propertyValidationDescriptor = std::exchange(other.propertyValidationDescriptor, nullptr);
            m_authoring = std::move(other.m_authoring);
            m_resolvedAsset = std::exchange(other.m_resolvedAsset, {});
            m_referenceRevision = std::exchange(other.m_referenceRevision, 1u);
            m_resolvedReferenceRevision = std::exchange(other.m_resolvedReferenceRevision, 0u);
            m_effectiveTemplates = std::move(other.m_effectiveTemplates);
        }
        return *this;
    }

    [[nodiscard]] AuthoringSnapshotPtr ReadAuthoring() const { return m_authoring.Read(); }
    /// @pre Retain ReadAuthoring() when the reference must survive a mutation or callback.
    [[nodiscard]] const MaterialSlotAuthoring& Authoring() const
    {
        const auto* snapshot = m_authoring.View();
        static const MaterialSlotAuthoring empty;
        return snapshot ? snapshot->Data() : empty;
    }
    [[nodiscard]] AuthoringEdit BeginAuthoringEdit() { return m_authoring.BeginEdit(); }
    [[nodiscard]] const std::string& GetMaterialPath() const { return Authoring().materialPath; }
    [[nodiscard]] bool IsVisible() const { return visible; }
    [[nodiscard]] asset::AssetHandle<asset::MaterialAsset> GetMaterialAsset() const
    {
        const auto explicitAsset = Authoring().materialAsset;
        return explicitAsset.IsValid() ? explicitAsset :
            (m_resolvedReferenceRevision == m_referenceRevision ? m_resolvedAsset : asset::AssetHandle<asset::MaterialAsset>{});
    }

    /// @note Explicit editor/reimport refresh invalidates derived bindings without pretending that authoring values changed.
    /// @pre The owner remains alive for this call; native release is last and no member is accessed after it.
    void InvalidateResolvedMaterialReference()
    {
        m_resolvedAsset = {}; m_resolvedReferenceRevision = 0;
        m_referenceRevision = m_referenceRevision == (std::numeric_limits<uint64_t>::max)() ? 1u : m_referenceRevision + 1u;
        m_effectiveTemplates = {};
        propertyValidationCache.clear(); propertyValidationDescriptor = nullptr;
        material.reset();
    }

    /// @pre This individual slot remains alive and stationary throughout importer callbacks. Use MaterialComponent's indexed resolver for movable slots.
    bool EnsureMaterialAsset()
    {
        if (GetMaterialAsset().IsValid()) return true;
        const auto snapshot = ReadAuthoring();
        if (!snapshot || snapshot->Data().materialPath.empty()) return false;
        const auto revision = m_referenceRevision;
        const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(snapshot->Data().materialPath);
        return PublishResolvedMaterialAsset(snapshot, revision, handle);
    }

    /// @note A changed path clears explicit/lazy binding and native validation; unchanged paths retain the existing binding.
    void SetMaterialPath(std::string path)
    {
        if (GetMaterialPath() == path) return;
        auto edit = BeginAuthoringEdit();
        edit.Draft().materialPath = std::move(path); edit.Draft().materialAsset = {};
        CommitSetter(edit);
    }
    void SetMaterialReference(std::string path, asset::AssetHandle<asset::MaterialAsset> handle)
    {
        auto edit = BeginAuthoringEdit();
        edit.Draft().materialPath = std::move(path); edit.Draft().materialAsset = handle;
        CommitSetter(edit);
    }
    void SetMaterialAsset(asset::AssetHandle<asset::MaterialAsset> handle)
    {
        auto edit = BeginAuthoringEdit(); edit.Draft().materialAsset = handle; CommitSetter(edit);
    }
    void SetVisible(bool visible)
    {
        this->visible = visible;
    }

    void SetPresentationDefaults(const std::string& path, const math::Vector4& tint,
        const std::string& texture, int64_t sortValue, bool receiveLighting)
    {
        auto edit = BeginAuthoringEdit(); auto& values = edit.Draft();
        if (!values.hasPresentationDefaults && !values.materialPath.empty() && values.materialPath != path)
            values.hasPresentationMaterialOverride = true;
        if (!values.hasPresentationMaterialOverride && values.materialPath != path) {
            values.materialPath = path; values.materialAsset = {};
        }
        values.hasPresentationDefaults = true;
        values.presentationParamDefaults["albedo"] = {tint.x, tint.y, tint.z, tint.w};
        values.presentationParamDefaults["receiveLighting"] = {receiveLighting ? 1.0f : 0.0f};
        if (texture.empty()) values.presentationTextureDefaults.erase("albedo");
        else values.presentationTextureDefaults["albedo"] = texture;
        values.presentationDoubleSided = true;
        values.presentationRenderQueue = static_cast<int32_t>(std::clamp<int64_t>(
            static_cast<int64_t>(renderer::RenderQueue::TRANSPARENT_QUEUE) + sortValue, INT32_MIN, INT32_MAX));
        CommitSetter(edit);
    }

    renderer::BlendMode GetBlendMode() const
    {
        const auto& values = Authoring();
        if (values.hasBlendModeOverride) return values.blendModeOverride;
        const auto* source = asset::AssetManager::Get<asset::MaterialAsset>(GetMaterialAsset());
        return source ? source->Authoring().blendMode : renderer::BlendMode::OPAQUE_BLEND;
    }
    bool IsDoubleSided() const
    {
        const auto& values = Authoring();
        if (values.hasDoubleSidedOverride) return values.doubleSidedOverride;
        if (values.hasPresentationDefaults) return values.presentationDoubleSided;
        const auto* source = asset::AssetManager::Get<asset::MaterialAsset>(GetMaterialAsset());
        return source && source->Authoring().doubleSided;
    }
    int32_t GetDepthBias() const
    {
        const auto* source = asset::AssetManager::Get<asset::MaterialAsset>(GetMaterialAsset());
        return source ? source->Authoring().depthBias : 0;
    }
    float GetDepthBiasSlope() const
    {
        const auto* source = asset::AssetManager::Get<asset::MaterialAsset>(GetMaterialAsset());
        return source ? source->Authoring().depthBiasSlope : 0.0f;
    }
    int32_t GetRenderQueue() const
    {
        const auto& values = Authoring();
        if (values.hasRenderQueueOverride) return values.renderQueueOverride;
        if (values.hasPresentationDefaults) return values.presentationRenderQueue;
        const auto* source = asset::AssetManager::Get<asset::MaterialAsset>(GetMaterialAsset());
        return source ? source->Authoring().renderQueue : renderer::RenderQueue::GEOMETRY;
    }
    const std::string& GetShaderPath() const
    {
        static const std::string empty;
        const auto* source = asset::AssetManager::Get<asset::MaterialAsset>(GetMaterialAsset());
        return source ? source->Authoring().shaderPath : empty;
    }
    asset::MeshType GetMeshType() const
    {
        const auto* source = asset::AssetManager::Get<asset::MaterialAsset>(GetMaterialAsset());
        return source ? source->Authoring().meshType : asset::MeshType::Any;
    }

private:
    /// @note No importer/native callback occurs here; a moved owner can publish only to its freshly reacquired identity.
    bool PublishResolvedMaterialAsset(const AuthoringSnapshotPtr& expected, uint64_t referenceRevision,
        asset::AssetHandle<asset::MaterialAsset> handle)
    {
        const auto current = ReadAuthoring();
        if (!current || current->OwnerToken() != expected->OwnerToken() || referenceRevision != m_referenceRevision) return false;
        m_resolvedAsset = handle; m_resolvedReferenceRevision = referenceRevision;
        return handle.IsValid();
    }
    asset::detail::MaterialAuthoringCommitResult CommitAuthoringEdit(AuthoringEdit& edit)
    {
        const auto previous = ReadAuthoring();
        const auto result = m_authoring.Commit(edit);
        if (result != asset::detail::MaterialAuthoringCommitResult::COMMITTED) return result;
        m_effectiveTemplates = {};
        const auto current = ReadAuthoring();
        if (!previous || previous->Data().materialPath != current->Data().materialPath
            || previous->Data().materialAsset != current->Data().materialAsset) {
            runtimeTextureOverrides.clear();
            InvalidateResolvedMaterialReference();
        }
        return result;
    }
    /// @note Immediate setters run without callbacks on the authoring thread; a conflict violates that owner-thread contract.
    void CommitSetter(AuthoringEdit& edit)
    {
        const auto result = edit.Commit(*this);
        assert(result != asset::detail::MaterialAuthoringCommitResult::CONFLICT);
        (void)result;
    }
    AuthoringState m_authoring;
    asset::AssetHandle<asset::MaterialAsset> m_resolvedAsset;
    uint64_t m_referenceRevision = 1;
    uint64_t m_resolvedReferenceRevision = 0;
    std::array<std::shared_ptr<const material_detail::EffectiveMaterialTemplate>, 2> m_effectiveTemplates;
    friend AuthoringEdit;
    friend class material_detail::EffectiveMaterialCache;
    friend struct MaterialComponent;
};

/// @note Slot zero is the base MaterialSlot; extra slots retain existing submesh fallback semantics.
struct MaterialComponent : MaterialSlot {
public:
    MaterialComponent() = default;
    /// @note Public live flags preserve generic reflection/managed trait behavior; they do not certify a template cache hit.
    bool enabled = true;
    std::vector<MaterialSlot> extraSlots;
    [[nodiscard]] bool IsEnabled() const { return enabled; }
    void SetEnabled(bool enabled) { this->enabled = enabled; }
    [[nodiscard]] size_t SlotCount() const { return 1u + extraSlots.size(); }
    [[nodiscard]] MaterialSlot& RawSlotAt(size_t index)
    {
        if (index == 0 || index > extraSlots.size()) return *this;
        return extraSlots[index - 1u];
    }
    [[nodiscard]] const MaterialSlot& RawSlotAt(size_t index) const
    {
        if (index == 0 || index > extraSlots.size()) return *this;
        return extraSlots[index - 1u];
    }
    [[nodiscard]] MaterialSlot& SlotAt(size_t index)
    {
        MaterialSlot& slot = RawSlotAt(index);
        return !slot.IsVisible() || !slot.GetMaterialPath().empty() ? slot : *this;
    }
    [[nodiscard]] const MaterialSlot& SlotAt(size_t index) const
    {
        const MaterialSlot& slot = RawSlotAt(index);
        return !slot.IsVisible() || !slot.GetMaterialPath().empty() ? slot : *this;
    }
    /// @pre The component remains alive throughout importer callbacks; its slots may move or be replaced.
    bool EnsureMaterialAsset(size_t index = 0) { return EnsureIndexedMaterialAsset(index, false); }
    /// @note Managed explicit-slot and serialization operations preserve RawSlotAt selection.
    bool EnsureRawMaterialAsset(size_t index) { return EnsureIndexedMaterialAsset(index, true); }
    void ResizeSlots(size_t count) { extraSlots.resize(count > 0 ? count - 1u : 0u); }
    void SetOnlyVisibleSlot(int index)
    {
        for (size_t slot = 0; slot < SlotCount(); ++slot)
            RawSlotAt(slot).SetVisible(index < 0 || static_cast<size_t>(index) == slot);
    }
    const char* GetTypeName() const { return "Material"; }
    void Reflect(IReflector& reflector)
    {
        std::string path = GetMaterialPath();
        reflector.Field("enabled", enabled); reflector.Field("materialPath", path);
        SetMaterialPath(std::move(path));
    }

private:
    bool EnsureIndexedMaterialAsset(size_t index, bool raw)
    {
        const auto existing = (raw ? RawSlotAt(index) : SlotAt(index)).GetMaterialAsset();
        if (existing.IsValid()) return true;
        const auto snapshot = (raw ? RawSlotAt(index) : SlotAt(index)).ReadAuthoring();
        if (!snapshot || snapshot->Data().materialPath.empty()) return false;
        const uint64_t revision = (raw ? RawSlotAt(index) : SlotAt(index)).m_referenceRevision;
        const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(snapshot->Data().materialPath);
        /// @note Load may relocate/replace extraSlots; no old slot pointer or reference survives it.
        return (raw ? RawSlotAt(index) : SlotAt(index)).PublishResolvedMaterialAsset(snapshot, revision, handle);
    }
};

} /// @note namespace fbzz::scene
