/// @file    RenderPasses/Geometry/TrailRenderPass.cpp
/// @brief   TrailComponent のリングバッファ更新、Catmull-Rom 補間、リボン頂点生成、DrawCall 発行 (IRenderPass 実装)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "Engine/Scene/Systems/RenderPasses/Geometry/TrailRenderPass.hpp"

#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MaterialAsset.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DrawCall.hpp>
#include <Engine/Renderer/DynamicBufferPool.hpp>
#include <Engine/Renderer/RenderState.hpp>
#include <Engine/Scene/Components/ParticleColorSpace.hpp>
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

// TrailVertex / TrailCB の定義は GeometryPasses.hpp。
// WHY: per-particle Trail のリボン (ParticlePass.cpp) が同じ頂点・同じ CB・同じシェーダーを使う。
//      ここに閉じたままだと、片方だけ直して「Trail ノードは正しいが粒子の帯は崩れる」形で壊れる。

// TrailDrawItem — Trail の透明描画をカメラから遠い順へ並べるための一時データ。
struct TrailDrawItem {
    renderer::DrawCall drawCall;
    float distanceSq = 0.0f;
};

// 帯の頂点バッファ。Component に 1 本持たせず、描くたびにプールから借りる。
// WHY: 帯はカメラへ正対するのでビューごとに形が変わる。1 本だと Scene View と Game View が
//      同じ実体を 2 回書き、DX12 では先に記録した Scene View の Draw まで Game View の形を読む。
//      前フレームの GPU がまだ読んでいる実体を書き直す問題もある (詳細は DynamicBufferPool.hpp)。
renderer::DynamicVertexBufferPool g_trailVertexPool;

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

    // 各点の幅方向。マイター接合そのものは粒子リボンと共通で、Trail だけが
    // alignment (View / Local / Velocity) で幅方向の決め方を変える。
    //
    // NOTE: 長さ 0 の線分の扱いが変わった。以前は FORWARD へ倒して «向きがある» ものと
    //       して扱っていたが、共通版では寄与しない。重なった点は minVertexDist で
    //       間引かれるので実データでは出ないが、出れば «前後の向きだけで決まる» 側になる。
    std::vector<math::Vector3> positions;
    positions.reserve(points.size());
    for (const TrailPoint& point : points) positions.push_back(point.position);

    std::vector<math::Vector3> normals;
    BuildRibbonMiterNormals(positions,
        [&](const math::Vector3& direction, const math::Vector3& point) {
            return ComputeRibbonNormal(direction, trail.alignment, cameraPos, point);
        },
        normals);

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
        const float halfWidth0 = TrailWidthAt(trail, age0, widthAge0) * 0.5f;
        const float halfWidth1 = TrailWidthAt(trail, age1, widthAge1) * 0.5f;
        const float u0 = trail.uvMode == TrailUVMode::Tile ? cumulativeLengths[i] : static_cast<float>(i) / segmentDenom;
        const float u1 = trail.uvMode == TrailUVMode::Tile ? cumulativeLengths[i + 1u] : static_cast<float>(i + 1u) / segmentDenom;

        const TrailVertex tl{ p0.position + normals[i] * halfWidth0, age0, 0.0f, u0 };
        const TrailVertex bl{ p0.position - normals[i] * halfWidth0, age0, 1.0f, u0 };
        const TrailVertex tr{ p1.position + normals[i + 1u] * halfWidth1, age1, 0.0f, u1 };
        const TrailVertex br{ p1.position - normals[i + 1u] * halfWidth1, age1, 1.0f, u1 };

        outVertices.insert(outVertices.end(), { tl, tr, bl, bl, tr, br });
    }
}

// 多キー色を CB へ詰める。キーが足りなければ何も書かず、シェーダーは
// colorStart / colorEnd の 2 点へ落ちる (既存シーンの見た目を変えないため)。
void FillTrailGradient(const TrailComponent& trail, TrailCB& cb)
{
    if (!TrailUsesColorGradient(trail)) return;

    const uint32_t count = (std::min)(trail.colorGradient.keyCount, kMaxParticleCurveKeys);
    for (uint32_t i = 0; i < count; ++i) {
        const ParticleGradientKey& key = trail.colorGradient.keys[i];
        cb.gradientColors[i] = ParticleSrgbToLinear(key.color);
        // 時刻は float4 × 2 に詰めてある (HLSL の float 配列は 1 要素 16 バイトを食う)。
        math::Vector4& slot = cb.gradientTimes[i / 4];
        switch (i % 4) {
        case 0:  slot.x = key.time; break;
        case 1:  slot.y = key.time; break;
        case 2:  slot.z = key.time; break;
        default: slot.w = key.time; break;
        }
    }
    cb.gradientKeyCount = count;
    cb.gradientInterpolation = static_cast<uint32_t>(trail.colorGradient.interpolation);
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
                    trail.texture = resources.GetWhiteTexture();
                } else {
                    trail.texture = resources.LoadTexture(resolvedTex);
                }
                trail.loadedTexturePath = resolvedTex;
            }
        }
    } else if (!trail.texture.IsValid()) {
        // 共有の 1 枚を借りる。実体ごとに作ると、その実体が畳まれたぶんだけ GPU に残る。
        trail.texture = resources.GetWhiteTexture();
        trail.loadedTexturePath.clear();
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
        return boneGo->transform.worldPosition +
            boneGo->transform.worldRotation * trail.attachOffset;
                    }
                }
            }
        }
    }

        return go.transform.worldPosition +
            go.transform.worldRotation * trail.attachOffset;
}

