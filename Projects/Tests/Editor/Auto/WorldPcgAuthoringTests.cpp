/// @file    WorldPcgAuthoringTests.cpp
/// @brief   PCG preview purity, regeneration ownership, partial edits and checked history.
/// @author  Hasegawa Jin
/// @date    2026-11-07
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <TestKit/Engine/ResourceTrackingRenderer.hpp>
#include <Tools/WorldAuthoring.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Op/OperatorGroups.hpp>
#include <Editor/Panels/WorldAuthoringPanel.hpp>
#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/SceneIO.hpp>
#include <Editor/Util/Selection.hpp>
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Asset/MeshInstancesAsset.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/TerrainComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Scene/Components/WorldPcgAuthoringComponent.hpp>
#include <Engine/Scene/Components/WorldPartitionComponent.hpp>
#include <Engine/Scene/Systems/TransformSystem.hpp>
#include <Engine/Scene/Systems/WaterSystem.hpp>
#include <Engine/Scene/TerrainHeightMapLoader.hpp>
#include <Engine/Scene/TerrainSurfaceQuery.hpp>
#include <Engine/Scene/WaterSurfaceQuery.hpp>
#include <Engine/Scene/WorldPcg.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <map>
#include <string_view>
#include <tuple>

namespace fbzz::tests {
namespace {

/// @note Replace the reference portion of a generated signature without changing its authored rows.
std::string LegacyPcgSignature(std::string_view signature, std::string_view reference,
    const asset::MeshInstancesAsset& asset)
{
    constexpr uint64_t PRIME = 1099511628211ull;
    constexpr uint64_t INVERSE_PRIME = 14886173955864302971ull;
    uint64_t hash = 0;
    const auto [end, error] = std::from_chars(signature.data(), signature.data() + signature.size(), hash, 16);
    const auto separator = reference.find('|');
    if (error != std::errc{} || end != signature.data() + signature.size() || separator == std::string_view::npos)
        return {};

    const auto append = [](uint64_t& value, uint64_t word) { value = (value ^ word) * PRIME; };
    /// @note The prior format hashed the reference hint before the same trailing component and row data.
    std::vector<uint64_t> suffix{1, 1, 0};
    const auto captureText = [&](std::string_view text) {
        for (unsigned char character : text) suffix.push_back(character);
        suffix.push_back(0);
    };
    const auto captureVector = [&](const math::Vector3& vector) {
        suffix.push_back(std::bit_cast<uint32_t>(vector.x));
        suffix.push_back(std::bit_cast<uint32_t>(vector.y));
        suffix.push_back(std::bit_cast<uint32_t>(vector.z));
    };
    captureText(asset.terrainPaintRef);
    suffix.push_back(0);
    for (const auto& part : asset.instances.Parts()) {
        captureText(part.meshPath);
        captureText(part.materialPath);
        suffix.push_back(part.lod);
    }
    for (const float height : asset.instances.LodHeights()) suffix.push_back(std::bit_cast<uint32_t>(height));
    for (const auto& row : asset.instances.Snapshot()->placements) {
        suffix.push_back(row.id);
        suffix.push_back(row.active ? 1 : 0);
        captureVector(row.position);
        captureVector(row.eulerRadians);
        captureVector(row.scale);
    }
    for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) hash = (hash * INVERSE_PRIME) ^ *it;
    hash *= INVERSE_PRIME;
    for (unsigned char character : reference.substr(separator)) append(hash, character);
    append(hash, 0);
    for (uint64_t word : suffix) append(hash, word);
    char bytes[16];
    const auto result = std::to_chars(bytes, bytes + sizeof(bytes), hash, 16);
    return std::string(bytes, result.ptr);
}

} /// @note namespace

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
    {Context().operators=nullptr;m_undo.Clear();editor::AssetDirtyRegistry::DiscardAll();asset::AssetManager::UnloadAll();EditorFixture::TearDown();}
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
    std::map<std::pair<std::string,uint32_t>,math::Vector3> Placements(std::string_view ruleKey = {})
    {
        std::map<std::pair<std::string,uint32_t>,math::Vector3> result;
        for (auto* owner : Owners()) {
            const auto* metadata = owner->GetComponent<scene::WorldPcgGeneratedComponent>();
            if (!ruleKey.empty() && metadata->ruleKey != ruleKey) continue;
            const auto snapshot = owner->GetComponent<scene::InstancedMeshComponent>()->Snapshot();
            if (!snapshot) continue;
            for (const auto& placement : snapshot->placements)
                if (placement.active) result[{owner->instanceId, placement.id}] = placement.position;
        }
        return result;
    }
    void SaveProfile() {std::string diagnostic;ASSERT_TRUE(scene::SaveWorldPcgProfile(m_path,m_profile,diagnostic))<<diagnostic;}
    void RegisterTerrainEditing()
    {
        editor::RegisterTerrainOperators(m_terrainOperators);
        editor::RegisterWorldPartitionOperators(m_terrainOperators);
        Context().operators = &m_terrainOperators;
    }
    editor::UndoStack m_undo;scene::WorldPcgProfile m_profile;std::string m_path,m_volume,m_terrain;
    editor::OperatorRegistry m_terrainOperators;
    testkit::ResourceTrackingRenderer m_device;renderer::ResourceManager m_resources{m_device};
};

