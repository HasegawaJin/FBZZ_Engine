/// @file    ControlPointEditing.cpp
/// @brief   制御点移動を候補状態と検証付きUndoへまとめる。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Editor/Tools/ControlPointEditing.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PlayModeController.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/SplineComponents.hpp>
#include <Engine/Scene/Components/ClothComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/ControlPointAdapters.hpp>
#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/ParticleCurve.hpp>
#include <type_traits>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_set>

namespace fbzz::editor {
namespace {

bool Finite(const math::Vector3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool SameLocal(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.position == rhs.position && lhs.rotation == rhs.rotation && lhs.scale == rhs.scale;
}

bool SameWorld(const scene::Transform& lhs, const scene::Transform& rhs)
{
    return lhs.worldPosition == rhs.worldPosition && lhs.worldRotation == rhs.worldRotation
        && lhs.worldScale == rhs.worldScale;
}

bool UnitRotation(const math::Quaternion& rotation)
{
    const double length = std::sqrt(static_cast<double>(rotation.x) * rotation.x + static_cast<double>(rotation.y) * rotation.y
        + static_cast<double>(rotation.z) * rotation.z + static_cast<double>(rotation.w) * rotation.w);
    return std::isfinite(length) && std::fabs(length - 1.0) <= 1e-3;
}

bool ValidLocal(const scene::Transform& transform)
{
    return Finite(transform.position) && Finite(transform.scale) && UnitRotation(transform.rotation)
        && std::fabs(transform.scale.x) > 1e-6f && std::fabs(transform.scale.y) > 1e-6f && std::fabs(transform.scale.z) > 1e-6f;
}

bool ValidHierarchy(scene::GameObject& object)
{
    for (auto* current = &object; current; current = current->GetParent())
        if (!ValidLocal(current->transform)) return false;
    return true;
}

bool Invertible(const scene::Transform& transform)
{
    return ValidLocal(transform) && Finite(transform.worldPosition) && Finite(transform.worldScale) && UnitRotation(transform.worldRotation)
        && std::fabs(transform.worldScale.x) > 1e-6f
        && std::fabs(transform.worldScale.y) > 1e-6f
        && std::fabs(transform.worldScale.z) > 1e-6f;
}

bool ReadUnsigned(const std::string& value, uint64_t& result)
{
    if (value.empty()) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool ReadVector(const OpData* value, math::Vector3& result)
{
    if (!value) return false;
    if (value->IsVec3()) { result = value->AsVec3(); return Finite(result); }
    if (!value->IsArray() || value->AsArray().size() != 3) return false;
    float numbers[3]{};
    for (size_t index = 0; index < 3; ++index) {
        const auto& number = value->AsArray()[index];
        if (number.IsInt()) numbers[index] = static_cast<float>(number.AsInt64());
        else if (!number.TryGetFloat(numbers[index])) return false;
    }
    result = {numbers[0], numbers[1], numbers[2]};
    return Finite(result);
}

OpData RotationData(const math::Quaternion& rotation)
{
    auto result = OpData::MakeArray();
    result.Push(rotation.x); result.Push(rotation.y); result.Push(rotation.z); result.Push(rotation.w);
    return result;
}

bool MatchesRotation(const OpData* value, const math::Quaternion& rotation)
{
    if (!value || !value->IsArray() || value->AsArray().size() != 4) return false;
    const float expected[] = {rotation.x, rotation.y, rotation.z, rotation.w};
    for (size_t index = 0; index < 4; ++index) {
        float number = 0.0f;
        const auto& component = value->AsArray()[index];
        if (component.IsInt()) number = static_cast<float>(component.AsInt64());
        else if (!component.TryGetFloat(number)) return false;
        if (number != expected[index]) return false;
    }
    return true;
}

struct MoveRecord {
    std::string guid;
    scene::EntityID entity{};
    uint64_t markerLifetime = 0;
    scene::EntityID parent{};
    std::string parentGuid;
    scene::Transform parentTransform;
    scene::Transform before;
    math::Vector3 afterPosition{};
};

struct OwnerStamp {
    std::string guid;
    scene::EntityID entity{};
    uint64_t waterLifetime = 0;
    uint64_t splineLifetime = 0;
    uint64_t clothLifetime = 0;
    uint64_t waterSignature = 0;
    uint64_t splineSignature = 0;
    uint64_t clothSignature = 0;
    uint64_t identitySignature = 0;
    scene::Transform pose;
    struct Consumer {
        std::string type;
        uint64_t lifetime = 0;
        uint64_t signature = 0;
        bool operator==(const Consumer&) const = default;
    };
    std::vector<Consumer> consumers;
};

uint64_t EntityKey(scene::EntityID entity)
{
    return static_cast<uint64_t>(entity.generation) << 32 | entity.index;
}

struct AffectedObjectStamp {
    std::string guid;
    std::string rootGuid;
    scene::EntityID entity{};
    std::string parentGuid;
    scene::EntityID parent{};
    scene::Transform pose;
    uint64_t markerLifetime = 0;
    uint64_t markerConfigSignature = 0;
    bool activeSelf = true;
    bool affectedTarget = false;
};

struct AffectedTargets {
    std::unordered_set<std::string> guids;
    std::unordered_set<uint64_t> entities;
    bool allReferences = false;
};

std::vector<AffectedObjectStamp> CaptureAffectedObjects(scene::Scene& scene,
    const std::vector<MoveRecord>& records)
{
    std::vector<AffectedObjectStamp> result;
    for (auto& object : scene.GameObjects()) {
        bool affected = false;
        bool target = false;
        std::string rootGuid;
        for (const auto& record : records) {
            auto* root = scene.GetGameObject(record.entity);
            if (!root) continue;
            const bool inSubtree = object.GetID() == root->GetID() || object.IsDescendantOf(*root);
            const bool ancestor = root->IsDescendantOf(object);
            affected |= inSubtree || ancestor;
            target |= inSubtree;
            if (inSubtree && rootGuid.empty()) rootGuid = record.guid;
        }
        if (!affected) continue;
        AffectedObjectStamp stamp;
        stamp.guid = object.instanceId; stamp.rootGuid = std::move(rootGuid);
        stamp.entity = object.GetID(); stamp.pose = object.transform;
        stamp.markerLifetime = scene.GetComponentLifetime<scene::TargetPointComponent>(stamp.entity);
        stamp.activeSelf = object.activeSelf();
        stamp.affectedTarget = target;
        if (const auto* marker = object.GetComponent<scene::TargetPointComponent>()) {
            uint64_t signature = 14695981039346656037ull;
            const std::array<uint64_t, 6> values{
                static_cast<uint64_t>(marker->visible),
                static_cast<uint64_t>(std::bit_cast<uint32_t>(marker->color.x)),
                static_cast<uint64_t>(std::bit_cast<uint32_t>(marker->color.y)),
                static_cast<uint64_t>(std::bit_cast<uint32_t>(marker->color.z)),
                static_cast<uint64_t>(std::bit_cast<uint32_t>(marker->color.w)),
                static_cast<uint64_t>(std::bit_cast<uint32_t>(marker->screenSize))};
            for (const uint64_t value : values)
                signature = (signature ^ value) * 1099511628211ull;
            stamp.markerConfigSignature = signature;
        }
        if (const auto* parent = object.GetParent()) {
            stamp.parentGuid = parent->instanceId;
            stamp.parent = parent->GetID();
        }
        result.push_back(std::move(stamp));
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.guid < rhs.guid; });
    return result;
}

bool SameAffectedObjects(const std::vector<AffectedObjectStamp>& lhs,
    const std::vector<AffectedObjectStamp>& rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
        const auto& a = lhs[index]; const auto& b = rhs[index];
        if (a.guid != b.guid || a.rootGuid != b.rootGuid || a.entity != b.entity || a.parentGuid != b.parentGuid
            || a.parent != b.parent || a.markerLifetime != b.markerLifetime
            || a.markerConfigSignature != b.markerConfigSignature || a.activeSelf != b.activeSelf
            || a.affectedTarget != b.affectedTarget
            || !SameLocal(a.pose, b.pose) || !SameWorld(a.pose, b.pose)) return false;
    }
    return true;
}

AffectedTargets MakeAffectedTargets(const std::vector<AffectedObjectStamp>& affected)
{
    AffectedTargets result;
    result.guids.reserve(affected.size()); result.entities.reserve(affected.size());
    for (const auto& object : affected) {
        if (!object.affectedTarget) continue;
        result.guids.insert(object.guid);
        result.entities.insert(EntityKey(object.entity));
    }
    return result;
}

class ReferenceSignatureReflector final : public scene::IReflector {
public:
    ReferenceSignatureReflector(scene::Scene& scene, const AffectedTargets* affected)
        : m_scene(scene), m_affected(affected) {}

