// FBZZ Engine
// DetailRenderSystem.cpp | fbzz::scene
// TerrainDetailComponent → GPU Instancing 描画の実装
//
// 設計:
//   - インスタンス位置はワールド空間で Bake する (Terrain の Translation を加算)
//   - Terrain が静的である前提: 移動時は needsBake = true で再 Bake
//   - チャンクサイズ: 16m × 16m (kDetailChunkSize)
//   - 密度マップ: DetailTool のランタイムマップを使用し、未作成時は密度 0
//
// レイヤー別描画パス:
//   Mesh/Billboard — Detail.hlsl     : VS t0=DetailInstance, PS t0=アルベド, b0=Camera, b2=DetailMaterialCB
//   Grass          — DetailGrass.hlsl: VS t0=GrassInstance (プロシージャル), b0=Camera, b3=Light, b2=DetailGrassCB
//
// テクスチャスロット (各シェーダーと同期):
//   instanceBuffer (VS t0) = StructuredBuffer<DetailInstance or GrassInstance>
//   PS t0 = アルベドテクスチャ
#include "Engine/Scene/Systems/DetailRenderSystem.hpp"
#include "Engine/Scene/Systems/RenderPassContext.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Transform.hpp"
#include "Engine/Scene/Entity.hpp"
#include "Engine/Scene/Components/TerrainComponent.hpp"
#include "Engine/Scene/Components/TerrainDetailComponent.hpp"
#include "Engine/Asset/AssetManager.hpp"
#include "Engine/Asset/Model.hpp"
#include "Engine/Core/Time.hpp"
#include "Engine/Renderer/Camera.hpp"
#include "Engine/Renderer/DrawCall.hpp"
#include "Engine/Renderer/IRenderer.hpp"
#include "Engine/Renderer/Mesh.hpp"
#include "Engine/Renderer/RenderState.hpp"
#include "Engine/Renderer/ResourceManager.hpp"
#include "Engine/Renderer/SamplerMode.hpp"
#include <Math/Frustum.hpp>
#include <Math/Matrix4.hpp>
#include <Math/Vector3.hpp>
#include <Math/Vector4.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {

// ============================================================
// 定数
// ============================================================

static constexpr float kDetailChunkSize     = 16.0f; // チャンク 1 辺のワールドサイズ [m]
static constexpr float kMaxDensityPerMeter2 =  4.0f; // density=1 のときの最大個数 [個/m²]

// ============================================================
// 内部 CB 型 (対応する HLSL cbuffer と完全に一致させること)
// ============================================================

// Detail.hlsl b2
struct DetailMaterialCB {
    float    alphaCutoff;
    uint32_t isBillboard;
    uint32_t hasAlbedoTex; // 1 if albedo texture is bound; 0 = fallback white
    float    _pad;
};
static_assert(sizeof(DetailMaterialCB) == 16, "DetailMaterialCB size mismatch");

// ============================================================
// エンティティ別 CB キャッシュ
// ============================================================

static std::unordered_map<uint32_t, renderer::ResourceHandle<renderer::ConstantBufferTag>> g_matCBCache;

// ============================================================
// Billboard 用ユニットクワッド (一度だけ生成)
// ============================================================

struct QuadVertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz;
    float u, v;
};

static renderer::ResourceHandle<renderer::BufferTag> s_quadVB;
static renderer::ResourceHandle<renderer::BufferTag> s_quadIB;
static uint64_t s_quadResetVersion = 0;

static void EnsureQuadBuffers(renderer::ResourceManager& resources)
{
    if (s_quadResetVersion == resources.GetResetVersion() && s_quadVB.IsValid())
        return;

    s_quadResetVersion = resources.GetResetVersion();

    // 単位クワッド: X[-0.5,0.5], Y[-0.5,0.5] (Billboard モードでは VS 内でカメラ向きに展開)
    static const QuadVertex verts[4] = {
        { -0.5f,  0.5f, 0.0f,  0.0f, 0.0f, -1.0f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f },
        {  0.5f,  0.5f, 0.0f,  0.0f, 0.0f, -1.0f,  1.0f, 0.0f, 0.0f,  1.0f, 0.0f },
        { -0.5f, -0.5f, 0.0f,  0.0f, 0.0f, -1.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f },
        {  0.5f, -0.5f, 0.0f,  0.0f, 0.0f, -1.0f,  1.0f, 0.0f, 0.0f,  1.0f, 1.0f },
    };
    static const uint32_t indices[6] = { 0, 1, 2, 1, 3, 2 };

    s_quadVB = resources.CreateVertexBuffer(verts, sizeof(verts), sizeof(QuadVertex));
    s_quadIB = resources.CreateIndexBuffer(indices, 6);
}

