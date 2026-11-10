/// @file    SceneSerializerTests.cpp
/// @brief   シーンの保存・復元が往復し、GPU リソース無しでもデータが揃うことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// @note 「保存して開いたら同じ」はシーンで最も事故が多い契約。項目を足したときに
/// @note 片側だけ書き忘れると、保存はできるのに開くと既定値へ戻る ── エディタで
/// @note 作業した内容が «静かに» 消え、履歴からも追えない。
///
/// @note ここでは LoadData (GPU リソースを作らない復元) を使う。デバイスを要求しないので、
/// @note 往復の検証そのものがテストとして成立する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Scene/GameObject.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/Script.hpp>
#include <Engine/Scene/ScriptComponent.hpp>
#include <Engine/Scene/Components/LightComponent.hpp>
#include <Engine/Scene/Components/InstancedMeshComponent.hpp>
#include <Engine/Scene/Components/MeshRenderer.hpp>
#include <Engine/Scene/Components/AnimatorComponent.hpp>
#include <Engine/Scene/Components/TerrainGridComponent.hpp>
#include <Engine/Scene/Components/WaterComponent.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <Math/MathUtils.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>

#include <memory>
#include <sstream>
#include <string>

namespace fbzz::tests {

class SceneSerializerTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }

    std::string ScenePath(const std::string& name = "test.scene") const
    {
        return m_temp.File(name).generic_string();
    }

    /// @brief scene を保存して、GPU リソース抜きで読み直す。
    std::unique_ptr<scene::Scene> RoundTrip(scene::Scene& source)
    {
        const std::string path = ScenePath();
        if (!scene::SceneSerializer::Save(source, path)) return nullptr;
        return scene::SceneSerializer::LoadData(path);
    }

private:
    testkit::TempDir m_temp{"scene"};
};

/// @name 骨格

TEST_F(SceneSerializerTest, SavesAndReloadsAnEmptyScene)
{
    scene::Scene source;

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->GetRootGameObjects().empty());
}

TEST_F(SceneSerializerTest, MigratesLegacyWaterAndPersistsTheNewSeaControls)
{
    const auto scene = scene::SceneSerializer::LoadDataFromText(R"(
[[gameobjects]]
name = 'Ocean'
[gameobjects.WaterComponent]
farOceanEnabled = true
waveAmplitudeScale = 0.75
[[gameobjects]]
name = 'Lake'
[gameobjects.WaterComponent]
farOceanEnabled = false
[[gameobjects]]
name = 'Shared'
[gameobjects.WaterComponent]
farOceanEnabled = true
wavesAssetRef = 'Assets/Shared.fzdata'
)", ScenePath());
    ASSERT_NE(scene, nullptr);
    auto* ocean = scene->Find("Ocean")->GetComponent<scene::WaterComponent>();
    const auto* lake = scene->Find("Lake")->GetComponent<scene::WaterComponent>();
    const auto* shared = scene->Find("Shared")->GetComponent<scene::WaterComponent>();
    ASSERT_NE(ocean, nullptr);
    ASSERT_NE(lake, nullptr);
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(ocean->waveAuthoring.preset, scene::WaterSeaPreset::OFFSHORE);
    EXPECT_EQ(lake->waveAuthoring.preset, scene::WaterSeaPreset::CALM);
    EXPECT_EQ(shared->waveAuthoring.preset, scene::WaterSeaPreset::AUTHORED);
    EXPECT_EQ(shared->wavesAssetRef.path, "Assets/Shared.fzdata");
    EXPECT_FLOAT_EQ(ocean->waveAmplitudeScale, 0.75f);
    auto& authoring = ocean->waveAuthoring;
    authoring.overrideHeading = true;
    authoring.headingDegrees = -30.0f;
    authoring.spreadDegrees = 70.0f;
    authoring.secondaryHeadingDegrees = 20.0f;
    authoring.secondaryStrength = 0.25f;
    authoring.rippleMode = scene::WaterRippleDirectionMode::MANUAL;
    authoring.rippleHeadingDegrees = 40.0f;
    const auto restored = RoundTrip(*scene);
    ASSERT_NE(restored, nullptr);
    const auto& loaded = restored->Find("Ocean")->GetComponent<scene::WaterComponent>()->waveAuthoring;
    EXPECT_EQ(loaded.preset, scene::WaterSeaPreset::OFFSHORE);
    EXPECT_TRUE(loaded.overrideHeading);
    EXPECT_FLOAT_EQ(loaded.headingDegrees, -30.0f);
    EXPECT_FLOAT_EQ(loaded.spreadDegrees, 70.0f);
    EXPECT_FLOAT_EQ(loaded.secondaryHeadingDegrees, 20.0f);
    EXPECT_FLOAT_EQ(loaded.secondaryStrength, 0.25f);
    EXPECT_EQ(loaded.rippleMode, scene::WaterRippleDirectionMode::MANUAL);
    EXPECT_FLOAT_EQ(loaded.rippleHeadingDegrees, 40.0f);
}

