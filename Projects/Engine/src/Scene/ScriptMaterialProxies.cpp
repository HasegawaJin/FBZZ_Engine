/// @file    ScriptMaterialProxies.cpp
/// @brief   Material 領域の Script 転送と関連する内部ヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Script.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/EntityRef.hpp>
#include <limits>
#include <type_traits>
#include <string_view>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <Engine/Asset/MaterialParamBinding.hpp>
#include <Engine/Core/Logger.hpp>
namespace fbzz::scene {
namespace {
uint32_t ParseTextureSlot(std::string_view slot)
{
    if (slot.size() < 2 || slot[0] != 't') return UINT32_MAX;

    uint32_t value = 0;
    for (size_t i = 1; i < slot.size(); ++i) {
        if (slot[i] < '0' || slot[i] > '9') return UINT32_MAX;
        value = value * 10u + static_cast<uint32_t>(slot[i] - '0');
    }
    return value < 16u ? value : UINT32_MAX;
}


std::string_view CanonicalTextureProperty(std::string_view property)
{
    static constexpr std::string_view slots[] = {
        "albedo", "normal", "metallic", "emissive", "ao", "tex5", "tex6", "tex7"
    };
    const uint32_t slot = ParseTextureSlot(property);
    return slot < std::size(slots) ? slots[slot] : property;
}


std::string_view CanonicalMaterialProperty(std::string_view property)
{
    return property == "grassShading" ? std::string_view{"planarShading"} : property;
}

/// @note 新旧 Fiber キーを同じ型検査へ接続し、旧シェーダーの反射名も受け付ける。

const renderer::ShaderVarDesc* FindCompatibleMaterialVariable(
    const renderer::ShaderDescriptor& descriptor, std::string_view property)
{
    const auto canonical = CanonicalMaterialProperty(property);
    if (const auto* variable = descriptor.FindVar(canonical)) return variable;
    return canonical == "planarShading" ? descriptor.FindVar("grassShading") : nullptr;
}


template<typename T>
void NormalizeMaterialShadingOverride(std::unordered_map<std::string, std::vector<T>>& overrides)
{
    auto legacy = overrides.extract("grassShading");
    if (!legacy.empty() && !overrides.contains("planarShading")) {
        legacy.key() = "planarShading";
        overrides.insert(std::move(legacy));
    }
}




/// @brief MaterialRef から共有アセットを解決する。未ロードならここでロードする。
asset::MaterialAsset::AuthoringSnapshotPtr ResolveSharedMaterial(const MaterialRef& material)
{
    const std::string path = material.ResolvePath();
    if (path.empty()) return {};
    const auto handle = asset::AssetManager::Load<asset::MaterialAsset>(path);
    if (!handle.IsValid()) return {};
    return asset::ReadMaterialAuthoring(handle);
}


} /// @note namespace
MaterialComponent* MaterialInstance::ResolveOwner(bool ensure) const
{
    if (!m_script || !m_script->m_scene) return nullptr;
    GameObject* object = m_target.IsValid()
        ? m_target.Resolve(*m_script->m_scene)
        : m_script->m_gameObject;
    if (!object || !object->IsValid()) return nullptr;
    if (auto* material = object->GetComponent<MaterialComponent>())
        return material;
    return ensure ? &object->AddComponent<MaterialComponent>() : nullptr;
}

/// @brief m_slot が指す MaterialSlot を解決する。ensure=true なら必要な数までスロットを伸ばす。
/// @note SkinnedMeshRenderer は 1 GameObject でモデル全体を描くため、submesh ごとのマテリアルは
/// @note 同じ GameObject 上のスロットとして並ぶ (例: «バイザーだけ光らせる» をスロット番号で行える)。

void* MaterialInstance::ResolveComponent(bool ensure) const
{
    MaterialComponent* owner = ResolveOwner(ensure);
    if (!owner) return nullptr;
    if (m_slot >= owner->SlotCount()) {
        if (!ensure) return nullptr;
        owner->ResizeSlots(static_cast<size_t>(m_slot) + 1u);
    }
    return &owner->RawSlotAt(static_cast<size_t>(m_slot));
}


bool MaterialInstance::IsValid() const
{
    return ResolveComponent(false) != nullptr;
}


bool MaterialInstance::HasProperty(MaterialPropertyId property) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    auto* owner = ResolveOwner(false);
    if (!owner || !owner->EnsureRawMaterialAsset(m_slot)) return false;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    const auto assetSnapshot = asset::ReadMaterialAuthoring(material->GetMaterialAsset());
    const auto* asset = assetSnapshot ? &assetSnapshot->Data() : nullptr;
    if (!asset) return false;
    if (asset->integerParams.contains(std::string(CanonicalMaterialProperty(property.name))) || asset::FindMaterialParam(*asset, property.name) ||
        asset->textures.contains(std::string(property.name)))
        return true;
    if (m_script) {
        const auto* descriptor = m_script->GetShaderDescriptor(material->GetShaderPath());
        if (descriptor && FindCompatibleMaterialVariable(*descriptor, property.name))
            return true;
        if (descriptor) {
            for (const auto& texture : descriptor->textures)
                if (texture.name == property.name) return true;
        }
    }
    return ParseTextureSlot(property.name) < 5u;
}


