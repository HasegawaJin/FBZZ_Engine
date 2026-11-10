/// @file    AdditionalControlPointOperators.cpp
/// @brief   Patrol、Line、Off-Mesh Linkの制御点MutationをChecked Undoへまとめる。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Tools/AdditionalControlPointEditing.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/ControlPointModes.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/NavigationControlPoints.hpp>
#include <Engine/Scene/PresentationControlPoints.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/Uuid.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

enum class Domain : std::uint8_t { PATROL, LINE, NAV_LINK };

struct AuthoringState {
    scene::ControlPointSourceMode source = scene::ControlPointSourceMode::INLINE;
    scene::ControlPointRuntimeMode runtime = scene::ControlPointRuntimeMode::LIVE;
    std::vector<math::Vector3> positions;
    std::vector<float> waitTimes;
    std::vector<float> speeds;
    std::vector<scene::NavMeshPatrolWaypointBinding> patrolBindings;
    std::vector<scene::NavMeshPatrolWaypointValue> patrolBake;
    float patrolWaitTime = 0.0f;
    scene::NavMeshPatrolComponent::Mode patrolMode = scene::NavMeshPatrolComponent::Mode::LOOP;
    scene::ControlPointSourceMode patrolBakeSource = scene::ControlPointSourceMode::INLINE;
    std::uint32_t patrolBakeVersion = 0;
    std::uint64_t patrolBakeSignature = 0;
    bool patrolBakeValid = false;
    std::vector<scene::LineControlPointBinding> lineBindings;
    std::vector<scene::LineControlPointValue> lineBake;
    scene::LineSpace lineSpace = static_cast<scene::LineSpace>(0);
    scene::LineSpace lineBakeSpace = static_cast<scene::LineSpace>(0);
    bool lineLoop = false;
    scene::ControlPointSourceMode lineBakeSource = scene::ControlPointSourceMode::INLINE;
    std::uint32_t lineBakeVersion = 0;
    std::uint64_t lineBakeSignature = 0;
    bool lineBakeValid = false;
    math::Vector3 linkStart{};
    math::Vector3 linkEnd{};
    scene::ObjectReference linkStartTarget;
    scene::ObjectReference linkEndTarget;

    bool operator==(const AuthoringState& other) const;
};

bool SameFloat(float lhs, float rhs)
{
    return std::bit_cast<std::uint32_t>(lhs) == std::bit_cast<std::uint32_t>(rhs);
}

bool SameVector(const math::Vector3& lhs, const math::Vector3& rhs)
{
    return SameFloat(lhs.x, rhs.x) && SameFloat(lhs.y, rhs.y) && SameFloat(lhs.z, rhs.z);
}

bool SameReference(const scene::ObjectReference& lhs, const scene::ObjectReference& rhs)
{
    return lhs.guid == rhs.guid;
}

void ClearReferenceCache(scene::ObjectReference& reference)
{
    reference.id = scene::EntityID::INVALID;
    reference.sceneGeneration = 0;
}

template<typename T>
void ClearReferenceCaches(std::vector<T>& bindings)
{
    for (auto& binding : bindings) ClearReferenceCache(binding.target);
}

template<typename T, typename Equal>
bool SameRows(const std::vector<T>& lhs, const std::vector<T>& rhs, Equal equal)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), equal);
}

bool AuthoringState::operator==(const AuthoringState& other) const
{
    return source == other.source && runtime == other.runtime
        && SameRows(positions, other.positions, SameVector)
        && SameRows(waitTimes, other.waitTimes, SameFloat)
        && SameRows(speeds, other.speeds, SameFloat)
        && SameRows(patrolBindings, other.patrolBindings, [](const auto& lhs, const auto& rhs) {
            return lhs.id == rhs.id && SameReference(lhs.target, rhs.target)
                && SameFloat(lhs.waitTime, rhs.waitTime) && SameFloat(lhs.speed, rhs.speed);
        })
        && SameRows(patrolBake, other.patrolBake, [](const auto& lhs, const auto& rhs) {
            return lhs.id == rhs.id && SameVector(lhs.worldPosition, rhs.worldPosition)
                && SameFloat(lhs.waitTime, rhs.waitTime) && SameFloat(lhs.speed, rhs.speed);
        })
        && SameFloat(patrolWaitTime, other.patrolWaitTime) && patrolMode == other.patrolMode
        && patrolBakeSource == other.patrolBakeSource && patrolBakeVersion == other.patrolBakeVersion
        && patrolBakeSignature == other.patrolBakeSignature && patrolBakeValid == other.patrolBakeValid
        && SameRows(lineBindings, other.lineBindings, [](const auto& lhs, const auto& rhs) {
            return lhs.id == rhs.id && SameReference(lhs.target, rhs.target);
        })
        && SameRows(lineBake, other.lineBake, [](const auto& lhs, const auto& rhs) {
            return lhs.id == rhs.id && SameVector(lhs.position, rhs.position);
        })
        && lineSpace == other.lineSpace && lineBakeSpace == other.lineBakeSpace
        && lineLoop == other.lineLoop && lineBakeSource == other.lineBakeSource
        && lineBakeVersion == other.lineBakeVersion && lineBakeSignature == other.lineBakeSignature
        && lineBakeValid == other.lineBakeValid
        && SameVector(linkStart, other.linkStart) && SameVector(linkEnd, other.linkEnd)
        && SameReference(linkStartTarget, other.linkStartTarget)
        && SameReference(linkEndTarget, other.linkEndTarget);
}

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const math::Quaternion& value)
{
    const double lengthSquared = static_cast<double>(value.x) * value.x
        + static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z
        + static_cast<double>(value.w) * value.w;
    return std::isfinite(lengthSquared) && std::abs(lengthSquared - 1.0) <= 2.0e-3;
}

bool FiniteChain(const scene::GameObject* object)
{
    for (; object; object = object->GetParent()) {
        const auto& pose = object->transform;
        if (!Finite(pose.position) || !Finite(pose.scale) || !Finite(pose.rotation)
            || std::abs(pose.scale.x) <= 1.0e-6f || std::abs(pose.scale.y) <= 1.0e-6f
            || std::abs(pose.scale.z) <= 1.0e-6f) return false;
    }
    return true;
}

/// @brief Convert an owner-local point through the Engine world TRS into an absolute point.
/// @param owner Synchronized owner transform with finite nonzero scale.
/// @param local Owner-local position.
/// @param world Receives the finite absolute position on success.
/// @return False when the transform or result is not usable.
/// @see Docs/design/script-transform-contract.md
bool ToOwnerWorld(const scene::Transform& owner, const math::Vector3& local, math::Vector3& world)
{
    if (!Finite(local) || !Finite(owner.worldPosition) || !Finite(owner.worldScale)
        || !Finite(owner.worldRotation) || std::abs(owner.worldScale.x) <= 1.0e-6f
        || std::abs(owner.worldScale.y) <= 1.0e-6f || std::abs(owner.worldScale.z) <= 1.0e-6f) return false;
    const math::Vector3 scaled{local.x * owner.worldScale.x, local.y * owner.worldScale.y,
        local.z * owner.worldScale.z};
    world = owner.worldPosition + owner.worldRotation * scaled;
    return Finite(world);
}

