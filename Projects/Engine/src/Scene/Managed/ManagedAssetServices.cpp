/// @file    ManagedAssetServices.cpp
/// @brief   GUID 型・要求所有者を検査し、実 async 利用権の状態と解放を扱う。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedAssetServices.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedPacketRead.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/SceneAssetReferences.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetStreaming.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
namespace fbzz::scene::managed_detail {
namespace {
struct TextureRecord {
    uint64_t owner = 0, generation = 0, instance = 0, scene = 0;
    uint64_t resourceOwner = 0, reset = 0;
    EntityID entity = EntityID::INVALID;
    renderer::ResourceHandle<renderer::TextureTag> texture;
    uint32_t width = 0, height = 0, pixelBytes = 0;
};
struct TextureReception {
    TextureRecord owner;
    uint64_t call = 0;
    int64_t texture = 0;
    uint32_t x = 0, y = 0, width = 0, height = 0, rowPitch = 0;
    size_t received = 0;
    std::vector<uint8_t> bytes;
};
bool OwnedTexture(const TextureRecord& record, Scene& scene, Script& caller, const ManagedBridgeRequest& request)
{
    const auto* owner = caller.scene.Self();
    return owner && owner->GetScene() == &scene && record.entity == owner->GetID()
        && record.owner == request.ownerId && record.generation == request.generation
        && record.instance == request.instanceId && record.scene == scene.GetRenderSceneGeneration();
}
renderer::ResourceManager* TextureResources(const TextureRecord& record)
{
    auto* resources = renderer::ResourceManager::FindOwner(record.resourceOwner);
    return resources
        && resources->GetResetVersion() == record.reset ? resources : nullptr;
}
bool TextureAvailable(const TextureRecord& record)
{
    auto* resources = TextureResources(record);
    return resources && resources->Get(record.texture);
}
void ReleaseTexture(const TextureRecord& record)
{
    if (auto* resources = TextureResources(record)) resources->Release(record.texture);
}
template<typename T>
bool TextureInteger(std::string_view text, T& value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
class TextureProxy final : public Script {
public:
    void Bind(Scene& scene, GameObject& target) { m_scene = &scene; m_gameObject = &target; }
};
}
struct ManagedAssetServices::Impl {
    struct Lease { Script* caller; uint64_t owner, generation, instance, scene; EntityID entity; };
    std::unordered_map<uint64_t, Lease> leases;
    std::unordered_map<int64_t, TextureRecord> textures;
    std::unordered_map<int64_t, TextureReception> textureReceptions;
    int64_t nextTexture = 1, nextReception = 1;
    ManagedBridgeStatus RunTexture(Scene& scene, Script& caller, GameObject& target, const ManagedBridgeRequest& request,
        int64_t integer, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result);
};
ManagedAssetServices::ManagedAssetServices() : m_impl(std::make_unique<Impl>()) {}
ManagedAssetServices::~ManagedAssetServices() {
    for (const auto& [token, _] : m_impl->leases) asset::AssetStreamer::Engine().ReleaseLease(token);
    for (const auto& [_, texture] : m_impl->textures) ReleaseTexture(texture);
}

ManagedBridgeStatus ManagedAssetServices::Impl::RunTexture(Scene& scene, Script& caller, GameObject& target,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result)
{
    const bool binding = request.op == ManagedBridgeOp::SERVICE_MATERIAL && request.index == 42;
    if ((!binding && (request.op != ManagedBridgeOp::SERVICE_ASSETS || request.index < 10 || request.index > 16))
        || request.flags != (request.index == 15 ? 1u : 0u)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!Script::IsInPlayMode() || !request.ownerId || !request.instanceId || !request.generation
        || (!binding && caller.scene.Self() != &target)) return ManagedBridgeStatus::INVALID_SCOPE;
    ManagedServiceResult output;
    if (request.index == 10) {
        if (integer || values.size() != 3 || !text.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        for (size_t index = 0; index < 3; ++index)
            if (!std::isfinite(values[index]) || std::trunc(values[index]) != values[index]) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (values[0] < 1 || values[0] > 16384 || values[1] < 1 || values[1] > 16384 || values[2] < 0 || values[2] > 1)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        auto* resources = scene.GetResourceManager();
        if (!resources || nextTexture == (std::numeric_limits<int64_t>::max)()) { result = {}; return ManagedBridgeStatus::OK; }
        const auto format = values[2] == 0 ? renderer::DynamicTextureFormat::R8 : renderer::DynamicTextureFormat::RGBA8;
        const auto texture = resources->CreateDynamicTexture(static_cast<uint32_t>(values[0]), static_cast<uint32_t>(values[1]), format);
        if (texture.IsValid()) {
            const int64_t token = nextTexture++;
            textures.emplace(token, TextureRecord{request.ownerId, request.generation, request.instanceId,
                scene.GetRenderSceneGeneration(), resources->GetInstanceId(), resources->GetResetVersion(), target.GetID(), texture,
                static_cast<uint32_t>(values[0]), static_cast<uint32_t>(values[1]), values[2] == 0 ? 1u : 4u});
            output.integer = token;
        }
        result = std::move(output); return ManagedBridgeStatus::OK;
    }
    if (request.index >= 12 && request.index <= 14) {
        if (request.flags || !request.input || request.inputBytes < 8 || request.inputBytes > kManagedBatchPacketMaxBytes)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        const std::string_view bytes(request.input, request.inputBytes);
        size_t offset = 0; uint64_t token = 0;
        if (!ReadManagedValue(bytes, offset, token) || token > INT64_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto found = textureReceptions.find(static_cast<int64_t>(token));
        if (found == textureReceptions.end()) { result = {}; return request.index == 14 ? ManagedBridgeStatus::OK : ManagedBridgeStatus::NOT_FOUND; }
        auto& reception = found->second;
        if (reception.call != request.callId || !OwnedTexture(reception.owner, scene, caller, request)) return ManagedBridgeStatus::INVALID_SCOPE;
        if (request.index == 12) {
            uint64_t byteOffset = 0; uint32_t count = 0, reserved = 0;
            if (!ReadManagedValue(bytes, offset, byteOffset) || !ReadManagedValue(bytes, offset, count)
                || !ReadManagedValue(bytes, offset, reserved) || reserved || count == 0 || count != bytes.size() - offset
                || byteOffset != reception.received || count > reception.bytes.size() - reception.received)
                return ManagedBridgeStatus::INVALID_ARGUMENT;
            std::memcpy(reception.bytes.data() + reception.received, bytes.data() + offset, count);
            reception.received += count; output.integer = static_cast<int64_t>(reception.received);
        } else {
            if (offset != bytes.size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            if (request.index == 13) {
                if (reception.received != reception.bytes.size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
                const auto texture = textures.find(reception.texture);
                if (texture != textures.end() && OwnedTexture(texture->second, scene, caller, request)) {
                    if (auto* resources = TextureResources(texture->second)) {
                        /// @note Dynamic ITexture::UpdateRegion synchronously delegates to its owning CPU renderer before borrowing these bytes.
                        if (auto* nativeTexture = resources->Get(texture->second.texture))
                            output.integer = nativeTexture->UpdateRegion(reception.x, reception.y, reception.width,
                                reception.height, reception.bytes.data(), reception.rowPitch) ? 1 : 0;
                    }
                }
            } else output.integer = 1;
            textureReceptions.erase(found);
        }
        result = std::move(output); return ManagedBridgeStatus::OK;
    }
    int64_t token = integer;
    if (binding) {
        if (!values.empty() || text.size() != 2 || text[0].empty() || text[0].find('\0') != std::string::npos
            || !TextureInteger(text[1], token) || token <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    const auto found = textures.find(token);
    if (found == textures.end()) { result = {}; return ManagedBridgeStatus::OK; }
    const TextureRecord record = found->second;
    if (!OwnedTexture(record, scene, caller, request)) return ManagedBridgeStatus::INVALID_SCOPE;
    if (binding) {
        auto* component = target.GetComponent<MaterialComponent>();
        if (!component || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::MATERIAL)))
            return ManagedBridgeStatus::NOT_FOUND;
        if (integer < 0 || static_cast<uint64_t>(integer) >= component->SlotCount()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (TextureAvailable(record)) {
            TextureProxy view; view.Bind(scene, target);
            output.integer = view.material.Instance(static_cast<uint32_t>(integer)).SetRuntimeTexture(
                MaterialPropertyId(text[0]), record.texture, record.resourceOwner, record.reset) ? 1 : 0;
        }
    } else if (request.index == 11) {
        if (!values.empty() || text.size() != 5) return ManagedBridgeStatus::INVALID_ARGUMENT;
        TextureReception reception; reception.owner = record; reception.call = request.callId; reception.texture = token;
        if (!TextureInteger(text[0], reception.x) || !TextureInteger(text[1], reception.y)
            || !TextureInteger(text[2], reception.width) || !TextureInteger(text[3], reception.height)
            || !TextureInteger(text[4], reception.rowPitch) || reception.width == 0 || reception.height == 0
            || reception.x > record.width || reception.width > record.width - reception.x
            || reception.y > record.height || reception.height > record.height - reception.y
            || reception.rowPitch < static_cast<uint64_t>(reception.width) * record.pixelBytes)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        const uint64_t required = static_cast<uint64_t>(reception.height - 1u) * reception.rowPitch
            + static_cast<uint64_t>(reception.width) * record.pixelBytes;
        if (required > INT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (TextureAvailable(record) && nextReception != (std::numeric_limits<int64_t>::max)()) {
            reception.bytes.resize(static_cast<size_t>(required));
            const int64_t receptionToken = nextReception++;
            textureReceptions.emplace(receptionToken, std::move(reception)); output.integer = receptionToken;
        }
    } else if (request.index == 15 || request.index == 16) {
        if (!values.empty() || !text.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (request.index == 15) output.integer = TextureAvailable(record) ? 1 : 0;
        else {
            std::erase_if(textureReceptions, [token](const auto& entry) { return entry.second.texture == token; });
            ReleaseTexture(record); textures.erase(found); output.integer = 1;
        }
    } else return ManagedBridgeStatus::INVALID_ARGUMENT;
    result = std::move(output); return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus ManagedAssetServices::Run(Scene& scene, Script& caller, GameObject& target,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result)
{
    if ((request.op == ManagedBridgeOp::SERVICE_ASSETS && request.index >= 10 && request.index <= 16)
        || (request.op == ManagedBridgeOp::SERVICE_MATERIAL && request.index == 42))
        return m_impl->RunTexture(scene, caller, target, request, integer, values, text, result);
    if (caller.scene.Self() != &target || !request.ownerId || !request.generation || !request.instanceId) return ManagedBridgeStatus::INVALID_SCOPE;
    if (request.flags > 1 || request.index > 6 || (request.flags == 1) != (request.index == 1 || request.index == 6)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!Script::IsInPlayMode()) return ManagedBridgeStatus::INVALID_SCOPE;
    ManagedServiceResult output;
    if (request.index == 0) {
        if (integer < 0 || integer > 2 || values.size() != 1 || (values[0] != 0 && values[0] != 1) || text.size() != 1
            || !text[0].starts_with("guid:") || text[0].find('\0') != std::string::npos) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const auto reference = ClassifySceneAssetReference(text[0], [](std::string_view guid) { return asset::AssetDatabase::PathFromGuid(asset::AssetDatabase::GuidFromRef(guid)); });
        if (!reference || (integer == 0 ? reference->kind != SceneAssetKind::Texture : integer == 1 ? reference->kind != SceneAssetKind::Material : reference->kind != SceneAssetKind::Model)) {
            result = {}; return ManagedBridgeStatus::OK;
        }
        const uint64_t token = caller.assets.Request(text[0], values[0] != 0);
        if (token) m_impl->leases.emplace(token, Impl::Lease{&caller, request.ownerId, request.generation, request.instanceId, scene.GetRenderSceneGeneration(), target.GetID()});
        output.integer = std::bit_cast<int64_t>(token);
    } else if (request.index == 5) {
        if (integer || !values.empty() || text.size() != 1 || text[0].empty() || text[0].find('\0') != std::string::npos)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.integer = caller.assets.PrefetchScene(text[0]) ? 1 : 0;
    } else if (request.index == 6) {
        if (integer || !values.empty() || text.size() != 1 || text[0].empty() || text[0].find('\0') != std::string::npos)
            return ManagedBridgeStatus::INVALID_ARGUMENT;
        output.values = {caller.assets.GetScenePrefetchProgress(text[0])};
    } else {
        if (!values.empty() || text.size() != (request.index == 2 ? 1U : 0U)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const uint64_t token = std::bit_cast<uint64_t>(integer); const auto found = m_impl->leases.find(token);
        if (found == m_impl->leases.end()) return ManagedBridgeStatus::NOT_FOUND;
        const auto& owned = found->second;
        if (owned.caller != &caller || owned.owner != request.ownerId || owned.generation != request.generation || owned.instance != request.instanceId
            || owned.scene != scene.GetRenderSceneGeneration() || owned.entity != target.GetID()) return ManagedBridgeStatus::INVALID_SCOPE;
        if (request.index == 1) {
            const auto status = asset::AssetStreamer::Engine().GetLeaseStatus(token);
            output.integer = status.publishedRevision;
            output.values = {static_cast<float>(status.state), static_cast<float>(status.error), status.updateFailed ? 1.f : 0.f,
                static_cast<float>(status.residentQuality), static_cast<float>(status.desiredQuality)};
            output.text = {std::to_string(status.residentBytes), status.message};
        } else if (request.index == 2) {
            int quality = 0; const auto parsed = std::from_chars(text[0].data(), text[0].data() + text[0].size(), quality);
            if (parsed.ec != std::errc{} || parsed.ptr != text[0].data() + text[0].size() || quality < 0 || quality > 255) return ManagedBridgeStatus::INVALID_ARGUMENT;
            caller.assets.SetQuality(token, quality);
        } else if (request.index == 3) output.integer = caller.assets.Retry(token);
        else { caller.assets.Release(token); m_impl->leases.erase(found); output.integer = 1; }
    }
    result = std::move(output); return ManagedBridgeStatus::OK;
}
void ManagedAssetServices::CancelOwner(uint64_t owner, Script* caller) {
    std::erase_if(m_impl->textureReceptions, [owner](const auto& entry) { return entry.second.owner.owner == owner; });
    for (auto it = m_impl->textures.begin(); it != m_impl->textures.end();) {
        if (it->second.owner != owner) { ++it; continue; }
        ReleaseTexture(it->second); it = m_impl->textures.erase(it);
    }
    for (auto it = m_impl->leases.begin(); it != m_impl->leases.end();) {
        if (it->second.owner != owner) { ++it; continue; }
        if (caller && caller == it->second.caller) caller->assets.Release(it->first);
        else asset::AssetStreamer::Engine().ReleaseLease(it->first);
        it = m_impl->leases.erase(it);
    }
}
void ManagedAssetServices::RetireGeneration(uint64_t generation) {
    std::erase_if(m_impl->textureReceptions, [generation](const auto& entry) { return entry.second.owner.generation == generation; });
    for (auto it = m_impl->textures.begin(); it != m_impl->textures.end();) {
        if (it->second.generation != generation) { ++it; continue; }
        ReleaseTexture(it->second); it = m_impl->textures.erase(it);
    }
    for (auto it = m_impl->leases.begin(); it != m_impl->leases.end();) {
        if (it->second.generation != generation) { ++it; continue; }
        asset::AssetStreamer::Engine().ReleaseLease(it->first); it = m_impl->leases.erase(it);
    }
}
void ManagedAssetServices::EndCall(uint64_t call)
{
    std::erase_if(m_impl->textureReceptions, [call](const auto& entry) { return entry.second.call == call; });
}
} /// @note namespace fbzz::scene::managed_detail
