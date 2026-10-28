/// @file    IKTargetReferenceTests.cpp
/// @brief   IK 永続参照の移行・欠損保持・Scene 寿命と複製契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-08
#include <TestKit/TestKit.hpp>
#include <Engine/Scene/Components/BoneComponent.hpp>
#include <Engine/Scene/Components/IKSolverComponent.hpp>
#include <Engine/Scene/Components/TargetPointComponent.hpp>
#include <Engine/Scene/IKTargetReferences.hpp>
#include <Engine/Scene/PrefabInstantiate.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::tests {
namespace {

class IKTargetReferenceTest : public testkit::Fixture {
protected:
    scene::Scene m_scene;
};

TEST_F(IKTargetReferenceTest, ExplicitSetAcceptsMarkerAndBoneAndPreservesChainLabelsAndWorldOffset)
{
    auto& target = m_scene.CreateGameObject("Marker");
    target.AddComponent<scene::TargetPointComponent>();
    auto& pole = m_scene.CreateGameObject("Bone");
    pole.AddComponent<scene::BoneComponent>();
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetName = "Hand";
    chain.poleName = "Elbow";
    chain.targetOffset = {1.0f, 2.0f, 3.0f};

    ASSERT_TRUE(scene::SetIKTargetReference(m_scene, chain, target.GetID()));
    ASSERT_TRUE(scene::SetIKPoleReference(m_scene, chain, pole.GetID()));
    scene::ResolveIKTargetReferences(m_scene, solver);
    EXPECT_EQ(chain.targetReference.guid, target.instanceId);
    EXPECT_EQ(chain.poleReference.guid, pole.instanceId);
    EXPECT_EQ(chain.targetEntity, target.GetID());
    EXPECT_EQ(chain.poleEntity, pole.GetID());
    EXPECT_EQ(chain.targetGuid, target.instanceId);
    EXPECT_EQ(chain.poleGuid, pole.instanceId);
    EXPECT_EQ(chain.targetName, "Hand");
    EXPECT_EQ(chain.poleName, "Elbow");
    EXPECT_VEC3_NEAR(chain.targetOffset, (math::Vector3{1.0f, 2.0f, 3.0f}), testkit::kTolerance);
}

TEST_F(IKTargetReferenceTest, InvalidExplicitSetLeavesExistingConnectionUntouched)
{
    auto& target = m_scene.CreateGameObject("Target");
    auto& removed = m_scene.CreateGameObject("Removed");
    const auto stale = removed.GetID();
    ASSERT_TRUE(m_scene.DestroyGameObject(stale));
    scene::IKChain chain;
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene, chain, target.GetID()));
    const auto before = chain.targetReference;
    EXPECT_FALSE(scene::SetIKTargetReference(m_scene, chain, stale));
    EXPECT_EQ(chain.targetReference.guid, before.guid);
    EXPECT_EQ(chain.targetReference.id, before.id);
    EXPECT_EQ(chain.targetReference.sceneGeneration, before.sceneGeneration);
    EXPECT_EQ(chain.targetEntity, target.GetID());
}

TEST_F(IKTargetReferenceTest, MissingLegacyGuidNeverFallsBackToMatchingNameOrEntity)
{
    auto& decoy = m_scene.CreateGameObject("Grip");
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetGuid = "missing-target-guid";
    chain.targetName = decoy.name;
    chain.targetEntity = decoy.GetID();
    chain.poleGuid = "missing-pole-guid";
    chain.poleName = decoy.name;
    chain.poleEntity = decoy.GetID();
    scene::InitializeIKTargetReferences(m_scene, solver);
    EXPECT_EQ(chain.targetReference.guid, "missing-target-guid");
    EXPECT_EQ(chain.poleReference.guid, "missing-pole-guid");
    EXPECT_FALSE(chain.targetEntity.IsValid());
    EXPECT_FALSE(chain.poleEntity.IsValid());
    EXPECT_FALSE(chain.targetReferenceMigrationPending);
    EXPECT_FALSE(chain.poleReferenceMigrationPending);
}

TEST_F(IKTargetReferenceTest, AmbiguousLegacyNameDoesNotConnectAfterOneCandidateIsRemoved)
{
    auto& first = m_scene.CreateGameObject("Grip");
    const auto firstId = first.GetID();
    m_scene.CreateGameObject("Grip");
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetName = "Grip";
    scene::InitializeIKTargetReferences(m_scene, solver);
    EXPECT_TRUE(chain.targetReference.guid.empty());
    EXPECT_FALSE(chain.targetEntity.IsValid());
    ASSERT_TRUE(m_scene.DestroyGameObject(firstId));
    scene::ResolveIKTargetReferences(m_scene, solver);
    EXPECT_TRUE(chain.targetReference.guid.empty());
    EXPECT_FALSE(chain.targetEntity.IsValid());
}

