/// @file    WorldPcgAuthoringTests.cpp
/// @brief   PCG preview purity, regeneration ownership, partial edits and checked history.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Tools/WorldAuthoring.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <map>

namespace fbzz::tests {

class WorldPcgAuthoringTest:public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();editor::AssetDirtyRegistry::DiscardAll();Context().projectRoot=util::FileSystem::PathToUtf8(ProjectRoot());
        asset::AssetManager::Init(m_resources,util::FileSystem::PathToUtf8(ProjectRoot()/"Assets"));
        auto& scene=AttachScene();Context().editScene=&scene;Context().undoStack=&m_undo;
        auto& ground=scene.CreateGameObject("Ground");auto& terrain=ground.AddComponent<scene::TerrainComponent>();terrain.Resize(17,17);terrain.cellSize=2;terrain.InitFlat();m_terrain=ground.instanceId;
        m_profile.grid.cellSize=16;m_profile.bounds={{0,0,0},{32,0,32}};
        scene::WorldPcgRule rule;rule.key="trees";rule.parts={{"primitive:cone","guid:test-material",0}};rule.lodHeights={0};rule.spacing=4;m_profile.rules.push_back(rule);
        m_path=util::FileSystem::PathToUtf8(ProjectRoot()/"Assets"/"Profiles"/"Forest.pcg");std::string diagnostic;
        ASSERT_TRUE(scene::SaveWorldPcgProfile(m_path,m_profile,diagnostic))<<diagnostic;ASSERT_FALSE(asset::AssetDatabase::GuidFromPath(m_path).empty());
        editor::OpArgs configure;configure.Set("profile",m_path);auto configured=Call(editor::ConfigureWorldPcgVolume,configure);ASSERT_TRUE(configured.ok)<<configured.message;
        ASSERT_NE(configured.data.Find("volume"),nullptr);m_volume=configured.data.Find("volume")->AsString();
    }
    void TearDown() override
    {m_undo.Clear();editor::AssetDirtyRegistry::DiscardAll();asset::AssetManager::UnloadAll();EditorFixture::TearDown();}
    using Service=editor::OpResult(*)(editor::OpContext&,const editor::OpArgs&);
    editor::OpResult Call(Service service,const editor::OpArgs& args={})
    {
        editor::OpContext context{Context(),m_undo};auto result=service(context,args);
        if(result.ok&&result.command)EXPECT_TRUE(m_undo.Push(std::move(result.command)));
        return result;
    }
    editor::OpArgs Arguments() const {editor::OpArgs args;args.Set("volume",m_volume);return args;}
    editor::OpResult Generate(editor::OpArgs args)
    {
        auto preview=Call(editor::PreviewWorldPcg,args);if(!preview.ok)return preview;
        args.Set("expected",preview.data.Find("expected")->AsString());return Call(editor::GenerateWorldPcg,args);
    }
    std::vector<scene::GameObject*> Owners()
    {
        std::vector<scene::GameObject*> result;for(auto id:AttachedScene()->GetEntities<scene::WorldPcgGeneratedComponent>())result.push_back(AttachedScene()->GetGameObject(id));return result;
    }
    std::map<std::pair<std::string,uint32_t>,math::Vector3> Placements()
    {
        std::map<std::pair<std::string,uint32_t>,math::Vector3> result;
        for(auto* owner:Owners()){const auto snapshot=owner->GetComponent<scene::InstancedMeshComponent>()->Snapshot();if(snapshot)for(const auto& p:snapshot->placements)if(p.active)result[{owner->instanceId,p.id}]=p.position;}
        return result;
    }
    void SaveProfile() {std::string diagnostic;ASSERT_TRUE(scene::SaveWorldPcgProfile(m_path,m_profile,diagnostic))<<diagnostic;}
    editor::UndoStack m_undo;scene::WorldPcgProfile m_profile;std::string m_path,m_volume,m_terrain;
    testkit::ResourceTrackingRenderer m_device;renderer::ResourceManager m_resources{m_device};
};

