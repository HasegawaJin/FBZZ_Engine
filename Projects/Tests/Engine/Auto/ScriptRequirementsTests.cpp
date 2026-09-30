/// @file    ScriptRequirementsTests.cpp
/// @brief   必須設定の診断、開始の停止、保存形式の互換性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/DataAsset.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Core/Scheduler/SystemContext.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/SceneSerializer.hpp>
#include <Engine/Scene/ScriptModules.hpp>
#include <Engine/Scene/ScriptValidation.hpp>
#include <Engine/Scene/Systems/ScriptSystem.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Physics/World.hpp>
#include <fstream>

namespace fbzz::tests {
namespace {
class RequirementTuning : public asset::DataAsset {
    FBZZ_DATA_ASSET(RequirementTuning)
public:
    FBZZ_FIELD(float, speed, 3.0f, "Speed")
};
FBZZ_REFLECT(RequirementTuning)

class RequiredPeer : public scene::Script { FBZZ_SCRIPT(RequiredPeer) };
FBZZ_REFLECT(RequiredPeer)

class RequiredSettings : public scene::Script {
    FBZZ_SCRIPT(RequiredSettings)
    FBZZ_REQUIRE_SCRIPT(RequiredPeer)
public:
    FBZZ_REQUIRED_ASSET(RequirementTuning, tuning, "Tuning")
    FBZZ_REQUIRED_REF(RequiredPeer, target, "Target")
    FBZZ_OBSERVE(int, observed, ReadObservation(), "Observation")
    int starts = 0;
    int updates = 0;
    int* destroyed = nullptr;
    mutable int observations = 0;
    int ReadObservation() const { return ++observations; }
    void OnStart() override { ++starts; }
    void OnUpdate() override { ++updates; }
    void OnDestroy() override { if (destroyed) ++*destroyed; }
};
FBZZ_REFLECT(RequiredSettings)

class SuppliesPeer : public scene::Script {
public:
    void OnAwake() override { scene.Self()->AddScript<RequiredPeer>(); }
};
} /// @note namespace

class ScriptRequirementsTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"ScriptRequirements"};
    scene::Scene m_scene;
    physics::World m_world;
    std::string m_assetPath;
    void SetUp() override
    {
        EngineFixture::SetUp();
        asset::DataAssetFactory::Register<RequirementTuning>();
        m_assetPath = m_temp.File("tuning.fzdata").generic_string();
        std::ofstream(m_assetPath) << "type = 'RequirementTuning'\nspeed = 4.0\n";
    }
    void TearDown() override
    {
        asset::DataAssetRegistry::ClearCache();
        EngineFixture::TearDown();
    }
    void Tick()
    {
        SystemContext context{m_scene, m_world, nullptr, nullptr, 0.016f, 0.02f, true, true};
        scene::ScriptSystem{}.Update(context);
    }
    RequiredSettings& AddConfigured()
    {
        auto& go = m_scene.CreateGameObject("Configured");
        go.AddScript<RequiredPeer>();
        auto& script = go.AddScript<RequiredSettings>();
        script.tuning.ref.path = m_assetPath;
        script.target.ref.id = go.GetID();
        return script;
    }
};

TEST_F(ScriptRequirementsTest, MissingSettingsReportObjectScriptAndPersistentKeyWithoutObserving)
{
    auto& go = m_scene.CreateGameObject("Player");
    auto& script = go.AddScript<RequiredSettings>();
    std::vector<scene::ScriptRequirementIssue> issues;
    scene::CollectScriptRequirementIssues(go, script, issues);
    ASSERT_EQ(issues.size(), 3u);
    EXPECT_EQ(issues[0].kind, scene::ScriptRequirementKind::Script);
    EXPECT_EQ(issues[1].fieldKey, "tuning");
    EXPECT_EQ(issues[2].fieldKey, "target");
    EXPECT_EQ(script.observations, 0);
    for (const auto& issue : issues) {
        EXPECT_TRUE(issue.blocksStart);
        EXPECT_FALSE(issue.addable);
        EXPECT_NE(scene::FormatScriptRequirementIssue(issue).find("Player / RequiredSettings"), std::string::npos);
    }
}

TEST_F(ScriptRequirementsTest, MissingSettingsBlockStartAndCannotBeBypassedByEnabling)
{
    auto& script = m_scene.CreateGameObject("Missing").AddScript<RequiredSettings>();
    Tick();
    EXPECT_TRUE(script.RequirementsBlocked());
    EXPECT_FALSE(script.enabled);
    script.enabled = true;
    Tick();
    EXPECT_EQ(script.starts, 0);
    EXPECT_EQ(script.updates, 0);
    EXPECT_FALSE(script.ExecuteCallback(&scene::Script::OnUpdate, "OnUpdate"));
    EXPECT_EQ(script.updates, 0);
}

TEST_F(ScriptRequirementsTest, BlockedStartStillCleansUpAwakeResourcesOnDestroyAndClear)
{
    int destroyed = 0;
    auto& first = m_scene.CreateGameObject("First");
    const auto firstId = first.GetID();
    first.AddScript<RequiredSettings>().destroyed = &destroyed;
    m_scene.CreateGameObject("Second").AddScript<RequiredSettings>().destroyed = &destroyed;
    Tick();
    ASSERT_TRUE(m_scene.DestroyGameObject(firstId));
    EXPECT_EQ(destroyed, 1);
    m_scene.Clear();
    EXPECT_EQ(destroyed, 2);
}

