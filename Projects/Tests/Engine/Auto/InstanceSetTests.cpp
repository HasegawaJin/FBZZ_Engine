/// @file    InstanceSetTests.cpp
/// @brief   Authored instance persistence, stable IDs, validation and copy-on-write scene isolation.
/// @author  Hasegawa Jin
/// @date    2026-10-10
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/InstanceSetAsset.hpp>
#include <Engine/Asset/DataAssetRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <array>
#include <filesystem>
#include <limits>

namespace fbzz::tests {
class InstanceSetTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp(); ASSERT_TRUE(m_temp.IsValid());
        asset::DataAssetRegistry::ClearCache();
        ASSERT_TRUE(asset::DataAssetRegistry::Create(File(),asset::InstanceSetAsset::TYPE_NAME));
        ASSERT_NE(Asset(),nullptr);
        ASSERT_TRUE(Asset()->Replace(m_parts,m_heights,m_placements));
    }
    void TearDown() override { asset::DataAssetRegistry::ClearCache(); EngineFixture::TearDown(); }
    std::string File() const { return m_temp.File("Instances.fzdata").generic_string(); }
    asset::InstanceSetAsset* Asset() const
    {
        auto* value=asset::DataAssetRegistry::Resolve(File());
        return value && std::string_view(value->GetTypeName())==asset::InstanceSetAsset::TYPE_NAME
            ? static_cast<asset::InstanceSetAsset*>(value) : nullptr;
    }
    scene::InstancedMeshComponent Component() const
    {
        scene::InstancedMeshComponent component; component.m_instanceSetRef.path=File(); return component;
    }
    testkit::TempDir m_temp{"InstanceSet"};
    std::array<asset::InstanceMeshPart,3> m_parts{{{"guid:trunk","guid:bark",0},{"guid:leaves","guid:leaf",0},{"guid:far","guid:leaf",1}}};
    std::array<float,2> m_heights{0.2f,0};
    std::array<renderer::InstancePlacement,2> m_placements{{{7,{1,2,3},{0,0.1f,0},{1,2,1},true},{21,{-4,0,5},{},{-1,1,1},true}}};
};

TEST_F(InstanceSetTest, SharedAssetRoundTripsPartsAndPlacementsWithoutRepublishingOnSave)
{
    auto* asset=Asset(); const auto before=asset->Snapshot(); ASSERT_NE(before,nullptr);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File())); EXPECT_EQ(asset->Snapshot(),before);
    auto replacement=m_placements; replacement[0].position={9,8,7};
    ASSERT_TRUE(asset->Replace(m_parts,m_heights,replacement));
    EXPECT_VEC3_NEAR(before->placements[0].position,math::Vector3(1,2,3),0.00001f);
    ASSERT_EQ(asset::DataAssetRegistry::ReloadFile(File()),1);
    EXPECT_EQ(Asset(),asset); ASSERT_NE(asset->Snapshot(),nullptr);
    EXPECT_EQ(asset->Parts().size(),3u); EXPECT_EQ(asset->Parts()[2].lod,1u);
    EXPECT_EQ(asset->Snapshot()->placements[1].id,21u);
    EXPECT_VEC3_NEAR(asset->Snapshot()->placements[0].position,before->placements[0].position,0.00001f);
}

TEST_F(InstanceSetTest, InvalidReplacementLeavesPublishedAssetUnchanged)
{
    const auto snapshot=Asset()->Snapshot(); auto placements=m_placements;
    placements[1].id=placements[0].id; EXPECT_FALSE(Asset()->Replace(m_parts,m_heights,placements));
    placements=m_placements; placements[0].scale.y=0; EXPECT_FALSE(Asset()->Replace(m_parts,m_heights,placements));
    placements=m_placements; placements[0].position.x=std::numeric_limits<float>::infinity(); EXPECT_FALSE(Asset()->Replace(m_parts,m_heights,placements));
    auto parts=m_parts; parts[2].lod=3; EXPECT_FALSE(Asset()->Replace(parts,m_heights,m_placements));
    const std::array<float,2> unsorted{0.1f,0.2f}; EXPECT_FALSE(Asset()->Replace(m_parts,unsorted,m_placements));
    EXPECT_EQ(Asset()->Snapshot(),snapshot);
}

