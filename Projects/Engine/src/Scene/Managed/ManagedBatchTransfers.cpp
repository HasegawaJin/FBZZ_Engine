/// @file    ManagedBatchTransfers.cpp
/// @brief   bounded packet から全件候補を組み立て、全体 preflight 後だけ変更する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedBatchTransfers.hpp"
#include "ManagedObjectLifecycle.hpp"
#include <Engine/Scene/Scene.hpp>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
namespace fbzz::scene::managed_detail {
struct ManagedBatchTransfers::Impl {
    struct Record {
        ManagedBridgeRequest original;
        uint64_t scene = 0;
        uint32_t stride = 0, outputStride = 0, received = 0;
        bool prepared = false;
        bool published = false;
        uint32_t applied = 0, skipped = 0;
        std::string input, output;
    };
    std::unordered_map<uint64_t, Record> records;
    struct Query { ManagedBridgeRequest original; uint64_t scene; uint32_t count; std::string bytes; };
    std::unordered_map<uint64_t, Query> queries;
    uint64_t next = 1;
};
ManagedBatchTransfers::ManagedBatchTransfers() : m_impl(std::make_unique<Impl>()) {}
ManagedBatchTransfers::~ManagedBatchTransfers() = default;
ManagedBridgeStatus ManagedBatchTransfers::Run(Scene& scene, const ManagedBridgeRequest& request, ManagedBridgeReply& reply,
    ManagedTransformBatch& transforms, ManagedTransformBatch::ObjectResolver resolver, void* context)
{
    reply = {}; const uint32_t phase = request.flags >> 8, options = request.flags & 255U;
    const bool read = request.op == ManagedBridgeOp::OBJECTS_READ_WORLD_POSITIONS;
    const bool set = request.op == ManagedBridgeOp::OBJECTS_SET_WORLD_POSITIONS;
    const bool translate = request.op == ManagedBridgeOp::OBJECTS_TRANSLATE_WORLD;
    const bool destroy = request.op == ManagedBridgeOp::OBJECTS_DESTROY;
    if ((!read && !set && !translate && !destroy) || (translate ? options > 1 : options != 0)
        || !resolver || !request.ownerId || !request.instanceId || !request.generation || !request.callId) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (phase == 1) {
        if (request.integer || request.inputBytes || request.outputCapacity || request.reserved0 || request.reserved1
            || (destroy && (!std::isfinite(request.x) || request.x < 0))
            || (translate && (!std::isfinite(request.x) || !std::isfinite(request.y) || !std::isfinite(request.z)))) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (m_impl->records.size() + m_impl->queries.size() >= 32 || m_impl->next > static_cast<uint64_t>(INT64_MAX)) return ManagedBridgeStatus::INVALID_SCOPE;
        Impl::Record record; record.original = request; record.original.flags = options; record.scene = scene.GetRenderSceneGeneration();
        record.stride = set ? kManagedBatchSetItemBytes : kManagedBatchReferenceBytes;
        record.outputStride = read ? kManagedBatchReadItemBytes : set ? kManagedBatchStatusBytes : 0;
        const uint64_t bytes = static_cast<uint64_t>(request.index) * record.stride;
        if (bytes > record.input.max_size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        record.input.reserve(static_cast<size_t>(bytes));
        const uint64_t token = m_impl->next++; m_impl->records.emplace(token, std::move(record)); reply.integer = static_cast<int64_t>(token);
        reply.callId = scene.GetRenderSceneGeneration(); return ManagedBridgeStatus::OK;
    }
    if (request.integer <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto found = m_impl->records.find(static_cast<uint64_t>(request.integer));
    if (found == m_impl->records.end()) return ManagedBridgeStatus::INVALID_SCOPE;
    auto& record = found->second; const auto& original = record.original;
    if (original.op != request.op || original.ownerId != request.ownerId || original.instanceId != request.instanceId
        || original.generation != request.generation || original.callId != request.callId || original.flags != options
        || record.scene != scene.GetRenderSceneGeneration()) return ManagedBridgeStatus::INVALID_SCOPE;
    reply.callId = record.scene;
    if (phase != 4 && (request.reserved0 || request.reserved1)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (phase == 5) { if (request.index || request.inputBytes || request.outputCapacity) return ManagedBridgeStatus::INVALID_ARGUMENT; m_impl->records.erase(found); return ManagedBridgeStatus::OK; }
    if (phase == 2) {
        if (record.prepared || request.outputCapacity || request.inputBytes == 0 || request.inputBytes > kManagedBatchPacketMaxBytes
            || request.inputBytes % record.stride || request.index != record.received
            || request.inputBytes / record.stride > original.index - record.received || !request.input) {
            m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        record.input.append(request.input, request.inputBytes); record.received += request.inputBytes / record.stride;
        reply.integer = record.received; return ManagedBridgeStatus::OK;
    }
    if (phase == 3 || phase == 6) {
        const bool publish = phase == 6;
        if ((publish ? !record.prepared || record.published : record.prepared) || record.received != original.index
            || request.inputBytes || request.outputCapacity || request.index) { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
        ManagedBridgeStatus status = ManagedBridgeStatus::OK;
        std::string freshOutput;
        if (destroy) {
            std::vector<GameObject*> targets; targets.reserve(original.index);
            for (uint32_t index = 0; index < original.index; ++index) {
                uint64_t token = 0, lifetime = 0; const char* row = record.input.data() + static_cast<size_t>(index) * record.stride;
                std::memcpy(&token, row, 8); std::memcpy(&lifetime, row + 8, 8);
                if ((token && !lifetime) || (lifetime && lifetime != record.scene)) { status = ManagedBridgeStatus::SCENE_MISMATCH; break; }
                GameObject* target = nullptr; if (token) status = resolver(context, token, target);
                if (status != ManagedBridgeStatus::OK) break; targets.push_back(target);
            }
            if (status == ManagedBridgeStatus::OK) status = ManagedObjectLifecycle::DestroyMany(scene, targets, original.x, reply.index, reply.flags, false);
            if (publish && status == ManagedBridgeStatus::OK && (reply.index != record.applied || reply.flags != record.skipped)) status = ManagedBridgeStatus::INVALID_SCOPE;
            if (publish && status == ManagedBridgeStatus::OK) status = ManagedObjectLifecycle::DestroyMany(scene, targets, original.x, reply.index, reply.flags, true);
        } else {
            const uint64_t bytes = static_cast<uint64_t>(original.index) * record.outputStride;
            if (bytes > record.output.max_size()) { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; }
            auto& destination = publish ? freshOutput : record.output; if (!publish || !read) destination.resize(static_cast<size_t>(bytes));
            if (publish && read) { reply.index = record.applied; reply.flags = record.skipped; }
            else status = transforms.RunAggregate(scene, original, record.input, destination, reply, resolver, context, false);
            if (publish && !read && status == ManagedBridgeStatus::OK
                && (reply.index != record.applied || reply.flags != record.skipped || destination != record.output)) status = ManagedBridgeStatus::INVALID_SCOPE;
            if (publish && !read && status == ManagedBridgeStatus::OK) status = transforms.RunAggregate(scene, original, record.input, destination, reply, resolver, context, true);
        }
        if (status != ManagedBridgeStatus::OK) { reply.index = reply.flags = reply.outputBytes = 0; m_impl->records.erase(found); return status; }
        record.prepared = true; record.published = publish; record.applied = reply.index; record.skipped = reply.flags;
        reply.outputBytes = 0; reply.generation = record.output.size(); return ManagedBridgeStatus::OK;
    }
    if (phase == 4) {
        const uint64_t offset = (static_cast<uint64_t>(request.reserved1) << 32) | request.reserved0;
        if (!record.prepared || record.published || request.inputBytes || !request.output || !request.outputCapacity || request.outputCapacity > kManagedBatchPacketMaxBytes
            || offset > record.output.size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        const size_t count = std::min<size_t>(request.outputCapacity, record.output.size() - static_cast<size_t>(offset));
        if (count) std::memcpy(request.output, record.output.data() + static_cast<size_t>(offset), count);
        reply.outputBytes = static_cast<uint32_t>(count); reply.generation = record.output.size(); reply.integer = static_cast<int64_t>(offset + count);
        reply.flags = offset + count == record.output.size() ? 1U : 0U; return ManagedBridgeStatus::OK;
    }
    return ManagedBridgeStatus::INVALID_ARGUMENT;
}
ManagedBridgeStatus ManagedBatchTransfers::OpenRead(Scene& scene, const ManagedBridgeRequest& request, std::string&& bytes,
    uint32_t count, ManagedBridgeReply& reply)
{
    if (!request.ownerId || !request.instanceId || !request.generation || !request.callId || request.flags > 255U) return ManagedBridgeStatus::INVALID_SCOPE;
    if (bytes.size() > static_cast<uint64_t>(INT64_MAX)) return ManagedBridgeStatus::INVALID_ARGUMENT;
    if (m_impl->queries.size() + m_impl->records.size() >= 32 || m_impl->next > static_cast<uint64_t>(INT64_MAX)) return ManagedBridgeStatus::PENDING_LIMIT;
    const uint64_t token = m_impl->next++, total = bytes.size(), lifetime = scene.GetRenderSceneGeneration();
    m_impl->queries.emplace(token, Impl::Query{request, lifetime, count, std::move(bytes)});
    reply.integer = static_cast<int64_t>(token); reply.index = count; reply.generation = total; reply.callId = lifetime;
    reply.outputBytes = total <= UINT32_MAX ? static_cast<uint32_t>(total) : 0;
    return ManagedBridgeStatus::OK;
}
ManagedBridgeStatus ManagedBatchTransfers::ReadSnapshot(Scene& scene, const ManagedBridgeRequest& request, ManagedBridgeReply& reply)
{
    reply = {}; if (request.integer <= 0 || request.inputBytes) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto found = m_impl->queries.find(static_cast<uint64_t>(request.integer)); if (found == m_impl->queries.end()) return ManagedBridgeStatus::NOT_FOUND;
    const auto& captured = found->second; const auto& original = captured.original;
    if (original.op != request.op || original.ownerId != request.ownerId || original.instanceId != request.instanceId
        || original.generation != request.generation || original.callId != request.callId || (request.flags & 255U) != original.flags
        || captured.scene != scene.GetRenderSceneGeneration()) return ManagedBridgeStatus::INVALID_SCOPE;
    reply.callId = captured.scene;
    const uint32_t phase = request.flags >> 8;
    if (phase == 3) { if (request.outputCapacity || request.index || request.reserved0 || request.reserved1) return ManagedBridgeStatus::INVALID_ARGUMENT; m_impl->queries.erase(found); return ManagedBridgeStatus::OK; }
    const uint64_t offset = (static_cast<uint64_t>(request.reserved1) << 32) | request.reserved0;
    if (phase != 2 || request.index || !request.output || !request.outputCapacity || request.outputCapacity > kManagedBatchPacketMaxBytes || offset > captured.bytes.size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const size_t count = std::min<size_t>(request.outputCapacity, captured.bytes.size() - static_cast<size_t>(offset));
    if (count) std::memcpy(request.output, captured.bytes.data() + static_cast<size_t>(offset), count);
    reply.outputBytes = static_cast<uint32_t>(count); reply.generation = captured.bytes.size(); reply.index = captured.count;
    reply.integer = static_cast<int64_t>(offset + count); reply.flags = offset + count == captured.bytes.size() ? 1U : 0U;
    return ManagedBridgeStatus::OK;
}
void ManagedBatchTransfers::EndCall(uint64_t call) {
    std::erase_if(m_impl->records, [call](const auto& row) { return row.second.original.callId == call; });
    std::erase_if(m_impl->queries, [call](const auto& row) { return row.second.original.callId == call; });
}
void ManagedBatchTransfers::CancelOwner(uint64_t owner) {
    std::erase_if(m_impl->records, [owner](const auto& row) { return row.second.original.ownerId == owner; });
    std::erase_if(m_impl->queries, [owner](const auto& row) { return row.second.original.ownerId == owner; });
}
void ManagedBatchTransfers::RetireGeneration(uint64_t generation) {
    std::erase_if(m_impl->records, [generation](const auto& row) { return row.second.original.generation == generation; });
    std::erase_if(m_impl->queries, [generation](const auto& row) { return row.second.original.generation == generation; });
}
} /// @note namespace fbzz::scene::managed_detail