    void Field(const char* name, float& value) override { Start(name, 1); Number(std::bit_cast<uint32_t>(value)); }
    void Field(const char* name, int& value) override { Start(name, 2); Number(static_cast<uint32_t>(value)); }
    void Field(const char* name, bool& value) override { Start(name, 3); Number(value); }
    void Field(const char* name, math::Vector2& value) override
    {
        Start(name, 4); Number(std::bit_cast<uint32_t>(value.x)); Number(std::bit_cast<uint32_t>(value.y));
    }
    void Field(const char* name, math::Vector3& value) override { Start(name, 5); Vector(value); }
    void Field(const char* name, math::Vector4& value) override
    {
        Start(name, 6); Number(std::bit_cast<uint32_t>(value.x)); Number(std::bit_cast<uint32_t>(value.y));
        Number(std::bit_cast<uint32_t>(value.z)); Number(std::bit_cast<uint32_t>(value.w));
    }
    void Field(const char* name, std::string& value) override { Start(name, 7); Text(value); }
    void Field(const char* name, math::Quaternion& value) override
    {
        Start(name, 8); Number(std::bit_cast<uint32_t>(value.x)); Number(std::bit_cast<uint32_t>(value.y));
        Number(std::bit_cast<uint32_t>(value.z)); Number(std::bit_cast<uint32_t>(value.w));
    }
    void Field(const char* name, scene::EntityID& value) override
    {
        Start(name, 9); Reference(name, {}, value);
    }
    void Field(const char* name, scene::EntityRef& value) override
    {
        Start(name, 10); Reference(name, value.guid, value.id);
    }
    void TypedObjectReferenceField(const char* name, scene::ObjectReference& value,
        const char* typeName) override
    {
        Start(name, 12); Text(typeName ? typeName : ""); Reference(name, value.guid, value.id);
    }
    void ObjectReferenceField(const char* name, scene::ObjectReference& value) override
    {
        Start(name, 13); Reference(name, value.guid, value.id);
    }
    void RefField(const char* name, scene::EntityRef& value, const char* typeName) override
    {
        Start(name, 14); Text(typeName ? typeName : ""); Reference(name, value.guid, value.id);
    }
    void ObjectReferenceListField(const char* name,
        std::vector<scene::ScriptObjectReference>& values) override
    {
        Start(name, 15); Number(values.size());
        for (const auto& value : values) Reference(name, value.guid, value.id);
    }
    void ListField(const char* name, std::vector<float>& values) override
    {
        Start(name, 16); Number(values.size()); for (const auto value : values) Number(std::bit_cast<uint32_t>(value));
    }
    void ListField(const char* name, std::vector<int>& values) override
    {
        Start(name, 17); Number(values.size()); for (const auto value : values) Number(static_cast<uint32_t>(value));
    }
    void ListField(const char* name, std::vector<bool>& values) override
    {
        Start(name, 18); Number(values.size()); for (const bool value : values) Number(value);
    }
    void ListField(const char* name, std::vector<std::string>& values) override
    {
        Start(name, 19); Number(values.size()); for (const auto& value : values) Text(value);
    }
    void ListField(const char* name, std::vector<math::Vector2>& values) override
    {
        Start(name, 20); Number(values.size()); for (const auto& value : values) {
            Number(std::bit_cast<uint32_t>(value.x)); Number(std::bit_cast<uint32_t>(value.y));
        }
    }
    void ListField(const char* name, std::vector<math::Vector3>& values) override
    {
        Start(name, 21); Number(values.size()); for (const auto& value : values) Vector(value);
    }
    void ListField(const char* name, std::vector<math::Vector4>& values) override
    {
        Start(name, 22); Number(values.size()); for (const auto& value : values) {
            Number(std::bit_cast<uint32_t>(value.x)); Number(std::bit_cast<uint32_t>(value.y));
            Number(std::bit_cast<uint32_t>(value.z)); Number(std::bit_cast<uint32_t>(value.w));
        }
    }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override
    {
        Start(name, 23); Number(values.size()); for (const auto& value : values) Reference(name, value.guid, value.id);
    }
    void RefListField(const char* name, std::vector<scene::EntityRef>& values,
        const char* typeName) override
    {
        Start(name, 24); Text(typeName ? typeName : ""); Number(values.size());
        for (const auto& value : values) Reference(name, value.guid, value.id);
    }
    void AssetField(const char* name, scene::ScriptAssetReference& value,
        scene::ScriptAssetType type) override
    {
        Start(name, 25); Number(static_cast<uint64_t>(type)); Text(value.guid); Text(value.path);
    }
    void AssetListField(const char* name, std::vector<scene::ScriptAssetReference>& values,
        scene::ScriptAssetType type) override
    {
        Start(name, 35); Number(static_cast<uint64_t>(type)); Number(values.size());
        for (const auto& value : values) { Text(value.guid); Text(value.path); }
    }
    void Field(const char* name, input::KeyCode& value) override
    {
        Start(name, 36); Number(static_cast<uint64_t>(value));
    }
    void Field(const char* name, scene::DataAssetRef& value) override
    {
        Start(name, 26); Text(value.path); Text(value.type);
    }
    void Field(const char* name, scene::ParticleCurve& value) override
    {
        Start(name, 27); Number(value.keyCount); Number(static_cast<uint64_t>(value.interpolation));
        for (const auto& key : value.keys) {
            Number(std::bit_cast<uint32_t>(key.time)); Number(std::bit_cast<uint32_t>(key.value));
        }
    }
    void Field(const char* name, scene::ParticleGradient& value) override
    {
        Start(name, 28); Number(value.keyCount); Number(static_cast<uint64_t>(value.interpolation));
        Number(static_cast<uint64_t>(value.colorSpace));
        for (const auto& key : value.keys) {
            Number(std::bit_cast<uint32_t>(key.time)); Number(std::bit_cast<uint32_t>(key.color.x));
            Number(std::bit_cast<uint32_t>(key.color.y)); Number(std::bit_cast<uint32_t>(key.color.z));
            Number(std::bit_cast<uint32_t>(key.color.w));
        }
    }
    void BeginObject(const char* name) override { Start(name, 29); }
    void EndObject() override { Number(30); }
    std::size_t BeginObjectList(const char* name, std::size_t count) override
    {
        Start(name, 31); Number(count); return count;
    }
    void BeginObjectElement(std::size_t index) override { Number(32); Number(index); }
    void EndObjectElement() override { Number(33); }
    std::size_t EndObjectList() override { Number(34); return scene::IReflector::NO_REMOVE; }
    [[nodiscard]] bool ReferencesAffected() const { return m_referencesAffected; }
    [[nodiscard]] uint64_t Signature() const { return m_signature; }

private:
    void Number(uint64_t value)
    {
        m_signature = (m_signature ^ value) * 1099511628211ull;
    }
    void Text(std::string_view value)
    {
        Number(value.size());
        for (const unsigned char byte : value) Number(byte);
    }
    void Start(const char* name, uint64_t type)
    {
        Text(name ? std::string_view(name) : std::string_view{}); Number(type);
    }
    void Vector(const math::Vector3& value)
    {
        Number(std::bit_cast<uint32_t>(value.x)); Number(std::bit_cast<uint32_t>(value.y));
        Number(std::bit_cast<uint32_t>(value.z));
    }
    void Reference(const char* name, std::string_view guid, scene::EntityID id)
    {
        Text(name ? std::string_view(name) : std::string_view{}); Text(guid);
        if (guid.empty()) Number(EntityKey(id));
        if (m_affected && (m_affected->allReferences
            || (!guid.empty() ? m_affected->guids.contains(std::string(guid))
                : id.IsValid() && m_affected->entities.contains(EntityKey(id)))))
            m_referencesAffected = true;
        scene::GameObject* object = guid.empty() ? m_scene.GetGameObject(id) : m_scene.FindByGuid(std::string(guid));
        if (!object) { Number(0); return; }
        const auto entity = object->GetID();
        Number(EntityKey(entity)); Text(object->instanceId); Number(object->activeInHierarchy());
        Number(m_scene.GetComponentLifetime<scene::TargetPointComponent>(entity));
        Vector(object->transform.position); Vector(object->transform.scale);
        Vector(object->transform.worldPosition); Vector(object->transform.worldScale);
        for (const float value : {object->transform.rotation.x, object->transform.rotation.y,
             object->transform.rotation.z, object->transform.rotation.w,
             object->transform.worldRotation.x, object->transform.worldRotation.y,
             object->transform.worldRotation.z, object->transform.worldRotation.w})
            Number(std::bit_cast<uint32_t>(value));
        if (const auto* parent = object->GetParent()) {
            Text(parent->instanceId); Number(EntityKey(parent->GetID()));
            Vector(parent->transform.position); Vector(parent->transform.scale);
            Vector(parent->transform.worldPosition); Vector(parent->transform.worldScale);
        } else Text({});
        if (!m_affected || m_affected->guids.contains(object->instanceId)
            || m_affected->entities.contains(EntityKey(entity))) m_referencesAffected = true;
    }