const std::string& MaterialInstance::ResolvePropertyName(
    void* component,
    MaterialPropertyId property) const
{
    auto& material = *static_cast<MaterialSlot*>(component);
    const auto name = CanonicalMaterialProperty(property.name);
    if (name == "planarShading") {
        const auto snapshot = material.ReadAuthoring();
        if (snapshot && (snapshot->Data().paramOverrides.contains("grassShading")
            || snapshot->Data().integerParamOverrides.contains("grassShading"))) {
            auto edit = material.BeginAuthoringEdit(); auto& values = edit.Draft();
            if (values.paramOverrides.contains("planarShading") || values.integerParamOverrides.contains("planarShading")) {
                values.paramOverrides.erase("grassShading"); values.integerParamOverrides.erase("grassShading");
            } else {
                NormalizeMaterialShadingOverride(values.paramOverrides);
                if (values.paramOverrides.contains("planarShading")) values.integerParamOverrides.erase("grassShading");
                else NormalizeMaterialShadingOverride(values.integerParamOverrides);
            }
            if (edit.Commit(material) == asset::detail::MaterialAuthoringCommitResult::CONFLICT) {
                static const std::string unavailable;
                FBZZ_LOG_WARN("Material legacy property normalization conflicted with a newer edit.");
                return unavailable;
            }
        }
    }
    auto [it, inserted] = material.propertyNameCache.try_emplace(property.hash, std::string(name));
    if (!inserted && it->second != name) it->second.assign(name);
    return it->second;
}


bool MaterialInstance::ValidateProperty(MaterialPropertyId property, PropertyKind kind) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    auto* owner = ResolveOwner(false);
    if (!owner || !owner->EnsureRawMaterialAsset(m_slot)) return false;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    const auto ownerSnapshot = material->ReadAuthoring();
    const std::string shaderPath = material->GetShaderPath();
    const auto* descriptor = m_script ? m_script->GetShaderDescriptor(shaderPath) : nullptr;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || material->ReadAuthoring() != ownerSnapshot) return false;
    if (!descriptor) {
        FBZZ_LOG_WARN("Material reflection unavailable: %s", shaderPath.c_str());
        return false;
    }
    if (kind == PropertyKind::TEXTURE) {
        const auto canonicalName = CanonicalTextureProperty(property.name);
        for (const auto& texture : descriptor->textures)
            if (texture.name == property.name || texture.name == canonicalName
                || texture.slot == ParseTextureSlot(property.name)) return true;
        return false;
    }
    const auto* variable = FindCompatibleMaterialVariable(*descriptor, property.name);
    bool valid = variable && variable->IsWritable() && !variable->elements;
    if (valid) {
        using Type = renderer::ShaderVarType;
        switch (kind) {
        case PropertyKind::FLOAT: valid = variable->varType == Type::Float && variable->varClass == renderer::ShaderVarClass::Scalar; break;
        case PropertyKind::INT: valid = variable->varType == Type::Int && variable->varClass == renderer::ShaderVarClass::Scalar; break;
        case PropertyKind::UINT: valid = variable->varType == Type::UInt && variable->varClass == renderer::ShaderVarClass::Scalar; break;
        case PropertyKind::BOOL: valid = variable->varType == Type::Bool && variable->varClass == renderer::ShaderVarClass::Scalar; break;
        case PropertyKind::VECTOR2:
        case PropertyKind::VECTOR3:
        case PropertyKind::VECTOR4:
            valid = variable->varType == Type::Float && variable->varClass == renderer::ShaderVarClass::Vector
                && variable->columns == (kind == PropertyKind::VECTOR2 ? 2 : kind == PropertyKind::VECTOR3 ? 3 : 4);
            break;
        case PropertyKind::MATRIX:
            valid = variable->varType == Type::Float && variable->varClass == renderer::ShaderVarClass::Matrix
                && variable->rows == 4 && variable->columns == 4;
            break;
        default: valid = false; break;
        }
    }
    if (!valid) FBZZ_LOG_WARN("Material property missing, unsupported or type mismatch: %.*s",
        static_cast<int>(property.name.size()), property.name.data());
    return valid;
}



