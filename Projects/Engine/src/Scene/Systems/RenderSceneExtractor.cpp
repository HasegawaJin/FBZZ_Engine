/// @file    RenderSceneExtractor.cpp
/// @brief   更新済み Scene のメッシュ・姿勢・材質・GPU 資源を描画入力へ固定する。
/// @author  Hasegawa Jin
/// @date    2026-09-20
#include <Engine/Scene/Systems/RenderSceneExtractor.hpp>
#include <Engine/Scene/Systems/RenderCustomPostExtractor.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Systems/RenderPasses/GeometryRoute.hpp>
#include <Engine/Renderer/IShader.hpp>
#include <Engine/Renderer/IBuffer.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Systems/RenderEnvironmentExtractor.hpp>
#include <Engine/Scene/Systems/RenderTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderDecalExtractor.hpp>
#include <Engine/Scene/Systems/RenderTerrainExtractor.hpp>
#include <Engine/Scene/Systems/RenderWaterExtractor.hpp>
#include <Engine/Scene/Systems/RenderFiberExtractor.hpp>
#include <Engine/Scene/Systems/RenderMeshTrailExtractor.hpp>
#include <Engine/Scene/Systems/RenderParticleExtractor.hpp>
#include "RenderPasses/Geometry/GeometryPasses.hpp"
#include "RenderPasses/Debug/SelectionPasses.hpp"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <unordered_map>