bool NearVector(const math::Vector3& lhs, const math::Vector3& rhs)
{
    const auto near = [](float a, float b) {
        return std::abs(a - b) <= 1.0e-4f * (std::max)(1.0f, (std::max)(std::abs(a), std::abs(b)));
    };
    return Finite(lhs) && Finite(rhs) && near(lhs.x, rhs.x) && near(lhs.y, rhs.y) && near(lhs.z, rhs.z);
}

scene::GameObject* ResolveOwner(EditorContext& ctx, const OpArgs& args)
{
    if (!ctx.activeScene) return nullptr;
    const std::string guid = args.GetString("node");
    return guid.empty() ? ctx.GetSelectedGO() : ctx.activeScene->FindByGuid(guid);
}

Domain ParseDomain(std::string_view kind, bool& valid)
{
    valid = true;
    if (kind == "patrol") return Domain::PATROL;
    if (kind == "line") return Domain::LINE;
    if (kind == "nav_link") return Domain::NAV_LINK;
    valid = false;
    return Domain::LINE;
}

std::uint64_t ComponentLifetime(scene::Scene& world, Domain domain, scene::EntityID owner)
{
    switch (domain) {
    case Domain::PATROL: return world.GetComponentLifetime<scene::NavMeshPatrolComponent>(owner);
    case Domain::LINE: return world.GetComponentLifetime<scene::LineRendererComponent>(owner);
    case Domain::NAV_LINK: return world.GetComponentLifetime<scene::NavMeshOffMeshLinkComponent>(owner);
    }
    return 0;
}

bool HasDomainComponent(scene::GameObject& owner, Domain domain)
{
    switch (domain) {
    case Domain::PATROL: return owner.GetComponent<scene::NavMeshPatrolComponent>() != nullptr;
    case Domain::LINE: return owner.GetComponent<scene::LineRendererComponent>() != nullptr;
    case Domain::NAV_LINK: return owner.GetComponent<scene::NavMeshOffMeshLinkComponent>() != nullptr;
    }
    return false;
}

AuthoringState Capture(scene::GameObject& owner, Domain domain)
{
    AuthoringState state;
    if (domain == Domain::PATROL) {
        const auto& value = *owner.GetComponent<scene::NavMeshPatrolComponent>();
        state.source = value.pointSourceMode; state.runtime = value.pointRuntimeMode;
        state.positions = value.waypoints; state.waitTimes = value.waypointWaitTimes;
        state.speeds = value.waypointSpeeds; state.patrolBindings = value.waypointBindings;
        state.patrolBake = value.bakedWaypoints; state.patrolWaitTime = value.waitTime;
        state.patrolMode = value.mode; state.patrolBakeSource = value.bakedSourceMode;
        state.patrolBakeVersion = value.bakedAdapterVersion;
        state.patrolBakeSignature = value.bakedInputSignature; state.patrolBakeValid = value.bakeValid;
        ClearReferenceCaches(state.patrolBindings);
    } else if (domain == Domain::LINE) {
        const auto& value = *owner.GetComponent<scene::LineRendererComponent>();
        state.source = value.pointSourceMode; state.runtime = value.pointRuntimeMode;
        state.positions = value.points; state.lineBindings = value.pointBindings;
        state.lineBake = value.bakedPoints; state.lineSpace = value.space;
        state.lineBakeSpace = value.bakedSpace; state.lineLoop = value.loop;
        state.lineBakeSource = value.bakedSourceMode; state.lineBakeVersion = value.bakedAdapterVersion;
        state.lineBakeSignature = value.bakedInputSignature; state.lineBakeValid = value.bakeValid;
        ClearReferenceCaches(state.lineBindings);
    } else {
        const auto& value = *owner.GetComponent<scene::NavMeshOffMeshLinkComponent>();
        state.source = value.pointSourceMode; state.linkStart = value.startPoint;
        state.linkEnd = value.endPoint; state.linkStartTarget = value.startTarget;
        state.linkEndTarget = value.endTarget;
        ClearReferenceCache(state.linkStartTarget); ClearReferenceCache(state.linkEndTarget);
    }
    return state;
}

void Write(scene::GameObject& owner, Domain domain, const AuthoringState& before,
    const AuthoringState& after, bool apply)
{
    const AuthoringState& value = apply ? after : before;
    if (domain == Domain::PATROL) {
        auto& patrol = *owner.GetComponent<scene::NavMeshPatrolComponent>();
        if (before.source != after.source) patrol.pointSourceMode = value.source;
        if (before.runtime != after.runtime) patrol.pointRuntimeMode = value.runtime;
        if (!SameRows(before.positions, after.positions, SameVector)) patrol.waypoints = value.positions;
        if (!SameRows(before.waitTimes, after.waitTimes, SameFloat)) patrol.waypointWaitTimes = value.waitTimes;
        if (!SameRows(before.speeds, after.speeds, SameFloat)) patrol.waypointSpeeds = value.speeds;
        if (!SameRows(before.patrolBindings, after.patrolBindings, [](const auto& lhs, const auto& rhs) {
                return lhs.id == rhs.id && SameReference(lhs.target, rhs.target)
                    && SameFloat(lhs.waitTime, rhs.waitTime) && SameFloat(lhs.speed, rhs.speed);
            })) patrol.waypointBindings = value.patrolBindings;
        if (!SameRows(before.patrolBake, after.patrolBake, [](const auto& lhs, const auto& rhs) {
                return lhs.id == rhs.id && SameVector(lhs.worldPosition, rhs.worldPosition)
                    && SameFloat(lhs.waitTime, rhs.waitTime) && SameFloat(lhs.speed, rhs.speed);
            })) patrol.bakedWaypoints = value.patrolBake;
        if (before.patrolBakeSignature != after.patrolBakeSignature) patrol.bakedInputSignature = value.patrolBakeSignature;
        if (before.patrolBakeSource != after.patrolBakeSource) patrol.bakedSourceMode = value.patrolBakeSource;
        if (before.patrolBakeVersion != after.patrolBakeVersion) patrol.bakedAdapterVersion = value.patrolBakeVersion;
        if (before.patrolBakeValid != after.patrolBakeValid) patrol.bakeValid = value.patrolBakeValid;
    } else if (domain == Domain::LINE) {
        auto& line = *owner.GetComponent<scene::LineRendererComponent>();
        if (before.source != after.source) line.pointSourceMode = value.source;
        if (before.runtime != after.runtime) line.pointRuntimeMode = value.runtime;
        if (!SameRows(before.positions, after.positions, SameVector)) line.points = value.positions;
        if (!SameRows(before.lineBindings, after.lineBindings, [](const auto& lhs, const auto& rhs) {
                return lhs.id == rhs.id && SameReference(lhs.target, rhs.target);
            })) line.pointBindings = value.lineBindings;
        if (!SameRows(before.lineBake, after.lineBake, [](const auto& lhs, const auto& rhs) {
                return lhs.id == rhs.id && SameVector(lhs.position, rhs.position);
            })) line.bakedPoints = value.lineBake;
        if (before.lineBakeSpace != after.lineBakeSpace) line.bakedSpace = value.lineBakeSpace;
        if (before.lineBakeSignature != after.lineBakeSignature) line.bakedInputSignature = value.lineBakeSignature;
        if (before.lineBakeSource != after.lineBakeSource) line.bakedSourceMode = value.lineBakeSource;
        if (before.lineBakeVersion != after.lineBakeVersion) line.bakedAdapterVersion = value.lineBakeVersion;
        if (before.lineBakeValid != after.lineBakeValid) line.bakeValid = value.lineBakeValid;
    } else {
        auto& link = *owner.GetComponent<scene::NavMeshOffMeshLinkComponent>();
        if (before.source != after.source) link.pointSourceMode = value.source;
        if (!SameVector(before.linkStart, after.linkStart)) link.startPoint = value.linkStart;
        if (!SameVector(before.linkEnd, after.linkEnd)) link.endPoint = value.linkEnd;
        if (!SameReference(before.linkStartTarget, after.linkStartTarget)) link.startTarget = value.linkStartTarget;
        if (!SameReference(before.linkEndTarget, after.linkEndTarget)) link.endTarget = value.linkEndTarget;
    }
}

