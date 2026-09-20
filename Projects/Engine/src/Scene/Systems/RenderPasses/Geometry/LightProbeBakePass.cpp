/// @file    LightProbeBakePass.cpp
/// @brief   Light Probe Volume の各プローブ位置で 6 面を描き、L2 球面調和へ射影して SH ボリュームへ書く。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @note 1 プローブ = 6 面ぶんのシーン描画なので、probesPerFrame 個ずつ複数フレームへ分けて焼く。
/// @note 面は深度付きの 2D RT。キューブ RT は深度を持たず、壁の向こうの物体が描画順しだいで手前に出て空の遮蔽が崩れるため使わない。
/// @note 射影 CS は生のボリュームへ書き、膨張 CS が壁に埋まったプローブを周りで埋めて描画用ボリュームを作る。
/// @see Docs/design/light-probe-gi.md
#include "GeometryPasses.hpp"
#include <Engine/Renderer/ComputeCall.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Scene/Components/LightProbeVolumeComponent.hpp>
#include <Engine/Scene/Components/MaterialComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Frustum.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Matrix4.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {
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

void RestartBake(LightProbeVolumeComponent& volume)
{
    volume.runtimeCursor = 0;
    volume.runtimePass   = 0;
    volume.runtimeBaking = true;
}

/// @brief 焼き結果を左右する設定の要約。箱と格子は別に見ている。
uint64_t SettingsKey(const LightProbeVolumeComponent& volume)
{
    const auto quantize = [](float v) {
        return static_cast<uint64_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 1000.0f));
    };
    uint64_t key = static_cast<uint64_t>(std::clamp(volume.captureResolution, 8, 128));
    key = key * 16u + static_cast<uint64_t>(std::clamp(volume.bounces, 1, 8));
    key = key * 1024u + quantize(volume.deringing);
    key = key * 2u + (volume.rejectInsideGeometry ? 1u : 0u);
    return key + 1u;
}

/// @brief 格子と面の解像度に合わせて GPU 資源を用意する。
/// @return 焼けない (資源を作れない) なら false。
bool EnsureRuntime(renderer::ResourceManager& resources, LightProbeVolumeComponent& volume)
{
    const auto grid = volume.ClampedGrid();
    if (!volume.runtimeVolume.IsValid() || !volume.runtimeRawVolume.IsValid() || volume.runtimeGrid != grid) {
        for (auto* texture : { &volume.runtimeVolume, &volume.runtimeRawVolume }) {
            if (texture->IsValid()) resources.Release(*texture);
            *texture = resources.CreateComputeTexture3D(
                static_cast<uint32_t>(grid[0]), static_cast<uint32_t>(grid[1]),
                static_cast<uint32_t>(grid[2] * kSHSlices));
        }
        volume.runtimeGrid  = grid;
        /// @note 作り直した直後の中身は未定義。1 回目を焼き終えるまで描画には使わない。
        volume.runtimeReady = false;
        RestartBake(volume);
    }
    const int faceSize = std::clamp(volume.captureResolution, 8, 128);
    const auto missing = [](const auto& faces) {
        return std::any_of(faces.begin(), faces.end(), [](const auto& face) { return !face.IsValid(); });
    };
    if (missing(volume.runtimeFaces) || missing(volume.runtimeFacing) || volume.runtimeFaceSize != faceSize) {
        for (auto* faces : { &volume.runtimeFaces, &volume.runtimeFacing }) {
            for (auto& face : *faces) {
                if (face.IsValid()) resources.Release(face);
                face = resources.CreateRenderTarget(static_cast<uint32_t>(faceSize), static_cast<uint32_t>(faceSize), 1);
            }
        }
        volume.runtimeFaceSize = faceSize;
    }
    return volume.runtimeVolume.IsValid() && volume.runtimeRawVolume.IsValid()
        && !missing(volume.runtimeFaces) && !missing(volume.runtimeFacing);
}

