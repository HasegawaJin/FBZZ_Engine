/// @file    ManagedComponentTransfers.cpp
/// @brief   UTF8 と recipe revision を検証し全骨名の受信後だけ実 row に移す。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedComponentTransfers.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedIntegerParsing.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/RagdollComponent.hpp>
#include <unordered_map>
#include <vector>
namespace fbzz::scene::managed_detail {
namespace {

/// @note ManagedStoreTransfers と同じ strict UTF8 条件で chunk は codepoint 境界を要求する。
bool Utf8(const std::string& text)
{
    uint32_t point = 0, minimum = 0, remaining = 0;
    for (unsigned char byte : text) {
        if (!remaining) {
            if (!byte) return false;
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
    return remaining == 0;
}
} /// @note namespace
struct ManagedComponentTransfers::Impl {
    struct Record {
        uint64_t owner, generation, instance, call, scene, birth, expected;
        EntityID entity;
        bool exclusions;
        int depth, previousDepth;
        std::string previousRoot, pendingName;
        std::vector<std::string> previousNames, names;
    };
    uint64_t next = 1;
    std::unordered_map<uint64_t, Record> records;
};
ManagedComponentTransfers::ManagedComponentTransfers() : m_impl(std::make_unique<Impl>()) {}
ManagedComponentTransfers::~ManagedComponentTransfers() = default;
ManagedBridgeStatus ManagedComponentTransfers::Run(Scene& scene, GameObject& target, const ManagedBridgeRequest& request,
    int64_t integer, std::span<const float> p, std::span<const std::string> t, ManagedServiceResult& result, bool fixedUpdate)
{
    if (request.op != ManagedBridgeOp::SERVICE_RAGDOLL) return ManagedBridgeStatus::UNSUPPORTED;
    if (!fixedUpdate || !Script::IsInPlayMode() || request.flags || !request.ownerId || !request.instanceId || !request.generation || !request.callId)
        return ManagedBridgeStatus::INVALID_SCOPE;
    auto* row = target.GetComponent<RagdollComponent>();
    if (!row || ManagedObjectControls::IsPendingComponentRemoval(target, static_cast<uint32_t>(ManagedNativeComponentKind::RAGDOLL))) return ManagedBridgeStatus::NOT_FOUND;
    if (request.index == 40) {
        uint64_t count = 0;
        if (integer < 0 || integer > INT32_MAX || p.size() != 1 || (p[0] != 0 && p[0] != 1) || t.size() != 1 || !ParseInteger(t[0], count)
            || count > std::vector<std::string>{}.max_size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (m_impl->records.size() >= 32 || m_impl->next > INT64_MAX) return ManagedBridgeStatus::PENDING_LIMIT;
        const bool exclusions = p[0] != 0;
        Impl::Record record{request.ownerId, request.generation, request.instanceId, request.callId, scene.GetRenderSceneGeneration(),
            scene.GetComponentLifetime<RagdollComponent>(target.GetID()), count, target.GetID(), exclusions, static_cast<int>(integer), row->maxDepth,
            row->rootBoneName, {}, exclusions ? row->excludedRootBones : row->extraRootBones, {}};
        record.names.reserve(static_cast<size_t>(count));
        const uint64_t token = m_impl->next++; m_impl->records.emplace(token, std::move(record));
        result = {}; result.integer = static_cast<int64_t>(token); return ManagedBridgeStatus::OK;
    }
    if (integer <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto found = m_impl->records.find(static_cast<uint64_t>(integer));
    if (found == m_impl->records.end()) return ManagedBridgeStatus::NOT_FOUND;
    auto& record = found->second;
    if (record.owner != request.ownerId || record.generation != request.generation || record.instance != request.instanceId || record.call != request.callId
        || record.scene != scene.GetRenderSceneGeneration() || record.entity != target.GetID()) return ManagedBridgeStatus::INVALID_SCOPE;
    const bool sameRecipe = record.exclusions ? row->excludedRootBones == record.previousNames
        : row->rootBoneName == record.previousRoot && row->extraRootBones == record.previousNames && row->maxDepth == record.previousDepth;
    if (scene.GetComponentLifetime<RagdollComponent>(target.GetID()) != record.birth || !sameRecipe) {
        m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_SCOPE;
    }
    const auto invalid = [&]() { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; };
    if (request.index == 43) {
        if (!p.empty() || !t.empty()) return invalid();
        m_impl->records.erase(found); result = {}; return ManagedBridgeStatus::OK;
    }
    if (request.index == 41) {
        uint64_t index = 0, offset = 0;
        if (p.size() != 1 || (p[0] != 0 && p[0] != 1) || t.size() != 3 || !ParseInteger(t[0], index) || !ParseInteger(t[1], offset)
            || index != record.names.size() || index >= record.expected || offset != record.pendingName.size() || t[2].empty()
            || t[2].size() > 32768 || !Utf8(t[2]) || t[2].size() > record.pendingName.max_size() - record.pendingName.size()) return invalid();
        record.pendingName.append(t[2]);
        if (p[0] != 0) { record.names.push_back(std::move(record.pendingName)); record.pendingName.clear(); }
        result = {}; result.integer = static_cast<int64_t>(record.names.size()); return ManagedBridgeStatus::OK;
    }
    if (request.index != 42 || !p.empty() || !t.empty() || record.names.size() != record.expected || !record.pendingName.empty()) return invalid();
    if (record.exclusions) row->excludedRootBones = std::move(record.names);
    else {
        std::string root; std::vector<std::string> extra;
        if (!record.names.empty()) {
            root = std::move(record.names.front()); extra.reserve(record.names.size() - 1);
            for (size_t index = 1; index < record.names.size(); ++index) extra.push_back(std::move(record.names[index]));
        }
        /// @note 全 allocation を済ませてから recipe だけを移し、稼働中 rig の所有状態は複製しない。
        row->rootBoneName = std::move(root); row->extraRootBones = std::move(extra); row->maxDepth = record.depth;
    }
    m_impl->records.erase(found); result = {}; return ManagedBridgeStatus::OK;
}
void ManagedComponentTransfers::EndCall(uint64_t call)
{ std::erase_if(m_impl->records, [call](const auto& item) { return item.second.call == call; }); }
void ManagedComponentTransfers::CancelOwner(uint64_t owner)
{ std::erase_if(m_impl->records, [owner](const auto& item) { return item.second.owner == owner; }); }
void ManagedComponentTransfers::RetireGeneration(uint64_t generation)
{ std::erase_if(m_impl->records, [generation](const auto& item) { return item.second.generation == generation; }); }
} /// @note namespace fbzz::scene::managed_detail
