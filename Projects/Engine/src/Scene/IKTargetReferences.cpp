/// @file    IKTargetReferences.cpp
/// @brief   IK の永続ターゲット参照と旧接続の一度だけの移行。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <Engine/Scene/IKTargetReferences.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>

namespace fbzz::scene {
namespace {

GameObject* FindUniqueName(Scene& scene, const std::string& name)
{
    if (name.empty()) return nullptr;
    GameObject* match = nullptr;
    for (GameObject& object : scene.GameObjects()) {
        if (object.name != name) continue;
        if (match) return nullptr;
        match = &object;
    }
    return match;
}

void InitializeReference(Scene& scene, ObjectReference& reference,
    EntityID legacyEntity, const std::string& legacyGuid,
    const std::string& legacyName, bool& migrationPending)
{
    if (!migrationPending) return;
    migrationPending = false;
    if (!reference.guid.empty()) return;
    if (!legacyGuid.empty()) {
        reference.guid = legacyGuid;
        return;
    }
    GameObject* target = legacyEntity.IsValid() ? scene.GetGameObject(legacyEntity) : nullptr;
    if (!target) target = FindUniqueName(scene, legacyName);
    if (target) reference.guid = target->instanceId;
    reference.id = EntityID::INVALID;
    reference.sceneGeneration = 0;
}

void ResolveReference(Scene& scene, ObjectReference& reference,
    EntityID& legacyEntity, std::string& legacyGuid)
{
    GameObject* target = nullptr;
    const uint64_t generation = scene.GetRenderSceneGeneration();
    if (!reference.guid.empty()) {
        if (reference.sceneGeneration == generation)
            target = scene.GetGameObject(reference.id);
        if (!target || target->instanceId != reference.guid)
            target = scene.FindByGuid(reference.guid);
    }
    reference.id = target ? target->GetID() : EntityID::INVALID;
    reference.sceneGeneration = generation;
    legacyEntity = reference.id;
    legacyGuid = reference.guid;
}

bool SetReference(Scene& scene, EntityID targetId, ObjectReference& reference,
    EntityID& legacyEntity, std::string& legacyGuid, bool& migrationPending)
{
    GameObject* target = scene.GetGameObject(targetId);
    if (!target || target->instanceId.empty()) return false;
    reference = {target->instanceId, target->GetID(), scene.GetRenderSceneGeneration()};
    legacyEntity = reference.id;
    legacyGuid = reference.guid;
    migrationPending = false;
    return true;
}

} /// @note namespace

void InitializeIKTargetReferences(Scene& scene, IKSolverComponent& solver)
{
    for (IKChain& chain : solver.chains) {
        InitializeReference(scene, chain.targetReference, chain.targetEntity,
            chain.targetGuid, chain.targetName, chain.targetReferenceMigrationPending);
        InitializeReference(scene, chain.poleReference, chain.poleEntity,
            chain.poleGuid, chain.poleName, chain.poleReferenceMigrationPending);
        ResolveReference(scene, chain.targetReference, chain.targetEntity, chain.targetGuid);
        ResolveReference(scene, chain.poleReference, chain.poleEntity, chain.poleGuid);
    }
}

void ResolveIKTargetReferences(Scene& scene, IKSolverComponent& solver)
{
    InitializeIKTargetReferences(scene, solver);
}

bool SetIKTargetReference(Scene& scene, IKChain& chain, EntityID target)
{
    if (!target.IsValid()) {
        ClearIKTargetReference(chain);
        return true;
    }
    return SetReference(scene, target, chain.targetReference, chain.targetEntity,
        chain.targetGuid, chain.targetReferenceMigrationPending);
}

bool SetIKPoleReference(Scene& scene, IKChain& chain, EntityID target)
{
    if (!target.IsValid()) {
        ClearIKPoleReference(chain);
        return true;
    }
    return SetReference(scene, target, chain.poleReference, chain.poleEntity,
        chain.poleGuid, chain.poleReferenceMigrationPending);
}

void ClearIKTargetReference(IKChain& chain)
{
    chain.targetReference = {};
    chain.targetEntity = EntityID::INVALID;
    chain.targetGuid.clear();
    chain.targetReferenceMigrationPending = false;
}

void ClearIKPoleReference(IKChain& chain)
{
    chain.poleReference = {};
    chain.poleEntity = EntityID::INVALID;
    chain.poleGuid.clear();
    chain.poleReferenceMigrationPending = false;
}

} /// @note namespace fbzz::scene