    scene::Scene& m_scene;
    const AffectedTargets* m_affected = nullptr;
    uint64_t m_signature = 14695981039346656037ull;
    bool m_referencesAffected = false;
};

uint64_t ClothSignature(const scene::ClothComponent& cloth)
{
    uint64_t signature = 14695981039346656037ull;
    const auto number = [&signature](uint64_t value) { signature = (signature ^ value) * 1099511628211ull; };
    const auto text = [&number](const std::string& value) { number(value.size()); for (const unsigned char byte : value) number(byte); };
    text(cloth.clothAssetPath); number(static_cast<uint64_t>(cloth.segments)); number(cloth.enabled);
    number(cloth.attachments.size());
    for (const auto& row : cloth.attachments) {
        number(row.bindingId); number(static_cast<uint64_t>(row.particle)); number(row.topologySignature);
        text(row.targetReference.guid);
        if (row.targetReference.guid.empty()) number(EntityKey(row.target));
        number(std::bit_cast<uint32_t>(row.localPosition.x)); number(std::bit_cast<uint32_t>(row.localPosition.y));
        number(std::bit_cast<uint32_t>(row.localPosition.z)); number(std::bit_cast<uint32_t>(row.maxDistance));
    }
    return signature;
}

void ReflectIKChain(ReferenceSignatureReflector& reflector, const scene::IKChain& chain)
{
    int type = static_cast<int>(chain.type); reflector.Field("type", type);
    bool enabled = chain.enabled; reflector.Field("enabled", enabled);
    float weight = chain.weight; reflector.Field("weight", weight);
    int order = chain.order; reflector.Field("order", order);
    auto boneNames = chain.boneNames; reflector.ListField("boneNames", boneNames);
    auto targetReference = chain.targetReference; reflector.ObjectReferenceField("targetReference", targetReference);
    auto poleReference = chain.poleReference; reflector.ObjectReferenceField("poleReference", poleReference);
    auto targetEntity = chain.targetEntity; reflector.Field("targetEntity", targetEntity);
    auto poleEntity = chain.poleEntity; reflector.Field("poleEntity", poleEntity);
    auto targetName = chain.targetName; reflector.Field("targetName", targetName);
    auto targetGuid = chain.targetGuid; reflector.Field("targetGuid", targetGuid);
    scene::ObjectReference targetGuidReference; targetGuidReference.guid = chain.targetGuid;
    reflector.ObjectReferenceField("targetGuidReference", targetGuidReference);
    auto poleName = chain.poleName; reflector.Field("poleName", poleName);
    auto poleGuid = chain.poleGuid; reflector.Field("poleGuid", poleGuid);
    scene::ObjectReference poleGuidReference; poleGuidReference.guid = chain.poleGuid;
    reflector.ObjectReferenceField("poleGuidReference", poleGuidReference);
    bool targetMigration = chain.targetReferenceMigrationPending; reflector.Field("targetMigration", targetMigration);
    bool poleMigration = chain.poleReferenceMigrationPending; reflector.Field("poleMigration", poleMigration);
    auto targetOffset = chain.targetOffset; reflector.Field("targetOffset", targetOffset);
    float maxExtension = chain.maxExtension; reflector.Field("maxExtension", maxExtension);
    float softness = chain.softness; reflector.Field("softness", softness);
    float minBend = chain.minBendAngleDegrees; reflector.Field("minBendAngleDegrees", minBend);
    float maxBend = chain.maxBendAngleDegrees; reflector.Field("maxBendAngleDegrees", maxBend);
    bool autoPole = chain.autoPole; reflector.Field("autoPole", autoPole);
    auto autoPoleDirection = chain.autoPoleLocalDirection; reflector.Field("autoPoleLocalDirection", autoPoleDirection);
    auto handRotation = chain.handRotationOffset; reflector.Field("handRotationOffset", handRotation);
    float handWeight = chain.handRotationWeight; reflector.Field("handRotationWeight", handWeight);
    int fullBodyIterations = chain.fullBodyIterations; reflector.Field("fullBodyIterations", fullBodyIterations);
    float fullBodyRotation = chain.fullBodyMaxRotationDegrees; reflector.Field("fullBodyMaxRotationDegrees", fullBodyRotation);
    float fullBodyTolerance = chain.fullBodyTolerance; reflector.Field("fullBodyTolerance", fullBodyTolerance);
    bool animatorWeight = chain.useAnimatorIKWeight; reflector.Field("useAnimatorIKWeight", animatorWeight);
    float rayUp = chain.rayUpRatio; reflector.Field("rayUpRatio", rayUp);
    float rayDown = chain.rayDownRatio; reflector.Field("rayDownRatio", rayDown);
    float surfaceOffset = chain.footSurfaceOffset; reflector.Field("footSurfaceOffset", surfaceOffset);
    float deadZone = chain.correctionDeadZone; reflector.Field("correctionDeadZone", deadZone);
    float maxCorrection = chain.maxCorrection; reflector.Field("maxCorrection", maxCorrection);
    float plantDistance = chain.footPlantDistance; reflector.Field("footPlantDistance", plantDistance);
    float smoothTime = chain.smoothTime; reflector.Field("smoothTime", smoothTime);
    auto footNormal = chain.footNormalAxis; reflector.Field("footNormalAxis", footNormal);
    bool adjustHip = chain.adjustHip; reflector.Field("adjustHip", adjustHip);
    auto hipName = chain.hipBoneName; reflector.Field("hipBoneName", hipName);
    float hipWeight = chain.hipWeight; reflector.Field("hipWeight", hipWeight);
    float maxLower = chain.maxHipLowering; reflector.Field("maxHipLowering", maxLower);
    float maxRaise = chain.maxHipRaising; reflector.Field("maxHipRaising", maxRaise);
    float bendReserve = chain.hipBendReserve; reflector.Field("hipBendReserve", bendReserve);
    bool physicsFeet = chain.useRagdollFootShape; reflector.Field("useRagdollFootShape", physicsFeet);
    bool spineAuto = chain.spineAutoWeight; reflector.Field("spineAutoWeight", spineAuto);
    float flatWeight = chain.spineFlatWeight; reflector.Field("spineFlatWeight", flatWeight);
    float slopeRamp = chain.spineSlopeRampMeters; reflector.Field("spineSlopeRampMeters", slopeRamp);
    auto lookAxis = chain.lookAtAxis; reflector.Field("lookAtAxis", lookAxis);
    auto lookUp = chain.lookAtUpAxis; reflector.Field("lookAtUpAxis", lookUp);
    float lookClamp = chain.lookAtClampAngle; reflector.Field("lookAtClampAngle", lookClamp);
    float lookSpeed = chain.lookAtSpeed; reflector.Field("lookAtSpeed", lookSpeed);
}

bool CaptureScriptConsumers(scene::Scene& scene, scene::GameObject& owner,
    const AffectedTargets& affected, std::vector<OwnerStamp::Consumer>& consumers)
{
    auto* scripts = owner.GetComponent<scene::ScriptComponent>();
    if (!scripts) return true;
    const uint64_t lifetime = scene.GetComponentLifetime<scene::ScriptComponent>(owner.GetID());
    for (auto& entry : scripts->scripts) {
        ReferenceSignatureReflector reflector(scene, &affected);
        auto savedId = entry.savedScriptId; reflector.Field("savedScriptId", savedId);
        auto inspectionId = entry.inspectionId; reflector.Field("inspectionId", inspectionId);
        if (entry.script) {
            auto scriptType = std::string(entry.script->GetTypeName()); reflector.Field("scriptType", scriptType);
            bool enabled = entry.script->enabled; reflector.Field("enabled", enabled);
            const bool succeeded = entry.script->ExecuteCallback([&] {
                entry.script->Reflect(reflector);
            }, "editor.control_point_reference_snapshot");
            if (!succeeded) return false;
        }
        if (entry.serialized) {
            auto serializedType = entry.serialized->type; reflector.Field("serializedType", serializedType);
            bool serializedEnabled = entry.serialized->enabled; reflector.Field("serializedEnabled", serializedEnabled);
            auto serializedFields = entry.serialized->fieldsToml; reflector.Field("serializedFields", serializedFields);
            for (const auto& guid : affected.guids) {
                if (guid.empty() || serializedFields.find(guid) == std::string::npos) continue;
                scene::ObjectReference reference; reference.guid = guid;
                reflector.ObjectReferenceField("serializedReference", reference);
            }
        }
        if (reflector.ReferencesAffected() || (affected.allReferences && entry.serialized)) {
            const std::string type = "Script/" + entry.savedScriptId + "/" + entry.inspectionId;
            consumers.push_back({type, lifetime, reflector.Signature()});
        }
    }
    return true;
}

bool CaptureOwner(scene::Scene& scene, scene::GameObject& owner,
    const AffectedTargets& affected, OwnerStamp& result, bool& hasReferences)
{
    result.guid = owner.instanceId; result.entity = owner.GetID(); result.pose = owner.transform;
    result.waterLifetime = scene.GetComponentLifetime<scene::WaterComponent>(result.entity);
    result.splineLifetime = scene.GetComponentLifetime<scene::SplineComponent>(result.entity);
    result.clothLifetime = scene.GetComponentLifetime<scene::ClothComponent>(result.entity);
    if (const auto* water = owner.GetComponent<scene::WaterComponent>()) {
        auto copy = *water;
        result.waterSignature = scene::ResolveWaterControlPoints(scene, result.entity, copy).inputSignature;
    }
    if (const auto* spline = owner.GetComponent<scene::SplineComponent>()) {
        auto copy = *spline;
        result.splineSignature = scene::ResolveSplinePoints(scene, result.entity, copy).inputSignature;
    }
    if (const auto* cloth = owner.GetComponent<scene::ClothComponent>()) result.clothSignature = ClothSignature(*cloth);

    bool scanValid = true;
    scene::ForEachRegisteredComponent([&]<typename T, typename Registration>() {
        if (!scanValid) return;
        const auto* component = owner.GetComponent<T>();
        if (!component) return;
        if constexpr (std::is_same_v<T, scene::ScriptComponent>) {
            scanValid = CaptureScriptConsumers(scene, owner, affected, result.consumers);
        } else if constexpr (std::is_same_v<T, scene::IKSolverComponent>) {
            ReferenceSignatureReflector reflector(scene, &affected);
            bool enabled = component->enabled; reflector.Field("enabled", enabled);
            static_cast<void>(reflector.BeginObjectList("chains", component->chains.size()));
            for (size_t index = 0; index < component->chains.size(); ++index) {
                reflector.BeginObjectElement(index); ReflectIKChain(reflector, component->chains[index]);
                reflector.EndObjectElement();
            }
            reflector.EndObjectList();
            if (reflector.ReferencesAffected())
                result.consumers.push_back({Registration::serializedName,
                    scene.GetComponentLifetime<T>(owner.GetID()), reflector.Signature()});
        } else if constexpr (std::is_same_v<T, scene::LODGroupComponent>) {
            auto copy = *component;
            ReferenceSignatureReflector reflector(scene, &affected);
            copy.Reflect(reflector);
            static_cast<void>(reflector.BeginObjectList("levels", copy.levels.size()));
            for (size_t levelIndex = 0; levelIndex < copy.levels.size(); ++levelIndex) {
                reflector.BeginObjectElement(levelIndex);
                auto& level = copy.levels[levelIndex];
                reflector.Field("screenRelativeHeight", level.screenRelativeHeight);
                static_cast<void>(reflector.BeginObjectList("renderers", level.renderers.size()));
                for (size_t rendererIndex = 0; rendererIndex < level.renderers.size(); ++rendererIndex) {
                    reflector.BeginObjectElement(rendererIndex);
                    const auto& renderer = level.renderers[rendererIndex];
                    scene::ObjectReference reference{renderer.instanceId, renderer.entity, 0};
                    reflector.ObjectReferenceField("renderer", reference);
                    reflector.EndObjectElement();
                }
                static_cast<void>(reflector.EndObjectList());
                reflector.EndObjectElement();
            }
            static_cast<void>(reflector.EndObjectList());
            if (reflector.ReferencesAffected())
                result.consumers.push_back({Registration::serializedName,
                    scene.GetComponentLifetime<T>(owner.GetID()), reflector.Signature()});
        } else if constexpr (Registration::hasReflect && std::is_copy_constructible_v<T>) {
            T copy = *component;
            ReferenceSignatureReflector reflector(scene, &affected);
            copy.Reflect(reflector);
            if (reflector.ReferencesAffected())
                result.consumers.push_back({Registration::serializedName,
                    scene.GetComponentLifetime<T>(owner.GetID()), reflector.Signature()});
        }
    });
    if (!scanValid) return false;

    const auto addLegacy = [&](const char* type, uint64_t lifetime, uint64_t configSignature,
        auto&& visitReferences) {
        ReferenceSignatureReflector reflector(scene, &affected);
        visitReferences(reflector);
        if (reflector.ReferencesAffected()) {
            result.consumers.push_back({type, lifetime, configSignature ^ reflector.Signature()});
        }
    };
    if (const auto* water = owner.GetComponent<scene::WaterComponent>())
        addLegacy("WaterPointBindings", result.waterLifetime, result.waterSignature,
            [&](ReferenceSignatureReflector& reflector) {
                for (const auto& row : water->pointBindings) {
                    auto id = static_cast<int>(row.id); reflector.Field("id", id);
                    auto reference = row.target; reflector.ObjectReferenceField("target", reference);
                }
            });
    if (const auto* spline = owner.GetComponent<scene::SplineComponent>())
        addLegacy("SplinePointBindings", result.splineLifetime, result.splineSignature,
            [&](ReferenceSignatureReflector& reflector) {
                for (const auto& row : spline->pointBindings) {
                    auto id = static_cast<int>(row.id); reflector.Field("id", id);
                    auto reference = row.target; reflector.ObjectReferenceField("target", reference);
                }
            });
    if (const auto* cloth = owner.GetComponent<scene::ClothComponent>())
        addLegacy("ClothAttachments", result.clothLifetime, result.clothSignature,
            [&](ReferenceSignatureReflector& reflector) {
                for (const auto& row : cloth->attachments) {
                    auto reference = row.targetReference;
                    if (!reference.guid.empty()) reflector.ObjectReferenceField("target", reference);
                    else { auto entity = row.target; reflector.Field("target", entity); }
                }
            });

    std::sort(result.consumers.begin(), result.consumers.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.type < rhs.type;
    });
    uint64_t identity = 14695981039346656037ull;
    for (const auto& consumer : result.consumers) {
        for (const unsigned char byte : consumer.type) identity = (identity ^ byte) * 1099511628211ull;
        identity = (identity ^ consumer.lifetime) * 1099511628211ull;
        identity = (identity ^ consumer.signature) * 1099511628211ull;
    }
    result.identitySignature = identity;
    hasReferences = !result.consumers.empty();
    return true;
}

bool CaptureOwners(scene::Scene& scene, const std::vector<AffectedObjectStamp>& affectedObjects,
    std::vector<OwnerStamp>& result)
{
    result.clear();
    const auto affected = MakeAffectedTargets(affectedObjects);
    for (auto& owner : scene.GameObjects()) {
        OwnerStamp stamp;
        bool hasReferences = false;
        if (!CaptureOwner(scene, owner, affected, stamp, hasReferences)) return false;
        if (hasReferences) result.push_back(std::move(stamp));
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) { return lhs.guid < rhs.guid; });
    return true;
}