TEST_F(IKTargetReferenceTest, AbsentLegacyNameDoesNotConnectWhenAnObjectAppearsLater)
{
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetName = "Grip";
    scene::InitializeIKTargetReferences(m_scene, solver);
    m_scene.CreateGameObject("Grip");
    scene::ResolveIKTargetReferences(m_scene, solver);
    EXPECT_TRUE(chain.targetReference.guid.empty());
    EXPECT_FALSE(chain.targetEntity.IsValid());
}

TEST_F(IKTargetReferenceTest, UniqueLegacyNameMigratesOnceAndSurvivesTargetRename)
{
    auto& target = m_scene.CreateGameObject("Grip");
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetName = target.name;
    scene::InitializeIKTargetReferences(m_scene, solver);
    EXPECT_EQ(chain.targetReference.guid, target.instanceId);
    EXPECT_EQ(chain.targetEntity, target.GetID());
    target.name = "Renamed";
    m_scene.CreateGameObject("Grip");
    scene::ResolveIKTargetReferences(m_scene, solver);
    EXPECT_EQ(chain.targetEntity, target.GetID());
    EXPECT_EQ(chain.targetName, "Grip");
}

TEST_F(IKTargetReferenceTest, ExplicitClearCannotResurrectFromLegacyFields)
{
    auto& target = m_scene.CreateGameObject("Grip");
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetName = target.name;
    chain.poleName = target.name;
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene, chain, target.GetID()));
    ASSERT_TRUE(scene::SetIKPoleReference(m_scene, chain, target.GetID()));
    scene::ClearIKTargetReference(chain);
    scene::ClearIKPoleReference(chain);
    chain.targetEntity = target.GetID();
    chain.poleEntity = target.GetID();
    chain.targetGuid = target.instanceId;
    chain.poleGuid = target.instanceId;
    scene::ResolveIKTargetReferences(m_scene, solver);
    EXPECT_TRUE(chain.targetReference.guid.empty());
    EXPECT_TRUE(chain.poleReference.guid.empty());
    EXPECT_TRUE(chain.targetGuid.empty());
    EXPECT_TRUE(chain.poleGuid.empty());
    EXPECT_FALSE(chain.targetEntity.IsValid());
    EXPECT_FALSE(chain.poleEntity.IsValid());
    EXPECT_EQ(chain.targetName, "Grip");
}

TEST_F(IKTargetReferenceTest, DeletedTargetRetainsGuidAndDoesNotAttachToNewSameNamedObject)
{
    auto& target = m_scene.CreateGameObject("Grip");
    const auto originalId = target.GetID();
    const auto originalGuid = target.instanceId;
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetName = target.name;
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene, chain, originalId));
    ASSERT_TRUE(m_scene.DestroyGameObject(originalId));
    m_scene.CreateGameObject("Grip");
    scene::ResolveIKTargetReferences(m_scene, solver);
    EXPECT_EQ(chain.targetReference.guid, originalGuid);
    EXPECT_EQ(chain.targetGuid, originalGuid);
    EXPECT_FALSE(chain.targetEntity.IsValid());
}

TEST_F(IKTargetReferenceTest, CachedEntityFromAnotherSceneCannotOverrideTheCanonicalGuid)
{
    scene::Scene previous;
    auto& oldTarget = previous.CreateGameObject("Grip");
    auto& decoy = m_scene.CreateGameObject("Grip");
    auto& target = m_scene.CreateGameObject("Current Grip");
    target.instanceId = oldTarget.instanceId;
    ASSERT_EQ(oldTarget.GetID().index, decoy.GetID().index);
    scene::IKSolverComponent solver;
    auto& chain = solver.chains.emplace_back();
    chain.targetReference = {oldTarget.instanceId, oldTarget.GetID(), previous.GetRenderSceneGeneration()};
    chain.targetEntity = decoy.GetID();
    scene::ResolveIKTargetReferences(m_scene, solver);
    EXPECT_EQ(chain.targetEntity, target.GetID());
    EXPECT_NE(chain.targetEntity, decoy.GetID());
    EXPECT_EQ(chain.targetReference.sceneGeneration, m_scene.GetRenderSceneGeneration());
}

