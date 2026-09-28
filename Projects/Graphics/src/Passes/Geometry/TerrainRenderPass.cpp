/// @file    TerrainRenderPass.cpp
/// @brief   TerrainComponent → GPU チャンクメッシュ生成・描画 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include <Graphics/Passes/Geometry/TerrainRenderPass.hpp>
#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <algorithm>
#include <optional>
namespace fbzz::renderer {
struct TerrainCameraFrameCB {
    math::Matrix4 view;
    math::Matrix4 projection;
    math::Matrix4 viewProjection;
    math::Matrix4 invViewProjection;
    math::Vector3 cameraPos; float nearZ;
    /// @note LAYOUT: PerFrameCB / Constants.hlsli の CameraConstants と一致させること。
    float         farZ;      float _reserved;
    float         isOrthographic; float _pad;
};
static_assert(sizeof(TerrainCameraFrameCB) == 288, "PerFrameCB size mismatch");
static bool IsChunkVisible(
    const math::Frustum&  frustum,
    const math::Matrix4&  world,
    const math::Vector3&  localMin,
    const math::Vector3&  localMax)
{
    math::Vector3 worldMin = {  1e30f,  1e30f,  1e30f };
    math::Vector3 worldMax = { -1e30f, -1e30f, -1e30f };
    const math::Vector3 corners[8] = {
        { localMin.x, localMin.y, localMin.z }, { localMax.x, localMin.y, localMin.z },
        { localMin.x, localMax.y, localMin.z }, { localMax.x, localMax.y, localMin.z },
        { localMin.x, localMin.y, localMax.z }, { localMax.x, localMin.y, localMax.z },
        { localMin.x, localMax.y, localMax.z }, { localMax.x, localMax.y, localMax.z },
    };
    for (const auto& c : corners) {
        const math::Vector4 wp = world * math::Vector4{ c.x, c.y, c.z, 1.0f };
        worldMin.x = std::min(worldMin.x, wp.x); worldMax.x = std::max(worldMax.x, wp.x);
        worldMin.y = std::min(worldMin.y, wp.y); worldMax.y = std::max(worldMax.y, wp.y);
        worldMin.z = std::min(worldMin.z, wp.z); worldMax.z = std::max(worldMax.z, wp.z);
    }
    const math::Vector3 center  = { (worldMin.x + worldMax.x) * 0.5f,
                                     (worldMin.y + worldMax.y) * 0.5f,
                                     (worldMin.z + worldMax.z) * 0.5f };
    const math::Vector3 extents = { (worldMax.x - worldMin.x) * 0.5f,
                                     (worldMax.y - worldMin.y) * 0.5f,
                                     (worldMax.z - worldMin.z) * 0.5f };
    return frustum.IntersectsAABB(center, extents);
}
/// @brief 地形の描画と影で同じチャンク LOD を選ぶ。
/// @note 異なるメッシュで自己影を描くと粗い三角形が表示面を貫き、斜面に偽の影が出る。
static int SelectTerrainLOD(const RenderTerrainInput& input, const RenderTerrainPatch& chunk,
                            const math::Vector3& cameraPosition)
{
    if (input.fiberSurface) return 0;
    const math::Vector3 localCenter = (chunk.aabbMin + chunk.aabbMax) * 0.5f;
    const math::Vector4 worldCenter = input.constants.worldMatrix
        * math::Vector4{ localCenter.x, localCenter.y, localCenter.z, 1.0f };
    const float dx = worldCenter.x - cameraPosition.x;
    const float dy = worldCenter.y - cameraPosition.y;
    const float dz = worldCenter.z - cameraPosition.z;
    const float distSq = dx * dx + dy * dy + dz * dz;
    const float nearDistance = input.chunkWorldSize * 2.0f;
    const float midDistance = input.chunkWorldSize * 6.0f;
    return distSq < nearDistance * nearDistance ? 0
         : distSq < midDistance * midDistance ? 1 : 2;
}
std::string_view TerrainRenderPass::Name() const
{
    return m_mode == TerrainDrawMode::GBuffer ? "TerrainGBuffer" : "TerrainForward";
}

void TerrainRenderPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    /// @note 影と Cookie の束縛は GBuffer / Forward で分岐していない (Execute の t13 / t28 / t31)。
    /// @note 以前は Deferred のときだけ申告から抜けていて、依存辺が張られないまま
    /// @note «同じ GBuffer を書く DeferredGBuffer が先に走るから» という偶然で順序が保たれていた。
    builder.Read("ShadowMap").Read("PunctualShadowMap").Read("LightCookieAtlas");

    /// @note GBuffer へ書けば DeferredLighting/GTAO/SSAO/SSR/ContactShadows に地形が含まれる。
    /// @note Forward では HDR へ直接ライティング結果を描く。
    const char* const target = m_mode == TerrainDrawMode::GBuffer ? "GBuffer" : "HDR";
    builder.ReadWrite(target).SetAutoTarget(target);
}

void TerrainRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    /// @note 描き先の束縛は Setup の SetAutoTarget が済ませている。

    /// @note Forward + GBuffer 前段では地形が GBuffer と Forward の 2 回走る。チャンクの統計は Forward 側だけで数える。
    std::optional<CullStatsRollback> rollback;
    if (m_mode == TerrainDrawMode::GBuffer && !ctx.isDeferred) rollback.emplace(ctx);

    /// @note エイリアス: TerrainRenderSystem の旧シグネチャ変数名を ctx から引く
    renderer::IRenderer&       renderer            = ctx.renderer;
    renderer::ResourceManager& resources           = ctx.resources;
    const renderer::Camera&    camera              = ctx.camera;
    const renderer::RenderSettings* settings       = &ctx.settings;
    auto shadowDepthTexture = ctx.resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
    auto shadowCB           = ctx.handles.shadowCB;
    auto lightCB            = ctx.handles.lightCB;

    /// @note static ローカルは初回のみ初期化される。ResourceManager::Reset() で世代が変わった
    /// @note 場合だけ再生成し、旧ハンドル（失効済み）へのアクセスを防ぐ。
    static uint64_t s_resetVersion = resources.GetResetVersion();
    static auto terrainShader = resources.LoadShader("Assets/Shaders/Terrain/Terrain.hlsl");
    /// @note Deferred 用: GBuffer(MRT) へ albedo/roughness/normal/metallic を書き出す地形シェーダ。
    static auto terrainGBufferShader = resources.LoadShader("Assets/Shaders/Terrain/TerrainGBuffer.hlsl");
    static auto terrainPSO    = resources.CreatePipelineState({
        renderer::RasterizerMode::SOLID,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto terrainWireframePSO = resources.CreatePipelineState({
        renderer::RasterizerMode::WIREFRAME,
        renderer::BlendMode::OPAQUE_BLEND,
        renderer::DepthMode::DEPTH_ON
    });
    static auto cameraCBH  = resources.CreateConstantBuffer(sizeof(TerrainCameraFrameCB));
    static auto terrainCBH = resources.CreateConstantBuffer(sizeof(TerrainObjectCB));

    if (s_resetVersion != resources.GetResetVersion()) {
        s_resetVersion      = resources.GetResetVersion();
        terrainShader       = resources.LoadShader("Assets/Shaders/Terrain/Terrain.hlsl");
        terrainGBufferShader = resources.LoadShader("Assets/Shaders/Terrain/TerrainGBuffer.hlsl");
        terrainPSO          = resources.CreatePipelineState({ renderer::RasterizerMode::SOLID,     renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        terrainWireframePSO = resources.CreatePipelineState({ renderer::RasterizerMode::WIREFRAME, renderer::BlendMode::OPAQUE_BLEND, renderer::DepthMode::DEPTH_ON });
        cameraCBH           = resources.CreateConstantBuffer(sizeof(TerrainCameraFrameCB));
        terrainCBH          = resources.CreateConstantBuffer(sizeof(TerrainObjectCB));
    }
    /// @note TAA ジッターを地形にも乗せる。乗せないと地形だけ AA されないうえ、b0 経由で描く
    /// @note 他の不透明物とサブピクセル単位でずれた深度になり、TAA の再投影が濁る。
    const math::Matrix4 jitteredProj =
        MakeJitteredProjection(camera, ctx.taaJitterNdcX, ctx.taaJitterNdcY);
    const math::Matrix4 jitteredVP = jitteredProj * camera.GetViewMatrix();

    {
        TerrainCameraFrameCB camData{};
        camData.view              = camera.GetViewMatrix();
        camData.projection        = jitteredProj;
        camData.viewProjection    = jitteredVP;
        camData.invViewProjection = math::Matrix4::Inverse(jitteredVP);
        camData.cameraPos         = camera.m_position;
        camData.nearZ             = camera.m_near;
        camData.farZ              = camera.m_far;
        camData.isOrthographic    =
            camera.m_projection == renderer::ProjectionMode::Orthographic ? 1.0f : 0.0f;
        resources.Update(cameraCBH, &camData, sizeof(camData));
    }

    const math::Frustum frustum = math::Frustum::FromViewProjection(camera.GetViewProjection());
    if (!ctx.renderScene) return;
    for (const auto& input : ctx.renderScene->terrains) {
        const auto& world = input.constants.worldMatrix;
        auto terrainCBData = input.constants;
        terrainCBData.wvpMatrix = jitteredVP * world;
        resources.Update(terrainCBH, &terrainCBData, sizeof(terrainCBData));
        for (const auto& chunk : input.patches) {
                /// @note 地形チャンクはそれぞれ独立にカリングされる描画候補なので、
                /// @note メッシュと同じ粒度で統計に数える。
                ++ctx.statsTotalObjects;
                /// @note カメラの Frustum Culling を切っている間はチャンクも落とさない。
                /// @note メッシュだけ全部出て地形だけ消えると、切り分けの道具として成立しない。
                if (ctx.frustumCullingEnabled &&
                    !IsChunkVisible(frustum, world, chunk.aabbMin, chunk.aabbMax)) {
                    ++ctx.statsFrustumCulled;
                    continue;
                }

                const int lod = SelectTerrainLOD(input, chunk, camera.m_position);
                /// @note 全セルが穴のチャンクはインデックスが 0 本。
                if (chunk.indexCountLOD[lod] == 0) continue;

                renderer::DrawCall call;
                call.vertexBuffer  = chunk.vertexBuffer;
                call.indexBuffer   = chunk.indexBufferLOD[lod];
                /// @note Deferred: GBuffer 書き込みシェーダ。Forward: 自前ライティングシェーダ。
                call.shader        = m_mode == TerrainDrawMode::GBuffer ? terrainGBufferShader
                                                                        : terrainShader;
                call.pipelineState = (settings && settings->IsWireframe()) ? terrainWireframePSO : terrainPSO;
                call.indexCount    = chunk.indexCountLOD[lod];
                call.layer         = renderer::RenderLayer::OPAQUE_LAYER;
                call.topology      = renderer::PrimitiveTopology::TRIANGLE_LIST;

                call.constantBuffers[0] = cameraCBH;
                call.constantBuffers[1] = terrainCBH;
                call.constantBuffers[3] = lightCB;
                call.constantBuffers[4] = shadowCB;
                /// @note b8: 画面空間 AO / 接触影の強度。Forward の地形がこれを読む。
                call.constantBuffers[8] = ctx.handles.advancedGraphicsCB;
                BindForwardShadingResources(call, ctx);

                call.textures[0]  = input.splatIndices;
                call.textures[1]  = input.splatWeights;
                call.textures[13] = shadowDepthTexture;

                SubmitCounted(ctx, call);
        }
    }
}
void SubmitTerrainShadowCasters(
    RenderPassContext&                            ctx,
    const math::Frustum&                          lightFrustum,
    renderer::ResourceHandle<renderer::ShaderTag> shadowShader,
    renderer::ResourceHandle<renderer::PipelineStateTag> pipelineState,
    renderer::ResourceHandle<renderer::ConstantBufferTag> frameCB,
    renderer::ResourceHandle<renderer::ConstantBufferTag> objectCB)
{
    /// @note ライト種別・shadow atlas・cascade の選択は ShadowPass 側に閉じ込め、
    /// @note こちらは「指定されたライト視錐台へ描けるチャンクを提出する」だけにする。
    if (!shadowShader.IsValid() || !pipelineState.IsValid())
        return;

    if (!ctx.renderScene) return;
    auto& resources = ctx.resources;
    for (const auto& input : ctx.renderScene->terrains) {
        const auto& world = input.constants.worldMatrix;
        PerObjectCB object{};
        object.world = world;
        object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        resources.Update(objectCB, &object, sizeof(object));
        for (const auto& chunk : input.patches) {
                if (!IsChunkVisible(lightFrustum, world, chunk.aabbMin, chunk.aabbMax))
                    continue;

                const int lod = SelectTerrainLOD(input, chunk, ctx.camera.m_position);
                if (chunk.indexCountLOD[lod] == 0) continue;

                renderer::DrawCall dc;
                dc.vertexBuffer       = chunk.vertexBuffer;
                dc.indexBuffer        = chunk.indexBufferLOD[lod];
                dc.indexCount         = chunk.indexCountLOD[lod];
                dc.shader             = shadowShader;
                dc.pipelineState      = pipelineState;
                dc.layer              = renderer::RenderLayer::OPAQUE_LAYER;
                dc.topology           = renderer::PrimitiveTopology::TRIANGLE_LIST;
                dc.constantBuffers[0] = frameCB;
                dc.constantBuffers[1] = objectCB;
                SubmitCountedShadow(ctx, dc);
        }
    }
}
void TerrainSelectionMaskSystem(RenderPassContext& ctx)
{
    if (!ctx.selectionOutlineEnabled) return;
    auto& h = ctx.handles;
    if (!h.selectionMaskShader.IsValid() || !h.selectionMaskPSO.IsValid()) return;

    if (!ctx.renderScene) return;
    for (const auto& input : ctx.renderScene->terrains) {
        if (!input.selected || input.layer >= 32 || (ctx.cullingMask & (1u << input.layer)) == 0) continue;
        const auto& world = input.constants.worldMatrix;
        PerObjectCB objData{};
        objData.world             = world;
        objData.worldInvTranspose = math::Matrix4::InverseTransposeAffine(world);
        ctx.resources.Update(h.objectCB, &objData, sizeof(PerObjectCB));
        for (const auto& chunk : input.patches) {
            if (chunk.indexCountLOD[0] == 0) continue;
            renderer::DrawCall dc;
            dc.vertexBuffer       = chunk.vertexBuffer;
            dc.indexBuffer        = chunk.indexBufferLOD[0];
            dc.indexCount         = chunk.indexCountLOD[0];
            dc.shader             = h.selectionMaskShader;
            dc.pipelineState      = h.selectionMaskPSO;
            dc.constantBuffers[0] = h.frameCB;
            dc.constantBuffers[1] = h.objectCB;
            ctx.renderer.Submit(dc, ctx.resources);
        }
    }
}

} /// @note namespace fbzz::renderer