bool SameOwner(const OwnerStamp& lhs, const OwnerStamp& rhs)
{
    return lhs.guid == rhs.guid && lhs.entity == rhs.entity
        && SameLocal(lhs.pose, rhs.pose) && SameWorld(lhs.pose, rhs.pose)
        && lhs.waterLifetime == rhs.waterLifetime && lhs.splineLifetime == rhs.splineLifetime
        && lhs.clothLifetime == rhs.clothLifetime && lhs.waterSignature == rhs.waterSignature
        && lhs.splineSignature == rhs.splineSignature && lhs.clothSignature == rhs.clothSignature
        && lhs.identitySignature == rhs.identitySignature && lhs.consumers == rhs.consumers;
}

bool OwnersMatch(scene::Scene& scene, const std::vector<OwnerStamp>& expected,
    const std::vector<AffectedObjectStamp>& affectedObjects)
{
    std::vector<OwnerStamp> current;
    if (!CaptureOwners(scene, affectedObjects, current) || current.size() != expected.size()) return false;
    for (size_t index = 0; index < current.size(); ++index)
        if (!SameOwner(current[index], expected[index])) return false;
    return true;
}

/// @note Authoring bake signatures remain GUID-independent; this gesture-only identity covers all reflected owner references.
uint64_t OwnerIdentitySignature(scene::Scene& scene, scene::GameObject& owner)
{
    AffectedTargets allTargets;
    allTargets.allReferences = true;
    OwnerStamp stamp;
    bool hasReferences = false;
    if (!CaptureOwner(scene, owner, allTargets, stamp, hasReferences)) return 0;
    return stamp.identitySignature;
}