TEST_F(InstanceSetTest, PlayCopiesAndRetainedRenderSnapshotsRemainImmutable)
{
    auto edit=Component(); auto play=edit; const auto authored=edit.Snapshot();
    ASSERT_TRUE(play.SetTransform(7,{6,5,4},{},{1,1,1}));
    const auto first=play.Snapshot(); EXPECT_EQ(edit.Snapshot(),authored);
    ASSERT_NE(first,authored); EXPECT_EQ(first->parentRevision,authored->revision);
    auto secondPlay=play;
    ASSERT_TRUE(play.SetTransform(21,{0,1,2},{},{1,1,1}));
    EXPECT_EQ(secondPlay.Snapshot(),first);
    EXPECT_VEC3_NEAR(first->placements[1].position,authored->placements[1].position,0.00001f);
    EXPECT_EQ(play.Snapshot()->parentRevision,first->revision);
    EXPECT_EQ(play.Snapshot()->dirtySlots,std::vector<uint32_t>({1}));
    play.ResetOverrides(); EXPECT_EQ(play.Snapshot(),authored);
}

TEST_F(InstanceSetTest, ResolvedSnapshotsObserveAuthoredChangesWithoutReplacingRuntimeOverrides)
{
    auto edit=Component(); auto play=edit;
    auto* definition=Asset(); ASSERT_NE(definition,nullptr);
    const auto authored=edit.Snapshot(*definition); ASSERT_NE(authored,nullptr); ASSERT_EQ(authored->placements.size(),2u);
    ASSERT_TRUE(play.SetTransform(7,{6,5,4},{},{1,1,1}));
    const auto overridden=play.Snapshot(*definition); ASSERT_NE(overridden,nullptr); ASSERT_EQ(overridden->placements.size(),2u);
    auto replacement=m_placements; replacement[0].position={9,8,7};

    ASSERT_TRUE(definition->Replace(m_parts,m_heights,replacement));
    const auto currentEdit=edit.Snapshot(*definition);
    const auto currentPlay=play.Snapshot(*definition);

    ASSERT_NE(currentEdit,nullptr); ASSERT_NE(currentPlay,nullptr);
    ASSERT_EQ(currentEdit->placements.size(),2u); ASSERT_EQ(currentPlay->placements.size(),2u);
    EXPECT_NE(currentEdit,authored); EXPECT_EQ(currentPlay,overridden);
    EXPECT_VEC3_NEAR(currentEdit->placements[0].position,math::Vector3(9,8,7),0.00001f);
    EXPECT_VEC3_NEAR(currentPlay->placements[0].position,math::Vector3(6,5,4),0.00001f);
    EXPECT_VEC3_NEAR(authored->placements[0].position,math::Vector3(1,2,3),0.00001f);
    EXPECT_VEC3_NEAR(overridden->placements[1].position,math::Vector3(-4,0,5),0.00001f);

    play.ResetOverrides();
    const auto reset=play.Snapshot(*definition);

    EXPECT_EQ(reset,currentEdit);
    EXPECT_VEC3_NEAR(overridden->placements[0].position,math::Vector3(6,5,4),0.00001f);
}

