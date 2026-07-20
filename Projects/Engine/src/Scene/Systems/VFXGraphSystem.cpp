// FBZZ Engine
// VFXGraphSystem.cpp | fbzz::scene
// .vfx DAGの読込、ランタイムGameObject生成、時間に沿ったComponent有効化
#include <Engine/Scene/Systems/VFXGraphSystem.hpp>
#include <Engine/Asset/VFXParameterRuntime.hpp>

#include <Engine/Asset/VFXGraphAsset.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/Model.hpp>
#include <Engine/Core/Logger.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Core/Time.hpp>
#include <Engine/Scene/Components/AudioSourceComponent.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/DecalComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/MeshTrailComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/ParticleEmitter.hpp>
#include <Engine/Scene/Components/RigidBodyComponent.hpp>
#include <Engine/Scene/Components/TrailComponent.hpp>
#include <Engine/Scene/Components/VFXGraphComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Systems/AudioSystem.hpp>
#include <Engine/Scene/Systems/ParticleSimulationSystem.hpp>
#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <algorithm>
#include <any>
#include <cmath>
#include <limits>
#include <span>
#include <unordered_set>
#include <vector>

namespace fbzz::scene {
namespace {

void SetNodeActive(Scene& scene, VFXRuntimeNodeState& state, bool active)
{
    GameObject* gameObject = scene.GetGameObject(state.entity);
    if (gameObject == nullptr) return;
    if (active) {
        gameObject->SetActive(true);
        if (auto* emitter = gameObject->GetComponent<ParticleEmitter>()) emitter->ResetPlayback();
        if (auto* graph = gameObject->GetComponent<VFXGraphComponent>()) graph->Restart();
        if (auto* audio = gameObject->GetComponent<AudioSourceComponent>()) {
            audio->m_played = false;
            audio->m_pendingPlay = true;
            audio->m_pendingStop = false;
        }
    } else {
        if (auto* emitter = gameObject->GetComponent<ParticleEmitter>()) {
            emitter->playing = false;
            emitter->particles.clear();
            emitter->gpuClearPending = true;
        }
        if (auto* audio = gameObject->GetComponent<AudioSourceComponent>()) {
            audio->m_pendingPlay = false;
            audio->m_pendingStop = true;
        }
        gameObject->SetActive(false);
    }
    state.active = active;
}

void DestroyRuntimeNodes(Scene& scene, VFXGraphComponent& component)
{
    for (const auto& state : component.runtimeNodes)
        if (scene.IsValid(state.entity)) scene.DestroyGameObject(state.entity);
    component.runtimeNodes.clear();
    component.runtimeLinks.clear();
    component.initialized = false;
    component.loadedGraphPath.clear();
    component.graphDuration = 0.0f;
    component.hasEventLinks = false;
    component.runtimeGraph.reset();
}

void ApplyTransform(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    constexpr float DEG_TO_RAD = 0.01745329251994329577f;
    gameObject.transform.position = node.localPosition;
    gameObject.transform.rotation = math::Quaternion::FromEuler(node.localRotationDegrees * DEG_TO_RAD);
    gameObject.transform.scale = node.localScale;
}

GameObject* FindVFXSocket(GameObject& root, std::string_view boneName)
{
    if (const auto* bone = root.GetComponent<BoneComponent>(); bone != nullptr && bone->boneName == boneName)
        return &root;
    for (int index = 0; index < root.GetChildCount(); ++index)
        if (GameObject* child = root.GetChild(index))
            if (GameObject* found = FindVFXSocket(*child, boneName)) return found;
    return nullptr;
}

void AddParticle(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    // authoring設定を丸ごと複製し、Graphの時間制御だけを上書きする。
    // WHY: モジュール追加時にランタイム生成コードへ転記を足す運用は保存漏れを再発させるため。
    ParticleEmitter emitter = node.particle;
    emitter.duration = node.duration;
    emitter.loop = false;
    emitter.startDelay = 0.0f;
    emitter.enabled = true;
    emitter.playing = true;
    emitter.ResetPlayback();
    gameObject.AddComponent<ParticleEmitter>(std::move(emitter));
    if (!node.particle.meshParticlePath.empty()) {
        MeshRenderer renderer;
        renderer.meshPath = node.particle.meshParticlePath;
        const auto model = asset::AssetManager::LoadModel(node.particle.meshParticlePath);
        if (model && !model->meshes.empty()) renderer.mesh = model->meshes.front().get();
        gameObject.AddComponent<MeshRenderer>(std::move(renderer));
        MeshTrailComponent meshParticles;
        meshParticles.materialPath = node.particle.materialPath;
        meshParticles.texturePath = node.particle.texturePath;
        meshParticles.duration = 0.01f;
        gameObject.AddComponent<MeshTrailComponent>(std::move(meshParticles));
    }
}

void AddTrail(GameObject& gameObject, const asset::VFXGraphNode& node, bool meshTrail)
{
    if (meshTrail) {
        if (!node.trail.meshPath.empty()) {
            MeshRenderer renderer;
            renderer.meshPath = node.trail.meshPath;
            const auto model = asset::AssetManager::LoadModel(node.trail.meshPath);
            if (model && !model->meshes.empty()) renderer.mesh = model->meshes.front().get();
            gameObject.AddComponent<MeshRenderer>(std::move(renderer));
        }
        MeshTrailComponent trail;
        trail.materialPath = node.trail.materialPath;
        trail.texturePath = node.trail.texturePath;
        trail.colorStart = node.trail.colorStart;
        trail.colorEnd = node.trail.colorEnd;
        trail.duration = node.trail.lifetime;
        gameObject.AddComponent<MeshTrailComponent>(std::move(trail));
        return;
    }
    TrailComponent trail;
    trail.materialPath = node.trail.materialPath;
    trail.texturePath = node.trail.texturePath;
    trail.colorStart = node.trail.colorStart;
    trail.colorEnd = node.trail.colorEnd;
    trail.duration = node.trail.lifetime;
    trail.widthStart = node.trail.widthStart;
    trail.widthEnd = node.trail.widthEnd;
    trail.beamMode = node.trail.beamMode;
    trail.beamStart = node.trail.beamStart;
    trail.beamEnd = node.trail.beamEnd;
    gameObject.AddComponent<TrailComponent>(std::move(trail));
}

void AddLight(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    LightComponent light;
    light.type = LightComponent::Type::Point;
    light.color = node.light.color;
    light.intensity = node.light.intensity;
    light.range = node.light.range;
    light.castShadows = false;
    gameObject.AddComponent<LightComponent>(light);
}

void AddAudio(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    AudioSourceComponent audio;
    audio.clipPath = node.audio.clipPath;
    audio.volume = node.audio.volume;
    audio.pitch = node.audio.pitch;
    audio.spatialBlend = node.audio.spatialBlend;
    audio.loop = node.audio.loop;
    audio.playOnAwake = true;
    gameObject.AddComponent<AudioSourceComponent>(std::move(audio));
}

void AddDecal(GameObject& gameObject, const asset::VFXGraphNode& node)
{
    DecalComponent decal;
    decal.albedoTexPath = node.decal.albedoPath;
    decal.normalTexPath = node.decal.normalPath;
    decal.emissiveTexPath = node.decal.emissivePath;
    decal.albedoColor[0] = node.decal.color.x;
    decal.albedoColor[1] = node.decal.color.y;
    decal.albedoColor[2] = node.decal.color.z;
    decal.albedoColor[3] = node.decal.color.w;
    decal.normalStrength = node.decal.normalStrength;
    decal.emissiveScale = node.decal.emissiveScale;
    decal.lifetime = node.duration;
    decal.fadeTime = node.decal.fadeTime;
    gameObject.AddComponent<DecalComponent>(std::move(decal));
}

void AddSubGraph(GameObject& gameObject, const asset::VFXGraphNode& node, int nestingDepth)
{
    VFXGraphComponent graph;
    graph.graphPath = node.subGraph.graphPath;
    graph.playOnAwake = false;
    graph.playing = false;
    graph.nestingDepth = nestingDepth;
    gameObject.AddComponent<VFXGraphComponent>(std::move(graph));
}

VFXRuntimeNodeState* FindRuntimeNode(VFXGraphComponent& component, int nodeId)
{
    for (auto& node : component.runtimeNodes)
        if (node.nodeId == nodeId) return &node;
    return nullptr;
}

void ResetRuntimeSchedule(Scene& scene, VFXGraphComponent& component)
{
    for (auto& node : component.runtimeNodes) {
        if (node.active) SetNodeActive(scene, node, false);
        node.startTime = node.scheduledStartTime;
        node.endTime = node.scheduledEndTime;
    }
    for (auto& link : component.runtimeLinks) {
        link.fired = false;
        if (link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnCollision)) {
            if (auto* target = FindRuntimeNode(component, link.toNode)) {
                target->startTime = (std::numeric_limits<float>::max)();
                target->endTime = target->startTime;
            }
        }
    }
}

void ApplyPlaybackScale(Scene& scene, VFXRuntimeNodeState& state,
                        float scale, bool editorPreview)
{
    GameObject* gameObject = scene.GetGameObject(state.entity);
    if (gameObject == nullptr) return;
    if (auto* emitter = gameObject->GetComponent<ParticleEmitter>()) {
        emitter->editorTimeScale = scale;
        emitter->editorTimeScaleFrame = Time::frameCount;
    }
    if (auto* graph = gameObject->GetComponent<VFXGraphComponent>()) {
        graph->speed = scale;
        if (editorPreview) graph->editorPreviewFrame = Time::frameCount;
    }
}

void ConsumeParticleEvents(Scene& scene, VFXGraphComponent& component)
{
    for (auto& link : component.runtimeLinks) {
        if (link.fired) continue;
        const bool collisionEvent = link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnCollision);
        const bool deathEvent = link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnDeath);
        if (!collisionEvent && !deathEvent) continue;
        VFXRuntimeNodeState* source = FindRuntimeNode(component, link.fromNode);
        VFXRuntimeNodeState* target = FindRuntimeNode(component, link.toNode);
        if (source == nullptr || target == nullptr) continue;
        GameObject* sourceObject = scene.GetGameObject(source->entity);
        const ParticleEmitter* emitter = sourceObject != nullptr
            ? sourceObject->GetComponent<ParticleEmitter>() : nullptr;
        if (emitter == nullptr || (collisionEvent ? emitter->collisionCountThisFrame : emitter->deathCountThisFrame) <= 0) continue;