struct PoseStamp {
    std::string guid;
    scene::EntityID entity = scene::EntityID::INVALID;
    std::string parent;
    scene::Transform pose;
    std::uint64_t markerLifetime = 0;
    bool activeSelf = true;
};

PoseStamp CapturePose(scene::Scene& world, scene::GameObject& object)
{
    PoseStamp stamp;
    stamp.guid = object.instanceId; stamp.entity = object.GetID(); stamp.pose = object.transform;
    if (const auto* parent = object.GetParent()) stamp.parent = parent->instanceId;
    stamp.markerLifetime = world.GetComponentLifetime<scene::TargetPointComponent>(stamp.entity);
    stamp.activeSelf = object.activeSelf();
    return stamp;
}

bool SameQuaternion(const math::Quaternion& lhs, const math::Quaternion& rhs)
{
    return SameFloat(lhs.x, rhs.x) && SameFloat(lhs.y, rhs.y)
        && SameFloat(lhs.z, rhs.z) && SameFloat(lhs.w, rhs.w);
}

bool SamePose(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return SameVector(lhs.position, rhs.position) && SameQuaternion(lhs.rotation, rhs.rotation)
        && SameVector(lhs.scale, rhs.scale);
}

bool PoseMatches(scene::Scene& world, const PoseStamp& stamp)
{
    auto* object = world.GetGameObject(stamp.entity);
    return object && object->instanceId == stamp.guid && SamePose(object->transform, stamp.pose)
        && (object->GetParent() ? object->GetParent()->instanceId : std::string{}) == stamp.parent
        && object->activeSelf() == stamp.activeSelf
        && world.GetComponentLifetime<scene::TargetPointComponent>(stamp.entity) == stamp.markerLifetime;
}

void CaptureChain(scene::Scene& world, scene::GameObject* object,
    std::vector<PoseStamp>& stamps, std::unordered_set<std::uint64_t>& known)
{
    for (; object; object = object->GetParent()) {
        const auto id = object->GetID();
        const std::uint64_t key = static_cast<std::uint64_t>(id.generation) << 32 | id.index;
        if (known.insert(key).second) stamps.push_back(CapturePose(world, *object));
    }
}

std::vector<std::string> TargetGuids(const AuthoringState& state, Domain domain)
{
    std::vector<std::string> result;
    if (domain == Domain::PATROL) {
        for (const auto& row : state.patrolBindings) if (!row.target.guid.empty()) result.push_back(row.target.guid);
    } else if (domain == Domain::LINE) {
        for (const auto& row : state.lineBindings) if (!row.target.guid.empty()) result.push_back(row.target.guid);
    } else {
        if (!state.linkStartTarget.guid.empty()) result.push_back(state.linkStartTarget.guid);
        if (!state.linkEndTarget.guid.empty()) result.push_back(state.linkEndTarget.guid);
    }
    return result;
}

struct CreatedObject {
    std::string guid = util::GenerateUUID();
    std::string parentGuid;
    std::string name;
    math::Vector3 position{};
    bool marker = false;
    PoseStamp stamp;
};

struct Plan {
    EditorContext* context = nullptr;
    Domain domain = Domain::LINE;
    std::uint64_t sceneGeneration = 0;
    std::string ownerGuid;
    scene::EntityID ownerId = scene::EntityID::INVALID;
    std::uint64_t componentLifetime = 0;
    AuthoringState before, after;
    std::vector<PoseStamp> dependencies;
    std::vector<CreatedObject> created;
    bool validateConversion = false;
    std::vector<math::Vector3> expectedPoints;
};

bool IsInsidePrefabInstance(const scene::GameObject* object)
{
    for (; object; object = object->GetParent()) if (!object->prefabAssetPath.empty()) return true;
    return false;
}

/// @note Absolute-world points share a parentless identity container so owner motion cannot move their targets.
void AddContainer(Plan& plan, std::string parentGuid, std::string name)
{
    CreatedObject container;
    container.parentGuid = std::move(parentGuid); container.name = std::move(name);
    plan.created.push_back(std::move(container));
}

std::string ControlPointContainerName(const scene::GameObject& owner)
{
    return "ControlPoints_" + owner.instanceId;
}

std::string FindRootContainer(Plan& plan, const scene::GameObject& owner)
{
    auto& world = *plan.context->activeScene;
    const std::string expectedName = ControlPointContainerName(owner);
    const scene::Transform identity;
    auto isReusable = [&](scene::GameObject* candidate) {
        if (!candidate || candidate->GetParent() || candidate->name != expectedName
            || !candidate->activeSelf() || !SamePose(candidate->transform, identity)) return false;
        bool hasComponent = false;
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            hasComponent |= candidate->GetComponent<T>() != nullptr;
        });
        return !hasComponent;
    };
    for (const auto& guid : TargetGuids(plan.before, plan.domain)) {
        auto* target = world.FindByGuid(guid);
        if (target && isReusable(target->GetParent())) return target->GetParent()->instanceId;
    }
    auto* named = world.Find(expectedName);
    return isReusable(named) ? named->instanceId : std::string{};
}

CreatedObject& AddPoint(Plan& plan, const std::string& parentGuid,
    std::uint32_t id, const math::Vector3& position)
{
    CreatedObject point;
    point.parentGuid = parentGuid; point.name = "Point_" + std::to_string(id);
    point.position = position; point.marker = true;
    plan.created.push_back(std::move(point));
    return plan.created.back();
}

scene::ObjectReference MakeReference(const CreatedObject& object)
{
    scene::ObjectReference reference;
    reference.guid = object.guid;
    return reference;
}

void AddDependencies(Plan& plan)
{
    auto& world = *plan.context->activeScene;
    std::unordered_set<std::uint64_t> known;
    for (const auto& stamp : plan.dependencies)
        known.insert(static_cast<std::uint64_t>(stamp.entity.generation) << 32 | stamp.entity.index);
    CaptureChain(world, world.GetGameObject(plan.ownerId), plan.dependencies, known);
    for (const auto& guid : TargetGuids(plan.before, plan.domain)) {
        if (auto* target = world.FindByGuid(guid)) CaptureChain(world, target, plan.dependencies, known);
    }
    for (const auto& guid : TargetGuids(plan.after, plan.domain)) {
        if (auto* target = world.FindByGuid(guid)) CaptureChain(world, target, plan.dependencies, known);
    }
}