bool MaterialInstance::SetValues(MaterialPropertyId property, std::span<const double> values) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    auto* owner = ResolveOwner(false);
    if (!owner || !owner->EnsureRawMaterialAsset(m_slot)) return false;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    const auto ownerSnapshot = material->ReadAuthoring();
    const std::string shaderPath = material->GetShaderPath();
    const auto* descriptor = m_script ? m_script->GetShaderDescriptor(shaderPath) : nullptr;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || material->ReadAuthoring() != ownerSnapshot) return false;
    const auto* variable = descriptor ? FindCompatibleMaterialVariable(*descriptor, property.name) : nullptr;
    if (!variable || !asset::ValidateMaterialValues(*variable, values)) {
        FBZZ_LOG_WARN("Material property rejected (reflection/type/count/range): %.*s",
            static_cast<int>(property.name.size()), property.name.data());
        return false;
    }
    const std::string key = ResolvePropertyName(material, property);
    if (key.empty()) return false;
    auto edit = material->BeginAuthoringEdit(); auto& draft = edit.Draft();
    if (variable->varType == renderer::ShaderVarType::Float) {
        auto& destination = draft.paramOverrides[key]; destination.resize(values.size());
        std::transform(values.begin(), values.end(), destination.begin(), [](double value) { return static_cast<float>(value); });
        draft.integerParamOverrides.erase(key);
    } else {
        auto& destination = draft.integerParamOverrides[key]; destination.clear(); destination.reserve(values.size());
        for (const double value : values) destination.push_back(static_cast<int64_t>(value));
        draft.paramOverrides.erase(key);
    }
    return edit.Commit(*material) != asset::detail::MaterialAuthoringCommitResult::CONFLICT;
}


bool MaterialInstance::TryGetValueCount(MaterialPropertyId property, size_t& count) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    auto* owner = ResolveOwner(false);
    if (!owner || !owner->EnsureRawMaterialAsset(m_slot)) return false;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    const auto ownerSnapshot = material->ReadAuthoring();
    const std::string shaderPath = material->GetShaderPath();
    const auto* descriptor = m_script ? m_script->GetShaderDescriptor(shaderPath) : nullptr;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || material->ReadAuthoring() != ownerSnapshot) return false;
    const auto* variable = descriptor ? FindCompatibleMaterialVariable(*descriptor, property.name) : nullptr;
    if (!variable) return false;
    const auto& key = ResolvePropertyName(material, property);
    const auto validate = [&](const auto& stored) {
        using Value = typename std::decay_t<decltype(stored)>::value_type;
        if (!asset::ValidateMaterialValues(*variable, std::span<const Value>(stored))) return false;
        count = stored.size(); return true;
    };
    if (const auto it = material->Authoring().integerParamOverrides.find(key); it != material->Authoring().integerParamOverrides.end()) return validate(it->second);
    if (const auto it = material->Authoring().paramOverrides.find(key); it != material->Authoring().paramOverrides.end()) return validate(it->second);
    if (const auto it = material->Authoring().presentationParamDefaults.find(key); it != material->Authoring().presentationParamDefaults.end()) return validate(it->second);
    if (const auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(material->GetMaterialAsset())) {
        if (const auto it = shared->Authoring().integerParams.find(key); it != shared->Authoring().integerParams.end()) return validate(it->second);
        if (const auto it = shared->Authoring().params.find(key); it != shared->Authoring().params.end()) return validate(it->second);
    }
    return false;
}


bool MaterialInstance::TryGetValues(MaterialPropertyId property, std::vector<double>& values) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    auto* owner = ResolveOwner(false);
    if (!owner || !owner->EnsureRawMaterialAsset(m_slot)) return false;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    const auto ownerSnapshot = material->ReadAuthoring();
    const std::string shaderPath = material->GetShaderPath();
    const auto* descriptor = m_script ? m_script->GetShaderDescriptor(shaderPath) : nullptr;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || material->ReadAuthoring() != ownerSnapshot) return false;
    const auto* variable = descriptor ? FindCompatibleMaterialVariable(*descriptor, property.name) : nullptr;
    if (!variable) return false;
    const auto& key = ResolvePropertyName(material, property);
    std::vector<double> result;
    if (const auto it = material->Authoring().integerParamOverrides.find(key); it != material->Authoring().integerParamOverrides.end())
        for (const auto value : it->second) result.push_back(static_cast<double>(value));
    else if (const auto it = material->Authoring().paramOverrides.find(key); it != material->Authoring().paramOverrides.end())
        for (const auto value : it->second) result.push_back(static_cast<double>(value));
    else if (const auto it = material->Authoring().presentationParamDefaults.find(key); it != material->Authoring().presentationParamDefaults.end())
        for (const auto value : it->second) result.push_back(static_cast<double>(value));
    else if (const auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(material->GetMaterialAsset())) {
        if (const auto it = shared->Authoring().integerParams.find(key); it != shared->Authoring().integerParams.end())
            for (const auto value : it->second) result.push_back(static_cast<double>(value));
        else if (const auto it = shared->Authoring().params.find(key); it != shared->Authoring().params.end())
            for (const auto value : it->second) result.push_back(static_cast<double>(value));
    }
    if (!asset::ValidateMaterialValues(*variable, result)) return false;
    values = std::move(result);
    return true;
}


