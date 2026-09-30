/// @file    ScriptObservationTests.cpp
/// @brief   GreenWare の実状態を更新待ちせず観測し、保存データと分離する契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Asset/DataAssetFactory.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Scene/TomlReflector.hpp>
#include <Scripts/Combat/Boss03BossComponent.hpp>
#include <Scripts/Combat/BossCoreComponent.hpp>
#include <Scripts/Combat/SerpentBossComponent.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <fstream>
#include <initializer_list>

namespace fbzz::tests {
namespace {

class ObservationSnapshot final : public util::TomlWriteReflector {
public:
    explicit ObservationSnapshot(toml::table& table) : TomlWriteReflector(table) {}
    bool BeginObservation(const char*, const char*) override { return true; }
};

toml::table Observe(scene::Script& script)
{
    toml::table values;
    ObservationSnapshot snapshot(values);
    script.Reflect(snapshot);
    return values;
}

template<class T>
void CheckUnstartedAndDisabled(std::initializer_list<const char*> keys)
{
    T script;
    script.enabled = false;
    const auto observed = Observe(script);
    toml::table saved;
    util::TomlWriteReflector writer(saved);
    script.Reflect(writer);
    for (const char* key : keys) {
        EXPECT_TRUE(observed.contains(key)) << key;
        EXPECT_FALSE(saved.contains(key)) << key;
    }
}

} /// @note namespace

class GreenWareScriptObservationTest : public testkit::EngineFixture {
protected:
    void TearDown() override
    {
        asset::DataAssetRegistry::ClearCache();
        EngineFixture::TearDown();
    }
};

TEST_F(GreenWareScriptObservationTest, AllMigratedFieldsAreReadableBeforeStartAndExcludedFromSave)
{
    CheckUnstartedAndDisabled<sandbox::EnemyHealthComponent>({"debugHealth"});
    CheckUnstartedAndDisabled<sandbox::PlayerHealthComponent>({"debugHealth", "debugFlashParts"});
    CheckUnstartedAndDisabled<sandbox::PlayerBreathComponent>({"debugBreath", "debugExhausted"});
    CheckUnstartedAndDisabled<sandbox::CombatManagerComponent>({
        "debugKills", "debugPushKills", "debugDamageToEnemies", "debugDamageToPlayer", "debugChain",
        "debugChainHits", "debugBestChain", "debugPerfectDodges", "debugCadence", "debugFaces"});
    CheckUnstartedAndDisabled<sandbox::BossCoreComponent>({"debugPhase"});
    CheckUnstartedAndDisabled<sandbox::SerpentBossComponent>({"debugSegments", "debugPhase", "debugEngaged"});
    CheckUnstartedAndDisabled<sandbox::Boss03BossComponent>({"debugWings", "debugPhase", "debugEngaged"});
}

TEST_F(GreenWareScriptObservationTest, EnemyDamageAndResetAreVisibleImmediately)
{
    scene::Scene scene;
    auto& object = scene.CreateGameObject("ObservationEnemy");
    auto& enemy = object.AddScript<sandbox::EnemyHealthComponent>();
    enemy.SetContext(&scene, &object);
    enemy.maxHealth = 10;
    enemy.ResetHealth();
    EXPECT_EQ(Observe(enemy)["debugHealth"].value_or(-1), 10);
    ASSERT_TRUE(enemy.ApplyDamage(3));
    EXPECT_EQ(Observe(enemy)["debugHealth"].value_or(-1), 7);
    EXPECT_FALSE(enemy.ApplyDamage(0));
    EXPECT_EQ(Observe(enemy)["debugHealth"].value_or(-1), 7);
    enemy.ResetHealth();
    EXPECT_EQ(Observe(enemy)["debugHealth"].value_or(-1), 10);
}

TEST_F(GreenWareScriptObservationTest, CombatCountersAndResetDoNotNeedAnUpdate)
{
    scene::Scene scene;
    auto& object = scene.CreateGameObject("ObservationCombat");
    auto& combat = object.AddScript<sandbox::CombatManagerComponent>();
    combat.SetContext(&scene, &object);
    combat.AddPushKill();
    combat.AddPushKill();
    combat.AddPerfectDodge();
    combat.ReportCadence(3, true);
    auto observed = Observe(combat);
    EXPECT_EQ(observed["debugPushKills"].value_or(-1), 2);
    EXPECT_EQ(observed["debugPerfectDodges"].value_or(-1), 1);
    EXPECT_EQ(observed["debugCadence"].value_or(-1), 3);
    combat.OnStart();
    observed = Observe(combat);
    EXPECT_EQ(observed["debugPushKills"].value_or(-1), 0);
    EXPECT_EQ(observed["debugPerfectDodges"].value_or(-1), 0);
    EXPECT_EQ(observed["debugCadence"].value_or(-1), 0);
    combat.OnDestroy();
}

TEST_F(GreenWareScriptObservationTest, BreathSpendExhaustionAndRefillAreVisibleWithoutAnUpdate)
{
    testkit::TempDir directory("greenware-observations");
    ASSERT_TRUE(directory.IsValid());
    asset::DataAssetFactory::Register<sandbox::PlayerTuning>();
    const auto file = directory.File("PlayerTuning.fzdata");
    {
        std::ofstream output(file);
        ASSERT_TRUE(output.is_open());
        output << "type = 'PlayerTuning'\nbreathMax = 100.0\nbreathDodgeCost = 30.0\nbreathGuardDrain = 100.0\n";
    }
    sandbox::PlayerBreathComponent breath;
    breath.tuning.ref.path = file.generic_string();
    ASSERT_TRUE(breath.tuning);
    breath.ResetBreath();
    EXPECT_NEAR(Observe(breath)["debugBreath"].value_or(-1.0), 100.0, 0.001);
    ASSERT_TRUE(breath.SpendDodge());
    EXPECT_NEAR(Observe(breath)["debugBreath"].value_or(-1.0), 70.0, 0.001);
    ASSERT_TRUE(breath.DrainGuard(1.0f));
    EXPECT_TRUE(Observe(breath)["debugExhausted"].value_or(false));
    EXPECT_NEAR(Observe(breath)["debugBreath"].value_or(-1.0), 0.0, 0.001);
    breath.enabled = false;
    breath.Refill();
    EXPECT_FALSE(Observe(breath)["debugExhausted"].value_or(true));
    EXPECT_NEAR(Observe(breath)["debugBreath"].value_or(-1.0), 100.0, 0.001);
}

TEST_F(GreenWareScriptObservationTest, MissingBossDependenciesHaveSafeValues)
{
    scene::Scene scene;
    auto& serpentObject = scene.CreateGameObject("Serpent");
    auto& serpent = serpentObject.AddScript<sandbox::SerpentBossComponent>();
    serpent.SetContext(&scene, &serpentObject);
    auto& wingedObject = scene.CreateGameObject("Winged");
    auto& winged = wingedObject.AddScript<sandbox::Boss03BossComponent>();
    winged.SetContext(&scene, &wingedObject);
    EXPECT_EQ(Observe(serpent)["debugSegments"].value_or(-1), 0);
    EXPECT_EQ(Observe(winged)["debugWings"].value_or(-1), 0);
    for (scene::Script* boss : {static_cast<scene::Script*>(&serpent), static_cast<scene::Script*>(&winged)}) {
        EXPECT_EQ(Observe(*boss)["debugPhase"].value_or(-1), 1);
        EXPECT_FALSE(Observe(*boss)["debugEngaged"].value_or(true));
    }
}

TEST_F(GreenWareScriptObservationTest, LegacySavedDebugValuesCannotOverwriteLiveState)
{
    sandbox::EnemyHealthComponent enemy;
    enemy.maxHealth = 7;
    enemy.ResetHealth();
    toml::table legacy;
    legacy.insert("maxHealth", 12);
    legacy.insert("debugHealth", 999);
    util::TomlReadReflector reader(legacy);
    enemy.Reflect(reader);
    EXPECT_EQ(enemy.maxHealth, 12);
    EXPECT_EQ(enemy.CurrentHealth(), 7);
    EXPECT_EQ(Observe(enemy)["debugHealth"].value_or(-1), 7);
    toml::table saved;
    util::TomlWriteReflector writer(saved);
    enemy.Reflect(writer);
    EXPECT_EQ(saved["maxHealth"].value_or(-1), 12);
    EXPECT_FALSE(saved.contains("debugHealth"));
}

TEST_F(GreenWareScriptObservationTest, PlayerModulesKeepSettingsAndExcludeMigratedObservations)
{
    sandbox::PlayerComponent player;
    player.terrainRecovery = true;
    toml::table saved;
    util::TomlWriteReflector writer(saved);
    player.Reflect(writer);
    EXPECT_TRUE(saved["terrainRecovery"].value_or(false));
    EXPECT_TRUE(saved.contains("useCameraForward"));
    const auto observed = Observe(player);
    for (const char* key : {"debugHealth", "debugFlashParts", "debugBreath", "debugExhausted"}) {
        EXPECT_FALSE(saved.contains(key)) << key;
        EXPECT_TRUE(observed.contains(key)) << key;
    }
}

} /// @note namespace fbzz::tests