TEST_F(WorldPcgAuthoringTest, RockNatureDraftIsReadOnlyAndRequiresExplicitSources)
{
    const auto count = AttachedScene()->GameObjectCount();
    const auto cursor = m_undo.GetCursor();
    editor::OpArgs args; args.Set("preset", std::string("rocks"));
    EXPECT_FALSE(Call(editor::InspectWorldPcgProfile, args).ok);
    args.Set("rockSource", std::string("guid:rock")); args.Set("pebbleSource", std::string("guid:pebble"));
    auto draft = Call(editor::InspectWorldPcgProfile, args);
    ASSERT_TRUE(draft.ok) << draft.message;
    const auto* text = draft.data.Find("text"); ASSERT_NE(text, nullptr); ASSERT_TRUE(text->IsString());
    scene::WorldPcgProfile profile; std::string diagnostic;
    ASSERT_TRUE(scene::DeserializeWorldPcgProfile(text->AsString(), profile, diagnostic)) << diagnostic;
    EXPECT_EQ(profile.rules.size(), 6u); EXPECT_EQ(profile.rules.front().prototypeRef, "guid:rock");
    EXPECT_EQ(profile.rules[1].prototypeRef, "guid:pebble");
    EXPECT_EQ(AttachedScene()->GameObjectCount(), count); EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny()); EXPECT_TRUE(Owners().empty());
    args.Set("path", m_path); EXPECT_FALSE(Call(editor::InspectWorldPcgProfile, args).ok);
}

class WorldPcgProfilePanelTest : public WorldPcgAuthoringTest {
protected:
    void SetUp() override
    {
        WorldPcgAuthoringTest::SetUp();
        editor::RegisterWorldPartitionOperators(m_operators);
        Context().operators = &m_operators;
        m_previous = ImGui::GetCurrentContext();
        m_gui = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1100, 1000};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_gui);
        ImGui::SetCurrentContext(m_previous);
        Context().operators = nullptr;
        WorldPcgAuthoringTest::TearDown();
    }

    void Frame(ImGuiID activate = 0)
    {
        ImGui::NewFrame();
        m_gui->NavActivateId = m_gui->NavActivateDownId = m_gui->NavActivatePressedId = activate;
        m_gui->NavActivateFlags = ImGuiActivateFlags_None;
        m_gui->NavInputSource = ImGuiInputSource_Keyboard;
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1100, 1000});
        m_panel.OnRender(Context());
        ImGui::Render();
    }

    ImGuiContext* m_previous = nullptr;
    ImGuiContext* m_gui = nullptr;
    editor::WorldAuthoringPanel m_panel;
    editor::OperatorRegistry m_operators;
};

TEST_F(WorldPcgProfilePanelTest, SavesDuplicatedProjectRuleAndRefusesAnExternalOverwrite)
{
    auto* volume = AttachedScene()->FindByGuid(m_volume);
    ASSERT_NE(volume, nullptr);
    editor::SelectEntity(Context(), volume->GetID());
    Frame(); Frame();
    auto* window = ImGui::FindWindowByName("###World / PCG");
    ASSERT_NE(window, nullptr);
    const ImGuiID profiles = window->GetID("Project placement profiles");
    window->StateStorage.SetInt(profiles, 1);
    Frame(window->GetID("Use selected volume"));
    Frame(ImHashStr("Load profile", 0, profiles));
    Frame(ImHashStr("Duplicate rule", 0, profiles));

    const auto save = ImHashStr("Save profile", 0, profiles);
    Frame(save);
    scene::WorldPcgProfile saved;
    std::string diagnostic;
    ASSERT_TRUE(scene::LoadWorldPcgProfile(m_path, saved, diagnostic)) << diagnostic;
    ASSERT_EQ(saved.rules.size(), 2u);
    EXPECT_EQ(saved.rules.front().key, m_profile.rules.front().key);
    EXPECT_NE(saved.rules.front().key, saved.rules.back().key);
    EXPECT_EQ(saved.rules.front().parts.front().meshPath, saved.rules.back().parts.front().meshPath);
    auto preview = Call(editor::PreviewWorldPcg, Arguments());
    ASSERT_TRUE(preview.ok) << preview.message;
    EXPECT_EQ(preview.data.Find("instanceCount")->AsInt64(), 128);

    Frame(ImHashStr("Duplicate rule", 0, profiles));
    saved.name = "External edit";
    ASSERT_TRUE(scene::SaveWorldPcgProfile(m_path, saved, diagnostic)) << diagnostic;
    Frame(save);
    ASSERT_TRUE(scene::LoadWorldPcgProfile(m_path, saved, diagnostic)) << diagnostic;
    EXPECT_EQ(saved.rules.size(), 2u);
    EXPECT_EQ(saved.name, "External edit");
}

TEST_F(WorldPcgProfilePanelTest, LivePreviewCoalescesDraftEditsAndRefreshesAfterSave)
{
    auto operation = *m_operators.Find("pcg.preview");
    std::vector<editor::OpData> previews;
    operation.exec = [execute = std::move(operation.exec), &previews](auto& context, const auto& args) {
        auto result = execute(context, args);
        previews.push_back(result.data);
        return result;
    };
    m_operators.Register(std::move(operation));
    auto* volume = AttachedScene()->FindByGuid(m_volume);
    ASSERT_NE(volume, nullptr);
    editor::SelectEntity(Context(), volume->GetID());
    Frame(); Frame();
    auto* window = ImGui::FindWindowByName("###World / PCG");
    ASSERT_NE(window, nullptr);
    const auto profiles = window->GetID("Project placement profiles");
    window->StateStorage.SetInt(profiles, 1);
    Frame(window->GetID("Use selected volume"));
    Frame(ImHashStr("Load profile", 0, profiles));
    Frame(ImHashStr("Duplicate rule", 0, profiles));
    m_panel.OnUpdate(Context(), 0.2f);
    EXPECT_TRUE(previews.empty());
    Frame(ImHashStr("Duplicate rule", 0, profiles));
    m_panel.OnUpdate(Context(), 0.2f);
    EXPECT_TRUE(previews.empty());
    m_panel.OnUpdate(Context(), 0.1f);
    ASSERT_EQ(previews.size(), 1u);
    ASSERT_TRUE(previews.back().Find("instanceCount"));
    EXPECT_EQ(previews.back().Find("instanceCount")->AsInt64(), 192);
    EXPECT_FALSE(previews.back().Find("expected"));
    m_panel.OnUpdate(Context(), 1.0f);
    EXPECT_EQ(previews.size(), 1u);

    Frame(ImHashStr("Save profile", 0, profiles));
    m_panel.OnUpdate(Context(), 0.3f);
    ASSERT_EQ(previews.size(), 2u);
    EXPECT_EQ(previews.back().Find("instanceCount")->AsInt64(), 192);
    EXPECT_TRUE(previews.back().Find("expected"));
    EXPECT_TRUE(Owners().empty());
}

