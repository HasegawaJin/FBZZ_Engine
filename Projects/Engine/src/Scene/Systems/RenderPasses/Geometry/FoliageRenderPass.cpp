// FBZZ Engine
// RenderPasses/Geometry/FoliageRenderPass.cpp | fbzz::scene
// 樹木・大型岩向けの複数SubMesh/Material対応GPU Instancing描画 (IRenderPass 実装)
#include "Engine/Scene/Systems/RenderPasses/Geometry/FoliageRenderPass.hpp"
#include "Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Components/FoliageComponent.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/MaterialAsset.hpp"
#include "Engine/Asset/Model.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include <Math/Matrix4.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fbzz::scene {

namespace {

struct FoliageMaterialCB {
    float baseColor[4];
    uint32_t hasAlbedo;
    float alphaCutoff;
    float _pad[2];
};
static_assert(sizeof(FoliageMaterialCB) == 32, "FoliageMaterialCB size mismatch");

float Random01(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
}

math::Vector3 TransformPoint(const math::Matrix4& matrix, float x, float y, float z)
{
    const math::Vector4 point = matrix * math::Vector4{ x, y, z, 1.0f };
    return { point.x, point.y, point.z };
}

void BakeSpecies(FoliageSpeciesCache& cache,
                 const FoliageSpecies& species,
                 const TerrainComponent& terrain,
                 const math::Matrix4& terrainWorld,
                 renderer::ResourceManager& resources)
{
    cache.instances.clear();
    cache.instanceBuffer = {};
    if (species.modelPath.empty() || terrain.columns < 2 || terrain.rows < 2)
        return;

    // WHY: ロード失敗のまま instances を積むと FoliageTool の stamp カウントが増え続け、
    //      存在しないアセットに対して Physics コライダーも生成されてしまう。
    if (!asset::AssetManager::LoadModel(species.modelPath))
        return;

    const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainD = static_cast<float>(terrain.rows - 1) * terrain.cellSize;
    const float minScale = std::max(0.01f, std::min(species.minScale, species.maxScale));
    const float maxScale = std::max(minScale, std::max(species.minScale, species.maxScale));
    uint32_t seed = species.seed;

    if (species.placementMode == FoliagePlacementMode::STAMP) {
        // STAMP は子 GO 階層 (FoliageBakeSystem) でヒエラルキー・物理を管理するが、
        // レンダリングは引き続き GPU instancing で行う。
        // WHY: 子 GO の MeshRenderer は MaterialComponent が設定されていないと描画されないため、
        //      Foliage シェーダー (foliagePSO) が確実な描画経路として機能する。
        cache.instances.reserve(species.stamps.size());
        for (const FoliageStamp& stamp : species.stamps) {
            const math::Vector3 world = TransformPoint(
                terrainWorld,
                stamp.localPosition.x,
                stamp.localPosition.y,
                stamp.localPosition.z);
            cache.instances.push_back({
                world.x, world.y, world.z, stamp.rotationY, std::max(0.01f, stamp.scale)
            });
        }
    } else {
        if (species.densityPer100SquareMeters <= 0.0f)
            return;
        const float spacing = std::sqrt(100.0f / species.densityPer100SquareMeters);
        for (float z = spacing * 0.5f; z < terrainD; z += spacing) {
            for (float x = spacing * 0.5f; x < terrainW; x += spacing) {
                const float localX = std::clamp(
                    x + (Random01(seed) - 0.5f) * spacing, 0.0f, terrainW);
                const float localZ = std::clamp(
                    z + (Random01(seed) - 0.5f) * spacing, 0.0f, terrainD);
                const float localY = terrain.GetHeightAt(localX, localZ);
                const math::Vector3 world =
                    TransformPoint(terrainWorld, localX, localY, localZ);

                const float rotation = species.randomYRotation
                    ? Random01(seed) * 6.2831853f : 0.0f;
                const float scale = minScale + Random01(seed) * (maxScale - minScale);
                cache.instances.push_back({
                    world.x, world.y, world.z, rotation, scale
                });
            }
        }
    }

    if (!cache.instances.empty()) {
        cache.instanceBuffer = resources.CreateStructuredBuffer(
            cache.instances.data(),
            static_cast<uint32_t>(cache.instances.size()),
            static_cast<uint32_t>(sizeof(FoliageInstance)));
    }
}

} // namespace

// ─── IRenderPass ──────────────────────────────────────────────────────────

std::string_view FoliageRenderPass::Name() const { return "FoliagePass"; }

std::vector<renderer::RenderGraph::ResourceAccess> FoliageRenderPass::DeclareAccesses(
    const RenderPassContext&) const
{
    return { { "HDR", renderer::RenderGraph::ResourceUsage::ReadWrite } };
}

