/// @file    ControlPointOperators.cpp
/// @brief   制御点の正本変換、接続編集とベイクを検証付きUndoへまとめる。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/Tools/AdditionalControlPointEditing.hpp>
#include <Engine/Scene/Components/NavMeshPatrolComponent.hpp>
#include <Engine/Scene/Components/NavMeshOffMeshLinkComponent.hpp>
#include <Engine/Scene/Components/PresentationComponents.hpp>
#include <Editor/Util/ObjectCreation.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/Uuid.hpp>
#include "../Tools/TerrainTool.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fbzz::editor {
namespace {

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool FinitePose(const scene::Transform& pose)
{
    const auto& q = pose.rotation;
    const double lengthSquared = static_cast<double>(q.x) * q.x + static_cast<double>(q.y) * q.y
        + static_cast<double>(q.z) * q.z + static_cast<double>(q.w) * q.w;
    return Finite(pose.position) && Finite(pose.scale) && std::isfinite(lengthSquared)
        && std::abs(lengthSquared - 1.0) <= 2.0e-3
        && std::abs(pose.scale.x) > 1.0e-6f && std::abs(pose.scale.y) > 1.0e-6f && std::abs(pose.scale.z) > 1.0e-6f;
}

bool FiniteChain(const scene::GameObject* object)
{
    for (; object; object = object->GetParent()) if (!FinitePose(object->transform)) return false;
    return true;
}

bool SameFloat(float lhs, float rhs)
{
    return std::bit_cast<uint32_t>(lhs) == std::bit_cast<uint32_t>(rhs);
}

bool SameVector(const math::Vector3& lhs, const math::Vector3& rhs)
{
    return SameFloat(lhs.x, rhs.x) && SameFloat(lhs.y, rhs.y) && SameFloat(lhs.z, rhs.z);
}

bool SameQuaternion(const math::Quaternion& lhs, const math::Quaternion& rhs)
{
    return SameFloat(lhs.x, rhs.x) && SameFloat(lhs.y, rhs.y) && SameFloat(lhs.z, rhs.z) && SameFloat(lhs.w, rhs.w);
}

bool EquivalentPosition(const math::Vector3& lhs, const math::Vector3& rhs)
{
    const auto near = [](float a, float b) { return std::abs(a - b) <= 1.0e-5f * (std::max)(1.0f, (std::max)(std::abs(a), std::abs(b))); };
    return Finite(lhs) && Finite(rhs) && near(lhs.x, rhs.x) && near(lhs.y, rhs.y) && near(lhs.z, rhs.z);
}

template<typename T, typename Equal>
bool SameRows(const std::vector<T>& lhs, const std::vector<T>& rhs, Equal equal)
{
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin(), equal);
}

bool SamePose(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position == rhs.position && lhs.rotation == rhs.rotation && lhs.scale == rhs.scale;
}

template<typename T>
void ClearReferenceCaches(std::vector<T>& bindings)
{
    for (auto& row : bindings) { row.target.id = scene::EntityID::INVALID; row.target.sceneGeneration = 0; }
}

/// @note Undoの比較はGUIDだけを使い、resolverが温めたEntityIDキャッシュを編集と見なさない。
struct PointAuthoring {
    scene::ControlPointSourceMode source = scene::ControlPointSourceMode::INLINE;
    scene::ControlPointRuntimeMode runtime = scene::ControlPointRuntimeMode::LIVE;
    std::vector<scene::WaterSplinePoint> waterPoints;
    std::vector<math::Vector3> splinePoints;
    std::vector<scene::WaterControlPointBinding> waterBindings;
    std::vector<scene::SplineControlPointBinding> splineBindings;
    float lakeHeight = 0.0f;
    scene::WaterSurfaceShape waterShape = scene::WaterSurfaceShape::RECTANGLE;
    bool splineClosed = false;
    bool bakedSplineClosed = false;
    std::vector<scene::WaterSplinePoint> waterBake;
    std::vector<scene::SplineControlPointValue> splineBake;
    uint64_t bakeSignature = 0;
    scene::ControlPointSourceMode bakeSource = scene::ControlPointSourceMode::INLINE;
    uint32_t bakeVersion = 0;
    bool bakeValid = false;
    scene::WaterSurfaceShape bakedWaterShape = scene::WaterSurfaceShape::RECTANGLE;
    float bakedLakeHeight = 0.0f;
    bool operator==(const PointAuthoring& other) const
    {
        const auto waterPoint = [](const auto& lhs, const auto& rhs) {
            return lhs.id == rhs.id && SameVector(lhs.localPosition, rhs.localPosition)
                && SameFloat(lhs.width, rhs.width) && SameFloat(lhs.depth, rhs.depth) && SameFloat(lhs.flowSpeed, rhs.flowSpeed);
        };
        const auto waterBinding = [](const auto& lhs, const auto& rhs) {
            return lhs.id == rhs.id && lhs.target.guid == rhs.target.guid
                && SameFloat(lhs.width, rhs.width) && SameFloat(lhs.depth, rhs.depth) && SameFloat(lhs.flowSpeed, rhs.flowSpeed);
        };
        return source == other.source && runtime == other.runtime && SameFloat(lakeHeight, other.lakeHeight)
            && waterShape == other.waterShape && splineClosed == other.splineClosed
            && bakedSplineClosed == other.bakedSplineClosed
            && SameRows(waterPoints, other.waterPoints, waterPoint) && SameRows(waterBake, other.waterBake, waterPoint)
            && SameRows(splinePoints, other.splinePoints, SameVector)
            && SameRows(waterBindings, other.waterBindings, waterBinding)
            && SameRows(splineBindings, other.splineBindings, [](const auto& lhs, const auto& rhs) { return lhs.id == rhs.id && lhs.target.guid == rhs.target.guid; })
            && SameRows(splineBake, other.splineBake, [](const auto& lhs, const auto& rhs) { return lhs.id == rhs.id && SameVector(lhs.localPosition, rhs.localPosition); })
            && bakeSignature == other.bakeSignature && bakeSource == other.bakeSource && bakeVersion == other.bakeVersion && bakeValid == other.bakeValid
            && bakedWaterShape == other.bakedWaterShape && SameFloat(bakedLakeHeight, other.bakedLakeHeight);
    }
};

PointAuthoring Capture(scene::GameObject& owner, bool waterKind)
{
    PointAuthoring state;
    if (waterKind) {
        const auto& water = *owner.GetComponent<scene::WaterComponent>();
        state.source = water.pointSourceMode; state.runtime = water.pointRuntimeMode;
        state.waterPoints = water.splinePoints; state.waterBindings = water.pointBindings;
        state.lakeHeight = water.lakePlaneHeight; state.waterBake = water.bakedSplinePoints;
        state.waterShape = water.surfaceShape; state.bakedWaterShape = water.bakedSurfaceShape; state.bakedLakeHeight = water.bakedLakePlaneHeight;
        state.bakeSignature = water.bakedInputSignature; state.bakeSource = water.bakedSourceMode;
        state.bakeVersion = water.bakedAdapterVersion; state.bakeValid = water.bakeValid;
        ClearReferenceCaches(state.waterBindings);
    } else {
        const auto& spline = *owner.GetComponent<scene::SplineComponent>();
        state.source = spline.pointSourceMode; state.runtime = spline.pointRuntimeMode;
        state.splinePoints = spline.points; state.splineBindings = spline.pointBindings;
        state.splineClosed = spline.closed;
        state.bakedSplineClosed = spline.bakedClosed;
        state.splineBake = spline.bakedPoints; state.bakeSignature = spline.bakedInputSignature;
        state.bakeSource = spline.bakedSourceMode; state.bakeVersion = spline.bakedAdapterVersion;
        state.bakeValid = spline.bakeValid; ClearReferenceCaches(state.splineBindings);
    }
    return state;
}

template<typename T>
void AssignChanged(T& field, const T& before, const T& after, bool apply)
{
    if (before != after) field = apply ? after : before;
}