TEST_F(InstanceSetTest, ResolvedSnapshotsUseNewReferencesAndKeepPreviousSnapshotsAlive)
{
    auto component=Component();
    auto* originalDefinition=Asset(); ASSERT_NE(originalDefinition,nullptr);
    const auto authored=component.Snapshot(*originalDefinition); ASSERT_NE(authored,nullptr); ASSERT_EQ(authored->placements.size(),2u);
    ASSERT_TRUE(component.SetTransform(7,{6,5,4},{},{1,1,1}));
    const auto overridden=component.Snapshot(*originalDefinition); ASSERT_NE(overridden,nullptr); ASSERT_EQ(overridden->placements.size(),2u);
    const auto otherFile=m_temp.File("OtherInstances.fzdata").generic_string();
    ASSERT_TRUE(asset::DataAssetRegistry::Create(otherFile,asset::InstanceSetAsset::TYPE_NAME));
    auto* otherBase=asset::DataAssetRegistry::Resolve(otherFile); ASSERT_NE(otherBase,nullptr);
    ASSERT_EQ(std::string_view(otherBase->GetTypeName()),asset::InstanceSetAsset::TYPE_NAME);
    auto* otherDefinition=static_cast<asset::InstanceSetAsset*>(otherBase);
    auto otherPlacements=m_placements;
    otherPlacements[0].position={30,20,10}; otherPlacements[1].id=29;
    ASSERT_TRUE(otherDefinition->Replace(m_parts,m_heights,otherPlacements));

    component.m_instanceSetRef.path=otherFile;
    const auto current=component.Snapshot(*otherDefinition);

    ASSERT_NE(current,nullptr); ASSERT_EQ(current->placements.size(),2u); EXPECT_NE(current,overridden);
    EXPECT_EQ(component.Snapshot(),current);
    EXPECT_VEC3_NEAR(current->placements[0].position,math::Vector3(30,20,10),0.00001f);
    EXPECT_EQ(current->placements[1].id,29u);
    EXPECT_VEC3_NEAR(authored->placements[0].position,math::Vector3(1,2,3),0.00001f);
    EXPECT_VEC3_NEAR(overridden->placements[0].position,math::Vector3(6,5,4),0.00001f);

    ASSERT_TRUE(component.SetTransform(29,{8,7,6},{},{1,1,1}));
    const auto changed=component.Snapshot(*otherDefinition);

    ASSERT_NE(changed,nullptr); ASSERT_EQ(changed->placements.size(),2u); EXPECT_NE(changed,current);
    EXPECT_VEC3_NEAR(changed->placements[1].position,math::Vector3(8,7,6),0.00001f);
    EXPECT_VEC3_NEAR(current->placements[1].position,math::Vector3(-4,0,5),0.00001f);
    EXPECT_VEC3_NEAR(overridden->placements[1].position,math::Vector3(-4,0,5),0.00001f);
}

TEST_F(InstanceSetTest, MultipleEditsPreserveDirtySlotsAndRemovedIdsAreNeverReused)
{
    auto component=Component(); const auto authored=component.Snapshot();
    ASSERT_TRUE(component.SetTransform(7,{2,0,0},{},{1,1,1}));
    ASSERT_TRUE(component.Remove(21)); EXPECT_FALSE(component.Remove(21));
    const uint32_t id=component.Add({3,0,0},{},{1,1,1}); EXPECT_EQ(id,22u);
    EXPECT_FALSE(component.SetTransform(21,{},{},{1,1,1}));
    const auto changed=component.Snapshot(); ASSERT_NE(changed,nullptr);
    EXPECT_EQ(changed->parentRevision,authored->revision);
    EXPECT_EQ(changed->dirtySlots,std::vector<uint32_t>({0,1,2}));
    ASSERT_EQ(changed->placements.size(),3u); EXPECT_FALSE(changed->placements[1].active);
    EXPECT_EQ(authored->placements.size(),2u); EXPECT_TRUE(authored->placements[1].active);
    ASSERT_TRUE(component.Remove(id)); EXPECT_EQ(component.Add({},{},{1,1,1}),23u);
}

