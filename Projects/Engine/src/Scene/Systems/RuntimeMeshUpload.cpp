/// @file    RuntimeMeshUpload.cpp
/// @brief   生成メッシュの資源を作成元へ返し、有効な描画スロットだけを公開する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include "RuntimeMeshUpload.hpp"
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/SceneRenderResources.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Graphics/Renderer/OwnedResource.hpp>
#include <algorithm>
#include <limits>

namespace fbzz::scene {
namespace {
constexpr uint32_t CapacityFor(uint32_t needed)
{
    uint32_t capacity = 256;
    while (capacity < needed) {
        if (capacity > std::numeric_limits<uint32_t>::max() / 2) return needed;
        capacity *= 2;
    }
    return capacity;
}
static_assert(CapacityFor(257) == 512);
static_assert(CapacityFor(UINT32_MAX) == UINT32_MAX);

bool IsLive(renderer::ResourceManager& resources, renderer::ResourceHandle<renderer::BufferTag> handle)
{
    renderer::BufferResourceMetadata metadata;
    return resources.GetBufferMetadata(handle, metadata);
}
} /// @note namespace

bool PrepareRuntimeMesh(Scene& scene, EntityID id, DoubleBufferedMesh& target, renderer::ResourceManager& resources)
{
    for (const auto& mesh : target.slots) {
        if (!mesh) continue;
        if ((mesh->vertexBuffer.IsValid() && !IsLive(resources, mesh->vertexBuffer))
            || (mesh->indexBuffer.IsValid() && !IsLive(resources, mesh->indexBuffer))) {
            UnlinkRuntimeMesh(scene, id, target);
            target.Reset();
            return true;
        }
    }
    return false;
}

bool UploadRuntimeMesh(DoubleBufferedMesh& target, renderer::ResourceManager& resources,
    std::span<const renderer::Vertex> vertices, std::span<const uint32_t> indices, MeshDirty dirty)
{
    if (vertices.size() > UINT32_MAX || indices.size() > UINT32_MAX) return false;
    const uint32_t next = target.current ^ 1u;
    auto& slot = target.slots[next];
    if (!slot) slot = std::make_unique<renderer::Mesh>();
    auto& mesh = *slot;
    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    const auto indexCount = static_cast<uint32_t>(indices.size());
    if (vertices.empty() || indices.empty()) {
        mesh.cpuVertices.clear(); mesh.cpuIndices.clear();
        mesh.vertexCount = mesh.indexCount = 0;
        mesh.ComputeBounds();
        target.current = next;
        return true;
    }

    const bool vertexLive = IsLive(resources, mesh.vertexBuffer);
    const bool indexLive = IsLive(resources, mesh.indexBuffer);
    const bool uploadIndices = dirty == MeshDirty::All || !indexLive
        || !std::equal(indices.begin(), indices.end(), mesh.cpuIndices.begin(), mesh.cpuIndices.end());
    renderer::OwnedResource<renderer::BufferTag> vertexReplacement, indexReplacement;
    const uint32_t vertexCapacity = vertexLive && mesh.vertexCapacity >= vertexCount ? mesh.vertexCapacity : CapacityFor(vertexCount);
    const uint32_t indexCapacity = indexLive && mesh.indexCapacity >= indexCount ? mesh.indexCapacity : CapacityFor(indexCount);
    if (!vertexLive || mesh.vertexCapacity < vertexCount) {
        vertexReplacement = resources.CreateVertexBuffer(nullptr, static_cast<size_t>(vertexCapacity) * sizeof(renderer::Vertex), sizeof(renderer::Vertex));
        if (!vertexReplacement.IsValid()) return false;
    }
    if (!indexLive || mesh.indexCapacity < indexCount) {
        indexReplacement = resources.CreateIndexBuffer(nullptr, indexCapacity);
        if (!indexReplacement.IsValid()) return false;
    }
    /// @note 新しい両バッファの確保が済むまで旧スロットを変更せず、失敗した候補は単独所有で返す。
    if (vertexReplacement.IsValid()) {
        renderer::ReleaseOwnedResource(mesh.vertexBuffer);
        mesh.vertexBuffer = vertexReplacement.Release();
    }
    if (indexReplacement.IsValid()) {
        renderer::ReleaseOwnedResource(mesh.indexBuffer);
        mesh.indexBuffer = indexReplacement.Release();
    }
    mesh.vertexCapacity = vertexCapacity; mesh.indexCapacity = indexCapacity;
    mesh.cpuVertices.assign(vertices.begin(), vertices.end());
    mesh.vertexCount = vertexCount;
    if (uploadIndices) {
        mesh.cpuIndices.assign(indices.begin(), indices.end());
        mesh.indexCount = indexCount;
    }
    mesh.ComputeBounds();
    resources.Update(mesh.vertexBuffer, mesh.cpuVertices.data(), vertices.size_bytes());
    if (uploadIndices) resources.Update(mesh.indexBuffer, mesh.cpuIndices.data(), indices.size_bytes());
    target.current = next;
    return true;
}
} /// @note namespace fbzz::scene