/// @note Component全体やsnapshot/physics handleを復元せず、変更したauthoring fieldだけを適用する。
void Write(scene::GameObject& owner, bool waterKind, const PointAuthoring& before,
           const PointAuthoring& after, bool apply)
{
    if (waterKind) {
        auto& water = *owner.GetComponent<scene::WaterComponent>();
        AssignChanged(water.pointSourceMode, before.source, after.source, apply);
        AssignChanged(water.pointRuntimeMode, before.runtime, after.runtime, apply);
        AssignChanged(water.splinePoints, before.waterPoints, after.waterPoints, apply);
        AssignChanged(water.pointBindings, before.waterBindings, after.waterBindings, apply);
        AssignChanged(water.lakePlaneHeight, before.lakeHeight, after.lakeHeight, apply);
        AssignChanged(water.bakedSplinePoints, before.waterBake, after.waterBake, apply);
        AssignChanged(water.bakedInputSignature, before.bakeSignature, after.bakeSignature, apply);
        AssignChanged(water.bakedSourceMode, before.bakeSource, after.bakeSource, apply);
        AssignChanged(water.bakedAdapterVersion, before.bakeVersion, after.bakeVersion, apply);
        AssignChanged(water.bakeValid, before.bakeValid, after.bakeValid, apply);
        AssignChanged(water.bakedSurfaceShape, before.bakedWaterShape, after.bakedWaterShape, apply);
        AssignChanged(water.bakedLakePlaneHeight, before.bakedLakeHeight, after.bakedLakeHeight, apply);
        water.meshDirty = true; water.foamDirty = true;
    } else {
        auto& spline = *owner.GetComponent<scene::SplineComponent>();
        AssignChanged(spline.pointSourceMode, before.source, after.source, apply);
        AssignChanged(spline.pointRuntimeMode, before.runtime, after.runtime, apply);
        AssignChanged(spline.points, before.splinePoints, after.splinePoints, apply);
        AssignChanged(spline.pointBindings, before.splineBindings, after.splineBindings, apply);
        AssignChanged(spline.bakedPoints, before.splineBake, after.splineBake, apply);
        AssignChanged(spline.bakedClosed, before.bakedSplineClosed, after.bakedSplineClosed, apply);
        AssignChanged(spline.bakedInputSignature, before.bakeSignature, after.bakeSignature, apply);
        AssignChanged(spline.bakedSourceMode, before.bakeSource, after.bakeSource, apply);
        AssignChanged(spline.bakedAdapterVersion, before.bakeVersion, after.bakeVersion, apply);
        AssignChanged(spline.bakeValid, before.bakeValid, after.bakeValid, apply);
    }
}

struct PoseStamp {
    std::string guid;
    scene::EntityID entity = scene::EntityID::INVALID;
    std::string parent;
    scene::Transform pose;
    uint64_t markerLifetime = 0;
};

PoseStamp CapturePose(scene::Scene& scene, scene::GameObject& object)
{
    PoseStamp stamp;
    stamp.guid = object.instanceId; stamp.entity = object.GetID(); stamp.pose = object.transform;
    if (const auto* parent = object.GetParent()) stamp.parent = parent->instanceId;
    stamp.markerLifetime = scene.GetComponentLifetime<scene::TargetPointComponent>(stamp.entity);
    return stamp;
}

bool PoseMatches(scene::Scene& scene, const PoseStamp& stamp)
{
    auto* object = scene.GetGameObject(stamp.entity);
    return object && object->instanceId == stamp.guid && SamePose(object->transform, stamp.pose)
        && (object->GetParent() ? object->GetParent()->instanceId : std::string{}) == stamp.parent
        && scene.GetComponentLifetime<scene::TargetPointComponent>(stamp.entity) == stamp.markerLifetime;
}

void CaptureChain(scene::Scene& scene, scene::GameObject* object, std::vector<PoseStamp>& stamps,
                  std::unordered_set<uint64_t>* known = nullptr)
{
    for (; object; object = object->GetParent()) {
        if (known) {
            const auto id = object->GetID();
            const uint64_t key = static_cast<uint64_t>(id.generation) << 32 | id.index;
            if (!known->insert(key).second) break;
            stamps.push_back(CapturePose(scene, *object)); continue;
        }
        if (std::none_of(stamps.begin(), stamps.end(), [object](const auto& stamp) { return stamp.entity == object->GetID(); }))
            stamps.push_back(CapturePose(scene, *object));
    }
}

struct NewObject {
    std::string guid = util::GenerateUUID();
    std::string parent;
    std::string name;
    math::Vector3 position{};
    bool marker = false;
    PoseStamp stamp;
};

/// @note typed referenceだけを拾う。未実体化Scriptは保存GUIDを含む場合に削除を保守的に拒否する。
class ReferenceProbe final : public scene::IReflector {
public:
    using scene::IReflector::Field;
    using scene::IReflector::ListField;
    explicit ReferenceProbe(const std::vector<NewObject>& objects) : m_objects(objects)
    {
        m_guids.reserve(objects.size()); m_entities.reserve(objects.size());
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
        for (auto& value : values) {
            if (!value.guid.empty()) found |= m_guids.contains(value.guid);
            else Field(name, value.id);
        }
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
        return std::any_of(m_objects.begin(), m_objects.end(), [&fields](const auto& object) { return fields.find(object.guid) != std::string::npos; });
    }
    bool found = false;
private:
    static uint64_t EntityKey(scene::EntityID id) { return static_cast<uint64_t>(id.generation) << 32 | id.index; }
    const std::vector<NewObject>& m_objects;
    std::unordered_set<std::string> m_guids;
    std::unordered_set<uint64_t> m_entities;
};

/// @note Keep runtime script callbacks outside the registry's generic lambda to avoid MSVC 14.51 C1001.
void ProbeScriptReferences(scene::GameObject& object, ReferenceProbe& probe)
{
    if (auto* scripts = object.GetComponent<scene::ScriptComponent>()) {
        for (auto& entry : scripts->scripts) {
            if (entry.script && !entry.script->ExecuteCallback([&] {
                    entry.script->Reflect(probe);
                }, "editor.control_point_reference_snapshot")) probe.found = true;
            if (entry.serialized && probe.SavedMayReference(entry.serialized->fieldsToml)) probe.found = true;
        }
    }
}

bool HasOtherReferences(scene::Scene& scene, scene::GameObject& owner, bool waterKind,
                        const std::vector<NewObject>& objects, uint32_t ignoredClothBinding = 0)
{
    ReferenceProbe probe(objects);
    for (auto& object : scene.GameObjects()) {
        ProbeScriptReferences(object, probe);
        if (probe.found) return true;
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (Registration::hasReflect && std::is_copy_constructible_v<T>) {
                if (ignoredClothBinding == 0 && &object == &owner && ((waterKind && std::is_same_v<T, scene::WaterComponent>)
                    || (!waterKind && std::is_same_v<T, scene::SplineComponent>))) return;
                if (auto* component = object.GetComponent<T>()) {
                    T copy = *component;
                    if constexpr (std::is_same_v<T, scene::ClothComponent>) {
                        if (&object == &owner && ignoredClothBinding != 0)
                            for (auto& row : copy.attachments) if (row.bindingId == ignoredClothBinding) {
                                row.target = scene::EntityID::INVALID; row.targetReference = {};
                            }
                    }
                    copy.Reflect(probe);
                    if constexpr (std::is_same_v<T, scene::IKSolverComponent>) for (auto& chain : copy.chains) {
                        probe.ObjectReferenceField("target", chain.targetReference);
                        probe.ObjectReferenceField("pole", chain.poleReference);
                        if (chain.targetReference.guid.empty()) probe.Field("targetEntity", chain.targetEntity);
                        if (chain.poleReference.guid.empty()) probe.Field("poleEntity", chain.poleEntity);
                    }
                    if constexpr (std::is_same_v<T, scene::LODGroupComponent>)
                        for (const auto& level : copy.levels) for (const auto& renderer : level.renderers) {
                            scene::ObjectReference reference{renderer.instanceId, renderer.entity, 0};
                            probe.ObjectReferenceField("renderer", reference);
                        }
                }
            }
        });
        if (probe.found) return true;
    }
    return false;
}

struct PointPlan {
    EditorContext* context = nullptr;
    uint64_t sceneGeneration = 0;
    std::string ownerGuid;
    scene::EntityID ownerId = scene::EntityID::INVALID;
    uint64_t componentLifetime = 0;
    bool waterKind = false;
    bool markerOnly = false;
    PointAuthoring before, after;
    std::vector<NewObject> created;
    std::vector<PoseStamp> dependencies;
    std::vector<PoseStamp> moved;
    std::vector<math::Vector3> movedPositions;
    bool validateConversion = false;
};

scene::GameObject* ResolveOwner(EditorContext& ctx, const OpArgs& args)
{
    return args.GetString("node").empty() ? ctx.GetSelectedGO() : ctx.activeScene->FindByGuid(args.GetString("node"));
}

std::shared_ptr<PointPlan> MakePlan(EditorContext& ctx, const OpArgs& args)
{
    if (!IsControlPointAuthoringAllowed(ctx)) return {};
    if (args.GetString("kind", "water") != "water" && args.GetString("kind") != "spline") return {};
    auto* owner = ResolveOwner(ctx, args);
    const bool waterKind = args.GetString("kind", "water") == "water";
    if (!owner || ctx.IsLocked(owner->GetID())) return {};
    const uint64_t lifetime = waterKind ? ctx.activeScene->GetComponentLifetime<scene::WaterComponent>(owner->GetID())
                                       : ctx.activeScene->GetComponentLifetime<scene::SplineComponent>(owner->GetID());
    if (lifetime == 0) return {};
    auto plan = std::make_shared<PointPlan>();
    plan->context = &ctx; plan->sceneGeneration = ctx.activeScene->GetRenderSceneGeneration();
    plan->ownerGuid = owner->instanceId; plan->ownerId = owner->GetID();
    plan->waterKind = waterKind; plan->componentLifetime = lifetime;
    plan->before = Capture(*owner, waterKind); plan->after = plan->before;
    return plan;
}

