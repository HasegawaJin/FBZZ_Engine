/// @file    TargetPointReferenceOperators.cpp
/// @brief   consumer参照と配置点作成をauthoring差分だけのChecked Undoへまとめる。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Editor/Tools/TargetPointReferenceEditing.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Scene/Components/CameraRigComponents.hpp>
#include <Engine/Scene/Components/ConstraintComponents.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/MotionWarpComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/VFXBeamComponent.hpp>
#include <Engine/Scene/Components/VFXLineComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/IKTargetReferences.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/Uuid.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

enum class ReferenceKind { CAMERA, CONSTRAINT, BEAM, VFX_LINE, NAV_AGENT, IK, MOTION_WARP, LOD };

struct SlotKey {
    ReferenceKind kind = ReferenceKind::CAMERA;
    std::string role = "target";
    int chainIndex = 0;
};

bool ParseKey(const OpArgs& args, SlotKey& key)
{
    const auto kind = args.GetString("kind");
    if (kind == "camera_follow") key.kind = ReferenceKind::CAMERA;
    else if (kind == "constraint") key.kind = ReferenceKind::CONSTRAINT;
    else if (kind == "vfx_beam") key.kind = ReferenceKind::BEAM;
    else if (kind == "vfx_line") key.kind = ReferenceKind::VFX_LINE;
    else if (kind == "nav_agent") key.kind = ReferenceKind::NAV_AGENT;
    else if (kind == "ik") key.kind = ReferenceKind::IK;
    else if (kind == "motion_warp") key.kind = ReferenceKind::MOTION_WARP;
    else if (kind == "lod_center") key.kind = ReferenceKind::LOD;
    else return false;
    key.role = args.GetString("role", "target");
    key.chainIndex = args.GetInt("chainIndex", 0);
    if (key.kind == ReferenceKind::BEAM || key.kind == ReferenceKind::VFX_LINE)
        return key.role == "from" || key.role == "to";
    return key.role == "target" || (key.kind == ReferenceKind::IK && key.role == "pole");
}

template<typename T>
uint64_t Lifetime(scene::Scene& scene, scene::GameObject& owner)
{
    return scene.GetComponentLifetime<T>(owner.GetID());
}

struct Slot {
    scene::EntityRef* entity = nullptr;
    scene::ObjectReference* object = nullptr;
    scene::IKChain* chain = nullptr;
    uint64_t lifetime = 0;
    int mode = 0;
};

Slot FindSlot(scene::Scene& scene, scene::GameObject& owner, const SlotKey& key)
{
    Slot slot;
    switch (key.kind) {
    case ReferenceKind::CAMERA:
        if (auto* row = owner.GetComponent<scene::CameraFollowComponent>()) {
            slot.entity = &row->target; slot.lifetime = Lifetime<scene::CameraFollowComponent>(scene, owner);
        }
        break;
    case ReferenceKind::CONSTRAINT:
        if (auto* row = owner.GetComponent<scene::TransformConstraintComponent>()) {
            slot.entity = &row->target; slot.mode = static_cast<int>(row->mode);
            slot.lifetime = Lifetime<scene::TransformConstraintComponent>(scene, owner);
        }
        break;
    case ReferenceKind::BEAM:
        if (auto* row = owner.GetComponent<scene::VFXBeamComponent>()) {
            slot.entity = key.role == "from" ? &row->fromEntity : &row->toEntity;
            slot.lifetime = Lifetime<scene::VFXBeamComponent>(scene, owner);
        }
        break;
    case ReferenceKind::VFX_LINE:
        if (auto* row = owner.GetComponent<scene::VFXLineComponent>()) {
            slot.entity = key.role == "from" ? &row->fromEntity : &row->toEntity;
            slot.lifetime = Lifetime<scene::VFXLineComponent>(scene, owner);
        }
        break;
    case ReferenceKind::NAV_AGENT:
        if (auto* row = owner.GetComponent<scene::NavMeshAgentComponent>()) {
            slot.entity = &row->target; slot.lifetime = Lifetime<scene::NavMeshAgentComponent>(scene, owner);
        }
        break;
    case ReferenceKind::IK:
        if (auto* row = owner.GetComponent<scene::IKSolverComponent>(); row && key.chainIndex >= 0
            && static_cast<size_t>(key.chainIndex) < row->chains.size()) {
            slot.chain = &row->chains[static_cast<size_t>(key.chainIndex)];
            slot.object = key.role == "pole" ? &slot.chain->poleReference : &slot.chain->targetReference;
            slot.lifetime = Lifetime<scene::IKSolverComponent>(scene, owner);
        }
        break;
    case ReferenceKind::MOTION_WARP:
        if (auto* row = owner.GetComponent<scene::MotionWarpComponent>()) {
            slot.object = &row->targetReference; slot.mode = static_cast<int>(row->targetPointMode);
            slot.lifetime = Lifetime<scene::MotionWarpComponent>(scene, owner);
        }
        break;
    case ReferenceKind::LOD:
        if (auto* row = owner.GetComponent<scene::LODGroupComponent>()) {
            slot.object = &row->centerTarget; slot.mode = static_cast<int>(row->centerSourceMode);
            slot.lifetime = Lifetime<scene::LODGroupComponent>(scene, owner);
        }
        break;
    }
    return slot;
}