// ============================================================
// 乱数 (決定論的 LCG)
// ============================================================

static float LcgRand(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
}

// ============================================================
// Terrain 描画・MeshCollider と同じ World Matrix でローカル点を変換する。
static math::Vector3 TransformPoint(const math::Matrix4& matrix, const math::Vector3& point)
{
    const math::Vector4 transformed = matrix * math::Vector4{ point.x, point.y, point.z, 1.0f };
    return { transformed.x, transformed.y, transformed.z };
}

// Bake
// ============================================================

static void BakeChunk(
    DetailChunk&                           chunk,
    int                                    cx,
    int                                    cz,
    const TerrainComponent&                terrain,
    const math::Matrix4&                   terrainWorld,
    const std::vector<DetailLayer>&        layers,
    const std::vector<DetailDensityMap>&   densityMaps,
    renderer::ResourceManager&             resources)
{
    const size_t layerCount = layers.size();
    chunk.chunkX = cx;
    chunk.chunkZ = cz;
    chunk.instancesPerLayer.assign(layerCount, {});
    chunk.instanceBuffers.assign(layerCount, {});
    chunk.grassInstancesPerLayer.assign(layerCount, {});
    chunk.grassInstanceBuffers.assign(layerCount, {});

    const float localX0 = static_cast<float>(cx) * kDetailChunkSize;
    const float localZ0 = static_cast<float>(cz) * kDetailChunkSize;
    const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
    const float terrainD = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
    const float localX1 = std::min(localX0 + kDetailChunkSize, terrainW);
    const float localZ1 = std::min(localZ0 + kDetailChunkSize, terrainD);

    if (localX1 <= localX0 || localZ1 <= localZ0) {
        chunk.isDirty = false;
        return;
    }

    uint32_t baseSeed = static_cast<uint32_t>(cx) * 31337u
                      + static_cast<uint32_t>(cz) * 7919u;

    for (size_t li = 0; li < layerCount; ++li)
    {
        const DetailLayer& layer = layers[li];
        if (layer.density <= 0.0f) continue;

        uint32_t seed = baseSeed + static_cast<uint32_t>(li) * 6271u;
        const float sampleStep = std::max(
            1.0f / (layer.density * kMaxDensityPerMeter2), 0.01f);

        const bool isGrass = (layer.type == DetailLayerType::Grass);

        for (float localX = localX0; localX < localX1; localX += sampleStep)
        {
            for (float localZ = localZ0; localZ < localZ1; localZ += sampleStep)
            {
                const float jx = localX + (LcgRand(seed) - 0.5f) * sampleStep;
                const float jz = localZ + (LcgRand(seed) - 0.5f) * sampleStep;

                // 密度マップサンプリング: DetailTool がペイントした値で間引く
                (void)layer.densityMapPath;
                // WHY: 新規 DetailLayer は空の Terrain から Paint を開始する。
                //      密度マップ未作成を全面密度1と解釈すると、初回 Bake で最大数が配置される。
                if (li >= densityMaps.size() || !densityMaps[li].IsValid())
                    continue;
                const float u = jx / terrainW;
                const float v = jz / terrainD;
                if (LcgRand(seed) > densityMaps[li].Sample(u, v)) continue;

                const float localY = terrain.GetHeightAt(jx, jz);
                const math::Vector3 worldPosition =
                    TransformPoint(terrainWorld, { jx, localY, jz });

                const float rotY  = layer.randomYRotation
                                  ? LcgRand(seed) * 6.2831853f
                                  : 0.0f;
                const float scale = layer.minScale
                                  + LcgRand(seed) * (layer.maxScale - layer.minScale);

                if (isGrass)
                {
                    const float windPhase = LcgRand(seed) * 6.2831853f;
                    chunk.grassInstancesPerLayer[li].push_back(
                        GrassInstance{
                            worldPosition.x, worldPosition.y, worldPosition.z,
                            rotY, scale, windPhase });
                }
                else
                {
                    chunk.instancesPerLayer[li].push_back(
                        DetailInstance{
                            worldPosition.x, worldPosition.y, worldPosition.z,
                            rotY, scale });
                }
            }
        }

        // GPU バッファ生成
        if (isGrass)
        {
            auto& gInst = chunk.grassInstancesPerLayer[li];
            if (!gInst.empty())
                chunk.grassInstanceBuffers[li] = resources.CreateStructuredBuffer(
                    gInst.data(),
                    static_cast<uint32_t>(gInst.size()),
                    static_cast<uint32_t>(sizeof(GrassInstance)));
        }
        else
        {
            auto& inst = chunk.instancesPerLayer[li];
            if (!inst.empty())
                chunk.instanceBuffers[li] = resources.CreateStructuredBuffer(
                    inst.data(),
                    static_cast<uint32_t>(inst.size()),
                    static_cast<uint32_t>(sizeof(DetailInstance)));
        }
    }

    chunk.isDirty = false;
}