void AddDependencies(PointPlan& plan)
{
    auto& scene = *plan.context->activeScene;
    std::unordered_set<uint64_t> known;
    for (const auto& stamp : plan.dependencies) known.insert(static_cast<uint64_t>(stamp.entity.generation) << 32 | stamp.entity.index);
    std::unordered_map<std::string, scene::GameObject*> objects;
    objects.reserve(scene.GameObjectCount());
    for (auto& object : scene.GameObjects()) objects.emplace(object.instanceId, &object);
    CaptureChain(scene, scene.GetGameObject(plan.ownerId), plan.dependencies, &known);
    const auto capture = [&](const auto& bindings) {
        for (const auto& row : bindings)
            if (const auto found = objects.find(row.target.guid); found != objects.end()) CaptureChain(scene, found->second, plan.dependencies, &known);
    };
    capture(plan.before.waterBindings); capture(plan.before.splineBindings);
}

bool ValidCreated(scene::Scene& scene, const NewObject& created)
{
    if (!PoseMatches(scene, created.stamp)) return false;
    auto* object = scene.GetGameObject(created.stamp.entity);
    if (!SameVector(object->transform.position, created.stamp.pose.position)
        || !SameQuaternion(object->transform.rotation, created.stamp.pose.rotation)
        || !SameVector(object->transform.scale, created.stamp.pose.scale)
        || object->name != created.name || !object->activeSelf() || !object->prefabAssetPath.empty()
        || !object->prefabSourceId.empty() || object->runtimeGenerated || object->tag != "Untagged" || object->layer != 0) return false;
    bool extraComponent = false;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (!std::is_same_v<T, scene::TargetPointComponent>) extraComponent |= object->GetComponent<T>() != nullptr;
    });
    if (extraComponent) return false;
    if (created.marker) {
        const auto* marker = object->GetComponent<scene::TargetPointComponent>();
        const scene::TargetPointComponent defaults;
        if (!marker || marker->visible != defaults.visible || !SameFloat(marker->screenSize, defaults.screenSize)
            || !SameFloat(marker->color.x, defaults.color.x) || !SameFloat(marker->color.y, defaults.color.y)
            || !SameFloat(marker->color.z, defaults.color.z) || !SameFloat(marker->color.w, defaults.color.w)) return false;
    }
    return true;
}