std::string Guid(scene::Scene& scene, const Slot& slot)
{
    if (slot.object) return slot.object->guid;
    if (!slot.entity) return {};
    if (!slot.entity->guid.empty()) return slot.entity->guid;
    auto* target = slot.entity->Resolve(scene);
    return target ? target->instanceId : std::string{};
}

void WriteSlot(scene::Scene& scene, scene::GameObject& owner, const SlotKey& key,
    const std::string& guid, int mode, bool writeMode)
{
    Slot slot = FindSlot(scene, owner, key);
    if (slot.entity) *slot.entity = {scene::EntityID::INVALID, guid, 0};
    if (slot.object) *slot.object = {guid, scene::EntityID::INVALID, 0};
    if (slot.chain) {
        if (key.role == "pole") {
            slot.chain->poleReferenceMigrationPending = false;
            slot.chain->poleGuid = guid; slot.chain->poleEntity = scene::EntityID::INVALID;
        } else {
            slot.chain->targetReferenceMigrationPending = false;
            slot.chain->targetGuid = guid; slot.chain->targetEntity = scene::EntityID::INVALID;
        }
        scene::ResolveIKTargetReferences(scene, *owner.GetComponent<scene::IKSolverComponent>());
    }
    if (!writeMode) return;
    if (key.kind == ReferenceKind::CONSTRAINT)
        owner.GetComponent<scene::TransformConstraintComponent>()->mode = static_cast<scene::TransformConstraintMode>(mode);
    if (key.kind == ReferenceKind::MOTION_WARP)
        owner.GetComponent<scene::MotionWarpComponent>()->targetPointMode = static_cast<scene::MotionWarpPointMode>(mode);
    if (key.kind == ReferenceKind::LOD)
        owner.GetComponent<scene::LODGroupComponent>()->centerSourceMode = static_cast<scene::ControlPointSourceMode>(mode);
}

bool Finite(math::Vector3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool SameFloat(float lhs, float rhs) { return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs); }
bool SameVector(math::Vector3 lhs, math::Vector3 rhs)
{
    return SameFloat(lhs.x, rhs.x) && SameFloat(lhs.y, rhs.y) && SameFloat(lhs.z, rhs.z);
}

bool SameQuaternion(math::Quaternion lhs, math::Quaternion rhs)
{
    return SameFloat(lhs.x, rhs.x) && SameFloat(lhs.y, rhs.y) && SameFloat(lhs.z, rhs.z) && SameFloat(lhs.w, rhs.w);
}

bool SameColor(math::Vector4 lhs, math::Vector4 rhs)
{
    return SameFloat(lhs.x, rhs.x) && SameFloat(lhs.y, rhs.y) && SameFloat(lhs.z, rhs.z) && SameFloat(lhs.w, rhs.w);
}

bool SameConstraint(const scene::TransformConstraintComponent& lhs,
    const scene::TransformConstraintComponent& rhs)
{
    return lhs.enabled == rhs.enabled && lhs.target.guid == rhs.target.guid && lhs.mode == rhs.mode
        && SameFloat(lhs.weight, rhs.weight) && SameVector(lhs.positionOffset, rhs.positionOffset)
        && SameVector(lhs.rotationOffsetDegrees, rhs.rotationOffsetDegrees)
        && SameVector(lhs.scaleMultiplier, rhs.scaleMultiplier) && SameVector(lhs.aimAxis, rhs.aimAxis)
        && SameVector(lhs.upAxis, rhs.upAxis) && lhs.maintainOffset == rhs.maintainOffset;
}