TEST_F(IKTargetReferenceTest, CanonicalNullAndMissingGuidSurviveSceneSaveLoadDespiteLegacyNameMatches)
{
    auto& owner = m_scene.CreateGameObject("Owner");
    auto& decoy = m_scene.CreateGameObject("Grip");
    auto& solver = owner.AddComponent<scene::IKSolverComponent>();
    auto& cleared = solver.chains.emplace_back();
    cleared.targetName = decoy.name;
    scene::ClearIKTargetReference(cleared);
    auto& missing = solver.chains.emplace_back();
    missing.targetReference.guid = "missing-target-guid";
    missing.targetReferenceMigrationPending = false;
    missing.targetName = decoy.name;
    missing.poleReference.guid = "missing-pole-guid";
    missing.poleReferenceMigrationPending = false;
    missing.poleName = decoy.name;
    scene::ResolveIKTargetReferences(m_scene, solver);
    const std::string ownerGuid = owner.instanceId;
    const auto text = scene::SceneSerializer::SaveToText(m_scene);
    auto loaded = scene::SceneSerializer::LoadDataFromText(text, "");
    ASSERT_NE(loaded, nullptr);
    auto* restoredOwner = loaded->FindByGuid(ownerGuid);
    ASSERT_NE(restoredOwner, nullptr);
    auto* restored = restoredOwner->GetComponent<scene::IKSolverComponent>();
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->chains.size(), 2u);
    scene::ResolveIKTargetReferences(*loaded, *restored);
    EXPECT_TRUE(restored->chains[0].targetReference.guid.empty());
    EXPECT_FALSE(restored->chains[0].targetEntity.IsValid());
    EXPECT_EQ(restored->chains[1].targetReference.guid, "missing-target-guid");
    EXPECT_EQ(restored->chains[1].poleReference.guid, "missing-pole-guid");
    EXPECT_FALSE(restored->chains[1].targetEntity.IsValid());
    EXPECT_FALSE(restored->chains[1].poleEntity.IsValid());
}

TEST_F(IKTargetReferenceTest, CloneForestRemapsInternalGuidAndPreservesMissingExternalGuid)
{
    auto& sourceOwner = m_scene.CreateGameObject("Owner");
    auto& sourceTarget = m_scene.CreateGameObject("Target");
    auto& solver = sourceOwner.AddComponent<scene::IKSolverComponent>();
    auto& chain = solver.chains.emplace_back();
    chain.targetName = "Hand";
    ASSERT_TRUE(scene::SetIKTargetReference(m_scene, chain, sourceTarget.GetID()));
    chain.poleReference.guid = "missing-external-pole-guid";
    chain.poleReferenceMigrationPending = false;
    scene::ResolveIKTargetReferences(m_scene, solver);

    scene::Scene destination;
    auto& clonedOwner = destination.CreateGameObject("OwnerClone");
    auto& clonedTarget = destination.CreateGameObject("TargetClone");
    destination.CopyComponentsFromPassive(m_scene, sourceOwner.GetID(), clonedOwner.GetID());
    const std::vector<std::pair<scene::EntityID, scene::EntityID>> replacements{
        {sourceOwner.GetID(), clonedOwner.GetID()}, {sourceTarget.GetID(), clonedTarget.GetID()}};
    const std::array<scene::EntityID, 1> copiedOwners{clonedOwner.GetID()};
    scene::RemapCopiedObjectReferences(m_scene, destination, replacements, copiedOwners);
    auto* copied = clonedOwner.GetComponent<scene::IKSolverComponent>();
    ASSERT_NE(copied, nullptr);
    ASSERT_EQ(copied->chains.size(), 1u);
    scene::ResolveIKTargetReferences(destination, *copied);
    EXPECT_EQ(copied->chains[0].targetReference.guid, clonedTarget.instanceId);
    EXPECT_EQ(copied->chains[0].targetEntity, clonedTarget.GetID());
    EXPECT_EQ(copied->chains[0].targetGuid, clonedTarget.instanceId);
    EXPECT_EQ(copied->chains[0].targetName, "Hand");
    EXPECT_EQ(copied->chains[0].poleReference.guid, "missing-external-pole-guid");
    EXPECT_FALSE(copied->chains[0].poleEntity.IsValid());
}

} /// @note namespace
} /// @note namespace fbzz::tests
