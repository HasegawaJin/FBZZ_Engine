/// @file    PlayerScriptModulesTests.cpp
/// @brief   親と内部モジュールの設定が同じ反射経路を通ることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-28
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Scripts/Player/PlayerComponent.hpp>
#include <map>
#include <string>

namespace fbzz::tests {
namespace {

class BooleanSnapshot final : public scene::IReflector {
public:
    using IReflector::Field;
    std::map<std::string, bool> values;
    bool reading = false;

    void Field(const char* name, bool& value) override
    {
        const std::string key = PersistentKey(name);
        if (reading) {
            if (auto it = values.find(key); it != values.end()) value = it->second;
        } else values[key] = value;
    }
    void Field(const char*, float&) override {}
    void Field(const char*, int&) override {}
    void Field(const char*, std::string&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, math::Quaternion&) override {}
};

} /// @note namespace

class PlayerScriptModulesTest : public testkit::EngineFixture {};

TEST_F(PlayerScriptModulesTest, ParentAndChildSettingsSurviveReflectionRoundTrip)
{
    sandbox::PlayerComponent player;
    player.terrainRecovery = true;
    BooleanSnapshot snapshot;
    player.Reflect(snapshot);
    ASSERT_TRUE(snapshot.values.contains("terrainRecovery"));
    EXPECT_TRUE(snapshot.values.at("terrainRecovery"));
    ASSERT_TRUE(snapshot.values.contains("useCameraForward"));
    snapshot.values["useCameraForward"] = false;

    sandbox::PlayerComponent restored;
    snapshot.reading = true;
    restored.Reflect(snapshot);
    EXPECT_TRUE(restored.terrainRecovery);
    BooleanSnapshot savedAgain;
    restored.Reflect(savedAgain);
    EXPECT_FALSE(savedAgain.values.at("useCameraForward"));
}

} /// @note namespace fbzz::tests
