// FBZZ Engine
// RenderPasses/Geometry/TrailRenderPass.cpp | fbzz::scene
// TrailComponent のリングバッファ更新、Catmull-Rom 補間、リボン頂点生成、DrawCall 発行 (IRenderPass 実装)
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Renderer/SamplerMode.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Transform.hpp>
#include <Engine/Util/Easing.hpp>
#include <Math/MathUtils.hpp>
#include "GeometryPasses.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fbzz::scene {

namespace {

// TrailVertex — Assets/Shaders/Material/Effects/Trail.hlsl の VS 入力と一致する CPU 頂点。
struct TrailVertex {
    math::Vector3 position;
    float         age;
    float         v;
    float         u;
};

// TrailCB — TrailConstants(cbuffer b2) の C++ ミラー。
struct TrailCB {
    math::Vector4 colorStart;
    math::Vector4 colorEnd;
    float uvScrollSpeed = 0.0f;
    float uvTiling = 1.0f;
    float time = 0.0f;
    float _pad = 0.0f;
};

// TrailDrawItem — Trail の透明描画をカメラから遠い順へ並べるための一時データ。
struct TrailDrawItem {
    renderer::DrawCall drawCall;
    float distanceSq = 0.0f;
};

static_assert(sizeof(TrailVertex) == 24, "TrailVertex layout mismatch");
static_assert(sizeof(TrailCB) == 48, "TrailCB layout mismatch");

void InitTrailStorage(TrailComponent& trail)
{
    trail.maxPoints = (std::max)(trail.maxPoints, 2);
    trail.pointBuffer.resize(static_cast<size_t>(trail.maxPoints));
    trail.ringHead = 0;
    trail.ringTail = 0;
    trail.ringCount = 0;
    trail.lastSampleTime = -1.0f;
}

TrailPoint& RingAt(TrailComponent& trail, int index)
{
    assert(index >= 0 && index < trail.ringCount);
    return trail.pointBuffer[static_cast<size_t>((trail.ringHead + index) % trail.maxPoints)];
}

const TrailPoint& RingAt(const TrailComponent& trail, int index)
{
    assert(index >= 0 && index < trail.ringCount);
    return trail.pointBuffer[static_cast<size_t>((trail.ringHead + index) % trail.maxPoints)];
}

void RingPushBack(TrailComponent& trail, const TrailPoint& point)
{
    if (trail.pointBuffer.size() != static_cast<size_t>(trail.maxPoints))
        InitTrailStorage(trail);

    if (trail.ringCount == trail.maxPoints) {
        trail.ringHead = (trail.ringHead + 1) % trail.maxPoints;
    } else {
        ++trail.ringCount;
    }

    trail.pointBuffer[static_cast<size_t>(trail.ringTail)] = point;
    trail.ringTail = (trail.ringTail + 1) % trail.maxPoints;
}

void RingExpireOld(TrailComponent& trail, float currentTime)
{
    const float oldestAllowedTime = currentTime - (std::max)(trail.duration, math::EPSILON);
    while (trail.ringCount > 0 && RingAt(trail, 0).timestamp < oldestAllowedTime) {
        trail.ringHead = (trail.ringHead + 1) % trail.maxPoints;
        --trail.ringCount;
    }
}

float DistanceSq(const math::Vector3& a, const math::Vector3& b)
{
    return (a - b).LengthSq();
}

float NearestTrailDistanceSq(const TrailComponent& trail, const math::Vector3& cameraPos)
{
    float nearest = 0.0f;
    for (int i = 0; i < trail.ringCount; ++i) {
        const float d = DistanceSq(RingAt(trail, i).position, cameraPos);
        nearest = (i == 0) ? d : (std::min)(nearest, d);
    }
    return nearest;
}

math::Vector3 SafeNormalize(const math::Vector3& v, const math::Vector3& fallback)
{
    return v.LengthSq() > math::EPSILON * math::EPSILON ? v.Normalized() : fallback;
}

math::Vector3 ComputeRibbonNormal(
    const math::Vector3& segmentDir,
    TrailAlignment alignment,
    const math::Vector3& cameraPos,
    const math::Vector3& pointPos)
{
    math::Vector3 up = math::Vector3::UP;
    if (alignment == TrailAlignment::CameraFacing) {
        up = SafeNormalize(cameraPos - pointPos, math::Vector3::UP);
        if (std::abs(math::Vector3::Dot(segmentDir, up)) > 0.99f)
            up = math::Vector3::UP;
    } else if (std::abs(math::Vector3::Dot(segmentDir, up)) > 0.99f) {
        up = math::Vector3::RIGHT;
    }

    return SafeNormalize(math::Vector3::Cross(segmentDir, up), math::Vector3::RIGHT);
}

float ApplyWidthEasing(TrailWidthEasing easing, float t)
{
    t = math::Clamp01(t);
    switch (easing) {
    case TrailWidthEasing::EaseIn:
        return util::Easing::EaseInQuad(t);
    case TrailWidthEasing::EaseOut:
        return util::Easing::EaseOutQuad(t);
    case TrailWidthEasing::EaseInOut:
        return util::Easing::EaseInOutQuad(t);
    case TrailWidthEasing::Linear:
    default:
        return t;
    }
}

TrailPoint CatmullRom(const TrailPoint& p0, const TrailPoint& p1, const TrailPoint& p2, const TrailPoint& p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;

    TrailPoint out;
    out.position =
        (p1.position * 2.0f +
         (p2.position - p0.position) * t +
         (p0.position * 2.0f - p1.position * 5.0f + p2.position * 4.0f - p3.position) * t2 +
         (-p0.position + p1.position * 3.0f - p2.position * 3.0f + p3.position) * t3) * 0.5f;
    out.timestamp = math::Lerp(p1.timestamp, p2.timestamp, t);
    return out;
}

std::vector<TrailPoint> BuildRenderPoints(const TrailComponent& trail)
{
    std::vector<TrailPoint> points;
    points.reserve(static_cast<size_t>(trail.ringCount));
    for (int i = 0; i < trail.ringCount; ++i)
        points.push_back(RingAt(trail, i));

    const int subdivisions = (std::max)(trail.smoothSubdivisions, 0);
    if (subdivisions == 0 || points.size() < 2)
        return points;

    std::vector<TrailPoint> smooth;
    smooth.reserve((points.size() - 1u) * static_cast<size_t>(subdivisions + 1) + 1u);

    for (size_t i = 0; i + 1u < points.size(); ++i) {
        const TrailPoint& p1 = points[i];
        const TrailPoint& p2 = points[i + 1u];

        TrailPoint p0 = (i > 0) ? points[i - 1u] : TrailPoint{ p1.position * 2.0f - p2.position, p1.timestamp - (p2.timestamp - p1.timestamp) };
        TrailPoint p3 = (i + 2u < points.size()) ? points[i + 2u] : TrailPoint{ p2.position * 2.0f - p1.position, p2.timestamp + (p2.timestamp - p1.timestamp) };

        smooth.push_back(p1);
        for (int s = 1; s <= subdivisions; ++s) {
            const float t = static_cast<float>(s) / static_cast<float>(subdivisions + 1);
            smooth.push_back(CatmullRom(p0, p1, p2, p3, t));
        }
    }
    smooth.push_back(points.back());
    return smooth;
}

void BuildTrailVertices(
    const TrailComponent& trail,
    const std::vector<TrailPoint>& points,
    const math::Vector3& cameraPos,
    float currentTime,
    std::vector<TrailVertex>& outVertices)
{
    outVertices.clear();
    if (points.size() < 2)
        return;

    std::vector<math::Vector3> normals(points.size(), math::Vector3::RIGHT);
    for (size_t i = 0; i < points.size(); ++i) {
        math::Vector3 nLeft = math::Vector3::RIGHT;
        math::Vector3 nRight = math::Vector3::RIGHT;
        bool hasLeft = false;
        bool hasRight = false;

        if (i > 0) {
            const math::Vector3 dir = SafeNormalize(points[i].position - points[i - 1u].position, math::Vector3::FORWARD);
            nLeft = ComputeRibbonNormal(dir, trail.alignment, cameraPos, points[i].position);
            hasLeft = true;
        }
        if (i + 1u < points.size()) {
            const math::Vector3 dir = SafeNormalize(points[i + 1u].position - points[i].position, math::Vector3::FORWARD);
            nRight = ComputeRibbonNormal(dir, trail.alignment, cameraPos, points[i].position);
            hasRight = true;
        }

        if (hasLeft && hasRight) {
            math::Vector3 miter = SafeNormalize(nLeft + nRight, nRight);
            const float cosHalfAngle = (std::max)(math::Vector3::Dot(miter, nLeft), 0.5f);
            normals[i] = miter * (1.0f / cosHalfAngle);
        } else {
            normals[i] = hasRight ? nRight : nLeft;
        }
    }

    const float duration = (std::max)(trail.duration, math::EPSILON);
    const float birthTime = currentTime - duration;
    const float segmentDenom = static_cast<float>(points.size() - 1u);
    std::vector<float> cumulativeLengths(points.size(), 0.0f);
    if (trail.uvMode == TrailUVMode::Tile) {
        for (size_t i = 1; i < points.size(); ++i)
            cumulativeLengths[i] = cumulativeLengths[i - 1u] + (points[i].position - points[i - 1u].position).Length();
    }
    outVertices.reserve((points.size() - 1u) * 6u);

    auto ageOf = [&](const TrailPoint& p) {
        return math::Clamp01((p.timestamp - birthTime) / duration);
    };

    for (size_t i = 0; i + 1u < points.size(); ++i) {
        const TrailPoint& p0 = points[i];
        const TrailPoint& p1 = points[i + 1u];
        const float age0 = ageOf(p0);
        const float age1 = ageOf(p1);
        const float widthAge0 = ApplyWidthEasing(trail.widthEasing, age0);
        const float widthAge1 = ApplyWidthEasing(trail.widthEasing, age1);
        const float halfWidth0 = math::Lerp(trail.widthEnd, trail.widthStart, widthAge0) * 0.5f;
        const float halfWidth1 = math::Lerp(trail.widthEnd, trail.widthStart, widthAge1) * 0.5f;
        const float u0 = trail.uvMode == TrailUVMode::Tile ? cumulativeLengths[i] : static_cast<float>(i) / segmentDenom;
        const float u1 = trail.uvMode == TrailUVMode::Tile ? cumulativeLengths[i + 1u] : static_cast<float>(i + 1u) / segmentDenom;

        const TrailVertex tl{ p0.position + normals[i] * halfWidth0, age0, 0.0f, u0 };
        const TrailVertex bl{ p0.position - normals[i] * halfWidth0, age0, 1.0f, u0 };
        const TrailVertex tr{ p1.position + normals[i + 1u] * halfWidth1, age1, 0.0f, u1 };
        const TrailVertex br{ p1.position - normals[i + 1u] * halfWidth1, age1, 1.0f, u1 };

        outVertices.insert(outVertices.end(), { tl, tr, bl, bl, tr, br });
    }
}

void EnsureResources(TrailComponent& trail, RenderPassContext& ctx)
{
    auto& resources = ctx.resources;
    trail.maxPoints = (std::max)(trail.maxPoints, 2);
    trail.duration = (std::max)(trail.duration, 0.01f);
    trail.sampleInterval = (std::max)(trail.sampleInterval, 0.0f);
    trail.minVertexDist = (std::max)(trail.minVertexDist, 0.0f);
    trail.smoothSubdivisions = (std::max)(trail.smoothSubdivisions, 0);
    trail.uvTiling = (std::max)(trail.uvTiling, 0.001f);

    if (trail.pointBuffer.size() != static_cast<size_t>(trail.maxPoints))
        InitTrailStorage(trail);

    if (!trail.vertexBuffer.IsValid() ||
        trail.allocatedMaxPoints != trail.maxPoints ||
        trail.allocatedSmoothSubdivisions != trail.smoothSubdivisions)
    {
        if (trail.vertexBuffer.IsValid())
            resources.Release(trail.vertexBuffer);
        const uint32_t maxSegments =
            static_cast<uint32_t>(trail.maxPoints - 1) * static_cast<uint32_t>(trail.smoothSubdivisions + 1);
        const uint32_t maxVertices = maxSegments * 6u;
        trail.vertexBuffer = resources.CreateVertexBuffer(
            nullptr,
            maxVertices * sizeof(TrailVertex),
            static_cast<uint32_t>(sizeof(TrailVertex)));
        trail.allocatedMaxPoints = trail.maxPoints;
        trail.allocatedSmoothSubdivisions = trail.smoothSubdivisions;
    }

    if (!trail.trailCB.IsValid())
        trail.trailCB = resources.CreateConstantBuffer(sizeof(TrailCB));

    // materialPath が設定されている場合: .mat の albedo テクスチャを優先する。
    if (!trail.materialPath.empty()) {
        const bool matChanged = (trail.loadedMaterialPath != trail.materialPath);
        if (matChanged) {
            trail.loadedMaterialPath = trail.materialPath;
            trail.loadedTexturePath.clear();
        }
        const auto matHandle = asset::AssetManager::LoadMaterial(trail.materialPath);
        if (const auto* mat = asset::AssetManager::GetMaterial(matHandle)) {
            const auto it = mat->textures.find("albedo");
            const std::string& resolvedTex = (it != mat->textures.end()) ? it->second : std::string{};
            if (!trail.texture.IsValid() || trail.loadedTexturePath != resolvedTex) {
                if (resolvedTex.empty()) {
                    static const uint8_t white[4] = { 255, 255, 255, 255 };
                    trail.texture = resources.CreateTexture(white, 1, 1);
                } else {
                    trail.texture = resources.LoadTexture(resolvedTex);
                }
                trail.loadedTexturePath = resolvedTex;
            }
        }
    } else if (!trail.texture.IsValid() || trail.loadedTexturePath != trail.texturePath) {
        // フォールバック: texturePath を直接使用する (materialPath 未設定時の既存挙動を維持)。
        if (trail.texturePath.empty()) {
            static const uint8_t white[4] = { 255, 255, 255, 255 };
            trail.texture = resources.CreateTexture(white, 1, 1);
        } else {
            trail.texture = resources.LoadTexture(trail.texturePath);
        }
        trail.loadedTexturePath = trail.texturePath;
    }
}

math::Vector3 ResolveTrailSamplePosition(Scene& scene, GameObject& go, const TrailComponent& trail)
{
    if (!trail.attachBone.empty()) {
        if (auto* smr = go.GetComponent<SkinnedMeshRenderer>()) {
            if (smr->model && smr->model->skeleton) {
                const auto it = smr->model->skeleton->nodeMap.find(trail.attachBone);
                if (it != smr->model->skeleton->nodeMap.end()) {
                    const int nodeIndex = it->second;
                    if (nodeIndex >= 0 && nodeIndex < static_cast<int>(smr->nodeEntities.size())) {
                        if (auto* boneGo = scene.GetGameObject(smr->nodeEntities[static_cast<size_t>(nodeIndex)]))
                            return boneGo->transform.position + boneGo->transform.rotation * trail.attachOffset;
                    }
                }
            }
        }
    }

    return go.transform.position + go.transform.rotation * trail.attachOffset;
}

void UpdateTrailPoints(TrailComponent& trail, const math::Vector3& currentPos, float currentTime)
{
    RingExpireOld(trail, currentTime);

    const bool firstSample = trail.lastSampleTime < 0.0f;
    const bool timeReady = firstSample || currentTime - trail.lastSampleTime >= trail.sampleInterval;
    if (!timeReady)
        return;

    const bool distanceReady =
        trail.ringCount == 0 ||
        DistanceSq(currentPos, RingAt(trail, trail.ringCount - 1).position) >= trail.minVertexDist * trail.minVertexDist;

    if (distanceReady) {
        RingPushBack(trail, { currentPos, currentTime });
        trail.lastSampleTime = currentTime;
    }
}

} // namespace