TEST_F(WorldPcgAuthoringTest, DraftPreviewUsesUnsavedInputsWithoutIssuingAGenerationToken)
{
    const auto before = AttachedScene()->GameObjectCount();
    const auto revision = m_undo.GetRevision();
    auto args = Arguments();
    const auto saved = Call(editor::PreviewWorldPcg, args);
    ASSERT_TRUE(saved.ok) << saved.message;
    args.Set("live", true);
    const auto live = Call(editor::PreviewWorldPcg, args);
    ASSERT_TRUE(live.ok) << live.message;
    EXPECT_EQ(live.data.Find("expected")->AsString(), saved.data.Find("expected")->AsString());

    auto profile = m_profile;
    profile.rules.front().density = 0.0f;
    std::string text, diagnostic;
    ASSERT_TRUE(scene::SerializeWorldPcgProfile(profile, text, diagnostic)) << diagnostic;
    args.Set("draftProfileText", text);
    args.Set("draftBoundsMin", math::Vector3{0,0,0}); args.Set("draftBoundsMax", math::Vector3{8,0,32});
    const auto draft = Call(editor::PreviewWorldPcg, args);
    ASSERT_TRUE(draft.ok) << draft.message;
    EXPECT_TRUE(draft.data.Find("draft")->AsBool());
    EXPECT_FALSE(draft.data.Find("expected"));
    EXPECT_EQ(draft.data.Find("instanceCount")->AsInt64(), 0);
    EXPECT_EQ(draft.data.Find("rejected")->Find("density")->AsInt64(), 16);
    args.Set("expected", saved.data.Find("expected")->AsString());
    EXPECT_FALSE(Call(editor::GenerateWorldPcg, args).ok);

    args.Set("draftBoundsMax", math::Vector3{2048,0,2048});
    const auto overBudget = Call(editor::PreviewWorldPcg, args);
    EXPECT_FALSE(overBudget.ok);
    EXPECT_EQ(overBudget.errorCode, "PCG_LIVE_BUDGET");
    EXPECT_EQ(AttachedScene()->GameObjectCount(), before);
    EXPECT_EQ(m_undo.GetRevision(), revision);
    EXPECT_TRUE(Owners().empty());
    scene::WorldPcgProfile disk;
    ASSERT_TRUE(scene::LoadWorldPcgProfile(m_path, disk, diagnostic));
    EXPECT_FLOAT_EQ(disk.rules.front().density, m_profile.rules.front().density);
}

TEST_F(WorldPcgAuthoringTest, VolumeInspectionAndPreviewReportEffectiveBoundsAndRejectedSites)
{
    m_profile.rules.front().density = 0.0f;
    SaveProfile();
    const auto before = AttachedScene()->GameObjectCount();
    const auto revision = m_undo.GetRevision();
    const auto volume = Call(editor::InspectWorldPcgVolume, Arguments());
    ASSERT_TRUE(volume.ok) << volume.message;
    EXPECT_VEC3_NEAR(volume.data.Find("boundsMax")->AsVec3(), m_profile.bounds.max, 0.0f);

    auto args = Arguments();
    args.Set("boundsMin", math::Vector3{0,0,0}); args.Set("boundsMax", math::Vector3{8,0,32});
    const auto baseline = Call(editor::PreviewWorldPcg, args);
    ASSERT_TRUE(baseline.ok) << baseline.message;
    args.Set("maxRejectedPoints", 8);
    const auto preview = Call(editor::PreviewWorldPcg, args);
    ASSERT_TRUE(preview.ok) << preview.message;
    EXPECT_EQ(preview.data.Find("expected")->AsString(), baseline.data.Find("expected")->AsString());
    EXPECT_FLOAT_EQ(preview.data.Find("boundsMax")->AsVec3().x, 8.0f);
    EXPECT_EQ(preview.data.Find("ruleCounts")->Find("trees")->AsInt64(), 0);
    const auto& samples = preview.data.Find("rejectedPoints")->AsArray();
    ASSERT_EQ(samples.size(), 8u);
    for (const auto& sample : samples) {
        EXPECT_EQ(sample.Find("reason")->AsString(), "density");
        EXPECT_LT(sample.Find("position")->AsVec3().x, 8.0f);
    }
    EXPECT_EQ(AttachedScene()->GameObjectCount(), before);
    EXPECT_EQ(m_undo.GetRevision(), revision);
    EXPECT_TRUE(Owners().empty());
}

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