std::shared_ptr<Plan> MakePlan(EditorContext& ctx, Domain domain, scene::GameObject& owner)
{
    auto& world = *ctx.activeScene;
    if (!HasDomainComponent(owner, domain) || ctx.IsLocked(owner.GetID())) return {};
    scene::FlushWorldTransforms(world);
    auto plan = std::make_shared<Plan>();
    plan->context = &ctx; plan->domain = domain; plan->sceneGeneration = world.GetRenderSceneGeneration();
    plan->ownerGuid = owner.instanceId; plan->ownerId = owner.GetID();
    plan->componentLifetime = ComponentLifetime(world, domain, owner.GetID());
    plan->before = Capture(owner, domain); plan->after = plan->before;
    AddDependencies(*plan);
    return plan;
}

bool ValidCreated(scene::Scene& world, const CreatedObject& created,
    const std::unordered_set<std::string>& createdGuids)
{
    if (!PoseMatches(world, created.stamp)) return false;
    auto* object = world.GetGameObject(created.stamp.entity);
    if (!object || object->name != created.name || !object->activeSelf() || !object->prefabAssetPath.empty()
        || !object->prefabSourceId.empty() || object->IsRuntimeGenerated() || object->tag != "Untagged" || object->layer != 0)
        return false;
    bool extraComponent = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (!std::is_same_v<T, scene::TargetPointComponent>)
            extraComponent |= object->GetComponent<T>() != nullptr;
    });
    if (extraComponent) return false;
    const auto* marker = object->GetComponent<scene::TargetPointComponent>();
    if (created.marker) {
        const scene::TargetPointComponent defaults;
        if (!marker || marker->visible != defaults.visible || marker->color != defaults.color
            || marker->screenSize != defaults.screenSize) return false;
    } else if (marker) return false;
    for (int index = 0; index < object->GetChildCount(); ++index) {
        const auto* child = object->GetChild(index);
        if (!child || !createdGuids.contains(child->instanceId)) return false;
    }
    return true;
}

class ReferenceProbe final : public scene::IReflector {
public:
    using scene::IReflector::Field;
    using scene::IReflector::ListField;
    explicit ReferenceProbe(const std::vector<CreatedObject>& objects)
    {
        for (const auto& object : objects) {
            m_guids.insert(object.guid);
            if (object.stamp.entity.IsValid()) m_entities.insert(EntityKey(object.stamp.entity));
        }
    }
    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char*, scene::EntityID& id) override
    {
        found |= id.IsValid() && m_entities.contains(EntityKey(id));
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        for (auto& value : values) Field(name, value.id);
    }
    void ObjectReferenceField(const char*, scene::ObjectReference& reference) override
    {
        found |= m_guids.contains(reference.guid);
    }
    void ObjectReferenceListField(const char* name, std::vector<scene::ObjectReference>& values) override
    {
        for (auto& value : values) ObjectReferenceField(name, value);
    }
    bool SavedMayReference(const std::string& fields) const
    {
        return std::any_of(m_guids.begin(), m_guids.end(), [&fields](const auto& guid) {
            return !guid.empty() && fields.find(guid) != std::string::npos;
        });
    }
    bool found = false;
private:
    static std::uint64_t EntityKey(scene::EntityID id)
    {
        return static_cast<std::uint64_t>(id.generation) << 32 | id.index;
    }
    std::unordered_set<std::string> m_guids;
    std::unordered_set<std::uint64_t> m_entities;
};

bool HasOtherReferences(scene::Scene& world, const Plan& plan)
{
    ReferenceProbe probe(plan.created);
    for (auto& object : world.GameObjects()) {
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (std::is_same_v<T, scene::ScriptComponent>) {
                if (auto* scripts = object.GetComponent<T>()) {
                    for (auto& entry : scripts->scripts) {
                        if (entry.script && !entry.script->IsRuntimeFaulted()) entry.script->Reflect(probe);
                        else if (entry.serialized && probe.SavedMayReference(entry.serialized->fieldsToml)) probe.found = true;
                    }
                }
            } else if constexpr (Registration::hasReflect && std::is_copy_constructible_v<T>) {
                const bool ownerDomainComponent = object.GetID() == plan.ownerId
                    && ((plan.domain == Domain::PATROL && std::is_same_v<T, scene::NavMeshPatrolComponent>)
                     || (plan.domain == Domain::LINE && std::is_same_v<T, scene::LineRendererComponent>)
                     || (plan.domain == Domain::NAV_LINK && std::is_same_v<T, scene::NavMeshOffMeshLinkComponent>));
                if (ownerDomainComponent) return;
                if (auto* component = object.GetComponent<T>()) {
                    T copy = *component;
                    copy.Reflect(probe);
                }
            }
        });
        if (probe.found) return true;
    }
    return false;
}

bool CreatePlannedObjects(Plan& plan, std::string& reason)
{
    auto& ctx = *plan.context;
    auto& world = *ctx.activeScene;
    if (plan.created.empty()) return true;
    if (IsInsidePrefabInstance(world.GetGameObject(plan.ownerId))) {
        reason = "Prefab instanceのHierarchy編集はasset編集またはunpack後に実行してください";
        return false;
    }
    if (!world.CanCreateGameObjects(plan.created.size())) {
        reason = "制御点を作成するEntity容量が不足しています";
        return false;
    }
    std::unordered_set<std::string> occupied;
    occupied.reserve(world.GameObjectCount() + plan.created.size());
    for (auto& object : world.GameObjects()) occupied.insert(object.instanceId);
    for (const auto& object : plan.created) {
        if (!occupied.insert(object.guid).second) {
            reason = "作成するGUIDが既に使われています";
            return false;
        }
    }
    std::unordered_map<std::string, scene::GameObject*> prepared;
    std::vector<scene::EntityID> made;
    made.reserve(plan.created.size());
    for (auto& spec : plan.created) {
        auto* object = world.TryCreateGameObject(spec.name);
        if (!object) {
            world.DestroyGameObjects(made);
            reason = "制御点の作成に失敗しました";
            return false;
        }
        made.push_back(object->GetID()); object->instanceId = spec.guid;
        scene::GameObject* parent = nullptr;
        if (!spec.parentGuid.empty()) {
            const auto found = prepared.find(spec.parentGuid);
            parent = found == prepared.end() ? world.FindByGuid(spec.parentGuid) : found->second;
            if (!parent || !object->SetParent(parent)) {
                world.DestroyGameObjects(made);
                reason = "制御点の親を設定できません";
                return false;
            }
        }
        object->transform.position = spec.position;
        if (spec.marker) object->AddComponent<scene::TargetPointComponent>();
        prepared.emplace(spec.guid, object);
        spec.stamp = CapturePose(world, *object);
    }
    scene::FlushWorldTransforms(world);
    for (auto& spec : plan.created) {
        if (auto* object = world.FindByGuid(spec.guid)) spec.stamp = CapturePose(world, *object);
    }
    return true;
}

