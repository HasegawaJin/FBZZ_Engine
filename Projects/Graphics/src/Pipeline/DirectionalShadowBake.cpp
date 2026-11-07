/// @file    DirectionalShadowBake.cpp
/// @brief   固定投影の方向ライト深度を生成し、最遠カスケードへ公開する。
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <Graphics/Pipeline/DirectionalShadowBake.hpp>

#include <Graphics/Passes/Geometry/GeometryPasses.hpp>
#include <Graphics/Renderer/DrawCall.hpp>
#include <Graphics/Renderer/IRenderer.hpp>
#include <Graphics/Renderer/RenderScene.hpp>
#include <Graphics/Renderer/ResourceManager.hpp>
#include <Core/Profiler/ProfileScope.hpp>
#include <bit>
#include <cmath>
#include <utility>

namespace fbzz::renderer {
namespace {

constexpr uint32_t kDirectionalShadowBakeMaxResolution = 4096;

bool ValidBakeBounds(const ShadowBounds& bounds)
{
    return bounds.valid && std::isfinite(bounds.center.x) && std::isfinite(bounds.center.y)
        && std::isfinite(bounds.center.z) && std::isfinite(bounds.radius) && bounds.radius >= 1.0f;
}

bool ValidBakeProjection(const ShadowCascade& cascade, const math::Vector3& center)
{
    for (const auto& row : cascade.viewProjection.m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    return cascade.frustum.Contains(center) && cascade.texelWorldSize > 0.0f
        && std::isfinite(cascade.texelWorldSize);
}

bool ValidBakeResolution(uint32_t resolution)
{
    return resolution >= 8u && resolution <= kDirectionalShadowBakeMaxResolution;
}

bool UnitBakeDirection(const math::Vector3& direction, math::Vector3& unit)
{
    const float lengthSquared = direction.LengthSq();
    if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12f) return false;
    unit = direction / std::sqrt(lengthSquared);
    return true;
}

bool PrepareCascadeToBakeClip(const math::Matrix4& bakeVP, const math::Matrix4& realtimeVP, math::Matrix4& transform)
{
    /// @note 正射影を前提とする。ワールドサイズの VP は行列式が小さいため、基底だけを逆変換する。
    if (realtimeVP.m[3][0] != 0.0f || realtimeVP.m[3][1] != 0.0f
        || realtimeVP.m[3][2] != 0.0f || realtimeVP.m[3][3] != 1.0f) return false;
    bool sameProjection = true;
    auto basis = math::Matrix4::Identity();
    auto clipToBasis = math::Matrix4::Identity();
    math::Vector3 axes[3];
    for (int row = 0; row < 3; ++row) {
        const auto& coefficients = realtimeVP.m[row];
        const math::Vector3 axis{coefficients[0], coefficients[1], coefficients[2]};
        const float scale = axis.Length();
        if (!std::isfinite(scale) || scale <= 0.0f || !std::isfinite(coefficients[3])) return false;
        axes[row] = axis / scale;
        basis.m[row][0] = axes[row].x;
        basis.m[row][1] = axes[row].y;
        basis.m[row][2] = axes[row].z;
        clipToBasis.m[row][row] = 1.0f / scale;
        clipToBasis.m[row][3] = -coefficients[3] / scale;
        for (int column = 0; column < 4; ++column) sameProjection &= coefficients[column] == bakeVP.m[row][column];
    }
    if (sameProjection) {
        transform = math::Matrix4::Identity();
        return true;
    }
    const float determinant = math::Vector3::Dot(axes[0], math::Vector3::Cross(axes[1], axes[2]));
    if (!std::isfinite(determinant) || std::abs(determinant) < 0.5f) return false;
    const auto clipToWorld = math::Matrix4::Inverse(basis) * clipToBasis;
    auto cascadeToBake = bakeVP * clipToWorld;
    for (const auto& row : cascadeToBake.m)
        for (float value : row)
            if (!std::isfinite(value)) return false;
    constexpr float kClipGuard = 1.0e-4f;
    /// @note 平行な光線に限り、復元元の XY をまだ未知の深度と切り離せる。
    if (!(cascadeToBake.m[2][2] > 0.0f) || std::abs(cascadeToBake.m[0][2]) >= kClipGuard
        || std::abs(cascadeToBake.m[1][2]) >= kClipGuard) return false;
    cascadeToBake.m[0][2] = cascadeToBake.m[1][2] = 0.0f;
    for (uint32_t index = 0; index < 8; ++index) {
        const float x = (index & 1u) != 0 ? 1.0f : -1.0f;
        const float y = (index & 2u) != 0 ? 1.0f : -1.0f;
        const float z = (index & 4u) != 0 ? 1.0f : 0.0f;
        const math::Vector4 clip{x, y, z, 1.0f};
        const auto world = clipToWorld * clip;
        const auto roundTrip = realtimeVP * world;
        if (!(std::abs(roundTrip.x - x) <= kClipGuard && std::abs(roundTrip.y - y) <= kClipGuard
            && std::abs(roundTrip.z - z) <= kClipGuard)) return false;
        const auto bakedClip = cascadeToBake * clip;
        /// @note 境界に丸め余裕を残す。8 頂点の内包で元の最遠 CSM 全体を覆う。
        if (!(std::abs(bakedClip.x) < 1.0f - kClipGuard && std::abs(bakedClip.y) < 1.0f - kClipGuard
            && bakedClip.z > kClipGuard && bakedClip.z < 1.0f - kClipGuard)) return false;
    }
    transform = cascadeToBake;
    return true;
}

ShadowCascade MakeFixedBakeCascade(const ShadowBounds& bounds, const math::Vector3& direction,
    uint32_t resolution, float shadowBias)
{
    /// @note 視点追従カスケードの中心移動と、光源側へ延びるキャスターの深度範囲を含む。
    const float radius = bounds.radius;
    const float pullback = 2.0f * radius + 20.0f;
    const float farPlane = pullback + radius + 20.0f;
    const math::Vector3 up = std::abs(direction.y) > 0.99f ? math::Vector3{1, 0, 0} : math::Vector3{0, 1, 0};
    ShadowCascade cascade;
    cascade.eyePos = bounds.center - direction * pullback;
    cascade.view = math::Matrix4::LookAt(cascade.eyePos, bounds.center, up);
    cascade.viewProjection = math::Matrix4::Orthographic(-radius, radius, -radius, radius, 1.0f, farPlane)
        * cascade.view;
    cascade.frustum = math::Frustum::FromViewProjection(cascade.viewProjection);
    cascade.viewportSize = resolution;
    cascade.texelWorldSize = (2.0f * radius) / static_cast<float>(resolution);
    cascade.biasNDC = (0.005f * shadowBias) / (farPlane - 1.0f);
    return cascade;
}

bool BakeLayerVisible(uint32_t layer, uint32_t mask)
{
    return layer < 32 && (mask & (1u << layer)) != 0;
}

struct StaticDepthDraw {
    math::Matrix4 world;
    ResourceHandle<BufferTag> vertices;
    ResourceHandle<BufferTag> indices;
    ResourceHandle<PipelineStateTag> pipeline;
    uint32_t indexCount;
};

bool CollectBakeDraws(ResourceManager& resources, const RenderScene& scene, uint32_t mask, bool includeMeshes,
    std::vector<StaticDepthDraw>& draws)
{
    const auto terrainPipeline = GetOrCreateMaterialPSO(resources, BlendMode::OPAQUE_BLEND, false, 0, 0.0f, false);
    if (!terrainPipeline.IsValid()) return false;
    const auto append = [&](const math::Matrix4& world, ResourceHandle<BufferTag> vb,
                            ResourceHandle<BufferTag> ib, ResourceHandle<PipelineStateTag> pipeline, uint32_t count) {
        BufferResourceMetadata vertices;
        BufferResourceMetadata indices;
        if (!pipeline.IsValid() || !resources.GetBufferMetadata(vb, vertices) || !vertices.live || vertices.gpuWritable
            || !resources.GetBufferMetadata(ib, indices) || !indices.live || !indices.indexBuffer
            || static_cast<uint64_t>(count) * sizeof(uint32_t) > indices.sizeBytes) return false;
        draws.push_back({world, vb, ib, pipeline, count});
        return true;
    };
    if (includeMeshes) {
        for (const auto& object : scene.Objects()) {
            if (!object.shadowBakeStatic || !BakeLayerVisible(object.layer, mask)) continue;
            if (object.firstItem > scene.items.size() || object.itemCount > scene.items.size() - object.firstItem) return false;
            for (size_t index = object.firstItem; index < static_cast<size_t>(object.firstItem) + object.itemCount; ++index) {
                const auto& item = scene.items[index];
                if (!IsDirectionalShadowBakeMesh(object, item)) continue;
                const auto& material = item.material;
                const auto pipeline = GetOrCreateMaterialPSO(resources, BlendMode::OPAQUE_BLEND, material.doubleSided,
                    material.depthBias, material.depthBiasSlope, false);
                if (!append(object.world, item.vertexBuffer, item.indexBuffer, pipeline, item.indexCount)) return false;
            }
        }
    }
    for (const auto& terrain : scene.terrains) {
        if (!IsDirectionalShadowBakeTerrain(terrain) || !BakeLayerVisible(terrain.layer, mask)) continue;
        for (const auto& patch : terrain.patches) {
            /// @note ビューの距離や投影サイズに依存しない正本の穴と高さを使う。
            if (patch.indexCountLOD[0] && !append(terrain.constants.worldMatrix, patch.vertexBuffer,
                patch.indexBufferLOD[0], terrainPipeline, patch.indexCountLOD[0])) return false;
        }
    }
    return !draws.empty();
}

bool EncodeBakeDepth(const std::vector<float>& rgba, size_t pixelCount, std::vector<uint8_t>& packed)
{
    if (rgba.size() != pixelCount * 4) return false;
    packed.resize(rgba.size());
    bool covered = false;
    for (size_t index = 0; index < rgba.size(); index += 4) {
        const float depth = rgba[index];
        /// @note Alpha は転写済みであることの証明。深度を持たない clear だけでは成功しない。
        if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f || rgba[index + 3] != 1.0f) return false;
        covered |= depth < 1.0f;
        const auto bits = std::bit_cast<uint32_t>(depth);
        for (unsigned channel = 0; channel < 4; ++channel)
            packed[index + channel] = static_cast<uint8_t>((bits >> (8u * channel)) & 0xFFu);
    }
    return covered;
}

} /// @note namespace