struct ReferencePlan {
    struct CreationPose {
        scene::EntityID entity = scene::EntityID::INVALID;
        std::string guid, parent;
        scene::Transform pose;
    };
    EditorContext* context = nullptr;
    uint64_t generation = 0, lifetime = 0;
    scene::EntityID owner = scene::EntityID::INVALID;
    std::string ownerGuid;
    SlotKey key;
    std::string before, after;
    int beforeMode = 0, afterMode = 0;
    bool writeMode = false, addConstraint = false, createPoint = false;
    int anchorDomain = 0;
    uint64_t anchorLifetime = 0;
    scene::TransformConstraintComponent addedConstraint;
    std::string createdGuid, createdName = "TargetPoint";
    math::Vector3 createdPosition{};
    scene::EntityID createdEntity = scene::EntityID::INVALID;
    uint64_t createdMarkerLifetime = 0;
    std::string createdParentGuid;
    scene::EntityID createdParent = scene::EntityID::INVALID;
    std::vector<CreationPose> creationPoses;
    const scene::IKChain* chainAddress = nullptr;
    size_t chainCount = 0;
    scene::IKSolverType chainType = scene::IKSolverType::TwoBone;
    int chainOrder = 0;
    std::string targetLabel, poleLabel;
    std::vector<std::string> bones;
};

bool DrivesOwnerPose(const ReferencePlan& plan)
{
    return plan.key.kind == ReferenceKind::CAMERA || plan.key.kind == ReferenceKind::CONSTRAINT
        || plan.key.kind == ReferenceKind::NAV_AGENT
        || (plan.key.kind == ReferenceKind::MOTION_WARP
            && plan.afterMode == static_cast<int>(scene::MotionWarpPointMode::LIVE));
}

bool DescendantOrSelf(const scene::GameObject& target, const scene::GameObject& owner)
{
    for (const auto* object = &target; object; object = object->GetParent()) if (object == &owner) return true;
    return false;
}

bool StructuralCreationAllowed(const EditorContext& ctx, const scene::GameObject& owner)
{
    for (const auto* object = &owner; object; object = object->GetParent())
        if (object->runtimeGenerated || !object->prefabAssetPath.empty() || !object->prefabSourceId.empty()
            || ctx.IsLocked(object->GetID())) return false;
    return true;
}

bool PrepareCreation(ReferencePlan& plan)
{
    auto& scene = *plan.context->activeScene;
    auto* owner = scene.GetGameObject(plan.owner);
    auto* parent = DrivesOwnerPose(plan) ? owner->GetParent() : owner;
    plan.createdParent = parent ? parent->GetID() : scene::EntityID::INVALID;
    plan.createdParentGuid = parent ? parent->instanceId : std::string{};
    if (!DrivesOwnerPose(plan)) return true;
    for (auto* object = owner; object; object = object->GetParent()) {
        const auto& q = object->transform.rotation;
        const double norm = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y
            + static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w;
        if (!Finite(object->transform.position) || !Finite(object->transform.scale)
            || !std::isfinite(norm) || std::abs(norm - 1.0) > 2.0e-3
            || object->transform.scale.x == 0.0f || object->transform.scale.y == 0.0f || object->transform.scale.z == 0.0f) return false;
        plan.creationPoses.push_back({object->GetID(), object->instanceId,
            object->GetParent() ? object->GetParent()->instanceId : std::string{}, object->transform});
    }
    scene::FlushWorldTransforms(scene);
    const auto scale = owner->transform.worldScale;
    const math::Vector3 offset = plan.createdPosition;
    const math::Vector3 delta = owner->transform.worldRotation * math::Vector3{offset.x * scale.x, offset.y * scale.y, offset.z * scale.z};
    math::Vector3 parentDelta = delta;
    if (parent) {
        const auto parentScale = parent->transform.worldScale;
        if (!Finite(parentScale) || parentScale.x == 0.0f || parentScale.y == 0.0f || parentScale.z == 0.0f) return false;
        const auto rotated = parent->transform.worldRotation.Inverse() * delta;
        parentDelta = {rotated.x / parentScale.x, rotated.y / parentScale.y, rotated.z / parentScale.z};
    }
    plan.createdPosition = owner->transform.position + parentDelta;
    return Finite(delta) && Finite(plan.createdPosition);
}

void CaptureChainIdentity(ReferencePlan& plan, scene::GameObject& owner, const Slot& slot)
{
    if (!slot.chain) return;
    plan.chainAddress = slot.chain; plan.chainCount = owner.GetComponent<scene::IKSolverComponent>()->chains.size();
    plan.chainType = slot.chain->type; plan.chainOrder = slot.chain->order;
    plan.targetLabel = slot.chain->targetName; plan.poleLabel = slot.chain->poleName; plan.bones = slot.chain->boneNames;
}