bool ApplyPlan(const std::shared_ptr<PointPlan>& plan, bool apply, std::string& reason)
{
    auto& ctx = *plan->context;
    if (!IsControlPointAuthoringAllowed(ctx) || ctx.activeScene->GetRenderSceneGeneration() != plan->sceneGeneration) {
        reason = "制御点のSceneまたは編集状態が変わりました"; return false;
    }
    auto& scene = *ctx.activeScene;
    auto* owner = scene.GetGameObject(plan->ownerId);
    const uint64_t lifetime = plan->markerOnly ? 0 : plan->waterKind
        ? scene.GetComponentLifetime<scene::WaterComponent>(plan->ownerId)
        : scene.GetComponentLifetime<scene::SplineComponent>(plan->ownerId);
    if ((!plan->ownerGuid.empty() && (!owner || owner->instanceId != plan->ownerGuid || ctx.IsLocked(owner->GetID())))
        || lifetime != plan->componentLifetime
        || (!plan->markerOnly && Capture(*owner, plan->waterKind) != (apply ? plan->before : plan->after))) {
        reason = "所有者、Component寿命、接続設定が変更されています"; return false;
    }
    if (!std::all_of(plan->dependencies.begin(), plan->dependencies.end(), [&scene](const auto& stamp) { return PoseMatches(scene, stamp); })) {
        reason = "参照先または親の姿勢が変更されています"; return false;
    }
    for (size_t index = 0; index < plan->moved.size(); ++index) {
        auto expected = plan->moved[index];
        if (!apply) expected.pose.position = plan->movedPositions[index];
        if (!PoseMatches(scene, expected) || ctx.IsLocked(expected.entity)) { reason = "高さを変える点が変更されています"; return false; }
    }
    if (!plan->created.empty()) {
        if (owner && IsInsidePrefabInstance(owner)) { reason = "Prefab instanceの構造編集はasset編集またはunpack後に実行してください"; return false; }
        if (apply) {
            if (!scene.CanCreateGameObjects(plan->created.size())) { reason = "点を作成するEntity容量が不足しています"; return false; }
            std::unordered_set<std::string> occupied;
            occupied.reserve(scene.GameObjectCount() + plan->created.size());
            for (auto& object : scene.GameObjects()) occupied.insert(object.instanceId);
            for (const auto& object : plan->created) if (!occupied.insert(object.guid).second) { reason = "作成するGUIDが既に使われています"; return false; }
        } else {
            std::unordered_set<std::string> createdGuids;
            for (const auto& created : plan->created) createdGuids.insert(created.guid);
            for (const auto& created : plan->created) {
                if (!ValidCreated(scene, created) || ctx.IsLocked(created.stamp.entity)) { reason = "作成した点が編集されています"; return false; }
                const auto* object = scene.GetGameObject(created.stamp.entity);
                for (int index = 0; index < object->GetChildCount(); ++index) {
                    const auto* child = object->GetChild(index);
                    if (!child || !createdGuids.contains(child->instanceId)) {
                        reason = "作成した点の下に別のオブジェクトがあります"; return false;
                    }
                }
            }
            scene::GameObject* probeOwner = owner;
            if (!probeOwner) probeOwner = scene.GetGameObject(plan->created.front().stamp.entity);
            if (HasOtherReferences(scene, *probeOwner, plan->waterKind, plan->created)) { reason = "作成した点が別のconsumerから共有されています"; return false; }
        }
    }
    std::vector<scene::EntityID> made;
    std::unordered_map<std::string, scene::GameObject*> preparedObjects;
    if (apply) {
        for (auto& object : plan->created) {
            auto* created = scene.TryCreateGameObject(object.name);
            if (!created) { scene.DestroyGameObjects(made); reason = "点の作成に失敗しました"; return false; }
            made.push_back(created->GetID()); created->instanceId = object.guid;
            created->transform.position = object.position;
            if (object.marker) created->AddComponent<scene::TargetPointComponent>();
            if (!object.parent.empty()) {
                const auto prepared = preparedObjects.find(object.parent);
                auto* parent = object.parent == plan->ownerGuid ? owner
                    : prepared != preparedObjects.end() ? prepared->second : scene.FindByGuid(object.parent);
                if (!parent || !created->SetParent(parent)) { scene.DestroyGameObjects(made); reason = "点の親設定に失敗しました"; return false; }
            }
            preparedObjects.emplace(object.guid, created);
            object.stamp = CapturePose(scene, *created);
        }
    }
    if (!plan->markerOnly) Write(*owner, plan->waterKind, plan->before, plan->after, apply);
    for (size_t index = 0; index < plan->moved.size(); ++index)
        scene.GetGameObject(plan->moved[index].entity)->transform.position = apply ? plan->movedPositions[index] : plan->moved[index].pose.position;
    scene::FlushWorldTransforms(scene);
    if (apply && plan->validateConversion) {
        bool valid = false;
        if (plan->waterKind) {
            auto points = scene::ResolveWaterControlPointAuthoring(scene, plan->ownerId, *owner->GetComponent<scene::WaterComponent>(), plan->after.runtime);
            valid = points.valid && SameRows(points.points, plan->before.waterPoints, [](const auto& lhs, const auto& rhs) {
                return lhs.id == rhs.id && EquivalentPosition(lhs.localPosition, rhs.localPosition)
                    && SameFloat(lhs.width, rhs.width) && SameFloat(lhs.depth, rhs.depth) && SameFloat(lhs.flowSpeed, rhs.flowSpeed);
            });
        } else {
            auto points = scene::ResolveSplinePointAuthoring(scene, plan->ownerId, *owner->GetComponent<scene::SplineComponent>(), plan->after.runtime);
            valid = points.valid && points.points.size() == plan->before.splinePoints.size();
            for (size_t index = 0; valid && index < points.points.size(); ++index)
                valid = EquivalentPosition(points.points[index].localPosition, plan->before.splinePoints[index]);
        }
        if (!valid) {
            Write(*owner, plan->waterKind, plan->before, plan->after, false);
            scene.DestroyGameObjects(made); reason = "変換前後の数値点が一致しません"; return false;
        }
    }
    if (!apply) {
        std::vector<scene::EntityID> removed;
        for (auto it = plan->created.rbegin(); it != plan->created.rend(); ++it) removed.push_back(it->stamp.entity);
        scene.DestroyGameObjects(removed);
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    reason.clear(); return true;
}

OpResult Commit(const std::shared_ptr<PointPlan>& plan, const std::string& label)
{
    OpResult result;
    if (plan->before == plan->after && plan->created.empty() && plan->moved.empty()) { result.noChange = true; return result; }
    auto command = std::make_unique<CheckedLambdaCommand>(label,
        [plan](std::string& reason) { return ApplyPlan(plan, true, reason); },
        [plan](std::string& reason) { return ApplyPlan(plan, false, reason); });
    std::string reason;
    if (!command->TryExecute(reason)) return OpResult::Err("CONFLICT", std::move(reason));
    result.command = std::move(command);
    OpData targets = OpData::MakeArray();
    for (const auto& object : plan->created) if (object.marker) targets.Push(object.guid);
    result.data.Set("targets", std::move(targets));
    return result;
}

scene::ObjectReference MakeReference(const NewObject& object)
{
    scene::ObjectReference reference; reference.guid = object.guid; return reference;
}

NewObject& AddContainer(PointPlan& plan)
{
    NewObject container; container.parent = plan.ownerGuid; container.name = "ControlPoints";
    plan.created.push_back(std::move(container)); return plan.created.back();
}

NewObject& AddPoint(PointPlan& plan, const std::string& parent, math::Vector3 position, uint32_t id)
{
    NewObject point; point.parent = parent; point.name = "Point_" + std::to_string(id);
    point.marker = true; point.position = position;
    plan.created.push_back(std::move(point)); return plan.created.back();
}

OpResult Convert(OpContext& context, const OpArgs& args)
{
    auto plan = MakePlan(context.ctx, args);
    if (!plan) return OpResult::Err("NO_OWNER", "WaterまたはSplineの所有者が必要です");
    const auto desired = args.GetString("source") == "POINT_OBJECTS" ? scene::ControlPointSourceMode::POINT_OBJECTS : scene::ControlPointSourceMode::INLINE;
    if (plan->before.source == desired) { OpResult result; result.noChange = true; return result; }
    auto& scene = *context.ctx.activeScene;
    auto* owner = scene.GetGameObject(plan->ownerId);
    if (!FiniteChain(owner)) return OpResult::Err("INVALID_TRANSFORM", "所有者のTransformが不正です");
    scene::FlushWorldTransforms(scene);
    AddDependencies(*plan);
    std::vector<scene::WaterSplinePoint> waterPoints;
    std::vector<scene::SplineControlPointValue> splinePoints;
    if (plan->waterKind) {
        auto candidate = *owner->GetComponent<scene::WaterComponent>();
        if (candidate.surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE && desired == scene::ControlPointSourceMode::POINT_OBJECTS && !candidate.splinePoints.empty())
            candidate.lakePlaneHeight = candidate.splinePoints.front().localPosition.y;
        auto resolved = scene::ResolveWaterControlPointAuthoring(scene, plan->ownerId, candidate, scene::ControlPointRuntimeMode::LIVE);
        if (!resolved.valid) return OpResult::Err("INVALID_POINTS", "Waterの全点を有効な数値点へ解決できません");
        waterPoints = std::move(resolved.points); plan->after.lakeHeight = candidate.lakePlaneHeight;
    } else {
        auto candidate = *owner->GetComponent<scene::SplineComponent>();
        auto resolved = scene::ResolveSplinePointAuthoring(scene, plan->ownerId, candidate, scene::ControlPointRuntimeMode::LIVE);
        if (!resolved.valid) return OpResult::Err("INVALID_POINTS", "Splineの全点を有効な数値点へ解決できません");
        splinePoints = std::move(resolved.points);
    }
    plan->after.source = desired; plan->after.runtime = scene::ControlPointRuntimeMode::LIVE;
    if (desired == scene::ControlPointSourceMode::INLINE) {
        plan->after.waterPoints = std::move(waterPoints);
        plan->after.splinePoints.clear();
        for (const auto& point : splinePoints) plan->after.splinePoints.push_back(point.localPosition);
        plan->after.waterBindings.clear(); plan->after.splineBindings.clear();
    } else {
        if (IsInsidePrefabInstance(owner)) return OpResult::Err("PREFAB_STRUCTURE", "Prefab asset編集またはunpack後に点を展開してください");
        const std::string parent = AddContainer(*plan).guid;
        plan->after.waterBindings.clear(); plan->after.splineBindings.clear();
        for (const auto& value : waterPoints) {
            const auto target = MakeReference(AddPoint(*plan, parent, value.localPosition, value.id));
            plan->after.waterBindings.push_back({value.id, target, value.width, value.depth, value.flowSpeed});
        }
        for (const auto& value : splinePoints) {
            const auto target = MakeReference(AddPoint(*plan, parent, value.localPosition, value.id));
            plan->after.splineBindings.push_back({value.id, target});
        }
        plan->after.waterPoints.clear(); plan->after.splinePoints.clear(); plan->validateConversion = true;
    }
    return Commit(plan, "Convert Control Points");
}

template<typename T, typename GetId>
uint32_t NextUnusedId(const std::vector<T>& bindings, GetId getId)
{
    std::vector<bool> used(bindings.size() + 2, false);
    for (const auto& row : bindings) if (getId(row) < used.size()) used[getId(row)] = true;
    for (uint32_t id = 1; id < used.size(); ++id) if (!used[id]) return id;
    return 0;
}

template<typename T>
uint32_t NextId(const std::vector<T>& bindings)
{
    return NextUnusedId(bindings, [](const auto& row) { return row.id; });
}

OpResult Add(OpContext& context, const OpArgs& args)
{
    auto plan = MakePlan(context.ctx, args);
    if (!plan || plan->before.source != scene::ControlPointSourceMode::POINT_OBJECTS)
        return OpResult::Err("POINT_OBJECTS_REQUIRED", "TargetPointを位置ソースにする所有者が必要です");
    auto* owner = context.ctx.activeScene->GetGameObject(plan->ownerId);
    if (IsInsidePrefabInstance(owner)) return OpResult::Err("PREFAB_STRUCTURE", "Prefab asset編集またはunpack後に点を追加してください");
    if (!FiniteChain(owner)) return OpResult::Err("INVALID_TRANSFORM", "所有者のTransformが不正です");
    math::Vector3 position = args.GetVec3("localPosition");
    if (!Finite(position)) return OpResult::Err("BAD_POSITION", "位置は有限なowner-local座標です");
    const size_t count = plan->waterKind ? plan->after.waterBindings.size() : plan->after.splineBindings.size();
    const size_t limit = plan->waterKind ? scene::WATER_SPLINE_MAX_CONTROL_POINTS : scene::MAX_CONTROL_POINT_BINDINGS;
    if (count >= limit) return OpResult::Err("LIMIT", "制御点の上限を越えています");
    if (plan->waterKind && owner->GetComponent<scene::WaterComponent>()->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE) {
        if (std::abs(position.y - plan->after.lakeHeight) > 1.0e-4f) return OpResult::Err("LAKE_PLANE", "湖の点はSurface Heightと同じlocal Yを指定してください");
    }
    const uint32_t id = plan->waterKind ? NextId(plan->after.waterBindings) : NextId(plan->after.splineBindings);
    std::string parent;
    const auto& firstGuid = plan->waterKind && !plan->before.waterBindings.empty() ? plan->before.waterBindings.front().target.guid
        : !plan->before.splineBindings.empty() ? plan->before.splineBindings.front().target.guid : std::string{};
    if (auto* first = context.ctx.activeScene->FindByGuid(firstGuid)) {
        auto* container = first->GetParent();
        const scene::Transform identity;
        if (container == owner) parent = owner->instanceId;
        else if (container && container->GetParent() == owner && SamePose(container->transform, identity)
                 && !container->GetComponent<scene::TargetPointComponent>() && !context.ctx.IsLocked(container->GetID())) {
            parent = container->instanceId; CaptureChain(*context.ctx.activeScene, container, plan->dependencies);
        }
    }
    if (parent.empty()) parent = AddContainer(*plan).guid;
    const auto target = MakeReference(AddPoint(*plan, parent, position, id));
    if (plan->waterKind) {
        const float width = args.GetFloat("width", 6.0f), depth = args.GetFloat("depth", 1.0f), flow = args.GetFloat("flowSpeed", 0.0f);
        if (!std::isfinite(width) || !std::isfinite(depth) || !std::isfinite(flow) || width <= 0.0f || depth < 0.0f)
            return OpResult::Err("BAD_ATTRIBUTES", "幅は正、水深は非負、流速は有限値です");
        plan->after.waterBindings.push_back({id, target, width, depth, flow});
    } else plan->after.splineBindings.push_back({id, target});
    AddDependencies(*plan);
    auto result = Commit(plan, "Add Control Point"); result.data.Set("id", id); return result;
}

OpResult EditRow(OpContext& context, const OpArgs& args, const std::string& action)
{
    auto plan = MakePlan(context.ctx, args);
    if (!plan || plan->before.source != scene::ControlPointSourceMode::POINT_OBJECTS)
        return OpResult::Err("POINT_OBJECTS_REQUIRED", "TargetPointを位置ソースにする所有者が必要です");
    const uint32_t id = static_cast<uint32_t>(args.GetInt("id"));
    scene::ObjectReference target;
    if (action == "bind") {
        auto* object = context.ctx.activeScene->FindByGuid(args.GetString("target"));
        if (!object || !object->GetComponent<scene::TargetPointComponent>() || !object->activeInHierarchy() || !FiniteChain(object))
            return OpResult::Err("NO_TARGET", "同じSceneの有効なTargetPointが必要です");
        target.guid = object->instanceId;
    }
    const auto edit = [&](auto& bindings) -> bool {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [id](const auto& row) { return row.id == id; });
        if (found == bindings.end()) return false;
        if (action == "bind" && std::any_of(bindings.begin(), bindings.end(), [id, &target](const auto& row) { return row.id != id && row.target.guid == target.guid; })) return false;
        if (action == "remove") bindings.erase(found);
        else if (action == "unbind" || action == "bind") found->target = target;
        else if (action == "reorder") {
            const int to = args.GetInt("toIndex", -1);
            if (to < 0 || static_cast<size_t>(to) >= bindings.size()) return false;
            auto moved = *found; bindings.erase(found); bindings.insert(bindings.begin() + to, std::move(moved));
        }
        return true;
    };
    if (!(plan->waterKind ? edit(plan->after.waterBindings) : edit(plan->after.splineBindings)))
        return OpResult::Err("BAD_ROW", "binding IDまたは移動先indexが不正です");
    return Commit(plan, "Edit Control Point Binding");
}

