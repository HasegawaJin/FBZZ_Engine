/// @file    PlayChangesAdoption.cpp
/// @brief   Whitelisted native values and generated forests cross Play boundaries without runtime resources.
/// @author  Hasegawa Jin
/// @date    2026-10-24
#include <Editor/Util/PlayChangesAdoption.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Util/NumberParsing.hpp>
#include <imgui.h>
#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>
#include <unordered_set>
namespace fbzz::editor {
namespace play_changes_detail {
using scene::Scene;
using scene::GameObject;
using scene::EntityID;
using scene::CameraComponent;
using scene::TargetPointComponent;
using scene::TransformConstraintComponent;
using Births = std::array<uint64_t, 3>;
/// @note Draft IDs never alias an earlier Play session, including a new controller for a reloaded project.
std::atomic<uint64_t> g_nextDraftId{1};
uint64_t ReserveDraftId() {
    uint64_t value = g_nextDraftId.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<uint64_t>::max()) {
        if (g_nextDraftId.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) return value;
    }
    return 0;
}
std::string Text(const OpData& data, std::string_view key) {
    const auto* value = data.Find(key); return value && value->IsString() ? value->AsString() : std::string{};
}
bool Fail(std::string& reason, std::string message) { reason = std::move(message); return false; }
std::string Json(const OpData& data) { std::string result; (void)WriteOpDataJson(data, result); return result; }
Births ComponentBirths(Scene& scene, EntityID id) {
    return {scene.GetComponentLifetime<CameraComponent>(id), scene.GetComponentLifetime<TargetPointComponent>(id),
        scene.GetComponentLifetime<TransformConstraintComponent>(id)};
}
std::string ParentGuid(GameObject& object) { auto* parent = object.GetParent(); return parent ? parent->instanceId : std::string{}; }
OpData Vec4(float x, float y, float z, float w) {
    OpData result = OpData::MakeArray(); result.Push(x); result.Push(y); result.Push(z); result.Push(w); return result;
}
/// @note This explicit list is independent of the registry: registering a new runtime component never enables adoption.
template<typename T> constexpr bool SUPPORTED_COMPONENT = std::is_same_v<T, CameraComponent>
    || std::is_same_v<T, TargetPointComponent> || std::is_same_v<T, TransformConstraintComponent>;
bool SupportedGenerated(GameObject& object, std::string& reason) {
    bool supported = true;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if constexpr (!SUPPORTED_COMPONENT<T>) if (object.GetComponent<T>()) {
            supported = false; reason = "UNSUPPORTED_COMPONENT: " + object.instanceId + "/" + Registration::serializedName;
        }
    });
    if (!object.prefabAssetPath.empty() || !object.prefabSourceId.empty() || !object.prefabSourceSnapshot.empty())
        return Fail(reason, "UNSUPPORTED_PREFAB: generated Prefab ownership requires the Prefab merge workflow.");
    return supported;
}
/// @note Flattened persistent paths are both the selection contract and immutable review payload.
OpData Values(GameObject& go) {
    OpData data = OpData::MakeObject();
    data.Set("GameObject.name", go.name); data.Set("GameObject.tag", go.tag); data.Set("GameObject.layer", go.layer);
    data.Set("GameObject.active", go.activeSelf());
    data.Set("Transform.position", go.transform.position); data.Set("Transform.scale", go.transform.scale);
    const auto& q = go.transform.rotation; data.Set("Transform.rotation", Vec4(q.x, q.y, q.z, q.w));
    if (auto* camera = go.GetComponent<CameraComponent>()) {
        data.Set("CameraComponent.fovY", camera->fovY); data.Set("CameraComponent.nearZ", camera->nearZ);
        data.Set("CameraComponent.farZ", camera->farZ); data.Set("CameraComponent.enabled", camera->enabled);
        data.Set("CameraComponent.isMain", camera->isMain);
    }
    if (auto* point = go.GetComponent<TargetPointComponent>()) {
        data.Set("TargetPointComponent.visible", point->visible); data.Set("TargetPointComponent.screenSize", point->screenSize);
        data.Set("TargetPointComponent.color", Vec4(point->color.x, point->color.y, point->color.z, point->color.w));
    }
    if (auto* constraint = go.GetComponent<TransformConstraintComponent>()) {
        data.Set("TransformConstraintComponent.enabled", constraint->enabled);
        data.Set("TransformConstraintComponent.mode", static_cast<int>(constraint->mode));
        data.Set("TransformConstraintComponent.weight", constraint->weight);
        auto reference = constraint->target; auto* target = reference.Resolve(*go.GetScene());
        data.Set("TransformConstraintComponent.target", target ? target->instanceId : constraint->target.guid);
        data.Set("TransformConstraintComponent.positionOffset", constraint->positionOffset);
        data.Set("TransformConstraintComponent.rotationOffsetDegrees", constraint->rotationOffsetDegrees);
        data.Set("TransformConstraintComponent.scaleMultiplier", constraint->scaleMultiplier);
        data.Set("TransformConstraintComponent.aimAxis", constraint->aimAxis);
        data.Set("TransformConstraintComponent.upAxis", constraint->upAxis);
        data.Set("TransformConstraintComponent.maintainOffset", constraint->maintainOffset);
    }
    return data;
}
bool Finite(const math::Vector3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
bool ValidValues(GameObject& go, std::string& reason, bool validateReferences = true) {
    const auto& q = go.transform.rotation;
    const double length = double(q.x)*q.x + double(q.y)*q.y + double(q.z)*q.z + double(q.w)*q.w;
    if (go.name.empty() || go.layer < 0 || go.layer > 31 || !Finite(go.transform.position) || !Finite(go.transform.scale)
        || !std::isfinite(length) || std::abs(length - 1.0) > 0.002)
        return Fail(reason, "INVALID_VALUE: name/layer or finite local TRS with normalized rotation required.");
    if (auto* camera = go.GetComponent<CameraComponent>()) {
        if (!std::isfinite(camera->fovY) || camera->fovY <= 0 || camera->fovY >= 180
            || !std::isfinite(camera->nearZ) || !std::isfinite(camera->farZ) || camera->nearZ <= 0 || camera->farZ <= camera->nearZ)
            return Fail(reason, "INVALID_VALUE: Camera requires 0 < fovY < 180 and 0 < nearZ < farZ.");
        if (!std::isfinite(camera->aspectRatio) || camera->aspectRatio <= 0 || !std::isfinite(camera->cullingBoundsPadding)
            || !std::isfinite(camera->maxDrawDistance) || !std::isfinite(camera->smallObjectScreenHeight)
            || !std::isfinite(camera->backgroundColor.x) || !std::isfinite(camera->backgroundColor.y)
            || !std::isfinite(camera->backgroundColor.z) || !std::isfinite(camera->backgroundColor.w)
            || static_cast<int>(camera->clearMode) < 0 || static_cast<int>(camera->clearMode) > 1)
            return Fail(reason, "INVALID_VALUE: Camera authoring data is invalid or non-finite.");
        for (float distance : camera->layerCullDistances) if (!std::isfinite(distance))
            return Fail(reason, "INVALID_VALUE: Camera layer cull distances must be finite.");
    }
    if (auto* point = go.GetComponent<TargetPointComponent>()) {
        if (!std::isfinite(point->screenSize) || point->screenSize <= 0)
            return Fail(reason, "INVALID_VALUE: TargetPoint screenSize must be finite and positive.");
    }
    if (auto* constraint = go.GetComponent<TransformConstraintComponent>()) {
        auto reference = constraint->target;
        if (validateReferences && reference.IsAssigned() && !reference.Resolve(*go.GetScene()))
            return Fail(reason, "UNMAPPED_REFERENCE: TransformConstraint target no longer resolves in its source Scene.");
        const int mode = static_cast<int>(constraint->mode);
        if (mode < 0 || mode > 5 || !std::isfinite(constraint->weight) || constraint->weight < 0 || constraint->weight > 1
            || !Finite(constraint->positionOffset) || !Finite(constraint->rotationOffsetDegrees)
            || !Finite(constraint->scaleMultiplier) || !Finite(constraint->aimAxis) || !Finite(constraint->upAxis))
            return Fail(reason, "INVALID_VALUE: invalid TransformConstraint authoring value.");
    }
    if (Json(Values(go)).empty()) return Fail(reason, "INVALID_VALUE: non-finite authoring values cannot be serialized.");
    return true;
}
void WriteValues(GameObject& go, const OpData& fields) {
    for (const auto& [path, value] : fields.AsObject()) {
        if (path == "GameObject.name") go.name = value.AsString();
        else if (path == "GameObject.tag") go.tag = value.AsString();
        else if (path == "GameObject.layer") go.layer = value.AsInt();
        else if (path == "GameObject.active") go.SetActive(value.AsBool());
        else if (path == "Transform.position") go.transform.position = value.AsVec3();
        else if (path == "Transform.scale") go.transform.scale = value.AsVec3();
        else if (path == "Transform.rotation") {
            const auto& items = value.AsArray(); go.transform.rotation = {items[0].AsFloat(), items[1].AsFloat(), items[2].AsFloat(), items[3].AsFloat()};
        }
        else if (path.starts_with("CameraComponent.")) {
            auto* camera = go.GetComponent<CameraComponent>();
            if (path == "CameraComponent.fovY") camera->fovY = value.AsFloat();
            else if (path == "CameraComponent.nearZ") camera->nearZ = value.AsFloat();
            else if (path == "CameraComponent.farZ") camera->farZ = value.AsFloat();
            else if (path == "CameraComponent.enabled") camera->enabled = value.AsBool();
            else if (path == "CameraComponent.isMain") camera->isMain = value.AsBool();
        }
        else if (path.starts_with("TargetPointComponent.")) {
            auto* point = go.GetComponent<TargetPointComponent>();
            if (path == "TargetPointComponent.visible") point->visible = value.AsBool();
            else if (path == "TargetPointComponent.screenSize") point->screenSize = value.AsFloat();
            else { const auto& a = value.AsArray(); point->color = {a[0].AsFloat(), a[1].AsFloat(), a[2].AsFloat(), a[3].AsFloat()}; }
        }
        else if (auto* constraint = go.GetComponent<TransformConstraintComponent>()) {
            if (path == "TransformConstraintComponent.enabled") constraint->enabled = value.AsBool();
            else if (path == "TransformConstraintComponent.mode") constraint->mode = static_cast<scene::TransformConstraintMode>(value.AsInt());
            else if (path == "TransformConstraintComponent.weight") constraint->weight = value.AsFloat();
            else if (path == "TransformConstraintComponent.target") {
                auto* target = go.GetScene()->FindByGuid(value.AsString());
                constraint->target = {target ? target->GetID() : EntityID::INVALID, value.AsString(), go.GetScene()->GetRenderSceneGeneration()};
            }
            else if (path == "TransformConstraintComponent.positionOffset") constraint->positionOffset = value.AsVec3();
            else if (path == "TransformConstraintComponent.rotationOffsetDegrees") constraint->rotationOffsetDegrees = value.AsVec3();
            else if (path == "TransformConstraintComponent.scaleMultiplier") constraint->scaleMultiplier = value.AsVec3();
            else if (path == "TransformConstraintComponent.aimAxis") constraint->aimAxis = value.AsVec3();
            else if (path == "TransformConstraintComponent.upAxis") constraint->upAxis = value.AsVec3();
            else if (path == "TransformConstraintComponent.maintainOffset") constraint->maintainOffset = value.AsBool();
        }
    }
}
struct Node {
    std::string guid, parent, name, tag;
    int layer = 0, sibling = 0;
    bool active = true;
    math::Vector3 position, scale;
    math::Quaternion rotation;
    std::optional<CameraComponent> camera;
    std::optional<TargetPointComponent> point;
    std::optional<TransformConstraintComponent> constraint;
};
Node ReadNode(GameObject& go) {
    Node node; node.guid = go.instanceId; node.parent = ParentGuid(go); node.name = go.name; node.tag = go.tag;
    node.layer = go.layer; node.active = go.activeSelf(); node.sibling = go.GetSiblingIndex();
    node.position = go.transform.position; node.rotation = go.transform.rotation; node.scale = go.transform.scale;
    if (auto* value = go.GetComponent<CameraComponent>()) node.camera = *value;
    if (auto* value = go.GetComponent<TargetPointComponent>()) node.point = *value;
    if (auto* value = go.GetComponent<TransformConstraintComponent>()) {
        node.constraint = *value;
        auto reference = value->target; auto* target = reference.Resolve(*go.GetScene());
        node.constraint->target = {EntityID::INVALID, target ? target->instanceId : value->target.guid};
    }
    return node;
}
void WriteNode(GameObject& go, const Node& node) {
    go.instanceId = node.guid; go.name = node.name; go.tag = node.tag; go.layer = node.layer; go.SetActive(node.active);
    go.runtimeGenerated = false;
    go.transform.position = node.position; go.transform.rotation = node.rotation; go.transform.scale = node.scale;
    if (node.camera) go.AddComponent<CameraComponent>(*node.camera);
    if (node.point) go.AddComponent<TargetPointComponent>(*node.point);
    if (node.constraint) go.AddComponent<TransformConstraintComponent>(*node.constraint);
}
bool RestoreGeneratedLocalAndOrder(Scene& scene, const std::vector<Node>& nodes, std::string& reason) {
    std::vector<const Node*> ordered;
    for (const auto& node : nodes) {
        auto* object = scene.FindByGuid(node.guid);
        object->transform.position = node.position; object->transform.rotation = node.rotation; object->transform.scale = node.scale;
        ordered.push_back(&node);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->parent != right->parent) return left->parent < right->parent;
        return left->sibling != right->sibling ? left->sibling < right->sibling : left->guid < right->guid;
    });
    for (size_t first = 0; first < ordered.size();) {
        size_t end = first + 1;
        while (end < ordered.size() && ordered[end]->parent == ordered[first]->parent) ++end;
        const auto& parentGuid = ordered[first]->parent;
        const bool internal = std::any_of(nodes.begin(), nodes.end(), [&](const auto& parent) { return parent.guid == parentGuid; });
        auto* parent = scene.FindByGuid(parentGuid);
        const size_t siblingCount = parent ? static_cast<size_t>(parent->GetChildCount()) : scene.GetRootGameObjects().size();
        if (siblingCount < end - first || siblingCount > static_cast<size_t>(std::numeric_limits<int>::max()))
            return Fail(reason, "SIBLING_CONFLICT: generated sibling group is incomplete or exceeds the hierarchy index range.");
        /// @note External groups stay after every unowned sibling, while retaining their captured Play ordinal order.
        for (size_t index = first; index < end; ++index) {
            auto* object = scene.FindByGuid(ordered[index]->guid);
            const int desired = internal ? ordered[index]->sibling : static_cast<int>(siblingCount - 1);
            if (object->GetSiblingIndex() != desired && !object->SetSiblingIndex(desired))
                return Fail(reason, "SIBLING_CONFLICT: generated Play sibling order could not be restored.");
        }
        first = end;
    }
    return true;
}
/// @note SceneSerializer supplies canonical persistence; only detached, whitelisted plain-data nodes reach it.
std::string SerializeNodes(const std::vector<Node>& nodes, std::string& reason) {
    Scene scratch;
    for (const auto& node : nodes) { auto* object = scratch.TryCreateGameObject(node.name); if (!object) return {}; WriteNode(*object, node); }
    for (const auto& node : nodes) if (auto* parent = scratch.FindByGuid(node.parent))
        if (!scratch.FindByGuid(node.guid)->SetParent(parent)) return {};
    if (!RestoreGeneratedLocalAndOrder(scratch, nodes, reason)) return {};
    scene::FlushWorldTransforms(scratch);
    return scene::SceneSerializer::CapturePassiveClone(scratch, reason);
}
struct Original { std::string guid, parent; EntityID edit, play; Births editBirths{}, playBirths{}; };
struct Patch { Original original; OpData before, after; };
struct Draft {
    uint64_t id = 0;
    std::vector<Patch> patches;
    std::vector<Node> generated;
    std::vector<Original> references;
    std::vector<EntityID> created;
    std::vector<Births> createdBirths;
    std::string generatedText, diagnostic;
    size_t preparedRevision = 0;
    bool prepared = false, consumed = false, discarded = false;
};
bool ReferenceRestoredByUndo(const Draft& draft, const std::string& owner, const std::string& path) {
    if (path != "TransformConstraintComponent.target") return false;
    return std::any_of(draft.patches.begin(), draft.patches.end(), [&](const auto& patch) {
        return patch.original.guid == owner && patch.after.Find(path) != nullptr;
    });
}
/// @note Read counterpart of the clone/remap reference traversal; id-only native references need not appear in TOML.
/// @see Engine/src/Scene/PrefabReferenceRemap.cpp
class IncomingNativeReferenceVisitor final {
public:
    IncomingNativeReferenceVisitor(Scene& scene, const Draft& draft, std::string owner, std::string& reason)
        : m_scene(scene), m_draft(draft), m_owner(std::move(owner)), m_reason(reason) {}
    void Component(const char* name) { m_path = name; }
    bool Clean() const { return m_clean; }
    void Reference(const std::string& name, const scene::EntityRef& value) {
        /// @note Resolve a local value so a refused Undo never fills or clears the live reference's mutable cache.
        auto reference = value;
        if (auto* target = reference.Resolve(m_scene)) Entity(name, target->GetID());
    }
    void References(const std::string& name, const std::vector<scene::EntityRef>& values) {
        for (size_t index = 0; index < values.size(); ++index) {
            Reference(name + "[" + std::to_string(index) + "]", values[index]);
        }
    }
    void Entity(const std::string& name, EntityID value) {
        if (!m_clean || ReferenceRestoredByUndo(m_draft, m_owner, Path(name))) return;
        if (std::find(m_draft.created.begin(), m_draft.created.end(), value) != m_draft.created.end()) Conflict(Path(name));
    }
    void Guid(const std::string& name, const std::string& value) {
        if (value.empty() || !m_clean || ReferenceRestoredByUndo(m_draft, m_owner, Path(name))) return;
        if (std::any_of(m_draft.generated.begin(), m_draft.generated.end(), [&](const auto& node) { return node.guid == value; })) Conflict(Path(name));
    }
    void LegacyIK(const std::string& name, const scene::ObjectReference& reference, EntityID legacy,
        const std::string& guid, const std::string& objectName, bool migrationPending) {
        Guid(name, reference.guid);
        if (!reference.guid.empty() || !migrationPending) return;
        Guid(name, guid);
        if (!guid.empty()) return;
        Entity(name, legacy);
        if (m_scene.GetGameObject(legacy) || objectName.empty()) return;
        GameObject* unique = nullptr;
        for (auto& object : m_scene.GameObjects()) if (object.name == objectName) {
            if (unique) return;
            unique = &object;
        }
        if (unique) Entity(name, unique->GetID());
    }
private:
    std::string Path(const std::string& name) const { return m_path.empty() ? name : m_path + "." + name; }
    void Conflict(const std::string& path) { m_clean = false; m_reason = "INCOMING_REFERENCE: " + m_owner + "/" + path + " references an adopted owner; clear that outside reference before Undo."; }
    Scene& m_scene;
    const Draft& m_draft;
    std::string m_owner, m_path;
    std::string& m_reason;
    bool m_clean = true;
};
template<typename T>
void InspectNativeReferenceSlots(const T& component, IncomingNativeReferenceVisitor& visitor) {
    /// @note Read authored EntityRef slots directly: invoking arbitrary Reflect here could run physics setters or normalize unrelated settings.
    if constexpr (std::is_same_v<T, scene::CameraFollowComponent> || std::is_same_v<T, scene::SocketAttachmentComponent>
        || std::is_same_v<T, scene::TransformConstraintComponent> || std::is_same_v<T, scene::NavMeshAgentComponent>)
        visitor.Reference("target", component.target);
    if constexpr (std::is_same_v<T, scene::CameraBlendComponent>) {
        visitor.Reference("fromCamera", component.fromCamera); visitor.Reference("toCamera", component.toCamera);
    }
    if constexpr (std::is_same_v<T, scene::JointComponent>) {
        visitor.Reference("connectedBody", component.connectedBody); visitor.References("chainBodies", component.chainBodies);
    }
    if constexpr (std::is_same_v<T, scene::SplineFollowerComponent>) visitor.Reference("spline", component.spline);
    if constexpr (std::is_same_v<T, scene::SequencePlayerComponent>) {
        for (size_t index = 0; index < component.bindings.size(); ++index)
            visitor.Reference("bindings[" + std::to_string(index) + "].target", component.bindings[index].target);
    }
    if constexpr (std::is_same_v<T, scene::UINavigation>) {
        visitor.Reference("up", component.up); visitor.Reference("down", component.down);
        visitor.Reference("left", component.left); visitor.Reference("right", component.right);
    }
    if constexpr (std::is_same_v<T, scene::VFXBeamComponent> || std::is_same_v<T, scene::VFXLineComponent>) {
        visitor.Reference("fromEntity", component.fromEntity); visitor.Reference("toEntity", component.toEntity);
    }
    if constexpr (std::is_same_v<T, scene::WaterExclusionComponent>) visitor.References("waters", component.waters);
    if constexpr (std::is_same_v<T, scene::WaterZoneComponent>) {
        for (size_t index = 0; index < component.members.size(); ++index)
            visitor.Reference("members[" + std::to_string(index) + "].water", component.members[index].water);
        for (size_t index = 0; index < component.riverMouths.size(); ++index)
            visitor.Reference("riverMouths[" + std::to_string(index) + "].river", component.riverMouths[index].river);
    }
    if constexpr (std::is_same_v<T, scene::IKSolverComponent>) {
        for (size_t index = 0; index < component.chains.size(); ++index) {
            const auto& chain = component.chains[index]; const auto path = "chains[" + std::to_string(index) + "]";
            visitor.LegacyIK(path + ".targetReference", chain.targetReference, chain.targetEntity, chain.targetGuid, chain.targetName, chain.targetReferenceMigrationPending);
            visitor.LegacyIK(path + ".poleReference", chain.poleReference, chain.poleEntity, chain.poleGuid, chain.poleName, chain.poleReferenceMigrationPending);
        }
    }
    if constexpr (std::is_same_v<T, scene::BoneComponent>) visitor.Entity("skinnedMeshEntity", component.skinnedMeshEntity);
    if constexpr (std::is_same_v<T, scene::SkinnedMeshRenderer>) {
        visitor.Entity("skeletonRootEntity", component.skeletonRootEntity);
        for (size_t index = 0; index < component.nodeEntities.size(); ++index) visitor.Entity("nodeEntities[" + std::to_string(index) + "]", component.nodeEntities[index]);
    }
    if constexpr (std::is_same_v<T, scene::TerrainGridComponent>) {
        for (size_t index = 0; index < component.cells.size(); ++index) visitor.Entity("cells[" + std::to_string(index) + "]", component.cells[index]);
    }
    if constexpr (std::is_same_v<T, scene::LODGroupComponent>) {
        for (size_t level = 0; level < component.levels.size(); ++level) for (size_t index = 0; index < component.levels[level].renderers.size(); ++index) {
            const auto& reference = component.levels[level].renderers[index];
            const auto path = "levels[" + std::to_string(level) + "].renderers[" + std::to_string(index) + "]";
            visitor.Entity(path, reference.entity); visitor.Guid(path, reference.instanceId);
        }
    }
    if constexpr (std::is_same_v<T, scene::ClothComponent>) {
        visitor.Entity("skinTarget", component.skinTarget);
        for (size_t index = 0; index < component.attachments.size(); ++index) {
            const auto& attachment = component.attachments[index]; const auto path = "attachments[" + std::to_string(index) + "]";
            visitor.Entity(path + ".target", attachment.target); visitor.Guid(path + ".targetReference", attachment.targetReference.guid);
        }
    }
}
bool NoIncomingNativeReferences(Scene& scene, const Draft& draft, std::string& reason) {
    for (auto& object : scene.GameObjects()) {
        if (std::any_of(draft.generated.begin(), draft.generated.end(), [&](const auto& node) { return node.guid == object.instanceId; })) continue;
        IncomingNativeReferenceVisitor visitor(scene, draft, object.instanceId, reason);
        scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if constexpr (!std::is_same_v<T, scene::ScriptComponent>) if (visitor.Clean()) if (auto* component = object.GetComponent<T>()) {
                visitor.Component(Registration::serializedName);
                InspectNativeReferenceSlots(*component, visitor);
            }
        });
        if (!visitor.Clean()) return false;
    }
    return true;
}
/// @note The passive persistence boundary includes reflected, custom-codec and managed saved references without serialization callbacks.
/// @note Ambiguous GUID text also blocks Undo; uninspectable saved state never grants permission to delete a referenced owner.
bool NoIncomingSavedReferences(Scene& scene, const Draft& draft, std::string& reason) {
    if (draft.generated.empty()) return true;
    std::unordered_set<std::string> generated;
    for (const auto& node : draft.generated) generated.insert(node.guid);
    for (auto& object : scene.GameObjects()) if (!generated.contains(object.instanceId) && object.runtimeGenerated)
        return Fail(reason, "REFERENCE_PROOF_UNAVAILABLE: an outside runtime-generated owner is omitted by passive persistence: " + object.instanceId);
    if (!NoIncomingNativeReferences(scene, draft, reason)) return false;
    std::string diagnostic; const auto text = scene::SceneSerializer::CapturePassiveClone(scene, diagnostic);
    if (text.empty()) return Fail(reason, "REFERENCE_PROOF_UNAVAILABLE: passive reference capture failed: " + diagnostic);
    auto parsed = toml::parse(text);
    const auto* owners = parsed ? parsed.table()["gameobjects"].as_array() : nullptr;
    if (!owners) return Fail(reason, "REFERENCE_PROOF_UNAVAILABLE: passive reference document is invalid.");
    const auto inspect = [&](auto&& self, const toml::node& node, const std::string& owner, const std::string& path) -> bool {
        if (ReferenceRestoredByUndo(draft, owner, path)) return true;
        if (const auto* value = node.as_string()) {
            for (const auto& guid : generated) if (value->get().find(guid) != std::string::npos)
                return Fail(reason, "INCOMING_REFERENCE: " + owner + "/" + path + " contains adopted owner " + guid + "; clear that outside reference before Undo.");
        } else if (const auto* table = node.as_table()) {
            for (const auto& [key, value] : *table) {
                const std::string child = path.empty() ? std::string(key.str()) : path + "." + std::string(key.str());
                if (!self(self, value, owner, child)) return false;
            }
        } else if (const auto* array = node.as_array()) {
            for (size_t index = 0; index < array->size(); ++index)
                if (!self(self, (*array)[index], owner, path + "[" + std::to_string(index) + "]")) return false;
        }
        return true;
    };
    std::unordered_set<std::string> inspected;
    for (const auto& item : *owners) {
        const auto* table = item.as_table(); if (!table) return Fail(reason, "REFERENCE_PROOF_UNAVAILABLE: malformed saved owner.");
        const auto guid = (*table)["instanceId"].value_or(std::string{});
        if (guid.empty() || !inspected.insert(guid).second) return Fail(reason, "REFERENCE_PROOF_UNAVAILABLE: missing or duplicate saved owner identity.");
        if (!generated.contains(guid) && !inspect(inspect, item, guid, {})) return false;
    }
    for (auto& object : scene.GameObjects()) if (!inspected.contains(object.instanceId))
        return Fail(reason, "REFERENCE_PROOF_UNAVAILABLE: passive persistence omitted an owner: " + object.instanceId);
    return true;
}
OpData Describe(const Draft& draft) {
    OpData data = OpData::MakeObject(); data.Set("adoptionId", std::to_string(draft.id));
    data.Set("prepared", draft.prepared); data.Set("consumed", draft.consumed); data.Set("diagnostic", draft.diagnostic);
    OpData changes = OpData::MakeArray();
    for (const auto& patch : draft.patches) for (const auto& [path, after] : patch.after.AsObject()) {
        OpData row = OpData::MakeObject(); row.Set("ownerGuid", patch.original.guid); row.Set("path", path);
        row.Set("before", *patch.before.Find(path)); row.Set("after", after); changes.Push(std::move(row));
    }
    data.Set("changes", changes);
    OpData created = OpData::MakeArray();
    for (const auto& node : draft.generated) {
        OpData row = OpData::MakeObject(); row.Set("ownerGuid", node.guid); row.Set("name", node.name); row.Set("parentGuid", node.parent); row.Set("playSiblingIndex", node.sibling);
        created.Push(std::move(row));
    }
    data.Set("generated", created); data.Set("generatedToml", draft.generatedText); return data;
}
} /// @note namespace play_changes_detail
struct PlayChangesAdoptionState::Impl {
    uint64_t editLifetime = 0, playLifetime = 0;
    bool active = true, stopped = false;
    std::string captureDiagnostic;
    std::vector<play_changes_detail::Original> originals;
    std::vector<std::shared_ptr<play_changes_detail::Draft>> drafts;
    const play_changes_detail::Original* Find(const std::string& guid) const {
        const auto found = std::find_if(originals.begin(), originals.end(), [&](const auto& value) { return value.guid == guid; });
        return found == originals.end() ? nullptr : &*found;
    }
    std::shared_ptr<play_changes_detail::Draft> FindDraft(uint64_t id) const {
        const auto found = std::find_if(drafts.begin(), drafts.end(), [id](const auto& value) { return value->id == id && !value->discarded; });
        return found == drafts.end() ? nullptr : *found;
    }
    bool Edit(EditorContext& context, std::string& reason) const {
        return context.activeScene && context.activeScene->GetRenderSceneGeneration() == editLifetime
            && context.playMode && context.playMode->IsInEditor() && !context.playMode->HasPendingRestore()
            && !context.activeScene->IsExecutionParked() && context.undoStack && context.undoStack->IsRecordingEnabled()
            && stopped ? true : play_changes_detail::Fail(reason, "NOT_ORIGINAL_EDIT: restore the original Edit Scene with Stop first.");
    }
    bool Match(scene::Scene& scene, const play_changes_detail::Original& proof, bool play, std::string& reason) const {
        const auto id = play ? proof.play : proof.edit; auto* object = scene.GetGameObject(id);
        if (!object || object->instanceId != proof.guid || scene.FindByGuid(proof.guid) != object)
            return play_changes_detail::Fail(reason, "STALE_BIRTH: original owner was deleted, recreated or its ID changed: " + proof.guid);
        return true;
    }
    bool Validate(EditorContext& context, play_changes_detail::Draft& draft, bool applied, std::string& reason) const;
    bool Transition(EditorContext& context, play_changes_detail::Draft& draft, bool apply, std::string& reason);
};
std::shared_ptr<PlayChangesAdoptionState> PlayChangesAdoptionState::Begin(scene::Scene& edit, scene::Scene& play, std::string& diagnostic) {
    using namespace play_changes_detail;
    auto impl = std::make_shared<Impl>(); impl->editLifetime = edit.GetRenderSceneGeneration(); impl->playLifetime = play.GetRenderSceneGeneration();
    std::unordered_set<std::string> ids;
    for (auto& object : edit.GameObjects()) {
        if (object.runtimeGenerated) continue;
        auto* clone = play.FindByGuid(object.instanceId);
        if (object.instanceId.empty() || !ids.insert(object.instanceId).second || !clone) {
            diagnostic = "ADOPTION_BASELINE: missing or duplicate persistent Edit identity."; return nullptr;
        }
        impl->originals.push_back({object.instanceId, ParentGuid(object), object.GetID(), clone->GetID(),
            ComponentBirths(edit, object.GetID()), ComponentBirths(play, clone->GetID())});
    }
    diagnostic.clear(); return std::make_shared<PlayChangesAdoptionState>(std::move(impl));
}
void PlayChangesAdoptionState::Finish(bool stopped) {
    if (!m_impl->active) return;
    m_impl->active = false; m_impl->stopped = stopped;
    if (!stopped) for (auto& draft : m_impl->drafts) { draft->prepared = false; draft->diagnostic = "CANCELLED: Play session did not complete a safe Stop."; }
}
OpResult PlayChangesAdoptionState::Capture(EditorContext& context, const OpData& selection) {
    using namespace play_changes_detail;
    auto* play = context.playMode;
    if (!m_impl->active || !play || !play->CanEditTemporaryScene(context.activeScene) || !play->ParkedEditScene()
        || context.activeScene->GetRenderSceneGeneration() != m_impl->playLifetime)
        return OpResult::Err("NOT_IN_PLAY", "Capture requires the active Play or Pause Scene before Stop.");
    if (m_impl->drafts.size() >= 64)
        return OpResult::Err("DRAFT_CAPACITY", "Discard a native adoption draft before capturing another.");
    const auto* selected = selection.Find("targets"); const auto* generated = selection.Find("generatedRoots");
    if ((selected && !selected->IsArray()) || (generated && !generated->IsArray())) return OpResult::Err("BAD_SELECTION", "targets and generatedRoots must be arrays.");
    auto draft = std::make_shared<Draft>(); std::string reason;
    std::unordered_set<std::string> sourceGuids;
    for (auto& source : context.activeScene->GameObjects()) if (source.instanceId.empty() || !sourceGuids.insert(source.instanceId).second)
        return OpResult::Err("ID_CONFLICT", "Play Scene contains an empty or duplicate persistent ID.");
    std::unordered_set<std::string> targets;
    if (selected) for (const auto& target : selected->AsArray()) {
        const auto guid = Text(target, "ownerGuid"); const auto* original = m_impl->Find(guid); const auto* paths = target.Find("paths");
        if (!original || !targets.insert(guid).second || !paths || !paths->IsArray() || paths->AsArray().empty())
            return OpResult::Err("BAD_SELECTION", "Select initial owners once, with nonempty explicit paths.");
        if (!m_impl->Match(*play->ParkedEditScene(), *original, false, reason) || !m_impl->Match(*context.activeScene, *original, true, reason))
            return OpResult::Err("STALE_BIRTH", reason);
        auto* source = context.activeScene->GetGameObject(original->play); auto* edit = play->ParkedEditScene()->GetGameObject(original->edit);
        if (ParentGuid(*source) != original->parent) return OpResult::Err("UNSUPPORTED_REPARENT", "Existing owner reparenting requires a separate hierarchy edit.");
        if (ComponentBirths(*context.activeScene, original->play) != original->playBirths || ComponentBirths(*play->ParkedEditScene(), original->edit) != original->editBirths)
            return OpResult::Err("STALE_COMPONENT", "Native component add/remove/re-add is outside this adoption scope.");
        if (!ValidValues(*source, reason)) return OpResult::Err("INVALID_VALUE", reason);
        const auto before = Values(*edit), after = Values(*source); Patch patch; patch.original = *original;
        patch.before = OpData::MakeObject(); patch.after = OpData::MakeObject(); std::unordered_set<std::string> seen;
        for (const auto& path : paths->AsArray()) {
            if (!path.IsString() || !seen.insert(path.AsString()).second || !before.Find(path.AsString()) || !after.Find(path.AsString()))
                return OpResult::Err("UNSUPPORTED_PATH", "Only documented authoring paths on surviving components can be captured.");
            const auto* value = after.Find(path.AsString());
            if (Json(*before.Find(path.AsString())) == Json(*value)) continue;
            patch.before.Set(path.AsString(), *before.Find(path.AsString())); patch.after.Set(path.AsString(), *value);
        }
        if (!patch.after.AsObject().empty()) draft->patches.push_back(std::move(patch));
    }
    std::unordered_set<std::string> generatedIds;
    std::vector<GameObject*> pending;
    if (generated) for (const auto& root : generated->AsArray()) {
        if (!root.IsString()) return OpResult::Err("BAD_SELECTION", "generatedRoots contains persistent owner GUID strings.");
        auto* object = context.activeScene->FindByGuid(root.AsString());
        if (!object || m_impl->Find(root.AsString())) return OpResult::Err("NOT_GENERATED", "A generated root must be born after this Play began.");
        pending.push_back(object);
    }
    while (!pending.empty()) {
        auto* source = pending.back(); pending.pop_back();
        if (!generatedIds.insert(source->instanceId).second) continue;
        if (m_impl->Find(source->instanceId) || source->instanceId.empty() || !SupportedGenerated(*source, reason) || !ValidValues(*source, reason))
            return OpResult::Err("UNSUPPORTED_GENERATED", reason.empty() ? "Generated forests cannot contain an original Edit owner." : reason);
        draft->generated.push_back(ReadNode(*source));
        for (int index = 0; index < source->GetChildCount(); ++index) pending.push_back(source->GetChild(index));
    }
    if (draft->patches.empty() && draft->generated.empty()) return OpResult::Err("NO_CHANGES", "The selected authoring values have no changes.");
    /// @note Sorting makes capture, review and Undo verification independent of Scene storage order.
    std::sort(draft->generated.begin(), draft->generated.end(), [](const auto& a, const auto& b) { return a.guid < b.guid; });
    auto addReference = [&](const std::string& guid) {
        if (guid.empty() || generatedIds.contains(guid)) return true;
        const auto* original = m_impl->Find(guid);
        if (!original || !m_impl->Match(*play->ParkedEditScene(), *original, false, reason)
            || !m_impl->Match(*context.activeScene, *original, true, reason))
            return Fail(reason, "UNMAPPED_REFERENCE: select the entire generated target graph, or use a surviving initial owner: " + guid);
        if (std::none_of(draft->references.begin(), draft->references.end(), [&](const auto& item) { return item.guid == guid; })) draft->references.push_back(*original);
        return true;
    };
    for (const auto& node : draft->generated) {
        if (!addReference(node.parent) || (node.constraint && !addReference(node.constraint->target.guid))) return OpResult::Err("UNMAPPED_REFERENCE", reason);
    }
    for (const auto& patch : draft->patches) if (const auto* target = patch.after.Find("TransformConstraintComponent.target")) {
        if (!addReference(target->AsString())) return OpResult::Err("UNMAPPED_REFERENCE", reason);
        if (!addReference(Text(patch.before, "TransformConstraintComponent.target"))) return OpResult::Err("UNMAPPED_REFERENCE", reason);
    }
    if (!draft->generated.empty()) {
        draft->generatedText = SerializeNodes(draft->generated, reason);
        if (draft->generatedText.empty() || !scene::SceneSerializer::LoadDataFromText(draft->generatedText, {}))
            return OpResult::Err("SERIALIZATION_FAILED", "Generated forest failed the existing data serializer: " + reason);
    }
    draft->id = ReserveDraftId(); if (!draft->id) return OpResult::Err("DRAFT_CAPACITY", "Native adoption identity space is exhausted.");
    m_impl->drafts.push_back(draft); return OpResult::Data(Describe(*draft));
}
OpResult PlayChangesAdoptionState::Review(uint64_t id) const {
    using namespace play_changes_detail;
    auto draft = m_impl->FindDraft(id); return draft ? OpResult::Data(Describe(*draft)) : OpResult::Err("EXPIRED_DRAFT", "The native adoption draft was discarded.");
}
bool PlayChangesAdoptionState::Impl::Validate(EditorContext& context, play_changes_detail::Draft& draft, bool applied, std::string& reason) const {
    using namespace play_changes_detail;
    if (!Edit(context, reason)) return false;
    Scene& scene = *context.activeScene; std::unordered_set<std::string> guids;
    for (auto& object : scene.GameObjects()) if (object.instanceId.empty() || !guids.insert(object.instanceId).second)
        return Fail(reason, "ID_CONFLICT: empty or duplicate persistent Scene identity.");
    for (const auto& reference : draft.references) if (!Match(scene, reference, false, reason)) return false;
    for (const auto& patch : draft.patches) {
        if (!Match(scene, patch.original, false, reason)) return false;
        auto* object = scene.GetGameObject(patch.original.edit);
        if (ComponentBirths(scene, object->GetID()) != patch.original.editBirths || ParentGuid(*object) != patch.original.parent)
            return Fail(reason, "STALE_COMPONENT_OR_PARENT: captured native component or parent changed.");
        const auto current = Values(*object); const auto& expected = applied ? patch.after : patch.before;
        for (const auto& [path, value] : expected.AsObject())
            if (!current.Find(path) || Json(*current.Find(path)) != Json(value)) return Fail(reason, "VALUE_CONFLICT: " + patch.original.guid + "/" + path);
        /// @note Validate the proposed combined state against current unselected values before publishing any selected field.
        Scene candidate; auto& proposed = candidate.CreateGameObject(object->name); WriteNode(proposed, ReadNode(*object));
        WriteValues(proposed, applied ? patch.before : patch.after);
        if (!ValidValues(proposed, reason, false)) return false;
    }
    auto proposedName = [&](GameObject& object) {
        for (const auto& patch : draft.patches) if (patch.original.guid == object.instanceId)
            if (const auto* name = (applied ? patch.before : patch.after).Find("GameObject.name")) return name->AsString();
        return object.name;
    };
    std::unordered_set<std::string> generatedIds; for (const auto& node : draft.generated) generatedIds.insert(node.guid);
    for (const auto& patch : draft.patches) if (const auto* name = (applied ? patch.before : patch.after).Find("GameObject.name"))
        for (auto& other : scene.GameObjects()) if (other.instanceId != patch.original.guid && !(applied && generatedIds.contains(other.instanceId))
            && ParentGuid(other) == patch.original.parent && proposedName(other) == name->AsString())
            return Fail(reason, "NAME_CONFLICT: another sibling already uses the proposed name.");
    if (!applied) {
        if (!scene.CanCreateGameObjects(draft.generated.size())) return Fail(reason, "CAPACITY: not enough Scene entity slots for the complete generated forest.");
        for (const auto& node : draft.generated) {
            if (scene.FindByGuid(node.guid)) return Fail(reason, "ID_CONFLICT: generated persistent ID already exists: " + node.guid);
            for (auto& other : scene.GameObjects()) if (ParentGuid(other) == node.parent && proposedName(other) == node.name)
                return Fail(reason, "NAME_CONFLICT: generated sibling name already exists: " + node.name);
            for (const auto& other : draft.generated) if (other.guid != node.guid && other.parent == node.parent && other.name == node.name)
                return Fail(reason, "NAME_CONFLICT: two generated siblings have the same name.");
        }
    } else {
        if (draft.created.size() != draft.generated.size() || draft.createdBirths.size() != draft.generated.size()) return Fail(reason, "STALE_GENERATED: created identity receipt is incomplete.");
        std::vector<Node> current;
        for (size_t index = 0; index < draft.generated.size(); ++index) {
            const auto& node = draft.generated[index]; auto* object = scene.GetGameObject(draft.created[index]);
            if (!object || object->instanceId != node.guid || ComponentBirths(scene, object->GetID()) != draft.createdBirths[index]
                || ParentGuid(*object) != node.parent || object->runtimeGenerated || !SupportedGenerated(*object, reason))
                return Fail(reason, "STALE_GENERATED: adopted owner/component/parent was removed or changed: " + node.guid);
            for (int child = 0; child < object->GetChildCount(); ++child) if (!generatedIds.contains(object->GetChild(child)->instanceId))
                return Fail(reason, "NEW_CHILD_CONFLICT: Undo would remove a child outside this adoption.");
            current.push_back(ReadNode(*object));
        }
        if (!current.empty() && SerializeNodes(current, reason) != draft.generatedText)
            return Fail(reason, "VALUE_CONFLICT: an adopted generated authoring value changed after Apply.");
        if (!NoIncomingSavedReferences(scene, draft, reason)) return false;
    }
    reason.clear(); return true;
}
bool PlayChangesAdoptionState::Impl::Transition(EditorContext& context, play_changes_detail::Draft& draft, bool apply, std::string& reason) {
    using namespace play_changes_detail;
    if (!Validate(context, draft, !apply, reason)) return false;
    Scene& scene = *context.activeScene;
    if (apply) {
        std::vector<EntityID> created; created.reserve(draft.generated.size());
        const auto rollback = [&]() { scene.DestroyGameObjects(created); };
        for (const auto& node : draft.generated) {
            auto* object = scene.TryCreateGameObject(node.name);
            if (!object) { rollback(); return Fail(reason, "CAPACITY: generated creation failed; selected properties were not changed."); }
            created.push_back(object->GetID()); WriteNode(*object, node);
        }
        for (const auto& node : draft.generated) if (!node.parent.empty()) {
            auto* object = scene.FindByGuid(node.guid); auto* parent = scene.FindByGuid(node.parent);
            if (!parent || !object->SetParent(parent)) { rollback(); return Fail(reason, "PARENT_CONFLICT: generated graph could not be attached."); }
        }
        if (!RestoreGeneratedLocalAndOrder(scene, draft.generated, reason)) { rollback(); return false; }
        for (const auto& node : draft.generated) {
            auto* object = scene.FindByGuid(node.guid);
            if (node.constraint) {
                auto* target = scene.FindByGuid(node.constraint->target.guid);
                object->GetComponent<TransformConstraintComponent>()->target = {
                    target ? target->GetID() : EntityID::INVALID, node.constraint->target.guid, scene.GetRenderSceneGeneration()};
            }
        }
        draft.created = std::move(created); draft.createdBirths.clear();
        for (auto id : draft.created) draft.createdBirths.push_back(ComponentBirths(scene, id));
    }
    for (const auto& patch : draft.patches) WriteValues(*scene.GetGameObject(patch.original.edit), apply ? patch.after : patch.before);
    if (!apply) { scene.DestroyGameObjects(draft.created); draft.created.clear(); draft.createdBirths.clear(); }
    scene::FlushWorldTransforms(scene);
    if (context.markSceneDirty) context.markSceneDirty();
    reason.clear(); return true;
}
OpResult PlayChangesAdoptionState::Prepare(EditorContext& context, uint64_t id) {
    using namespace play_changes_detail;
    auto draft = m_impl->FindDraft(id); if (!draft || draft->consumed) return OpResult::Err("EXPIRED_DRAFT", "Draft was consumed or discarded.");
    draft->prepared = false;
    if (!m_impl->Validate(context, *draft, false, draft->diagnostic)) return OpResult::Err("ADOPTION_CONFLICT", draft->diagnostic);
    draft->preparedRevision = context.undoStack->GetRevision(); draft->prepared = true; return OpResult::Data(Describe(*draft));
}
OpResult PlayChangesAdoptionState::Apply(EditorContext& context, uint64_t id) {
    using namespace play_changes_detail;
    auto draft = m_impl->FindDraft(id);
    if (!draft || draft->consumed || !draft->prepared) return OpResult::Err("UNPREPARED_DRAFT", "Review and prepare this draft before explicit Apply.");
    if (!context.undoStack || context.undoStack->GetRevision() != draft->preparedRevision) {
        draft->prepared = false; return OpResult::Err("STALE_PREVIEW", "Edit history changed since preview; prepare and review again.");
    }
    if (!m_impl->Transition(context, *draft, true, draft->diagnostic)) { draft->prepared = false; return OpResult::Err("ADOPTION_CONFLICT", draft->diagnostic); }
    draft->consumed = true; draft->prepared = false;
    auto impl = m_impl; auto* retainedContext = &context;
    OpResult result = OpResult::Data(Describe(*draft));
    result.command = std::make_unique<CheckedLambdaCommand>("Adopt selected Play changes",
        [impl, draft, retainedContext](std::string& reason) { return impl->Transition(*retainedContext, *draft, true, reason); },
        [impl, draft, retainedContext](std::string& reason) { return impl->Transition(*retainedContext, *draft, false, reason); });
    return result;
}
void PlayChangesAdoptionState::Discard(uint64_t id) {
    if (auto draft = m_impl->FindDraft(id)) draft->discarded = true;
    std::erase_if(m_impl->drafts, [](const auto& draft) { return draft->discarded; });
}
void RegisterPlayChangesAdoptionOperators(OperatorRegistry& registry) {
    const auto state = [](OpContext& context) { return context.ctx.playMode ? context.ctx.playMode->ChangesAdoption() : nullptr; };
    EditorOperator capture; capture.id = "scene.play_changes.capture"; capture.label = "Capture Selected Play Changes"; capture.category = "Play"; capture.kind = OpKind::Action;
    capture.params = {{"selection", OpParamType::Data, "Explicit native targets/paths and generatedRoots GUIDs", true}};
    capture.exec = [state](OpContext& c, const OpArgs& args) {
        auto session = state(c); const auto* selection = args.GetData("selection");
        return session && selection ? session->Capture(c.ctx, *selection) : OpResult::Err("NO_PLAY_CAPTURE", "No native Play capture session or selection.");
    }; registry.Register(std::move(capture));
    for (const std::string action : {"review", "prepare", "apply", "discard"}) {
        EditorOperator operation; operation.id = "scene.play_changes." + action; operation.label = "Play Changes " + action; operation.category = "Play";
        operation.kind = action == "review" ? OpKind::Query : action == "apply" ? OpKind::Mutation : OpKind::Action;
        operation.undoLabel = action == "apply" ? "Adopt selected Play changes" : "";
        operation.params = {{"adoptionId", OpParamType::String, "Native adoption draft ID", true}};
        operation.exec = [state, action](OpContext& c, const OpArgs& args) {
            uint64_t id = 0; auto session = state(c);
            if (!session || !util::ParseInteger(args.GetString("adoptionId"), id)) return OpResult::Err("BAD_DRAFT", "Invalid native adoption draft ID.");
            if (action == "review") return session->Review(id);
            if (action == "prepare") return session->Prepare(c.ctx, id);
            if (action == "apply") return session->Apply(c.ctx, id);
            session->Discard(id); return OpResult::Ok();
        }; registry.Register(std::move(operation));
    }
}
void PlayChangesAdoptionState::Draw(EditorContext& context) {
    using namespace play_changes_detail;
    ImGui::SeparatorText("Selected Play changes (native / generated)");
    ImGui::TextWrapped("Supported generated components: Camera, Target Point, Transform Constraint. Other components require their own workflow.");
    if (m_impl->active && context.playMode->CanEditTemporaryScene(context.activeScene)) {
        auto* storage = ImGui::GetStateStorage();
        bool transform = storage->GetBool(ImGui::GetID("adopt.transform"), true);
        bool metadata = storage->GetBool(ImGui::GetID("adopt.metadata"), false);
        bool components = storage->GetBool(ImGui::GetID("adopt.components"), false);
        ImGui::Checkbox("Local Transform", &transform); ImGui::SameLine(); ImGui::Checkbox("Name / tag / layer / active", &metadata);
        ImGui::Checkbox("Supported component values", &components);
        storage->SetBool(ImGui::GetID("adopt.transform"), transform); storage->SetBool(ImGui::GetID("adopt.metadata"), metadata); storage->SetBool(ImGui::GetID("adopt.components"), components);
        if (ImGui::Button("Capture selected objects / generated forests")) {
            OpData selection = OpData::MakeObject(), targets = OpData::MakeArray(), roots = OpData::MakeArray();
            for (auto id : context.selectedEntities) if (auto* object = context.activeScene->GetGameObject(id)) {
                if (!m_impl->Find(object->instanceId)) { roots.Push(object->instanceId); continue; }
                OpData paths = OpData::MakeArray();
                const auto values = Values(*object);
                for (const auto& [path, value] : values.AsObject()) {
                    (void)value;
                    if ((transform && path.starts_with("Transform.")) || (metadata && path.starts_with("GameObject."))
                        || (components && !path.starts_with("Transform.") && !path.starts_with("GameObject."))) paths.Push(path);
                }
                if (paths.AsArray().empty()) continue;
                OpData target = OpData::MakeObject(); target.Set("ownerGuid", object->instanceId); target.Set("paths", std::move(paths)); targets.Push(std::move(target));
            }
            selection.Set("targets", std::move(targets)); selection.Set("generatedRoots", std::move(roots));
            OpArgs args; args.Set("selection", selection); const auto result = InvokeOperator(context, "scene.play_changes.capture", args);
            m_impl->captureDiagnostic = result.ok ? std::string{} : result.message;
        }
    }
    if (!m_impl->captureDiagnostic.empty()) ImGui::TextWrapped("%s", m_impl->captureDiagnostic.c_str());
    uint64_t discard = 0;
    for (const auto& draft : m_impl->drafts) {
        ImGui::PushID(std::to_string(draft->id).c_str());
        if (draft->discarded) { ImGui::TextWrapped("%s", draft->diagnostic.c_str()); ImGui::PopID(); continue; }
        ImGui::Text("Draft %llu%s", static_cast<unsigned long long>(draft->id), draft->consumed ? " / applied" : "");
        if (!draft->diagnostic.empty()) ImGui::TextWrapped("%s", draft->diagnostic.c_str());
        if (ImGui::TreeNode("Immutable captured diff / generated serialized values")) {
            for (const auto& patch : draft->patches) for (const auto& [path, after] : patch.after.AsObject()) {
                ImGui::TextWrapped("%s / %s\nBefore: %s\nAfter: %s", patch.original.guid.c_str(), path.c_str(), Json(*patch.before.Find(path)).c_str(), Json(after).c_str());
            }
            if (!draft->generatedText.empty()) ImGui::TextUnformatted(draft->generatedText.c_str());
            ImGui::TreePop();
        }
        ImGui::BeginDisabled(!m_impl->stopped || draft->consumed || !context.playMode->IsInEditor());
        if (ImGui::Button("Validate / prepare preview")) { OpArgs args; args.Set("adoptionId", std::to_string(draft->id)); const auto result = InvokeOperator(context, "scene.play_changes.prepare", args); if (!result.ok) draft->diagnostic = result.message; }
        ImGui::SameLine(); ImGui::BeginDisabled(!draft->prepared);
        if (ImGui::Button("Apply to original Edit")) { OpArgs args; args.Set("adoptionId", std::to_string(draft->id)); const auto result = InvokeOperator(context, "scene.play_changes.apply", args); if (!result.ok) draft->diagnostic = result.message; }
        ImGui::EndDisabled(); ImGui::EndDisabled(); ImGui::SameLine(); if (ImGui::Button("Discard capture")) discard = draft->id;
        ImGui::PopID();
    }
    if (discard) Discard(discard);
}
void DrawPlayChangesAdoptionReview(EditorContext& context) {
    if (context.playMode) if (auto state = context.playMode->ChangesAdoption()) state->Draw(context);
}
} /// @note namespace fbzz::editor