bool ChainMatches(const ReferencePlan& plan, scene::GameObject& owner, const Slot& slot)
{
    if (!plan.chainAddress) return true;
    const auto* solver = owner.GetComponent<scene::IKSolverComponent>();
    return slot.chain && solver && slot.chain == plan.chainAddress && solver->chains.size() == plan.chainCount
        && slot.chain->type == plan.chainType && slot.chain->order == plan.chainOrder
        && slot.chain->targetName == plan.targetLabel && slot.chain->poleName == plan.poleLabel
        && slot.chain->boneNames == plan.bones;
}

/// @note 全登録consumerと保存Script参照を検査し、Undo対象の一つのslotだけをコピー上で除外する。
class ReferenceProbe final : public scene::IReflector {
public:
    using scene::IReflector::Field;
    using scene::IReflector::ListField;
    std::string guid;
    scene::EntityID entity = scene::EntityID::INVALID;
    bool found = false;
    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, scene::EntityID& value) override { found |= value.IsValid() && value == entity; }
    void ObjectReferenceField(const char*, scene::ObjectReference& value) override { found |= value.guid == guid; }
    void ObjectReferenceListField(const char* name, std::vector<scene::ObjectReference>& values) override
    {
        for (auto& value : values) ObjectReferenceField(name, value);
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        for (auto& value : values) { found |= value.guid == guid; Field(name, value.id); }
    }
};

template<typename T>
void ClearCopiedSlot(T& copy, const SlotKey& key)
{
    if constexpr (std::is_same_v<T, scene::CameraFollowComponent>) {
        if (key.kind == ReferenceKind::CAMERA) copy.target = {};
    } else if constexpr (std::is_same_v<T, scene::TransformConstraintComponent>) {
        if (key.kind == ReferenceKind::CONSTRAINT) copy.target = {};
    } else if constexpr (std::is_same_v<T, scene::VFXBeamComponent>) {
        if (key.kind == ReferenceKind::BEAM) (key.role == "from" ? copy.fromEntity : copy.toEntity) = {};
    } else if constexpr (std::is_same_v<T, scene::VFXLineComponent>) {
        if (key.kind == ReferenceKind::VFX_LINE) (key.role == "from" ? copy.fromEntity : copy.toEntity) = {};
    } else if constexpr (std::is_same_v<T, scene::NavMeshAgentComponent>) {
        if (key.kind == ReferenceKind::NAV_AGENT) copy.target = {};
    } else if constexpr (std::is_same_v<T, scene::MotionWarpComponent>) {
        if (key.kind == ReferenceKind::MOTION_WARP) copy.targetReference = {};
    } else if constexpr (std::is_same_v<T, scene::LODGroupComponent>) {
        if (key.kind == ReferenceKind::LOD) copy.centerTarget = {};
    } else if constexpr (std::is_same_v<T, scene::IKSolverComponent>) {
        if (key.kind == ReferenceKind::IK && key.chainIndex >= 0 && static_cast<size_t>(key.chainIndex) < copy.chains.size()) {
            auto& chain = copy.chains[static_cast<size_t>(key.chainIndex)];
            if (key.role == "pole") scene::ClearIKPoleReference(chain); else scene::ClearIKTargetReference(chain);
        }
    }
}

bool HasOtherReferences(scene::Scene& scene, const ReferencePlan& plan)
{
    ReferenceProbe probe; probe.guid = plan.createdGuid; probe.entity = plan.createdEntity;
    for (auto& object : scene.GameObjects()) {
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (std::is_same_v<T, scene::ScriptComponent>) {
                if (auto* scripts = object.GetComponent<T>()) for (auto& entry : scripts->scripts) {
                    if (entry.script && !entry.script->IsRuntimeFaulted()) entry.script->Reflect(probe);
                    if (entry.serialized && entry.serialized->fieldsToml.find(probe.guid) != std::string::npos) probe.found = true;
                }
            } else if constexpr (Registration::hasReflect && std::is_copy_constructible_v<T>) {
                if (auto* component = object.GetComponent<T>()) {
                    T copy = *component;
                    if (object.GetID() == plan.owner) ClearCopiedSlot(copy, plan.key);
                    copy.Reflect(probe);
                    if constexpr (std::is_same_v<T, scene::IKSolverComponent>) for (auto& chain : copy.chains) {
                        probe.ObjectReferenceField("target", chain.targetReference);
                        probe.ObjectReferenceField("pole", chain.poleReference);
                        probe.Field("legacyTarget", chain.targetEntity); probe.Field("legacyPole", chain.poleEntity);
                    }
                }
            }
        });
        if (probe.found) return true;
    }
    return false;
}