bool IsDirectionalShadowBakeMesh(const RenderObject& object, const RenderMeshItem& item)
{
    return object.shadowBakeStatic && object.castShadows && object.colorEligible && object.lodVisible
        && !object.skinned && !object.rayLodSelectionRequired && object.lodDither == 0.0f
        && object.leafWindBoundsPadding == 0.0f && item.visible && item.indexCount > 0
        && item.vertexBuffer.IsValid() && item.indexBuffer.IsValid() && !item.deformedVertexBuffer.IsValid()
        && item.vertexPositionOffset == 0 && !item.preparedSkinning && item.material.valid
        && item.material.shaderCapabilities.vertex == ShaderVertexContract::STANDARD_SURFACE
        && item.material.capabilities.blend == BlendMode::OPAQUE_BLEND && item.material.surface.alphaCutoff == 0.0f;
}

bool IsDirectionalShadowBakeTerrain(const RenderTerrainInput& terrain)
{
    return terrain.shadowBakeStatic;
}

bool DirectionalShadowBakeIncludesMeshes(const ShadowBounds& coverage, uint32_t resolution)
{
    constexpr float kMaxMeshTexelWorldSize = 1.0f;
    return ValidBakeBounds(coverage) && ValidBakeResolution(resolution)
        && coverage.radius <= 0.5f * kMaxMeshTexelWorldSize * static_cast<float>(resolution);
}