MoveRecord CaptureRecord(scene::Scene& scene, scene::GameObject& target)
{
    MoveRecord result;
    result.guid = target.instanceId; result.entity = target.GetID(); result.before = target.transform;
    result.afterPosition = target.transform.position;
    result.markerLifetime = scene.GetComponentLifetime<scene::TargetPointComponent>(result.entity);
    if (const auto* parent = target.GetParent()) {
        result.parent = parent->GetID(); result.parentGuid = parent->instanceId; result.parentTransform = parent->transform;
    }
    return result;
}

bool RecordMatches(scene::Scene& scene, const MoveRecord& record, bool applied)
{
    const auto* target = scene.GetGameObject(record.entity);
    if (!target || target->instanceId != record.guid
        || scene.GetComponentLifetime<scene::TargetPointComponent>(record.entity) != record.markerLifetime) return false;
    const auto& transform = target->transform;
    if (transform.position != (applied ? record.afterPosition : record.before.position)
        || transform.rotation != record.before.rotation || transform.scale != record.before.scale) return false;
    const auto* parent = target->GetParent();
    if (!record.parent.IsValid()) return parent == nullptr;
    return parent && parent->GetID() == record.parent && parent->instanceId == record.parentGuid
        && SameLocal(parent->transform, record.parentTransform) && SameWorld(parent->transform, record.parentTransform);
}

