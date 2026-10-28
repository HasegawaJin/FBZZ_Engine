/// @file    ManagedStoreTransfers.cpp
/// @brief   owner/call/store revision の検証・strict UTF8 と未公開 string の所有移送。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include "ManagedStoreTransfers.hpp"
#include "ManagedPacketRead.hpp"
#include "ManagedServicesBridge.hpp"
#include <Engine/Core/Application.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Util/SaveStore.hpp>
#include <limits>
#include <cstring>
#include <unordered_map>

namespace fbzz::scene::managed_detail {
namespace {
template<typename T> bool ReadTransfer(std::string_view bytes, size_t& position, T& value)
{
    return ReadManagedValue(bytes, position, value);
}
util::SaveStore& Store(ManagedBridgeOp operation)
{ auto& app = core::Application::Get(); return operation == ManagedBridgeOp::SERVICE_CONFIG ? app.GetConfigStore() : app.GetSaveStore(); }
struct Utf8State {
    uint32_t point = 0, minimum = 0, remaining = 0;
    bool Push(std::string_view bytes)
    {
        for (unsigned char byte : bytes) {
            if (!remaining) {
                if (byte < 0x80) continue;
                if (byte >= 0xC2 && byte <= 0xDF) { point = byte & 0x1F; minimum = 0x80; remaining = 1; }
                else if (byte >= 0xE0 && byte <= 0xEF) { point = byte & 0x0F; minimum = 0x800; remaining = 2; }
                else if (byte >= 0xF0 && byte <= 0xF4) { point = byte & 7; minimum = 0x10000; remaining = 3; }
                else return false;
            } else {
                if ((byte & 0xC0) != 0x80) return false;
                point = (point << 6) | (byte & 0x3F);
                if (!--remaining && (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF))) return false;
            }
        }
        return true;
    }
};
} /// @note namespace
struct ManagedStoreTransfers::Impl {
    struct Reception {
        ManagedBridgeOp store;
        uint64_t generation, instance, owner, call, scene, revision, expected;
        EntityID entity;
        std::string key, path, bytes, type, schema;
        Utf8State utf8;
    };
    uint64_t next = 1;
    std::unordered_map<uint64_t, Reception> receptions;
};
ManagedStoreTransfers::ManagedStoreTransfers() : m_impl(std::make_unique<Impl>()) {}
ManagedStoreTransfers::~ManagedStoreTransfers() = default;
ManagedBridgeStatus ManagedStoreTransfers::Begin(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
    int64_t expectedBytes, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result)
{
    if (!Script::IsInPlayMode() || !request.callId || !request.ownerId || !request.instanceId || !request.generation) return ManagedBridgeStatus::INVALID_SCOPE;
    const bool document = request.index == 26;
    if (request.flags || expectedBytes < 0 || !values.empty() || text.size() != (document ? 3U : 1U) || text[0].empty()
        || text[0].find('\0') != std::string::npos) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (document && (text[1].empty() || text[2].empty() || text[1].find('\0') != std::string::npos || text[2].find('\0') != std::string::npos)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (m_impl->receptions.size() >= 32 || m_impl->next > static_cast<uint64_t>(INT64_MAX)) return ManagedBridgeStatus::PENDING_LIMIT;
    auto& store = Store(request.op);
    Impl::Reception reception{request.op, request.generation, request.instanceId, request.ownerId, request.callId,
        scene.GetRenderSceneGeneration(), store.GetRevision(), static_cast<uint64_t>(expectedBytes), target.GetID(),
        text[0], store.GetPath(), {}, document ? text[1] : std::string{}, document ? text[2] : std::string{}, {}};
    if (reception.expected > reception.bytes.max_size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    reception.bytes.reserve(static_cast<size_t>(reception.expected));
    const uint64_t token = m_impl->next++;
    m_impl->receptions.emplace(token, std::move(reception));
    result = {}; result.integer = static_cast<int64_t>(token);
    return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus ManagedStoreTransfers::Run(Scene& scene, GameObject& target, const ManagedBridgeRequest& request, ManagedServiceResult& result)
{
    if (!Script::IsInPlayMode() || request.flags || !request.input || request.inputBytes < 8) return ManagedBridgeStatus::INVALID_SCOPE;
    std::string_view bytes(request.input, request.inputBytes); size_t position = 0; uint64_t token = 0;
    if (!ReadTransfer(bytes, position, token)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto found = m_impl->receptions.find(token);
    if (found == m_impl->receptions.end()) return ManagedBridgeStatus::NOT_FOUND;
    auto& reception = found->second;
    if (reception.owner != request.ownerId || reception.instance != request.instanceId || reception.generation != request.generation
        || reception.call != request.callId || reception.scene != scene.GetRenderSceneGeneration() || reception.entity != target.GetID()
        || reception.store != request.op) return ManagedBridgeStatus::INVALID_SCOPE;
    auto& store = Store(request.op);
    if (store.GetRevision() != reception.revision || store.GetPath() != reception.path) {
        m_impl->receptions.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    if (request.index == 25) {
        if (bytes.size() != 8) return ManagedBridgeStatus::INVALID_ARGUMENT;
        m_impl->receptions.erase(found); result = {}; return ManagedBridgeStatus::OK;
    }
    if (request.index == 23) {
        uint64_t offset = 0; uint32_t count = 0, reserved = 0;
        if (!ReadTransfer(bytes, position, offset) || !ReadTransfer(bytes, position, count) || !ReadTransfer(bytes, position, reserved)
            || reserved || !count || count > 65536U - 24U || bytes.size() - position != count || offset != reception.bytes.size()
            || offset > reception.expected || count > reception.expected - offset) {
            m_impl->receptions.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        auto next = reception.utf8;
        if (!next.Push(bytes.substr(position))) { m_impl->receptions.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
        reception.bytes.append(bytes.substr(position)); reception.utf8 = next;
        result = {}; result.integer = static_cast<int64_t>(reception.bytes.size()); return ManagedBridgeStatus::OK;
    }
    if (request.index != 24 || bytes.size() != 8) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (reception.bytes.size() != reception.expected || reception.utf8.remaining) {
        m_impl->receptions.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT;
    }
    const bool committed = reception.type.empty()
        ? store.CommitString(reception.key, std::move(reception.bytes), reception.revision, reception.path)
        : store.CommitDocument(reception.key, std::move(reception.type), std::move(reception.schema), std::move(reception.bytes), reception.revision, reception.path);
    m_impl->receptions.erase(found);
    if (!committed) return ManagedBridgeStatus::INVALID_ARGUMENT;
    result = {}; result.integer = 1; return ManagedBridgeStatus::OK;
}
void ManagedStoreTransfers::CancelOwner(uint64_t owner)
{ std::erase_if(m_impl->receptions, [owner](const auto& entry) { return entry.second.owner == owner; }); }
void ManagedStoreTransfers::RetireGeneration(uint64_t generation)
{ std::erase_if(m_impl->receptions, [generation](const auto& entry) { return entry.second.generation == generation; }); }
void ManagedStoreTransfers::EndCall(uint64_t call)
{ std::erase_if(m_impl->receptions, [call](const auto& entry) { return entry.second.call == call; }); }
} /// @note namespace fbzz::scene::managed_detail