bool BakeDirectionalStaticShadow(IRenderer& renderer, ResourceManager& resources, const RenderScene& scene,
    const ShadowBounds& coverage, const math::Vector3& direction, uint32_t resolution, uint32_t cullingMask,
    DirectionalShadowBakeData& out)
{
    FBZZ_PROFILE_SCOPE("DirectionalShadowBake::Generate");
    math::Vector3 unit;
    if (!ValidBakeBounds(coverage) || !ValidBakeResolution(resolution) || cullingMask == 0
        || !UnitBakeDirection(direction, unit)) return false;
    const auto cascade = MakeFixedBakeCascade(coverage, unit, resolution, 1.0f);
    if (!ValidBakeProjection(cascade, coverage.center)) return false;
    std::vector<StaticDepthDraw> draws;
    if (!CollectBakeDraws(resources, scene, cullingMask, DirectionalShadowBakeIncludesMeshes(coverage, resolution), draws)) return false;

    const auto shadowShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/ShadowMap.hlsl");
    const auto extractShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/BakedDepthExtract.hlsl");
    if (!shadowShader.IsValid() || !extractShader.IsValid()) return false;
    const auto depth = resources.CreateRenderTarget(resolution, resolution, {0, Format::RGBA16F, true, false});
    const auto extract = resources.CreateRenderTarget(resolution, resolution, {1, Format::RGBA32F, false, false});
    const auto frameCB = resources.CreateConstantBuffer(sizeof(PerFrameCB));
    const auto objectCB = resources.CreateConstantBuffer(sizeof(PerObjectCB));
    const auto extractPipeline = resources.CreatePipelineState({RasterizerMode::SOLID_NOCULL,
        BlendMode::OPAQUE_BLEND, DepthMode::DEPTH_OFF});
    const auto depthTexture = depth.IsValid() ? resources.GetDepthTexture(depth) : ResourceHandle<TextureTag>{};

    std::vector<float> rgba;
    uint32_t width = 0, height = 0;
    bool captured = false;
    if (depthTexture.IsValid() && extract.IsValid() && frameCB.IsValid() && objectCB.IsValid() && extractPipeline.IsValid()) {
        PerFrameCB frame{};
        frame.viewProjection = cascade.viewProjection;
        frame.view = cascade.view;
        frame.cameraPos = cascade.eyePos;
        frame.isOrthographic = 1.0f;
        renderer.BeginFrame();
        renderer.SetRenderTarget(depth, resources);
        renderer.ClearDepth();
        resources.Update(frameCB, &frame, sizeof(frame));

        DrawCall draw;
        draw.shader = shadowShader;
        draw.constantBuffers[0] = frameCB;
        draw.constantBuffers[1] = objectCB;
        for (const auto& input : draws) {
            PerObjectCB object{};
            object.world = input.world;
            resources.Update(objectCB, &object, sizeof(object));
            draw.vertexBuffer = input.vertices;
            draw.indexBuffer = input.indices;
            draw.indexCount = input.indexCount;
            draw.pipelineState = input.pipeline;
            renderer.Submit(draw, resources);
        }
        /// @see D3D12 CopyTextureRegion の深度コピー制約 https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copytextureregion
        renderer.SetRenderTarget(extract, resources);
        renderer.Clear({0, 0, 0, 0});
        DrawCall copy;
        copy.shader = extractShader;
        copy.pipelineState = extractPipeline;
        copy.vertexCount = 3;
        copy.textures[0] = depthTexture;
        renderer.Submit(copy, resources);
        renderer.SetRenderTarget({}, resources);
        renderer.EndFrame();
        captured = renderer.CaptureRenderTargetToLinearRGBA(extract, resources, rgba, width, height);
    }
    if (extractPipeline.IsValid()) resources.Release(extractPipeline);
    if (objectCB.IsValid()) resources.Release(objectCB);
    if (frameCB.IsValid()) resources.Release(frameCB);
    if (extract.IsValid()) resources.Release(extract);
    if (depth.IsValid()) resources.Release(depth);

    DirectionalShadowBakeData result{unit, coverage, resolution, cullingMask, {}};
    if (!captured || width != resolution || height != resolution
        || !EncodeBakeDepth(rgba, static_cast<size_t>(resolution) * resolution, result.depthRgba)) return false;
    out = std::move(result);
    return true;
}

