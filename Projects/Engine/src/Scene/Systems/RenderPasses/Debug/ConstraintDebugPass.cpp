/// @file    ConstraintDebugPass.cpp
/// @brief   物理拘束を描く。張られている拘束は World の実体から、未接続の JointComponent は設定値から。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#include "DebugPasses.hpp"
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/JointComponent.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/MathUtils.hpp>
#include <Physics/ConstraintDebugGeometry.hpp>
#include <Physics/World.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr math::Vector4 kLiveColor    = { 1.00f, 0.82f, 0.18f, 1.0f };
/// 未接続 (編集中 / 相手が見つからない) の拘束。張られた実体と見分ける。
constexpr math::Vector4 kAuthorColor  = { 0.95f, 0.60f, 0.20f, 1.0f };
constexpr math::Vector4 kAxisColor    = { 0.30f, 0.80f, 1.00f, 1.0f };
constexpr math::Vector4 kLimitColor   = { 0.40f, 1.00f, 0.50f, 1.0f };
constexpr math::Vector4 kMissingColor = { 1.00f, 0.25f, 0.20f, 1.0f };
constexpr float         kMarker       = 0.05f;

/// @brief JointSync の相手探しを GameObject 単位でなぞる。剛体の実体は見ない (編集中は無いため)。
GameObject* FindAuthoredPartner(Scene& scene, GameObject& go, const JointComponent& joint)
{
    if (joint.connectedBody.IsValid()) return joint.connectedBody.Resolve(scene);
    if (!joint.connectToParent) return nullptr;
    for (GameObject* parent = go.GetParent(); parent; parent = parent->GetParent()) {
        const auto* rb = parent->GetComponent<RigidBodyComponent>();
        if (rb && rb->enabled && parent->activeInHierarchy()) return parent;
    }
    return nullptr;
}

math::Vector3 SafeAxis(const math::Vector3& axis)
{
    return axis.LengthSq() > 1e-8f ? axis.Normalized() : math::Vector3::UP;
}

/// @brief axis に直交する基準方向。hint を射影し、平行なら別の軸から作る。
math::Vector3 PerpendicularTo(const math::Vector3& axis, const math::Vector3& hint)
{
    math::Vector3 planar = hint - axis * math::Vector3::Dot(hint, axis);
    if (planar.LengthSq() < 1e-6f) {
        const math::Vector3 fallback = std::abs(axis.y) < 0.99f ? math::Vector3::UP : math::Vector3{ 1, 0, 0 };
        planar = math::Vector3::Cross(axis, fallback);
    }
    return planar.Normalized();
}

