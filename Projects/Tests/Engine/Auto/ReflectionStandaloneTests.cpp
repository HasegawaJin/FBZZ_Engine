/// @file    ReflectionStandaloneTests.cpp
/// @brief   Script の完全型なしで共通反射とフィールド宣言を利用できることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-31
#include <Engine/Scene/Reflection.hpp>
#include <Engine/Scene/Components/CharacterControllerComponent.hpp>
#include <type_traits>
namespace fbzz::tests::reflection_standalone_detail {
template<class T, class = void> struct IsComplete : std::false_type {};
template<class T> struct IsComplete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};
static_assert(!IsComplete<scene::Script>::value);
static_assert(std::string_view(scene::RefTypeNameOf<scene::GameObject>("GameObject")).empty());
static_assert(std::string_view(scene::RefTypeNameOf<scene::CharacterControllerComponent>("CharacterControllerComponent"))
    == "CharacterControllerComponent");
}
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

namespace fbzz::tests {
namespace {
struct ReflectedValue final : scene::IScriptSerializable {
    FBZZ_SERIALIZABLE(ReflectedValue)
    FBZZ_FIELD(int, count, 7, "Count")
    FBZZ_FIELD(bool, enabled, true, "Enabled")
};
FBZZ_REFLECT(ReflectedValue)
class Recorder final : public scene::IReflector {
public:
    std::vector<std::string> keys;
    std::vector<std::string> labels;
    void Field(const char* name, int& value) override
    {
        keys.push_back(PersistentKey(name)); labels.push_back(DisplayName(name)); value = 11;
    }
    void Field(const char* name, bool& value) override
    {
        keys.push_back(PersistentKey(name)); labels.push_back(DisplayName(name)); value = false;
    }
    void Field(const char*, float&) override {}
    void Field(const char*, math::Vector2&) override {}
    void Field(const char*, math::Vector3&) override {}
    void Field(const char*, math::Vector4&) override {}
    void Field(const char*, math::Quaternion&) override {}
    void Field(const char*, std::string&) override {}
};
}
class ReflectionStandaloneTest : public testkit::EngineFixture {};
TEST_F(ReflectionStandaloneTest, ReflectsDeclaredFieldsInOrderWithoutAScriptRuntime)
{
    ReflectedValue value;
    Recorder recorder;

    value.Reflect(recorder);

    EXPECT_EQ(recorder.keys, (std::vector<std::string>{"count", "enabled"}));
    EXPECT_EQ(recorder.labels, (std::vector<std::string>{"Count", "Enabled"}));
    EXPECT_EQ(value.count, 11);
    EXPECT_FALSE(value.enabled);
}
}