TEST_F(WorldPcgAuthoringTest, PreviewDoesNotWriteAssetsDirtySceneOrHistoryAndRepeatedGenerationIsNoOp)
{
    Context().sceneDirty=false;const auto cursor=m_undo.GetCursor();const auto count=AttachedScene()->GameObjectCount();
    auto preview=Call(editor::PreviewWorldPcg,Arguments());ASSERT_TRUE(preview.ok)<<preview.message;
    EXPECT_EQ(preview.data.Find("instanceCount")->AsInt64(),64);EXPECT_FALSE(Context().sceneDirty);EXPECT_EQ(m_undo.GetCursor(),cursor);
    EXPECT_EQ(AttachedScene()->GameObjectCount(),count);EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());EXPECT_TRUE(Owners().empty());
    auto generated=Generate(Arguments());ASSERT_TRUE(generated.ok)<<generated.message;const auto first=Placements();EXPECT_EQ(first.size(),64u);
    const auto recorded=m_undo.GetCursor();auto repeated=Generate(Arguments());ASSERT_TRUE(repeated.ok)<<repeated.message;EXPECT_TRUE(repeated.noChange);EXPECT_EQ(m_undo.GetCursor(),recorded);EXPECT_EQ(Placements(),first);
    ASSERT_TRUE(m_undo.Undo());EXPECT_TRUE(Owners().empty());ASSERT_TRUE(m_undo.Redo());EXPECT_EQ(Placements(),first);
}

TEST_F(WorldPcgAuthoringTest, PartialRegenerationPreservesOutsideRowsInTheSameCell)
{
    ASSERT_TRUE(Generate(Arguments()).ok);const auto original=Placements();m_profile.rules[0].density=0;SaveProfile();
    auto args=Arguments();args.Set("boundsMin",math::Vector3{0,0,0});args.Set("boundsMax",math::Vector3{8,0,32});
    auto regenerated=Generate(args);ASSERT_TRUE(regenerated.ok)<<regenerated.message;const auto partial=Placements();EXPECT_EQ(partial.size(),48u);
    for(const auto& [key,position]:original){const auto found=partial.find(key);if(position.x<8)EXPECT_EQ(found,partial.end());else {ASSERT_NE(found,partial.end());EXPECT_VEC3_NEAR(found->second,position,0.000001f);}}
    ASSERT_TRUE(m_undo.Undo());EXPECT_EQ(Placements(),original);ASSERT_TRUE(m_undo.Redo());EXPECT_EQ(Placements(),partial);
}

TEST_F(WorldPcgAuthoringTest, MaterialMaskUsesGuidIdentityAndPaintedWeightsAcrossTerrainLayerOrders)
{
    auto* first = AttachedScene()->FindByGuid(m_terrain)->GetComponent<scene::TerrainComponent>();
    auto& neighbor = AttachedScene()->CreateGameObject("Neighbor");
    neighbor.transform.position.x = 32.0f;
    auto& second = neighbor.AddComponent<scene::TerrainComponent>();
    second.Resize(17, 17);
    second.cellSize = 2.0f;
    second.InitFlat();
    const std::string grass = "guid:11111111111111111111111111111111|Assets/Grass.mat";
    const std::string path = "guid:22222222222222222222222222222222|Assets/Path.mat";
    first->layerMaterials = {grass, path};
    second.layerMaterials = {path, grass};
    for (int z = 0; z < 17; ++z) {
        for (int x = 0; x < 17; ++x) {
            ASSERT_TRUE(first->PaintLayerAtGrid(x, z, 1, 0.5f));
            ASSERT_TRUE(second.PaintLayerAtGrid(x, z, 1, 1.0f));
        }
    }
    m_profile.bounds.max.x = 64.0f;
    m_profile.rules.front().includeMaterial = "guid:11111111111111111111111111111111|Assets/OldGrass.mat";
    m_profile.rules.front().excludeMaterial = path;
    SaveProfile();
    const auto preview = Call(editor::PreviewWorldPcg, Arguments());
    ASSERT_TRUE(preview.ok) << preview.message;
    EXPECT_EQ(preview.data.Find("instanceCount")->AsInt64(), 64);
    EXPECT_EQ(preview.data.Find("rejected")->Find("material")->AsInt64(), 64);
    for (const auto& point : preview.data.Find("points")->AsArray())
        EXPECT_GE(point.Find("position")->AsVec3().x, 32.0f);
}

