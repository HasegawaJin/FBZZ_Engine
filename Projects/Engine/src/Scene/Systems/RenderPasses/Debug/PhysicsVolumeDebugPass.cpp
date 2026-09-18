/// @file    PhysicsVolumeDebugPass.cpp
/// @brief   VolumeComponent のトリガー形状・効果の向き・残り時間を描く。
/// @author  Hasegawa Jin
/// @date    2026-09-17
///
/// @note 範囲と向きは physics::ColliderVolume::Contains / Apply の式から描く。
///       Vortex は «コライダーの AABB 中心を通るワールド Y 軸» 周りで、Transform の回転を受けない。
#include "DebugPasses.hpp"
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/ColliderComponent.hpp>
#include <Engine/Scene/Components/VolumeComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/ColliderSync.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Physics/ColliderDebugGeometry.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace fbzz::scene {

namespace {

constexpr math::Vector4 kWarningColor  = { 1.00f, 0.20f, 0.20f, 1.0f };
constexpr math::Vector4 kInactiveColor = { 0.55f, 0.55f, 0.60f, 0.6f };
constexpr float         kUnselectedAlpha = 0.6f;
constexpr float         kTwoPi = 6.28318530f;

math::Vector4 VolumeColor(physics::VolumeType type)
{
    switch (type) {
    case physics::VolumeType::Gravity:      return { 0.40f, 0.65f, 1.00f, 1.0f };
    case physics::VolumeType::Vortex:       return { 0.80f, 0.50f, 1.00f, 1.0f };
    case physics::VolumeType::Explosion:    return { 1.00f, 0.55f, 0.20f, 1.0f };
    case physics::VolumeType::TimeDilation: return { 0.70f, 1.00f, 0.40f, 1.0f };
    case physics::VolumeType::Magnetic:     return { 1.00f, 0.35f, 0.65f, 1.0f };
    }
    return { 1.0f, 1.0f, 1.0f, 1.0f };
}

math::Vector4 WithAlpha(const math::Vector4& color, float alpha)
{
    return { color.x, color.y, color.z, color.w * alpha };
}

/// @brief ColliderVolume::Contains が正しく判定できる形状か。
/// @note それ以外 (OBB / Mesh / 凸包 / HeightField) は Capsule として読まれ、範囲が壊れる。
bool IsVolumeShapeSupported(physics::ColliderType type)
{
    return type == physics::ColliderType::SPHERE || type == physics::ColliderType::AABB
        || type == physics::ColliderType::CYLINDER || type == physics::ColliderType::CAPSULE;
}

/// @brief 同じ GameObject に付いたトリガーコライダー群の線と外接箱。
struct TriggerShape {
    bool           found     = false;
    bool           supported = true;
    physics::AABB  bounds{};
};

template<typename T>
void CollectTrigger(RenderPassContext& ctx, GameObject& go, const math::Vector4& color,
                    const physics::ColliderDebugView& view, TriggerShape& shape)
{
    T* col = go.GetComponent<T>();
    if (col == nullptr || !col->enabled || !col->isTrigger) return;
    if (!PrepareCollider(ctx.scene, go, *col)) return;

    const physics::ColliderDebugGeometry geometry = physics::BuildColliderDebugGeometry(*col->collider, view);
    for (const physics::DebugLine& line : geometry.lines)
        renderer::DebugDraw::Line(ctx.renderer, line.from, line.to, color);

    const physics::AABB aabb = col->collider->GetAABB();
    shape.bounds    = shape.found ? shape.bounds.Merge(aabb) : aabb;
    shape.found     = true;
    shape.supported = shape.supported && IsVolumeShapeSupported(col->collider->GetType());
}

TriggerShape DrawTriggers(RenderPassContext& ctx, GameObject& go, const math::Vector4& color)
{
    physics::ColliderDebugView view;
    view.cameraPosition = ctx.camera.m_position;
    view.enabled        = true;

    TriggerShape shape;
    CollectTrigger<AabbColliderComponent>(ctx, go, color, view, shape);
    CollectTrigger<BoxColliderComponent>(ctx, go, color, view, shape);
    CollectTrigger<SphereColliderComponent>(ctx, go, color, view, shape);
    CollectTrigger<CapsuleColliderComponent>(ctx, go, color, view, shape);
    CollectTrigger<CylinderColliderComponent>(ctx, go, color, view, shape);
    CollectTrigger<MeshColliderComponent>(ctx, go, color, view, shape);
    CollectTrigger<ConvexHullColliderComponent>(ctx, go, color, view, shape);
    return shape;
}

/// @brief トリガーが無い Volume の目印。物理側は何も言わずに効かないので、置いた場所に赤い印を出す。
void DrawMissingTrigger(renderer::IRenderer& r, const math::Vector3& position)
{
    constexpr float s = 0.4f;
    renderer::DebugDraw::Line(r, position + math::Vector3{ -s, -s, 0 }, position + math::Vector3{ s, s, 0 }, kWarningColor);
    renderer::DebugDraw::Line(r, position + math::Vector3{ -s, s, 0 }, position + math::Vector3{ s, -s, 0 }, kWarningColor);
    renderer::DebugDraw::Line(r, position + math::Vector3{ 0, -s, -s }, position + math::Vector3{ 0, s, s }, kWarningColor);
    renderer::DebugDraw::Line(r, position + math::Vector3{ 0, s, -s }, position + math::Vector3{ 0, -s, s }, kWarningColor);
    DrawDebugSphere(r, position, s, kWarningColor, true);
}

void DrawArrow(renderer::IRenderer& r, const math::Vector3& from, const math::Vector3& to,
               const math::Vector4& color)
{
    const float length = (to - from).Length();
    if (length < 1.0e-4f) return;
    renderer::DebugDraw::Arrow(r, from, to, length * 0.25f, length * 0.08f, color);
}

/// @brief duration の残りを箱の天面の輪で示す。明るい弧が残り、暗い輪が全体。
void DrawRemainingTime(renderer::IRenderer& r, const VolumeComponent& volume, const physics::AABB& bounds,
                       float ringRadius, const math::Vector4& color)
{
    if (volume.duration < 0.0f) return;
    const math::Vector3 center = { (bounds.min.x + bounds.max.x) * 0.5f, bounds.max.y,
                                   (bounds.min.z + bounds.max.z) * 0.5f };
    DrawDebugCircle(r, center, math::Vector3::UP, ringRadius, WithAlpha(color, 0.3f));
    const float remaining = volume.duration > 0.0f
        ? std::clamp(1.0f - volume.elapsed / volume.duration, 0.0f, 1.0f) : 0.0f;
    if (remaining <= 0.0f) return;

    constexpr int kSegments = 32;
    const int segments = (std::max)(1, static_cast<int>(std::ceil(kSegments * remaining)));
    math::Vector3 prev = center + math::Vector3::RIGHT * ringRadius;
    for (int i = 1; i <= segments; ++i) {
        const float angle = kTwoPi * remaining * static_cast<float>(i) / static_cast<float>(segments);
        const math::Vector3 next = center + math::Vector3{ std::cos(angle), 0.0f, std::sin(angle) } * ringRadius;
        renderer::DebugDraw::Line(r, prev, next, color);
        prev = next;
    }
}

void DrawEffect(renderer::IRenderer& r, const VolumeComponent& volume, const physics::AABB& bounds,
                const math::Vector4& color)
{
    const math::Vector3 center = bounds.Center();
    const math::Vector3 ext    = bounds.Extents();
    const float horizontal = (std::max)((std::min)(ext.x, ext.z), 0.25f);
    const float glyph      = (std::max)((std::min)({ ext.x, ext.y, ext.z }), 0.25f);

    switch (volume.type) {
    case physics::VolumeType::Gravity: {
        const float g = volume.gravity.Length();
        if (g < 1.0e-4f) break;
        const math::Vector3 dir = volume.gravity * (1.0f / g);
        /// @note 長さは標準重力を 1 とした比。0.25〜2 倍に丸めて箱の大きさへ掛ける。
        const float length = glyph * 0.8f * std::clamp(g / 9.81f, 0.25f, 2.0f);
        static constexpr std::array<math::Vector3, 5> kOffsets = {
            math::Vector3{ 0, 0, 0 }, math::Vector3{ 0.5f, 0, 0.5f }, math::Vector3{ -0.5f, 0, 0.5f },
            math::Vector3{ 0.5f, 0, -0.5f }, math::Vector3{ -0.5f, 0, -0.5f } };
        for (const math::Vector3& o : kOffsets) {
            const math::Vector3 p = center + math::Vector3{ o.x * ext.x, 0.0f, o.z * ext.z };
            DrawArrow(r, p - dir * (length * 0.5f), p + dir * (length * 0.5f), color);
        }
        break;
    }
    case physics::VolumeType::Vortex: {
        const float ring = horizontal * 0.7f;
        DrawDebugSwirl(r, center, math::Vector3::UP, ring, volume.swirlStrength, color);
        if (volume.inwardStrength != 0.0f) {
            const float sign = volume.inwardStrength > 0.0f ? 1.0f : -1.0f;
            static constexpr std::array<math::Vector3, 4> kDirs = {
                math::Vector3{ 1, 0, 0 }, math::Vector3{ -1, 0, 0 }, math::Vector3{ 0, 0, 1 }, math::Vector3{ 0, 0, -1 } };
            for (const math::Vector3& d : kDirs) {
                const math::Vector3 outer = center + d * ring;
                const math::Vector3 inner = center + d * (ring * 0.55f);
                DrawArrow(r, sign > 0.0f ? outer : inner, sign > 0.0f ? inner : outer, color);
            }
        }
        if (volume.liftStrength != 0.0f) {
            const float sign = volume.liftStrength > 0.0f ? 1.0f : -1.0f;
            DrawArrow(r, center, center + math::Vector3::UP * (glyph * 0.7f * sign), color);
        }
        break;
    }
    case physics::VolumeType::Explosion: {
        static constexpr std::array<math::Vector3, 6> kDirs = {
            math::Vector3{ 1, 0, 0 }, math::Vector3{ -1, 0, 0 }, math::Vector3{ 0, 1, 0 },
            math::Vector3{ 0, -1, 0 }, math::Vector3{ 0, 0, 1 }, math::Vector3{ 0, 0, -1 } };
        for (const math::Vector3& d : kDirs)
            DrawArrow(r, center + d * (glyph * 0.2f), center + d * (glyph * 0.8f), color);
        break;
    }
    case physics::VolumeType::TimeDilation: {
        /// @note 破線 = 等速 (1x)、実線 = この Volume の timeScale。内側なら遅く、外側なら速い。
        const float reference = horizontal * 0.4f;
        DrawDebugCircle(r, center, math::Vector3::UP, reference, WithAlpha(color, 0.5f), true);
        DrawDebugCircle(r, center, math::Vector3::UP, reference * std::clamp(volume.timeScale, 0.02f, 2.0f), color);
        break;
    }
    case physics::VolumeType::Magnetic: {
        const float b = volume.magneticField.Length();
        if (b < 1.0e-4f) break;
        const math::Vector3 dir = volume.magneticField * (1.0f / b);
        const math::Vector3 side = DebugPerpendicular(dir) * (glyph * 0.4f);
        const float length = glyph * 0.8f;
        for (float k : { -1.0f, 0.0f, 1.0f }) {
            const math::Vector3 p = center + side * k;
            DrawArrow(r, p - dir * (length * 0.5f), p + dir * (length * 0.5f), color);
        }
        break;
    }
    }
}

} // namespace

