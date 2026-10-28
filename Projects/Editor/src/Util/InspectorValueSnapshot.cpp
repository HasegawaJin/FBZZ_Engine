/// @file    InspectorValueSnapshot.cpp
/// @brief   Reflection の値・構造体配列を、Component runtime から分離して保存する。
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <Editor/Util/InspectorValueSnapshot.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/SpringBoneComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/NavMeshAgentComponent.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>

namespace fbzz::editor {

class InspectorValueSnapshot::ValueReflector final : public scene::IReflector {
public:
    explicit ValueReflector(InspectorValueSnapshot& owner) : m_owner(owner) {}
    void Field(const char* name, float& value) override { m_owner.Value(name, value); }
    void Field(const char* name, int& value) override { m_owner.Value(name, value); }
    void Field(const char* name, bool& value) override { m_owner.Value(name, value); }
    void Field(const char* name, std::string& value) override { m_owner.Value(name, value); }
    void Field(const char* name, math::Vector2& value) override { m_owner.Value(name, value); }
    void Field(const char* name, math::Vector3& value) override { m_owner.Value(name, value); }
    void Field(const char* name, math::Vector4& value) override { m_owner.Value(name, value); }
    void Field(const char* name, math::Quaternion& value) override { m_owner.Value(name, value); }
    void Field(const char* name, scene::EntityID& value) override { m_owner.Value(name, value); }
    void Field(const char* name, scene::EntityRef& value) override { m_owner.Value(name, value); }
    void Field(const char* name, input::KeyCode& value) override { m_owner.Value(name, value); }
    void Field(const char* name, scene::ParticleCurve& value) override { m_owner.Value(name, value); }
    void Field(const char* name, scene::ParticleGradient& value) override { m_owner.Value(name, value); }
    void ListField(const char* name, std::vector<float>& values) override { m_owner.Value(name, values); }
    void ListField(const char* name, std::vector<int>& values) override { m_owner.Value(name, values); }
    void ListField(const char* name, std::vector<bool>& values) override { m_owner.Value(name, values); }
    void ListField(const char* name, std::vector<std::string>& values) override { m_owner.Value(name, values); }
    void ListField(const char* name, std::vector<math::Vector2>& values) override { m_owner.Value(name, values); }
    void ListField(const char* name, std::vector<math::Vector3>& values) override { m_owner.Value(name, values); }
    void ListField(const char* name, std::vector<math::Vector4>& values) override { m_owner.Value(name, values); }
    void ListField(const char* name, std::vector<scene::EntityRef>& values) override { m_owner.Value(name, values); }
    void AssetField(const char* name, scene::ScriptAssetReference& value, scene::ScriptAssetType) override
    { m_owner.Value(name, value); }
    void AssetListField(const char* name, std::vector<scene::ScriptAssetReference>& values, scene::ScriptAssetType) override
    { m_owner.Value(name, values); }
    void ObjectReferenceField(const char* name, scene::ScriptObjectReference& value) override { m_owner.Value(name, value); }
    void ObjectReferenceListField(const char* name, std::vector<scene::ScriptObjectReference>& values) override
    { m_owner.Value(name, values); }
    size_t BeginObjectList(const char* name, size_t count) override { m_owner.Value(name, count); return count; }
private:
    InspectorValueSnapshot& m_owner;
};

InspectorValueSnapshot::InspectorValueSnapshot() : m_reflector(std::make_unique<ValueReflector>(*this)) {}
InspectorValueSnapshot::~InspectorValueSnapshot() = default;
void InspectorValueSnapshot::BeginCapture() { m_cursor = 0; m_applying = false; }
void InspectorValueSnapshot::EndCapture() { m_entries.resize(m_cursor); }
void InspectorValueSnapshot::BeginApply() { m_cursor = 0; m_applying = true; }
void InspectorValueSnapshot::Clear() { m_entries.clear(); m_cursor = 0; m_applying = false; }
scene::IReflector& InspectorValueSnapshot::Reflector() { return *m_reflector; }

void ProjectInspectorUndoValues(scene::SpringBoneComponent& component, InspectorValueSnapshot& values)
{
    auto& reflector = values.Reflector();
    component.chains.resize(reflector.BeginObjectList("springChains", component.chains.size()));
    for (auto& chain : component.chains) {
        values.Value("enabled", chain.enabled);
        values.Value("rootBoneName", chain.rootBoneName);
        values.Value("maxDepth", chain.maxDepth);
        values.Value("stiffness", chain.stiffness);
        values.Value("damping", chain.damping);
        values.Value("gravityPower", chain.gravityPower);
        values.Value("gravityDirection", chain.gravityDirection);
        values.Value("radius", chain.radius);
        values.Value("limitAngle", chain.limitAngle);
        values.Value("weight", chain.weight);
        values.Value("leafTailLength", chain.leafTailLength);
    }
    values.Value("springColliders", component.colliders);
    values.Value("enabled", component.enabled);
    values.Value("simulateInEditor", component.simulateInEditor);
    values.Value("teleportResetDistance", component.teleportResetDistance);
}

void ProjectInspectorUndoValues(scene::AnimatorComponent& component, InspectorValueSnapshot& values)
{
    values.Value("enabled", component.enabled);
    values.Value("controllerPath", component.controllerPath);
    values.Value("speed", component.speed);
    values.Value("playing", component.playing);
    values.Value("externalPose", component.externalPose);
    values.Value("rootMotion", component.rootMotion);
    values.Value("defaultStateName", component.defaultStateName);
}

void ProjectInspectorUndoValues(scene::SkinnedMeshRenderer& component, InspectorValueSnapshot& values)
{
    values.Value("model", component.model);
    values.Value("submeshIndices", component.submeshIndices);
    values.Value("enabled", component.enabled);
    values.Value("castShadows", component.castShadows);
    values.Value("modelPath", component.modelPath);
}

void ProjectInspectorUndoValues(scene::TrailComponent& component, InspectorValueSnapshot& values)
{
    values.Value("enabled", component.enabled);
    values.Value("duration", component.duration);
    values.Value("maxPoints", component.maxPoints);
    values.Value("sampleInterval", component.sampleInterval);
    values.Value("minVertexDist", component.minVertexDist);
    values.Value("widthStart", component.widthStart);
    values.Value("widthEnd", component.widthEnd);
    values.Value("breakDistance", component.breakDistance);
    values.Value("widthCurveEnabled", component.widthCurveEnabled);
    values.Value("widthCurve", component.widthCurve);
    values.Value("colorGradientEnabled", component.colorGradientEnabled);
    values.Value("colorGradient", component.colorGradient);
    values.Value("beamMode", component.beamMode);
    values.Value("beamStart", component.beamStart);
    values.Value("beamEnd", component.beamEnd);
    values.Value("widthEasing", component.widthEasing);
    values.Value("colorStart", component.colorStart);
    values.Value("colorEnd", component.colorEnd);
    values.Value("alignment", component.alignment);
    values.Value("smoothSubdivisions", component.smoothSubdivisions);
    values.Value("attachBone", component.attachBone);
    values.Value("attachOffset", component.attachOffset);
    values.Value("clearOnDisable", component.clearOnDisable);
    values.Value("materialPath", component.materialPath);
    values.Value("uvMode", component.uvMode);
    values.Value("uvScrollSpeed", component.uvScrollSpeed);
    values.Value("uvTiling", component.uvTiling);
}

void ProjectInspectorUndoValues(scene::NavMeshAgentComponent& component, InspectorValueSnapshot& values)
{
    values.Value("enabled", component.enabled);
    values.Value("radius", component.radius);
    values.Value("maxSpeed", component.maxSpeed);
    values.Value("acceleration", component.acceleration);
    values.Value("angularSpeedDeg", component.angularSpeedDeg);
    values.Value("stoppingDistance", component.stoppingDistance);
    values.Value("avoidancePriority", component.avoidancePriority);
    values.Value("agentTypeId", component.agentTypeId);
    values.Value("snapToNavMesh", component.snapToNavMesh);
    values.Value("updatePosition", component.updatePosition);
    values.Value("updateRotation", component.updateRotation);
    values.Value("autoBraking", component.autoBraking);
    values.Value("areaMask", component.areaMask);
    values.Value("target", component.target);
    values.Value("repathInterval", component.repathInterval);
}

void ProjectInspectorUndoValues(scene::NavMeshSurfaceComponent& component, InspectorValueSnapshot& values)
{
    values.Value("enabled", component.enabled);
    values.Value("size", component.size);
    values.Value("cellSize", component.cellSize);
    values.Value("maxSlopeAngleDeg", component.maxSlopeAngleDeg);
    values.Value("agentRadius", component.agentRadius);
    values.Value("agentHeight", component.agentHeight);
    values.Value("maxClimb", component.maxClimb);
    values.Value("agentTypeId", component.agentTypeId);
    values.Value("collectObjects", component.collectObjects);
    values.Value("areaCosts", component.areaCosts);
}

void ProjectInspectorUndoValues(scene::IKSolverComponent& component, InspectorValueSnapshot& values)
{
    values.Value("chains", component.chains);
    values.Value("enabled", component.enabled);
}

void ProjectInspectorUndoValues(scene::TerrainGridComponent& component, InspectorValueSnapshot& values)
{
    values.Value("cellInstanceIds", component.cellInstanceIds);
    values.Value("cellCountX", component.cellCountX);
    values.Value("cellCountZ", component.cellCountZ);
    values.Value("defaultColumns", component.defaultColumns);
    values.Value("defaultRows", component.defaultRows);
    values.Value("defaultCellSize", component.defaultCellSize);
    values.Value("defaultChunkSize", component.defaultChunkSize);
}

} /// @note namespace fbzz::editor