TEST_F(WorldPcgAuthoringTest, EditedGeneratedOwnersRequireDetachAndDetachHistorySurvivesGeneratedRecreation)
{
    ASSERT_TRUE(Generate(Arguments()).ok);ASSERT_FALSE(Owners().empty());const auto ownerId=Owners().front()->instanceId;
    editor::OpArgs detach;detach.Set("node",ownerId);auto detached=Call(editor::DetachWorldPcg,detach);ASSERT_TRUE(detached.ok)<<detached.message;
    ASSERT_TRUE(m_undo.Undo());ASSERT_TRUE(m_undo.Undo());EXPECT_EQ(AttachedScene()->FindByGuid(ownerId),nullptr);
    ASSERT_TRUE(m_undo.Redo());ASSERT_TRUE(m_undo.Redo());auto* restored=AttachedScene()->FindByGuid(ownerId);ASSERT_NE(restored,nullptr);
    EXPECT_EQ(restored->GetComponent<scene::WorldPcgGeneratedComponent>(),nullptr);ASSERT_TRUE(m_undo.Undo());
    restored->transform.position.x=2;auto conflicted=Generate(Arguments());EXPECT_FALSE(conflicted.ok);EXPECT_EQ(conflicted.errorCode,"PCG_MANUAL_OVERRIDE");
    detached=Call(editor::DetachWorldPcg,detach);ASSERT_TRUE(detached.ok);EXPECT_EQ(restored->GetComponent<scene::WorldPcgGeneratedComponent>(),nullptr);
}

TEST_F(WorldPcgAuthoringTest, StalePreviewRejectsBeforePublishingOwners)
{
    auto args=Arguments();auto preview=Call(editor::PreviewWorldPcg,args);ASSERT_TRUE(preview.ok);
    args.Set("expected",preview.data.Find("expected")->AsString());
    auto* ground=AttachedScene()->FindByGuid(m_terrain);ground->transform.position.y=3;
    auto result=Call(editor::GenerateWorldPcg,args);EXPECT_FALSE(result.ok);EXPECT_EQ(result.errorCode,"PCG_CONFLICT");EXPECT_TRUE(Owners().empty());EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());
}

TEST_F(WorldPcgAuthoringTest, UndoRefusesToMutateAnOutputThatBecameShared)
{
    ASSERT_TRUE(Generate(Arguments()).ok);m_profile.rules[0].density=0;SaveProfile();ASSERT_TRUE(Generate(Arguments()).ok);
    ASSERT_FALSE(Owners().empty());const auto component=*Owners().front()->GetComponent<scene::InstancedMeshComponent>();
    auto& manual=AttachedScene()->CreateGameObject("Manual sibling");manual.AddComponent<scene::InstancedMeshComponent>(component);
    const auto before=manual.GetComponent<scene::InstancedMeshComponent>()->Snapshot();const auto cursor=m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo());EXPECT_EQ(m_undo.GetCursor(),cursor);EXPECT_EQ(manual.GetComponent<scene::InstancedMeshComponent>()->Snapshot(),before);
}

TEST_F(WorldPcgAuthoringTest, ProfileSaveUsesByteCompareAndSwap)
{
    editor::OpArgs inspect;inspect.Set("path",m_path);auto current=Call(editor::InspectWorldPcgProfile,inspect);ASSERT_TRUE(current.ok);
    const auto expected=current.data.Find("fingerprint")->AsString();std::string authored=current.data.Find("text")->AsString();authored+="\n";
    ASSERT_TRUE(util::FileSystem::WriteTextAtomic(m_path,authored));m_profile.name="New name";std::string updated,diagnostic;ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile,updated,diagnostic));
    editor::OpArgs save;save.Set("path",m_path);save.Set("text",updated);save.Set("expected",expected);auto rejected=Call(editor::SaveWorldPcgProfile,save);
    EXPECT_FALSE(rejected.ok);EXPECT_EQ(rejected.errorCode,"CONFLICT");std::string after;ASSERT_TRUE(util::FileSystem::ReadText(util::FileSystem::PathFromUtf8(m_path),after));EXPECT_EQ(after,authored);
}