bool MaterialInstance::SetFloat(MaterialPropertyId property, float value) const
{
    if (!ValidateProperty(property, PropertyKind::FLOAT)) return false;
    const double values[] = { value };
    return SetValues(property, values);
}


bool MaterialInstance::SetInt(MaterialPropertyId property, int value) const
{
    if (!ValidateProperty(property, PropertyKind::INT)) return false;
    const double values[] = { static_cast<double>(value) };
    return SetValues(property, values);
}


bool MaterialInstance::SetVector3(MaterialPropertyId property, const math::Vector3& value) const
{
    if (!ValidateProperty(property, PropertyKind::VECTOR3)) return false;
    const double values[] = { value.x, value.y, value.z };
    return SetValues(property, values);
}


bool MaterialInstance::SetVector4(MaterialPropertyId property, const math::Vector4& value) const
{
    if (!ValidateProperty(property, PropertyKind::VECTOR4)) return false;
    const double values[] = { value.x, value.y, value.z, value.w };
    return SetValues(property, values);
}


bool MaterialInstance::SetUInt(MaterialPropertyId property, uint32_t value) const
{
    if (!ValidateProperty(property, PropertyKind::UINT)) return false;
    const double values[] = { static_cast<double>(value) };
    return SetValues(property, values);
}


bool MaterialInstance::TryGetUInt(MaterialPropertyId property, uint32_t& value) const
{
    if (!ValidateProperty(property, PropertyKind::UINT)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    value = static_cast<uint32_t>(values[0]);
    return true;
}


bool MaterialInstance::SetBool(MaterialPropertyId property, bool value) const
{
    if (!ValidateProperty(property, PropertyKind::BOOL)) return false;
    const double values[] = { value ? 1.0 : 0.0 };
    return SetValues(property, values);
}


bool MaterialInstance::TryGetBool(MaterialPropertyId property, bool& value) const
{
    if (!ValidateProperty(property, PropertyKind::BOOL)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    value = values[0] != 0;
    return true;
}


bool MaterialInstance::SetVector2(MaterialPropertyId property, const math::Vector2& value) const
{
    if (!ValidateProperty(property, PropertyKind::VECTOR2)) return false;
    const double values[] = { value.x, value.y };
    return SetValues(property, values);
}


bool MaterialInstance::TryGetVector2(MaterialPropertyId property, math::Vector2& value) const
{
    if (!ValidateProperty(property, PropertyKind::VECTOR2)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    value = {static_cast<float>(values[0]), static_cast<float>(values[1])};
    return true;
}


bool MaterialInstance::SetMatrix(MaterialPropertyId property, const math::Matrix4& value) const
{
    if (!ValidateProperty(property, PropertyKind::MATRIX)) return false;
    double values[16];
    for (size_t i = 0; i < 16; ++i) values[i] = value.m[i / 4][i % 4];
    return SetValues(property, values);
}


bool MaterialInstance::TryGetMatrix(MaterialPropertyId property, math::Matrix4& value) const
{
    if (!ValidateProperty(property, PropertyKind::MATRIX)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    for (size_t i = 0; i < 16; ++i) value.m[i / 4][i % 4] = static_cast<float>(values[i]);
    return true;
}


bool ScriptMaterialProxy::SetUInt(std::string_view param, uint32_t value) const
{
    return Instance().SetUInt(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetBool(std::string_view param, bool value) const
{
    return Instance().SetBool(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetVector2(std::string_view param, const math::Vector2& value) const
{
    return Instance().SetVector2(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetMatrix(std::string_view param, const math::Matrix4& value) const
{
    return Instance().SetMatrix(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetValues(std::string_view param, std::span<const double> value) const
{
    return Instance().SetValues(MaterialPropertyId(param), value);
}


bool MaterialInstance::SetColor(MaterialPropertyId property, const math::Vector4& value) const
{
    return SetVector4(property, value);
}


bool MaterialInstance::SetTexture(MaterialPropertyId property, const TextureRef& texture) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !ValidateProperty(property, PropertyKind::TEXTURE)) {
        FBZZ_LOG_WARN("Material texture property not found or type mismatch: %.*s",
            static_cast<int>(property.name.size()), property.name.data());
        return false;
    }
    const std::string key = ResolvePropertyName(material, MaterialPropertyId(CanonicalTextureProperty(property.name)));
    if (key.empty()) return false;
    const auto path = texture.ResolvePath();
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    auto edit = material->BeginAuthoringEdit(); edit.Draft().textureOverrides[key] = path;
    if (edit.Commit(*material) == asset::detail::MaterialAuthoringCommitResult::CONFLICT) return false;
    std::erase_if(material->runtimeTextureOverrides,
        [&](const auto& entry) { return entry.first == key || entry.second.propertyName == property.name; });
    return true;
}


bool MaterialInstance::SetRuntimeTexture(MaterialPropertyId property, renderer::ResourceHandle<renderer::TextureTag> texture,
    uint64_t resourceOwner, uint64_t resetVersion) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    auto* context = m_script ? m_script->m_scene : nullptr;
    auto* resources = context ? context->GetResourceManager() : nullptr;
    const auto available = [&] { return resources && resources == context->GetResourceManager()
        && resources->GetInstanceId() == resourceOwner && resources->GetResetVersion() == resetVersion && resources->Get(texture); };
    if (!material || !property.IsValid() || !texture.IsValid() || !available()) return false;
    auto* owner = ResolveOwner(false);
    if (!owner || !owner->EnsureRawMaterialAsset(m_slot)) return false;
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !available()) return false;
    const auto ownerSnapshot = material->ReadAuthoring();
    const std::string_view canonical = CanonicalTextureProperty(property.name);
    const auto* descriptor = m_script ? m_script->GetShaderDescriptor(material->GetShaderPath()) : nullptr;
    if (!descriptor) return false;
    uint32_t textureSlot = UINT32_MAX;
    std::string reflectedName;
    for (const auto& reflected : descriptor->textures) {
        if (reflected.slot >= asset::kMaterialTextureSlotNames.size()) continue;
        if (reflected.name == property.name || reflected.name == canonical || reflected.slot == ParseTextureSlot(property.name)
            || property.name == asset::kMaterialTextureSlotNames[reflected.slot]) {
            textureSlot = reflected.slot; reflectedName = reflected.name; break;
        }
    }
    if (textureSlot >= asset::kMaterialTextureSlotNames.size()) return false;
    const std::string key = asset::kMaterialTextureSlotNames[textureSlot];
    material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || material->ReadAuthoring() != ownerSnapshot || !available()) return false;
    material->runtimeTextureOverrides[key] = {texture, resourceOwner, resetVersion, std::move(reflectedName)};
    return true;
}


bool MaterialInstance::TryGetFloat(MaterialPropertyId property, float& value) const
{
    if (!ValidateProperty(property, PropertyKind::FLOAT)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    value = static_cast<float>(values[0]);
    return true;
}


bool MaterialInstance::TryGetInt(MaterialPropertyId property, int& value) const
{
    if (!ValidateProperty(property, PropertyKind::INT)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    value = static_cast<int>(values[0]);
    return true;
}


bool MaterialInstance::TryGetVector3(MaterialPropertyId property, math::Vector3& value) const
{
    if (!ValidateProperty(property, PropertyKind::VECTOR3)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    value = {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])};
    return true;
}


bool MaterialInstance::TryGetVector4(MaterialPropertyId property, math::Vector4& value) const
{
    if (!ValidateProperty(property, PropertyKind::VECTOR4)) return false;
    std::vector<double> values;
    if (!TryGetValues(property, values)) return false;
    value = {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2]), static_cast<float>(values[3])};
    return true;
}


bool MaterialInstance::TryGetColor(
    MaterialPropertyId property,
    math::Vector4& value) const
{
    return TryGetVector4(property, value);
}


bool MaterialInstance::TryGetTexture(MaterialPropertyId property, TextureRef& texture) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material || !property.IsValid()) return false;
    const MaterialPropertyId canonical(CanonicalTextureProperty(property.name));
    const std::string& key = ResolvePropertyName(material, canonical);
    const auto overrideIt = material->Authoring().textureOverrides.find(key);
    if (overrideIt != material->Authoring().textureOverrides.end()) {
        texture.SetPath(overrideIt->second);
        return true;
    }
    if (const auto it = material->Authoring().presentationTextureDefaults.find(key); it != material->Authoring().presentationTextureDefaults.end()) {
        texture.SetPath(it->second);
        return true;
    }
    const auto* shared = asset::AssetManager::Get<asset::MaterialAsset>(material->GetMaterialAsset());
    if (!shared) return false;
    const auto it = shared->Authoring().textures.find(key);
    if (it == shared->Authoring().textures.end()) return false;
    texture.SetPath(it->second);
    return true;
}


bool MaterialInstance::ClearOverride(MaterialPropertyId property) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    const std::string key = ResolvePropertyName(material, property);
    const std::string textureKey = ResolvePropertyName(material, MaterialPropertyId(CanonicalTextureProperty(property.name)));
    if (key.empty() || textureKey.empty()) return false;
    auto edit = material->BeginAuthoringEdit(); auto& values = edit.Draft();
    const size_t removed = values.paramOverrides.erase(key) + values.integerParamOverrides.erase(key) + values.textureOverrides.erase(textureKey);
    const bool runtime = std::any_of(material->runtimeTextureOverrides.begin(), material->runtimeTextureOverrides.end(),
        [&](const auto& entry) { return entry.first == textureKey || entry.second.propertyName == property.name; });
    if (!removed && !runtime) return false;
    if (edit.Commit(*material) == asset::detail::MaterialAuthoringCommitResult::CONFLICT) return false;
    std::erase_if(material->runtimeTextureOverrides,
        [&](const auto& entry) { return entry.first == textureKey || entry.second.propertyName == property.name; });
    return true;
}


bool MaterialInstance::ClearAllOverrides() const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false));
    if (!material) return false;
    auto edit = material->BeginAuthoringEdit(); auto& values = edit.Draft();
    values.paramOverrides.clear(); values.integerParamOverrides.clear(); values.textureOverrides.clear();
    values.hasBlendModeOverride = false; values.hasDoubleSidedOverride = false; values.hasRenderQueueOverride = false;
    if (edit.Commit(*material) == asset::detail::MaterialAuthoringCommitResult::CONFLICT) return false;
    material->runtimeTextureOverrides.clear();
    return true;
}