void ClearRing(TrailComponent& trail)
{
    trail.ringHead = 0;
    trail.ringTail = 0;
    trail.ringCount = 0;
    trail.lastSampleTime = -1.0f;
}

void UpdateTrailPoints(TrailComponent& trail, const math::Vector3& currentPos, float currentTime)
{
    RingExpireOld(trail, currentTime);

    // 瞬間移動の切断。sampleInterval の «待ち» より前に判定する ─ 跳んだフレームを
    // 待たせると、その 1 フレームのあいだ古い点と新しい点が 1 本の筋でつながる。
    if (trail.ringCount > 0
        && TrailIsDiscontinuous(trail, RingAt(trail, trail.ringCount - 1).position, currentPos)) {
        ClearRing(trail);
    }

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

void TrailRenderPass::Setup(PassBuilder& builder, const RenderPassContext&) const
{
    builder.ReadWrite("HDR").SetAutoTarget("HDR");
}

void TrailRenderPass::Execute(PassResources&, RenderPassContext& ctx)
{
    auto& renderer = ctx.renderer;
    auto& resources = ctx.resources;
    auto& h = ctx.handles;

    if (!h.trailShader.IsValid() || !h.trailPSO.IsValid())
        return;

    renderer.SetRenderTarget(ctx.Res().Target("HDR"), resources);

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
            ClearRing(*trail);
            continue;
        }

        EnsureResources(*trail, ctx);

        if (trail->enabled && trail->beamMode) {
            const auto transformPoint = [&go, &trail](const math::Vector3& point) {
                if (trail->beamWorldSpace) return point;
                const math::Vector3 scaled{
        point.x * go.transform.worldScale.x,
        point.y * go.transform.worldScale.y,
        point.z * go.transform.worldScale.z };
    return go.transform.worldPosition +
        go.transform.worldRotation * scaled;
            };
            trail->ringHead = 0;
            trail->ringTail = 0;
            trail->ringCount = 0;
            // 折れ線が与えられていればそれを、無ければ従来どおり 2 端点を描く。
            if (trail->beamPoints.size() >= 2) {
                for (const math::Vector3& point : trail->beamPoints)
                    RingPushBack(*trail, { transformPoint(point), currentTime });
            } else {
                RingPushBack(*trail, { transformPoint(trail->beamStart), currentTime });
                RingPushBack(*trail, { transformPoint(trail->beamEnd), currentTime });
            }
            trail->lastSampleTime = currentTime;
        } else if (trail->enabled) {
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

        const size_t maxVertices =
            static_cast<size_t>(trail->maxPoints - 1) *
            static_cast<size_t>(trail->smoothSubdivisions + 1) * 6u;
        const size_t uploadVertices = (std::min)(vertices.size(), maxVertices);
        const auto vertexBuffer = g_trailVertexPool.Acquire(
            resources, uploadVertices, static_cast<uint32_t>(sizeof(TrailVertex)));
        if (!vertexBuffer.IsValid())
            continue;
        resources.Update(vertexBuffer, vertices.data(), uploadVertices * sizeof(TrailVertex));

        TrailCB cb{};
        // オーサリング値 (sRGB) → リニア。素材のリニア化はシェーダー側が行う。
        cb.colorStart = ParticleSrgbToLinear(trail->colorStart);
        cb.colorEnd = ParticleSrgbToLinear(trail->colorEnd);
        FillTrailGradient(*trail, cb);
        cb.uvScrollSpeed = trail->uvScrollSpeed;
        cb.uvTiling = trail->uvTiling;
        cb.time = currentTime;
        cb.flags = IsEffectTextureSrgb(trail->loadedTexturePath) ? kTrailFlagSrgbTexture : 0u;
        resources.Update(trail->trailCB, &cb, sizeof(cb));

        renderer::DrawCall dc;
        dc.vertexBuffer = vertexBuffer;
        dc.shader = h.trailShader;
        dc.pipelineState = h.trailPSO;
        dc.constantBuffers[0] = h.frameCB;
        dc.constantBuffers[2] = trail->trailCB;
        dc.textures[0] = trail->texture;
        dc.vertexCount = static_cast<uint32_t>(uploadVertices);
        dc.layer = renderer::RenderLayer::TRANSPARENT_LAYER;
        dc.topology = renderer::PrimitiveTopology::TRIANGLE_LIST;
        drawItems.push_back({ dc, NearestTrailDistanceSq(*trail, ctx.camera.m_position) });
    }

    std::sort(drawItems.begin(), drawItems.end(), [](const TrailDrawItem& a, const TrailDrawItem& b) {
        return a.distanceSq > b.distanceSq;
    });

    for (const auto& item : drawItems)
        SubmitCounted(ctx, item.drawCall);
}

} // namespace fbzz::scene