TEST_F(SceneSerializerTest, KeepsDirectionalShadowBakePresetsAndDefaultsLegacyLightsToRealtime)
{
    scene::Scene source;
    auto& light = source.CreateGameObject("Sun").AddComponent<scene::LightComponent>();
    light.shadowMode = scene::LightComponent::ShadowMode::AUTO_BAKE;
    light.shadowBakeDayDirection = {0, -1, 0};
    light.shadowBakeEveningDirection = {0.6f, -0.2f, 0.7f};
    source.CreateGameObject("Rock").AddComponent<scene::MeshRenderer>().bakeStaticShadow = true;

    const auto expectBaked = [](scene::Scene& restored) {
        auto* object = restored.Find("Sun");
        ASSERT_NE(object, nullptr);
        const auto* component = object->GetComponent<scene::LightComponent>();
        ASSERT_NE(component, nullptr);
        EXPECT_EQ(component->shadowMode, scene::LightComponent::ShadowMode::AUTO_BAKE);
        EXPECT_EQ(component->shadowBakeDayDirection, (math::Vector3{0, -1, 0}));
        EXPECT_EQ(component->shadowBakeEveningDirection, (math::Vector3{0.6f, -0.2f, 0.7f}));
        auto* rock = restored.Find("Rock");
        ASSERT_NE(rock, nullptr);
        const auto* mesh = rock->GetComponent<scene::MeshRenderer>();
        ASSERT_NE(mesh, nullptr);
        EXPECT_TRUE(mesh->bakeStaticShadow);
    };
    auto saved = RoundTrip(source);
    ASSERT_NE(saved, nullptr);
    expectBaked(*saved);

    std::string diagnostic;
    const auto cloneText = scene::SceneSerializer::CapturePassiveClone(source, diagnostic);
    ASSERT_FALSE(cloneText.empty());
    auto cloned = scene::SceneSerializer::LoadDataFromText(cloneText, ScenePath());
    ASSERT_NE(cloned, nullptr);
    expectBaked(*cloned);

    std::istringstream lines(cloneText);
    std::string legacy;
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("shadowMode") == std::string::npos && line.find("shadowBake") == std::string::npos
            && line.find("bakeStaticShadow") == std::string::npos)
            legacy += line + '\n';
    }
    auto old = scene::SceneSerializer::LoadDataFromText(legacy, ScenePath());
    ASSERT_NE(old, nullptr);
    auto* oldObject = old->Find("Sun");
    ASSERT_NE(oldObject, nullptr);
    const auto* oldLight = oldObject->GetComponent<scene::LightComponent>();
    ASSERT_NE(oldLight, nullptr);
    EXPECT_EQ(oldLight->shadowMode, scene::LightComponent::ShadowMode::REALTIME);
    EXPECT_EQ(oldLight->shadowBakeDayDirection, math::Vector3::ZERO);
    EXPECT_EQ(oldLight->shadowBakeEveningDirection, math::Vector3::ZERO);
    auto* oldRock = old->Find("Rock");
    ASSERT_NE(oldRock, nullptr);
    const auto* oldMesh = oldRock->GetComponent<scene::MeshRenderer>();
    ASSERT_NE(oldMesh, nullptr);
    EXPECT_FALSE(oldMesh->bakeStaticShadow);
}

