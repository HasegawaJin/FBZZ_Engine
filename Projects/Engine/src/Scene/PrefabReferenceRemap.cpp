/// @file    PrefabReferenceRemap.cpp
/// @brief   プレファブ階層の差し替え後に、保存対象のシーン内参照を新しい実体へ移す。
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <Engine/Scene/PrefabInstantiate.hpp>

#include <Engine/Scene/ComponentRegistry.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace fbzz::scene {
namespace {

using EntityReplacements = std::vector<std::pair<EntityID, EntityID>>;

class PrefabReferenceReflector final : public IReflector {
public:
    explicit PrefabReferenceReflector(const EntityReplacements& replacements)
        : m_replacements(replacements) {}

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

    void ListField(const char*, std::vector<EntityRef>& values) override
    {
        for (EntityRef& value : values) Remap(value.id);
    }

    /// @note INVALID への対応は、アセットから削除された実体への参照を解除する。
    void Remap(EntityID& value) const
    {
        if (!value.IsValid()) return;
        for (const auto& [previous, replacement] : m_replacements) {
            if (value != previous) continue;
            value = replacement;
            return;
        }
    }

private:
    const EntityReplacements& m_replacements;
};

/// @note 専用 Serializer が扱う参照は Reflect に現れないため、対応する EntityID だけを移す。
template<typename T>
void RemapCustomComponentReferences(T& component, const PrefabReferenceReflector& reflector)
{
    if constexpr (std::is_same_v<T, IKSolverComponent>) {
        for (auto& chain : component.chains) {
            reflector.Remap(chain.targetEntity);
            reflector.Remap(chain.poleEntity);
        }
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

    PrefabReferenceReflector reflector(replacements);
    for (GameObject& gameObject : scene.GameObjects()) {
        ForEachRegisteredComponent([&]<typename T, typename Registration>() {
            if (auto* component = gameObject.GetComponent<T>())
                RemapComponentReferences<T, Registration>(*component, reflector);
        });

        if (auto* scripts = gameObject.GetComponent<ScriptComponent>()) {
            for (auto& entry : scripts->scripts) {
                if (!entry.script || entry.script->IsRuntimeFaulted()) continue;
                /// @note 観測 getter は既定の BeginObservation=false により評価しない。
                entry.script->ExecuteCallback([&] { entry.script->Reflect(reflector); },
                                               "PrefabReferenceRemap");
            }
        }
    }
}

void CopyPreparedPrefabComponents(Scene& source, Scene& destination,
                                  const EntityReplacements& replacements)
{
    PrefabReferenceReflector reflector(replacements);
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
    }
}

} /// @note namespace fbzz::scene
