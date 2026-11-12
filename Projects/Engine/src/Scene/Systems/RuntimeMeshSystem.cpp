/// @file    RuntimeMeshSystem.cpp
/// @brief   手続きメッシュのアップロードと meshPath の再解決
/// @author  Hasegawa Jin
/// @date    2026-08-26
#include <Engine/Scene/Systems/RuntimeMeshSystem.hpp>
#include "RuntimeMeshUpload.hpp"
#include "WaterfallRuntime.hpp"
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>

#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ProceduralMeshComponent.hpp>
#include <Engine/Scene/Components/WaterfallComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/MeshResolver.hpp>
#include <Engine/Scene/Scene.hpp>

#include <cstdint>

namespace fbzz::scene {

ComponentAccess RuntimeMeshSystem::GetAccess() const { return ComponentAccess{}.Unrestricted(); }

void RuntimeMeshSystem::Update(SystemContext& ctx)
{
    if (!ctx.resources) return;
    renderer::ResourceManager& resources = *ctx.resources;
    UpdateWaterfallRuntime(ctx.scene, ctx.playing && !ctx.simulating ? 0.0f : ctx.dt);

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

        if (PrepareRuntimeMesh(ctx.scene, id, procedural->runtimeMesh, resources)) procedural->dirty = MeshDirty::All;
        const bool uploaded = procedural->dirty != MeshDirty::None;
        if (uploaded) {
            auto* waterfall = go->GetComponent<WaterfallPart>();
            if (!UploadRuntimeMesh(procedural->runtimeMesh, resources, procedural->builder.Vertices(),
                    procedural->builder.Indices(), procedural->dirty)) {
                if (waterfall) {
                    if (auto* mesh = go->GetComponent<MeshRenderer>(); mesh && mesh->mesh) {
                        mesh->mesh = nullptr;
                        NotifyMeshRendererChanged(ctx.scene, id);
                    }
                }
                continue;
            }
            if (waterfall) {
                waterfall->farIndexStart = waterfall->pendingFarIndexStart;
                procedural->runtimeMesh.Current()->boundsRadius += WATERFALL_VERTEX_MARGIN;
            }
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