struct MovePlan {
    EditorContext* context = nullptr;
    uint64_t sceneGeneration = 0;
    std::vector<MoveRecord> records;
    std::vector<AffectedObjectStamp> beforeAffected;
    std::vector<AffectedObjectStamp> afterAffected;
    std::vector<OwnerStamp> beforeOwners;
    std::vector<OwnerStamp> afterOwners;
};

bool ApplyMove(const std::shared_ptr<MovePlan>& plan, bool apply, std::string& reason)
{
    auto& ctx = *plan->context;
    if (!IsControlPointAuthoringAllowed(ctx) || ctx.activeScene->GetRenderSceneGeneration() != plan->sceneGeneration) {
        reason = "制御点のSceneまたは編集状態が変わりました"; return false;
    }
    auto& scene = *ctx.activeScene;
    const auto& expectedAffected = apply ? plan->beforeAffected : plan->afterAffected;
    if (!std::all_of(plan->records.begin(), plan->records.end(), [&scene, apply](const MoveRecord& record) {
            return RecordMatches(scene, record, !apply);
        }) || !SameAffectedObjects(CaptureAffectedObjects(scene, plan->records), expectedAffected)
        || !OwnersMatch(scene, apply ? plan->beforeOwners : plan->afterOwners, expectedAffected)) {
        reason = "制御点、親の姿勢、接続設定が変更されています"; return false;
    }
    for (const auto& record : plan->records)
        scene.GetGameObject(record.entity)->transform.position = apply ? record.afterPosition : record.before.position;
    scene::FlushWorldTransforms(scene);
    if (apply) {
        plan->afterAffected = CaptureAffectedObjects(scene, plan->records);
        if (!CaptureOwners(scene, plan->afterAffected, plan->afterOwners)) {
            for (const auto& record : plan->records)
                scene.GetGameObject(record.entity)->transform.position = record.before.position;
            scene::FlushWorldTransforms(scene);
            reason = "接続されたScriptの参照を安全に確認できません";
            return false;
        }
    }
    if (ctx.markSceneDirty) ctx.markSceneDirty();
    reason.clear(); return true;
}

OpResult ExecuteMove(OpContext& context, const OpArgs& args)
{
    auto& ctx = context.ctx;
    const auto* moves = args.GetData("moves");
    if (!IsControlPointAuthoringAllowed(ctx) || !moves || !moves->IsArray() || moves->AsArray().empty())
        return OpResult::Err("BAD_ARG", "movesに制御点とローカル位置を指定してください");
    if (moves->AsArray().size() > scene::MAX_CONTROL_POINT_BINDINGS)
        return OpResult::Err("LIMIT", "一度に移動する制御点の上限を越えています");
    const auto sceneGeneration = ctx.activeScene->GetRenderSceneGeneration();
    uint64_t expected = 0;
    if (args.Has("scene") && (!ReadUnsigned(args.GetString("scene"), expected) || expected != sceneGeneration))
        return OpResult::Err("STALE_SCENE", "制御点のSceneが変わりました");
    if (args.Has("revision") && (!ReadUnsigned(args.GetString("revision"), expected) || expected != context.undo.GetRevision()))
        return OpResult::Err("STALE_HISTORY", "編集中にUndo履歴が変更されました");
    auto plan = std::make_shared<MovePlan>();
    plan->context = &ctx; plan->sceneGeneration = sceneGeneration;
    bool marker = false, changed = false;
    std::unordered_set<uint64_t> ids;
    for (const auto& row : moves->AsArray()) {
        const auto* node = row.Find("node");
        auto* target = node && node->IsString() ? ctx.activeScene->FindByGuid(node->AsString()) : nullptr;
        if (!target || ctx.IsLocked(target->GetID())) return OpResult::Err("NO_TARGET", "移動する点が欠損またはロックされています");
        if (!ValidHierarchy(*target)) return OpResult::Err("INVALID_TRANSFORM", "制御点または祖先のローカル姿勢が不正です");
        if (!ids.insert(EntityKey(target->GetID())).second)
            return OpResult::Err("DUPLICATE_TARGET", "同じ点を複数回指定できません");
        auto record = CaptureRecord(*ctx.activeScene, *target);
        if (!Finite(record.before.position) || !Invertible(record.before)
            || (record.parent.IsValid() && !Invertible(record.parentTransform)))
            return OpResult::Err("INVALID_TRANSFORM", "制御点または親のTransformが不正です");
        if (!ReadVector(row.Find("position"), record.afterPosition)) return OpResult::Err("BAD_POSITION", "positionは有限なローカル座標3成分です");
        if (const auto* before = row.Find("expectedPosition")) {
            math::Vector3 position, scale;
            uint64_t lifetime = 0, generation = 0;
            const auto* life = row.Find("markerLifetime"); const auto* gen = row.Find("entityGeneration");
            const auto* parent = row.Find("parent");
            if (!ReadVector(before, position) || position != record.before.position
                || !ReadVector(row.Find("expectedScale"), scale) || scale != record.before.scale
                || !MatchesRotation(row.Find("expectedRotation"), record.before.rotation)
                || !life || !ReadUnsigned(life->AsString(), lifetime) || lifetime != record.markerLifetime
                || !gen || !ReadUnsigned(gen->AsString(), generation) || generation != record.entity.generation
                || !parent || parent->AsString() != record.parentGuid)
                return OpResult::Err("STALE_TARGET", "制御点の開始姿勢または寿命が変わりました");
        }
        marker |= record.markerLifetime != 0 || HasTargetPointSelection(ctx);
        changed |= record.afterPosition != record.before.position;
        plan->records.push_back(std::move(record));
    }
    if (!marker) return OpResult::Err("NO_TARGET_POINT", "TargetPointを含む選択が必要です");
    for (const auto& record : plan->records) {
        for (const auto* parent = ctx.activeScene->GetGameObject(record.entity)->GetParent(); parent; parent = parent->GetParent())
            if (ids.contains(EntityKey(parent->GetID())))
                return OpResult::Err("ANCESTOR_TARGET", "選択した祖先と子へ二重に移動を適用できません");
    }
    OpResult result;
    if (!changed) { result.noChange = true; return result; }
    std::sort(plan->records.begin(), plan->records.end(), [](const auto& lhs, const auto& rhs) { return lhs.guid < rhs.guid; });
    scene::FlushWorldTransforms(*ctx.activeScene);
    plan->beforeAffected = CaptureAffectedObjects(*ctx.activeScene, plan->records);
    if (!CaptureOwners(*ctx.activeScene, plan->beforeAffected, plan->beforeOwners))
        return OpResult::Err("REFERENCE_SCAN_FAILED", "接続されたScriptの参照を安全に確認できません");
    auto command = std::make_unique<CheckedLambdaCommand>("Move Target Points",
        [plan](std::string& reason) { return ApplyMove(plan, true, reason); },
        [plan](std::string& reason) { return ApplyMove(plan, false, reason); });
    std::string reason;
    if (!command->TryExecute(reason)) return OpResult::Err("STALE_TARGET", std::move(reason));
    result.command = std::move(command);
    return result;
}
} /// @note namespace