TEST_F(SceneSerializerTest, KeepsVegetationCellBudgetsInTheSceneAndPlayClone)
{
    using Instances = scene::InstancedMeshComponent;
    const Instances::VegetationBudget authored{true, 48, 16, 8};
    scene::Scene source;
    source.CreateGameObject("Giant Trees").AddComponent<Instances>().m_vegetationBudget = authored;

    const auto expectBudget = [](scene::Scene& restored, const Instances::VegetationBudget& expected) {
        auto* owner = restored.Find("Giant Trees");
        ASSERT_NE(owner, nullptr);
        const auto* instances = owner->GetComponent<Instances>();
        ASSERT_NE(instances, nullptr);
        EXPECT_EQ(instances->m_vegetationBudget, expected);
    };
    auto saved = RoundTrip(source);
    ASSERT_NE(saved, nullptr);
    expectBudget(*saved, authored);

    std::string diagnostic;
    const auto cloneText = scene::SceneSerializer::CapturePassiveClone(source, diagnostic);
    ASSERT_FALSE(cloneText.empty());
    auto cloned = scene::SceneSerializer::LoadDataFromText(cloneText, ScenePath());
    ASSERT_NE(cloned, nullptr);
    expectBudget(*cloned, authored);

    std::istringstream lines(cloneText);
    std::string legacy, line;
    while (std::getline(lines, line)) {
        if (line.find("vegetation") == std::string::npos) legacy += line + '\n';
    }
    auto old = scene::SceneSerializer::LoadDataFromText(legacy, ScenePath());
    ASSERT_NE(old, nullptr);
    expectBudget(*old, {});
}

TEST_F(SceneSerializerTest, KeepsAnimatorPoseLodAcrossSaveAndPassivePlayCloneAndDefaultsLegacyData)
{
    scene::Scene source;
    auto& animator = source.CreateGameObject("Crowd").AddComponent<scene::AnimatorComponent>();
    ASSERT_TRUE(animator.SetUpdateLod(true, 2, 8, 1.0f / 30, 1.0f / 15));
    const auto check = [](scene::Scene& restored) {
        auto* object = restored.Find("Crowd");
        ASSERT_NE(object, nullptr);
        const auto* value = object->GetComponent<scene::AnimatorComponent>();
        ASSERT_NE(value, nullptr);
        EXPECT_TRUE(value->poseLodEnabled);
        EXPECT_FLOAT_EQ(value->poseLodNearDistance, 2);
        EXPECT_FLOAT_EQ(value->poseLodFarDistance, 8);
        EXPECT_NEAR(value->poseLodMidInterval, 1.0f / 30, 1.0e-6f);
        EXPECT_NEAR(value->poseLodFarInterval, 1.0f / 15, 1.0e-6f);
    };
    auto saved = RoundTrip(source);
    ASSERT_NE(saved, nullptr); check(*saved);
    std::string diagnostic;
    const auto cloneText = scene::SceneSerializer::CapturePassiveClone(source, diagnostic);
    ASSERT_FALSE(cloneText.empty()); EXPECT_TRUE(diagnostic.empty());
    auto cloned = scene::SceneSerializer::LoadDataFromText(cloneText, ScenePath());
    ASSERT_NE(cloned, nullptr); check(*cloned);
    std::istringstream lines(cloneText);
    std::string legacy, line;
    while (std::getline(lines, line)) if (line.find("poseLod") == std::string::npos) legacy += line + '\n';
    auto old = scene::SceneSerializer::LoadDataFromText(legacy, ScenePath());
    ASSERT_NE(old, nullptr);
    const auto* oldAnimator = old->Find("Crowd")->GetComponent<scene::AnimatorComponent>();
    ASSERT_NE(oldAnimator, nullptr);
    EXPECT_FALSE(oldAnimator->poseLodEnabled);
    EXPECT_FLOAT_EQ(oldAnimator->poseLodNearDistance, 20);
    EXPECT_FLOAT_EQ(oldAnimator->poseLodFarDistance, 60);
}