namespace fbzz::scene {
namespace {

std::atomic<uint64_t> s_renderSceneSerial{ 0 };

uint64_t EntityKey(EntityID id)
{
    return (static_cast<uint64_t>(id.index) << 32) | id.generation;
}

struct RayLodSelection {
    bool visible = false;
    bool ambiguous = false;
    uint64_t group = 0;
};

/// @note LOD0 は全ビュー共通の正本。GUID fallback は Raster と同じで、カメラ・crossfade は参照しない。
std::unordered_map<uint64_t, RayLodSelection> CollectRayLodRenderers(Scene& scene,
    std::vector<renderer::RenderRayLodDiagnostic>& diagnostics)
{
    std::unordered_map<uint64_t, RayLodSelection> result;
    for (EntityID id : scene.GetEntities<LODGroupComponent>()) {
        const auto* group = scene.GetComponent<LODGroupComponent>(id);
        const auto* owner = scene.GetGameObject(id);
        if (!group || !group->enabled || !owner || !owner->activeInHierarchy()) continue;
        bool canonicalMissing = false;
        for (size_t levelIndex = 0; levelIndex < group->levels.size(); ++levelIndex) {
            const auto& level = group->levels[levelIndex];
            for (const auto& reference : level.renderers) {
                EntityID renderer = reference.entity;
                if (!scene.IsValid(renderer) && !reference.instanceId.empty()) {
                    if (const auto* resolved = scene.FindByGuid(reference.instanceId))
                        renderer = resolved->GetID();
                }
                if (!scene.IsValid(renderer)) {
                    canonicalMissing |= levelIndex == 0;
                    continue;
                }
                const auto* target = scene.GetGameObject(renderer);
                canonicalMissing |= levelIndex == 0 && (!target
                    || (!scene.GetComponent<MeshRenderer>(renderer) && !scene.GetComponent<SkinnedMeshRenderer>(renderer)));
                const auto [found, inserted] = result.emplace(EntityKey(renderer),
                    RayLodSelection{levelIndex == 0, false, EntityKey(id)});
                if (!inserted) {
                    found->second.visible |= levelIndex == 0;
                    found->second.ambiguous |= found->second.group != EntityKey(id);
                }
            }
        }
        /// @note 未解決の canonical renderer の layer は証明できず、下位 LOD の layer で除外しない。
        if (canonicalMissing)
            diagnostics.push_back({id.index, id.generation, UINT32_MAX});
    }
    return result;
}

template<typename T>
math::Matrix4 AdvanceWorld(T& component, const math::Matrix4& world, uint64_t frame)
{
    if (component.prevWorldFrame != frame) {
        component.prevWorldMatrix = frame > 0 && component.prevWorldFrame == frame - 1
            ? component.worldMatrixSnapshot : world;
        component.worldMatrixSnapshot = world;
        component.prevWorldFrame = frame;
    }
    return component.prevWorldMatrix;
}

template<typename T>
renderer::RenderObject ExtractObject(GameObject& go, T& component, uint64_t frame)
{
    renderer::RenderObject object;
    object.sourceIndex = go.GetID().index;
    object.sourceGeneration = go.GetID().generation;
    object.layer = static_cast<uint32_t>(go.layer);
    object.world = go.transform.GetWorldMatrix();
    object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
    object.previousWorld = AdvanceWorld(component, object.world, frame);
    object.sortPosition = go.transform.position;
    object.castShadows = component.castShadows;
    object.lodVisible = component.lodVisible;
    object.lodDither = component.lodDither;
    return object;
}

renderer::RenderMeshItem ExtractMesh(GameObject& go, const renderer::Mesh& mesh)
{
    renderer::RenderMeshItem item;
    item.vertexBuffer = mesh.vertexBuffer;
    item.skinningVertexBuffer = mesh.vertexBuffer;
    item.indexBuffer = mesh.indexBuffer;
    item.indexCount = mesh.indexCount;
    item.vertexCount = mesh.vertexCount;
    item.vertexStride = mesh.isSkinned ? sizeof(renderer::SkinnedVertex) : sizeof(renderer::Vertex);
    item.reliableOccluder = IsReliableOccluder(mesh);
    const auto bounds = ComputeWorldBounds(go.transform, mesh);
    item.boundsCenter = bounds.center;
    item.boundsRadius = mesh.boundsRadius > 0.0f ? bounds.radius : 0.0f;
    return item;
}

void CopyMaterial(renderer::RenderMaterial& output, const renderer::Material* material,
                  renderer::ResourceManager& resources)
{
    output.valid = material != nullptr;
    if (!material) return;
    output.shader = material->shader;
    output.paramsBuffer = material->paramsBuffer;
    for (size_t i = 0; i < output.textures.size() && i < material->textures.size(); ++i)
        output.textures[i] = material->textures[i];
    output.directGBufferParams = material->paramData.size() == output.gbufferParams.size();
    if (output.directGBufferParams) {
        std::memcpy(output.gbufferParams.data(), material->paramData.data(), output.gbufferParams.size());
        return;
    }
    /// @note 異なるシェーダーの CB レイアウトを GBuffer の 96 バイトへ正規化する。
    const float roughness = 0.5f;
    const float tiling[] = { 1.0f, 1.0f };
    std::memcpy(output.gbufferParams.data() + 20, &roughness, sizeof(roughness));
    std::memcpy(output.gbufferParams.data() + 48, tiling, sizeof(tiling));
    const auto* shader = resources.Get(material->shader);
    if (!shader) return;
    const auto& descriptor = shader->GetDescriptor();
    auto copy = [&](std::string_view name, uint32_t offset, uint32_t bytes) {
        const auto* variable = descriptor.FindVar(name);
        if (variable && variable->offset + bytes <= material->paramData.size())
            std::memcpy(output.gbufferParams.data() + offset,
                        material->paramData.data() + variable->offset, bytes);
    };
    copy("albedo", 0, 16);
    copy("metallic", 16, 4);
    copy("roughness", 20, 4);
    copy("normalStrength", 24, 4);
    copy("occlusionStrength", 28, 4);
    copy("emissiveColor", 32, 12);
    copy("emissiveScale", 44, 4);
    copy("uvTiling", 48, 8);
    copy("uvOffset", 56, 8);
    copy("alphaCutoff", 64, 4);
    if (descriptor.textureMaskOffset != UINT32_MAX &&
        descriptor.textureMaskOffset + 4 <= material->paramData.size())
        std::memcpy(output.gbufferParams.data() + 80,
                    material->paramData.data() + descriptor.textureMaskOffset, 4);
}

void ExtractRayMaterialInputs(renderer::RenderMaterial& material,
    const renderer::Material* source, const MaterialSlot& slot, renderer::ResourceManager& resources,
    bool currentDeformationVerified)
{
    std::string path = source ? source->shaderPath : std::string{};
    if (path.starts_with("guid:")) path = asset::AssetManager::ResolveAssetPath(path);
    std::replace(path.begin(), path.end(), '\\', '/');
    /// @note Raster の basename 分類ではなく、検証した標準 VS の配置だけを geometry の根拠にする。
    const bool surfacePbr = path.ends_with("Assets/Shaders/Material/Surface/PBR.hlsl") || path == "Material/Surface/PBR.hlsl";
    const bool skinnedPbr = path.ends_with("Assets/Shaders/Material/Skinned/SkinnedPBR.hlsl")
        || path == "Material/Skinned/SkinnedPBR.hlsl";
    /// @note SkinnedPBR の PS だけを共有し、現在版の 60-byte 変形済み入力なしで任意の skinning VS を受理しない。
    const bool pbr = surfacePbr || (skinnedPbr && currentDeformationVerified);
    const bool unclipped = path.ends_with("Assets/Shaders/Material/Surface/Lit.hlsl")
        || path.ends_with("Assets/Shaders/Material/Surface/Fallback.hlsl")
        || path == "Material/Surface/Lit.hlsl" || path == "Material/Surface/Fallback.hlsl";
    const auto* shader = resources.Get(material.shader);
    const bool standardGeometry = material.valid && shader && (pbr || unclipped);
    /// @note Lit / Fallback は texture alpha に関係なく alpha=1 を返し、clip を実行しない。
    float alpha = 0, cutoff = 0;
    uint32_t textureMask = 0;
    std::memcpy(&alpha, material.gbufferParams.data() + 12, sizeof(alpha));
    std::memcpy(&cutoff, material.gbufferParams.data() + 64, sizeof(cutoff));
    std::memcpy(&textureMask, material.gbufferParams.data() + 80, sizeof(textureMask));
    material.rayCapabilities = standardGeometry && unclipped
        ? renderer::RayMaterialCapabilities{true, renderer::RayOpacity::OPAQUE_SURFACE}
        : renderer::ResolveStaticRayMaterialCapabilities(standardGeometry,
            material.capabilities.blend, alpha, cutoff, (textureMask & 1u) != 0);
    uint32_t declaredTextureMask = 0;
    bool unresolvedTexture = false;
    const auto declareTexture = [&](const std::string& name, const std::string& texturePath) {
        if (texturePath.empty()) return;
        for (uint32_t i = 0; i < renderer::kMaterialTextureSlotCount; ++i) {
            if (name != renderer::kMaterialTextureSlots[i].key) continue;
            declaredTextureMask |= 1u << i;
            return;
        }
        unresolvedTexture = true;
    };
    const auto* materialAsset = asset::AssetManager::Get<asset::MaterialAsset>(slot.materialAsset);
    /// @note 未ロードや無効な GPU handle でも、元 .mat の texture 指定を定数材質と誤認しない。
    if (materialAsset) {
        for (const auto& [name, texturePath] : materialAsset->textures) {
            const auto replacement = slot.textureOverrides.find(name);
            declareTexture(name, replacement != slot.textureOverrides.end() ? replacement->second : texturePath);
        }
    }
    for (const auto& [name, texturePath] : slot.textureOverrides)
        declareTexture(name, texturePath);
    bool advancedLobes = false;
    if (pbr && shader && source) {
        /// @note GBuffer の 96 bytes 外にある追加ローブを、定数 metallic / roughness と誤認しない。
        for (const auto* name : {"clearcoat", "sheen", "anisotropy"}) {
            const auto* variable = shader->GetDescriptor().FindVar(name);
            if (!variable) continue;
            if (variable->offset > source->paramData.size()
                || sizeof(float) > source->paramData.size() - variable->offset) {
                advancedLobes = true;
                continue;
            }
            float amount = 0;
            std::memcpy(&amount, source->paramData.data() + variable->offset, sizeof(amount));
            advancedLobes |= !std::isfinite(amount) || amount != 0;
        }
    }
    material.surface = source ? source->ResolveRaySurface(material.gbufferParams, standardGeometry && pbr,
        declaredTextureMask, unresolvedTexture, advancedLobes, resources)
        : renderer::ResolveConstantSurfaceMaterial(material.gbufferParams, false);
    /// @note Surface resolution includes authored-but-not-yet-uploaded textures; never share an opaque BLAS variant for them.
    if (!unclipped)
        material.rayCapabilities = renderer::ResolveStaticRayMaterialCapabilities(standardGeometry,
            material.capabilities.blend, alpha, cutoff, (material.surface.textureMask & 1u) != 0);
}

} /// @note namespace

renderer::RenderScene ExtractRenderSceneGeometry(Scene& scene, uint64_t frameStamp,
    renderer::ResourceHandle<renderer::ConstantBufferTag> identityPalette)
{
    renderer::RenderScene output;
    output.snapshotSerial = ++s_renderSceneSerial;
    output.sceneGeneration = scene.GetRenderSceneGeneration();
    output.frameStamp = frameStamp;
    const auto lodRenderers = CollectRayLodRenderers(scene, output.rayLodDiagnostics);
    for (auto& go : scene.GameObjects()) {
        if (!go.activeInHierarchy()) continue;
        if (auto* mr = go.GetComponent<MeshRenderer>(); mr && mr->enabled && mr->mesh && !mr->mesh->isSkinned) {
            auto object = ExtractObject(go, *mr, frameStamp);
            if (const auto lod = lodRenderers.find(EntityKey(go.GetID())); lod != lodRenderers.end()) {
                object.rayVisible = lod->second.visible;
                object.rayLodSelectionRequired = lod->second.ambiguous;
            }
            auto item = ExtractMesh(go, *mr->mesh);
            object.boundsCenter = item.boundsCenter;
            object.boundsRadius = item.boundsRadius;
            object.firstItem = static_cast<uint32_t>(output.items.size());
            object.itemCount = 1;
            item.objectIndex = static_cast<uint32_t>(output.objects.size());
            output.items.push_back(std::move(item));
            output.objects.push_back(object);
        }
        auto* smr = go.GetComponent<SkinnedMeshRenderer>();
        if (!smr || !smr->enabled || !smr->model) continue;
        auto object = ExtractObject(go, *smr, frameStamp);
        if (const auto lod = lodRenderers.find(EntityKey(go.GetID())); lod != lodRenderers.end()) {
            object.rayVisible = lod->second.visible;
            object.rayLodSelectionRequired = lod->second.ambiguous;
        }
        object.skinned = true;
        object.firstItem = static_cast<uint32_t>(output.items.size());
        WorldBounds bounds{};
        if (ComputeSkinnedWorldBounds(go, *smr, bounds)) {
            object.boundsCenter = bounds.center;
            object.boundsRadius = bounds.radius;
        }
        const auto* animator = FindAnimator(go);
        object.skinningPalette = ResolveSkinningCB(animator ? animator->skinningBuffer
            : renderer::ResourceHandle<renderer::ConstantBufferTag>{}, smr->model, identityPalette);
        object.previousSkinningValid = animator && animator->prevBoneMatricesValid &&
            animator->skinningBuffer.IsValid() && animator->prevSkinningBuffer.IsValid();
        object.previousSkinningPalette = object.previousSkinningValid
            ? animator->prevSkinningBuffer : object.skinningPalette;
        for (size_t slot = 0; slot < smr->SubmeshCount(); ++slot) {
            const auto* mesh = smr->SubmeshMesh(slot);
            if (!mesh) continue;
            auto item = ExtractMesh(go, *mesh);
            item.objectIndex = static_cast<uint32_t>(output.objects.size());
            item.materialSlot = static_cast<uint32_t>(slot);
            item.sourceSubmesh = smr->SubmeshAt(slot);
            item.skinningVertexBuffer = smr->ResolveSlotVertexBuffer(slot, mesh->vertexBuffer);
            /// @note 古いフレームやモーフ対象に残った GPU 出力を再利用しない。
            if (smr->gpuSkinningFrame == frameStamp && smr->skinnedBufferModel == smr->model)
                item.deformedVertexBuffer = smr->ResolveSlotSkinnedVertexBuffer(slot);
            output.items.push_back(std::move(item));
        }
        object.itemCount = static_cast<uint32_t>(output.items.size()) - object.firstItem;
        output.objects.push_back(object);
    }
    for (auto& object : output.objects) {
        auto* go = scene.GetGameObject({ object.sourceIndex, object.sourceGeneration });
        const auto* component = go ? go->GetComponent<MaterialComponent>() : nullptr;
        if (!component) continue;
        object.colorEligible = component->materialAsset.IsValid();
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            auto& item = output.items[i];
            const auto& slot = component->SlotAt(item.materialSlot);
            item.slotVisible = slot.visible;
            item.visible = component->enabled && slot.visible;
            item.material.capabilities = ExtractGeometryMaterial(slot);
        }
    }
    return output;
}

