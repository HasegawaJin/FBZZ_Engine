/// @file    RigDebugPasses.cpp
/// @brief   IK チェーン・SpringBone・ソケット / Transform 拘束の追従関係を描く。
/// @author  Hasegawa Jin
/// @date    2026-09-16
#include "DebugPasses.hpp"
#include <Engine/Asset/Model.hpp>
#include <Engine/Renderer/DebugDraw.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/RenderPasses/RenderPassContext.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace fbzz::scene {

namespace {

constexpr float kMarker = 0.04f;

/// @brief IKSystem / SpringBoneSystem と同じ探し方 (自身 → 直下の子)。
SkinnedMeshRenderer* FindRigRenderer(GameObject& owner)
{
    if (auto* renderer = owner.GetComponent<SkinnedMeshRenderer>()) return renderer;
    for (int i = 0, count = owner.GetChildCount(); i < count; ++i)
        if (GameObject* child = owner.GetChild(i))
            if (auto* renderer = child->GetComponent<SkinnedMeshRenderer>()) return renderer;
    return nullptr;
}

/// @brief ノード番号の骨 GameObject。範囲外や未生成なら nullptr。
GameObject* BoneObject(Scene& scene, const SkinnedMeshRenderer& smr, int nodeIndex)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(smr.nodeEntities.size())) return nullptr;
    return scene.GetGameObject(smr.nodeEntities[static_cast<size_t>(nodeIndex)]);
}

/// @brief 骨名の完全一致だけを引く。IKSystem のヒューマノイド別名解決はここでは真似しない。
GameObject* BoneByName(Scene& scene, const SkinnedMeshRenderer& smr, const std::string& name)
{
    if (!smr.model || !smr.model->skeleton) return nullptr;
    const auto& nodeMap = smr.model->skeleton->nodeMap;
    const auto it = nodeMap.find(name);
    return it == nodeMap.end() ? nullptr : BoneObject(scene, smr, it->second);
}

math::Vector3 ScaledRotate(const Transform& tf, const math::Vector3& local)
{
    return tf.worldRotation * math::Vector3{ local.x * tf.worldScale.x,
                                             local.y * tf.worldScale.y,
                                             local.z * tf.worldScale.z };
}

/// @brief base の子孫から name の GameObject を幅優先で探す。見つからなければ nullptr。
GameObject* FindDescendantByName(GameObject& base, const std::string& name)
{
    std::vector<GameObject*> open{ &base };
    for (size_t i = 0; i < open.size(); ++i) {
        GameObject* current = open[i];
        if (current->name == name) return current;
        for (int c = 0, count = current->GetChildCount(); c < count; ++c)
            if (GameObject* child = current->GetChild(c)) open.push_back(child);
    }
    return nullptr;
}

} // namespace

/// @name IK

std::string_view IKDebugPass::Name() const { return "IKDebug"; }

bool IKDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showIK;
}

void IKDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kChainColor    = { 0.30f, 0.90f, 1.00f, 1.0f };
    constexpr math::Vector4 kDisabledColor = { 0.45f, 0.50f, 0.55f, 1.0f };
    constexpr math::Vector4 kTargetColor   = { 1.00f, 0.35f, 0.35f, 1.0f };
    constexpr math::Vector4 kPoleColor     = { 1.00f, 0.80f, 0.30f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    std::vector<math::Vector3> joints;
    for (EntityID id : ctx.scene.GetEntities<IKSolverComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* ik = ctx.scene.GetComponent<IKSolverComponent>(id);
        if (!go || !ik || !go->activeInHierarchy()) continue;
        SkinnedMeshRenderer* smr = FindRigRenderer(*go);
        if (!smr) continue;

        for (const IKChain& chain : ik->chains) {
            const bool live = ik->enabled && chain.enabled && chain.weight > 0.0f;
            joints.clear();
            for (const std::string& boneName : chain.boneNames)
                if (GameObject* bone = BoneByName(ctx.scene, *smr, boneName))
                    joints.push_back(bone->transform.worldPosition);
            if (joints.empty()) continue;

            const math::Vector4 color = live ? kChainColor : kDisabledColor;
            renderer::DebugDraw::Polyline(ctx.renderer, joints, false, color);
            for (const math::Vector3& joint : joints)
                renderer::DebugDraw::Sphere(ctx.renderer, joint, kMarker, color);

            if (GameObject* target = ctx.scene.GetGameObject(chain.targetEntity)) {
                const math::Vector3 goal = target->transform.worldPosition + chain.targetOffset;
                renderer::DebugDraw::Box(ctx.renderer, goal, { kMarker * 1.5f, kMarker * 1.5f, kMarker * 1.5f },
                                         kTargetColor);
                renderer::DebugDraw::LineDashed(ctx.renderer, joints.back(), goal, kTargetColor, 0.05f, 16);
            }
            if (GameObject* pole = ctx.scene.GetGameObject(chain.poleEntity)) {
                const math::Vector3 polePosition = pole->transform.worldPosition;
                renderer::DebugDraw::Sphere(ctx.renderer, polePosition, kMarker * 1.5f, kPoleColor);
                if (joints.size() >= 2)
                    renderer::DebugDraw::LineDashed(ctx.renderer, joints[joints.size() / 2], polePosition,
                                                    kPoleColor, 0.05f, 16);
            }
        }
    }
    renderer::DebugDraw::Flush();
}

/// @name SpringBone

std::string_view SpringBoneDebugPass::Name() const { return "SpringBoneDebug"; }

bool SpringBoneDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showSpringBones;
}

void SpringBoneDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kBoneColor     = { 0.95f, 0.55f, 1.00f, 1.0f };
    constexpr math::Vector4 kColliderColor = { 0.40f, 1.00f, 0.60f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());
    for (EntityID id : ctx.scene.GetEntities<SpringBoneComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* spring = ctx.scene.GetComponent<SpringBoneComponent>(id);
        if (!go || !spring || !spring->enabled || !go->activeInHierarchy()) continue;
        SkinnedMeshRenderer* smr = FindRigRenderer(*go);
        if (!smr) continue;

        /// @note nodes は SpringBoneSystem が初回に組む。未構築 (編集中で一度も回っていない) なら骨だけ出ない。
        for (const SpringBoneChain& chain : spring->chains) {
            if (!chain.enabled) continue;
            for (const SpringBoneNodeState& node : chain.nodes) {
                GameObject* bone = BoneObject(ctx.scene, *smr, node.nodeIndex);
                if (!bone) continue;
                renderer::DebugDraw::Line(ctx.renderer, bone->transform.worldPosition, node.currentTail, kBoneColor);
                renderer::DebugDraw::Sphere(ctx.renderer, node.currentTail, (std::max)(chain.radius, 0.005f),
                                            kBoneColor);
            }
        }

        /// @note 位置の解き方は SpringBoneSystem の ResolvedCollider と同じ (骨の TRS、半径は最大スケール)。
        for (const SpringBoneCollider& collider : spring->colliders) {
            if (!collider.enabled || collider.radius <= 0.0f) continue;
            GameObject* base = collider.boneName.empty() ? go : BoneByName(ctx.scene, *smr, collider.boneName);
            if (!base) continue;
            const Transform& tf = base->transform;
            const float scale = (std::max)({ std::abs(tf.worldScale.x), std::abs(tf.worldScale.y),
                                             std::abs(tf.worldScale.z) });
            const float radius = collider.radius * scale;
            const math::Vector3 start = tf.worldPosition + ScaledRotate(tf, collider.offset);
            if (collider.shape != SpringBoneColliderShape::Capsule) {
                renderer::DebugDraw::Sphere(ctx.renderer, start, radius, kColliderColor);
                continue;
            }
            const math::Vector3 end = tf.worldPosition + ScaledRotate(tf, collider.tailOffset);
            renderer::DebugDraw::Sphere(ctx.renderer, start, radius, kColliderColor);
            renderer::DebugDraw::Sphere(ctx.renderer, end, radius, kColliderColor);
            renderer::DebugDraw::Line(ctx.renderer, start, end, kColliderColor);
        }
    }
    renderer::DebugDraw::Flush();
}

/// @name Socket / TransformConstraint

std::string_view AttachmentDebugPass::Name() const { return "AttachmentDebug"; }

bool AttachmentDebugPass::IsEnabled(const RenderPassContext& ctx) const
{
    return ctx.settings.showAttachments;
}

void AttachmentDebugPass::Execute(PassResources&, RenderPassContext& ctx)
{
    constexpr math::Vector4 kSocketColor     = { 0.35f, 1.00f, 0.85f, 1.0f };
    constexpr math::Vector4 kConstraintColor = { 1.00f, 0.65f, 0.25f, 1.0f };
    constexpr math::Vector4 kMissingColor    = { 1.00f, 0.25f, 0.20f, 1.0f };

    renderer::DebugDraw::BeginFrame(ctx.renderer, ctx.resources, ctx.camera.GetViewProjection());

    for (EntityID id : ctx.scene.GetEntities<SocketAttachmentComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* socket = ctx.scene.GetComponent<SocketAttachmentComponent>(id);
        if (!go || !socket || !socket->enabled || !go->activeInHierarchy()) continue;
        const math::Vector3 self = go->transform.worldPosition;

        /// @note 目標が空なら親を起点にする (ソケットは祖先側のリグに生えていることが多い)。
        GameObject* base = socket->target.IsValid() ? socket->target.Resolve(ctx.scene) : go->GetParent();
        GameObject* anchor = base;
        if (base && !socket->socketName.empty())
            anchor = FindDescendantByName(*base, socket->socketName);
        if (!anchor) {
            renderer::DebugDraw::Sphere(ctx.renderer, self, kMarker * 2.0f, kMissingColor);
            continue;
        }
        const math::Vector3 socketPosition = anchor->transform.worldPosition;
        renderer::DebugDraw::LineDashed(ctx.renderer, socketPosition, self, kSocketColor, 0.05f, 16);
        renderer::DebugDraw::Box(ctx.renderer, socketPosition, { kMarker, kMarker, kMarker },
                                 anchor->transform.worldRotation, kSocketColor);
    }

    for (EntityID id : ctx.scene.GetEntities<TransformConstraintComponent>()) {
        GameObject* go = ctx.scene.GetGameObject(id);
        auto* constraint = ctx.scene.GetComponent<TransformConstraintComponent>(id);
        if (!go || !constraint || !constraint->enabled || !go->activeInHierarchy()) continue;
        const math::Vector3 self = go->transform.worldPosition;
        GameObject* target = constraint->target.Resolve(ctx.scene);
        if (!target) {
            renderer::DebugDraw::Sphere(ctx.renderer, self, kMarker * 2.0f, kMissingColor);
            continue;
        }
        const math::Vector3 targetPosition = target->transform.worldPosition;
        const float weight = std::clamp(constraint->weight, 0.0f, 1.0f);
        const math::Vector4 color = { kConstraintColor.x, kConstraintColor.y, kConstraintColor.z,
                                      0.35f + 0.65f * weight };
        renderer::DebugDraw::Arrow(ctx.renderer, self, targetPosition, 0.12f, 0.04f, color);
    }

    renderer::DebugDraw::Flush();
}

} // namespace fbzz::scene
