/// @file    ManagedRenderTransfers.cpp
/// @brief   finite geometry・連続 page・triangle topology と exact birth の全件検証と原子的 commit。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include "ManagedRenderTransfers.hpp"
#include "ManagedServicesBridge.hpp"
#include "ManagedObjectControls.hpp"
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ControlPointResolver.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <algorithm>
#include <bit>
#include <Engine/Util/NumberParsing.hpp>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fbzz::scene::managed_detail {
namespace {
bool Offset(std::string_view text, uint64_t& value)
{
    return util::ParseInteger(text, value);
}
uint32_t Kind(ManagedBridgeOp operation)
{
    return static_cast<uint32_t>(operation == ManagedBridgeOp::SERVICE_LINE
        ? ManagedNativeComponentKind::LINE_RENDERER : ManagedNativeComponentKind::MESH_RENDERER);
}
uint64_t Birth(Scene& scene, GameObject& target, ManagedBridgeOp operation)
{
    return operation == ManagedBridgeOp::SERVICE_LINE ? scene.GetComponentLifetime<LineRendererComponent>(target.GetID())
        : scene.GetComponentLifetime<MeshRenderer>(target.GetID());
}
} /// @note namespace

struct ManagedRenderTransfers::Impl {
    struct Record {
        ManagedBridgeOp operation;
        uint64_t owner, instance, generation, call, scene, object, birth, proceduralBirth;
        EntityID entity;
        uint32_t expectedVertices, expectedIndices;
        bool world, verticesOnly;
        std::vector<math::Vector3> points;
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
    };
    uint64_t next = 1;
    std::unordered_map<uint64_t, Record> records;
};
ManagedRenderTransfers::ManagedRenderTransfers() : m_impl(std::make_unique<Impl>()) {}
ManagedRenderTransfers::~ManagedRenderTransfers() = default;