bool MaterialInstance::SetBlendMode(MaterialBlendMode blendMode) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false)); if (!material) return false;
    auto edit = material->BeginAuthoringEdit(); auto& values = edit.Draft();
    switch (blendMode) {
    case MaterialBlendMode::Alpha: values.blendModeOverride = renderer::BlendMode::ALPHA_BLEND; break;
    case MaterialBlendMode::Additive: values.blendModeOverride = renderer::BlendMode::ADDITIVE; break;
    default: values.blendModeOverride = renderer::BlendMode::OPAQUE_BLEND; break;
    }
    values.hasBlendModeOverride = true;
    return edit.Commit(*material) != asset::detail::MaterialAuthoringCommitResult::CONFLICT;
}


bool MaterialInstance::SetDoubleSided(bool doubleSided) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false)); if (!material) return false;
    auto edit = material->BeginAuthoringEdit(); edit.Draft().doubleSidedOverride = doubleSided; edit.Draft().hasDoubleSidedOverride = true;
    return edit.Commit(*material) != asset::detail::MaterialAuthoringCommitResult::CONFLICT;
}


bool MaterialInstance::SetRenderQueue(int32_t renderQueue) const
{
    auto* material = static_cast<MaterialSlot*>(ResolveComponent(false)); if (!material) return false;
    auto edit = material->BeginAuthoringEdit(); edit.Draft().renderQueueOverride = renderQueue; edit.Draft().hasRenderQueueOverride = true;
    return edit.Commit(*material) != asset::detail::MaterialAuthoringCommitResult::CONFLICT;
}