TEST_F(InstanceSetTest, InvalidRuntimeTransformsAndMissingAssetsHaveNoObservableChanges)
{
    auto component=Component(); const auto before=component.Snapshot();
    EXPECT_FALSE(component.SetTransform(7,{},{},{0,1,1})); EXPECT_EQ(component.Add({},{},{1,0,1}),0u);
    EXPECT_EQ(component.Snapshot(),before);
    component.m_instanceSetRef.path=""; EXPECT_EQ(component.Snapshot(),nullptr);
    EXPECT_FALSE(component.Remove(7)); EXPECT_EQ(component.Add({},{},{1,1,1}),0u);
}
TEST_F(InstanceSetTest, SceneSerializationPreservesAuthoringAndDoesNotPersistRuntimeOverrides)
{
    scene::Scene source; auto& owner=source.CreateGameObject("TreesAndDetails");
    auto& component=owner.AddComponent<scene::InstancedMeshComponent>(); component.m_instanceSetRef.path=File();
    component.m_castShadows=false; component.m_maxDistance=85;
    ASSERT_TRUE(component.SetTransform(7,{50,40,30},{},{1,1,1}));
    const auto path=m_temp.File("Instances.scene").generic_string(); ASSERT_TRUE(scene::SceneSerializer::Save(source,path));
    const auto loaded=scene::SceneSerializer::LoadData(path); ASSERT_NE(loaded,nullptr);
    const auto entities=loaded->GetEntities<scene::InstancedMeshComponent>(); ASSERT_EQ(entities.size(),1u);
    const auto* restored=loaded->GetComponent<scene::InstancedMeshComponent>(entities[0]);
    ASSERT_NE(restored,nullptr); EXPECT_FALSE(restored->m_castShadows); EXPECT_FLOAT_EQ(restored->m_maxDistance,85);
    ASSERT_NE(restored->Snapshot(),nullptr); EXPECT_VEC3_NEAR(restored->Snapshot()->placements[0].position,m_placements[0].position,0.00001f);
}
/// @note Canonical and lexical aliases must share the same authored asset through reload and undo.
TEST_F(InstanceSetTest, CanonicalPathAliasesShareReloadsAndRestoredSnapshots)
{
    auto* definition = Asset(); ASSERT_NE(definition, nullptr);
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    const auto original = asset::DataAssetRegistry::Snapshot(File()); ASSERT_FALSE(original.empty());
    const auto parent = m_temp.Path().generic_string();
    auto backslashes = File(); for (char& c : backslashes) if (c == '/') c = '\\';
    const std::array<std::string, 4> aliases{File(), parent + "/./Instances.fzdata",
        parent + "/unused/../Instances.fzdata", backslashes};
    for (const auto& alias : aliases) {
        EXPECT_EQ(asset::DataAssetRegistry::Resolve(alias), definition);
        EXPECT_EQ(asset::DataAssetRegistry::TypeOf(alias), asset::InstanceSetAsset::TYPE_NAME);
        EXPECT_EQ(asset::DataAssetRegistry::Snapshot(alias), original);
    }
    auto replacement = m_placements; replacement[0].position = {9, 8, 7};
    ASSERT_TRUE(definition->Replace(m_parts, m_heights, replacement));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(aliases[1]));
    replacement[0].position = {6, 5, 4};
    ASSERT_TRUE(definition->Replace(m_parts, m_heights, replacement));
    ASSERT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    EXPECT_VEC3_NEAR(definition->Snapshot()->placements[0].position, math::Vector3(9, 8, 7), 0.00001f);
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(aliases[2], original));
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(aliases[3]), definition);
    EXPECT_VEC3_NEAR(definition->Snapshot()->placements[0].position, m_placements[0].position, 0.00001f);
    EXPECT_TRUE(asset::DataAssetRegistry::Save(aliases[3]));
}

/// @note UTF-8 and spaces retain the general conversion path and distinct persistent identities.
TEST_F(InstanceSetTest, FallbackPathsPreserveUtf8AndDistinctAssetIdentity)
{
    const auto name = util::FileSystem::PathFromUtf8("Special Dir/Utf8-\xE6\x97\xA5\xE6\x9C\xAC.fzdata");
    const auto full = m_temp.Path() / name;
    const auto firstPath = util::FileSystem::PathToUtf8(full);
    const auto secondPath = util::FileSystem::PathToUtf8(full.parent_path() / "Other.fzdata");
    const auto alias = util::FileSystem::PathToUtf8(full.parent_path() / "." / full.filename());
    ASSERT_TRUE(asset::DataAssetRegistry::Create(firstPath, asset::InstanceSetAsset::TYPE_NAME));
    ASSERT_TRUE(asset::DataAssetRegistry::Create(secondPath, asset::InstanceSetAsset::TYPE_NAME));
    auto* first = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(firstPath));
    auto* second = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(secondPath));
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr); EXPECT_NE(first, second);
    auto replacement = m_placements; replacement[0].position = {12, 13, 14};
    ASSERT_TRUE(first->Replace(m_parts, m_heights, replacement));
    ASSERT_TRUE(second->Replace(m_parts, m_heights, m_placements));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(firstPath));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(secondPath));
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(alias), first);
    asset::DataAssetRegistry::ClearCache();
    first = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(alias));
    second = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(secondPath));
    ASSERT_NE(first, nullptr); ASSERT_NE(second, nullptr); EXPECT_NE(first, second);
    EXPECT_VEC3_NEAR(first->Snapshot()->placements[0].position, math::Vector3(12, 13, 14), 0.00001f);
    EXPECT_VEC3_NEAR(second->Snapshot()->placements[0].position, m_placements[0].position, 0.00001f);
}