math::Vector3 BoxMin(const GameObject& owner, const LightProbeVolumeComponent& volume)
{
    const math::Vector3 extents{ std::max(volume.boxExtents.x, 0.01f), std::max(volume.boxExtents.y, 0.01f),
                                 std::max(volume.boxExtents.z, 0.01f) };
    return owner.transform.worldPosition - extents;
}

math::Vector3 BoxSize(const LightProbeVolumeComponent& volume)
{
    return { std::max(volume.boxExtents.x, 0.01f) * 2.0f, std::max(volume.boxExtents.y, 0.01f) * 2.0f,
             std::max(volume.boxExtents.z, 0.01f) * 2.0f };
}

/// @brief 格子 (x, y, z) のプローブ位置。箱を格子に割った各セルの中心。
/// @note LightProbeGI.hlsli の «texel 中心 = プローブ» と同じ並び。角に置くと壁面上のプローブが壁の裏を見て漏れる。
math::Vector3 ProbePosition(const math::Vector3& boxMin, const math::Vector3& boxSize,
                            const std::array<int, 3>& grid, int x, int y, int z)
{
    const auto t = [](int i, int n) { return (static_cast<float>(i) + 0.5f) / static_cast<float>(n); };
    return { boxMin.x + boxSize.x * t(x, grid[0]), boxMin.y + boxSize.y * t(y, grid[1]),
             boxMin.z + boxSize.z * t(z, grid[2]) };
}

/// @brief 捕捉で描く 1 物体ぶん。
struct CaptureItem {
    const GameObject*     object;
    const renderer::Mesh* mesh;
    renderer::Material*   material;
    renderer::ResourceHandle<renderer::PipelineStateTag> pso;
};

/// @brief この面の視錐台に入る不透明・非スキンの MeshRenderer を集める。
/// @note 半透明・加算は «光を遮る面» ではないので焼かない。混ぜると炎や光の粒の色がそのまま環境光になる。
void CollectCaptureItems(RenderPassContext& ctx, const GameObject& owner, const math::Frustum& frustum,
                         std::vector<CaptureItem>& items)
{
    items.clear();
    for (auto& go : ctx.scene.GameObjects()) {
        if (&go == &owner || !ShouldRenderGameObject(go, ctx.cullingMask)) continue;
        auto* meshRenderer = go.GetComponent<MeshRenderer>();
        auto* material = go.GetComponent<MaterialComponent>();
        if (!meshRenderer || !meshRenderer->enabled || !meshRenderer->mesh
            || !material || !material->EnsureMaterialAsset()) continue;
        const auto* mesh = meshRenderer->mesh;
        if (!mesh || mesh->isSkinned || !mesh->vertexBuffer.IsValid() || !mesh->indexBuffer.IsValid()) continue;
        if (material->GetBlendMode() != renderer::BlendMode::OPAQUE_BLEND) continue;
        if (mesh->boundsRadius > 0.0f) {
            const WorldBounds bounds = ComputeWorldBounds(go.transform, *mesh);
            if (!frustum.IntersectsSphere(bounds.center, bounds.radius)) continue;
        }
        auto* gpuMaterial = SyncMaterial(*material, ctx.resources);
        if (!gpuMaterial || !gpuMaterial->shader.IsValid()) continue;
        items.push_back({ &go, mesh, gpuMaterial,
                          GetOrCreateMaterialPSO(ctx.resources, *material) });
    }
}

void UpdateObjectCB(RenderPassContext& ctx, const GameObject& go)
{
    PerObjectCB object{};
    object.world = go.transform.GetWorldMatrix();
    object.worldInvTranspose = math::Matrix4::InverseTransposeAffine(object.world);
    ctx.resources.Update(ctx.handles.objectCB, &object, sizeof(object));
}