bool IsControlPointAuthoringAllowed(const EditorContext& ctx)
{
    return ctx.activeScene && ctx.undoStack && ctx.undoStack->IsRecordingEnabled()
        && !ctx.managedBuildBusy && (!ctx.playMode || (ctx.playMode->IsInEditor() && !ctx.playMode->HasPendingRestore()));
}

bool HasTargetPointSelection(const EditorContext& ctx)
{
    if (!ctx.activeScene) return false;
    return std::any_of(ctx.selectedEntities.begin(), ctx.selectedEntities.end(), [&ctx](scene::EntityID entity) {
        auto* object = ctx.activeScene->GetGameObject(entity);
        return object && object->GetComponent<scene::TargetPointComponent>();
    });
}

uint64_t ControlPointOwnerReferenceSignature(scene::Scene& scene, scene::EntityID owner)
{
    auto* object = scene.GetGameObject(owner);
    return object ? OwnerIdentitySignature(scene, *object) : 0;
}

void RegisterTargetPointMoveOperator(OperatorRegistry& registry)
{
    EditorOperator op;
    op.id = "target_point.move"; op.label = "Move Target Points"; op.category = "Control Points";
    op.desc = "制御点のローカル位置を一括変更し、一件の検証付きUndoにする。";
    op.kind = OpKind::Mutation; op.undoLabel = "Move Target Points";
    OpParam moves; moves.name = "moves"; moves.type = OpParamType::Data;
    moves.desc = "{node,position}の配列。候補編集は開始姿勢と寿命も渡す。";
    OpParam scene; scene.name = "scene"; scene.type = OpParamType::String; scene.required = false;
    OpParam revision; revision.name = "revision"; revision.type = OpParamType::String; revision.required = false;
    op.params = {moves, scene, revision};
    op.poll = [](const OpContext& ctx, const OpArgs&) { return IsControlPointAuthoringAllowed(ctx.ctx); };
    op.exec = ExecuteMove;
    registry.Register(std::move(op));

    EditorOperator connections;
    connections.id = "target_point.inspect_connections";
    connections.label = "Inspect Target Point Connections";
    connections.category = "Control Points";
    connections.desc = "配置点への参照を持つオブジェクトを調べる。親子関係だけの接続は含めない。";
    connections.kind = OpKind::Query;
    OpParam target; target.name = "target"; target.type = OpParamType::NodeId;
    target.desc = "同じSceneのTargetPoint GUID";
    connections.params = {target};
    connections.poll = [](const OpContext& context, const OpArgs&) { return context.ctx.activeScene != nullptr; };
    /// @note 明示確認時だけSceneを走査する。保存済みScriptのGUID文字列は保守的な参照候補として扱う。
    /// @see Docs/design/control-point-authoring.md Editorでの操作手順。
    connections.exec = [](OpContext& context, const OpArgs& args) {
        auto& scene = *context.ctx.activeScene;
        auto* point = scene.FindByGuid(args.GetString("target"));
        if (!point || !point->GetComponent<scene::TargetPointComponent>())
            return OpResult::Err("NO_TARGET_POINT", "確認するTargetPointが見つかりません");
        AffectedTargets affected;
        affected.guids.insert(point->instanceId);
        affected.entities.insert(EntityKey(point->GetID()));
        auto owners = OpData::MakeArray();
        for (auto& object : scene.GameObjects()) {
            OwnerStamp stamp;
            bool hasReferences = false;
            if (!CaptureOwner(scene, object, affected, stamp, hasReferences))
                return OpResult::Err("REFERENCE_INSPECTION_FAILED", "参照の確認を完了できません");
            if (!hasReferences) continue;
            auto owner = OpData::MakeObject();
            owner.Set("guid", object.instanceId);
            owner.Set("name", object.name);
            owners.Push(std::move(owner));
        }
        OpResult result;
        result.data.Set("owners", std::move(owners));
        return result;
    };
    registry.Register(std::move(connections));
}

struct ControlPointMoveSession::Impl {
    uint64_t sceneGeneration = 0;
    size_t revision = 0;
    std::vector<scene::EntityID> selection;
    std::vector<MoveRecord> records;
    std::vector<AffectedObjectStamp> affected;
    std::vector<OwnerStamp> owners;
    std::vector<ControlPointPreview> previews;
    std::string editOwner;
    math::Vector3 pivot{};
    int gizmoMode = 0, gizmoSpace = 0, gizmoPivot = 0;
    bool active = false;
};

ControlPointMoveSession::ControlPointMoveSession() : m_impl(std::make_unique<Impl>()) {}
ControlPointMoveSession::~ControlPointMoveSession() = default;
ControlPointMoveSession::ControlPointMoveSession(ControlPointMoveSession&&) noexcept = default;
ControlPointMoveSession& ControlPointMoveSession::operator=(ControlPointMoveSession&&) noexcept = default;