        if (target->active) SetNodeActive(scene, *target, false);
        const float duration = target->scheduledEndTime - target->scheduledStartTime;
        target->startTime = component.playTime + link.delay;
        target->endTime = target->startTime + duration;
        component.graphDuration = (std::max)(component.graphDuration, target->endTime);
        link.fired = true;
    }
}

// ScriptのFBZZ_FIELDを読み取り専用で走査し、Attribute Bindingの汎用ゲーム属性面にする。
class VFXAttributeReflector final : public IReflector {
public:
    explicit VFXAttributeReflector(std::string_view field) : m_field(field) {}
    void Field(const char* name, float& value) override { Capture(name, value); }
    void Field(const char* name, int& value) override { Capture(name, value); }
    void Field(const char* name, bool& value) override { Capture(name, value); }
    void Field(const char* name, math::Vector2& value) override { Capture(name, value); }
    void Field(const char* name, math::Vector3& value) override { Capture(name, value); }
    void Field(const char* name, math::Vector4& value) override { Capture(name, value); }
    void Field(const char* name, std::string& value) override { Capture(name, value); }
    void Field(const char* name, math::Quaternion& value) override { Capture(name, value); }
    [[nodiscard]] const std::any& Value() const { return m_value; }
private:
    template<typename T> void Capture(const char* name, const T& value)
    {
        if (!m_value.has_value() && name != nullptr && m_field == name) m_value = value;
    }
    std::string_view m_field;
    std::any m_value;
};