TEST_F(SceneSerializerTest, KeepsEveryGameObject)
{
    scene::Scene source;
    source.CreateGameObject("Alpha");
    source.CreateGameObject("Beta");
    source.CreateGameObject("Gamma");

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored->Find("Alpha"), nullptr);
    EXPECT_NE(restored->Find("Beta"), nullptr);
    EXPECT_NE(restored->Find("Gamma"), nullptr);
    EXPECT_EQ(restored->GetRootGameObjects().size(), 3u);
}

TEST_F(SceneSerializerTest, KeepsNameTagAndLayer)
{
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Player");
    object.tag   = "Enemy";
    object.layer = 4;

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Player");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->tag, "Enemy");
    EXPECT_EQ(reloaded->layer, 4);
}

TEST_F(SceneSerializerTest, KeepsThePersistentIdentifier)
{
    /// @note instanceId はクロスオブジェクト参照 (IK の Pole/Target 等) の永続キー。
    /// @note 保存で振り直すと、開き直した瞬間に参照が全部切れる。
    scene::Scene source;
    const std::string id = source.CreateGameObject("Player").instanceId;
    ASSERT_FALSE(id.empty());

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored->FindByGuid(id), nullptr);
}

TEST_F(SceneSerializerTest, KeepsTheActiveFlag)
{
    scene::Scene source;
    source.CreateGameObject("Hidden").SetActive(false);

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Hidden");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_FALSE(reloaded->activeSelf());
}

/// @name Transform

TEST_F(SceneSerializerTest, KeepsTheLocalTransform)
{
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Moved");
    object.transform.position = { 1.5f, -2.5f, 3.25f };
    object.transform.rotation =
        math::Quaternion::FromAxisAngle(math::Vector3::UP, math::ToRad(30.0f));
    object.transform.scale = { 2.0f, 0.5f, 1.0f };

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Moved");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_VEC3_NEAR(reloaded->transform.position, math::Vector3(1.5f, -2.5f, 3.25f),
                     testkit::kLooseTolerance);
    EXPECT_VEC3_NEAR(reloaded->transform.scale, math::Vector3(2.0f, 0.5f, 1.0f),
                     testkit::kLooseTolerance);
    EXPECT_QUAT_NEAR(reloaded->transform.rotation, object.transform.rotation,
                     testkit::kLooseTolerance);
}

/// @name 階層

TEST_F(SceneSerializerTest, KeepsTheParentChildHierarchy)
{
    scene::Scene source;
    scene::GameObject& parent = source.CreateGameObject("Parent");
    scene::GameObject& child  = source.CreateGameObject("Child");
    child.SetParent(parent);

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    /// @note 親子が復元されていれば、ルートは親だけになる。
    EXPECT_EQ(restored->GetRootGameObjects().size(), 1u);
    EXPECT_NE(restored->Find("Child"), nullptr);
}

TEST_F(SceneSerializerTest, KeepsDeepHierarchies)
{
    scene::Scene source;
    scene::GameObject& a = source.CreateGameObject("A");
    scene::GameObject& b = source.CreateGameObject("B");
    scene::GameObject& c = source.CreateGameObject("C");
    b.SetParent(a);
    c.SetParent(b);

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->GetRootGameObjects().size(), 1u);
    EXPECT_NE(restored->Find("C"), nullptr);
}

