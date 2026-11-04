/// @file    PrefabReferenceRemap.cpp
/// @brief   プレファブ階層の差し替え後に、保存対象のシーン内参照を新しい実体へ移す。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/LODGroupComponent.hpp>
#include <Engine/Scene/Components/SkinnedMeshRenderer.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>

#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <unordered_map>
#include <vector>

namespace fbzz::scene {
namespace {

using EntityReplacements = std::vector<std::pair<EntityID, EntityID>>;

class PrefabReferenceReflector final : public IReflector {
public:
    explicit PrefabReferenceReflector(const EntityReplacements& replacements,
                                      const Scene* sourceScene = nullptr,
                                      Scene* destinationScene = nullptr,
                                      bool clearExternalReferences = false)
        : m_sourceScene(sourceScene)
        , m_destinationScene(destinationScene)
        , m_clearExternalReferences(clearExternalReferences)
    {
        m_entityMap.reserve(replacements.size());
        for (const auto& [source, destination] : replacements)
            m_entityMap.emplace(Key(source), destination);
    }

    using IReflector::Field;
    using IReflector::ListField;

    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, bool&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Quaternion&) override {}

    void Field(const char*, EntityID& value) override { Remap(value); }

    void Field(const char*, EntityRef& value) override
    {
        Remap(value);
    }

    void ListField(const char*, std::vector<EntityRef>& values) override
    {
        for (EntityRef& value : values) Remap(value);
    }

    void ObjectReferenceField(const char*, ObjectReference& value) override
    {
        Remap(value);
    }

    void ObjectReferenceListField(const char*, std::vector<ObjectReference>& values) override
    {
        for (ObjectReference& value : values) Remap(value);
    }

    /// @note INVALID への対応は、アセットから削除された実体への参照を解除する。
    void Remap(EntityID& value) const
    {
        if (!value.IsValid()) return;
        const auto found = m_entityMap.find(Key(value));
        if (found != m_entityMap.end()) value = found->second;
        else if (m_clearExternalReferences) value = EntityID::INVALID;
    }

    void Remap(ObjectReference& value) const
    {
        if (value.guid.empty()) {
            value.id = EntityID::INVALID;
            value.sceneGeneration = m_destinationScene
                ? m_destinationScene->GetRenderSceneGeneration() : 0;
            return;
        }
        if (!m_sourceScene || !m_destinationScene) return;
        GameObject* sourceObject = nullptr;
        if (value.sceneGeneration == m_sourceScene->GetRenderSceneGeneration()
            && m_sourceScene->IsValid(value.id)) {
            GameObject* cached = m_sourceScene->GetGameObject(value.id);
            if (cached && cached->instanceId == value.guid) sourceObject = cached;
        }
        if (!sourceObject) sourceObject = m_sourceScene->FindByGuid(value.guid);

        const auto mapped = sourceObject ? m_entityMap.find(Key(sourceObject->GetID())) : m_entityMap.end();
        if (mapped != m_entityMap.end()) {
            const EntityID destinationId = mapped->second;
            GameObject* destinationObject = destinationId.IsValid()
                ? m_destinationScene->GetGameObject(destinationId) : nullptr;
            value.id = destinationObject ? destinationId : EntityID::INVALID;
            value.sceneGeneration = m_destinationScene->GetRenderSceneGeneration();
            if (destinationObject) value.guid = destinationObject->instanceId;
            return;
        }

        if (m_clearExternalReferences || m_sourceScene != m_destinationScene) {
            value.id = EntityID::INVALID;
            value.sceneGeneration = m_destinationScene->GetRenderSceneGeneration();
            return;
        }

        value.id = sourceObject ? sourceObject->GetID() : EntityID::INVALID;
        value.sceneGeneration = m_destinationScene->GetRenderSceneGeneration();
        if (sourceObject) value.guid = sourceObject->instanceId;
    }

    void Remap(EntityRef& value) const
    {
        if (value.guid.empty() && m_sourceScene && value.id.IsValid()) {
            const GameObject* original = m_sourceScene->GetGameObject(value.id);
            if (original) value.guid = original->instanceId;
        }
        if (value.guid.empty()) { Remap(value.id); value.sceneGeneration = 0; return; }
        ObjectReference reference{value.guid, value.id, value.sceneGeneration};
        Remap(reference);
        value.guid = std::move(reference.guid);
        value.id = reference.id;
        value.sceneGeneration = reference.sceneGeneration;
    }