TEST_F(WorldPcgAuthoringTest, TerrainStampRefreshesNaturalPlacementAndSharesItsUndo)
{
    RegisterTerrainEditing();
    m_profile.rules.front().maxSlope = 90.0f;
    auto lowRocks = m_profile.rules.front();
    lowRocks.key = "lowRocks";
    lowRocks.maxHeight = 1.0f;
    m_profile.rules.push_back(lowRocks);
    auto highRocks = lowRocks;
    highRocks.key = "highRocks";
    highRocks.minHeight = 1.0f;
    highRocks.maxHeight = 10.0f;
    m_profile.rules.push_back(highRocks);
    SaveProfile();
    ASSERT_TRUE(Generate(Arguments()).ok);
    const auto originalTrees = Placements("trees");
    const auto originalLowRocks = Placements("lowRocks");
    const auto originalPlacements = Placements();
    ASSERT_EQ(originalTrees.size(), 64u);
    ASSERT_EQ(originalLowRocks.size(), 64u);
    ASSERT_TRUE(Placements("highRocks").empty());

    const scene::InstancedMeshComponent* generatedSet = nullptr;
    for (auto* owner : Owners()) {
        const auto* metadata = owner->GetComponent<scene::WorldPcgGeneratedComponent>();
        if (metadata->ruleKey == "trees" && metadata->cellX == 0 && metadata->cellZ == 0) {
            generatedSet = owner->GetComponent<scene::InstancedMeshComponent>();
            break;
        }
    }
    ASSERT_NE(generatedSet, nullptr);
    const auto authoredSet = *generatedSet;
    auto& manual = AttachedScene()->CreateGameObject("Hand-placed trees");
    manual.AddComponent<scene::InstancedMeshComponent>(authoredSet);
    const auto manualId = manual.instanceId;
    const auto manualBefore = authoredSet.Snapshot();
    ASSERT_NE(manualBefore, nullptr);

    const auto stampPath = util::FileSystem::PathToUtf8(File("TerrainPlacementStamp.png"));
    scene::TerrainComponent image;
    image.columns = 9;
    image.rows = 17;
    image.heightData.assign(image.VertexCount(), 1.0f);
    ASSERT_TRUE(scene::SaveHeightMapToFile(stampPath, image));
    editor::OpArgs args;
    args.Set("node", m_terrain);
    args.Set("path", stampPath);
    args.Set("center", math::Vector3{8.0f, 0.0f, 16.0f});
    args.Set("width", 16.0f);
    args.Set("depth", 32.0f);
    args.Set("heightScale", 2.0f);
    args.Set("edgeFade", 0.0f);
    const auto cursor = m_undo.GetCursor();
    const auto stamped = editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args);
    ASSERT_TRUE(stamped.ok) << stamped.message;
    EXPECT_EQ(m_undo.GetCursor(), cursor + 1u);

    auto* terrain = AttachedScene()->FindByGuid(m_terrain)->GetComponent<scene::TerrainComponent>();
    ASSERT_FLOAT_EQ(terrain->GetHeightAt(8.0f, 8.0f), 2.0f);
    const auto trees = Placements("trees");
    ASSERT_EQ(trees.size(), originalTrees.size());
    for (const auto& [identity, oldPosition] : originalTrees) {
        const auto moved = trees.find(identity);
        ASSERT_NE(moved, trees.end());
        const auto height = terrain->GetHeightAt(oldPosition.x, oldPosition.z);
        EXPECT_VEC3_NEAR(moved->second, math::Vector3(oldPosition.x, height, oldPosition.z), 0.00001f);
    }
    const auto lowRocksAfter = Placements("lowRocks");
    EXPECT_FALSE(lowRocksAfter.empty());
    EXPECT_LT(lowRocksAfter.size(), originalLowRocks.size());
    for (const auto& [identity, oldPosition] : originalLowRocks) {
        const auto height = terrain->GetHeightAt(oldPosition.x, oldPosition.z);
        const auto remaining = lowRocksAfter.find(identity);
        if (height > 1.0f) EXPECT_EQ(remaining, lowRocksAfter.end());
        else {
            ASSERT_NE(remaining, lowRocksAfter.end());
            EXPECT_NEAR(remaining->second.y, height, 0.00001f);
        }
    }
    const auto highRocksAfter = Placements("highRocks");
    ASSERT_FALSE(highRocksAfter.empty());
    for (const auto& [identity, position] : highRocksAfter) {
        EXPECT_GE(position.y, 1.0f) << identity.second;
        EXPECT_NEAR(position.y, terrain->GetHeightAt(position.x, position.z), 0.00001f);
    }
    const auto changedPlacements = Placements();
    const auto checkManual = [&] {
        auto* current = AttachedScene()->FindByGuid(manualId);
        ASSERT_NE(current, nullptr);
        EXPECT_EQ(current->GetComponent<scene::WorldPcgGeneratedComponent>(), nullptr);
        ASSERT_NE(current->GetComponent<scene::InstancedMeshComponent>(), nullptr);
        EXPECT_EQ(current->GetComponent<scene::InstancedMeshComponent>()->Snapshot(), manualBefore);
    };
    checkManual();

    ASSERT_TRUE(editor::AssetDirtyRegistry::HasAny());
    const auto stampedAgain = editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args);
    ASSERT_TRUE(stampedAgain.ok) << stampedAgain.message;
    EXPECT_EQ(m_undo.GetCursor(), cursor + 2u);
    EXPECT_FLOAT_EQ(terrain->GetHeightAt(8.0f, 8.0f), 4.0f);
    const auto latestPlacements = Placements();
    EXPECT_NE(latestPlacements, changedPlacements);
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    EXPECT_FALSE(editor::AssetDirtyRegistry::HasAny());

    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_FLOAT_EQ(terrain->GetHeightAt(8.0f, 8.0f), 2.0f);
    EXPECT_EQ(Placements(), changedPlacements);
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_FLOAT_EQ(terrain->GetHeightAt(8.0f, 8.0f), 0.0f);
    EXPECT_EQ(Placements(), originalPlacements);
    checkManual();
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_FLOAT_EQ(terrain->GetHeightAt(8.0f, 8.0f), 2.0f);
    EXPECT_EQ(Placements(), changedPlacements);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_FLOAT_EQ(terrain->GetHeightAt(8.0f, 8.0f), 4.0f);
    EXPECT_EQ(Placements(), latestPlacements);
    checkManual();
}