bool ControlPointMoveSession::Begin(EditorContext& ctx)
{
    Cancel();
    if (!IsControlPointAuthoringAllowed(ctx) || !HasTargetPointSelection(ctx)) return false;
    for (const auto entity : ctx.selectedEntities)
        if (auto* object = ctx.activeScene->GetGameObject(entity); object && !ValidHierarchy(*object)) return false;
    scene::FlushWorldTransforms(*ctx.activeScene);
    auto& state = *m_impl;
    state.sceneGeneration = ctx.activeScene->GetRenderSceneGeneration(); state.revision = ctx.undoStack->GetRevision();
    state.selection = ctx.selectedEntities; state.editOwner = ctx.controlPointOwnerGuid;
    state.gizmoMode = static_cast<int>(ctx.gizmoMode); state.gizmoSpace = static_cast<int>(ctx.gizmoSpace);
    state.gizmoPivot = static_cast<int>(ctx.gizmoPivot);
    std::unordered_set<uint64_t> selectedIds;
    for (const auto entity : ctx.selectedEntities) selectedIds.insert(EntityKey(entity));
    for (const auto entity : ctx.selectedEntities) {
        auto* object = ctx.activeScene->GetGameObject(entity);
        if (!object || ctx.IsLocked(entity)) continue;
        bool selectedAncestor = false;
        for (const auto* parent = object->GetParent(); parent; parent = parent->GetParent())
            if (selectedIds.contains(EntityKey(parent->GetID()))) selectedAncestor = true;
        if (selectedAncestor) continue;
        auto record = CaptureRecord(*ctx.activeScene, *object);
        if (!Finite(record.before.position) || !Invertible(record.before)
            || (record.parent.IsValid() && !Invertible(record.parentTransform))) { Cancel(); return false; }
        state.records.push_back(std::move(record));
    }
    if (state.records.empty()) return false;
    std::sort(state.records.begin(), state.records.end(), [](const auto& lhs, const auto& rhs) { return lhs.guid < rhs.guid; });
    if (auto* primary = ctx.GetSelectedGO()) state.pivot = primary->transform.worldPosition;
    state.affected = CaptureAffectedObjects(*ctx.activeScene, state.records);
    if (!CaptureOwners(*ctx.activeScene, state.affected, state.owners)) { Cancel(); return false; }
    state.active = true;
    return SetWorldDelta(ctx, math::Vector3::ZERO);
}

bool ControlPointMoveSession::Validate(EditorContext& ctx) const
{
    const auto& state = *m_impl;
    if (!state.active || !IsControlPointAuthoringAllowed(ctx)
        || state.sceneGeneration != ctx.activeScene->GetRenderSceneGeneration()) return false;
    for (const auto& record : state.records)
        if (auto* object = ctx.activeScene->GetGameObject(record.entity); !object || !ValidHierarchy(*object)) return false;
    scene::FlushWorldTransforms(*ctx.activeScene);
    return state.active && IsControlPointAuthoringAllowed(ctx)
        && state.sceneGeneration == ctx.activeScene->GetRenderSceneGeneration()
        && state.revision == ctx.undoStack->GetRevision() && state.selection == ctx.selectedEntities
        && state.editOwner == ctx.controlPointOwnerGuid && state.gizmoMode == static_cast<int>(ctx.gizmoMode)
        && state.gizmoSpace == static_cast<int>(ctx.gizmoSpace) && state.gizmoPivot == static_cast<int>(ctx.gizmoPivot)
        && std::all_of(state.records.begin(), state.records.end(), [&ctx](const MoveRecord& record) { return RecordMatches(*ctx.activeScene, record, false); })
        && SameAffectedObjects(CaptureAffectedObjects(*ctx.activeScene, state.records), state.affected)
        && OwnersMatch(*ctx.activeScene, state.owners, state.affected);
}

bool ControlPointMoveSession::SetWorldDelta(EditorContext& ctx, const math::Vector3& delta)
{
    if (!Finite(delta) || !Validate(ctx)) { Cancel(); return false; }
    auto& state = *m_impl; state.previews.clear();
    auto* editOwner = state.editOwner.empty() ? nullptr : ctx.activeScene->FindByGuid(state.editOwner);
    const auto* water = editOwner ? editOwner->GetComponent<scene::WaterComponent>() : nullptr;
    for (auto& record : state.records) {
        auto worldDelta = delta;
        if (water && water->surfaceShape == scene::WaterSurfaceShape::SPLINE_LAKE
            && std::any_of(water->pointBindings.begin(), water->pointBindings.end(), [&record](const auto& binding) { return binding.target.guid == record.guid; })) {
            if (!Invertible(editOwner->transform)) { Cancel(); return false; }
            auto local = editOwner->transform.worldRotation.Inverse() * worldDelta;
            local = {local.x / editOwner->transform.worldScale.x, 0.0f, local.z / editOwner->transform.worldScale.z};
            worldDelta = editOwner->transform.worldRotation * math::Vector3{
                local.x * editOwner->transform.worldScale.x, local.y * editOwner->transform.worldScale.y,
                local.z * editOwner->transform.worldScale.z};
        }
        auto localDelta = worldDelta;
        if (record.parent.IsValid()) {
            localDelta = record.parentTransform.worldRotation.Inverse() * worldDelta;
            localDelta = {localDelta.x / record.parentTransform.worldScale.x,
                          localDelta.y / record.parentTransform.worldScale.y,
                          localDelta.z / record.parentTransform.worldScale.z};
        }
        record.afterPosition = record.before.position + localDelta;
        if (!Finite(record.afterPosition)) { Cancel(); return false; }
        state.previews.push_back({record.entity, record.before.worldPosition + worldDelta, worldDelta});
    }
    for (const auto& object : state.affected) {
        if (!object.affectedTarget || std::any_of(state.records.begin(), state.records.end(), [&object](const MoveRecord& record) {
                return record.entity == object.entity;
            })) continue;
        const auto root = std::lower_bound(state.records.begin(), state.records.end(), object.rootGuid,
            [](const MoveRecord& record, const std::string& guid) { return record.guid < guid; });
        if (root == state.records.end() || root->guid != object.rootGuid) continue;
        const auto preview = std::find_if(state.previews.begin(), state.previews.end(), [root](const auto& value) {
            return value.entity == root->entity;
        });
        if (preview != state.previews.end()) {
            const auto worldDelta = preview->worldDelta;
            state.previews.push_back({object.entity, object.pose.worldPosition + worldDelta, worldDelta});
        }
    }
    std::sort(state.previews.begin(), state.previews.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.entity.index < rhs.entity.index;
    });
    return true;
}

bool ControlPointMoveSession::Commit(EditorContext& ctx)
{
    if (!Validate(ctx)) { Cancel(); return false; }
    auto moves = OpData::MakeArray();
    for (const auto& record : m_impl->records) {
        auto row = OpData::MakeObject();
        row.Set("node", record.guid); row.Set("position", record.afterPosition);
        row.Set("expectedPosition", record.before.position); row.Set("expectedScale", record.before.scale);
        row.Set("expectedRotation", RotationData(record.before.rotation)); row.Set("parent", record.parentGuid);
        row.Set("markerLifetime", std::to_string(record.markerLifetime)); row.Set("entityGeneration", std::to_string(record.entity.generation));
        moves.Push(std::move(row));
    }
    OpArgs args; args.Set("moves", std::move(moves)); args.Set("scene", std::to_string(m_impl->sceneGeneration));
    args.Set("revision", std::to_string(m_impl->revision));
    const bool success = InvokeOperator(ctx, "target_point.move", args).ok;
    Cancel(); return success;
}

void ControlPointMoveSession::Cancel()
{
    if (!m_impl) return;
    m_impl->active = false; m_impl->records.clear(); m_impl->affected.clear();
    m_impl->owners.clear(); m_impl->previews.clear();
}

bool ControlPointMoveSession::Active() const { return m_impl && m_impl->active; }
math::Vector3 ControlPointMoveSession::Pivot() const { return m_impl->pivot; }
const std::vector<ControlPointPreview>& ControlPointMoveSession::Preview() const { return m_impl->previews; }
} /// @note namespace fbzz::editor