    void RemapIKChain(IKChain& chain) const
    {
        const auto migrate = [&](ObjectReference& reference, EntityID legacy,
                                 const std::string& legacyGuid, const std::string& name, bool pending) {
            if (!pending || !reference.guid.empty()) return;
            reference.guid = legacyGuid;
            const GameObject* original = nullptr;
            if (reference.guid.empty() && m_sourceScene && legacy.IsValid()) original = m_sourceScene->GetGameObject(legacy);
            if (reference.guid.empty() && !original && m_sourceScene && !name.empty()) {
                auto candidates = m_sourceScene->GetRootGameObjects();
                while (!candidates.empty()) {
                    const GameObject* object = candidates.back(); candidates.pop_back();
                    for (int i = 0; i < object->GetChildCount(); ++i) candidates.push_back(object->GetChild(i));
                    if (object->name != name) continue;
                    if (original) { original = nullptr; break; }
                    original = object;
                }
            }
            if (original) reference.guid = original->instanceId;
            reference.id = legacy;
            reference.sceneGeneration = m_sourceScene ? m_sourceScene->GetRenderSceneGeneration() : 0;
        };
        migrate(chain.targetReference, chain.targetEntity, chain.targetGuid, chain.targetName, chain.targetReferenceMigrationPending);
        migrate(chain.poleReference, chain.poleEntity, chain.poleGuid, chain.poleName, chain.poleReferenceMigrationPending);
        Remap(chain.targetReference); Remap(chain.poleReference);
        chain.targetEntity = chain.targetReference.id; chain.targetGuid = chain.targetReference.guid;
        chain.poleEntity = chain.poleReference.id; chain.poleGuid = chain.poleReference.guid;
        chain.targetReferenceMigrationPending = false; chain.poleReferenceMigrationPending = false;
    }

private:
    static uint64_t Key(EntityID value)
    {
        return (static_cast<uint64_t>(value.generation) << 32) | value.index;
    }

    const Scene* m_sourceScene = nullptr;
    Scene* m_destinationScene = nullptr;
    bool m_clearExternalReferences = false;
    std::unordered_map<uint64_t, EntityID> m_entityMap;
};

void RemapScriptReferences(ScriptComponent& component, PrefabReferenceReflector& reflector)
{
    for (auto& entry : component.scripts) {
        if (!entry.script || entry.script->IsRuntimeFaulted()) continue;
        entry.script->ExecuteCallback([&] { entry.script->Reflect(reflector); }, "ObjectReferenceRemap");
    }
}

/// @note 専用 Serializer が扱う参照は Reflect に現れないため、対応する EntityID だけを移す。
template<typename T>
void RemapCustomComponentReferences(T& component, const PrefabReferenceReflector& reflector)
{
    if constexpr (std::is_same_v<T, IKSolverComponent>) {
        for (auto& chain : component.chains) reflector.RemapIKChain(chain);
    }
    if constexpr (std::is_same_v<T, BoneComponent>)
        reflector.Remap(component.skinnedMeshEntity);
    if constexpr (std::is_same_v<T, SkinnedMeshRenderer>) {
        reflector.Remap(component.skeletonRootEntity);
        for (EntityID& node : component.nodeEntities) reflector.Remap(node);
    }
    if constexpr (std::is_same_v<T, TerrainGridComponent>)
        for (EntityID& cell : component.cells) reflector.Remap(cell);
    if constexpr (std::is_same_v<T, LODGroupComponent>)
        for (auto& level : component.levels)
            for (auto& renderer : level.renderers) reflector.Remap(renderer.entity);
}

template<typename T, typename Registration>
void RemapComponentReferences(T& component, PrefabReferenceReflector& reflector)
{
    if constexpr (Registration::hasReflect) component.Reflect(reflector);
    RemapCustomComponentReferences(component, reflector);
}

} /// @note namespace

void RemapScenePrefabEntityReferences(Scene& scene, const EntityReplacements& replacements)
{
    if (replacements.empty()) return;

    PrefabReferenceReflector reflector(replacements, &scene, &scene);
    for (GameObject& gameObject : scene.GameObjects()) {
        ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if (auto* component = gameObject.GetComponent<T>())
                RemapComponentReferences<T, Registration>(*component, reflector);
        });

        if (auto* scripts = gameObject.GetComponent<ScriptComponent>()) {
            RemapScriptReferences(*scripts, reflector);
        }
    }
}

void CopyPreparedPrefabComponents(Scene& source, Scene& destination,
                                  const EntityReplacements& replacements)
{
    PrefabReferenceReflector reflector(replacements, &source, &destination, true);
    for (const auto& [sourceId, destinationId] : replacements) {
        GameObject* sourceObject = source.GetGameObject(sourceId);
        GameObject* destinationObject = destination.GetGameObject(destinationId);
        if (!sourceObject || !destinationObject) continue;

        std::vector<bool> missingComponents;
        ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            missingComponents.push_back(sourceObject->GetComponent<T>()
                                        && !destinationObject->GetComponent<T>());
        });
        destination.CopyComponentsFrom(source, sourceId, destinationId);

        /// @note 準備 Scene の ID は既存 Scene と重なるため、新規コピー分だけへ適用する。
        std::size_t componentIndex = 0;
        ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            const bool wasMissing = missingComponents[componentIndex++];
            if (wasMissing)
                if (auto* component = destinationObject->GetComponent<T>())
                    RemapComponentReferences<T, Registration>(*component, reflector);
        });
        if (auto* scripts = destinationObject->GetComponent<ScriptComponent>())
            RemapScriptReferences(*scripts, reflector);
    }
}

void RemapCopiedObjectReferences(Scene& source, Scene& destination,
                                 const EntityReplacements& replacements,
                                 std::span<const EntityID> copiedOwners)
{
    PrefabReferenceReflector reflector(replacements, &source, &destination, &source != &destination);
    for (EntityID ownerId : copiedOwners) {
        GameObject* owner = destination.GetGameObject(ownerId);
        if (!owner) continue;
        ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if (auto* component = owner->GetComponent<T>())
                RemapComponentReferences<T, Registration>(*component, reflector);
        });
        if (auto* scripts = owner->GetComponent<ScriptComponent>())
            RemapScriptReferences(*scripts, reflector);
    }
}

} /// @note namespace fbzz::scene