// ============================================================
// メインシステム
// ============================================================

void DetailRenderSystem(RenderPassContext& ctx)
{
    Scene&                     scene     = ctx.scene;
    renderer::IRenderer&       renderer  = ctx.renderer;
    renderer::ResourceManager& resources = ctx.resources;
    const renderer::Camera&    camera    = ctx.camera;
    const auto&                handles   = ctx.handles;

    // Bake は shader の初期化状態に依存しない。
    // WHY: shader 準備前に return すると needsBake が解除されず、
    //      Editor 上で Baking Detail 表示が永続化するため。
    const bool shadersReady = handles.detailMeshShader.IsValid();
    if (shadersReady)
        EnsureQuadBuffers(resources);

    const math::Frustum* frustum = ctx.cameraFrustum;
    const float curTime = fbzz::Time::time;

    if (shadersReady)
        renderer.SetSampler(0, renderer::SamplerMode::WRAP_ANISOTROPIC);

    for (auto [detail, terrain, transform] :
         scene.View<TerrainDetailComponent, TerrainComponent, Transform>())
    {
        if (!detail.enabled || detail.layers.empty()) continue;
        if (!terrain.enabled || terrain.heightData.empty()) continue;

        const math::Matrix4 terrainWorld = transform.GetWorldMatrix();
        const bool transformChanged =
            !detail.hasBakedTransform
            || detail.bakedWorldPosition != transform.worldPosition
            || !(detail.bakedWorldRotation == transform.worldRotation)
            || detail.bakedWorldScale != transform.worldScale;

        // ============================================================
        // Bake
        // ============================================================
        if (detail.needsBake || detail.chunks.empty() || transformChanged)
        {
            detail.chunks.clear();

            const float terrainW = static_cast<float>(terrain.columns - 1) * terrain.cellSize;
            const float terrainD = static_cast<float>(terrain.rows    - 1) * terrain.cellSize;
            const int numCX = static_cast<int>(std::ceil(terrainW / kDetailChunkSize));
            const int numCZ = static_cast<int>(std::ceil(terrainD / kDetailChunkSize));

            detail.chunks.resize(static_cast<size_t>(numCX) * static_cast<size_t>(numCZ));
            for (int cz = 0; cz < numCZ; ++cz)
                for (int cx = 0; cx < numCX; ++cx)
                    BakeChunk(detail.chunks[static_cast<size_t>(cz) * numCX + cx],
                              cx, cz, terrain, terrainWorld, detail.layers, detail.densityMaps, resources);

            detail.needsBake = false;
            detail.bakedWorldPosition = transform.worldPosition;
            detail.bakedWorldRotation = transform.worldRotation;
            detail.bakedWorldScale    = transform.worldScale;
            detail.hasBakedTransform  = true;
        }

        // ============================================================
        if (!shadersReady)
            continue;

        // EntityID を逆引きして CB キャッシュのキーに使う
        // ============================================================
        EntityID eid{};
        for (EntityID candidate : scene.GetEntities<TerrainDetailComponent>()) {
            if (scene.GetComponent<TerrainDetailComponent>(candidate) == &detail) {
                eid = candidate;
                break;
            }
        }
        if (!scene.IsValid(eid)) continue;

        auto& matCBH = g_matCBCache[eid.index];
        if (!matCBH.IsValid())
            matCBH = resources.CreateConstantBuffer(sizeof(DetailMaterialCB));

        // ============================================================
        // 描画
        // ============================================================
        const math::Vector3 camPos = {
            camera.m_position.x, camera.m_position.y, camera.m_position.z
        };

        for (auto& chunk : detail.chunks)
        {
            // チャンク中心距離チェック (全レイヤーの最大 drawDistance を採用)
            const math::Vector3 chunkCenter = TransformPoint(terrainWorld, {
                (static_cast<float>(chunk.chunkX) + 0.5f) * kDetailChunkSize,
                0.0f,
                (static_cast<float>(chunk.chunkZ) + 0.5f) * kDetailChunkSize
            });

            float maxDraw = 0.0f;
            for (const auto& l : detail.layers)
                maxDraw = std::max(maxDraw, l.drawDistance);

            const float dx = chunkCenter.x - camPos.x;
            const float dz = chunkCenter.z - camPos.z;
            const float distSq = dx * dx + dz * dz;
            if (distSq > (maxDraw + kDetailChunkSize) * (maxDraw + kDetailChunkSize))
                continue;

            // AABB フラスタムカリング
            if (frustum)
            {
                const float localX0 = static_cast<float>(chunk.chunkX) * kDetailChunkSize;
                const float localZ0 = static_cast<float>(chunk.chunkZ) * kDetailChunkSize;
                const float localX1 = std::min(localX0 + kDetailChunkSize,
                    static_cast<float>(terrain.columns - 1) * terrain.cellSize);
                const float localZ1 = std::min(localZ0 + kDetailChunkSize,
                    static_cast<float>(terrain.rows - 1) * terrain.cellSize);

                math::Vector3 aabbMin = { 1e30f, 1e30f, 1e30f };
                math::Vector3 aabbMax = { -1e30f, -1e30f, -1e30f };
                for (float localY : { -terrain.maxHeight, terrain.maxHeight + 5.0f })
                for (float localZ : { localZ0, localZ1 })
                for (float localX : { localX0, localX1 }) {
                    const math::Vector3 corner =
                        TransformPoint(terrainWorld, { localX, localY, localZ });
                    aabbMin.x = std::min(aabbMin.x, corner.x);
                    aabbMin.y = std::min(aabbMin.y, corner.y);
                    aabbMin.z = std::min(aabbMin.z, corner.z);
                    aabbMax.x = std::max(aabbMax.x, corner.x);
                    aabbMax.y = std::max(aabbMax.y, corner.y);
                    aabbMax.z = std::max(aabbMax.z, corner.z);
                }
                if (!frustum->IntersectsAABB(aabbMin, aabbMax))
                    continue;
            }

            // ---- レイヤー別 DrawCall ----
            for (size_t li = 0; li < detail.layers.size(); ++li)
            {
                const DetailLayer& layer = detail.layers[li];
                if (distSq > layer.drawDistance * layer.drawDistance)
                    continue;

                const bool isGrass     = (layer.type == DetailLayerType::Grass);
                const bool isBillboard = (layer.type == DetailLayerType::Billboard);

                // ---- Grass ----
                if (isGrass)
                {
                    if (li >= chunk.grassInstanceBuffers.size()) continue;
                    const auto& instBuf  = chunk.grassInstanceBuffers[li];
                    const auto& instList = chunk.grassInstancesPerLayer[li];
                    if (!instBuf.IsValid() || instList.empty()) continue;

                    if (!handles.detailGrassShader.IsValid()) continue;

                    // アルベドテクスチャ
                    renderer::ResourceHandle<renderer::TextureTag> albedoTex;
                    if (!layer.texturePath.empty())
                        albedoTex = asset::AssetManager::LoadTexture(layer.texturePath);

                    // DetailGrassCB を更新
                    DetailGrassCB gcb{};
                    gcb.windDir[0]    = 0.7071f; // デフォルト風向き: X+Z 斜め
                    gcb.windDir[1]    = 0.0f;
                    gcb.windDir[2]    = 0.7071f;
                    gcb.gTime         = curTime;
                    gcb.windStrength  = layer.windStrength;
                    gcb.windFrequency = layer.windFrequency;
                    gcb.bladeHeight   = layer.bladeHeight;
                    gcb.bladeWidth    = layer.bladeWidth;
                    gcb.bladeSegments = layer.bladeSegments;
                    gcb.alphaCutoff   = 0.3f;
                    gcb.hasAlbedoTex  = albedoTex.IsValid() ? 1 : 0;
                    gcb._pad          = 0.0f;
                    resources.Update(handles.detailGrassCB, &gcb, sizeof(gcb));

                    // 頂点バッファなし: vertexCount = bladeSegments * 6
                    renderer::DrawCall dc{};
                    dc.shader         = handles.detailGrassShader;
                    dc.pipelineState  = handles.detailNoCullPSO; // 両面描画
                    dc.vertexCount    = static_cast<uint32_t>(layer.bladeSegments * 6);
                    dc.instanceCount  = static_cast<uint32_t>(instList.size());
                    dc.instanceBuffer = instBuf;
                    dc.constantBuffers[0] = handles.frameCB;
                    dc.constantBuffers[2] = handles.detailGrassCB;
                    dc.constantBuffers[3] = handles.lightCB;
                    dc.textures[0]        = albedoTex;

                    renderer.Submit(dc, resources);
                }
                // ---- Mesh / Billboard ----
                else
                {
                    if (li >= chunk.instanceBuffers.size()) continue;
                    const auto& instBuf  = chunk.instanceBuffers[li];
                    const auto& instList = chunk.instancesPerLayer[li];
                    if (!instBuf.IsValid() || instList.empty()) continue;

                    // アルベドテクスチャ
                    renderer::ResourceHandle<renderer::TextureTag> albedoTex;
                    if (!layer.texturePath.empty())
                        albedoTex = asset::AssetManager::LoadTexture(layer.texturePath);

                    // DetailMaterialCB 更新
                    DetailMaterialCB matData{};
                    matData.alphaCutoff  = albedoTex.IsValid() ? 0.5f : 0.0f;
                    matData.isBillboard  = isBillboard ? 1u : 0u;
                    matData.hasAlbedoTex = albedoTex.IsValid() ? 1u : 0u;
                    resources.Update(matCBH, &matData, sizeof(matData));

                    auto submitMesh = [&](renderer::ResourceHandle<renderer::BufferTag> vb,
                                          renderer::ResourceHandle<renderer::BufferTag> ib,
                                          uint32_t indexCount) {
                        if (!vb.IsValid() || !ib.IsValid() || indexCount == 0) return;

                        renderer::DrawCall dc{};
                        dc.shader         = isBillboard ? handles.detailBillboardShader
                                                        : handles.detailMeshShader;
                        // Detail 用メッシュは外部アセット由来で winding が統一されないため、
                        // Billboard と同様に両面描画して配置結果が消えることを防ぐ。
                        dc.pipelineState  = handles.detailNoCullPSO;
                        dc.vertexBuffer   = vb;
                        dc.indexBuffer    = ib;
                        dc.indexCount     = indexCount;
                        dc.instanceCount  = static_cast<uint32_t>(instList.size());
                        dc.instanceBuffer = instBuf;
                        dc.constantBuffers[0] = handles.frameCB;
                        dc.constantBuffers[2] = matCBH;
                        dc.textures[0]        = albedoTex;

                        renderer.Submit(dc, resources);
                    };

                    if (isBillboard) {
                        submitMesh(s_quadVB, s_quadIB, 6);
                    } else {
                        if (layer.meshPath.empty()) continue;
                        const auto* model = asset::AssetManager::Load<asset::Model>(layer.meshPath);
                        if (!model) continue;

                        // FBX はマテリアル単位で複数サブメッシュに分割されるため全て描画する。
                        for (const auto& mesh : model->meshes) {
                            if (!mesh) continue;
                            submitMesh(mesh->vertexBuffer, mesh->indexBuffer, mesh->indexCount);
                        }
                    }
                }
            }
        }
    }
}

} // namespace fbzz::scene