bool CoerceVFXAttribute(const std::any& input, reflection::PropertyType target, std::any& output)
{
    if (target == reflection::PropertyType::Float) {
        if (const auto* value = std::any_cast<float>(&input)) output = *value;
        else if (const auto* value = std::any_cast<int>(&input)) output = static_cast<float>(*value);
        else if (const auto* value = std::any_cast<bool>(&input)) output = *value ? 1.0f : 0.0f;
    } else if (target == reflection::PropertyType::Int) {
        if (const auto* value = std::any_cast<int>(&input)) output = *value;
        else if (const auto* value = std::any_cast<float>(&input)) output = static_cast<int>(*value);
        else if (const auto* value = std::any_cast<bool>(&input)) output = *value ? 1 : 0;
    } else if (target == reflection::PropertyType::Bool) {
        if (const auto* value = std::any_cast<bool>(&input)) output = *value;
        else if (const auto* value = std::any_cast<float>(&input)) output = *value != 0.0f;
        else if (const auto* value = std::any_cast<int>(&input)) output = *value != 0;
    } else if (target == reflection::PropertyType::Vector3) {
        if (const auto* value = std::any_cast<math::Vector3>(&input)) output = *value;
        else if (const auto* value = std::any_cast<math::Vector4>(&input)) output = math::Vector3{ value->x, value->y, value->z };
    } else if (target == reflection::PropertyType::Color) {
        if (const auto* value = std::any_cast<math::Vector4>(&input)) output = *value;
        else if (const auto* value = std::any_cast<math::Vector3>(&input)) output = math::Vector4{ value->x, value->y, value->z, 1.0f };
    } else if (target == reflection::PropertyType::String || target == reflection::PropertyType::AssetRef) {
        if (const auto* value = std::any_cast<std::string>(&input)) output = *value;
    } else {
        output = input;
    }
    return output.has_value();
}