MaterialInstance ScriptMaterialProxy::Instance(uint32_t slot) const
{
    return { script, {}, slot };
}


MaterialInstance ScriptMaterialProxy::Instance(EntityRef target, uint32_t slot) const
{
    return { script, target, slot };
}


bool ScriptMaterialProxy::SetSharedMaterial(const MaterialRef& material, uint32_t slot) const
{
    return SetSharedMaterial({}, material, slot);
}


bool ScriptMaterialProxy::SetSharedMaterial(EntityRef target,
                                            const MaterialRef& materialRef,
                                            uint32_t slot) const
{
    MaterialInstance instance{script, target, slot};
    auto* material = static_cast<MaterialSlot*>(instance.ResolveComponent(true)); if (!material) return false;
    const std::string path = materialRef.ResolvePath();
    const auto handle = path.empty() ? asset::AssetHandle<asset::MaterialAsset>{} : asset::AssetManager::Load<asset::MaterialAsset>(path);
    if (!path.empty() && !handle.IsValid()) {
        FBZZ_LOG_WARN("Shared material could not be loaded: %s", path.c_str()); return false;
    }
    material = static_cast<MaterialSlot*>(instance.ResolveComponent(true)); if (!material) return false;
    auto edit = material->BeginAuthoringEdit(); auto& values = edit.Draft();
    if (values.materialPath != path) {
        values.paramOverrides.clear(); values.integerParamOverrides.clear(); values.textureOverrides.clear();
        values.hasBlendModeOverride = false; values.hasDoubleSidedOverride = false; values.hasRenderQueueOverride = false;
    }
    values.materialPath = path; values.materialAsset = handle; values.hasPresentationMaterialOverride = true;
    const auto result = edit.Commit(*material);
    if (result == asset::detail::MaterialAuthoringCommitResult::CONFLICT) return false;
    material = static_cast<MaterialSlot*>(instance.ResolveComponent(false));
    if (!material) return false;
    material = static_cast<MaterialSlot*>(instance.ResolveComponent(false));
    if (!material) return false;
    material = static_cast<MaterialSlot*>(instance.ResolveComponent(false));
    if (!material) return false;
    material->propertyValidationCache.clear(); material->propertyValidationDescriptor = nullptr;
    return true;
}