void DestroyPlannedObjects(scene::Scene& world, const std::vector<CreatedObject>& objects)
{
    std::vector<scene::EntityID> ids;
    ids.reserve(objects.size());
    for (auto iterator = objects.rbegin(); iterator != objects.rend(); ++iterator)
        if (iterator->stamp.entity.IsValid()) ids.push_back(iterator->stamp.entity);
    if (!ids.empty()) world.DestroyGameObjects(ids);
}

bool ValidateConvertedPoints(Plan& plan, scene::GameObject& owner)
{
    auto& world = *plan.context->activeScene;
    if (plan.domain == Domain::PATROL) {
        auto& patrol = *owner.GetComponent<scene::NavMeshPatrolComponent>();
        const auto resolved = scene::ResolveNavMeshPatrolPointAuthoring(world, owner.GetID(), patrol,
            scene::ControlPointRuntimeMode::LIVE);
        if (!resolved.valid || resolved.points.size() != plan.expectedPoints.size()) return false;
        for (std::size_t index = 0; index < resolved.points.size(); ++index)
            if (!NearVector(resolved.points[index].worldPosition, plan.expectedPoints[index])) return false;
        return true;
    }
    if (plan.domain == Domain::LINE) {
        auto& line = *owner.GetComponent<scene::LineRendererComponent>();
        const auto resolved = scene::ResolveLineControlPointAuthoring(world, owner.GetID(), line,
            scene::ControlPointRuntimeMode::LIVE);
        if (!resolved.valid || resolved.points.size() != plan.expectedPoints.size()) return false;
        for (std::size_t index = 0; index < resolved.points.size(); ++index)
            if (!NearVector(resolved.points[index], plan.expectedPoints[index])) return false;
        return true;
    }
    const auto resolved = scene::ResolveNavMeshOffMeshLinkEndpoints(world, owner.GetID(),
        *owner.GetComponent<scene::NavMeshOffMeshLinkComponent>());
    return resolved.valid && NearVector(resolved.startPoint, plan.expectedPoints[0])
        && NearVector(resolved.endPoint, plan.expectedPoints[1]);
}

bool ApplyPlan(const std::shared_ptr<Plan>& plan, bool apply, std::string& reason)
{
    auto& ctx = *plan->context;
    if (!IsControlPointAuthoringAllowed(ctx) || !ctx.activeScene
        || ctx.activeScene->GetRenderSceneGeneration() != plan->sceneGeneration) {
        reason = "Sceneまたは編集状態が変わりました";
        return false;
    }
    auto& world = *ctx.activeScene;
    auto* owner = world.GetGameObject(plan->ownerId);
    if (!owner || owner->instanceId != plan->ownerGuid || ctx.IsLocked(plan->ownerId)
        || ComponentLifetime(world, plan->domain, plan->ownerId) != plan->componentLifetime
        || !HasDomainComponent(*owner, plan->domain)
        || !(Capture(*owner, plan->domain) == (apply ? plan->before : plan->after))) {
        reason = "所有者、Component寿命、または制御点authoring値が変更されています";
        return false;
    }
    for (const auto& stamp : plan->dependencies) {
        if (!PoseMatches(world, stamp) || ctx.IsLocked(stamp.entity)) {
            reason = "所有者、参照先、または親の姿勢が変更されています";
            return false;
        }
    }
    if (apply && !CreatePlannedObjects(*plan, reason)) return false;
    if (!apply && !plan->created.empty()) {
        std::unordered_set<std::string> createdGuids;
        for (const auto& created : plan->created) createdGuids.insert(created.guid);
        for (const auto& created : plan->created) {
            if (!ValidCreated(world, created, createdGuids) || ctx.IsLocked(created.stamp.entity)) {
                reason = "作成した制御点が移動または編集されています";
                return false;
            }
        }
        if (HasOtherReferences(world, *plan)) {
            reason = "作成した制御点が別のComponentやScriptから共有されています";
            return false;
        }
    }
    Write(*owner, plan->domain, plan->before, plan->after, apply);
    scene::FlushWorldTransforms(world);
    if (apply && plan->validateConversion && !ValidateConvertedPoints(*plan, *owner)) {
        Write(*owner, plan->domain, plan->before, plan->after, false);
        scene::FlushWorldTransforms(world);
        DestroyPlannedObjects(world, plan->created);
        reason = "変換前後の数値点が一致しません";
        return false;
    }
    if (!apply) DestroyPlannedObjects(world, plan->created);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    reason.clear();
    return true;
}

OpResult Commit(const std::shared_ptr<Plan>& plan, const char* label)
{
    OpResult result;
    if (plan->before == plan->after && plan->created.empty()) {
        result.noChange = true;
        return result;
    }
    auto command = std::make_unique<CheckedLambdaCommand>(label,
        [plan](std::string& reason) { return ApplyPlan(plan, true, reason); },
        [plan](std::string& reason) { return ApplyPlan(plan, false, reason); });
    std::string reason;
    if (!command->TryExecute(reason)) return OpResult::Err("CONFLICT", std::move(reason));
    result.command = std::move(command);
    OpData targets = OpData::MakeArray();
    for (const auto& created : plan->created) if (created.marker) targets.Push(created.guid);
    result.data.Set("targets", std::move(targets));
    return result;
}

std::uint32_t NextId(const AuthoringState& state, Domain domain)
{
    std::unordered_set<std::uint32_t> used;
    if (domain == Domain::PATROL) for (const auto& row : state.patrolBindings) used.insert(row.id);
    if (domain == Domain::LINE) for (const auto& row : state.lineBindings) used.insert(row.id);
    for (std::uint32_t id = 1; id <= 0x7fffffffu; ++id) if (!used.contains(id)) return id;
    return 0;
}

std::string AddPointParent(Plan& plan, scene::GameObject& owner)
{
    if (plan.domain == Domain::PATROL) {
        const std::string existing = FindRootContainer(plan, owner);
        if (!existing.empty()) return existing;
        AddContainer(plan, {}, ControlPointContainerName(owner));
        return plan.created.back().guid;
    }
    if (plan.domain == Domain::LINE) {
        auto* line = owner.GetComponent<scene::LineRendererComponent>();
        if (line && line->space == scene::LineSpace::World) {
            const std::string existing = FindRootContainer(plan, owner);
            if (!existing.empty()) return existing;
            AddContainer(plan, {}, ControlPointContainerName(owner));
            return plan.created.back().guid;
        }
        for (const auto& row : plan.before.lineBindings) {
            if (auto* target = plan.context->activeScene->FindByGuid(row.target.guid)) {
                auto* parent = target->GetParent();
                if (parent == &owner) return owner.instanceId;
                const scene::Transform identity;
                if (parent && parent->GetParent() == &owner && SamePose(parent->transform, identity)
                    && !parent->GetComponent<scene::TargetPointComponent>()) return parent->instanceId;
            }
        }
    }
    if (plan.domain == Domain::NAV_LINK) {
        const std::string existing = FindRootContainer(plan, owner);
        if (!existing.empty()) return existing;
        AddContainer(plan, {}, ControlPointContainerName(owner));
        return plan.created.back().guid;
    }
    AddContainer(plan, plan.ownerGuid, "ControlPoints");
    return plan.created.back().guid;
}