void FoliageRenderPass::Execute(RenderPassContext& ctx)
{
    ctx.renderer.SetRenderTarget(ctx.handles.hdrRT, ctx.resources);

    if (!ctx.handles.foliageShader.IsValid())
        return;

    ctx.renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);

    for (auto [foliage, terrain, transform] :
         ctx.scene.View<FoliageComponent, TerrainComponent, Transform>()) {
        if (!foliage.enabled || foliage.species.empty()) continue;
        if (!terrain.enabled || terrain.heightData.empty()
            || terrain.columns < 2 || terrain.rows < 2) {
            continue;
        }

        const math::Matrix4 terrainWorld = transform.GetWorldMatrix();
        const bool transformChanged =
            !foliage.hasBakedTransform
            || foliage.bakedWorldPosition != transform.worldPosition
            || !(foliage.bakedWorldRotation == transform.worldRotation)
            || foliage.bakedWorldScale != transform.worldScale;

        if (foliage.needsBake
            || foliage.caches.size() != foliage.species.size()
            || transformChanged) {
            foliage.caches.resize(foliage.species.size());
            for (size_t i = 0; i < foliage.species.size(); ++i)
                BakeSpecies(foliage.caches[i], foliage.species[i],
                            terrain, terrainWorld, ctx.resources);

            foliage.needsBake = false;
            foliage.bakedWorldPosition = transform.worldPosition;
            foliage.bakedWorldRotation = transform.worldRotation;
            foliage.bakedWorldScale = transform.worldScale;
            foliage.hasBakedTransform = true;
        }

        for (size_t speciesIndex = 0; speciesIndex < foliage.species.size(); ++speciesIndex) {
            const auto& species = foliage.species[speciesIndex];
            auto& cache = foliage.caches[speciesIndex];
            if (!cache.instanceBuffer.IsValid() || cache.instances.empty()) continue;

            // WHAT: MVPでは Terrain 全体を1バッチにするため、中心と外接半径で粗く距離カリングする。
            // WHY: 個体単位のCPU選別は毎フレームの再転送を招く。将来のチャンク分割時に精密化する。
            const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
            const float terrainD = static_cast<float>(terrain.rows - 1) * terrain.cellSize;
            const math::Vector3 terrainCenter =
                TransformPoint(terrainWorld, terrainW * 0.5f, 0.0f, terrainD * 0.5f);
            const float halfW = terrainW * std::abs(transform.worldScale.x) * 0.5f;
            const float halfD = terrainD * std::abs(transform.worldScale.z) * 0.5f;
            const float terrainRadius = std::sqrt(halfW * halfW + halfD * halfD);
            const float cameraDx = ctx.camera.m_position.x - terrainCenter.x;
            const float cameraDz = ctx.camera.m_position.z - terrainCenter.z;
            const float cameraDistance = std::sqrt(cameraDx * cameraDx + cameraDz * cameraDz);
            if (cameraDistance > std::max(0.0f, species.drawDistance) + terrainRadius)
                continue;

            const auto* model = asset::AssetManager::LoadModel(species.modelPath);
            if (!model) continue;
            cache.materialConstantBuffers.resize(model->meshes.size());

            for (size_t meshIndex = 0; meshIndex < model->meshes.size(); ++meshIndex) {
                const auto& mesh = model->meshes[meshIndex];
                if (!mesh || !mesh->vertexBuffer.IsValid()
                    || !mesh->indexBuffer.IsValid() || mesh->indexCount == 0) {
                    continue;
                }

                const asset::MaterialAsset* material = nullptr;
                if (meshIndex < species.subMeshMaterialPaths.size()
                    && !species.subMeshMaterialPaths[meshIndex].empty()) {
                    const auto handle = asset::AssetManager::LoadMaterial(
                        species.subMeshMaterialPaths[meshIndex]);
                    material = asset::AssetManager::GetMaterial(handle);
                }

                FoliageMaterialCB materialData{};
                materialData.baseColor[0] = 0.65f;
                materialData.baseColor[1] = 0.70f;
                materialData.baseColor[2] = 0.60f;
                materialData.baseColor[3] = 1.0f;
                materialData.alphaCutoff = 0.0f;

                renderer::ResourceHandle<renderer::TextureTag> albedo;
                bool doubleSided = false;
                if (material) {
                    if (const auto color = material->params.find("base_color");
                        color != material->params.end()) {
                        for (size_t i = 0; i < std::min<size_t>(4, color->second.size()); ++i)
                            materialData.baseColor[i] = color->second[i];
                    }
                    if (const auto cutoff = material->params.find("alphaCutoff");
                        cutoff != material->params.end() && !cutoff->second.empty()) {
                        materialData.alphaCutoff = cutoff->second[0];
                    }
                    if (const auto texture = material->textures.find("albedo");
                        texture != material->textures.end() && !texture->second.empty()) {
                        albedo = ctx.resources.LoadTexture(texture->second);
                        materialData.hasAlbedo = albedo.IsValid() ? 1u : 0u;
                        if (materialData.alphaCutoff <= 0.0f)
                            materialData.alphaCutoff = 0.5f;
                    }
                    doubleSided = material->doubleSided;
                }

                auto& materialCB = cache.materialConstantBuffers[meshIndex];
                if (!materialCB.IsValid())
                    materialCB = ctx.resources.CreateConstantBuffer(sizeof(FoliageMaterialCB));
                ctx.resources.Update(materialCB, &materialData, sizeof(materialData));

                renderer::DrawCall draw{};
                draw.shader = ctx.handles.foliageShader;
                // ビューモードに応じて PSO を切り替える。ワイヤーフレーム時は共用 wireframePSO を使う。
                draw.pipelineState = ctx.settings.IsWireframe()
                    ? ctx.handles.wireframePSO
                    : (doubleSided ? ctx.handles.foliageNoCullPSO : ctx.handles.foliagePSO);
                draw.vertexBuffer = mesh->vertexBuffer;
                draw.indexBuffer = mesh->indexBuffer;
                draw.indexCount = mesh->indexCount;
                draw.instanceCount = static_cast<uint32_t>(cache.instances.size());
                draw.instanceBuffer = cache.instanceBuffer;
                draw.constantBuffers[0] = ctx.handles.frameCB;
                draw.constantBuffers[2] = materialCB;
                draw.constantBuffers[3] = ctx.handles.lightCB;
                draw.textures[0] = albedo;
                ctx.renderer.Submit(draw, ctx.resources);
            }
        }
    }
}

} // namespace fbzz::scene