/// @brief 1 面ぶんのシーンを描く。有効判定が要るなら同じ物体を表裏だけの面へも描く。
/// @return この面の逆 ViewProjection (CS が画素の方向を復元するのに使う)。
math::Matrix4 RenderFace(RenderPassContext& ctx, const GameObject& owner, const LightProbeVolumeComponent& volume,
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

    CollectCaptureItems(ctx, owner, math::Frustum::FromViewProjection(frame.viewProjection), items);

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

void BakeProbe(RenderPassContext& ctx, const GameObject& owner, LightProbeVolumeComponent& volume, int index,
               renderer::ResourceHandle<renderer::TextureTag> probeInput, std::vector<CaptureItem>& items)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const auto& grid = volume.runtimeGrid;
    const int x = index % grid[0];
    const int y = (index / grid[0]) % grid[1];
    const int z = index / (grid[0] * grid[1]);
    const math::Vector3 position = ProbePosition(BoxMin(owner, volume), BoxSize(volume), grid, x, y, z);

    LightProbeProjectCB project{};
    for (uint32_t face = 0; face < 6; ++face)
        project.faceInvViewProj[face] = RenderFace(ctx, owner, volume, position, face, probeInput, items);
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
void DilateVolume(RenderPassContext& ctx, const LightProbeVolumeComponent& volume)
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

/// @brief 箱までの距離の二乗。中なら 0。
float DistanceSqToBox(const math::Vector3& point, const math::Vector3& boxMin, const math::Vector3& boxSize)
{
    const float dx = std::max({ boxMin.x - point.x, 0.0f, point.x - (boxMin.x + boxSize.x) });
    const float dy = std::max({ boxMin.y - point.y, 0.0f, point.y - (boxMin.y + boxSize.y) });
    const float dz = std::max({ boxMin.z - point.z, 0.0f, point.z - (boxMin.z + boxSize.z) });
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

void FillLightProbeVolumeConstants(const GameObject& owner, const LightProbeVolumeComponent& volume,
                                   ProbeVolumeParamsCB& data)
{
    const math::Vector3 boxSize = BoxSize(volume);
    data.boxMin     = BoxMin(owner, volume);
    data.intensity  = std::max(volume.intensity, 0.0f);
    data.invSize    = { 1.0f / boxSize.x, 1.0f / boxSize.y, 1.0f / boxSize.z };
    data.fade       = std::max(volume.edgeFade, 0.0f);
    for (int axis = 0; axis < 3; ++axis)
        data.grid[axis] = static_cast<uint32_t>(volume.runtimeGrid[axis]);
    data.normalBias = std::max(volume.normalBias, 0.0f);
}

LightProbeVolumeSelection ExecuteLightProbeBakePass(RenderPassContext& ctx, const AdvancedGraphicsCB& mainData)
{
    auto& h = ctx.handles;
    auto& resources = ctx.resources;
    const bool canBake = h.lightProbeProjectCS.IsValid() && h.lightProbeProjectCB.IsValid()
        && h.lightProbeCaptureFrameCB.IsValid() && h.lightProbeCaptureAdvancedCB.IsValid();

    struct Candidate { LightProbeVolumeSelection::Entry entry; float distanceSq; float volumeSize; };
    std::vector<Candidate> ready;
    std::vector<CaptureItem> items;
    for (auto& go : ctx.scene.GameObjects()) {
        auto* volume = go.GetComponent<LightProbeVolumeComponent>();
        if (!volume || !volume->enabled || !go.activeInHierarchy() || !canBake) continue;
        if (!EnsureRuntime(resources, *volume)) continue;
        if (volume->bakeRequested) {
            volume->bakeRequested = false;
            RestartBake(*volume);
        }
        /// @note 箱が動いた・大きさが変わった・焼きの設定が変わったら焼き直す。古い結果は焼き上がるまで使う。
        const math::Vector3 boxMin = BoxMin(go, *volume);
        const math::Vector3 boxSize = BoxSize(*volume);
        if ((boxMin - volume->runtimeBoxMin).LengthSq() > 1.0e-6f
            || (boxSize - volume->runtimeBoxSize).LengthSq() > 1.0e-6f) {
            volume->runtimeBoxMin = boxMin;
            volume->runtimeBoxSize = boxSize;
            RestartBake(*volume);
        }
        if (const uint64_t key = SettingsKey(*volume); key != volume->runtimeSettingsKey) {
            volume->runtimeSettingsKey = key;
            RestartBake(*volume);
        }

        if (volume->runtimeBaking) {
            /// @note 捕捉で描く面は、メインビューと同じ IBL・天候を受けつつ «メインカメラの画面» に依存する項を切る。
            ///       画面空間 AO / 接触影は別視点の画素を読むことになり、面に無関係な黒が焼き込まれる。
            AdvancedGraphicsCB capture = mainData;
            capture.screenAoStrength = 0.0f;
            capture.screenContactShadowStrength = 0.0f;
            capture.probeVolumes[0] = {};
            capture.probeVolumes[1] = {};
            capture.probeSpecularOcclusion = std::clamp(volume->specularOcclusion, 0.0f, 1.0f);
            /// @note 2 回目以降は前回の結果を受けた面を描くので、反射が 1 回ずつ増える (同じボリュームを読み書きしながら進めるので、途中は新旧が混ざる)。
            const bool multiBounce = volume->bounces > 1 && volume->runtimeReady && volume->runtimePass > 0;
            if (multiBounce) FillLightProbeVolumeConstants(go, *volume, capture.probeVolumes[0]);
            resources.Update(h.lightProbeCaptureAdvancedCB, &capture, sizeof(capture));
            resources.Update(h.lightCB, &ctx.lightData, sizeof(ctx.lightData));

            const auto probeInput = multiBounce ? volume->runtimeVolume
                                                : renderer::ResourceHandle<renderer::TextureTag>{};
            const int probeCount = volume->ProbeCount();
            const int budget = std::clamp(volume->probesPerFrame, 1, 256);
            for (int n = 0; n < budget && volume->runtimeCursor < probeCount; ++n)
                BakeProbe(ctx, go, *volume, volume->runtimeCursor++, probeInput, items);
            DilateVolume(ctx, *volume);

            if (volume->runtimeCursor >= probeCount) {
                volume->runtimeCursor = 0;
                volume->runtimeReady = true;
                ++volume->runtimePass;
                if (volume->runtimePass >= std::clamp(volume->bounces, 1, 8)) {
                    /// @note 常時更新は «前回の結果を受けて焼く» を回し続ける。反射の回数は収束した値に落ち着く。
                    volume->runtimeBaking = volume->realtimeUpdate;
                    volume->runtimePass = std::clamp(volume->bounces, 1, 8);
                }
            }
        }

        if (!volume->runtimeReady) continue;
        ready.push_back({ { &go, volume }, DistanceSqToBox(ctx.camera.m_position, boxMin, boxSize),
                          boxSize.x * boxSize.y * boxSize.z });
    }

    /// @note 画素ごとに引けるのは 2 つまで。カメラに近い 2 つを選び、小さい箱を内側 (上に重ねる側) にする。
    std::sort(ready.begin(), ready.end(), [](const Candidate& a, const Candidate& b) {
        return a.distanceSq != b.distanceSq ? a.distanceSq < b.distanceSq : a.volumeSize < b.volumeSize;
    });
    LightProbeVolumeSelection selected;
    if (ready.empty()) return selected;
    if (ready.size() == 1) {
        selected.inner = ready[0].entry;
        return selected;
    }
    const bool firstIsInner = ready[0].volumeSize <= ready[1].volumeSize;
    selected.inner = (firstIsInner ? ready[0] : ready[1]).entry;
    selected.outer = (firstIsInner ? ready[1] : ready[0]).entry;
    return selected;
}

} // namespace fbzz::scene