bool ResolveVFXAttribute(Scene& scene, GameObject& owner, std::string path,
                         reflection::PropertyType target, std::any& output)
{
    GameObject* source = &owner;
    if (path.starts_with("self.")) path.erase(0, 5);
    else if (path.starts_with("parent.")) { source = owner.GetParent(); path.erase(0, 7); }
    else if (path.starts_with("guid:")) {
        const std::size_t separator = path.find('.');
        if (separator == std::string::npos) return false;
        source = scene.FindByGuid(path.substr(5, separator - 5));
        path.erase(0, separator + 1);
    }
    if (source == nullptr) return false;

    std::any raw;
    if (path == "transform.position") raw = source->transform.position;
    else if (path == "transform.worldPosition") raw = source->transform.worldPosition;
    else if (path == "transform.scale") raw = source->transform.scale;
    else if (path == "physics.velocity" || path == "physics.speed"
             || path == "physics.angularVelocity") {
        const auto* body = source->GetComponent<RigidBodyComponent>();
        if (body == nullptr || body->rigidBody == nullptr) return false;
        const math::Vector3 value = path == "physics.angularVelocity"
            ? body->rigidBody->GetAngularVelocity() : body->rigidBody->GetVelocity();
        raw = path == "physics.speed" ? std::any{ value.Length() } : std::any{ value };
    } else if (path.starts_with("animator.")) {
        const auto* animator = source->GetComponent<AnimatorComponent>();
        if (animator == nullptr) return false;
        const std::string name = path.substr(9);
        const auto parameter = std::find_if(animator->parameters.begin(), animator->parameters.end(),
            [&](const AnimatorParameter& value) { return value.name == name; });
        if (parameter == animator->parameters.end()) return false;
        if (parameter->type == ParamType::Float) raw = parameter->floatValue;
        else if (parameter->type == ParamType::Int) raw = parameter->intValue;
        else raw = parameter->boolValue;
    } else if (path.starts_with("script.")) {
        const std::size_t typeEnd = path.find('.', 7);
        if (typeEnd == std::string::npos) return false;
        const std::string typeName = path.substr(7, typeEnd - 7);
        const std::string fieldName = path.substr(typeEnd + 1);
        auto* scripts = source->GetComponent<ScriptComponent>();
        if (scripts == nullptr) return false;
        for (auto& entry : scripts->scripts) {
            if (entry.script == nullptr || typeName != entry.script->GetTypeName()) continue;
            VFXAttributeReflector reflector(fieldName);
            entry.script->Reflect(reflector);
            raw = reflector.Value();
            break;
        }
    }
    return raw.has_value() && CoerceVFXAttribute(raw, target, output);
}