TEST_F(WorldPcgAuthoringTest, ReplacingAProfileRequiresFingerprintWhileIdenticalSaveIsNoChange)
{
    std::string before;ASSERT_TRUE(util::FileSystem::ReadText(util::FileSystem::PathFromUtf8(m_path),before));
    m_profile.name="Replacement";std::string updated,diagnostic;ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile,updated,diagnostic));
    editor::OpArgs save;save.Set("path",m_path);save.Set("text",updated);auto rejected=Call(editor::SaveWorldPcgProfile,save);
    EXPECT_FALSE(rejected.ok);EXPECT_EQ(rejected.errorCode,"CONFLICT");std::string after;ASSERT_TRUE(util::FileSystem::ReadText(util::FileSystem::PathFromUtf8(m_path),after));EXPECT_EQ(after,before);
    const auto cursor=m_undo.GetCursor();save.Set("text",before);auto identical=Call(editor::SaveWorldPcgProfile,save);ASSERT_TRUE(identical.ok)<<identical.message;
    EXPECT_TRUE(identical.noChange);EXPECT_EQ(m_undo.GetCursor(),cursor);
}

TEST_F(WorldPcgAuthoringTest, OmittedPartitionUsesTheUniqueControllerAndRejectsAmbiguity)
{
    auto& controller=AttachedScene()->CreateGameObject("World");controller.AddComponent<scene::WorldPartitionComponent>().cellSize=64;
    auto preview=Call(editor::PreviewWorldPcg,Arguments());ASSERT_TRUE(preview.ok)<<preview.message;
    EXPECT_EQ(preview.data.Find("partition")->AsString(),controller.instanceId);EXPECT_EQ(preview.data.Find("cellCount")->AsInt64(),1);
    auto generated=Generate(Arguments());ASSERT_TRUE(generated.ok)<<generated.message;EXPECT_EQ(Owners().size(),1u);
    auto& second=AttachedScene()->CreateGameObject("Other World");second.AddComponent<scene::WorldPartitionComponent>().cellSize=32;
    preview=Call(editor::PreviewWorldPcg,Arguments());EXPECT_FALSE(preview.ok);EXPECT_EQ(preview.errorCode,"AMBIGUOUS_PARTITION");
    auto explicitRequest=Arguments();explicitRequest.Set("partition",controller.instanceId);preview=Call(editor::PreviewWorldPcg,explicitRequest);ASSERT_TRUE(preview.ok)<<preview.message;
    EXPECT_EQ(preview.data.Find("cellCount")->AsInt64(),1);
}

TEST_F(WorldPcgAuthoringTest, PartialVolumeConfigurationPreservesOmittedValuesAndExplicitlyResetsOverrides)
{
    editor::OpArgs initial;initial.Set("volume",m_volume);initial.Set("seed",37);initial.Set("protectManual",false);initial.Set("enabled",false);
    initial.Set("boundsMin",math::Vector3{2,0,3});initial.Set("boundsMax",math::Vector3{22,0,23});auto configured=Call(editor::ConfigureWorldPcgVolume,initial);ASSERT_TRUE(configured.ok)<<configured.message;
    const auto before=*AttachedScene()->FindByGuid(m_volume)->GetComponent<scene::WorldPcgVolumeComponent>();const auto cursor=m_undo.GetCursor();
    editor::OpArgs omitted;omitted.Set("volume",m_volume);configured=Call(editor::ConfigureWorldPcgVolume,omitted);ASSERT_TRUE(configured.ok)<<configured.message;EXPECT_TRUE(configured.noChange);EXPECT_EQ(m_undo.GetCursor(),cursor);
    const auto* current=AttachedScene()->FindByGuid(m_volume)->GetComponent<scene::WorldPcgVolumeComponent>();EXPECT_EQ(current->profilePath,before.profilePath);EXPECT_EQ(current->boundsMin,before.boundsMin);EXPECT_EQ(current->boundsMax,before.boundsMax);
    EXPECT_EQ(current->seed,37);EXPECT_TRUE(current->overrideSeed);EXPECT_TRUE(current->overrideBounds);EXPECT_FALSE(current->protectManual);EXPECT_FALSE(current->enabled);
    omitted.Set("seed",-1);omitted.Set("useProfileBounds",true);configured=Call(editor::ConfigureWorldPcgVolume,omitted);ASSERT_TRUE(configured.ok)<<configured.message;
    current=AttachedScene()->FindByGuid(m_volume)->GetComponent<scene::WorldPcgVolumeComponent>();EXPECT_FALSE(current->overrideSeed);EXPECT_FALSE(current->overrideBounds);EXPECT_FALSE(current->enabled);EXPECT_FALSE(current->protectManual);
}