std::string_view PhysicsVolumeDebugPass::Name() const { return "PhysicsVolumeDebug"; }

bool PhysicsVolumeDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showPhysicsVolumes;
}

void PhysicsVolumeDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (EntityID id : ctx.scene.GetEntities<VolumeComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        const VolumeComponent* volume = ctx.scene.GetComponent<VolumeComponent>(id);
        if (go == nullptr || volume == nullptr || !go->activeInHierarchy()) continue;

        /// @note PhysicsSystem と同じ «効いているか» の判定。無効・期限切れは灰色で形だけ残す。
        const bool expired = volume->duration >= 0.0f && volume->elapsed >= volume->duration;
        const bool active  = volume->enabled && !expired;
        const float alpha  = IsSelectedForDebug(*go, ctx) ? 1.0f : kUnselectedAlpha;
        const math::Vector4 color = WithAlpha(active ? VolumeColor(volume->type) : kInactiveColor, alpha);

        const TriggerShape shape = DrawTriggers(ctx, *go, color);
        if (!shape.found) {
            DrawMissingTrigger(ctx.renderer, go->transform.worldPosition);
            continue;
        }
        if (!shape.supported) {
            renderer::DebugDraw::Line(ctx.renderer, shape.bounds.min, shape.bounds.max, kWarningColor);
            renderer::DebugDraw::Line(ctx.renderer,
                { shape.bounds.min.x, shape.bounds.max.y, shape.bounds.min.z },
                { shape.bounds.max.x, shape.bounds.min.y, shape.bounds.max.z }, kWarningColor);
        }

        DrawEffect(ctx.renderer, *volume, shape.bounds, color);
        const math::Vector3 ext = shape.bounds.Extents();
        DrawRemainingTime(ctx.renderer, *volume, shape.bounds,
                          (std::max)((std::min)(ext.x, ext.z) * 0.3f, 0.2f), color);
    }
    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