void ApplyRuntimeNodeSettings(Scene& scene, const VFXRuntimeNodeState& state,
                              const asset::VFXGraphNode& node, std::string_view schemaPath)
{
    GameObject* object = scene.GetGameObject(state.entity);
    if (object == nullptr) return;
    if (schemaPath == "localPosition" || schemaPath == "localRotationDegrees" || schemaPath == "localScale")
        ApplyTransform(*object, node);
    if (node.type == asset::VFXNodeType::Particle && schemaPath.starts_with("particle.")) {
        if (auto* emitter = object->GetComponent<ParticleEmitter>()) {
            reflection::ResolvedProperty sourceProperty;
            reflection::ResolvedProperty targetProperty;
            const std::string_view leaf = schemaPath.substr(9);
            if (reflection::ResolveProperty(asset::GetParticleEmitterSchema(), &node.particle, leaf, sourceProperty)
                && reflection::ResolveProperty(asset::GetParticleEmitterSchema(), emitter, leaf, targetProperty)
                && sourceProperty.property != nullptr && targetProperty.property != nullptr)
                targetProperty.property->set(targetProperty.owner, sourceProperty.property->get(sourceProperty.constOwner));
        }
    } else if (node.type == asset::VFXNodeType::Light) {
        if (auto* light = object->GetComponent<LightComponent>()) {
            light->color = node.light.color; light->intensity = node.light.intensity; light->range = node.light.range;
        }
    } else if (node.type == asset::VFXNodeType::Audio) {
        if (auto* audio = object->GetComponent<AudioSourceComponent>()) {
            audio->clipPath = node.audio.clipPath; audio->volume = node.audio.volume; audio->pitch = node.audio.pitch;
            audio->spatialBlend = node.audio.spatialBlend; audio->loop = node.audio.loop;
        }
    } else if (node.type == asset::VFXNodeType::Trail) {
        if (auto* trail = object->GetComponent<TrailComponent>()) {
            trail->materialPath = node.trail.materialPath; trail->texturePath = node.trail.texturePath;
            trail->colorStart = node.trail.colorStart; trail->colorEnd = node.trail.colorEnd;
            trail->duration = node.trail.lifetime; trail->widthStart = node.trail.widthStart; trail->widthEnd = node.trail.widthEnd;
            trail->beamMode = node.trail.beamMode; trail->beamStart = node.trail.beamStart; trail->beamEnd = node.trail.beamEnd;
        }
    } else if (node.type == asset::VFXNodeType::MeshTrail) {
        if (auto* trail = object->GetComponent<MeshTrailComponent>()) {
            trail->materialPath = node.trail.materialPath; trail->texturePath = node.trail.texturePath;
            trail->colorStart = node.trail.colorStart; trail->colorEnd = node.trail.colorEnd; trail->duration = node.trail.lifetime;
        }
    } else if (node.type == asset::VFXNodeType::Decal) {
        if (auto* decal = object->GetComponent<DecalComponent>()) {
            decal->albedoTexPath = node.decal.albedoPath; decal->normalTexPath = node.decal.normalPath;
            decal->emissiveTexPath = node.decal.emissivePath;
            decal->albedoColor[0] = node.decal.color.x; decal->albedoColor[1] = node.decal.color.y;
            decal->albedoColor[2] = node.decal.color.z; decal->albedoColor[3] = node.decal.color.w;
            decal->normalStrength = node.decal.normalStrength; decal->emissiveScale = node.decal.emissiveScale;
            decal->fadeTime = node.decal.fadeTime;
        }
    } else if (node.type == asset::VFXNodeType::SubGraph) {
        if (auto* graph = object->GetComponent<VFXGraphComponent>(); graph && graph->graphPath != node.subGraph.graphPath) {
            graph->graphPath = node.subGraph.graphPath; graph->reloadRequested = true;
        }
    }
}