void DrawAuthoredJoint(RenderPassContext& ctx, GameObject& go, const JointComponent& joint)
{
    auto& r = ctx.renderer;
    const Transform& self = go.transform;

    if (joint.type == JointType::Chain) {
        std::vector<math::Vector3> points{ self.worldPosition };
        for (const EntityRef& ref : joint.chainBodies) {
            GameObject* body = ref.Resolve(ctx.scene);
            if (!body) {
                renderer::DebugDraw::Sphere(r, points.back(), kMarker * 2.0f, kMissingColor);
                break;
            }
            points.push_back(body->transform.worldPosition);
        }
        renderer::DebugDraw::Polyline(r, points, false, kAuthorColor);
        for (const math::Vector3& point : points)
            renderer::DebugDraw::Sphere(r, point, kMarker, kAuthorColor);
        return;
    }

    GameObject* partner = FindAuthoredPartner(ctx.scene, go, joint);
    if (!partner || partner == &go) {
        renderer::DebugDraw::Sphere(r, self.worldPosition, kMarker * 2.0f, kMissingColor);
        return;
    }
    const Transform& other = partner->transform;
    renderer::DebugDraw::LineDashed(r, self.worldPosition, other.worldPosition, kAuthorColor, 0.1f, 16);

    const math::Vector3 axis = SafeAxis(joint.axis);
    switch (joint.type) {
    case JointType::Hinge: {
        const math::Vector3 anchorA = self.worldPosition + self.worldRotation * joint.anchor;
        const math::Vector3 anchorB = other.worldPosition + other.worldRotation * joint.connectedAnchor;
        renderer::DebugDraw::Sphere(r, anchorA, kMarker, kAuthorColor);
        renderer::DebugDraw::Sphere(r, anchorB, kMarker, kAuthorColor);
        renderer::DebugDraw::Line(r, anchorA, anchorB, kMissingColor);
        renderer::DebugDraw::Arrow(r, anchorA, anchorA + axis * 0.5f, 0.1f, 0.03f, kAxisColor);
        if (joint.useLimits) {
            const math::Vector3 reference = PerpendicularTo(axis, self.worldPosition - anchorA);
            const math::Vector3 from =
                math::Quaternion::FromAxisAngle(axis, math::ToRad(joint.lowerLimit)) * reference;
            const float sweep = math::ToRad(joint.upperLimit - joint.lowerLimit);
            renderer::DebugDraw::Arc(r, anchorA, axis, from, 0.35f, sweep, kLimitColor);
            renderer::DebugDraw::Line(r, anchorA, anchorA + from * 0.35f, kLimitColor);
            const math::Vector3 to =
                math::Quaternion::FromAxisAngle(axis, math::ToRad(joint.upperLimit)) * reference;
            renderer::DebugDraw::Line(r, anchorA, anchorA + to * 0.35f, kLimitColor);
        }
        break;
    }
    case JointType::Slider:
        renderer::DebugDraw::Arrow(r, self.worldPosition, self.worldPosition + axis * 0.5f, 0.1f, 0.03f, kAxisColor);
        if (joint.useLimits)
            renderer::DebugDraw::Line(r, self.worldPosition + axis * joint.lowerLimit,
                                      self.worldPosition + axis * joint.upperLimit, kLimitColor);
        break;
    case JointType::Distance:
    case JointType::Rope:
    case JointType::Spring: {
        /// @note autoDistance は張った瞬間の間隔を使うので、編集中は «今の間隔» がそのまま長さ。
        if (joint.autoDistance) break;
        const math::Vector3 toSelf = self.worldPosition - other.worldPosition;
        const float gap = toSelf.Length();
        if (gap < 1e-5f) break;
        const math::Vector3 restPoint = other.worldPosition + toSelf * ((std::max)(joint.distance, 0.0f) / gap);
        renderer::DebugDraw::Sphere(r, restPoint, kMarker, kLimitColor);
        break;
    }
    case JointType::Fixed:
    case JointType::Chain:
        break;
    }
}

} // namespace

std::string_view ConstraintDebugPass::Name() const { return "ConstraintDebug"; }

bool ConstraintDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showConstraints;
}

void ConstraintDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    if (ctx.physicsWorld) {
        for (const auto* constraint : ctx.physicsWorld->GetConstraints()) {
            if (!constraint) continue;
            const physics::ConstraintDebugGeometry geometry =
                physics::BuildConstraintDebugGeometry(*constraint);
            for (const physics::DebugLine& line : geometry.lines)
                renderer::DebugDraw::Line(ctx.renderer, line.from, line.to, kLiveColor);
        }
    }

    /// @note 物理は Play 中しか拘束を張らない (SimOnly)。編集中と接続に失敗した関節は設定値から描く。
    for (EntityID id : ctx.scene.GetEntities<JointComponent>()) {
        auto* joint = ctx.scene.GetComponent<JointComponent>(id);
        GameObject* go = ctx.scene.GetGameObject(id);
        if (!joint || !go || !joint->enabled || !go->activeInHierarchy()) continue;
        if (joint->connected && ctx.physicsWorld) continue;
        DrawAuthoredJoint(ctx, *go, *joint);
    }

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