bool ValidateTarget(scene::Scene& world, const std::string& guid, scene::GameObject*& target)
{
    target = guid.empty() ? nullptr : world.FindByGuid(guid);
    return target && target->GetComponent<scene::TargetPointComponent>()
        && target->activeInHierarchy() && FiniteChain(target);
}

OpResult Convert(OpContext& context, Domain domain, const OpArgs& args, scene::GameObject& owner,
    const std::shared_ptr<Plan>& plan)
{
    const std::string source = args.GetString("source");
    if (source != "INLINE" && source != "POINT_OBJECTS")
        return OpResult::Err("BAD_SOURCE", "sourceにはINLINEまたはPOINT_OBJECTSを指定してください");
    const auto desired = source == "POINT_OBJECTS" ? scene::ControlPointSourceMode::POINT_OBJECTS
        : scene::ControlPointSourceMode::INLINE;
    if (plan->before.source == desired) { OpResult result; result.noChange = true; return result; }
    if (IsInsidePrefabInstance(&owner))
        return OpResult::Err("PREFAB_STRUCTURE", "Prefab asset編集またはunpack後に制御点sourceを変換してください");
    if (!FiniteChain(&owner)) return OpResult::Err("INVALID_TRANSFORM", "所有者または祖先のTransformが不正です");
    auto& world = *context.ctx.activeScene;
    scene::FlushWorldTransforms(world);

    if (domain == Domain::PATROL) {
        auto* patrol = owner.GetComponent<scene::NavMeshPatrolComponent>();
        const auto snapshot = scene::ResolveNavMeshPatrolPointAuthoring(world, owner.GetID(), *patrol,
            scene::ControlPointRuntimeMode::LIVE);
        if (!snapshot.valid) return OpResult::Err("INVALID_POINTS", "Patrolの全点を有効な数値点へ解決できません");
        plan->expectedPoints.reserve(snapshot.points.size());
        for (const auto& point : snapshot.points) plan->expectedPoints.push_back(point.worldPosition);
        plan->after.source = desired; plan->after.runtime = scene::ControlPointRuntimeMode::LIVE;
        if (desired == scene::ControlPointSourceMode::INLINE) {
            plan->after.positions.clear(); plan->after.waitTimes.clear(); plan->after.speeds.clear();
            for (const auto& point : snapshot.points) {
                plan->after.positions.push_back(point.worldPosition);
                plan->after.waitTimes.push_back(point.waitTime);
                plan->after.speeds.push_back(point.speed);
            }
            plan->after.patrolBindings.clear();
        } else {
            AddContainer(*plan, {}, ControlPointContainerName(owner));
            const std::string container = plan->created.back().guid;
            plan->after.patrolBindings.clear();
            for (const auto& point : snapshot.points) {
                const auto& created = AddPoint(*plan, container, point.stableId, point.worldPosition);
                scene::NavMeshPatrolWaypointBinding row;
                row.id = point.stableId; row.target = MakeReference(created);
                row.waitTime = point.waitTime; row.speed = point.speed;
                plan->after.patrolBindings.push_back(std::move(row));
            }
            plan->after.positions.clear(); plan->after.waitTimes.clear(); plan->after.speeds.clear();
        }
    } else if (domain == Domain::LINE) {
        auto* line = owner.GetComponent<scene::LineRendererComponent>();
        const auto snapshot = scene::ResolveLineControlPointAuthoring(world, owner.GetID(), *line,
            scene::ControlPointRuntimeMode::LIVE);
        if (!snapshot.valid) return OpResult::Err("INVALID_POINTS", "Lineの全点を有効な数値点へ解決できません");
        plan->expectedPoints = snapshot.points;
        plan->after.source = desired; plan->after.runtime = scene::ControlPointRuntimeMode::LIVE;
        if (desired == scene::ControlPointSourceMode::INLINE) {
            plan->after.positions = snapshot.points; plan->after.lineBindings.clear();
        } else {
            const bool worldSpace = line->space == scene::LineSpace::World;
            AddContainer(*plan, worldSpace ? std::string{} : plan->ownerGuid,
                worldSpace ? ControlPointContainerName(owner) : "ControlPoints");
            const std::string container = plan->created.back().guid;
            plan->after.lineBindings.clear();
            for (std::size_t index = 0; index < snapshot.points.size(); ++index) {
                const std::uint32_t id = static_cast<std::uint32_t>(index + 1);
                const auto& created = AddPoint(*plan, container, id, snapshot.points[index]);
                plan->after.lineBindings.push_back({id, MakeReference(created)});
            }
            plan->after.positions.clear();
        }
    } else {
        auto* link = owner.GetComponent<scene::NavMeshOffMeshLinkComponent>();
        const auto snapshot = scene::ResolveNavMeshOffMeshLinkEndpoints(world, owner.GetID(), *link);
        if (!snapshot.valid) return OpResult::Err("INVALID_POINTS", "Off-Mesh Linkの両端を有効なworld位置へ解決できません");
        plan->expectedPoints = {snapshot.startPoint, snapshot.endPoint};
        plan->after.source = desired;
        if (desired == scene::ControlPointSourceMode::INLINE) {
            plan->after.linkStart = snapshot.startPoint; plan->after.linkEnd = snapshot.endPoint;
            plan->after.linkStartTarget = {}; plan->after.linkEndTarget = {};
        } else {
            AddContainer(*plan, {}, ControlPointContainerName(owner));
            const std::string container = plan->created.back().guid;
            auto& start = AddPoint(*plan, container, 1, snapshot.startPoint);
            const auto startReference = MakeReference(start);
            auto& end = AddPoint(*plan, container, 2, snapshot.endPoint);
            plan->after.linkStartTarget = startReference;
            plan->after.linkEndTarget = MakeReference(end);
        }
    }
    plan->validateConversion = true;
    AddDependencies(*plan);
    return Commit(plan, "Convert Control Point Source");
}

std::uint32_t RowId(const OpArgs& args)
{
    const int id = args.GetInt("id");
    return id > 0 ? static_cast<std::uint32_t>(id) : 0u;
}