void ApplyDynamicVFXBindings(Scene& scene, GameObject& owner, VFXGraphComponent& component)
{
    if (component.runtimeGraph == nullptr) return;
    const asset::VFXGraphAsset& graph = *component.runtimeGraph;
    const float normalizedTime = component.graphDuration > 0.0f
        ? std::clamp(component.playTime / component.graphDuration, 0.0f, 1.0f) : 0.0f;
    for (const auto& binding : graph.bindings) {
        const auto definition = asset::FindVFXParameter(graph, binding.paramName);
        const auto source = definition != nullptr ? asset::ResolveVFXParamSource(graph, component, *definition) : nullptr;
        if (source == nullptr || std::holds_alternative<asset::VFXConstant>(source->source)
            || std::holds_alternative<asset::VFXRandomRange>(source->source)) continue;
        const auto authoring = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const auto& node) { return node.id == binding.nodeId; });
        const auto runtime = std::find_if(component.runtimeNodes.begin(), component.runtimeNodes.end(),
            [&](const auto& node) { return node.nodeId == binding.nodeId; });
        if (authoring == graph.nodes.end() || runtime == component.runtimeNodes.end()) continue;
        asset::VFXGraphNode node = *authoring;
        reflection::ResolvedProperty property;
        if (!reflection::ResolveProperty(asset::GetVFXNodeSchema(), &node, binding.schemaPath, property)
            || property.property == nullptr) continue;
        std::any value;
        bool resolved = false;
        if (const auto* attribute = std::get_if<asset::VFXAttributeRef>(&source->source))
            resolved = ResolveVFXAttribute(scene, owner, attribute->path, property.property->type, value);
        else
            resolved = asset::EvaluateVFXParamValue(graph, *source, *property.property, normalizedTime,
                node.particle.randomSeed, definition->name, value);
        if (!resolved || !property.property->set(property.owner, value)) continue;
        ApplyRuntimeNodeSettings(scene, *runtime, node, binding.schemaPath);
    }
}