/// @name 共有 .mat の読み取り


bool ScriptMaterialProxy::HasSharedProperty(const MaterialRef& material,
                                            MaterialPropertyId property) const
{
    const auto sharedSnapshot = ResolveSharedMaterial(material);
    const auto* shared = sharedSnapshot ? &sharedSnapshot->Data() : nullptr;
    if (!shared || !property.IsValid()) return false;
    const std::string key(CanonicalMaterialProperty(property.name));
    return asset::FindMaterialParam(*shared, property.name) || shared->integerParams.contains(key) || shared->textures.contains(key);
}


bool ScriptMaterialProxy::TryGetSharedFloat(const MaterialRef& material,
                                            MaterialPropertyId property, float& value) const
{
    const auto sharedSnapshot = ResolveSharedMaterial(material);
    const auto* shared = sharedSnapshot ? &sharedSnapshot->Data() : nullptr;
    if (!shared || !property.IsValid()) return false;
    const auto* values = asset::FindMaterialParam(*shared, property.name);
    if (!values || values->empty()) return false;
    value = (*values)[0];
    return true;
}


bool ScriptMaterialProxy::TryGetSharedValues(const MaterialRef& material,
                                             MaterialPropertyId property, std::vector<double>& values) const
{
    const auto sharedSnapshot = ResolveSharedMaterial(material);
    const auto* shared = sharedSnapshot ? &sharedSnapshot->Data() : nullptr;
    if (!shared || !property.IsValid()) return false;
    const std::string key(CanonicalMaterialProperty(property.name));
    std::vector<double> result;
    if (const auto it = shared->integerParams.find(key); it != shared->integerParams.end()) {
        for (const int64_t value : it->second) result.push_back(static_cast<double>(value));
    } else if (const auto* stored = asset::FindMaterialParam(*shared, property.name)) {
        result.assign(stored->begin(), stored->end());
    } else return false;
    values = std::move(result);
    return true;
}


bool ScriptMaterialProxy::TryGetSharedVector4(const MaterialRef& material,
                                              MaterialPropertyId property,
                                              math::Vector4& value) const
{
    const auto sharedSnapshot = ResolveSharedMaterial(material);
    const auto* shared = sharedSnapshot ? &sharedSnapshot->Data() : nullptr;
    if (!shared || !property.IsValid()) return false;
    const auto* stored = asset::FindMaterialParam(*shared, property.name);
    if (!stored || stored->empty()) return false;

    /// @note .mat は float / float2 / float3 / float4 を同じ形式で持つため、
    /// @note 足りない成分は 0 で埋める (Instance() の読み出しと同じ規則)。
    const auto& values = *stored;
    value = {
        values[0],
        values.size() > 1 ? values[1] : 0.0f,
        values.size() > 2 ? values[2] : 0.0f,
        values.size() > 3 ? values[3] : 0.0f
    };
    return true;
}


bool ScriptMaterialProxy::TryGetSharedVector3(const MaterialRef& material,
                                              MaterialPropertyId property,
                                              math::Vector3& value) const
{
    math::Vector4 vector;
    if (!TryGetSharedVector4(material, property, vector)) return false;
    value = { vector.x, vector.y, vector.z };
    return true;
}


bool ScriptMaterialProxy::TryGetSharedColor(const MaterialRef& material,
                                            MaterialPropertyId property,
                                            math::Vector4& value) const
{
    return TryGetSharedVector4(material, property, value);
}


bool ScriptMaterialProxy::TryGetSharedTexture(const MaterialRef& material,
                                              MaterialPropertyId property,
                                              TextureRef& texture) const
{
    const auto sharedSnapshot = ResolveSharedMaterial(material);
    const auto* shared = sharedSnapshot ? &sharedSnapshot->Data() : nullptr;
    if (!shared || !property.IsValid()) return false;
    const MaterialPropertyId canonical(CanonicalTextureProperty(property.name));
    const auto it = shared->textures.find(std::string(canonical.name));
    if (it == shared->textures.end()) return false;
    texture.SetPath(it->second);
    return true;
}


bool ScriptMaterialProxy::EnsureMaterial(std::string_view materialPath) const
{
    auto* material = static_cast<MaterialSlot*>(Instance().ResolveComponent(true));
    if (!material) return false;
    if (material->GetMaterialPath() != materialPath)
        return SetSharedMaterial(MaterialRef(materialPath));
    return material->EnsureMaterialAsset();
}


