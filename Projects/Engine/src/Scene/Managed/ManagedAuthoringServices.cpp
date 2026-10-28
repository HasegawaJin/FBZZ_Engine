/// @file    ManagedAuthoringServices.cpp
/// @brief   明示スロット・実 GUID 解決・型検証と別ストアへの原子的な値転送。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include "ManagedAuthoringServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedIntegerParsing.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Core/Application.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Asset/TextureAsset.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Util/SaveStore.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace fbzz::scene::managed_detail {
namespace {
bool Text(std::string_view text) { return !text.empty() && text.find('\0') == std::string_view::npos; }

bool ResolveTypedAsset(std::string_view reference, bool material, std::string& path)
{
    if (!reference.starts_with("guid:") || !Text(reference.substr(5))) return false;
    path = asset::AssetDatabase::PathFromGuid(std::string(reference.substr(5)));
    if (path.empty()) return false;
    std::string extension = util::FileSystem::PathToUtf8(util::FileSystem::PathFromUtf8(path).extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (material) return extension == ".mat" && asset::AssetManager::Load<asset::MaterialAsset>(path).IsValid();
    return extension != ".mat" && asset::AssetManager::Load<asset::TextureAsset>(path).IsValid();
}
bool RegisteredGuid(const std::string& path, std::string& reference)
{
    if (path.empty()) return false;
    const std::string resolved = asset::AssetManager::ResolveAssetPath(path);
    if (resolved.empty()) return false;
    const std::string guid = asset::AssetDatabase::TryGetGuidFromPath(resolved);
    if (guid.empty()) return false;
    reference = "guid:" + guid;
    return true;
}
ManagedBridgeStatus MaterialOperation(Script& proxy, GameObject& target, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> p, std::span<const std::string> t, ManagedServiceResult& out)
{
    if (request.flags > 1 || request.index > 41 || (request.index >= 30 && request.index <= 35)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool read = request.flags == 1;
    const bool expectedRead = request.index == 0 || request.index == 1 || request.index == 3 || request.index == 5
        || (request.index >= 6 && request.index <= 20 && !(request.index & 1U)) || request.index == 25 || request.index == 27
        || request.index == 29 || request.index == 37 || request.index == 39 || request.index == 41;
    if (read != expectedRead) return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto* component = target.GetComponent<MaterialComponent>();
    if (!component || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::MATERIAL))) return ManagedBridgeStatus::NOT_FOUND;
    if (request.index <= 2) {
        if (!p.empty() || !t.empty() || (request.index < 2 ? integer != 0 : integer < 0 || integer > 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 0) out.integer = static_cast<int64_t>(component->SlotCount());
        else if (request.index == 1) out.integer = component->enabled;
        else component->enabled = integer != 0;
        return ManagedBridgeStatus::OK;
    }
    /// @note RawSlotAt と proxy の fallback/auto-resize を許す前に、既存 slot のみと確定する。
    if (integer < 0 || static_cast<uint64_t>(integer) >= component->SlotCount()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const uint32_t slot = static_cast<uint32_t>(integer);
    auto& row = component->RawSlotAt(slot);
    const MaterialInstance instance = proxy.material.Instance(slot);
    if (request.index >= 36) {
        const bool needsShared = read && ((request.index == 37 && !row.Authoring().hasBlendModeOverride)
            || (request.index == 39 && !row.Authoring().hasDoubleSidedOverride && !row.Authoring().hasPresentationDefaults)
            || (request.index == 41 && !row.Authoring().hasRenderQueueOverride && !row.Authoring().hasPresentationDefaults));
        if (needsShared && !row.GetMaterialPath().empty() && !component->EnsureRawMaterialAsset(slot)) return ManagedBridgeStatus::NOT_FOUND;
        auto& currentRow = component->RawSlotAt(slot);
        if (request.index == 40) {
            int queue = 0;
            if (!p.empty() || t.size() != 1 || !ParseInteger(t[0], queue)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            out.integer = instance.SetRenderQueue(queue);
        } else {
            if (!t.empty() || p.size() != (read ? 0U : 1U)) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 36) {
                if (p[0] < 0 || p[0] > 3 || std::trunc(p[0]) != p[0]) return ManagedBridgeStatus::INVALID_ARGUMENT;
                auto edit = currentRow.BeginAuthoringEdit();
                edit.Draft().blendModeOverride = static_cast<renderer::BlendMode>(static_cast<int>(p[0]));
                edit.Draft().hasBlendModeOverride = true;
                out.integer = edit.Commit(currentRow) != asset::detail::MaterialAuthoringCommitResult::CONFLICT;
            } else if (request.index == 38) {
                if (p[0] != 0 && p[0] != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
                out.integer = instance.SetDoubleSided(p[0] != 0);
            } else if (request.index == 37) {
                const auto mode = currentRow.Authoring().hasBlendModeOverride ? currentRow.Authoring().blendModeOverride
                    : currentRow.GetMaterialAsset().IsValid() ? currentRow.GetBlendMode() : renderer::BlendMode::OPAQUE_BLEND;
                if (mode == renderer::BlendMode::OPAQUE_BLEND) out.integer = 0;
                else if (mode == renderer::BlendMode::ALPHA_BLEND) out.integer = 1;
                else if (mode == renderer::BlendMode::ADDITIVE) out.integer = 2;
                else if (mode == renderer::BlendMode::PREMULTIPLIED) out.integer = 3;
                else return ManagedBridgeStatus::UNSUPPORTED;
            } else if (request.index == 39) out.integer = currentRow.IsDoubleSided();
            else out.integer = currentRow.GetRenderQueue();
        }
        return ManagedBridgeStatus::OK;
    }
    if (request.index == 3 || request.index == 4) {
        if (!t.empty() || p.size() != (read ? 0U : 1U) || (!read && p[0] != 0 && p[0] != 1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (read) out.integer = row.visible; else row.visible = p[0] != 0;
        return ManagedBridgeStatus::OK;
    }
    if (request.index == 23 || request.index == 25) {
        if (!p.empty() || !t.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 23) out.integer = instance.ClearAllOverrides();
        else { std::string reference; out.integer = RegisteredGuid(row.GetMaterialPath(), reference); if (out.integer) out.text.push_back(std::move(reference)); }
        return ManagedBridgeStatus::OK;
    }
    if (request.index == 24) {
        if (!p.empty() || t.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
        std::string path;
        if (!t[0].empty() && !ResolveTypedAsset(t[0], true, path)) { out.integer = 0; return ManagedBridgeStatus::OK; }
        MaterialRef reference;
        if (!path.empty()) { reference.reference.guid = t[0].substr(5); reference.reference.path = path; }
        out.integer = proxy.material.SetSharedMaterial(reference, slot);
        return ManagedBridgeStatus::OK;
    }
    const size_t strings = request.index == 9 || request.index == 11 || request.index == 26 ? 2U : 1U;
    const size_t floats = request.index == 28 ? 16U : request.index == 7 || request.index == 13 ? 1U : request.index == 15 ? 2U
        : request.index == 17 ? 3U : request.index == 19 || request.index == 21 ? 4U : 0U;
    if (t.size() != strings || !Text(t[0]) || p.size() != floats) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const MaterialPropertyId property(t[0]);
    switch (request.index) {
    case 5: out.integer = instance.HasProperty(property); break;
    case 6: { float value = 0; out.integer = instance.TryGetFloat(property, value); if (out.integer) out.values = {value}; break; }
    case 7: out.integer = instance.SetFloat(property, p[0]); break;
    case 8: { int value = 0; const bool ok = instance.TryGetInt(property, value); out.values = {ok ? 1.f : 0.f}; out.integer = value; break; }
    case 9: { int value = 0; if (!ParseInteger(t[1], value)) return ManagedBridgeStatus::INVALID_ARGUMENT; out.integer = instance.SetInt(property, value); break; }
    case 10: { uint32_t value = 0; const bool ok = instance.TryGetUInt(property, value); out.values = {ok ? 1.f : 0.f}; out.integer = value; break; }
    case 11: { uint32_t value = 0; if (!ParseInteger(t[1], value)) return ManagedBridgeStatus::INVALID_ARGUMENT; out.integer = instance.SetUInt(property, value); break; }
    case 12: { bool value = false; out.integer = instance.TryGetBool(property, value); if (out.integer) out.values = {value ? 1.f : 0.f}; break; }
    case 13: if (p[0] != 0 && p[0] != 1) return ManagedBridgeStatus::INVALID_ARGUMENT; out.integer = instance.SetBool(property, p[0] != 0); break;
    case 14: { math::Vector2 value; out.integer = instance.TryGetVector2(property, value); if (out.integer) out.values = {value.x, value.y}; break; }
    case 15: out.integer = instance.SetVector2(property, {p[0], p[1]}); break;
    case 16: { math::Vector3 value; out.integer = instance.TryGetVector3(property, value); if (out.integer) out.values = {value.x, value.y, value.z}; break; }
    case 17: out.integer = instance.SetVector3(property, {p[0], p[1], p[2]}); break;
    case 18:
    case 20: { math::Vector4 value; out.integer = request.index == 18 ? instance.TryGetVector4(property, value) : instance.TryGetColor(property, value); if (out.integer) out.values = {value.x, value.y, value.z, value.w}; break; }
    case 19: out.integer = instance.SetVector4(property, {p[0], p[1], p[2], p[3]}); break;
    case 21: out.integer = instance.SetColor(property, {p[0], p[1], p[2], p[3]}); break;
    case 22: out.integer = instance.ClearOverride(property); break;
    case 26: {
        std::string path;
        if (!ResolveTypedAsset(t[1], false, path)) { out.integer = 0; break; }
        TextureRef reference; reference.reference.guid = t[1].substr(5); reference.reference.path = path;
        out.integer = instance.SetTexture(property, reference); break;
    }
    case 27: {
        TextureRef reference; std::string guid;
        out.integer = instance.TryGetTexture(property, reference) && RegisteredGuid(reference.ResolvePath(), guid);
        if (out.integer) out.text.push_back(std::move(guid)); break;
    }
    case 28: {
        math::Matrix4 matrix; for (size_t rowIndex = 0; rowIndex < 4; ++rowIndex) for (size_t column = 0; column < 4; ++column) matrix.m[rowIndex][column] = p[rowIndex * 4 + column];
        out.integer = instance.SetMatrix(property, matrix); break;
    }
    case 29: {
        math::Matrix4 matrix; out.integer = instance.TryGetMatrix(property, matrix);
        if (out.integer) for (const auto& rowValues : matrix.m) for (float value : rowValues) out.values.push_back(value);
        break;
    }
    default: return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus StoreOperation(const ManagedBridgeRequest& request, int64_t integer,
    std::span<const float> p, std::span<const std::string> t, ManagedServiceResult& out)
{
    if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
    if (request.flags > 1 || (request.index > 21 && request.index != 27)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool read = request.flags == 1;
    const bool expectedRead = request.index == 0 || request.index == 2 || request.index == 3
        || (request.index >= 8 && request.index <= 20 && !(request.index & 1U)) || request.index == 27;
    if (read != expectedRead) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const size_t strings = request.index == 1 ? 1U : request.index >= 3 && request.index <= 4 ? 1U
        : request.index >= 8 ? request.index == 15 ? 2U : 1U : 0U;
    const size_t floats = request.index == 27 ? 0U : request.index >= 12 && request.index <= 13 ? 1U : request.index >= 16 && request.index <= 17 ? 2U
        : request.index >= 18 && request.index <= 19 ? 3U : request.index >= 20 ? 4U : 0U;
    if (t.size() != strings || p.size() != floats || (strings && !Text(t[0]))) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (request.index == 8 || request.index == 9) { if (integer < 0 || integer > 1) return ManagedBridgeStatus::INVALID_ARGUMENT; }
    else if (request.index == 10 || request.index == 11) { if (integer < INT32_MIN || integer > INT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT; }
    else if (integer) return ManagedBridgeStatus::INVALID_ARGUMENT;
    auto& app = core::Application::Get();
    auto& store = request.op == ManagedBridgeOp::SERVICE_CONFIG ? app.GetConfigStore() : app.GetSaveStore();
    switch (request.index) {
    case 0: out.text = {store.GetPath()}; break;
    case 1: store.SetPath(t[0]); break;
    case 2: out.integer = store.IsDirty(); break;
    case 3: out.integer = store.Has(t[0]); break;
    case 4: store.Remove(t[0]); break;
    case 5: store.Clear(); break;
    case 6: out.integer = store.Save(); break;
    case 7: out.integer = store.Load(); break;
    case 8: out.integer = store.GetBool(t[0], integer != 0); break;
    case 9: store.SetBool(t[0], integer != 0); break;
    case 10: out.integer = store.GetInt(t[0], static_cast<int>(integer)); break;
    case 11: store.SetInt(t[0], static_cast<int>(integer)); break;
    case 12: out.values = {store.GetFloat(t[0], p[0])}; break;
    case 13: store.SetFloat(t[0], p[0]); break;
    case 14:
        out.hasBorrowedText = store.TryGetStringView(t[0], out.borrowedText);
        out.integer = out.hasBorrowedText;
        break;
    case 15: store.SetString(t[0], t[1]); break;
    case 16: { const auto value = store.GetVector2(t[0], {p[0], p[1]}); out.values = {value.x, value.y}; break; }
    case 17: store.SetVector2(t[0], {p[0], p[1]}); break;
    case 18: { const auto value = store.GetVector3(t[0], {p[0], p[1], p[2]}); out.values = {value.x, value.y, value.z}; break; }
    case 19: store.SetVector3(t[0], {p[0], p[1], p[2]}); break;
    case 20: { const auto value = store.GetVector4(t[0], {p[0], p[1], p[2], p[3]}); out.values = {value.x, value.y, value.z, value.w}; break; }
    case 21: store.SetVector4(t[0], {p[0], p[1], p[2], p[3]}); break;
    case 27: {
        std::string_view type, schema;
        out.hasBorrowedText = store.TryGetDocumentView(t[0], type, schema, out.borrowedText); out.integer = out.hasBorrowedText;
        if (out.hasBorrowedText) out.text = {std::string(type), std::string(schema)};
        break;
    }
    }
    return ManagedBridgeStatus::OK;
}
} /// @note namespace
ManagedBridgeStatus RunManagedAuthoringService(Scene&, Script& proxy, GameObject& target,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result)
{
    ManagedServiceResult output;
    const auto status = request.op == ManagedBridgeOp::SERVICE_MATERIAL
        ? MaterialOperation(proxy, target, request, integer, values, text, output)
        : StoreOperation(request, integer, values, text, output);
    if (status == ManagedBridgeStatus::OK) result = std::move(output);
    return status;
}
} /// @note namespace fbzz::scene::managed_detail