TEST_F(WorldPcgAuthoringTest, TerrainMaterialPaintUpdatesNaturalPlacementWithoutChangingHeights)
{
    RegisterTerrainEditing();
    auto* terrain = AttachedScene()->FindByGuid(m_terrain)->GetComponent<scene::TerrainComponent>();
    const std::string grass = "guid:11111111111111111111111111111111|Assets/Grass.mat";
    const std::string path = "guid:22222222222222222222222222222222|Assets/Path.mat";
    terrain->layerMaterials = {grass, path};
    terrain->EnsureSplat();
    m_profile.rules.front().excludeMaterial = path;
    SaveProfile();
    ASSERT_TRUE(Generate(Arguments()).ok);
    const auto before = Placements();
    ASSERT_EQ(before.size(), 64u);
    const auto heights = terrain->heightData;

    const auto maskPath = util::FileSystem::PathToUtf8(File("TerrainPlacementMask.png"));
    scene::TerrainComponent mask = *terrain;
    mask.heightData.assign(mask.VertexCount(), 1.0f);
    ASSERT_TRUE(scene::SaveHeightMapToFile(maskPath, mask));
    editor::OpArgs args;
    args.Set("node", m_terrain);
    args.Set("path", maskPath);
    args.Set("layer", 1);
    const auto cursor = m_undo.GetCursor();
    const auto painted = editor::InvokeOperator(Context(), "terrain.layer.import_mask", args);
    ASSERT_TRUE(painted.ok) << painted.message;
    EXPECT_EQ(m_undo.GetCursor(), cursor + 1u);
    EXPECT_FLOAT_EQ(terrain->GetLayerWeightAtGrid(8, 8, 1), 1.0f);
    EXPECT_EQ(terrain->heightData, heights);
    EXPECT_TRUE(Placements().empty());

    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_FLOAT_EQ(terrain->GetLayerWeightAtGrid(8, 8, 1), 0.0f);
    EXPECT_EQ(Placements(), before);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_FLOAT_EQ(terrain->GetLayerWeightAtGrid(8, 8, 1), 1.0f);
    EXPECT_TRUE(Placements().empty());
}

TEST_F(WorldPcgAuthoringTest, LocalTerrainRefreshMatchesFullQuotaAcrossCellBoundary)
{
    RegisterTerrainEditing();
    auto* ground = AttachedScene()->FindByGuid(m_terrain);
    auto* terrain = ground->GetComponent<scene::TerrainComponent>();
    terrain->Resize(129, 257);
    terrain->InitFlat();
    m_profile.grid.cellSize = 32.0f;
    m_profile.bounds.max = {256, 0, 512};
    m_profile.maxPlacementsPerCell = 96;
    m_profile.rules.front().maxHeight = 0.5f;
    m_profile.rules.front().alignToSurface = true;
    SaveProfile();
    const auto generated = Generate(Arguments());
    ASSERT_TRUE(generated.ok) << generated.message;
    const auto before = Placements();
    ASSERT_EQ(before.size(), 8u * m_profile.maxPlacementsPerCell);
    std::map<std::string, std::shared_ptr<const renderer::InstanceSetData>> distantSnapshots;
    for (auto* owner : Owners()) {
        const auto* metadata = owner->GetComponent<scene::WorldPcgGeneratedComponent>();
        const auto footprint = scene::WorldCellFootprint(m_profile.grid, {metadata->cellX, metadata->cellZ});
        if (footprint.min.z >= scene::VEGETATION_BUDGET_CELL_SIZE)
            distantSnapshots.emplace(owner->instanceId, owner->GetComponent<scene::InstancedMeshComponent>()->Snapshot());
    }
    ASSERT_GT(distantSnapshots.size(), 64u);
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);

    const auto stampPath = util::FileSystem::PathToUtf8(File("QuotaBoundaryStamp.png"));
    scene::TerrainComponent image;
    image.Resize(17, 21);
    image.heightData.assign(image.VertexCount(), 1.0f);
    ASSERT_TRUE(scene::SaveHeightMapToFile(stampPath, image));
    editor::OpArgs args;
    args.Set("node", m_terrain);
    args.Set("path", stampPath);
    args.Set("center", math::Vector3{128, 0, 64});
    args.Set("width", 32.0f);
    args.Set("depth", 40.0f);
    args.Set("heightScale", 2.0f);
    args.Set("edgeFade", 0.0f);
    const auto cursor = m_undo.GetCursor();
    const auto stamped = editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args);
    ASSERT_TRUE(stamped.ok) << stamped.message;
    EXPECT_EQ(m_undo.GetCursor(), cursor + 1u);
    ASSERT_FLOAT_EQ(terrain->GetHeightAt(128.0f, 64.0f), 2.0f);
    const auto refreshed = Placements();
    EXPECT_NE(refreshed, before);
    EXPECT_EQ(refreshed.size(), before.size());

    scene::WorldPcgResult full;
    std::string diagnostic;
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, [&](float x, float z, scene::WorldPcgSurfaceSample& out) {
        scene::TerrainSurfaceSample sample;
        if (!scene::TrySampleTerrainWorld(*terrain, ground->transform, x, z, sample)) return false;
        out.position = sample.position;
        out.normal = sample.normal;
        return true;
    }, {}, full, diagnostic)) << diagnostic;
    std::map<std::pair<scene::WorldCellId, uint32_t>, renderer::InstancePlacement> expected;
    for (const auto& group : full.groups) {
        const auto origin = scene::WorldCellOrigin(m_profile.grid, group.cell);
        for (auto placement : group.placements) {
            placement.position += origin;
            expected.emplace(std::pair{group.cell, placement.id}, placement);
        }
    }
    for (auto* owner : Owners()) {
        const auto* metadata = owner->GetComponent<scene::WorldPcgGeneratedComponent>();
        const auto* component = owner->GetComponent<scene::InstancedMeshComponent>();
        const auto snapshot = component->Snapshot();
        ASSERT_NE(snapshot, nullptr);
        const auto distant = distantSnapshots.find(owner->instanceId);
        if (distant != distantSnapshots.end()) {
            EXPECT_EQ(snapshot, distant->second);
            EXPECT_FALSE(editor::AssetDirtyRegistry::IsDirty(asset::AssetManager::ResolveAssetPath(component->m_instanceSetRef.path)));
            distantSnapshots.erase(distant);
        }
        for (const auto& placement : snapshot->placements) {
            if (!placement.active) continue;
            const auto found = expected.find({{metadata->cellX, metadata->cellZ}, placement.id});
            ASSERT_NE(found, expected.end()) << owner->name << " placement " << placement.id;
            EXPECT_VEC3_NEAR(placement.position, found->second.position, 0.00001f);
            EXPECT_VEC3_NEAR(placement.eulerRadians, found->second.eulerRadians, 0.00001f);
            EXPECT_VEC3_NEAR(placement.scale, found->second.scale, 0.00001f);
            expected.erase(found);
        }
    }
    EXPECT_TRUE(expected.empty());
    EXPECT_TRUE(distantSnapshots.empty());
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(Placements(), before);
    ASSERT_TRUE(m_undo.Redo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(Placements(), refreshed);
}