OpResult Attributes(OpContext& context, const OpArgs& args)
{
    auto plan = MakePlan(context.ctx, args);
    if (!plan || !plan->waterKind || plan->before.source != scene::ControlPointSourceMode::POINT_OBJECTS)
        return OpResult::Err("WATER_REQUIRED", "TargetPointを使うWaterが必要です");
    const uint32_t id = static_cast<uint32_t>(args.GetInt("id"));
    const auto row = std::find_if(plan->after.waterBindings.begin(), plan->after.waterBindings.end(), [id](const auto& value) { return value.id == id; });
    if (row == plan->after.waterBindings.end()) return OpResult::Err("BAD_ROW", "binding IDが見つかりません");
    if (args.Has("width")) row->width = args.GetFloat("width");
    if (args.Has("depth")) row->depth = args.GetFloat("depth");
    if (args.Has("flowSpeed")) row->flowSpeed = args.GetFloat("flowSpeed");
    const bool river = context.ctx.activeScene->GetGameObject(plan->ownerId)->GetComponent<scene::WaterComponent>()->surfaceShape == scene::WaterSurfaceShape::SPLINE_RIVER;
    if (!std::isfinite(row->width) || !std::isfinite(row->depth) || !std::isfinite(row->flowSpeed) || (river && (row->width <= 0.0f || row->depth < 0.0f)))
        return OpResult::Err("BAD_ATTRIBUTES", "幅は正、水深は非負、流速は有限値です");
    return Commit(plan, "Set Water Point Attributes");
}

OpResult Bake(OpContext& context, const OpArgs& args)
{
    auto plan = MakePlan(context.ctx, args);
    if (!plan) return OpResult::Err("NO_OWNER", "WaterまたはSplineの所有者が必要です");
    auto& scene = *context.ctx.activeScene;
    if (!FiniteChain(scene.GetGameObject(plan->ownerId))) return OpResult::Err("INVALID_TRANSFORM", "所有者のTransformが不正です");
    scene::FlushWorldTransforms(scene); AddDependencies(*plan);
    auto* owner = scene.GetGameObject(plan->ownerId);
    if (plan->waterKind) {
        auto result = scene::ResolveWaterControlPointAuthoring(scene, plan->ownerId, *owner->GetComponent<scene::WaterComponent>(), scene::ControlPointRuntimeMode::BAKED_STATIC);
        if (!result.valid) return OpResult::Err("INVALID_POINTS", "現在の点をベイクできません");
        plan->after.bakedLakeHeight = plan->before.source == scene::ControlPointSourceMode::INLINE
            && plan->before.waterShape == scene::WaterSurfaceShape::SPLINE_LAKE && !result.points.empty()
            ? result.points.front().localPosition.y : plan->before.lakeHeight;
        plan->after.waterBake = std::move(result.points); plan->after.bakeSignature = result.inputSignature;
        plan->after.bakedWaterShape = plan->before.waterShape;
    } else {
        auto result = scene::ResolveSplinePointAuthoring(scene, plan->ownerId, *owner->GetComponent<scene::SplineComponent>(), scene::ControlPointRuntimeMode::BAKED_STATIC);
        if (!result.valid) return OpResult::Err("INVALID_POINTS", "現在の点をベイクできません");
        plan->after.splineBake = std::move(result.points); plan->after.bakeSignature = result.inputSignature;
        plan->after.bakedSplineClosed = plan->before.splineClosed;
    }
    plan->after.bakeSource = plan->after.source; plan->after.bakeVersion = scene::CONTROL_POINT_ADAPTER_VERSION;
    plan->after.bakeValid = true; plan->after.runtime = scene::ControlPointRuntimeMode::BAKED_STATIC;
    return Commit(plan, "Bake Control Points");
}

OpResult LakeHeight(OpContext& context, const OpArgs& args)
{
    auto plan = MakePlan(context.ctx, args);
    if (!plan || !plan->waterKind || plan->before.source != scene::ControlPointSourceMode::POINT_OBJECTS)
        return OpResult::Err("LAKE_REQUIRED", "TargetPointを使う湖が必要です");
    auto& scene = *context.ctx.activeScene;
    auto* owner = scene.GetGameObject(plan->ownerId);
    auto* water = owner->GetComponent<scene::WaterComponent>();
    const float height = args.GetFloat("height");
    if (water->surfaceShape != scene::WaterSurfaceShape::SPLINE_LAKE || !std::isfinite(height))
        return OpResult::Err("LAKE_REQUIRED", "湖と有限なowner-local高さが必要です");
    if (!FiniteChain(owner)) return OpResult::Err("INVALID_TRANSFORM", "所有者のTransformが不正です");
    scene::FlushWorldTransforms(scene);
    std::vector<NewObject> targets;
    for (const auto& row : plan->before.waterBindings) {
        auto* point = scene.FindByGuid(row.target.guid);
        if (!point || !point->GetComponent<scene::TargetPointComponent>() || !point->IsDescendantOf(*owner) || context.ctx.IsLocked(point->GetID()) || !FiniteChain(point))
            return OpResult::Err("EXTERNAL_TARGET", "高さ変更には所有者配下の解決済み点だけを使えます");
        if (std::any_of(targets.begin(), targets.end(), [point](const auto& target) { return target.guid == point->instanceId; }))
            return OpResult::Err("DUPLICATE_TARGET", "湖の同じ点を複数の行へ接続できません");
        NewObject probe; probe.guid = point->instanceId; probe.stamp = CapturePose(scene, *point); targets.push_back(std::move(probe));
    }
    if (HasOtherReferences(scene, *owner, true, targets)) return OpResult::Err("SHARED_TARGET", "共有点の一括高さ変更はできません");
    for (const auto& target : targets) {
        auto* point = scene.GetGameObject(target.stamp.entity);
        for (auto* parent = point->GetParent(); parent && parent != owner; parent = parent->GetParent())
            if (std::any_of(targets.begin(), targets.end(), [parent](const auto& row) { return row.guid == parent->instanceId; }))
                return OpResult::Err("ANCESTOR_TARGET", "接続点同士が親子になっている湖の高さは一括変更できません");
    }
    auto references = plan->before.waterBindings;
    std::vector<scene::ControlPointBinding> bindings;
    for (auto& row : references) bindings.push_back({row.id, &row.target});
    std::vector<scene::ResolvedControlPoint> resolved;
    scene::ControlPointResolveStatus status;
    uint64_t signature = 0;
    if (!scene::ResolveControlPointBindings(scene, owner->GetID(), bindings, true, resolved, status, signature))
        return OpResult::Err("INVALID_TRANSFORM", "湖の全点をowner-local位置へ解決できません");
    for (size_t index = 0; index < targets.size(); ++index) {
        const auto& target = targets[index];
        auto* point = scene.GetGameObject(target.stamp.entity);
        auto* parent = point->GetParent();
        CaptureChain(scene, parent, plan->dependencies);
        const auto scale = parent->transform.worldScale;
        if (!Finite(scale) || std::abs(scale.x) <= 1.0e-6f || std::abs(scale.y) <= 1.0e-6f || std::abs(scale.z) <= 1.0e-6f)
            return OpResult::Err("INVALID_TRANSFORM", "点の親scaleが逆変換できません");
        const auto worldDelta = owner->transform.worldRotation * math::Vector3{0.0f,
            (height - resolved[index].ownerLocalPosition.y) * owner->transform.worldScale.y, 0.0f};
        const auto delta = parent->transform.worldRotation.Inverse() * worldDelta;
        const math::Vector3 position = point->transform.position + math::Vector3{delta.x / scale.x, delta.y / scale.y, delta.z / scale.z};
        if (!Finite(position)) return OpResult::Err("INVALID_TRANSFORM", "高さを有限なlocal位置へ戻せません");
        if (position != point->transform.position) { plan->moved.push_back(target.stamp); plan->movedPositions.push_back(position); }
    }
    CaptureChain(scene, owner, plan->dependencies);
    plan->after.lakeHeight = height;
    return Commit(plan, "Set Lake Surface Height");
}

