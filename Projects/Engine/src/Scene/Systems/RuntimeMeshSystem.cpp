/// @file    RuntimeMeshSystem.cpp
/// @brief   手続きメッシュのアップロードと meshPath の再解決
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/Systems/RuntimeMeshSystem.hpp>

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
#include <memory>

namespace fbzz::scene {

namespace {

/// 容量は 2 の冪で取る。毎フレーム組み直す形 (伸び縮みするリボン) はぴったり確保すると
/// 1 頂点増えるたびにバッファを作り直すことになり、DoubleBufferedMesh の意味が消える。
uint32_t CapacityFor(uint32_t needed)
{
    uint32_t capacity = 256;
    while (capacity < needed) capacity *= 2;
    return capacity;
}

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

    /// @note 空にされた (mesh.Clear()) だけなら描く数を 0 にすれば足りる。GPU バッファは
    ///       次に組み直したとき使い回せるので、ここで解放するとかえって作り直しが増える。
    if (vertexCount == 0 || indexCount == 0) {
        mesh.cpuVertices.clear();
        mesh.cpuIndices.clear();
        mesh.vertexCount = 0;
        mesh.indexCount  = 0;
        mesh.ComputeBounds();
        return;
    }

    /// @note 2 枚を交互に使うので、今から書く側が前回の全更新を受けているとは限らない
    ///       (最初の 1 回や、直前が頂点だけの更新)。インデックスが今の三角形と食い違っていたら
    ///       頂点だけ上げても繋がり方が古いまま描かれるため、食い違いを見て全更新へ落とす。
    const bool topologyMatches = mesh.indexBuffer.IsValid() && mesh.indexCount == indexCount;
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

} // namespace

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
    }

    for (EntityID id : ctx.scene.GetEntities<ProceduralMeshComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* procedural = ctx.scene.GetComponent<ProceduralMeshComponent>(id);
        if (!go || !procedural) continue;

        if (procedural->dirty != MeshDirty::None) {
            UploadProceduralMesh(*procedural, resources);
            procedural->dirty = MeshDirty::None;
        }
        if (!procedural->runtimeMesh.HasMesh()) continue;

        auto* meshRenderer = go->GetComponent<MeshRenderer>();
        if (!meshRenderer) meshRenderer = &go->AddComponent<MeshRenderer>();
        meshRenderer->mesh    = procedural->runtimeMesh.Current();
        meshRenderer->enabled = procedural->enabled && procedural->builder.TriangleCount() > 0;

        if (procedural->materialPath.empty()
            || procedural->materialPath == procedural->appliedMaterialPath)
            continue;
        auto* material = go->GetComponent<MaterialComponent>();
        if (!material) material = &go->AddComponent<MaterialComponent>();
        material->enabled      = true;
        material->materialPath = procedural->materialPath;
        procedural->appliedMaterialPath = procedural->materialPath;
    }
}

} // namespace fbzz::scene