bool ValidCreated(scene::Scene& scene, const ReferencePlan& plan)
{
    auto* point = scene.GetGameObject(plan.createdEntity);
    const auto* parent = point ? point->GetParent() : nullptr;
    if (!point || point->instanceId != plan.createdGuid
        || (parent ? parent->GetID() : scene::EntityID::INVALID) != plan.createdParent
        || (parent ? parent->instanceId : std::string{}) != plan.createdParentGuid || point->GetChildCount() != 0
        || point->name != plan.createdName || !point->activeSelf() || point->runtimeGenerated
        || !point->prefabAssetPath.empty() || !point->prefabSourceId.empty() || point->tag != "Untagged" || point->layer != 0
        || !SameVector(point->transform.position, plan.createdPosition)
        || !SameQuaternion(point->transform.rotation, math::Quaternion::Identity()) || !SameVector(point->transform.scale, math::Vector3::ONE)
        || scene.GetComponentLifetime<scene::TargetPointComponent>(point->GetID()) != plan.createdMarkerLifetime) return false;
    const auto* marker = point->GetComponent<scene::TargetPointComponent>();
    const scene::TargetPointComponent defaults;
    if (!marker || marker->visible != defaults.visible || !SameColor(marker->color, defaults.color) || !SameFloat(marker->screenSize, defaults.screenSize)) return false;
    bool extra = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (!std::is_same_v<T, scene::TargetPointComponent>) extra |= point->GetComponent<T>() != nullptr;
    });
    return !extra && !HasOtherReferences(scene, plan);
}

bool ApplyPlan(const std::shared_ptr<ReferencePlan>& plan, bool apply, std::string& reason)
{
    auto& ctx = *plan->context;
    if (!IsControlPointAuthoringAllowed(ctx) || ctx.activeScene->GetRenderSceneGeneration() != plan->generation) {
        reason = "Sceneまたは編集状態が変わりました"; return false;
    }
    auto& scene = *ctx.activeScene;
    auto* owner = scene.GetGameObject(plan->owner);
    if (!owner || owner->runtimeGenerated || owner->instanceId != plan->ownerGuid || ctx.IsLocked(plan->owner)) {
        reason = "参照の所有者が変更されています"; return false;
    }
    if ((plan->anchorDomain == 1 && Lifetime<scene::LightComponent>(scene, *owner) != plan->anchorLifetime)
        || (plan->anchorDomain == 2 && Lifetime<scene::FlowField>(scene, *owner) != plan->anchorLifetime)) {
        reason = "anchorのconsumer寿命が変更されています"; return false;
    }
    Slot slot = FindSlot(scene, *owner, plan->key);
    const bool adding = apply && plan->addConstraint;
    if (adding) {
        if (slot.lifetime != 0 || !StructuralCreationAllowed(ctx, *owner)) {
            reason = "Constraintの構造が変更されています"; return false;
        }
    } else if (slot.lifetime != plan->lifetime || !ChainMatches(*plan, *owner, slot)
        || Guid(scene, slot) != (apply ? plan->before : plan->after)
        || (plan->writeMode && slot.mode != (apply ? plan->beforeMode : plan->afterMode))) {
        reason = "Component寿命または接続が変更されています"; return false;
    }
    if (plan->createPoint && apply) {
        auto* parent = scene.GetGameObject(plan->createdParent);
        if (!scene.CanCreateGameObjects() || !StructuralCreationAllowed(ctx, *owner)
            || (plan->createdParent.IsValid() && (!parent || parent->instanceId != plan->createdParentGuid || ctx.IsLocked(parent->GetID())))
            || scene.FindByGuid(plan->createdGuid)) { reason = "点を安全に作成できません"; return false; }
        for (const auto& stamp : plan->creationPoses) {
            const auto* object = scene.GetGameObject(stamp.entity);
            if (!object || object->instanceId != stamp.guid
                || (object->GetParent() ? object->GetParent()->instanceId : std::string{}) != stamp.parent
                || !SameVector(object->transform.position, stamp.pose.position)
                || !SameVector(object->transform.scale, stamp.pose.scale) || !SameQuaternion(object->transform.rotation, stamp.pose.rotation)) {
                reason = "作成位置の基準Transformが変更されています"; return false;
            }
        }
    }
    if (plan->createPoint && !apply && (ctx.IsLocked(plan->createdEntity) || !ValidCreated(scene, *plan))) {
        reason = "作成した点が編集されたか、他の参照へ共有されています"; return false;
    }
    if (plan->addConstraint && !apply
        && !SameConstraint(*owner->GetComponent<scene::TransformConstraintComponent>(), plan->addedConstraint)) {
        reason = "追加したConstraintが編集されています"; return false;
    }
    if (plan->createPoint && apply) {
        auto* point = scene.TryCreateGameObject(plan->createdName);
        if (!point) { reason = "Sceneの作成可能数を越えました"; return false; }
        point->instanceId = plan->createdGuid;
        if (auto* parent = scene.GetGameObject(plan->createdParent)) point->SetParent(*parent);
        point->transform.position = plan->createdPosition;
        point->AddComponent<scene::TargetPointComponent>();
        plan->createdEntity = point->GetID();
        plan->createdMarkerLifetime = scene.GetComponentLifetime<scene::TargetPointComponent>(point->GetID());
    }
    if (adding) {
        owner->AddComponent<scene::TransformConstraintComponent>(plan->addedConstraint);
        plan->lifetime = Lifetime<scene::TransformConstraintComponent>(scene, *owner);
    } else {
        WriteSlot(scene, *owner, plan->key, apply ? plan->after : plan->before,
            apply ? plan->afterMode : plan->beforeMode, plan->writeMode);
    }
    if (plan->addConstraint && !apply) {
        owner->RemoveComponent<scene::TransformConstraintComponent>(); plan->lifetime = 0;
    }
    if (plan->createPoint && !apply) scene.DestroyGameObject(plan->createdEntity);
    if (plan->createPoint) scene::FlushWorldTransforms(scene);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    reason.clear(); return true;
}