std::vector<scene::ClothAttachment> CaptureAttachments(const scene::ClothComponent& cloth)
{
    auto rows = cloth.attachments;
    for (auto& row : rows) {
        row.targetReference.id = scene::EntityID::INVALID; row.targetReference.sceneGeneration = 0;
        if (row.bindingId != 0) row.target = scene::EntityID::INVALID;
    }
    return rows;
}

bool SameAttachments(const std::vector<scene::ClothAttachment>& lhs, const std::vector<scene::ClothAttachment>& rhs)
{
    return SameRows(lhs, rhs, [](const auto& a, const auto& b) {
        return a.bindingId == b.bindingId && a.particle == b.particle && a.target == b.target
            && a.targetReference.guid == b.targetReference.guid && a.topologySignature == b.topologySignature
            && SameVector(a.localPosition, b.localPosition) && SameFloat(a.maxDistance, b.maxDistance);
    });
}

uint64_t CurrentClothTopology(const scene::ClothComponent& cloth)
{
    const asset::ClothAsset* source = nullptr;
    if (!cloth.clothAssetPath.empty()) {
        source = asset::AssetManager::Get(asset::AssetManager::Load<asset::ClothAsset>(cloth.clothAssetPath));
        if (!source) return 0;
    }
    return cloth.segments >= 2 ? scene::ClothTopologySignature(source, static_cast<uint32_t>(cloth.segments)) : 0;
}

struct ClothBindingPlan {
    EditorContext* context = nullptr;
    uint64_t sceneGeneration = 0;
    std::string ownerGuid;
    scene::EntityID owner = scene::EntityID::INVALID;
    uint64_t lifetime = 0;
    uint64_t topology = 0;
    bool checkTopology = false;
    std::vector<scene::ClothAttachment> before, after;
    std::vector<NewObject> created;
    uint32_t editedBinding = 0;
};

bool ApplyClothBinding(const std::shared_ptr<ClothBindingPlan>& plan, bool apply, std::string& reason)
{
    auto& ctx = *plan->context;
    if (!IsControlPointAuthoringAllowed(ctx) || ctx.activeScene->GetRenderSceneGeneration() != plan->sceneGeneration) {
        reason = "布のSceneまたは編集状態が変わりました"; return false;
    }
    auto* owner = ctx.activeScene->GetGameObject(plan->owner);
    auto* cloth = owner ? owner->GetComponent<scene::ClothComponent>() : nullptr;
    if (!cloth || owner->instanceId != plan->ownerGuid || ctx.IsLocked(plan->owner)
        || ctx.activeScene->GetComponentLifetime<scene::ClothComponent>(plan->owner) != plan->lifetime
        || !SameAttachments(CaptureAttachments(*cloth), apply ? plan->before : plan->after)
        || (plan->checkTopology && CurrentClothTopology(*cloth) != plan->topology)) {
        reason = "布のComponent寿命、接続、topologyが変更されています"; return false;
    }
    auto& scene = *ctx.activeScene;
    if (!plan->created.empty()) {
        if (IsInsidePrefabInstance(owner) || owner->runtimeGenerated || !FiniteChain(owner)) {
            reason = "布の親子構造またはTransformを変更しています"; return false;
        }
        for (auto* ancestor = owner; ancestor; ancestor = ancestor->GetParent()) {
            if (ancestor->runtimeGenerated || ctx.IsLocked(ancestor->GetID())) {
                reason = "固定先を置く親が生成専用またはロックされています"; return false;
            }
        }
        if (apply) {
            if (!scene.CanCreateGameObjects(plan->created.size())) { reason = "固定先を作成する容量がありません"; return false; }
            for (const auto& created : plan->created)
                if (scene.FindByGuid(created.guid)) { reason = "固定先のGUIDが既に使われています"; return false; }
        } else {
            for (const auto& created : plan->created) {
                auto* point = scene.GetGameObject(created.stamp.entity);
                if (!ValidCreated(scene, created) || ctx.IsLocked(created.stamp.entity)
                    || !point || point->GetParent() != owner || point->GetChildCount() != 0) {
                    reason = "作成した固定先が編集されています"; return false;
                }
            }
            if (HasOtherReferences(scene, *owner, false, plan->created, plan->editedBinding)) {
                reason = "作成した固定先が別の接続から共有されています"; return false;
            }
        }
    }
    if (apply && !plan->created.empty()) {
        auto& created = plan->created.front();
        auto* point = scene.TryCreateGameObject(created.name);
        if (!point) { reason = "固定先を作成する容量がありません"; return false; }
        const auto pointId = point->GetID(); point->instanceId = created.guid;
        owner = scene.GetGameObject(plan->owner);
        cloth = owner ? owner->GetComponent<scene::ClothComponent>() : nullptr;
        if (!owner || !cloth || owner->instanceId != plan->ownerGuid
            || scene.GetComponentLifetime<scene::ClothComponent>(plan->owner) != plan->lifetime
            || !SameAttachments(CaptureAttachments(*cloth), plan->before) || !point->SetParent(owner)) {
            scene.DestroyGameObject(pointId); reason = "生成中にClothの所有者または親子構造が変更されました"; return false;
        }
        point->transform.position = created.position;
        point->AddComponent<scene::TargetPointComponent>(); created.stamp = CapturePose(scene, *point);
        owner = scene.GetGameObject(plan->owner); cloth = owner ? owner->GetComponent<scene::ClothComponent>() : nullptr;
        if (!cloth || owner->instanceId != plan->ownerGuid
            || scene.GetComponentLifetime<scene::ClothComponent>(plan->owner) != plan->lifetime) {
            scene.DestroyGameObject(pointId); reason = "生成中にClothの寿命が変更されました"; return false;
        }
    }
    cloth->attachments = apply ? plan->after : plan->before;
    if (!apply && !plan->created.empty()) scene.DestroyGameObject(plan->created.front().stamp.entity);
    if (!plan->created.empty()) scene::FlushWorldTransforms(scene);
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    reason.clear(); return true;
}

std::shared_ptr<ClothBindingPlan> MakeClothBindingPlan(EditorContext& ctx, scene::GameObject& owner)
{
    auto plan = std::make_shared<ClothBindingPlan>();
    plan->context = &ctx; plan->sceneGeneration = ctx.activeScene->GetRenderSceneGeneration();
    plan->ownerGuid = owner.instanceId; plan->owner = owner.GetID();
    plan->lifetime = ctx.activeScene->GetComponentLifetime<scene::ClothComponent>(plan->owner);
    plan->before = CaptureAttachments(*owner.GetComponent<scene::ClothComponent>()); plan->after = plan->before;
    return plan;
}