TEST_F(WorldPcgAuthoringTest, TerrainRefreshUndoRejectsExternalPlacementChanges)
{
    RegisterTerrainEditing();
    const auto generated = Generate(Arguments());
    ASSERT_TRUE(generated.ok) << generated.message;
    ASSERT_FALSE(Owners().empty());
    const auto* instances = Owners().front()->GetComponent<scene::InstancedMeshComponent>();
    const auto path = asset::AssetManager::ResolveAssetPath(instances->m_instanceSetRef.path);
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    const auto before = Placements();
    const auto stampPath = util::FileSystem::PathToUtf8(File("ExternalPlacementStamp.png"));
    scene::TerrainComponent image;
    image.Resize(17, 17);
    image.heightData.assign(image.VertexCount(), 1.0f);
    ASSERT_TRUE(scene::SaveHeightMapToFile(stampPath, image));
    editor::OpArgs args;
    args.Set("node", m_terrain);
    args.Set("path", stampPath);
    args.Set("center", math::Vector3{16, 0, 16});
    args.Set("width", 32.0f);
    args.Set("depth", 32.0f);
    args.Set("heightScale", 2.0f);
    args.Set("edgeFade", 0.0f);
    const auto stamped = editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args);
    ASSERT_TRUE(stamped.ok) << stamped.message;
    const auto refreshed = Placements();
    ASSERT_NE(refreshed, before);
    ASSERT_TRUE(editor::AssetDirtyRegistry::IsDirty(path));

    asset::MeshInstancesAsset external;
    ASSERT_TRUE(asset::LoadMeshInstancesAsset(path, external));
    auto rows = external.instances.Snapshot()->placements;
    ASSERT_FALSE(rows.empty());
    rows.front().position.y += 1.0f;
    ASSERT_TRUE(external.instances.Replace(external.instances.Parts(), external.instances.LodHeights(), rows));
    ASSERT_TRUE(asset::SaveMeshInstancesAsset(path, external));

    const auto cursor = m_undo.GetCursor();
    EXPECT_FALSE(m_undo.Undo());
    EXPECT_EQ(m_undo.GetCursor(), cursor);
    EXPECT_EQ(Placements(), refreshed);
    const auto* terrain = AttachedScene()->FindByGuid(m_terrain)->GetComponent<scene::TerrainComponent>();
    EXPECT_FLOAT_EQ(terrain->GetHeightAt(16.0f, 16.0f), 2.0f);
}