float EstimateRenderTexturePixels(const renderer::RenderObject& object,
    const renderer::RenderMeshItem& item, const math::Vector3& cameraPosition,
    float projectionScaleY, uint32_t height, bool orthographic)
{
    const auto center = object.skinned ? object.boundsCenter : item.boundsCenter;
    const float radius = object.skinned ? object.boundsRadius : item.boundsRadius;
    if (!(projectionScaleY > 0.0f) || !(radius > 0.0f) || height == 0) return 0.0f;
    const float distance = (center - cameraPosition).Length();
    if (!orthographic && distance <= radius) return static_cast<float>(height);
    return radius * projectionScaleY * static_cast<float>(height) / (orthographic ? 1.0f : distance);
}

void ExtractRenderScene(RenderPassContext& ctx, RenderFrameGeometryCache* frameGeometry)
{
    ctx.frameStamp = ctx.resources.FrameStamp();
    ctx.time = Time::time;
    ctx.deltaTime = Time::deltaTime;
    ctx.unscaledDeltaTime = Time::unscaledDeltaTime;

    std::shared_ptr<renderer::RenderScene> output;
    const uint64_t resetVersion = ctx.resources.GetResetVersion();
    if (frameGeometry && frameGeometry->geometry && frameGeometry->scene == &ctx.scene &&
        frameGeometry->geometry->sceneGeneration == ctx.scene.GetRenderSceneGeneration() &&
        frameGeometry->resources == &ctx.resources && frameGeometry->frameStamp == Time::frameCount &&
        frameGeometry->resetVersion == resetVersion &&
        frameGeometry->identityPalette == ctx.handles.bindPoseSkinningCB) {
        output = std::make_shared<renderer::RenderScene>(*frameGeometry->geometry);
        output->snapshotSerial = ++s_renderSceneSerial;
    } else {
        output = std::make_shared<renderer::RenderScene>(
            ExtractRenderSceneGeometry(ctx.scene, Time::frameCount, ctx.handles.bindPoseSkinningCB));
        if (frameGeometry) {
            frameGeometry->scene = &ctx.scene;
            frameGeometry->resources = &ctx.resources;
            frameGeometry->frameStamp = Time::frameCount;
            frameGeometry->resetVersion = resetVersion;
            frameGeometry->identityPalette = ctx.handles.bindPoseSkinningCB;
            frameGeometry->geometry = std::make_shared<renderer::RenderScene>(*output);
        }
    }
    /// @note 後から開いた Ray view が LOD0 を追加変形しても、最初の Raster view の snapshot に閉じ込めない。
    for (const auto& object : output->objects) {
        if (!object.skinned) continue;
        auto* go = ctx.scene.GetGameObject({object.sourceIndex, object.sourceGeneration});
        const auto* smr = go ? go->GetComponent<SkinnedMeshRenderer>() : nullptr;
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            auto& item = output->items[i];
            item.deformedVertexBuffer = smr && smr->gpuSkinningFrame == Time::frameCount
                && smr->skinnedBufferModel == smr->model ? smr->ResolveSlotSkinnedVertexBuffer(item.materialSlot)
                : renderer::ResourceHandle<renderer::BufferTag>{};
        }
    }
    for (auto& item : output->items) {
        const auto* vertices = ctx.resources.Get(item.vertexBuffer);
        const auto* indices = ctx.resources.Get(item.indexBuffer);
        const auto* deformed = ctx.resources.Get(item.deformedVertexBuffer);
        item.deformedContentVersion = deformed ? deformed->GetContentVersion() : 0;
        item.vertexContentVersion = vertices ? vertices->GetContentVersion() : 0;
        item.indexContentVersion = indices ? indices->GetContentVersion() : 0;
        if (vertices) item.vertexStride = vertices->GetStride();
    }
    for (auto& object : output->objects) {
        auto* go = ctx.scene.GetGameObject({ object.sourceIndex, object.sourceGeneration });
        object.selected = go && IsSelectedForOutline(*go, ctx.settings);
        auto* component = go ? go->GetComponent<MaterialComponent>() : nullptr;
        if (!component) continue;
        object.colorEligible = component->EnsureMaterialAsset();
        for (uint32_t i = object.firstItem; i < object.firstItem + object.itemCount; ++i) {
            auto& item = output->items[i];
            auto& slot = component->SlotAt(item.materialSlot);
            item.slotVisible = slot.visible;
            item.visible = component->enabled && slot.visible;
            slot.EnsureMaterialAsset();
            auto& material = item.material;
            material.capabilities = ExtractGeometryMaterial(slot);
            material.renderQueue = slot.GetRenderQueue();
            material.doubleSided = slot.IsDoubleSided();
            material.depthBias = slot.GetDepthBias();
            material.depthBiasSlope = slot.GetDepthBiasSlope();
            if (!object.colorEligible || !item.visible) continue;
            float screenPixels = 0.0f;
            const bool projectedTextureRequest = !ctx.isDeferred ||
                renderer::ResolveGeometryRoute(material.capabilities, true) == renderer::GeometryRoute::GBuffer;
            /// @note スキンドメッシュはアニメーション姿勢を含む物体全体の境界を使い、参照姿勢の submesh 境界へ戻さない。
            if (projectedTextureRequest)
                screenPixels = EstimateRenderTexturePixels(object, item, ctx.camera.m_position,
                    ctx.cullProjScaleY, ctx.height, ctx.cullOrthographic);
            const auto* source = SyncMaterialSlot(*component, item.materialSlot, ctx.resources,
                                                   object.skinned, screenPixels);
            CopyMaterial(material, source, ctx.resources);
            const auto* deformed = ctx.resources.Get(item.deformedVertexBuffer);
            const bool currentDeformationVerified = object.skinned && item.deformedContentVersion != 0
                && deformed && deformed->GetStride() == sizeof(renderer::Vertex)
                && deformed->GetContentVersion() == item.deformedContentVersion;
            ExtractRayMaterialInputs(material, source, slot, ctx.resources, currentDeformationVerified);
            item.forwardMaterial = material;
            if (object.skinned && source && IsSurfaceMaterial(slot)) {
                LogSkinnedSurfaceFallbackWarningOnce(source->shaderPath);
                CopyMaterial(item.forwardMaterial, GetFallbackMaterial(ctx.resources, true), ctx.resources);
            }
        }
    }
    for (const auto& request : ctx.settings.objectMaskRequests) {
        if (!renderer::IsObjectMaskRequestLive(request, output->frameStamp)) continue;
        ++output->liveMaskRequests;
        auto* root = ctx.scene.GetGameObject({ request.id.index, request.id.generation });
        if (!root) continue;
        renderer::RenderMaskGroup group;
        group.payload = { request.color[0], request.color[1], request.color[2], std::clamp(request.value, 0.02f, 1.0f) };
        group.visibleOnly = request.visibleOnly;
        const auto collect = [&](const auto& self, GameObject& go) -> void {
            if (!ShouldRenderGameObject(go, ctx.cullingMask)) return;
            for (uint32_t i = 0; i < output->objects.size(); ++i) {
                const auto& object = output->objects[i];
                if (object.sourceIndex == go.GetID().index && object.sourceGeneration == go.GetID().generation)
                    group.objectIndices.push_back(i);
            }
            if (request.includeChildren)
                for (int i = 0; i < go.GetChildCount(); ++i)
                    if (auto* child = go.GetChild(i)) self(self, *child);
        };
        collect(collect, *root);
        output->maskGroups.push_back(std::move(group));
    }
    ExtractRenderCustomPost(ctx, *output);
    ExtractRenderTrails(ctx, *output);
    ExtractRenderDecals(ctx, *output);
    ExtractRenderTerrains(ctx, *output);
    ExtractRenderWater(ctx, *output);
    ExtractRenderFibers(ctx, *output);
    ExtractRenderParticles(ctx, *output);
    ExtractRenderMeshTrails(ctx, *output);
    ctx.environment = ExtractRenderEnvironment(ctx);
    output->environment = ctx.environment;
    output->lighting = static_cast<const renderer::RenderLightingInput&>(ctx);
    ctx.renderScene = std::move(output);
}

} /// @note namespace fbzz::scene