/// @note 作成と既存先への接続で同じ粒子・topology・属性検証を使い、Scene変更前にplanを完成させる。
OpResult PrepareClothBinding(ClothBindingPlan& plan, const scene::ClothComponent& cloth,
    const OpArgs& args, const std::string& targetGuid, bool bind)
{
    const int particle = args.GetInt("particle", -1);
    if (args.Has("id") && args.GetInt("id") < 0) return OpResult::Err("BAD_ROW", "binding IDは非負の整数です");
    const uint32_t id = args.GetInt("id") > 0 ? static_cast<uint32_t>(args.GetInt("id")) : 0;
    auto row = std::find_if(plan.after.begin(), plan.after.end(), [id, particle](const auto& value) {
        return id != 0 ? value.bindingId == id : value.particle == particle;
    });
    plan.editedBinding = row != plan.after.end() ? row->bindingId : 0;
    if (!bind) {
        if (row == plan.after.end()) return OpResult::Err("BAD_ROW", "解除するbinding IDまたはparticleが見つかりません");
        plan.after.erase(row);
    } else {
        const asset::ClothAsset* source = nullptr;
        if (!cloth.clothAssetPath.empty()) {
            source = asset::AssetManager::Get(asset::AssetManager::Load<asset::ClothAsset>(cloth.clothAssetPath));
            if (!source) return OpResult::Err("NO_CLOTH_ASSET", "Cloth Assetを読み込めません");
        }
        uint32_t count = 0;
        if (!args.Has("particle") || cloth.segments < 2 || !scene::ResolveClothTopologyForParticle(source,
            static_cast<uint32_t>(cloth.segments), particle, plan.topology, count))
            return OpResult::Err("BAD_PARTICLE", "現在のCloth topologyにあるparticleを指定してください");
        plan.checkTopology = true;
        if (std::any_of(plan.after.begin(), plan.after.end(), [&row, &plan, particle](const auto& value) {
            return value.particle == particle && (row == plan.after.end() || &value != &*row);
        })) return OpResult::Err("DUPLICATE_PARTICLE", "同じ物理質点を複数の固定先へ接続できません");
        const auto offset = args.GetVec3("localPosition");
        const float radius = args.GetFloat("maxDistance", row != plan.after.end() ? row->maxDistance : 0.0f);
        if (!Finite(offset) || !std::isfinite(radius) || radius < 0.0f) return OpResult::Err("BAD_ATTRIBUTES", "offsetはtarget-local、maxDistanceは非負のworld mです");
        if (row == plan.after.end()) {
            if (id != 0) return OpResult::Err("BAD_ROW", "明示したbinding IDが見つかりません");
            if (plan.after.size() >= scene::MAX_CONTROL_POINT_BINDINGS) return OpResult::Err("LIMIT", "接続数の上限を越えています");
            scene::ClothAttachment attachment; attachment.particle = particle;
            plan.after.push_back(attachment); row = std::prev(plan.after.end());
        }
        if (row->bindingId == 0) {
            row->bindingId = NextUnusedId(plan.after, [](const auto& value) { return value.bindingId; });
        }
        row->particle = particle; row->target = scene::EntityID::INVALID; row->targetReference.guid = targetGuid;
        row->targetReference.id = scene::EntityID::INVALID; row->targetReference.sceneGeneration = 0;
        row->localPosition = offset; row->maxDistance = radius; row->topologySignature = plan.topology;
        plan.editedBinding = row->bindingId;
    }
    return OpResult::Ok();
}

OpResult CommitClothBinding(const std::shared_ptr<ClothBindingPlan>& plan)
{
    OpResult result;
    if (SameAttachments(plan->before, plan->after) && plan->created.empty()) { result.noChange = true; return result; }
    auto command = std::make_unique<CheckedLambdaCommand>("Edit Cloth Target Binding",
        [plan](std::string& reason) { return ApplyClothBinding(plan, true, reason); },
        [plan](std::string& reason) { return ApplyClothBinding(plan, false, reason); });
    std::string reason;
    if (!command->TryExecute(reason)) return OpResult::Err("CONFLICT", std::move(reason));
    result.data.Set("id", plan->editedBinding);
    if (!plan->created.empty()) result.data.Set("target", plan->created.front().guid);
    result.command = std::move(command); return result;
}

/// @note 新規または明示rebindだけに現在topologyを刻む。旧接続の読み込みではverifiedにしない。
OpResult EditClothBinding(OpContext& context, const OpArgs& args, bool bind)
{
    auto& ctx = context.ctx;
    auto* owner = ResolveOwner(ctx, args);
    auto* cloth = owner ? owner->GetComponent<scene::ClothComponent>() : nullptr;
    if (!cloth || ctx.IsLocked(owner->GetID())) return OpResult::Err("NO_CLOTH", "Cloth所有者が必要です");
    auto plan = MakeClothBindingPlan(ctx, *owner);
    std::string targetGuid;
    if (bind) {
        auto* target = ctx.activeScene->FindByGuid(args.GetString("target"));
        if (!target || !target->activeInHierarchy() || !FiniteChain(target)) return OpResult::Err("NO_TARGET", "同じSceneの有効なGameObjectが必要です");
        targetGuid = target->instanceId;
    }
    auto prepared = PrepareClothBinding(*plan, *cloth, args, targetGuid, bind);
    return prepared.ok ? CommitClothBinding(plan) : std::move(prepared);
}

OpResult CreateClothAnchor(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    auto* owner = ResolveOwner(ctx, args);
    auto* cloth = owner ? owner->GetComponent<scene::ClothComponent>() : nullptr;
    if (!cloth || owner->runtimeGenerated || !owner->activeInHierarchy() || ctx.IsLocked(owner->GetID()))
        return OpResult::Err("NO_CLOTH", "編集できる有効なCloth所有者が必要です");
    if (IsInsidePrefabInstance(owner)) return OpResult::Err("PREFAB_STRUCTURE", "Prefab asset編集またはunpack後に固定先を作成してください");
    if (!FiniteChain(owner)) return OpResult::Err("INVALID_TRANSFORM", "Clothの親Transformが不正です");
    const auto markerPosition = args.GetVec3("markerPosition");
    if (!Finite(markerPosition)) return OpResult::Err("BAD_POSITION", "固定先の位置は有限なowner-local座標です");
    auto plan = MakeClothBindingPlan(ctx, *owner);
    NewObject marker; marker.marker = true; marker.parent = owner->instanceId; marker.position = markerPosition;
    auto prepared = PrepareClothBinding(*plan, *cloth, args, marker.guid, true);
    if (!prepared.ok) return prepared;
    marker.name = args.GetString("name", "ClothAnchor_" + std::to_string(plan->editedBinding));
    if (marker.name.empty()) return OpResult::Err("BAD_NAME", "固定先の名前が必要です");
    plan->created.push_back(std::move(marker));
    return CommitClothBinding(plan);
}

OpParam Param(std::string name, OpParamType type, std::string desc, bool required = true)
{
    OpParam param; param.name = std::move(name); param.type = type; param.desc = std::move(desc); param.required = required; return param;
}

std::vector<OpParam> OwnerParams()
{
    auto kind = Param("kind", OpParamType::String, "利用側のComponent", false);
    kind.enumValues = {"water", "spline", "cloth", "patrol", "line", "nav_link"}; kind.defaultValue = std::string("water");
    return {Param("node", OpParamType::NodeId, "所有者のGUID。省略すると選択中", false), kind};
}

void RegisterMutation(OperatorRegistry& registry, const std::string& id, const std::string& label,
                      std::vector<OpParam> params, OpExec exec)
{
    EditorOperator op; op.id = id; op.label = label; op.category = "Control Points"; op.desc = label;
    op.kind = OpKind::Mutation; op.undoLabel = label; op.params = std::move(params);
    op.exec = [id, original = std::move(exec)](OpContext& context, const OpArgs& args) {
        const std::string kind = args.GetString("kind");
        if (kind == "patrol" || kind == "line" || kind == "nav_link")
            return InvokeAdditionalControlPointOperation(context, id, args);
        return original(context, args);
    };
    op.poll = [](const OpContext& context, const OpArgs&) { return IsControlPointAuthoringAllowed(context.ctx); };
    registry.Register(std::move(op));
}

} /// @note namespace

