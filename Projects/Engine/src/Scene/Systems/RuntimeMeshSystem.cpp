/// @file    RuntimeMeshSystem.cpp
/// @brief   手続きメッシュのアップロードと meshPath の再解決
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/Systems/RuntimeMeshSystem.hpp>
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>

#include <Engine/Core/Memory/MakeUnique.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Scene.hpp>

#include <cstdint>
#include <limits>
#include <memory>

namespace fbzz::scene {

namespace {

/// @note 頻繁な再確保を避けるため2の冪へ切り上げ、uint32で次の冪を表せないときは実件数を使う。
constexpr uint32_t CapacityFor(uint32_t needed)
{
    uint32_t capacity = 256;
    while (capacity < needed) {
        if (capacity > std::numeric_limits<uint32_t>::max() / 2) return needed;
        capacity *= 2;
    }
    return capacity;
}
static_assert(CapacityFor(0) == 256 && CapacityFor(257) == 512);
static_assert(CapacityFor(0x80000000U) == 0x80000000U && CapacityFor(0x80000001U) == 0x80000001U);
static_assert(CapacityFor(std::numeric_limits<uint32_t>::max()) == std::numeric_limits<uint32_t>::max());

void UploadProceduralMesh(ProceduralMeshComponent& component,
                          renderer::ResourceManager& resources)
{
    DoubleBufferedMesh& target = component.runtimeMesh;
    target.current ^= 1u;
    std::unique_ptr<renderer::Mesh>& slot = target.slots[target.current];
    if (!slot) slot = core::MakeUnique<renderer::Mesh>();
    if (!slot) return;
    renderer::Mesh& mesh = *slot;

    const MeshBuilder& builder = component.builder;
    const uint32_t vertexCount = builder.VertexCount();
    const uint32_t indexCount  = builder.IndexCount();

    /// @note 空の形は描く件数だけを0にし、次の更新で再利用するGPU bufferを保持する。
    if (vertexCount == 0 || indexCount == 0) {
        mesh.cpuVertices.clear();
        mesh.cpuIndices.clear();
        mesh.vertexCount = 0;
        mesh.indexCount  = 0;
        mesh.ComputeBounds();
        return;
    }

    /// @note 二枚交互のbufferは前回の全更新を共有しないため、topologyが一致しないslotへはindexもuploadする。
    const bool topologyMatches = mesh.indexBuffer.IsValid() && mesh.indexCount == indexCount
        && mesh.cpuIndices == builder.Indices();
    const bool uploadIndices =
        component.dirty == MeshDirty::All || !topologyMatches;

    mesh.cpuVertices = builder.Vertices();
    mesh.vertexCount = vertexCount;
    if (uploadIndices) {
        mesh.cpuIndices = builder.Indices();
        mesh.indexCount = indexCount;
    }
    mesh.ComputeBounds();

    if (!mesh.vertexBuffer.IsValid() || mesh.vertexCapacity < vertexCount) {
        if (mesh.vertexBuffer.IsValid()) resources.Release(mesh.vertexBuffer);
        mesh.vertexCapacity = CapacityFor(vertexCount);
        mesh.vertexBuffer   = resources.CreateVertexBuffer(
            nullptr, static_cast<size_t>(mesh.vertexCapacity) * sizeof(renderer::Vertex),
            sizeof(renderer::Vertex));
    }
    if (uploadIndices && (!mesh.indexBuffer.IsValid() || mesh.indexCapacity < indexCount)) {
        if (mesh.indexBuffer.IsValid()) resources.Release(mesh.indexBuffer);
        mesh.indexCapacity = CapacityFor(indexCount);
        mesh.indexBuffer   = resources.CreateIndexBuffer(nullptr, mesh.indexCapacity);
    }

    if (vertexCount > 0)
        resources.Update(mesh.vertexBuffer, mesh.cpuVertices.data(),
                         static_cast<size_t>(vertexCount) * sizeof(renderer::Vertex));
    if (uploadIndices && indexCount > 0)
        resources.Update(mesh.indexBuffer, mesh.cpuIndices.data(),
                         static_cast<size_t>(indexCount) * sizeof(uint32_t));
}

} /// @note namespace

ComponentAccess RuntimeMeshSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }

void RuntimeMeshSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources) return;
    renderer::ResourceManager& resources = *ctx.resources;

    for (EntityID id : ctx.scene.GetEntities<MeshRenderer>()) {
        auto* meshRenderer = ctx.scene.GetComponent<MeshRenderer>(id);
        if (!meshRenderer || !meshRenderer->meshPathDirty) continue;
        meshRenderer->meshPathDirty = false;
        meshRenderer->mesh = ResolveMeshPath(meshRenderer->meshPath, resources);
        NotifyMeshRendererChanged(ctx.scene, id);
    }

    for (EntityID id : ctx.scene.GetEntities<ProceduralMeshComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* procedural = ctx.scene.GetComponent<ProceduralMeshComponent>(id);
        if (!go || !procedural) continue;

        const bool uploaded = procedural->dirty != MeshDirty::None;
        if (uploaded) {
            UploadProceduralMesh(*procedural, resources);
            procedural->dirty = MeshDirty::None;
        }
        if (!procedural->runtimeMesh.HasMesh()) continue;

        auto* meshRenderer = go->GetComponent<MeshRenderer>();
        if (!meshRenderer) meshRenderer = &go->AddComponent<MeshRenderer>();
        auto* currentMesh = procedural->enabled && procedural->builder.TriangleCount() > 0
            ? procedural->runtimeMesh.Current() : nullptr;
        if (uploaded || meshRenderer->mesh != currentMesh) {
            meshRenderer->mesh = currentMesh;
            NotifyMeshRendererChanged(ctx.scene, id);
        }

        if (procedural->materialPath.empty()
            || procedural->materialPath == procedural->appliedMaterialPath)
            continue;
        auto* material = go->GetComponent<MaterialComponent>();
        if (!material) material = &go->AddComponent<MaterialComponent>();
        /// @note 手続きの材質は既定割当。明示した共有割当と Material.Enabled を差分反映で戻さない。
        if (!material->Authoring().hasPresentationMaterialOverride)
            material->SetMaterialPath(procedural->materialPath);
        procedural->appliedMaterialPath = procedural->materialPath;
    }
}

} /// @note namespace fbzz::scene