OpResult Commit(const std::shared_ptr<ReferencePlan>& plan)
{
    OpResult result;
    if (!plan->createPoint && !plan->addConstraint && plan->before == plan->after
        && (!plan->writeMode || plan->beforeMode == plan->afterMode)) { result.noChange = true; return result; }
    auto command = std::make_unique<CheckedLambdaCommand>("Connect TargetPoint",
        [plan](std::string& reason) { return ApplyPlan(plan, true, reason); },
        [plan](std::string& reason) { return ApplyPlan(plan, false, reason); });
    std::string reason;
    if (!command->TryExecute(reason)) return OpResult::Err("CONFLICT", std::move(reason));
    result.command = std::move(command);
    result.data.Set("target", plan->after);
    return result;
}

scene::GameObject* Owner(EditorContext& ctx, const OpArgs& args)
{
    return args.GetString("node").empty() ? ctx.GetSelectedGO() : ctx.activeScene->FindByGuid(args.GetString("node"));
}

std::shared_ptr<ReferencePlan> MakePlan(EditorContext& ctx, const OpArgs& args)
{
    if (!IsControlPointAuthoringAllowed(ctx)) return {};
    auto* owner = Owner(ctx, args);
    SlotKey key;
    if (!owner || owner->runtimeGenerated || ctx.IsLocked(owner->GetID()) || !ParseKey(args, key)) return {};
    if (key.kind == ReferenceKind::IK)
        if (auto* solver = owner->GetComponent<scene::IKSolverComponent>()) scene::InitializeIKTargetReferences(*ctx.activeScene, *solver);
    Slot slot = FindSlot(*ctx.activeScene, *owner, key);
    if (slot.lifetime == 0) return {};
    auto plan = std::make_shared<ReferencePlan>();
    plan->context = &ctx; plan->generation = ctx.activeScene->GetRenderSceneGeneration();
    plan->owner = owner->GetID(); plan->ownerGuid = owner->instanceId;
    plan->key = key; plan->lifetime = slot.lifetime;
    plan->before = Guid(*ctx.activeScene, slot); plan->after = plan->before;
    plan->beforeMode = slot.mode; plan->afterMode = slot.mode;
    CaptureChainIdentity(*plan, *owner, slot);
    return plan;
}