TEST_F(SceneSerializerTest, TerrainGridResolvesForwardAndMissingCellGuids)
{
    const auto restored = scene::SceneSerializer::LoadDataFromText(R"(
[[gameobjects]]
name = 'Grid'
instanceId = 'grid'
[gameobjects.TerrainGridComponent]
cellCountX = 3
cellCountZ = 1
cells = ['cell', 'missing', '']
[[gameobjects]]
name = 'Cell'
instanceId = 'cell'
)", ScenePath());
    ASSERT_NE(restored, nullptr);
    auto* gridOwner = restored->FindByGuid("grid");
    auto* cell = restored->FindByGuid("cell");
    ASSERT_NE(gridOwner, nullptr);
    ASSERT_NE(cell, nullptr);
    const auto* grid = gridOwner->GetComponent<scene::TerrainGridComponent>();
    ASSERT_NE(grid, nullptr);
    EXPECT_EQ(grid->GetCell(0, 0), cell->GetID());
    EXPECT_EQ(grid->GetCell(1, 0), scene::EntityID::INVALID);
    EXPECT_EQ(grid->GetCell(2, 0), scene::EntityID::INVALID);
}

/// @name 実行時生成物

TEST_F(SceneSerializerTest, DoesNotSaveRuntimeGeneratedObjects)
{
    /// @note VFX のノード実体や Water の飛沫。保存すると、開くたびにゾンビ GO が増える。
    scene::Scene source;
    source.CreateGameObject("Authored");
    source.CreateGameObject("Spawned").SetRuntimeGenerated(true);

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored->Find("Authored"), nullptr);
    EXPECT_EQ(restored->Find("Spawned"), nullptr);
}

/// @name GPU リソースを作らない復元

TEST_F(SceneSerializerTest, KeepsTheMeshPathWithoutBuildingGpuResources)
{
    /// @note これが «データ復元と GPU リソース生成を分けた» ことの中身。
    /// @note デバイス無しでも参照 (パス) は完全に残り、実体だけが後回しになる。
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Prop");
    scene::MeshRenderer renderer{};
    renderer.meshPath    = "Assets/Models/Prop.fbx";
    renderer.castShadows = false;
    object.AddComponent<scene::MeshRenderer>(std::move(renderer));

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    scene::GameObject* reloaded = restored->Find("Prop");
    ASSERT_NE(reloaded, nullptr);
    auto* mr = reloaded->GetComponent<scene::MeshRenderer>();
    ASSERT_NE(mr, nullptr);
    EXPECT_EQ(mr->meshPath, "Assets/Models/Prop.fbx");
    EXPECT_FALSE(mr->castShadows);
    /// @note GPU リソースは作らない
    EXPECT_EQ(mr->mesh, nullptr);
}

TEST_F(SceneSerializerTest, KeepsSeveralMeshPathsIndependently)
{
    /// @note 後から ResolveMeshes() で結び直すのはこの meshPath。1 つでも落ちると
    /// @note «そのオブジェクトだけ描かれない» になる。
    scene::Scene source;
    for (int i = 0; i < 3; ++i) {
        scene::GameObject& object = source.CreateGameObject("Prop" + std::to_string(i));
        scene::MeshRenderer renderer{};
        renderer.meshPath = "Assets/Models/Prop" + std::to_string(i) + ".fbx";
        object.AddComponent<scene::MeshRenderer>(std::move(renderer));
    }

    const std::unique_ptr<scene::Scene> restored = RoundTrip(source);

    ASSERT_NE(restored, nullptr);
    for (int i = 0; i < 3; ++i) {
        scene::GameObject* reloaded = restored->Find("Prop" + std::to_string(i));
        ASSERT_NE(reloaded, nullptr) << i;
        auto* mr = reloaded->GetComponent<scene::MeshRenderer>();
        ASSERT_NE(mr, nullptr) << i;
        EXPECT_EQ(mr->meshPath, "Assets/Models/Prop" + std::to_string(i) + ".fbx");
    }
}

/// @name 壊れた入力

TEST_F(SceneSerializerTest, RejectsTextThatIsNotToml)
{
    EXPECT_EQ(scene::SceneSerializer::LoadDataFromText("{{{ not toml ]]]", "broken.scene"),
              nullptr);
}

