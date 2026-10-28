/// @file    ManagedJointTransfers.cpp
/// @brief   全 chain targets の検証後だけ既存 Joint row を変更する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedJointTransfers.hpp"
#include "ManagedJointService.hpp"
#include "ManagedObjectControls.hpp"
#include "ManagedServicesBridge.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <charconv>
#include <cmath>
#include <cstring>
#include <unordered_map>
namespace fbzz::scene::managed_detail {
struct ManagedJointTransfers::Impl {
    struct Record { uint64_t owner, generation, instance, call, scene, birth; EntityID entity;
        uint32_t expected, iterations; float distance; std::vector<uint64_t> targets; };
    std::unordered_map<uint64_t, Record> records; uint64_t next = 1;
};
ManagedJointTransfers::ManagedJointTransfers() : m_impl(std::make_unique<Impl>()) {}
ManagedJointTransfers::~ManagedJointTransfers() = default;
ManagedBridgeStatus ManagedJointTransfers::Begin(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
    int64_t count, std::span<const float> values, std::span<const std::string> text, ManagedServiceResult& result, bool fixed)
{
    if (!fixed || request.flags || !request.ownerId || !request.instanceId || !request.generation || !request.callId) return ManagedBridgeStatus::INVALID_SCOPE;
    if (count < 1 || count > UINT32_MAX || values.size() != 1 || !std::isfinite(values[0]) || text.size() != 1) return ManagedBridgeStatus::INVALID_ARGUMENT;
    uint32_t iterations = 0; const auto parsed = std::from_chars(text[0].data(), text[0].data() + text[0].size(), iterations);
    if (parsed.ec != std::errc{} || parsed.ptr != text[0].data() + text[0].size() || !iterations || iterations > INT32_MAX) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (!target.GetComponent<JointComponent>() || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::JOINT))) return ManagedBridgeStatus::NOT_FOUND;
    if (m_impl->records.size() >= 32 || m_impl->next > INT64_MAX) return ManagedBridgeStatus::PENDING_LIMIT;
    Impl::Record record{request.ownerId, request.generation, request.instanceId, request.callId, scene.GetRenderSceneGeneration(),
        scene.GetComponentLifetime<JointComponent>(target.GetID()), target.GetID(), static_cast<uint32_t>(count), iterations, values[0], {}};
    record.targets.reserve(static_cast<size_t>(count)); const uint64_t token = m_impl->next++; m_impl->records.emplace(token, std::move(record));
    result = {}; result.integer = static_cast<int64_t>(token); return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus ManagedJointTransfers::Run(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
    ManagedServiceResult& result, Resolver resolver, void* context, bool fixed)
{
    if (!fixed || request.flags || !request.input || request.inputBytes < 8) return ManagedBridgeStatus::INVALID_SCOPE;
    uint64_t token = 0; std::memcpy(&token, request.input, 8); const auto found = m_impl->records.find(token);
    if (found == m_impl->records.end()) return ManagedBridgeStatus::NOT_FOUND;
    auto& record = found->second;
    if (record.owner != request.ownerId || record.generation != request.generation || record.instance != request.instanceId || record.call != request.callId
        || record.scene != scene.GetRenderSceneGeneration() || record.entity != target.GetID()) return ManagedBridgeStatus::INVALID_SCOPE;
    auto* row = target.GetComponent<JointComponent>();
    if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::JOINT))
        || scene.GetComponentLifetime<JointComponent>(target.GetID()) != record.birth) { m_impl->records.erase(found); return ManagedBridgeStatus::NOT_FOUND; }
    if (request.index == 21) { if (request.inputBytes != 8) return ManagedBridgeStatus::INVALID_ARGUMENT; m_impl->records.erase(found); result = {}; return ManagedBridgeStatus::OK; }
    if (request.index == 20) {
        uint64_t offset = 0; uint32_t count = 0, reserved = 0;
        if (request.inputBytes < 24) return ManagedBridgeStatus::INVALID_ARGUMENT;
        std::memcpy(&offset, request.input + 8, 8); std::memcpy(&count, request.input + 16, 4); std::memcpy(&reserved, request.input + 20, 4);
        if (reserved || !count || request.inputBytes > kManagedBatchPacketMaxBytes || offset != record.targets.size()
            || count > record.expected - record.targets.size() || request.inputBytes != 24ULL + static_cast<uint64_t>(count) * 16) { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
        for (uint32_t index = 0; index < count; ++index) {
            uint64_t id = 0, lifetime = 0; std::memcpy(&id, request.input + 24 + static_cast<size_t>(index) * 16, 8);
            std::memcpy(&lifetime, request.input + 32 + static_cast<size_t>(index) * 16, 8);
            if (!id || lifetime != record.scene) { m_impl->records.erase(found); return ManagedBridgeStatus::SCENE_MISMATCH; }
        }
        for (uint32_t index = 0; index < count; ++index) { uint64_t id = 0; std::memcpy(&id, request.input + 24 + static_cast<size_t>(index) * 16, 8); record.targets.push_back(id); }
        result = {}; result.integer = static_cast<int64_t>(record.targets.size()); return ManagedBridgeStatus::OK;
    }
    if (request.index != 22 || request.inputBytes != 8 || record.targets.size() != record.expected || !resolver) { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
    std::vector<GameObject*> targets; targets.reserve(record.targets.size());
    for (uint64_t id : record.targets) {
        GameObject* object = nullptr; const auto status = resolver(context, id, object);
        if (status != ManagedBridgeStatus::OK) { m_impl->records.erase(found); return status; } targets.push_back(object);
    }
    JointComponent candidate = *row; const bool valid = PrepareJointChain(scene, target, targets, record.distance, record.iterations, candidate);
    if (valid) *row = std::move(candidate);
    m_impl->records.erase(found); result = {}; result.integer = valid; return ManagedBridgeStatus::OK;
}
void ManagedJointTransfers::EndCall(uint64_t call) { std::erase_if(m_impl->records, [call](const auto& row) { return row.second.call == call; }); }
void ManagedJointTransfers::CancelOwner(uint64_t owner) { std::erase_if(m_impl->records, [owner](const auto& row) { return row.second.owner == owner; }); }
void ManagedJointTransfers::RetireGeneration(uint64_t generation) { std::erase_if(m_impl->records, [generation](const auto& row) { return row.second.generation == generation; }); }
} /// @note namespace fbzz::scene::managed_detail