OpResult Bind(OpContext& context, const OpArgs& args, bool create)
{
    auto plan = MakePlan(context.ctx, args);
    if (!plan) return OpResult::Err("NO_SLOT", "参照を持つComponentまたはIKチェーンが必要です");
    auto& scene = *context.ctx.activeScene;
    if (create) {
        plan->createPoint = true; plan->createdGuid = util::GenerateUUID();
        plan->createdPosition = args.GetVec3("localPosition");
        plan->createdName = args.GetString("name", "TargetPoint");
        if (!Finite(plan->createdPosition) || plan->createdName.empty()) return OpResult::Err("BAD_POINT", "有限なlocalPositionと名前が必要です");
        plan->after = plan->createdGuid;
    } else {
        plan->after = args.GetString("target");
        auto* target = plan->after.empty() ? nullptr : scene.FindByGuid(plan->after);
        if (!plan->after.empty() && ((!target && plan->after != plan->before) || (target && target->runtimeGenerated))) return OpResult::Err("NO_TARGET", "同じSceneの保存対象GOが必要です");
        if (target && (plan->key.kind == ReferenceKind::LOD || plan->key.kind == ReferenceKind::MOTION_WARP)
            && !target->GetComponent<scene::TargetPointComponent>()) return OpResult::Err("NO_MARKER", "この用途ではTargetPointが必要です");
    }
    if (plan->key.kind == ReferenceKind::LOD) {
        plan->writeMode = true;
        plan->afterMode = static_cast<int>(plan->after.empty() ? scene::ControlPointSourceMode::INLINE : scene::ControlPointSourceMode::POINT_OBJECTS);
    }
    if (args.Has("mode")) {
        if (plan->key.kind != ReferenceKind::MOTION_WARP) return OpResult::Err("BAD_MODE", "modeはMotionWarpのCAPTURED/LIVE指定です");
        const auto mode = args.GetString("mode");
        if (mode != "CAPTURED" && mode != "LIVE") return OpResult::Err("BAD_MODE", "CAPTUREDまたはLIVEを指定してください");
        plan->writeMode = true; plan->afterMode = static_cast<int>(mode == "LIVE" ? scene::MotionWarpPointMode::LIVE : scene::MotionWarpPointMode::CAPTURED);
    }
    if (create) {
        if (!PrepareCreation(*plan)) return OpResult::Err("BAD_POSE", "点を作成する基準Transformが無効です");
    } else if (auto* target = scene.FindByGuid(plan->after); target && DrivesOwnerPose(*plan)
        && DescendantOrSelf(*target, *scene.GetGameObject(plan->owner))) {
        return OpResult::Err("FEEDBACK_TARGET", "自身または子孫を姿勢の追従先にできません");
    }
    return Commit(plan);
}

OpResult ConfigureAnchor(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    auto* owner = Owner(ctx, args);
    const auto kind = args.GetString("kind");
    if (!owner || owner->runtimeGenerated || ctx.IsLocked(owner->GetID()) || (kind == "light" && !owner->GetComponent<scene::LightComponent>())
        || (kind == "flow_field" && !owner->GetComponent<scene::FlowField>())
        || (kind != "light" && kind != "flow_field")) return OpResult::Err("NO_OWNER", "LightまたはFlowFieldが必要です");
    const bool create = args.GetString("target").empty();
    auto* target = create ? nullptr : ctx.activeScene->FindByGuid(args.GetString("target"));
    if (!create && (!target || target->runtimeGenerated || !target->GetComponent<scene::TargetPointComponent>())) return OpResult::Err("NO_MARKER", "同じSceneのTargetPointが必要です");
    const auto mode = args.GetString("mode", "Position");
    if (mode != "Position" && mode != "LookAt" && mode != "Parent") return OpResult::Err("BAD_MODE", "Position、LookAt、Parentを指定してください");
    auto slotArgs = args; slotArgs.Set("kind", std::string("constraint")); slotArgs.Set("role", std::string("target"));
    auto plan = MakePlan(ctx, slotArgs);
    if (!plan) {
        plan = std::make_shared<ReferencePlan>(); plan->context = &ctx;
        plan->generation = ctx.activeScene->GetRenderSceneGeneration(); plan->owner = owner->GetID(); plan->ownerGuid = owner->instanceId;
        plan->key.kind = ReferenceKind::CONSTRAINT; plan->addConstraint = true;
    }
    plan->anchorDomain = kind == "light" ? 1 : 2;
    plan->anchorLifetime = kind == "light" ? Lifetime<scene::LightComponent>(*ctx.activeScene, *owner)
        : Lifetime<scene::FlowField>(*ctx.activeScene, *owner);
    if (create) {
        plan->createPoint = true; plan->createdGuid = util::GenerateUUID();
        plan->createdName = args.GetString("name", "TargetPoint"); plan->createdPosition = args.GetVec3("localPosition");
        if (!Finite(plan->createdPosition) || plan->createdName.empty()) return OpResult::Err("BAD_POINT", "有限なlocalPositionと名前が必要です");
    }
    plan->after = create ? plan->createdGuid : target->instanceId; plan->writeMode = true;
    plan->afterMode = static_cast<int>(mode == "LookAt" ? scene::TransformConstraintMode::LookAt
        : mode == "Parent" ? scene::TransformConstraintMode::Parent : scene::TransformConstraintMode::Position);
    plan->addedConstraint.target = {scene::EntityID::INVALID, plan->after, 0};
    plan->addedConstraint.mode = static_cast<scene::TransformConstraintMode>(plan->afterMode);
    if (create) {
        if (!PrepareCreation(*plan)) return OpResult::Err("BAD_POSE", "anchorを作成する基準Transformが無効です");
    } else if (DescendantOrSelf(*target, *owner)) return OpResult::Err("FEEDBACK_TARGET", "自身または子孫をanchorにできません");
    return Commit(plan);
}