bool InitializeGraph(Scene& scene, GameObject& owner, VFXGraphComponent& component)
{
    constexpr int MAX_NESTING_DEPTH = 8;
    if (component.nestingDepth > MAX_NESTING_DEPTH) {
        FBZZ_LOG_WARN("VFX Graph nesting exceeded %d: %s", MAX_NESTING_DEPTH, component.graphPath.c_str());
        return false;
    }
    asset::VFXGraphAsset graph;
    std::string error;
    if (!asset::LoadVFXGraphAsset(component.graphPath, graph, &error)) {
        FBZZ_LOG_WARN("VFX Graph load failed: %s (%s)", component.graphPath.c_str(), error.c_str());
        return false;
    }
    std::vector<float> startTimes;
    if (!asset::BuildVFXGraphSchedule(graph, startTimes, component.graphDuration, &error)) {
        FBZZ_LOG_WARN("VFX Graph schedule failed: %s (%s)", component.graphPath.c_str(), error.c_str());
        return false;
    }
    component.resolvedParameters.clear();
    for (const auto& definition : graph.parameters) {
        const auto* source = asset::ResolveVFXParamSource(graph, component, definition);
        if (source != nullptr) component.resolvedParameters.push_back({ definition.name, *source });
    }
    component.runtimeNodes.clear();
    component.runtimeLinks.clear();
    std::unordered_set<int> eventTargets;
    std::unordered_set<int> eventSources;
    for (const auto& link : graph.links) {
        component.runtimeLinks.push_back({ link.fromNode, link.toNode,
            static_cast<std::uint8_t>(link.trigger), link.delay, false });
        if (link.trigger == asset::VFXLinkTrigger::OnCollision || link.trigger == asset::VFXLinkTrigger::OnDeath) {
            eventTargets.insert(link.toNode);
            eventSources.insert(link.fromNode);
        }
    }
    component.hasEventLinks = !eventTargets.empty();
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        asset::VFXGraphNode node = graph.nodes[index];
        // GPUシミュレーションは粒子死/衝突カウンタをCPUへreadbackしない。
        // イベント源だけCPUへ縮退させ、グラフの発火意味論と決定論を黙って失わないようにする。
        if (node.type == asset::VFXNodeType::Particle && eventSources.contains(node.id))
            node.particle.simulationMode = ParticleSimulationMode::Cpu;
        const float normalizedTime = component.graphDuration > 0.0f
            ? std::clamp(startTimes[index] / component.graphDuration, 0.0f, 1.0f) : 0.0f;
        asset::ApplyVFXBindings(graph, component, node, normalizedTime);
        if (node.type == asset::VFXNodeType::Entry || node.type == asset::VFXNodeType::Delay) {
            component.runtimeNodes.push_back({
                .nodeId = node.id,
                .entity = EntityID::INVALID,
                .startTime = startTimes[index],
                .endTime = startTimes[index] + node.duration,
                .scheduledStartTime = startTimes[index],
                .scheduledEndTime = startTimes[index] + node.duration,
            });
            continue;
        }
        // owner GUIDとNode IDを含め、同じ.vfxを複数配置しても名前参照やlintが衝突しないようにする。
        GameObject& generated = scene.CreateGameObject(
            "__VFX_" + owner.instanceId + "_" + std::to_string(node.id) + "_" + node.name);
        generated.SetParent(owner);
        if (!node.attachBone.empty()) {
            if (GameObject* socket = FindVFXSocket(owner, node.attachBone)) generated.SetParent(*socket);
            else FBZZ_LOG_WARN("VFX socket not found: %s", node.attachBone.c_str());
        }
        ApplyTransform(generated, node);
        switch (node.type) {
        case asset::VFXNodeType::Particle: AddParticle(generated, node); break;
        case asset::VFXNodeType::Trail: AddTrail(generated, node, false); break;
        case asset::VFXNodeType::MeshTrail: AddTrail(generated, node, true); break;
        case asset::VFXNodeType::Light: AddLight(generated, node); break;
        case asset::VFXNodeType::Audio: AddAudio(generated, node); break;
        case asset::VFXNodeType::Decal: AddDecal(generated, node); break;
        case asset::VFXNodeType::SubGraph: {
            AddSubGraph(generated, node, component.nestingDepth + 1);
            if (auto* child = generated.GetComponent<VFXGraphComponent>()) {
                for (const auto& forward : graph.subGraphForwards) {
                    if (forward.nodeId != node.id) continue;
                    const auto parameter = std::find_if(component.resolvedParameters.begin(),
                        component.resolvedParameters.end(), [&](const asset::VFXParamOverride& value) {
                            return value.paramName == forward.parentParam;
                        });
                    if (parameter != component.resolvedParameters.end())
                        child->parameterOverrides.push_back({ forward.childParam, parameter->value });
                }
            }
            break;
        }
        default: break;
        }
        generated.SetActive(false);
        const bool waitsForEvent = eventTargets.contains(node.id);
        const float runtimeStart = waitsForEvent
            ? (std::numeric_limits<float>::max)() : startTimes[index];
        component.runtimeNodes.push_back({
            .nodeId = node.id,
            .entity = generated.GetID(),
            .startTime = runtimeStart,
            .endTime = waitsForEvent ? runtimeStart : startTimes[index] + node.duration,
            .scheduledStartTime = startTimes[index],
            .scheduledEndTime = startTimes[index] + node.duration,
        });
    }
    component.loadedGraphPath = component.graphPath;
    component.initialized = true;
    component.reloadRequested = false;
    component.playTime = 0.0f;
    component.stopped = false;
    component.playing = component.playing || component.playOnAwake;
    component.runtimeGraph = std::make_shared<asset::VFXGraphAsset>(std::move(graph));
    return true;
}

} // namespace

ComponentAccess VFXGraphSystem::GetAccess() const
{
    // Scene構造を変更し複数Componentを生成するため、型単位の並列化対象から外す。
    return ComponentAccess{}.Unrestricted();
}

OrderingHints VFXGraphSystem::GetOrder() const
{
    // AudioSystemより先に音源を有効化する。Particleは後段LateUpdateなので同フレームに評価される。
    return OrderingHints{}.Before<AudioSystem>();
}

