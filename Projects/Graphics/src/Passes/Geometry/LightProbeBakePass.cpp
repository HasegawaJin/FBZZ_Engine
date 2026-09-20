/// @file    LightProbeBakePass.cpp
/// @brief   Light Probe Volume の各プローブ位置で 6 面を描き、L2 球面調和へ射影して SH ボリュームへ書く。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 1 プローブ = 6 面ぶんのシーン描画なので、probesPerFrame 個ずつ複数フレームへ分けて焼く。
/// @note 面は深度付きの 2D RT。キューブ RT は深度を持たず、壁の向こうの物体が描画順しだいで手前に出て空の遮蔽が崩れるため使わない。
/// @note 射影 CS は生のボリュームへ書き、膨張 CS が壁に埋まったプローブを周りで埋めて描画用ボリュームを作る。
/// @see Docs/design/light-probe-gi.md
#include "RenderScenePassHelpers.hpp"
#include <Graphics/Effects/RenderProbeInput.hpp>
#include <Graphics/Renderer/ComputeCall.hpp>
#include <Math/MathUtils.hpp>
namespace fbzz::renderer {
namespace {
/// @brief LightProbeProject.cs.hlsl の b0。
/// @note LAYOUT: Assets/Shaders/IBL/LightProbeProject.cs.hlsl の LightProbeProjectConstants と一致させること。
struct LightProbeProjectCB {
    math::Matrix4 faceInvViewProj[6];
    math::Vector3 probePos;
    uint32_t      faceSize;
    uint32_t      probeCoord[3];
    uint32_t      gridZ;
    float         skyRadianceLimit;
    float         surfaceRadianceLimit;
    float         environmentMip;
    uint32_t      hasEnvironment;
    float         deringing;
    uint32_t      hasFacing;
    float         backfaceLimit;
    float         _pad;
};
static_assert(sizeof(LightProbeProjectCB) == kLightProbeProjectCBSize,
              "LightProbeProjectCB must match LightProbeProjectConstants (448 bytes)");

/// @brief LightProbeDilate.cs.hlsl の b0。
/// @note LAYOUT: Assets/Shaders/IBL/LightProbeDilate.cs.hlsl の LightProbeDilateConstants と一致させること。
struct LightProbeDilateCB {
    uint32_t grid[3];
    float    validLimit;
};
static_assert(sizeof(LightProbeDilateCB) == kLightProbeDilateCBSize,
              "LightProbeDilateCB must match LightProbeDilateConstants (16 bytes)");

/// @note 面の色・深度・表裏を置く CS のスロット。LightProbeProject.cs.hlsl の LIGHT_PROBE_*_SLOT と一致させること。
constexpr uint32_t kFaceColorSlot   = 0;
constexpr uint32_t kFaceDepthSlot   = 6;
constexpr uint32_t kEnvironmentSlot = 16;
constexpr uint32_t kFaceFacingSlot  = 20;
/// @note SH 係数 7 枚を Z 方向へ積む。LightProbeGI.hlsli の kLightProbeSHSlices と一致させること。
constexpr int kSHSlices = 7;

/// @note 空の輝度上限は IrradianceConvolution.cs.hlsl の DIFFUSE_RADIANCE_LIMIT と同じ値にする。箱の外 (キューブ) と中 (プローブ) で太陽ピークの扱いが違うと境目で明るさが段になる。
constexpr float kSkyRadianceLimit     = 4.0f;
constexpr float kSurfaceRadianceLimit = 64.0f;
/// @note 環境キューブは 1 段落として引く。32 px の面から見れば 0 段でも細部は拾えず、エイリアスだけが残る。
constexpr float kEnvironmentMip       = 1.0f;
/// @note 裏面が見える立体角の割合がこれを超えたら «形状の中» とみなす。開いた形状の縁を覗き込むだけなら 1 割程度に収まる。
constexpr float kBackfaceLimit        = 0.25f;
/// @note 膨張 CS がこれ以上の有効度を «有効» と扱う。
constexpr float kValidLimit           = 0.5f;
constexpr float kCaptureNear = 0.05f;
constexpr float kCaptureFar  = 300.0f;

struct CubeFaceBasis { math::Vector3 forward; math::Vector3 up; };
/// @note ReflectionProbeCapturePass と同じ 6 方向。面の中身は CS が逆 ViewProjection から方向を復元して読むので、D3D のキューブ面の並びに合わせる必要は無い。
constexpr CubeFaceBasis kCubeFaces[6] = {
    {{ 1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    {{-1.0f,  0.0f,  0.0f}, { 0.0f, 1.0f,  0.0f}},
    {{ 0.0f,  1.0f,  0.0f}, { 0.0f, 0.0f, -1.0f}},
    {{ 0.0f, -1.0f,  0.0f}, { 0.0f, 0.0f,  1.0f}},
    {{ 0.0f,  0.0f,  1.0f}, { 0.0f, 1.0f,  0.0f}},
    {{ 0.0f,  0.0f, -1.0f}, { 0.0f, 1.0f,  0.0f}},
};

math::Vector3 ProbePosition(const math::Vector3& boxMin, const math::Vector3& boxSize,
                            const std::array<int, 3>& grid, int x, int y, int z)
{
    const auto t = [](int i, int n) { return (static_cast<float>(i) + 0.5f) / static_cast<float>(n); };
    return { boxMin.x + boxSize.x * t(x, grid[0]), boxMin.y + boxSize.y * t(y, grid[1]),
             boxMin.z + boxSize.z * t(z, grid[2]) };
}

/// @brief 捕捉で描く 1 物体ぶん。
struct CaptureItem {
    const renderer::RenderObject* object;
    const renderer::RenderMeshItem* mesh;
    const renderer::RenderMaterial* material;
    renderer::ResourceHandle<renderer::PipelineStateTag> pso;
};

/// @brief この面の視錐台に入る不透明・非スキンの MeshRenderer を集める。
/// @note 半透明・加算は «光を遮る面» ではないので焼かない。混ぜると炎や光の粒の色がそのまま環境光になる。
void CollectCaptureItems(RenderPassContext& ctx, const RenderLightProbeInput& volume, const math::Frustum& frustum,
                         std::vector<CaptureItem>& items)
{
    items.clear();
    const auto& input = SceneInput(ctx);
    for (const auto& object : input.objects) {
        if (object.skinned || !object.colorEligible || !MatchesView(ctx, object) ||
            (object.sourceIndex == volume.sourceIndex && object.sourceGeneration == volume.sourceGeneration)) continue;
        const auto& item = input.items[object.firstItem];
        const auto& material = item.material;
        if (!HasColorGeometry(item) || !material.valid || !material.shader.IsValid() ||
            material.capabilities.blend != renderer::BlendMode::OPAQUE_BLEND) continue;
        if (object.boundsRadius > 0.0f && !frustum.IntersectsSphere(object.boundsCenter, object.boundsRadius)) continue;
        items.push_back({ &object, &item, &material,
            GetOrCreateMaterialPSO(ctx.resources, material.capabilities.blend,
                material.doubleSided, material.depthBias, material.depthBiasSlope, false) });
    }
}

void UpdateObjectCB(RenderPassContext& ctx, const renderer::RenderObject& go)
{
    PerObjectCB object{};
    object.world = go.world;
    object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
    ctx.resources.Update(ctx.handles.objectCB, &object, sizeof(object));
}

/// @brief 1 面ぶんのシーンを描く。有効判定が要るなら同じ物体を表裏だけの面へも描く。
/// @return この面の逆 ViewProjection (CS が画素の方向を復元するのに使う)。
math::Matrix4 RenderFace(RenderPassContext& ctx, const RenderLightProbeInput& volume,
                         const math::Vector3& position, uint32_t face,
                         renderer::ResourceHandle<renderer::TextureTag> probeInput,
                         std::vector<CaptureItem>& items)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const math::Matrix4 projection = math::Matrix4::Perspective(math::ToRad(90.0f), 1.0f, kCaptureNear, kCaptureFar);
    const math::Matrix4 view = math::Matrix4::LookAt(position, position + kCubeFaces[face].forward, kCubeFaces[face].up);
    PerFrameCB frame{};
    frame.view = view;
    frame.projection = projection;
    frame.viewProjection = projection * view;
    frame.invViewProjection = math::Matrix4::Inverse(frame.viewProjection);
    frame.cameraPos = position;
    frame.nearZ = kCaptureNear;
    frame.farZ = kCaptureFar;
    resources.Update(h.lightProbeCaptureFrameCB, &frame, sizeof(frame));

    CollectCaptureItems(ctx, volume, math::Frustum::FromViewProjection(frame.viewProjection), items);

    ctx.renderer.SetRenderTarget(volume.runtimeFaces[face], resources);
    /// @note 深度もクリア値 1 へ戻る。CS はこの値のまま残った画素を «空» として環境キューブで埋める。
    ctx.renderer.Clear({ 0.0f, 0.0f, 0.0f, 0.0f });
    for (const CaptureItem& item : items) {
        UpdateObjectCB(ctx, *item.object);
        renderer::DrawCall draw{};
        draw.vertexBuffer = item.mesh->vertexBuffer;
        draw.indexBuffer = item.mesh->indexBuffer;
        draw.indexCount = item.mesh->indexCount;
        draw.vertexCount = item.mesh->vertexCount;
        draw.shader = item.material->shader;
        draw.pipelineState = item.pso;
        draw.constantBuffers[0] = h.lightProbeCaptureFrameCB;
        draw.constantBuffers[1] = h.objectCB;
        draw.constantBuffers[2] = item.material->paramsBuffer;
        draw.constantBuffers[3] = h.lightCB;
        draw.constantBuffers[4] = h.shadowCB;
        draw.constantBuffers[8] = h.lightProbeCaptureAdvancedCB;
        for (size_t i = 0; i < item.material->textures.size() && i < 8; ++i)
            if (item.material->textures[i].IsValid()) draw.textures[i] = item.material->textures[i];
        draw.textures[8]  = resources.GetDepthTexture(ctx.Res().Target("ShadowMap"));
        draw.textures[16] = h.iblIrradiance;
        draw.textures[17] = h.iblPrefilter;
        draw.textures[18] = h.iblBrdfLut;
        /// @note クラスタリストはメインカメラの視錐台向けなので、プローブ位置から描くここでは Linear へ落とす。
        BindForwardShadingResources(draw, ctx, /*forceLinearLights=*/true);
        /// @note 多重反射の焼きでは «このボリュームの前回の結果» だけを受けた面を描く。メインビューが選んだボリュームとは限らないので上書きする。
        draw.textures[22] = probeInput;
        draw.textures[21] = {};
        ctx.renderer.Submit(draw, resources);
    }

    if (volume.rejectInsideGeometry && h.lightProbeFacingShader.IsValid()) {
        /// @note 表裏はカリング無しで描かないと分からない (裏面カリングのマテリアル PSO では裏が消え、壁の中から外が素通しで見える)。
        ctx.renderer.SetRenderTarget(volume.runtimeFacing[face], resources);
        ctx.renderer.Clear({ 1.0f, 1.0f, 1.0f, 1.0f });
        const auto cullNone = GetOrCreateMaterialPSO(resources, renderer::BlendMode::OPAQUE_BLEND, /*doubleSided=*/true);
        for (const CaptureItem& item : items) {
            UpdateObjectCB(ctx, *item.object);
            renderer::DrawCall draw{};
            draw.vertexBuffer = item.mesh->vertexBuffer;
            draw.indexBuffer = item.mesh->indexBuffer;
            draw.indexCount = item.mesh->indexCount;
            draw.vertexCount = item.mesh->vertexCount;
            draw.shader = h.lightProbeFacingShader;
            draw.pipelineState = cullNone;
            draw.constantBuffers[0] = h.lightProbeCaptureFrameCB;
            draw.constantBuffers[1] = h.objectCB;
            ctx.renderer.Submit(draw, resources);
        }
    }
    return frame.invViewProjection;
}

void BakeProbe(RenderPassContext& ctx, const RenderLightProbeInput& volume, int index,
               renderer::ResourceHandle<renderer::TextureTag> probeInput, std::vector<CaptureItem>& items)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const auto& grid = volume.runtimeGrid;
    const int x = index % grid[0];
    const int y = (index / grid[0]) % grid[1];
    const int z = index / (grid[0] * grid[1]);
    const math::Vector3 position = ProbePosition(volume.boxMin, volume.boxSize, grid, x, y, z);

    LightProbeProjectCB project{};
    for (uint32_t face = 0; face < 6; ++face)
        project.faceInvViewProj[face] = RenderFace(ctx, volume, position, face, probeInput, items);
    ctx.renderer.SetRenderTarget({}, resources);

    const bool useFacing = volume.rejectInsideGeometry && h.lightProbeFacingShader.IsValid();
    project.probePos = position;
    project.faceSize = static_cast<uint32_t>(volume.runtimeFaceSize);
    project.probeCoord[0] = static_cast<uint32_t>(x);
    project.probeCoord[1] = static_cast<uint32_t>(y);
    project.probeCoord[2] = static_cast<uint32_t>(z);
    project.gridZ = static_cast<uint32_t>(grid[2]);
    project.skyRadianceLimit = kSkyRadianceLimit;
    project.surfaceRadianceLimit = kSurfaceRadianceLimit;
    project.environmentMip = kEnvironmentMip;
    project.hasEnvironment = h.iblPrefilter.IsValid() ? 1u : 0u;
    project.deringing = std::clamp(volume.deringing, 0.0f, 1.0f);
    project.hasFacing = useFacing ? 1u : 0u;
    project.backfaceLimit = kBackfaceLimit;
    resources.Update(h.lightProbeProjectCB, &project, sizeof(project));

    renderer::ComputeCall call;
    call.shader = h.lightProbeProjectCS;
    call.constantBuffers[0] = h.lightProbeProjectCB;
    for (uint32_t face = 0; face < 6; ++face) {
        call.srvInputs[kFaceColorSlot + face] = resources.GetColorTexture(volume.runtimeFaces[face], 0);
        call.srvInputs[kFaceDepthSlot + face] = resources.GetDepthTexture(volume.runtimeFaces[face]);
        if (useFacing)
            call.srvInputs[kFaceFacingSlot + face] = resources.GetColorTexture(volume.runtimeFacing[face], 0);
    }
    if (h.iblPrefilter.IsValid()) call.srvInputs[kEnvironmentSlot] = h.iblPrefilter;
    call.uavOutputs[0] = volume.runtimeRawVolume;
    ctx.renderer.Dispatch(call, resources);
}

/// @brief 生のボリュームから、壁に埋まったプローブを周りで埋めた描画用ボリュームを作る。
/// @note 毎回全プローブを書き直すので、焼き途中でも描画用ボリュームは «その時点で焼けた値» を持つ。有効判定を切っているときは射影 CS が有効度 1 を書くので素通しになる。
void DilateVolume(RenderPassContext& ctx, const RenderLightProbeInput& volume)
{
    auto& h = ctx.handles;
    if (!h.lightProbeDilateCS.IsValid() || !h.lightProbeDilateCB.IsValid()) return;
    LightProbeDilateCB data{};
    for (int axis = 0; axis < 3; ++axis) data.grid[axis] = static_cast<uint32_t>(volume.runtimeGrid[axis]);
    data.validLimit = kValidLimit;
    ctx.resources.Update(h.lightProbeDilateCB, &data, sizeof(data));

    renderer::ComputeCall call;
    call.shader = h.lightProbeDilateCS;
    call.constantBuffers[0] = h.lightProbeDilateCB;
    call.srvInputs[0] = volume.runtimeRawVolume;
    call.uavOutputs[0] = volume.runtimeVolume;
    call.dispatchX = (data.grid[0] + 3u) / 4u;
    call.dispatchY = (data.grid[1] + 3u) / 4u;
    call.dispatchZ = (data.grid[2] + 3u) / 4u;
    ctx.renderer.Dispatch(call, ctx.resources);
}

}
void BakeLightProbe(RenderPassContext& ctx, const RenderLightProbeInput& input, int index, ResourceHandle<TextureTag> probeInput)
{
    std::vector<CaptureItem> items;
    BakeProbe(ctx, input, index, probeInput, items);
}
void DilateLightProbeVolume(RenderPassContext& ctx, const RenderLightProbeInput& input) { DilateVolume(ctx, input); }
}