// ─── IRenderPass ──────────────────────────────────────────────────────────────

std::string_view TrailRenderPass::Name() const { return "Trail"; }

std::vector<renderer::RenderGraph::ResourceAccess> TrailRenderPass::DeclareAccesses(
    const RenderPassContext&) const
{
    return { { "HDR", renderer::RenderGraph::ResourceUsage::ReadWrite } };
}

void TrailRenderPass::Execute(RenderPassContext& ctx)
{
    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    if (!h.trailShader.IsValid() || !h.trailPSO.IsValid())
        return;

    renderer.SetRenderTarget(h.hdrRT, resources);
    renderer.SetSampler(0, renderer::SamplerMode::WRAP_BILINEAR);

    const float currentTime = Time::time;
    std::vector<TrailVertex> vertices;
    std::vector<TrailDrawItem> drawItems;

    for (auto& go : ctx.scene.GameObjects()) {
        if (!ShouldRenderGameObject(go, ctx.cullingMask))
            continue;

        auto* trail = go.GetComponent<TrailComponent>();
        if (!trail)
            continue;

        if (!trail->enabled && trail->clearOnDisable) {
            trail->ringCount = 0;
            trail->ringHead = 0;
            trail->ringTail = 0;
            trail->lastSampleTime = -1.0f;
            continue;
        }

        EnsureResources(*trail, ctx);

        if (trail->enabled) {
            const math::Vector3 samplePos = ResolveTrailSamplePosition(ctx.scene, go, *trail);
            UpdateTrailPoints(*trail, samplePos, currentTime);
        }
        else
            RingExpireOld(*trail, currentTime);
        if (trail->ringCount < 2)
            continue;

        const std::vector<TrailPoint> renderPoints = BuildRenderPoints(*trail);
        BuildTrailVertices(*trail, renderPoints, ctx.camera.m_position, currentTime, vertices);
        if (vertices.empty())
            continue;

        const size_t maxBytes =
            static_cast<size_t>(trail->maxPoints - 1) *
            static_cast<size_t>(trail->smoothSubdivisions + 1) *
            6u * sizeof(TrailVertex);
        const size_t uploadBytes = (std::min)(vertices.size() * sizeof(TrailVertex), maxBytes);
        resources.Update(trail->vertexBuffer, vertices.data(), uploadBytes);

        TrailCB cb{};
        cb.colorStart = trail->colorStart;
        cb.colorEnd = trail->colorEnd;
        cb.uvScrollSpeed = trail->uvScrollSpeed;
        cb.uvTiling = trail->uvTiling;
        cb.time = currentTime;
        resources.Update(trail->trailCB, &cb, sizeof(cb));

        renderer::DrawCall dc;
        dc.vertexBuffer = trail->vertexBuffer;
        dc.shader = h.trailShader;
        dc.pipelineState = h.trailPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[2] = trail->trailCB;
        dc.textures[0] = trail->texture;
        dc.vertexCount = static_cast<uint32_t>(uploadBytes / sizeof(TrailVertex));
        dc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        dc.topology = renderer::PrimitiveTopology::TRIANGLE_LIST;
        drawItems.push_back({ dc, NearestTrailDistanceSq(*trail, ctx.camera.m_position) });
    }

    std::sort(drawItems.begin(), drawItems.end(), [](const TrailDrawItem& a, const TrailDrawItem& b) {
        return a.distanceSq > b.distanceSq;
    });

    for (const auto& item : drawItems) {
        renderer.Submit(item.drawCall, resources);

        ++ctx.statsDrawCalls;
        ctx.statsVertexCount += static_cast<int>(item.drawCall.vertexCount);
        ctx.statsTriangleCount += static_cast<int>(item.drawCall.vertexCount / 3u);
    }
}

} // namespace fbzz::scene