ResourceHandle<TextureTag> UploadDirectionalShadowBake(ResourceManager& resources,
    const DirectionalShadowBakeData& data)
{
    if (!ValidBakeResolution(data.resolution) || data.depthRgba.size() != static_cast<size_t>(data.resolution) * data.resolution * 4)
        return {};
    bool covered = false;
    for (size_t index = 0; index < data.depthRgba.size(); index += 4) {
        uint32_t bits = 0;
        for (unsigned channel = 0; channel < 4; ++channel)
            bits |= static_cast<uint32_t>(data.depthRgba[index + channel]) << (8u * channel);
        const float depth = std::bit_cast<float>(bits);
        if (!std::isfinite(depth) || depth < 0.0f || depth > 1.0f) return {};
        covered |= depth < 1.0f;
    }
    if (!covered) return {};
    return resources.CreateTexture(data.depthRgba.data(), data.resolution, data.resolution);
}

bool PrepareDirectionalShadowBakeView(ResourceManager& resources, const DirectionalShadowBakeData& data,
    ResourceHandle<TextureTag> texture, const ShadowBounds& currentBounds, const math::Vector3& currentDirection,
    uint32_t cullingMask, float shadowBias, const PreparedShadows& shadows, DirectionalShadowBakeView& out)
{
    if (shadows.count < 2 || shadows.count > kMaxShadowCascades || cullingMask != data.cullingMask
        || !ValidBakeBounds(data.coverage) || !ValidBakeBounds(currentBounds)
        || !ValidBakeResolution(data.resolution) || !std::isfinite(shadowBias)) return false;
    const float centerDistance = (currentBounds.center - data.coverage.center).Length();
    if (!std::isfinite(centerDistance) || centerDistance + currentBounds.radius > data.coverage.radius) return false;
    math::Vector3 live;
    math::Vector3 baked;
    if (!UnitBakeDirection(currentDirection, live) || !UnitBakeDirection(data.lightDirection, baked)
        || (live - baked).LengthSq() > 1.0e-12f) return false;

    const auto& far = shadows.cascades[shadows.count - 1];
    const auto* depth = resources.Get(texture);
    if (!depth || far.viewportSize == 0 || !std::isfinite(far.texelWorldSize) || far.texelWorldSize <= 0.0f
        || depth->GetWidth() != data.resolution
        || depth->GetHeight() != data.resolution) return false;
    const auto restoreShader = resources.LoadShader("Assets/Shaders/Pipeline/Shadow/BakedDepthRestore.hlsl");
    const auto restorePipeline = GetOrCreateMaterialPSO(resources, BlendMode::OPAQUE_BLEND, true, 0, 0.0f, false);
    if (!resources.Get(restoreShader) || !resources.Get(restorePipeline)) return false;

    /// @note 保存した正規化ベクトルをそのまま再使用してベイク時と同じ投影にする。
    if ((data.lightDirection - baked).LengthSq() > 1.0e-12f) return false;
    const auto fixed = MakeFixedBakeCascade(data.coverage, data.lightDirection, data.resolution, shadowBias);
    math::Matrix4 cascadeToBakeClip;
    if (!ValidBakeProjection(fixed, data.coverage.center)
        || !PrepareCascadeToBakeClip(fixed.viewProjection, far.viewProjection, cascadeToBakeClip)) return false;

    const bool coarserSource = fixed.texelWorldSize > far.texelWorldSize;
    const auto& depthRow = fixed.viewProjection.m[2];
    const float depthPerMeter = math::Vector3{depthRow[0], depthRow[1], depthRow[2]}.Length();
    /// @note 粗い連続面だけを補間し、PCF の投影と受け側バイアスは元の CSM に保つ。
    const float maxSmoothDepthStep = coarserSource ? 8.0f * fixed.texelWorldSize * depthPerMeter : 0.0f;
    const float sourceDepthBias = coarserSource ? 0.02f * (fixed.texelWorldSize - far.texelWorldSize) * depthPerMeter : 0.0f;
    out = {texture, restoreShader, restorePipeline, cascadeToBakeClip, maxSmoothDepthStep, sourceDepthBias,
        DirectionalShadowBakeIncludesMeshes(data.coverage, data.resolution)};
    return true;
}

} /// @note namespace fbzz::renderer