std::vector<OpParam> SlotParams()
{
    std::vector<OpParam> params{
        {"node", OpParamType::NodeId, "参照の所有者。省略時は選択GO", false},
        {"kind", OpParamType::String, "consumerの種類"},
        {"role", OpParamType::String, "target/from/to/pole", false, std::string("target")},
        {"chainIndex", OpParamType::Int, "IKチェーンのindex", false, 0}};
    params[1].enumValues = {"camera_follow", "constraint", "vfx_beam", "vfx_line", "nav_agent", "ik", "motion_warp", "lod_center"};
    params[2].enumValues = {"target", "from", "to", "pole"};
    return params;
}

void Register(OperatorRegistry& registry, const char* id, const char* label, OpKind kind,
    std::vector<OpParam> params, OpExec execute)
{
    EditorOperator op; op.id = id; op.label = label; op.category = "TargetPoint"; op.desc = label;
    op.kind = kind; op.params = std::move(params); op.exec = std::move(execute);
    if (kind == OpKind::Mutation) op.undoLabel = label;
    op.poll = [](const OpContext& ctx, const OpArgs&) { return IsControlPointAuthoringAllowed(ctx.ctx); };
    registry.Register(std::move(op));
}

} /// @note namespace

void RegisterTargetPointReferenceOperators(OperatorRegistry& registry)
{
    auto bind = SlotParams(); bind.push_back({"target", OpParamType::NodeId, "接続先GUID。空文字で明示解除"});
    bind.push_back({"mode", OpParamType::String, "MotionWarpだけCAPTURED/LIVEを指定", false});
    Register(registry, "target_point.bind_reference", "配置点の参照を接続", OpKind::Mutation, std::move(bind),
        [](OpContext& ctx, const OpArgs& args) { return Bind(ctx, args, false); });
    auto create = SlotParams(); create.push_back({"localPosition", OpParamType::Vec3, "owner原点のlocal位置", false, math::Vector3::ZERO});
    create.push_back({"name", OpParamType::String, "作成する点の名前", false, std::string("TargetPoint")});
    Register(registry, "target_point.create_and_bind", "配置点を作成して接続", OpKind::Mutation, std::move(create),
        [](OpContext& ctx, const OpArgs& args) { return Bind(ctx, args, true); });
    Register(registry, "target_point.select_reference", "参照先を選択", OpKind::Action, SlotParams(),
        [](OpContext& context, const OpArgs& args) {
            auto plan = MakePlan(context.ctx, args);
            auto* target = plan ? context.ctx.activeScene->FindByGuid(plan->before) : nullptr;
            if (!target) return OpResult::Err("MISSING_TARGET", "参照先が見つかりません");
            SelectEntity(context.ctx, target->GetID()); return OpResult::Ok();
        });
    std::vector<OpParam> anchor{{"node", OpParamType::NodeId, "Light/FlowField所有者。省略時は選択GO", false},
        {"kind", OpParamType::String, "light/flow_field"}, {"target", OpParamType::NodeId, "TargetPointのGUID。空なら作成"},
        {"mode", OpParamType::String, "Position/LookAt/Parent", false, std::string("Position")}};
    anchor[1].enumValues = {"light", "flow_field"}; anchor[3].enumValues = {"Position", "LookAt", "Parent"};
    anchor.push_back({"localPosition", OpParamType::Vec3, "新規点のowner-local位置", false, math::Vector3::ZERO});
    anchor.push_back({"name", OpParamType::String, "新規点の名前", false, std::string("TargetPoint")});
    Register(registry, "target_point.configure_anchor", "Light/FlowFieldのanchorを接続", OpKind::Mutation,
        std::move(anchor), ConfigureAnchor);
}

} /// @note namespace fbzz::editor