bool ScriptMaterialProxy::HasParam(std::string_view param) const
{
    return Instance().HasProperty(MaterialPropertyId(param));
}


bool ScriptMaterialProxy::SetFloat(std::string_view param, float value) const
{
    return Instance().SetFloat(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetInt(std::string_view param, int value) const
{
    return Instance().SetInt(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetVector3(std::string_view param, const math::Vector3& value) const
{
    return Instance().SetVector3(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetVector4(std::string_view param, const math::Vector4& value) const
{
    return Instance().SetVector4(MaterialPropertyId(param), value);
}


bool ScriptMaterialProxy::SetTexture(std::string_view slot, std::string_view texturePath) const
{
    return Instance().SetTexture(MaterialPropertyId(slot), TextureRef(texturePath));
}


float ScriptMaterialProxy::GetFloat(std::string_view param) const
{
    float value = 0.0f;
    (void)Instance().TryGetFloat(MaterialPropertyId(param), value);
    return value;
}


int ScriptMaterialProxy::GetInt(std::string_view param) const
{
    int value = 0;
    (void)Instance().TryGetInt(MaterialPropertyId(param), value);
    return value;
}


math::Vector3 ScriptMaterialProxy::GetVector3(std::string_view param) const
{
    math::Vector3 value;
    (void)Instance().TryGetVector3(MaterialPropertyId(param), value);
    return value;
}


math::Vector4 ScriptMaterialProxy::GetVector4(std::string_view param) const
{
    math::Vector4 value;
    (void)Instance().TryGetVector4(MaterialPropertyId(param), value);
    return value;
}


bool ScriptMaterialProxy::SetEnabled(bool enabled) const
{
MaterialComponent* material = Instance().ResolveOwner(false);
    if (!material) return false;
    material->SetEnabled(enabled); return true;
}


bool ScriptMaterialProxy::IsEnabled() const
{
    const MaterialComponent* material = Instance().ResolveOwner(false);
    return material && material->IsEnabled();
}


bool ScriptMaterialProxy::SetSlotVisible(uint32_t slot, bool visible) const
{
    auto* material = static_cast<MaterialSlot*>(Instance(slot).ResolveComponent(true));
    if (!material) return false;
    material->SetVisible(visible); return true;
}


bool ScriptMaterialProxy::IsSlotVisible(uint32_t slot) const
{
    /// @note ensure = false。存在しないスロットを問い合わせただけで作らない。
    const auto* material = static_cast<const MaterialSlot*>(Instance(slot).ResolveComponent(false));
    return material && material->IsVisible();
}


std::string ScriptMaterialProxy::GetSharedMaterialPath(uint32_t slot) const
{
    const auto* material = static_cast<const MaterialSlot*>(Instance(slot).ResolveComponent(false));
    return material ? material->GetMaterialPath() : std::string{};
}


std::string ScriptMaterialProxy::GetSharedMaterialPath(EntityRef target, uint32_t slot) const
{
    const auto* material =
        static_cast<const MaterialSlot*>(Instance(target, slot).ResolveComponent(false));
    return material ? material->GetMaterialPath() : std::string{};
}


uint32_t ScriptMaterialProxy::GetSlotCount() const
{
    const MaterialComponent* material = Instance().ResolveOwner(false);
    /// @note スロット 0 は MaterialComponent 自身が兼ねるので extraSlots + 1。
    return material ? static_cast<uint32_t>(material->extraSlots.size() + 1) : 0u;
}


bool ScriptMaterialProxy::IsDoubleSided() const
{
    const auto* material = static_cast<const MaterialSlot*>(Instance().ResolveComponent(false));
    return material && material->IsDoubleSided();
}


int32_t ScriptMaterialProxy::GetRenderQueue() const
{
    const auto* material = static_cast<const MaterialSlot*>(Instance().ResolveComponent(false));
    return material ? material->GetRenderQueue() : renderer::RenderQueue::GEOMETRY;
}


bool ScriptMaterialProxy::SetOnlyVisibleSlot(int slot) const
{
    MaterialComponent* material = Instance().ResolveOwner(false);
    if (!material) return false;
    material->SetOnlyVisibleSlot(slot);
    return true;
}


bool ScriptMaterialProxy::SetBlendMode(MaterialBlendMode blendMode) const
{
    return Instance().SetBlendMode(blendMode);
}


bool ScriptMaterialProxy::SetDoubleSided(bool doubleSided) const
{
    return Instance().SetDoubleSided(doubleSided);
}


bool ScriptMaterialProxy::SetRenderQueue(int32_t renderQueue) const
{
    return Instance().SetRenderQueue(renderQueue);
}


void ScriptMaterialProxy::QueueRenderPass(UserRenderPassDesc desc) const
{
    if (script) script->QueueRenderPass(std::move(desc));
}


} /// @note namespace fbzz::scene