ManagedBridgeStatus ManagedRenderTransfers::Run(Scene& scene, GameObject& target, Script& view,
    const ManagedBridgeRequest& request, int64_t integer, std::span<const float> values,
    std::span<const std::string> text, ManagedServiceResult& result)
{
    (void)view;
    if (request.op != ManagedBridgeOp::SERVICE_LINE && request.op != ManagedBridgeOp::SERVICE_MESH) return ManagedBridgeStatus::UNSUPPORTED;
    if (!request.callId || !request.ownerId || !request.instanceId || !request.generation
        || target.GetScene() != &scene || scene.GetGameObject(target.GetID()) != &target) return ManagedBridgeStatus::INVALID_SCOPE;
    if (request.flags) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const bool line = request.op == ManagedBridgeOp::SERVICE_LINE;
    const uint32_t kind = Kind(request.op);
    const uint64_t birth = Birth(scene, target, request.op);
    if (!birth || ManagedObjectControls::IsPendingComponentRemoval(target, kind)) return ManagedBridgeStatus::NOT_FOUND;
    if (request.index == 50) {
        if (values.size() != 1 || (values[0] != 0 && values[0] != 1) || !text.empty()) return ManagedBridgeStatus::INVALID_ARGUMENT;
        if (m_impl->records.size() >= 32 || m_impl->next > static_cast<uint64_t>(INT64_MAX)) return ManagedBridgeStatus::PENDING_LIMIT;
        uint32_t vertices = 0, indices = 0;
        if (line) {
            if (integer < 0 || static_cast<uint64_t>(integer) > MAX_CONTROL_POINT_BINDINGS) return ManagedBridgeStatus::INVALID_ARGUMENT;
            vertices = static_cast<uint32_t>(integer);
        } else {
            const uint64_t counts = std::bit_cast<uint64_t>(integer);
            vertices = static_cast<uint32_t>(counts); indices = static_cast<uint32_t>(counts >> 32);
            if (indices % 3 || (values[0] == 1 && indices != 0)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        }
        const bool verticesOnly = !line && values[0] == 1;
        const auto* procedural = target.GetComponent<ProceduralMeshComponent>();
        if (verticesOnly && (!procedural || procedural->builder.VertexCount() != vertices)) return ManagedBridgeStatus::INVALID_ARGUMENT;
        Impl::Record record{request.op, request.ownerId, request.instanceId, request.generation, request.callId,
            scene.GetRenderSceneGeneration(), std::bit_cast<uint64_t>(request.integer), birth,
            verticesOnly ? scene.GetComponentLifetime<ProceduralMeshComponent>(target.GetID()) : 0,
            target.GetID(), vertices, indices, line && values[0] == 1, verticesOnly, {}, {}, {}};
        if (line) {
            if (vertices > record.points.max_size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            record.points.reserve(vertices);
        } else {
            if (vertices > record.vertices.max_size() || indices > record.indices.max_size()) return ManagedBridgeStatus::INVALID_ARGUMENT;
            record.vertices.reserve(vertices); record.indices.reserve(indices);
        }
        const uint64_t token = m_impl->next++;
        m_impl->records.emplace(token, std::move(record));
        result = {}; result.integer = static_cast<int64_t>(token);
        return ManagedBridgeStatus::OK;
    }
    if (integer <= 0) return ManagedBridgeStatus::INVALID_ARGUMENT;
    const auto found = m_impl->records.find(static_cast<uint64_t>(integer));
    if (found == m_impl->records.end()) return ManagedBridgeStatus::NOT_FOUND;
    auto& record = found->second;
    if (record.operation != request.op || record.owner != request.ownerId || record.instance != request.instanceId
        || record.generation != request.generation || record.call != request.callId || record.scene != scene.GetRenderSceneGeneration()
        || record.entity != target.GetID() || record.object != std::bit_cast<uint64_t>(request.integer)) return ManagedBridgeStatus::INVALID_SCOPE;
    if (record.birth != birth) { m_impl->records.erase(found); return ManagedBridgeStatus::NOT_FOUND; }
    const auto invalid = [&] { m_impl->records.erase(found); return ManagedBridgeStatus::INVALID_ARGUMENT; };
    const uint32_t abort = line ? 53U : 54U;
    if (request.index == abort) {
        if (!values.empty() || !text.empty()) return invalid();
        m_impl->records.erase(found); result = {}; return ManagedBridgeStatus::OK;
    }
    if (request.index == 51) {
        uint64_t offset = 0;
        const size_t stride = line ? 3U : 15U;
        const size_t limit = line ? 126U : 120U;
        const size_t written = line ? record.points.size() : record.vertices.size();
        if (text.size() != 1 || !Offset(text[0], offset) || offset != written || values.empty() || values.size() > limit
            || values.size() % stride || offset > record.expectedVertices || values.size() / stride > record.expectedVertices - offset
            || std::any_of(values.begin(), values.end(), [](float value) { return !std::isfinite(value); })) return invalid();
        for (size_t index = 0; index < values.size(); index += stride) {
            if (line) record.points.push_back({values[index], values[index + 1], values[index + 2]});
            else {
                MeshVertex vertex;
                vertex.position = {values[index], values[index + 1], values[index + 2]};
                vertex.normal = {values[index + 3], values[index + 4], values[index + 5]};
                vertex.tangent = {values[index + 6], values[index + 7], values[index + 8]};
                vertex.uv = {values[index + 9], values[index + 10]};
                vertex.color = {values[index + 11], values[index + 12], values[index + 13], values[index + 14]};
                record.vertices.push_back(vertex);
            }
        }
        result = {}; result.integer = static_cast<int64_t>(line ? record.points.size() : record.vertices.size());
        return ManagedBridgeStatus::OK;
    }
    if (!line && request.index == 52) {
        uint64_t offset = 0;
        if (record.verticesOnly || !values.empty() || text.size() < 2 || text.size() > 128 || !Offset(text[0], offset)
            || offset != record.indices.size() || offset > record.expectedIndices || text.size() - 1 > record.expectedIndices - offset) return invalid();
        std::vector<uint32_t> page;
        page.reserve(text.size() - 1);
        for (size_t index = 1; index < text.size(); ++index) {
            uint64_t value = 0;
            if (!Offset(text[index], value) || value >= record.expectedVertices || value > UINT32_MAX) return invalid();
            page.push_back(static_cast<uint32_t>(value));
        }
        record.indices.insert(record.indices.end(), page.begin(), page.end());
        result = {}; result.integer = static_cast<int64_t>(record.indices.size());
        return ManagedBridgeStatus::OK;
    }
    const uint32_t commit = line ? 52U : 53U;
    if (request.index != commit || !values.empty() || !text.empty()) return invalid();
    if (line) {
        if (record.points.size() != record.expectedVertices) return invalid();
        auto* component = target.GetComponent<LineRendererComponent>();
        component->points = std::move(record.points);
        component->space = record.world ? LineSpace::World : LineSpace::Local;
        component->pointSourceMode = ControlPointSourceMode::INLINE;
        component->pointRuntimeMode = ControlPointRuntimeMode::LIVE;
    } else {
        if (record.vertices.size() != record.expectedVertices || record.indices.size() != record.expectedIndices) return invalid();
        auto* procedural = target.GetComponent<ProceduralMeshComponent>();
        if (record.verticesOnly) {
            if (!procedural || scene.GetComponentLifetime<ProceduralMeshComponent>(target.GetID()) != record.proceduralBirth
                || procedural->builder.VertexCount() != record.expectedVertices) return invalid();
            /// @note 頂点だけの更新でも既存 topology 全件を照合し、変更済みの不正な添字を GPU へ渡さない。
            const auto& topology = procedural->builder.Indices();
            if (topology.size() % 3 || std::any_of(topology.begin(), topology.end(), [&record](uint32_t index) { return index >= record.expectedVertices; })) return invalid();
            procedural->builder.Vertices() = std::move(record.vertices);
            if (procedural->dirty != MeshDirty::All) procedural->dirty = MeshDirty::Vertices;
        } else {
            if (!procedural) procedural = &target.AddComponent<ProceduralMeshComponent>();
            procedural->builder.Vertices() = std::move(record.vertices);
            procedural->builder.Indices() = std::move(record.indices);
            procedural->dirty = MeshDirty::All;
            procedural->enabled = true;
            auto* renderer = target.GetComponent<MeshRenderer>();
            renderer->meshPath.clear(); renderer->meshPathDirty = false; renderer->enabled = true;
        }
    }
    m_impl->records.erase(found); result = {}; result.integer = 1;
    return ManagedBridgeStatus::OK;
}
void ManagedRenderTransfers::CancelOwner(uint64_t owner)
{ std::erase_if(m_impl->records, [owner](const auto& entry) { return entry.second.owner == owner; }); }
void ManagedRenderTransfers::RetireGeneration(uint64_t generation)
{ std::erase_if(m_impl->records, [generation](const auto& entry) { return entry.second.generation == generation; }); }
void ManagedRenderTransfers::EndCall(uint64_t call)
{ std::erase_if(m_impl->records, [call](const auto& entry) { return entry.second.call == call; }); }
} /// @note namespace fbzz::scene::managed_detail