TEST_F(WorldPcgAuthoringTest, ReopenedTerrainRefreshKeepsGeneratedOwnership)
{
    RegisterTerrainEditing();
    const auto generated = Generate(Arguments());
    ASSERT_TRUE(generated.ok) << generated.message;
    ASSERT_FALSE(Owners().empty());
    const auto ownerId = Owners().front()->instanceId;
    const auto* beforeOwner = Owners().front()->GetComponent<scene::InstancedMeshComponent>();
    ASSERT_TRUE(asset::AssetDatabase::IsGuidRef(beforeOwner->m_instanceSetRef.path));
    ASSERT_EQ(editor::AssetDirtyRegistry::SaveAll(), 0);
    const auto ownerAssetPath = asset::AssetManager::ResolveAssetPath(beforeOwner->m_instanceSetRef.path);
    asset::MeshInstancesAsset ownerAsset;
    ASSERT_TRUE(asset::LoadMeshInstancesAsset(ownerAssetPath, ownerAsset));
    auto* metadata = Owners().front()->GetComponent<scene::WorldPcgGeneratedComponent>();
    const auto legacy = LegacyPcgSignature(metadata->signature, beforeOwner->m_instanceSetRef.path, ownerAsset);
    ASSERT_FALSE(legacy.empty());
    ASSERT_NE(legacy, metadata->signature);
    metadata->signature = legacy;
    const auto before = Placements();
    const auto scenePath = util::FileSystem::PathToUtf8(File("TerrainRefresh.scene"));
    ASSERT_TRUE(editor::SceneIO::Save(*AttachedScene(), scenePath));
    m_undo.Clear();
    ASSERT_TRUE(editor::SceneIO::Load(*AttachedScene(), scenePath));
    auto* owner = AttachedScene()->FindByGuid(ownerId);
    ASSERT_NE(owner, nullptr);
    const auto* reopenedOwner = owner->GetComponent<scene::InstancedMeshComponent>();
    ASSERT_NE(reopenedOwner, nullptr);
    EXPECT_FALSE(asset::AssetDatabase::IsGuidRef(reopenedOwner->m_instanceSetRef.path));
    EXPECT_EQ(owner->GetComponent<scene::WorldPcgGeneratedComponent>()->signature, legacy);
    ASSERT_EQ(Placements(), before);
    const auto same = Generate(Arguments());
    ASSERT_TRUE(same.ok) << same.message;
    EXPECT_TRUE(same.noChange);

    const auto stampPath = util::FileSystem::PathToUtf8(File("ReopenPlacementStamp.png"));
    scene::TerrainComponent image;
    image.Resize(17, 17);
    image.heightData.assign(image.VertexCount(), 1.0f);
    ASSERT_TRUE(scene::SaveHeightMapToFile(stampPath, image));
    editor::OpArgs args;
    args.Set("node", m_terrain);
    args.Set("path", stampPath);
    args.Set("center", math::Vector3{16, 0, 16});
    args.Set("width", 32.0f);
    args.Set("depth", 32.0f);
    args.Set("heightScale", 2.0f);
    args.Set("edgeFade", 0.0f);
    const auto stamped = editor::InvokeOperator(Context(), "terrain.heightmap.stamp", args);
    ASSERT_TRUE(stamped.ok) << stamped.message;
    const auto refreshed = Placements();
    ASSERT_EQ(refreshed.size(), before.size());
    for (const auto& [key, position] : before) {
        const auto found = refreshed.find(key);
        ASSERT_NE(found, refreshed.end());
        EXPECT_VEC3_NEAR(found->second, position + math::Vector3(0, 2, 0), 0.00001f);
    }
    ASSERT_TRUE(m_undo.Undo()) << m_undo.LastDiagnostic();
    EXPECT_EQ(Placements(), before);

    /// @note An unrelated asset with the same rows cannot claim the legacy signature.
    const auto differentPath = util::FileSystem::PathToUtf8(ProjectRoot() / "Assets" / "DifferentOwner.meshinstances");
    ASSERT_TRUE(asset::SaveMeshInstancesAsset(differentPath, ownerAsset));
    ASSERT_FALSE(asset::AssetDatabase::GuidFromPath(differentPath).empty());
    owner->GetComponent<scene::InstancedMeshComponent>()->m_instanceSetRef.path = differentPath;
    const auto protectedOutput = Generate(Arguments());
    EXPECT_FALSE(protectedOutput.ok);
    EXPECT_EQ(protectedOutput.errorCode, "PCG_MANUAL_OVERRIDE");
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

TEST_F(WorldPcgAuthoringTest, IndexedSurfaceSamplingMatchesFullSceneQueries)
{
    auto& world = *AttachedScene();
    world.FindByGuid(m_terrain)->transform.position.y = 4.0f;
    const auto yaw = math::Quaternion::FromAxisAngle(math::Vector3::UP, 0.4f);
    for (int i = 0; i < 6; ++i) {
        auto& object = world.CreateGameObject("Ground Tile");
        object.transform.position = {64.0f + 128.0f * i, 0.5f, 0.0f};
        object.transform.scale.x = -1.0f;
        auto& tile = object.AddComponent<scene::TerrainComponent>();
        tile.Resize(17, 17);
        tile.cellSize = 2.0f;
        tile.InitFlat();
        if (i == 0) {
            auto& rotated = world.CreateGameObject("Rotated Ground");
            rotated.transform.position = {23.0f, 5.0f, 21.0f};
            rotated.transform.rotation = yaw;
            auto& rotatedTile = rotated.AddComponent<scene::TerrainComponent>();
            rotatedTile.Resize(8, 8);
            rotatedTile.InitFlat();
        }
    }
    auto& sea = world.CreateGameObject("Horizon").AddComponent<scene::WaterComponent>();
    sea.farOceanEnabled = true;
    sea.extentX = sea.extentZ = 2.0f;
    sea.enableGerstnerWaves = true;
    sea.waveCount = 1;
    sea.waves[0] = {{1.0f, 0.0f}, 0.8f, 17.0f, 0.5f};
    for (int i = 0; i < 5; ++i) {
        auto& river = world.CreateGameObject("River");
        river.transform.position = {9.0f + 128.0f * i, 4.8f, 9.0f};
        river.transform.rotation = yaw;
        auto& water = river.AddComponent<scene::WaterComponent>();
        water.surfaceShape = scene::WaterSurfaceShape::SPLINE_RIVER;
        water.enableGerstnerWaves = false;
        water.splinePoints.resize(2);
        water.splinePoints[0].id = 1;
        water.splinePoints[1].id = 2;
        water.splinePoints[1].localPosition = {22.0f, 0.0f, 0.0f};
        for (auto& point : water.splinePoints) {
            point.width = 12.0f;
            point.depth = 4.0f;
        }
    }
    std::vector<scene::GameObject*> biomes;
    for (int i = 0; i < 6; ++i) {
        auto& object = world.CreateGameObject("Biome");
        auto& biome = object.AddComponent<scene::WorldBiomeVolumeComponent>();
        biome.boundsMin = {12.0f * i, 0.0f, 0.0f};
        biome.boundsMax = {12.0f * i + 20.0f, 10.0f, 32.0f};
        biome.biome = "bank";
        biome.density = i % 2 == 0 ? 1.0f : 0.5f;
        biome.priority = i % 3;
        biomes.push_back(&object);
    }
    std::sort(biomes.begin(), biomes.end(), [](scene::GameObject* a, scene::GameObject* b) {
        const auto first = a->GetComponent<scene::WorldBiomeVolumeComponent>()->priority;
        const auto second = b->GetComponent<scene::WorldBiomeVolumeComponent>()->priority;
        return first == second ? a->instanceId < b->instanceId : first > second;
    });
    scene::FlushWorldTransforms(world);
    for (const auto id : world.GetEntities<scene::WaterComponent>()) {
        auto& water = *world.GetComponent<scene::WaterComponent>(id);
        ASSERT_TRUE(scene::PrepareWaterSplineSnapshot(water, world.GetGameObject(id)->transform));
    }

    m_profile.bounds.max.x = 64.0f;
    m_profile.rules.front().biome = "bank";
    auto waterLevel = m_profile.rules.front();
    waterLevel.key = "waterLevel";
    waterLevel.useWaterLevel = true;
    waterLevel.minWaterOffset = -0.2f;
    waterLevel.maxWaterOffset = 0.9f;
    m_profile.rules.push_back(std::move(waterLevel));
    SaveProfile();
    const auto generated = Generate(Arguments());
    ASSERT_TRUE(generated.ok) << generated.message;

    scene::WorldPcgResult full;
    std::string diagnostic;
    const auto sampleWater = [&](scene::WorldPcgSurfaceSample& out, bool habitat) {
        for (const auto id : world.GetEntities<scene::WaterComponent>()) {
            const auto& water = *world.GetComponent<scene::WaterComponent>(id);
            const auto& pose = world.GetGameObject(id)->transform;
            scene::WaterSurfaceHeightSample sample;
            if (!scene::TrySampleInsideWaterSurfaceHeightWorld(water, pose, out.position, 0, sample)) continue;
            if (!habitat && out.position.y <= sample.position.y + 0.15f) {
                out.water = true;
                out.waterDistance = 0.0f;
                break;
            }
            if (habitat) {
                const float offset = (out.position.y - pose.worldPosition.y) - sample.baseHeightOffset;
                if (offset >= -sample.waterDepthLimit
                    && (!std::isfinite(out.waterOffset) || std::abs(offset) < std::abs(out.waterOffset)))
                    out.waterOffset = offset;
            }
        }
    };
    const auto fullSampler = [&](float x, float z, scene::WorldPcgSurfaceSample& out) {
        bool found = false;
        for (const auto id : world.GetEntities<scene::TerrainComponent>()) {
            const auto& terrain = *world.GetComponent<scene::TerrainComponent>(id);
            scene::TerrainSurfaceSample sample;
            if (!scene::TrySampleTerrainWorld(terrain, world.GetGameObject(id)->transform, x, z, sample)) continue;
            if (found && sample.position.y <= out.position.y) continue;
            out.position = sample.position;
            out.normal = sample.normal;
            found = true;
        }
        if (!found) return false;
        for (auto* object : biomes) {
            const auto& biome = *object->GetComponent<scene::WorldBiomeVolumeComponent>();
            if (x >= biome.boundsMin.x && x < biome.boundsMax.x && z >= biome.boundsMin.z && z < biome.boundsMax.z
                && out.position.y >= biome.boundsMin.y && out.position.y <= biome.boundsMax.y) {
                out.biome = biome.biome;
                out.biomeDensity = biome.density;
                break;
            }
        }
        sampleWater(out, false);
        return true;
    };
    ASSERT_TRUE(scene::GenerateWorldPcg(m_profile, fullSampler, {}, full, diagnostic,
        [&](const auto&, auto& out) { sampleWater(out, true); })) << diagnostic;
    EXPECT_GT(full.stats.waterRejected, 0u);
    EXPECT_GT(full.stats.waterLevelRejected, 0u);

    std::map<std::tuple<scene::WorldCellId, std::string, uint32_t>, math::Vector3> expected;
    for (const auto& group : full.groups)
        for (const auto& placement : group.placements)
            expected.emplace(std::tuple{group.cell, group.ruleKey, placement.id},
                placement.position + scene::WorldCellOrigin(m_profile.grid, group.cell));
    ASSERT_FALSE(expected.empty());
    for (auto* owner : Owners()) {
        const auto& metadata = *owner->GetComponent<scene::WorldPcgGeneratedComponent>();
        const auto snapshot = owner->GetComponent<scene::InstancedMeshComponent>()->Snapshot();
        ASSERT_NE(snapshot, nullptr);
        for (const auto& placement : snapshot->placements) {
            const auto found = expected.find({{metadata.cellX, metadata.cellZ}, metadata.ruleKey, placement.id});
            ASSERT_NE(found, expected.end());
            EXPECT_VEC3_NEAR(placement.position, found->second, 0.00001f);
            expected.erase(found);
        }
    }
    EXPECT_TRUE(expected.empty());
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