TEST_F(WorldPcgAuthoringTest, ProfileSaveWorksWithoutAnAttachedSceneButRefusesManagedBuilds)
{
    std::string text,diagnostic;ASSERT_TRUE(scene::SerializeWorldPcgProfile(m_profile,text,diagnostic));editor::OpArgs save;
    save.Set("path",util::FileSystem::PathToUtf8(ProjectRoot()/"Assets"/"NoScene.pcg"));save.Set("text",text);
    auto* active=Context().activeScene;auto* edit=Context().editScene;Context().activeScene=nullptr;Context().editScene=nullptr;
    auto saved=Call(editor::SaveWorldPcgProfile,save);EXPECT_TRUE(saved.ok)<<saved.message;
    Context().managedBuildBusy=true;auto busy=Call(editor::SaveWorldPcgProfile,save);EXPECT_FALSE(busy.ok);EXPECT_EQ(busy.errorCode,"NOT_EDITABLE");
    Context().managedBuildBusy=false;Context().activeScene=active;Context().editScene=edit;
}

TEST_F(WorldPcgAuthoringTest, RegenerationAfterDetachKeepsManualOwnerAndUsesNewGeneratedIdentity)
{
    ASSERT_TRUE(Generate(Arguments()).ok);ASSERT_FALSE(Owners().empty());auto* manual=Owners().front();const auto manualId=manual->instanceId;
    const auto oldMetadata=*manual->GetComponent<scene::WorldPcgGeneratedComponent>();const auto before=manual->GetComponent<scene::InstancedMeshComponent>()->Snapshot();
    editor::OpArgs detach;detach.Set("node",manualId);ASSERT_TRUE(Call(editor::DetachWorldPcg,detach).ok);
    auto regenerated=Generate(Arguments());ASSERT_TRUE(regenerated.ok)<<regenerated.message;EXPECT_EQ(Owners().size(),4u);
    manual=AttachedScene()->FindByGuid(manualId);ASSERT_NE(manual,nullptr);EXPECT_EQ(manual->GetComponent<scene::WorldPcgGeneratedComponent>(),nullptr);
    EXPECT_EQ(manual->GetComponent<scene::InstancedMeshComponent>()->Snapshot(),before);
    bool replaced=false;for(auto* owner:Owners()){const auto* metadata=owner->GetComponent<scene::WorldPcgGeneratedComponent>();
        if(metadata->ruleKey==oldMetadata.ruleKey&&metadata->cellX==oldMetadata.cellX&&metadata->cellZ==oldMetadata.cellZ){EXPECT_NE(owner->instanceId,manualId);replaced=true;}}
    EXPECT_TRUE(replaced);auto repeated=Generate(Arguments());ASSERT_TRUE(repeated.ok)<<repeated.message;EXPECT_TRUE(repeated.noChange);
    EXPECT_EQ(manual->GetComponent<scene::InstancedMeshComponent>()->Snapshot(),before);
}

} /// @note namespace fbzz::tests