TEST_F(SceneSerializerTest, RejectsAMissingFile)
{
    EXPECT_EQ(scene::SceneSerializer::LoadData(ScenePath("does_not_exist.scene")), nullptr);
}

TEST_F(SceneSerializerTest, AcceptsAnEmptyDocument)
{
    /// @note 空のシーンファイルは «壊れている» ではなく «何も無い»。
    const std::unique_ptr<scene::Scene> restored =
        scene::SceneSerializer::LoadDataFromText("", "empty.scene");

    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->GetRootGameObjects().empty());
}

/// @name 安定性

TEST_F(SceneSerializerTest, SavingTwiceProducesTheSameFile)
{
    /// @note 触っていないのに差分が出ると、シーンのコミットに毎回ノイズが混ざる。
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Stable");
    object.transform.position = { 1.0f, 2.0f, 3.0f };

    ASSERT_TRUE(scene::SceneSerializer::Save(source, ScenePath("a.scene")));
    ASSERT_TRUE(scene::SceneSerializer::Save(source, ScenePath("b.scene")));

    std::string first;
    std::string second;
    ASSERT_TRUE(util::FileSystem::ReadText(ScenePath("a.scene"), first));
    ASSERT_TRUE(util::FileSystem::ReadText(ScenePath("b.scene"), second));
    EXPECT_EQ(first, second);
}

TEST_F(SceneSerializerTest, ASecondRoundTripChangesNothing)
{
    scene::Scene source;
    scene::GameObject& object = source.CreateGameObject("Stable");
    object.tag                = "Player";
    object.transform.position = { 1.0f, 2.0f, 3.0f };

    const std::unique_ptr<scene::Scene> once = RoundTrip(source);
    ASSERT_NE(once, nullptr);
    const std::unique_ptr<scene::Scene> twice = RoundTrip(*once);

    ASSERT_NE(twice, nullptr);
    scene::GameObject* reloaded = twice->Find("Stable");
    ASSERT_NE(reloaded, nullptr);
    EXPECT_EQ(reloaded->tag, "Player");
    EXPECT_VEC3_NEAR(reloaded->transform.position, math::Vector3(1.0f, 2.0f, 3.0f),
                     testkit::kLooseTolerance);
}

TEST_F(SceneSerializerTest, MissingScriptsKeepStableSavedIdentityThroughSavePlayRestoreAndClone)
{
    scene::Scene source;
    auto& owner = source.CreateGameObject("IdentityOwner");
    auto& scripts = owner.AddComponent<scene::ScriptComponent>();
    auto& first = scripts.scripts.emplace_back();
    first.savedScriptId = "11111111-1111-4111-8111-111111111111";
    first.serialized = std::make_unique<scene::SerializedScriptData>(scene::SerializedScriptData{"MissingIdentityComponent", "value = 1", true, first.savedScriptId});
    auto& second = scripts.scripts.emplace_back();
    second.savedScriptId = "22222222-2222-4222-8222-222222222222";
    second.serialized = std::make_unique<scene::SerializedScriptData>(scene::SerializedScriptData{"MissingIdentityComponent", "value = 2", false, second.savedScriptId});
    const std::string ownerGuid = owner.instanceId;
    const std::string snapshot = scene::SceneSerializer::SaveToText(source);
    ASSERT_FALSE(snapshot.empty());
    auto restored = scene::SceneSerializer::LoadDataFromText(snapshot, "play.fbzz");
    ASSERT_NE(restored, nullptr);
    auto* restoredOwner = restored->FindByGuid(ownerGuid);
    ASSERT_NE(restoredOwner, nullptr);
    auto* restoredScripts = restoredOwner->GetComponent<scene::ScriptComponent>();
    ASSERT_NE(restoredScripts, nullptr);
    ASSERT_EQ(restoredScripts->scripts.size(), 2u);
    EXPECT_EQ(restoredScripts->scripts[0].savedScriptId, scripts.scripts[0].savedScriptId);
    EXPECT_EQ(restoredScripts->scripts[1].savedScriptId, scripts.scripts[1].savedScriptId);
    EXPECT_EQ(restoredScripts->scripts[0].serialized->savedScriptId, scripts.scripts[0].savedScriptId);
    EXPECT_NE(restoredScripts->scripts[0].inspectionId, scripts.scripts[0].inspectionId);
    EXPECT_EQ(scene::SceneSerializer::SaveToText(*restored), snapshot);
    auto& cloneOwner = restored->CreateGameObject("IdentityClone");
    auto cloned = scene::CloneScriptComponent(*restoredScripts, restored.get(), restored.get(), &cloneOwner);
    ASSERT_EQ(cloned.scripts.size(), 2u);
    EXPECT_EQ(cloned.scripts[0].savedScriptId, scripts.scripts[0].savedScriptId);
    EXPECT_NE(cloneOwner.instanceId, ownerGuid);
    cloneOwner.AddComponent<scene::ScriptComponent>(std::move(cloned));
    EXPECT_FALSE(scene::SceneSerializer::SaveToText(*restored).empty());
}