/// @note A failed alias remains failed until creation publishes a valid asset or the cache is retired.
TEST_F(InstanceSetTest, MissingAliasesRecoverThroughCreateAndCacheRetirement)
{
    const auto missing = m_temp.File("Later.fzdata").generic_string();
    const auto alias = m_temp.Path().generic_string() + "/./Later.fzdata";
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(missing), nullptr);
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(alias), nullptr);
    EXPECT_TRUE(asset::DataAssetRegistry::TypeOf(alias).empty());
    ASSERT_TRUE(asset::DataAssetRegistry::Create(alias, asset::InstanceSetAsset::TYPE_NAME));
    auto* definition = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(missing));
    ASSERT_NE(definition, nullptr);
    ASSERT_TRUE(definition->Replace(m_parts, m_heights, m_placements));
    ASSERT_TRUE(asset::DataAssetRegistry::Save(alias));
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(alias), definition);
    asset::DataAssetRegistry::ClearCache();
    auto* reloaded = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(missing));
    ASSERT_NE(reloaded, nullptr); ASSERT_NE(reloaded->Snapshot(), nullptr);
    ASSERT_EQ(reloaded->Snapshot()->placements.size(), m_placements.size());
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(alias), reloaded);
    EXPECT_VEC3_NEAR(reloaded->Snapshot()->placements[0].position, m_placements[0].position, 0.00001f);
}


class InstanceSetGuidTest : public InstanceSetTest {
protected:
    void SetUp() override
    {
        InstanceSetTest::SetUp();
        asset::AssetDatabase::Init(m_temp.Path().generic_string() + "/");
        const auto guid = asset::AssetDatabase::GuidFromPath(File());
        ASSERT_FALSE(guid.empty()); m_reference = "guid:" + guid;
    }
    void TearDown() override { asset::AssetDatabase::Shutdown(); InstanceSetTest::TearDown(); }
    std::string m_reference;
};

TEST_F(InstanceSetGuidTest, WarmGuidSeesReloadUndoAndCacheRetirement)
{
    auto* first = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(m_reference));
    ASSERT_NE(first, nullptr); ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    const auto original = asset::DataAssetRegistry::Snapshot(File()); ASSERT_FALSE(original.empty());
    auto changed = m_placements; changed[0].position = {11, 12, 13};
    ASSERT_TRUE(first->Replace(m_parts, m_heights, changed)); ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    ASSERT_EQ(asset::DataAssetRegistry::ReloadFile(File()), 1);
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(m_reference), first);
    EXPECT_VEC3_NEAR(first->Snapshot()->placements[0].position, math::Vector3(11, 12, 13), 0.00001f);
    ASSERT_TRUE(asset::DataAssetRegistry::RestoreSnapshot(m_reference, original));
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(m_reference), first);
    EXPECT_VEC3_NEAR(first->Snapshot()->placements[0].position, m_placements[0].position, 0.00001f);
    asset::DataAssetRegistry::ClearCache();
    auto* current = static_cast<asset::InstanceSetAsset*>(asset::DataAssetRegistry::Resolve(m_reference));
    ASSERT_NE(current, nullptr); ASSERT_NE(current->Snapshot(), nullptr);
    EXPECT_VEC3_NEAR(current->Snapshot()->placements[0].position, math::Vector3(11, 12, 13), 0.00001f);
}

TEST_F(InstanceSetGuidTest, WarmGuidFollowsIndexMovesRemovalAndProjectRetirement)
{
    ASSERT_TRUE(asset::DataAssetRegistry::Save(File()));
    auto* old = asset::DataAssetRegistry::Resolve(m_reference); ASSERT_NE(old, nullptr);
    const auto moved = m_temp.File("Moved.fzdata").generic_string(); std::error_code error;
    std::filesystem::rename(File(), moved, error); ASSERT_FALSE(error);
    std::filesystem::rename(File() + ".meta", moved + ".meta", error); ASSERT_FALSE(error);
    asset::AssetDatabase::OnAssetMoved(File(), moved);
    auto* current = asset::DataAssetRegistry::Resolve(m_reference); ASSERT_NE(current, nullptr); EXPECT_NE(current, old);
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(File()), current);
    asset::AssetDatabase::OnAssetRemoved(moved);
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(m_reference), nullptr);
    asset::AssetDatabase::Shutdown();
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(m_reference), nullptr);
    asset::AssetDatabase::Init(m_temp.Path().generic_string() + "/");
    EXPECT_EQ(asset::DataAssetRegistry::Resolve(m_reference), current);
}

} /// @note namespace fbzz::tests