TEST_F(ScriptRequirementsTest, ConfiguredSettingsStartAndKeepExistingSerializationKeys)
{
    auto& script = AddConfigured();
    Tick();
    EXPECT_EQ(script.starts, 1);
    EXPECT_EQ(script.updates, 1);
    EXPECT_EQ(script.observations, 0);
    const auto document = toml::parse(scene::SceneSerializer::SaveToText(m_scene));
    ASSERT_TRUE(document);
    const auto* objects = document["gameobjects"].as_array();
    ASSERT_NE(objects, nullptr);
    ASSERT_EQ(objects->size(), 1u);
    const auto* scripts = (*objects)[0].as_table()->get_as<toml::array>("ScriptComponents");
    ASSERT_NE(scripts, nullptr);
    ASSERT_EQ(scripts->size(), 2u);
    const auto* values = (*scripts)[1].as_table()->get_as<toml::table>("fields");
    ASSERT_NE(values, nullptr);
    EXPECT_EQ((*values)["tuning"].value_or(std::string{}), m_assetPath);
    EXPECT_EQ((*values)["target"].value_or(std::string{}), script.scene.Self()->instanceId);
    EXPECT_FALSE(values->contains("observed"));
    EXPECT_EQ(script.observations, 0);
}

TEST_F(ScriptRequirementsTest, MissingFileMalformedAssetAndWrongTypeAreDiagnosedBeforePlay)
{
    auto& script = AddConfigured();
    script.tuning.ref.path = m_temp.File("absent.fzdata").generic_string();
    auto issues = scene::ValidateSceneScriptRequirements(m_scene);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].reason.find("cannot be found"), std::string::npos);
    script.tuning.ref.path = m_temp.File("broken.fzdata").generic_string();
    std::ofstream(script.tuning.ref.path) << "[broken";
    issues = scene::ValidateSceneScriptRequirements(m_scene);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].reason.find("cannot be loaded"), std::string::npos);
    script.tuning.ref.path = m_assetPath;
    script.tuning.ref.type = "OtherTuning";
    issues = scene::ValidateSceneScriptRequirements(m_scene);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].reason.find("expects 'OtherTuning'"), std::string::npos);
}

TEST_F(ScriptRequirementsTest, ReferencesRejectWrongTypeAmbiguityAndDeletedTarget)
{
    auto& script = AddConfigured();
    auto& target = m_scene.CreateGameObject("Target");
    const auto id = target.GetID();
    script.target.ref.id = id;
    EXPECT_EQ(scene::ValidateSceneScriptRequirements(m_scene).size(), 1u);
    target.AddScript<RequiredPeer>();
    EXPECT_TRUE(scene::ValidateSceneScriptRequirements(m_scene).empty());
    target.AddScript<RequiredPeer>();
    EXPECT_EQ(scene::ValidateSceneScriptRequirements(m_scene).size(), 1u);
    ASSERT_TRUE(m_scene.DestroyGameObject(id));
    auto issues = scene::ValidateSceneScriptRequirements(m_scene);
    ASSERT_EQ(issues.size(), 1u);
    EXPECT_NE(issues[0].reason.find("no longer exists"), std::string::npos);
}

TEST_F(ScriptRequirementsTest, InspectorValidationDoesNotLoadAssetsOrMutateReferences)
{
    auto& script = AddConfigured();
    auto& go = *m_scene.GameObjects().begin();
    const auto id = script.target.ref.id;
    std::vector<scene::ScriptRequirementIssue> issues;
    scene::CollectScriptRequirementIssues(go, script, issues);
    EXPECT_TRUE(issues.empty());
    EXPECT_TRUE(asset::DataAssetRegistry::TypeOf(m_assetPath).empty());
    EXPECT_EQ(script.target.ref.id, id);
    EXPECT_EQ(script.observations, 0);
}

TEST_F(ScriptRequirementsTest, AwakeCanSupplyRequiredAttachmentBeforeStartValidation)
{
    auto& go = m_scene.CreateGameObject("AwakeSupplied");
    auto& script = go.AddScript<RequiredSettings>();
    script.tuning.ref.path = m_assetPath;
    script.target.ref.id = go.GetID();
    go.AddScript<SuppliesPeer>();
    Tick();
    EXPECT_EQ(script.starts, 1);
    EXPECT_FALSE(script.RequirementsBlocked());
}

TEST_F(ScriptRequirementsTest, InternalModulesUseTheSameBlockingValidation)
{
    auto& go = m_scene.CreateGameObject("Owner");
    scene::Script owner;
    owner.SetContext(&m_scene, &go);
    RequiredSettings module;
    scene::ScriptModules modules{owner, {scene::ScriptModule{module}.Update()}};
    modules.Start();
    module.enabled = true;
    modules.Update(0.016f);
    EXPECT_TRUE(module.RequirementsBlocked());
    EXPECT_EQ(module.starts, 0);
    EXPECT_EQ(module.updates, 0);
    modules.Destroy();
}
} /// @note namespace fbzz::tests