void VFXGraphSystem::Update(SystemContext& ctx)
{
    // GetEntities は Scene 内部ストレージを指す span を返すため、ループ内で GameObject を
    // 生成/破棄すると無効化される。イテレーション前に vector へコピーしてスナップショットを取る。
    const std::span<const EntityID> ownerView = ctx.scene.GetEntities<VFXGraphComponent>();
    const std::vector<EntityID> owners(ownerView.begin(), ownerView.end());
    for (const EntityID id : owners) {
        GameObject* owner = ctx.scene.GetGameObject(id);
        VFXGraphComponent* component = ctx.scene.GetComponent<VFXGraphComponent>(id);
        if (owner == nullptr || component == nullptr) continue;

        const bool editorPreview = component->editorPreviewFrame > 0
            && component->editorPreviewFrame + 2 >= Time::frameCount;
        if (!ctx.simulating && !editorPreview) {
            // Editorプレビューが途切れたら一時GameObjectを破棄し、次回PlayをplayOnAwakeから初期化する。
            if (component->initialized) DestroyRuntimeNodes(ctx.scene, *component);
            continue;
        }
        if (!component->enabled || component->graphPath.empty()) {
            DestroyRuntimeNodes(ctx.scene, *component);
            continue;
        }
        if (component->reloadRequested || component->loadedGraphPath != component->graphPath) {
            DestroyRuntimeNodes(ctx.scene, *component);
        }
        if (!component->initialized && !InitializeGraph(ctx.scene, *owner, *component)) continue;
        if (component->restartRequested) {
            ResetRuntimeSchedule(ctx.scene, *component);
            component->restartRequested = false;
            component->playTime = 0.0f;
        }
        if (component->editorScrubTime >= 0.0f) {
            const float targetTime = std::clamp(component->editorScrubTime, 0.0f,
                                                (std::max)(component->graphDuration, 0.0f));
            ResetRuntimeSchedule(ctx.scene, *component);
            component->playTime = targetTime;
            component->editorScrubTime = -1.0f;
            for (auto& node : component->runtimeNodes) {
                const bool shouldBeActive = targetTime >= node.startTime
                    && (node.endTime <= node.startTime || targetTime < node.endTime);
                if (shouldBeActive) {
                    SetNodeActive(ctx.scene, node, true);
                    if (GameObject* object = ctx.scene.GetGameObject(node.entity)) {
                        if (auto* emitter = object->GetComponent<ParticleEmitter>())
                            emitter->editorScrubTime = (std::max)(targetTime - node.startTime, 0.0f);
                        if (auto* graph = object->GetComponent<VFXGraphComponent>()) {
                            graph->editorScrubTime = (std::max)(targetTime - node.startTime, 0.0f);
                            graph->Pause();
                            if (editorPreview) graph->editorPreviewFrame = Time::frameCount;
                        }
                    }
                }
            }
        }
        // Curve/Gradient/Attribute/Signalは固定defaultと異なり時刻・ゲーム状態へ追従するため毎フレーム再評価する。
        ApplyDynamicVFXBindings(ctx.scene, *owner, *component);
        if (!component->playing) {
            for (auto& node : component->runtimeNodes) {
                if (component->stopped && node.active) SetNodeActive(ctx.scene, node, false);
                else if (node.active) ApplyPlaybackScale(ctx.scene, node, 0.0f, editorPreview);
            }
            continue;
        }

        component->playTime += ctx.dt * (std::max)(component->speed, 0.0f);
        if (component->loop && component->graphDuration > 0.0f
            && component->playTime >= component->graphDuration) {
            component->playTime = std::fmod(component->playTime, component->graphDuration);
            ResetRuntimeSchedule(ctx.scene, *component);
        }
        ConsumeParticleEvents(ctx.scene, *component);
        for (auto& node : component->runtimeNodes) {
            const bool shouldBeActive = component->playTime >= node.startTime
                && (node.endTime <= node.startTime || component->playTime < node.endTime);
            if (node.active != shouldBeActive) SetNodeActive(ctx.scene, node, shouldBeActive);
            if (node.active) ApplyPlaybackScale(ctx.scene, node, component->speed, editorPreview);
        }
        const bool awaitingEvent = std::any_of(component->runtimeLinks.begin(), component->runtimeLinks.end(),
            [](const VFXRuntimeLinkState& link) {
                return !link.fired
                    && (link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnCollision)
                        || link.trigger == static_cast<std::uint8_t>(asset::VFXLinkTrigger::OnDeath));
            });
        if (!component->loop && component->graphDuration > 0.0f
            && component->playTime >= component->graphDuration && !awaitingEvent) {
            component->playing = false;
        }
    }
}

} // namespace fbzz::scene