OpResult Add(OpContext& context, Domain domain, const OpArgs& args,
    scene::GameObject& owner, const std::shared_ptr<Plan>& plan)
{
    if (plan->before.source != scene::ControlPointSourceMode::POINT_OBJECTS)
        return OpResult::Err("POINT_OBJECTS_REQUIRED", "TargetPointを位置ソースにする所有者が必要です");
    std::uint32_t id = 0;
    if (domain == Domain::NAV_LINK) {
        id = RowId(args);
        if (id != 1 && id != 2) return OpResult::Err("BAD_ENDPOINT", "Off-Mesh LinkのidはStart=1、End=2です");
        const auto& existing = id == 1 ? plan->before.linkStartTarget : plan->before.linkEndTarget;
        if (!existing.guid.empty()) return OpResult::Err("ENDPOINT_BOUND", "このLink endpointには既にTargetPointが接続されています");
    }
    if (!args.Has("worldPosition") && !FiniteChain(&owner))
        return OpResult::Err("INVALID_TRANSFORM", "所有者または祖先のTransformが不正です");
    auto& world = *context.ctx.activeScene;
    scene::FlushWorldTransforms(world);
    if (args.Has("worldPosition") == args.Has("localPosition"))
        return OpResult::Err("BAD_POSITION", "worldPositionまたはlocalPositionを1つ指定してください");
    const bool worldSpace = domain != Domain::LINE
        || owner.GetComponent<scene::LineRendererComponent>()->space == scene::LineSpace::World;
    math::Vector3 position = args.GetVec3("worldPosition");
    if (args.Has("worldPosition")) {
        if (!worldSpace || !Finite(position))
            return OpResult::Err("BAD_POSITION", "worldPositionはworld座標のPoint Objectsだけに指定できます");
    } else {
        const math::Vector3 localPosition = args.GetVec3("localPosition");
        if (!Finite(localPosition)) return OpResult::Err("BAD_POSITION", "点位置は有限値である必要があります");
        position = localPosition;
        if (worldSpace && !ToOwnerWorld(owner.transform, localPosition, position))
            return OpResult::Err("INVALID_TRANSFORM", "owner-local位置をworldへ変換できません");
    }
    if (domain != Domain::NAV_LINK) {
        const std::size_t count = domain == Domain::PATROL ? plan->after.patrolBindings.size() : plan->after.lineBindings.size();
        const std::size_t maximum = domain == Domain::PATROL ? scene::MAX_NAV_MESH_PATROL_WAYPOINTS
            : scene::MAX_CONTROL_POINT_BINDINGS;
        if (count >= maximum) return OpResult::Err("LIMIT", "制御点数の上限を越えています");
        id = NextId(plan->after, domain);
        if (id == 0) return OpResult::Err("ID_EXHAUSTED", "利用可能な制御点IDがありません");
    }
    const std::string parent = AddPointParent(*plan, owner);
    const auto& created = AddPoint(*plan, parent, id, position);
    const auto reference = MakeReference(created);
    if (domain == Domain::PATROL) {
        const float wait = args.GetFloat("waitTime", plan->after.patrolWaitTime);
        const float speed = args.GetFloat("speed", 0.0f);
        if (!std::isfinite(wait) || !std::isfinite(speed) || wait < 0.0f || speed < 0.0f)
            return OpResult::Err("BAD_ATTRIBUTES", "waitTimeとspeedは有限な非負値です");
        plan->after.patrolBindings.push_back({id, reference, wait, speed});
    } else if (domain == Domain::LINE) plan->after.lineBindings.push_back({id, reference});
    else if (id == 1) plan->after.linkStartTarget = reference;
    else plan->after.linkEndTarget = reference;
    AddDependencies(*plan);
    auto result = Commit(plan, "Add Control Point");
    result.data.Set("id", static_cast<int>(id));
    return result;
}

OpResult EditRow(OpContext& context, Domain domain, const OpArgs& args,
    const std::shared_ptr<Plan>& plan, std::string_view opId)
{
    auto* owner = context.ctx.activeScene->GetGameObject(plan->ownerId);
    if (owner && IsInsidePrefabInstance(owner)
        && (opId == "control_points.bind" || opId == "control_points.unbind"
            || opId == "control_points.remove" || opId == "control_points.reorder"))
        return OpResult::Err("PREFAB_STRUCTURE", "Prefab asset編集またはunpack後に制御点bindingを変更してください");
    const std::uint32_t id = RowId(args);
    if (id == 0) return OpResult::Err("BAD_ID", "正のbinding IDが必要です");
    if (domain == Domain::NAV_LINK) {
        if (plan->before.source != scene::ControlPointSourceMode::POINT_OBJECTS || (id != 1 && id != 2))
            return OpResult::Err("BAD_ENDPOINT", "POINT_OBJECTSのStartはID 1、EndはID 2を指定してください");
        if (opId == "control_points.bind") {
            scene::GameObject* target = nullptr;
            const std::string guid = args.GetString("target");
            if (!ValidateTarget(*context.ctx.activeScene, guid, target)) return OpResult::Err("NO_TARGET", "同じSceneの有効なTargetPointが必要です");
            const auto& other = id == 1 ? plan->before.linkEndTarget : plan->before.linkStartTarget;
            if (other.guid == guid) return OpResult::Err("DUPLICATE_TARGET", "StartとEndには別のTargetPointを指定してください");
            scene::ObjectReference reference; reference.guid = guid;
            (id == 1 ? plan->after.linkStartTarget : plan->after.linkEndTarget) = std::move(reference);
        } else if (opId == "control_points.unbind") {
            (id == 1 ? plan->after.linkStartTarget : plan->after.linkEndTarget) = {};
        } else return OpResult::Err("UNSUPPORTED", "Off-Mesh Linkの行操作はbind/unbindのみです");
    } else if (plan->before.source != scene::ControlPointSourceMode::POINT_OBJECTS) {
        return OpResult::Err("POINT_OBJECTS_REQUIRED", "TargetPointを位置ソースにする所有者が必要です");
    } else {
        auto edit = [&](auto& rows) -> bool {
            const auto found = std::find_if(rows.begin(), rows.end(), [id](const auto& row) { return row.id == id; });
            if (found == rows.end()) return false;
            const std::size_t index = static_cast<std::size_t>(found - rows.begin());
            if (opId == "control_points.bind") {
                scene::GameObject* target = nullptr;
                const std::string guid = args.GetString("target");
                if (!ValidateTarget(*context.ctx.activeScene, guid, target)) return false;
                if (domain == Domain::PATROL && (target == owner || target->IsDescendantOf(*owner)))
                    return false;
                if (std::any_of(rows.begin(), rows.end(), [id, &guid](const auto& row) { return row.id != id && row.target.guid == guid; })) return false;
                found->target = {}; found->target.guid = guid;
                return true;
            }
            if (opId == "control_points.unbind") { found->target = {}; return true; }
            if (opId == "control_points.remove") { rows.erase(found); return true; }
            if (opId == "control_points.reorder") {
                const int destination = args.GetInt("toIndex", -1);
                if (destination < 0 || static_cast<std::size_t>(destination) >= rows.size()) return false;
                const auto value = *found;
                rows.erase(found);
                rows.insert(rows.begin() + destination, value);
                return true;
            }
            if (opId == "control_points.set_patrol_attributes" && domain == Domain::PATROL) {
                if constexpr (std::is_same_v<std::remove_cvref_t<decltype(*found)>,
                    scene::NavMeshPatrolWaypointBinding>) {
                    const float wait = args.GetFloat("waitTime", found->waitTime);
                    const float speed = args.GetFloat("speed", found->speed);
                    if (!std::isfinite(wait) || !std::isfinite(speed) || wait < 0.0f || speed < 0.0f) return false;
                    found->waitTime = wait; found->speed = speed;
                    return true;
                }
                return false;
            }
            return false;
        };
        const bool changed = domain == Domain::PATROL ? edit(plan->after.patrolBindings) : edit(plan->after.lineBindings);
        if (!changed) return OpResult::Err("BAD_ROW", "対象の制御点が見つからないか引数が不正です");
    }
    AddDependencies(*plan);
    return Commit(plan, "Edit Control Point Binding");
}

