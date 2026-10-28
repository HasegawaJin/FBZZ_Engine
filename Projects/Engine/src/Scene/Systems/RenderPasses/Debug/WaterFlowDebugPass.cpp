/// @file    WaterFlowDebugPass.cpp
/// @brief   水面の水流・渦・浮力の届く深さを全 WaterComponent について描く。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// @note 読むのは WaterSystem が毎フレーム書くランタイム値 (current / surfaceFlows / waves)。
///       .mat の値を読むと環境風や倍率を掛ける前の «効いていない» 値になる。
/// @see Docs/design/water-waves.md
#include "DebugPasses.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WaterExclusionComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/WaterZoneSnapshot.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

constexpr math::Vector4 kOutlineColor  = { 0.30f, 0.80f, 1.00f, 1.0f };
constexpr math::Vector4 kCurrentColor  = { 0.35f, 1.00f, 0.90f, 1.0f };
constexpr math::Vector4 kSurfaceFlowColor = { 0.80f, 0.55f, 1.00f, 1.0f };
constexpr math::Vector4 kDepthColor    = { 0.30f, 0.55f, 0.90f, 0.45f };
constexpr math::Vector4 kExclusionColor = { 1.00f, 0.18f, 0.62f, 0.95f };
constexpr float         kUnselectedAlpha = 0.6f;

/// @note 1 辺の矢印の最大本数。海サイズの水面でも画面が矢印で埋まらない密度。
constexpr int   kMaxArrowsPerSide = 7;
/// @note 矢印の間隔の下限 [m]。小さなプールで矢印が重ならないように。
constexpr float kMinArrowSpacing  = 1.0f;
/// @note 流速 1 m/s を何秒ぶんの長さで描くか。間隔の 8 割で頭打ちにする。
constexpr float kSecondsPerArrow  = 1.0f;

math::Vector4 WithAlpha(const math::Vector4& color, float alpha)
{
    return { color.x, color.y, color.z, color.w * alpha };
}

void DrawRect(renderer::IRenderer& r, const math::Vector3& center, float halfX, float halfZ,
              const math::Vector4& color, bool dashed)
{
    const math::Vector3 corners[4] = {
        { center.x - halfX, center.y, center.z - halfZ }, { center.x + halfX, center.y, center.z - halfZ },
        { center.x + halfX, center.y, center.z + halfZ }, { center.x - halfX, center.y, center.z + halfZ } };
    for (int i = 0; i < 4; ++i) {
        const math::Vector3& a = corners[i];
        const math::Vector3& b = corners[(i + 1) % 4];
        if (dashed) renderer::DebugDraw::LineDashed(r, a, b, color, (std::max)(halfX, halfZ) * 0.05f, 24);
        else        renderer::DebugDraw::Line(r, a, b, color);
    }
}

} /// @note namespace

std::string_view WaterFlowDebugPass::Name() const { return "WaterFlowDebug"; }

bool WaterFlowDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    if (ctx.settings.showWaterFlow) return true;
    for (EntityID id : ctx.scene.GetEntities<WaterExclusionComponent>()) {
        const GameObject* go = ctx.scene.GetGameObject(id);
        if (go && go->activeInHierarchy() && IsSelectedForDebug(*go, ctx)) return true;
    }
    return false;
}

void WaterFlowDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    PrepareWaterSceneSurfaces(ctx.scene);
    const float time = Time::time;
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    if (ctx.settings.showWaterFlow) for (EntityID id : ctx.scene.GetEntities<WaterComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        const WaterComponent* water = ctx.scene.GetComponent<WaterComponent>(id);
        if (go == nullptr || water == nullptr || !go->activeInHierarchy()
            || WaterSurfaceZoneOwner(*water, WaterSurfaceReadView::SIMULATION).IsValid()) continue;

        const Transform& tf = WaterSurfaceTransform(*water, go->transform, WaterSurfaceReadView::SIMULATION);
        const math::Vector3 center = tf.worldPosition;
        const math::Vector2 localExtent = WaterSurfaceLocalExtent(*water, WaterSurfaceReadView::SIMULATION);
        const float halfX = localExtent.x * 0.5f * std::abs(tf.worldScale.x);
        const float halfZ = localExtent.y * 0.5f * std::abs(tf.worldScale.z);
        const float alpha = (IsSelectedForDebug(*go, ctx) ? 1.0f : kUnselectedAlpha)
                          * (water->enabled ? 1.0f : 0.4f);

        if (UsesWaterSurfaceGeometry(*water, WaterSurfaceReadView::SIMULATION)) {
            const auto* geometry = WaterSurfaceGeometry(*water, WaterSurfaceReadView::SIMULATION);
            if (!geometry) continue;
            const auto& vertices = geometry->vertices;
            const auto drawEdge = [&](size_t a, size_t b) {
                renderer::DebugDraw::Line(ctx.renderer, vertices[a].worldPosition, vertices[b].worldPosition,
                    WithAlpha(kOutlineColor, alpha));
            };
            const WaterZoneSnapshot* zoneSnapshot = water->simulationReadEnabled
                && water->simulationSurfacePublished
                ? water->simulationZoneSnapshot.get() : water->zoneSnapshot.get();
            if (zoneSnapshot) {
                for (const auto& edge : zoneSnapshot->boundaryEdges) drawEdge(edge[0], edge[1]);
            } else {
                const auto& outline = geometry->outlineIndices;
                for (size_t i = 0; i < outline.size(); ++i) drawEdge(outline[i], outline[(i + 1) % outline.size()]);
            }
            const size_t stride = (std::max)(size_t{1}, vertices.size() / 32u);
            for (size_t i = 0; i < vertices.size(); i += stride) {
                const auto& vertex = vertices[i];
                const float speed = vertex.worldVelocity.Length();
                if (speed > 1.0e-3f) {
                    const auto offset = vertex.worldVelocity * ((std::min)(speed, 4.0f) / speed);
                    renderer::DebugDraw::Arrow(ctx.renderer, vertex.worldPosition, vertex.worldPosition + offset,
                        offset.Length() * 0.3f, offset.Length() * 0.1f, WithAlpha(kCurrentColor, alpha));
                }
                if (water->buoyancyEnabled)
                    renderer::DebugDraw::Line(ctx.renderer, vertex.worldPosition,
                        vertex.worldPosition - math::Vector3{0.0f, vertex.depth, 0.0f}, WithAlpha(kDepthColor, alpha));
            }
            continue;
        }

        DrawRect(ctx.renderer, center, halfX, halfZ, WithAlpha(kOutlineColor, alpha), false);
        if (water->buoyancyEnabled && water->buoyancyDepth > 0.0f) {
            /// @note 浮力はここより深くへ届かない。水面の下に洞窟を置いたときの切り分け用。
            DrawRect(ctx.renderer, center - math::Vector3{ 0.0f, water->buoyancyDepth, 0.0f },
                     halfX, halfZ, WithAlpha(kDepthColor, alpha), true);
        }

        const float speed = water->current.Length();
        if (speed > 1.0e-3f) {
            const float spacing = (std::max)((std::max)(halfX, halfZ) * 2.0f / kMaxArrowsPerSide, kMinArrowSpacing);
            const int nx = std::clamp(static_cast<int>(halfX * 2.0f / spacing), 1, kMaxArrowsPerSide);
            const int nz = std::clamp(static_cast<int>(halfZ * 2.0f / spacing), 1, kMaxArrowsPerSide);
            const math::Vector3 dir = { water->current.x / speed, 0.0f, water->current.y / speed };
            const float length = (std::min)(speed * kSecondsPerArrow, spacing * 0.8f);
            const float head   = length * 0.3f;
            const math::Vector4 color = WithAlpha(kCurrentColor, alpha);
            for (int ix = 0; ix < nx; ++ix) {
                for (int iz = 0; iz < nz; ++iz) {
                    const float x = center.x - halfX + halfX * 2.0f * (static_cast<float>(ix) + 0.5f) / nx;
                    const float z = center.z - halfZ + halfZ * 2.0f * (static_cast<float>(iz) + 0.5f) / nz;
                    /// @note 波に乗せて描く。基準面に置くと高い波の下へ矢印が沈んで見えない。
                    const float y = center.y + water->GetSurfaceHeightAt(x, z, time,
                        WaterSurfaceReadView::SIMULATION) + 0.05f;
                    const math::Vector3 p = { x, y, z };
                    renderer::DebugDraw::Arrow(ctx.renderer, p - dir * (length * 0.5f), p + dir * (length * 0.5f),
                                               head, head * 0.35f, color);
                }
            }
        }

        const math::Vector4 flowColor = WithAlpha(kSurfaceFlowColor, alpha);
        const int liveFlows = (std::min)(water->surfaceFlowCount,
                                         static_cast<int>(water->surfaceFlows.size()));
        for (int i = 0; i < liveFlows; ++i) {
            const WaterSurfaceFlow& flow = water->surfaceFlows[static_cast<size_t>(i)];
            /// @note 半径を持たない Baked は箱で描くべきなので、ここでは円を出さない。
            if (flow.radius <= 0.0f) continue;
            const math::Vector3 c = { flow.center.x, center.y, flow.center.y };
            DrawDebugCircle(ctx.renderer, c, math::Vector3::UP, flow.radius, WithAlpha(flowColor, 0.5f), true);
            /// @note 渦の speed は回転軸 y の符号つき。SampleFlow と同じ «軸まわり右回り» で描く。
            if (flow.kind == FlowFieldType::Vortex) {
                DrawDebugSwirl(ctx.renderer, c, math::Vector3::UP, flow.radius * 0.5f,
                               flow.speed >= 0.0f ? 1.0f : -1.0f, flowColor);
            }
            /// @note 形の向き。穴は下、盛り上がりは上へ矢印を出す。
            if (std::abs(flow.height) > 0.0f) {
                const math::Vector3 tip = c + math::Vector3{ 0.0f, flow.height, 0.0f };
                const float head = std::abs(flow.height) * 0.25f;
                renderer::DebugDraw::Arrow(ctx.renderer, c, tip, head, head * 0.32f, flowColor);
            }
        }
    }
    for (EntityID id : ctx.scene.GetEntities<WaterExclusionComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        const WaterExclusionComponent* exclusion = ctx.scene.GetComponent<WaterExclusionComponent>(id);
        if (!go || !exclusion || !go->activeInHierarchy()
            || (!ctx.settings.showWaterFlow && !IsSelectedForDebug(*go, ctx))) continue;
        const Transform& transform = go->transform;
        const double rotationLength = transform.worldRotation.Length();
        if (!std::isfinite(transform.worldPosition.x) || !std::isfinite(transform.worldPosition.y)
            || !std::isfinite(transform.worldPosition.z) || !std::isfinite(rotationLength)
            || std::abs(rotationLength - 1.0) > 0.001
            || !std::isfinite(exclusion->size.x) || !std::isfinite(exclusion->size.y)
            || !std::isfinite(exclusion->size.z) || exclusion->size.x <= 0.0f
            || exclusion->size.y <= 0.0f || exclusion->size.z <= 0.0f) continue;
        const math::Vector3 halfExtent{
            exclusion->size.x * std::abs(transform.worldScale.x) * 0.5f,
            exclusion->size.y * std::abs(transform.worldScale.y) * 0.5f,
            exclusion->size.z * std::abs(transform.worldScale.z) * 0.5f
        };
        const float alpha = exclusion->enabled ? 1.0f : 0.35f;
        renderer::DebugDraw::Box(ctx.renderer, transform.worldPosition, halfExtent,
            transform.worldRotation, WithAlpha(kExclusionColor, alpha));
    }
    renderer::DebugDraw::Flush();
}

} /// @note namespace fbzz::scene
