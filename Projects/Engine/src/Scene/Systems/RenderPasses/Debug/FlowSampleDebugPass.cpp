/// @file    FlowSampleDebugPass.cpp
/// @brief   格子点で実効流速 (SampleFlow) を評価して矢印で描く。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// @note 設定値ではなく Scene::FlowFrame() の合成結果を引く。環境流・重ねた場・Baked を
///       足し合わせた «粒子や剛体が実際に受ける流れ» はここでしか見えない。
/// @see Docs/design/flow-field.md
#include "DebugPasses.hpp"
#include <Engine/Core/Time.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/Fields/FlowFieldFrame.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <algorithm>
#include <cmath>

namespace fbzz::scene {

namespace {

/// @name 格子の範囲
///@{
/// @note 選択が無いときはカメラ前方のこの距離を中心に置く。足元より少し先が «いま見ている所»。
constexpr float kCameraDistance = 12.0f;
constexpr math::Vector3 kCameraHalfExtents = { 10.0f, 5.0f, 10.0f };
/// @note 1 軸の半幅の上限 [m]。巨大な場を選んでも格子が粗くなりすぎないように。
constexpr float kMaxHalfExtent = 30.0f;
/// @note 全域 (radius <= 0) の場だけを選んだときの半幅。
constexpr float kGlobalFieldHalfExtent = 8.0f;
///@}

/// @note 1 軸の標本数の上限。9^3 = 729 本で矢印が読める密度の限界。
constexpr int   kMaxSamplesPerAxis = 9;
constexpr float kMinSpacing        = 0.25f;
/// @note 色の目盛りの上限 [m/s]。これ以上は同じ赤。長さは画面内の最大値で正規化するので、色で絶対値を読む。
constexpr float kColorMaxSpeed = 10.0f;
constexpr float kStillSpeed    = 1.0e-3f;

struct SampleRegion {
    math::Vector3 center;
    math::Vector3 halfExtents;
    bool          snapToGrid = false;
};

/// @brief 選択中の FlowField を囲む箱。選択に FlowField が無ければ false。
bool ResolveSelectedRegion(RenderPassContext& ctx, SampleRegion& out)
{
    math::Vector3 minP{ 0.0f, 0.0f, 0.0f };
    math::Vector3 maxP{ 0.0f, 0.0f, 0.0f };
    bool found = false;
    for (EntityID id : ctx.scene.GetEntities<FlowField>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        const FlowField* component = ctx.scene.GetComponent<FlowField>(id);
        if (go == nullptr || component == nullptr || !go->activeInHierarchy()) continue;
        if (!IsSelectedForDebug(*go, ctx)) continue;

        for (const FlowFieldSettings& settings : component->forces) {
            if (!settings.enabled) continue;
            float half = kGlobalFieldHalfExtent;
            if (settings.fieldType == FlowFieldType::Baked) {
                /// @note 回転した箱も覆えるよう、最長の半辺を 3 軸に使う。
                half = (std::max)({ std::abs(settings.vectorFieldExtents.x),
                                    std::abs(settings.vectorFieldExtents.y),
                                    std::abs(settings.vectorFieldExtents.z) });
            } else if (settings.radius > 0.0f) {
                half = settings.radius;
            }
            const math::Vector3 c = go->transform.worldPosition;
            const math::Vector3 lo = { c.x - half, c.y - half, c.z - half };
            const math::Vector3 hi = { c.x + half, c.y + half, c.z + half };
            if (!found) {
                minP = lo;
                maxP = hi;
                found = true;
            } else {
                minP = { (std::min)(minP.x, lo.x), (std::min)(minP.y, lo.y), (std::min)(minP.z, lo.z) };
                maxP = { (std::max)(maxP.x, hi.x), (std::max)(maxP.y, hi.y), (std::max)(maxP.z, hi.z) };
            }
        }
    }
    if (!found) return false;
    out.center      = (minP + maxP) * 0.5f;
    out.halfExtents = (maxP - minP) * 0.5f;
    out.snapToGrid  = false;
    return true;
}

/// @brief 速さ [m/s] を 青 → 緑 → 黄 → 赤 へ写す。
math::Vector4 SpeedColor(float speed)
{
    const float t = std::clamp(speed / kColorMaxSpeed, 0.0f, 1.0f);
    if (t < 0.33f) {
        const float k = t / 0.33f;
        return { 0.25f, 0.55f + 0.45f * k, 1.0f - 0.6f * k, 1.0f };
    }
    if (t < 0.66f) {
        const float k = (t - 0.33f) / 0.33f;
        return { 0.25f + 0.75f * k, 1.0f, 0.4f - 0.3f * k, 1.0f };
    }
    const float k = (t - 0.66f) / 0.34f;
    return { 1.0f, 1.0f - 0.75f * k, 0.1f, 1.0f };
}

} // namespace

std::string_view FlowSampleDebugPass::Name() const { return "FlowSampleDebug"; }

bool FlowSampleDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showFlowSamples;
}

void FlowSampleDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    const std::vector<ActiveFlowField>& fields = *ctx.scene.FlowFrame().fields;
    if (fields.empty()) return;

    SampleRegion region;
    if (!ResolveSelectedRegion(ctx, region)) {
        region.center      = ctx.camera.m_position + ctx.camera.GetForward() * kCameraDistance;
        region.halfExtents = kCameraHalfExtents;
        region.snapToGrid  = true;
    }
    region.halfExtents = {
        std::clamp(region.halfExtents.x, kMinSpacing, kMaxHalfExtent),
        std::clamp(region.halfExtents.y, kMinSpacing, kMaxHalfExtent),
        std::clamp(region.halfExtents.z, kMinSpacing, kMaxHalfExtent) };

    const float longest = (std::max)({ region.halfExtents.x, region.halfExtents.y, region.halfExtents.z });
    const float spacing = (std::max)(longest * 2.0f / static_cast<float>(kMaxSamplesPerAxis - 1), kMinSpacing);
    const auto countFor = [spacing](float half) {
        return std::clamp(static_cast<int>(std::floor(half * 2.0f / spacing)) + 1, 1, kMaxSamplesPerAxis);
    };
    const int nx = countFor(region.halfExtents.x);
    const int ny = countFor(region.halfExtents.y);
    const int nz = countFor(region.halfExtents.z);

    math::Vector3 origin = {
        region.center.x - spacing * static_cast<float>(nx - 1) * 0.5f,
        region.center.y - spacing * static_cast<float>(ny - 1) * 0.5f,
        region.center.z - spacing * static_cast<float>(nz - 1) * 0.5f };
    /// @note カメラ追従の格子はワールドの目へ揃える。揃えないとカメラを動かすたびに標本点が滑り、流れが揺れて見える。
    if (region.snapToGrid) {
        origin = { std::floor(origin.x / spacing) * spacing,
                   std::floor(origin.y / spacing) * spacing,
                   std::floor(origin.z / spacing) * spacing };
    }

    m_positions.clear();
    m_velocities.clear();
    float maxSpeed = 0.0f;
    for (int ix = 0; ix < nx; ++ix) {
        for (int iy = 0; iy < ny; ++iy) {
            for (int iz = 0; iz < nz; ++iz) {
                const math::Vector3 p = { origin.x + spacing * static_cast<float>(ix),
                                          origin.y + spacing * static_cast<float>(iy),
                                          origin.z + spacing * static_cast<float>(iz) };
                bool covered = false;
                /// @note channels は全ビット。特定のエミッターだけが受ける場も «ここに流れがある» として描く。
                const math::Vector3 v = SampleFlow(p, fields, 0xFFFFFFFFu, Time::time, &covered);
                if (!covered) continue;
                m_positions.push_back(p);
                m_velocities.push_back(v);
                maxSpeed = (std::max)(maxSpeed, v.Length());
            }
        }
    }
    if (m_positions.empty()) return;

    constexpr math::Vector4 kStillColor = { 0.6f, 0.6f, 0.65f, 0.8f };
    const float maxLength = spacing * 0.85f;
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetGpuViewProjection());
    for (size_t i = 0; i < m_positions.size(); ++i) {
        const math::Vector3& p = m_positions[i];
        const math::Vector3& v = m_velocities[i];
        const float speed = v.Length();
        if (speed < kStillSpeed || maxSpeed < kStillSpeed) {
            /// @note 覆われているが流速 0 の点。«場の外» と区別するため小さな十字を置く。
            const float s = spacing * 0.05f;
            renderer::DebugDraw::Line(ctx.renderer, p - math::Vector3{ s, 0, 0 }, p + math::Vector3{ s, 0, 0 }, kStillColor);
            renderer::DebugDraw::Line(ctx.renderer, p - math::Vector3{ 0, 0, s }, p + math::Vector3{ 0, 0, s }, kStillColor);
            continue;
        }
        const float length = maxLength * (speed / maxSpeed);
        const math::Vector3 dir = v * (1.0f / speed);
        /// @note 矢印は標本点を中心に置く。始点に置くと格子の片側へ偏って場の中心がずれて見える。
        const math::Vector3 from = p - dir * (length * 0.5f);
        const math::Vector3 to   = p + dir * (length * 0.5f);
        const float head = (std::max)(length * 0.3f, spacing * 0.06f);
        renderer::DebugDraw::Arrow(ctx.renderer, from, to, head, head * 0.35f, SpeedColor(speed));
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