void RegisterControlPointOperators(OperatorRegistry& registry)
{
    auto params = OwnerParams();
    auto source = Param("source", OpParamType::String, "新しい位置ソース"); source.enumValues = {"INLINE", "POINT_OBJECTS"}; params.push_back(source);
    RegisterMutation(registry, "control_points.convert", "Convert Control Points", std::move(params), Convert);
    params = OwnerParams(); params[1].enumValues = {"cloth"}; params[1].defaultValue = std::string("cloth");
    params.push_back(Param("particle", OpParamType::Int, "固定するClothの現在の物理質点番号"));
    params.push_back(Param("markerPosition", OpParamType::Vec3, "新規TargetPointのCloth owner-local位置", false));
    params.push_back(Param("localPosition", OpParamType::Vec3, "target-local拘束offset。省略すると原点", false));
    params.push_back(Param("maxDistance", OpParamType::Float, "非負のworld拘束半径[m]", false));
    params.push_back(Param("id", OpParamType::Int, "明示rebindするbinding ID。省略するとparticleの既存行または新しい行", false));
    params.push_back(Param("name", OpParamType::String, "新規固定先の表示名", false));
    RegisterMutation(registry, "control_points.create_cloth_anchor", "Create And Bind Cloth Anchor", std::move(params), CreateClothAnchor);
    params = OwnerParams(); params.push_back(Param("localPosition", OpParamType::Vec3, "所有者のlocal座標。worldPositionと排他", false));
    params.push_back(Param("worldPosition", OpParamType::Vec3, "world点列用の絶対位置。localPositionと排他", false));
    params.push_back(Param("id", OpParamType::Int, "NavLinkの固定端点ID: 1=起点、2=終点", false));
    for (const auto* field : {"width", "depth", "flowSpeed"}) params.push_back(Param(field, OpParamType::Float, "Waterの点属性", false));
    RegisterMutation(registry, "control_points.add", "Add Control Point", std::move(params), Add);
    for (const auto* action : {"remove", "reorder", "unbind", "bind"}) {
        const bool clothCapable = std::string(action) == "bind" || std::string(action) == "unbind";
        params = OwnerParams(); params.push_back(Param("id", OpParamType::Int, "安定したbinding ID", !clothCapable));
        if (clothCapable) params.push_back(Param("particle", OpParamType::Int, "Clothの現在の物理質点番号。bind時必須", false));
        if (std::string(action) == "reorder") params.push_back(Param("toIndex", OpParamType::Int, "移動先の0始まりindex"));
        if (std::string(action) == "bind") params.push_back(Param("target", OpParamType::NodeId, "同じSceneのTargetPoint GUID"));
        if (std::string(action) == "bind") {
            params.push_back(Param("localPosition", OpParamType::Vec3, "Cloth target-local offset。省略すると原点", false));
            params.push_back(Param("maxDistance", OpParamType::Float, "Clothのworld拘束半径[m]", false));
        }
        RegisterMutation(registry, "control_points." + std::string(action), "Edit Control Point Binding", std::move(params),
            [action = std::string(action)](OpContext& context, const OpArgs& args) {
                if (args.GetString("kind") == "cloth" && (action == "bind" || action == "unbind")) return EditClothBinding(context, args, action == "bind");
                return EditRow(context, args, action);
            });
    }
    params = OwnerParams(); params.push_back(Param("id", OpParamType::Int, "安定したbinding ID"));
    for (const auto* field : {"width", "depth", "flowSpeed"}) params.push_back(Param(field, OpParamType::Float, "変更するWater属性", false));
    RegisterMutation(registry, "control_points.set_water_attributes", "Set Water Point Attributes", std::move(params), Attributes);
    params = OwnerParams(); params.push_back(Param("id", OpParamType::Int, "巡回行の安定ID"));
    params.push_back(Param("waitTime", OpParamType::Float, "到達後の待機秒数", false));
    params.push_back(Param("speed", OpParamType::Float, "world速度[m/s]。0はAgent既定値", false));
    RegisterMutation(registry, "control_points.set_patrol_attributes", "Set Patrol Point Attributes", std::move(params),
        [](OpContext& context, const OpArgs& args) {
            OpArgs values = args;
            if (!values.Has("kind")) values.Set("kind", std::string{"patrol"});
            return InvokeAdditionalControlPointOperation(context, "control_points.set_patrol_attributes", values);
        });
    params = OwnerParams(); params.push_back(Param("height", OpParamType::Float, "所有者のlocal平面高さ"));
    RegisterMutation(registry, "control_points.set_lake_height", "Set Lake Surface Height", std::move(params), LakeHeight);
    RegisterMutation(registry, "control_points.bake", "Bake Control Points", OwnerParams(), Bake);
    RegisterMutation(registry, "control_points.set_runtime", "Set Control Point Runtime Mode", [&] {
        auto values = OwnerParams(); auto mode = Param("mode", OpParamType::String, "点の実行時追従"); mode.enumValues = {"LIVE", "BAKED_STATIC"}; values.push_back(mode); return values;
    }(), [](OpContext& context, const OpArgs& args) {
        auto plan = MakePlan(context.ctx, args);
        if (!plan) return OpResult::Err("NO_OWNER", "WaterまたはSplineの所有者が必要です");
        if (args.GetString("mode") == "BAKED_STATIC" && !plan->before.bakeValid) return OpResult::Err("MISSING_BAKE", "先に現在の点をBakeしてください");
        plan->after.runtime = args.GetString("mode") == "LIVE" ? scene::ControlPointRuntimeMode::LIVE : scene::ControlPointRuntimeMode::BAKED_STATIC;
        return Commit(plan, "Set Control Point Runtime Mode");
    });
    params = {Param("parent", OpParamType::NodeId, "親GUID。省略するとSceneのroot", false),
              Param("localPosition", OpParamType::Vec3, "親のlocal座標", false), Param("name", OpParamType::String, "点の表示名", false)};
    RegisterMutation(registry, "target_point.create", "Create Target Point", std::move(params), [](OpContext& context, const OpArgs& args) {
        auto& ctx = context.ctx; const std::string parentGuid = args.GetString("parent");
        auto* parent = parentGuid.empty() ? nullptr : ctx.activeScene->FindByGuid(parentGuid);
        if ((!parentGuid.empty() && !parent) || (parent && ctx.IsLocked(parent->GetID()))) return OpResult::Err("NO_PARENT", "親が欠損またはロックされています");
        if (IsInsidePrefabInstance(parent)) return OpResult::Err("PREFAB_STRUCTURE", "Prefab asset編集またはunpack後に作成してください");
        if (parent && !FiniteChain(parent)) return OpResult::Err("INVALID_TRANSFORM", "親のTransformが不正です");
        const auto position = args.GetVec3("localPosition");
        if (!Finite(position)) return OpResult::Err("BAD_POSITION", "位置は有限なlocal座標です");
        auto plan = std::make_shared<PointPlan>(); plan->context = &ctx;
        plan->sceneGeneration = ctx.activeScene->GetRenderSceneGeneration(); plan->markerOnly = true;
        if (parent) { plan->ownerGuid = parentGuid; plan->ownerId = parent->GetID(); CaptureChain(*ctx.activeScene, parent, plan->dependencies); }
        auto& point = AddPoint(*plan, parentGuid, position, 1); point.name = args.GetString("name", "TargetPoint");
        return Commit(plan, "Create Target Point");
    });
    EditorOperator edit; edit.id = "control_points.edit"; edit.label = "Edit Control Points"; edit.category = "Control Points";
    edit.desc = "所有者の点編集を開始または終了する"; edit.kind = OpKind::Action;
    edit.params = {Param("owner", OpParamType::NodeId, "所有者GUID。空なら点編集を終了", false)};
    edit.poll = [](const OpContext& context, const OpArgs&) { return IsControlPointAuthoringAllowed(context.ctx); };
    edit.exec = [](OpContext& context, const OpArgs& args) {
        const std::string guid = args.GetString("owner");
        auto* owner = guid.empty() ? nullptr : context.ctx.activeScene->FindByGuid(guid);
        if (!guid.empty() && (!owner || (!owner->GetComponent<scene::WaterComponent>() && !owner->GetComponent<scene::SplineComponent>()
            && !owner->GetComponent<scene::NavMeshPatrolComponent>() && !owner->GetComponent<scene::NavMeshOffMeshLinkComponent>()
            && !owner->GetComponent<scene::LineRendererComponent>()) || context.ctx.IsLocked(owner->GetID())))
            return OpResult::Err("NO_OWNER", "点列または端点を持つ所有者が必要です");
        if (!guid.empty()) {
            context.ctx.clothPinPainting = false;
            if (context.ctx.mapEditingMode) context.ctx.mapActiveTool = EditorContext::MapTool::Grid;
            if (context.ctx.terrainTool) context.ctx.terrainTool->SetActive(false);
            context.ctx.gizmoMode = EditorContext::GizmoMode::Translate;
            context.ctx.vertexSnapActive = false; context.ctx.surfaceSnapActive = false;
        }
        context.ctx.controlPointOwnerGuid = guid; return OpResult::Ok();
    };
    registry.Register(std::move(edit));
    EditorOperator select; select.id = "control_points.select"; select.label = "Select Point Target"; select.category = "Control Points";
    select.desc = "接続先をHierarchyで選択する。Clothは通常GameObjectも選択可能"; select.kind = OpKind::Action;
    select.params = {Param("target", OpParamType::NodeId, "選択する点または固定先GUID")};
    select.poll = [](const OpContext& context, const OpArgs&) { return IsControlPointAuthoringAllowed(context.ctx); };
    select.exec = [](OpContext& context, const OpArgs& args) {
        auto* target = context.ctx.activeScene->FindByGuid(args.GetString("target"));
        if (!target) return OpResult::Err("NO_TARGET", "選択する接続先が見つかりません");
        SelectEntity(context.ctx, target->GetID()); return OpResult::Ok();
    };
    registry.Register(std::move(select));
}
} /// @note namespace fbzz::editor