TEST_F(SceneSerializerTest, RejectsOwnerLocalDuplicateIdsBeforeLoadingOrAppendingAnyObjects)
{
    const std::string text = R"([[gameobjects]]
name = 'DuplicateIdentity'
instanceId = 'duplicate-owner'
[[gameobjects.ScriptComponents]]
type = 'MissingIdentityComponent'
savedScriptId = '11111111-1111-4111-8111-111111111111'
[[gameobjects.ScriptComponents]]
type = 'MissingIdentityComponent'
savedScriptId = '11111111-1111-4111-8111-111111111111'
)";
    EXPECT_EQ(scene::SceneSerializer::LoadDataFromText(text, "duplicate.fbzz"), nullptr);
    scene::Scene existing;
    auto& sentinel = existing.CreateGameObject("Unchanged");
    const auto sentinelId = sentinel.GetID();
    std::vector<scene::EntityID> roots;
    EXPECT_FALSE(scene::SceneSerializer::AppendObjects(existing, text, nullptr, roots));
    EXPECT_TRUE(roots.empty());
    EXPECT_EQ(existing.GameObjectCount(), 1u);
    EXPECT_NE(existing.GetGameObject(sentinelId), nullptr);
}

TEST_F(SceneSerializerTest, RejectsDuplicateIdsOnSaveAndCloneAndAssignsLegacyMissingIdsOnce)
{
    const std::string text = R"([[gameobjects]]
name = 'LegacyIdentity'
[[gameobjects.ScriptComponents]]
type = 'MissingIdentityComponent'
[[gameobjects.ScriptComponents]]
type = 'MissingIdentityComponent'
)";
    auto restored = scene::SceneSerializer::LoadDataFromText(text, "legacy.fbzz");
    ASSERT_NE(restored, nullptr);
    auto* owner = restored->Find("LegacyIdentity");
    auto* scripts = owner->GetComponent<scene::ScriptComponent>();
    ASSERT_EQ(scripts->scripts.size(), 2u);
    EXPECT_NE(scripts->scripts[0].savedScriptId, scripts->scripts[1].savedScriptId);
    const std::string saved = scene::SceneSerializer::SaveToText(*restored);
    EXPECT_FALSE(saved.empty());
    scripts->scripts[1].savedScriptId = scripts->scripts[0].savedScriptId;
    std::string diagnostic;
    EXPECT_TRUE(scene::SceneSerializer::SaveToText(*restored, {}, diagnostic).empty());
    EXPECT_FALSE(diagnostic.empty());
    auto& cloneOwner = restored->CreateGameObject("InvalidClone");
    EXPECT_TRUE(scene::CloneScriptComponent(*scripts, restored.get(), restored.get(), &cloneOwner).scripts.empty());
}

} /// @note namespace fbzz::tests