bool BakeMatches(const AuthoringState& current, Domain domain, scene::GameObject& owner,
    scene::Scene& world, std::uint64_t& signature)
{
    if (domain == Domain::PATROL) {
        const auto snapshot = scene::ResolveNavMeshPatrolPointAuthoring(world, owner.GetID(),
            *owner.GetComponent<scene::NavMeshPatrolComponent>(), scene::ControlPointRuntimeMode::BAKED_STATIC);
        signature = snapshot.inputSignature;
        if (!snapshot.valid || !current.patrolBakeValid || current.patrolBakeVersion != scene::NAV_MESH_PATROL_ADAPTER_VERSION
            || current.patrolBakeSource != current.source || current.patrolBakeSignature != signature
            || current.patrolBake.size() != snapshot.points.size()) return false;
        for (std::size_t index = 0; index < snapshot.points.size(); ++index) {
            const auto& point = snapshot.points[index]; const auto& baked = current.patrolBake[index];
            if (point.stableId != baked.id || !SameVector(point.worldPosition, baked.worldPosition)
                || !SameFloat(point.waitTime, baked.waitTime) || !SameFloat(point.speed, baked.speed)) return false;
        }
        return true;
    }
    auto& line = *owner.GetComponent<scene::LineRendererComponent>();
    const auto snapshot = scene::ResolveLineControlPointAuthoring(world, owner.GetID(), line,
        scene::ControlPointRuntimeMode::BAKED_STATIC);
    signature = snapshot.inputSignature;
    if (!snapshot.valid || !current.lineBakeValid || current.lineBakeVersion != scene::LINE_CONTROL_POINT_ADAPTER_VERSION
        || current.lineBakeSource != current.source || current.lineBakeSpace != current.lineSpace
        || current.lineBakeSignature != signature || current.lineBake.size() != snapshot.points.size()) return false;
    for (std::size_t index = 0; index < snapshot.points.size(); ++index) {
        const std::uint32_t id = current.source == scene::ControlPointSourceMode::INLINE
            ? static_cast<std::uint32_t>(index + 1) : current.lineBindings[index].id;
        if (current.lineBake[index].id != id || !SameVector(current.lineBake[index].position, snapshot.points[index])) return false;
    }
    return true;
}

OpResult Bake(OpContext& context, Domain domain, scene::GameObject& owner,
    const std::shared_ptr<Plan>& plan)
{
    if (domain == Domain::NAV_LINK) return OpResult::Err("NO_BAKE", "Off-Mesh Link endpoint capture is handled by NavMesh bake");
    auto& world = *context.ctx.activeScene;
    scene::FlushWorldTransforms(world);
    std::uint64_t signature = 0;
    if (domain == Domain::PATROL) {
        const auto snapshot = scene::ResolveNavMeshPatrolPointAuthoring(world, owner.GetID(),
            *owner.GetComponent<scene::NavMeshPatrolComponent>(), scene::ControlPointRuntimeMode::BAKED_STATIC);
        if (!snapshot.valid) return OpResult::Err("INVALID_POINTS", "Patrolの全点を解決できません");
        signature = snapshot.inputSignature;
        plan->after.patrolBake.clear();
        for (const auto& point : snapshot.points)
            plan->after.patrolBake.push_back({point.stableId, point.worldPosition, point.waitTime, point.speed});
        plan->after.patrolBakeSource = plan->after.source;
        plan->after.patrolBakeVersion = scene::NAV_MESH_PATROL_ADAPTER_VERSION;
        plan->after.patrolBakeSignature = signature; plan->after.patrolBakeValid = true;
    } else {
        auto& line = *owner.GetComponent<scene::LineRendererComponent>();
        const auto snapshot = scene::ResolveLineControlPointAuthoring(world, owner.GetID(), line,
            scene::ControlPointRuntimeMode::BAKED_STATIC);
        if (!snapshot.valid) return OpResult::Err("INVALID_POINTS", "Lineの全点を解決できません");
        signature = snapshot.inputSignature;
        plan->after.lineBake.clear();
        for (std::size_t index = 0; index < snapshot.points.size(); ++index) {
            const std::uint32_t id = plan->after.source == scene::ControlPointSourceMode::INLINE
                ? static_cast<std::uint32_t>(index + 1) : plan->after.lineBindings[index].id;
            plan->after.lineBake.push_back({id, snapshot.points[index]});
        }
        plan->after.lineBakeSpace = snapshot.space; plan->after.lineBakeSource = plan->after.source;
        plan->after.lineBakeVersion = scene::LINE_CONTROL_POINT_ADAPTER_VERSION;
        plan->after.lineBakeSignature = signature; plan->after.lineBakeValid = true;
    }
    plan->after.runtime = scene::ControlPointRuntimeMode::BAKED_STATIC;
    return Commit(plan, "Bake Control Points");
}

OpResult SetRuntime(OpContext& context, Domain domain, scene::GameObject& owner,
    const OpArgs& args, const std::shared_ptr<Plan>& plan)
{
    if (domain == Domain::NAV_LINK) return OpResult::Err("NO_RUNTIME_MODE", "Off-Mesh LinkはNavMesh capture policyを使用します");
    const std::string mode = args.GetString("mode");
    if (mode == "LIVE") plan->after.runtime = scene::ControlPointRuntimeMode::LIVE;
    else if (mode == "BAKED_STATIC") {
        std::uint64_t signature = 0;
        scene::FlushWorldTransforms(*context.ctx.activeScene);
        if (!BakeMatches(plan->before, domain, owner, *context.ctx.activeScene, signature))
            return OpResult::Err("STALE_BAKE", "現在の制御点と一致するベイクが必要です");
        plan->after.runtime = scene::ControlPointRuntimeMode::BAKED_STATIC;
    } else return OpResult::Err("BAD_MODE", "modeにはLIVEまたはBAKED_STATICを指定してください");
    return Commit(plan, "Set Control Point Runtime Mode");
}

} /// @note namespace

OpResult InvokeAdditionalControlPointOperation(OpContext& context,
    std::string_view opId, const OpArgs& args)
{
    const std::string kind = args.GetString("kind");
    bool validKind = false;
    const Domain domain = ParseDomain(kind, validKind);
    if (!validKind) return OpResult::Err("UNSUPPORTED_KIND", "kindにはpatrol、line、nav_linkを指定してください");
    auto* owner = ResolveOwner(context.ctx, args);
    if (!owner || !HasDomainComponent(*owner, domain)) return OpResult::Err("NO_OWNER", "対応する制御点ownerが必要です");
    auto plan = MakePlan(context.ctx, domain, *owner);
    if (!plan) return OpResult::Err("LOCKED", "ownerがロックされているか変更できません");
    if (opId == "control_points.convert") return Convert(context, domain, args, *owner, plan);
    if (opId == "control_points.add") return Add(context, domain, args, *owner, plan);
    if (opId == "control_points.remove" || opId == "control_points.reorder"
        || opId == "control_points.unbind" || opId == "control_points.bind"
        || opId == "control_points.set_patrol_attributes")
        return EditRow(context, domain, args, plan, opId);
    if (opId == "control_points.bake") return Bake(context, domain, *owner, plan);
    if (opId == "control_points.set_runtime") return SetRuntime(context, domain, *owner, args, plan);
    return OpResult::Err("UNSUPPORTED_OPERATION", "この操作は追加制御点adapterの対象外です");
}

} /// @note namespace fbzz::editor
