/// @file    InputActionMapTests.cpp
/// @brief   キーコンフィグ (.inputactions) の保存・読み込みと、壊れた設定への耐性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// 設定ファイルが壊れていたときに «全操作不能» にしないこと、が一番大事な契約。
/// 読み込み失敗で空にすると、ゲームは起動するのに何も動かせない状態になり、
/// プレイヤーからは «バグで固まった» としか見えない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Input/InputActionMap.hpp>
#include <Engine/Input/InputBinding.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace fbzz::tests {
namespace {

bool HasAction(std::string_view name)
{
    const auto& actions = input::InputActionMap::GetActions();
    return std::any_of(actions.begin(), actions.end(),
                       [&](const input::InputAction& a) { return a.name == name; });
}

bool HasAxis(std::string_view name)
{
    const auto& axes = input::InputActionMap::GetAxes();
    return std::any_of(axes.begin(), axes.end(),
                       [&](const input::InputAxis& a) { return a.name == name; });
}

} // namespace

/// マップは静的な状態なので、テストごとに作り直して後始末する。
class InputActionMapTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        input::InputActionMap::Clear();
    }

    void TearDown() override
    {
        // 静的な状態なので、有効フラグまで含めて必ず戻す。
        input::InputActionMap::SetEnabled(true);
        input::InputActionMap::Clear();
        testkit::EngineFixture::TearDown();
    }

    std::string File(const std::string& name) const
    {
        return m_temp.File(name).generic_string();
    }

    void WriteText(const std::string& name, const std::string& text) const
    {
        std::ofstream out(m_temp.File(name), std::ios::binary);
        out << text;
    }

private:
    testkit::TempDir m_temp{"inputmap"};
};

// --- 既定のバインド ---------------------------------------------------------

TEST_F(InputActionMapTest, DefaultsProvideSomethingToPlayWith)
{
    // 設定ファイルが無い状態でも遊べること。空のまま起動すると何も動かせない。
    input::InputActionMap::LoadDefaults();

    EXPECT_FALSE(input::InputActionMap::GetActions().empty());
    EXPECT_FALSE(input::InputActionMap::GetAxes().empty());
}

TEST_F(InputActionMapTest, InitializePreservesProjectMenuBindings)
{
    WriteText("menu.inputactions",
              "[[action]]\nname = 'Submit'\n"
              "[[action]]\nname = 'Cancel'\n");
    ASSERT_TRUE(input::InputActionMap::LoadFromFile(File("menu.inputactions")));

    input::InputActionMap::Initialize();
    input::InputActionMap::Initialize();

    EXPECT_TRUE(HasAction("Submit"));
    EXPECT_TRUE(HasAction("Cancel"));
    EXPECT_EQ(input::InputActionMap::GetActions().size(), 2u);
    EXPECT_TRUE(input::InputActionMap::GetAxes().empty());
}

TEST_F(InputActionMapTest, InitializePreservesExplicitEmptyConfiguration)
{
    WriteText("empty.inputactions", "");
    ASSERT_TRUE(input::InputActionMap::LoadFromFile(File("empty.inputactions")));

    input::InputActionMap::Initialize();

    EXPECT_TRUE(input::InputActionMap::GetActions().empty());
    EXPECT_TRUE(input::InputActionMap::GetAxes().empty());
}

TEST_F(InputActionMapTest, ClearRemovesEverything)
{
    input::InputActionMap::LoadDefaults();

    input::InputActionMap::Clear();

    EXPECT_TRUE(input::InputActionMap::GetActions().empty());
    EXPECT_TRUE(input::InputActionMap::GetAxes().empty());
}

// --- 往復 -------------------------------------------------------------------

TEST_F(InputActionMapTest, SavesAndReloadsTheDefaultBindings)
{
    input::InputActionMap::LoadDefaults();
    const std::size_t actionCount = input::InputActionMap::GetActions().size();
    const std::size_t axisCount   = input::InputActionMap::GetAxes().size();
    ASSERT_TRUE(input::InputActionMap::SaveToFile(File("keys.inputactions")));

    input::InputActionMap::Clear();
    ASSERT_TRUE(input::InputActionMap::LoadFromFile(File("keys.inputactions")));

    EXPECT_EQ(input::InputActionMap::GetActions().size(), actionCount);
    EXPECT_EQ(input::InputActionMap::GetAxes().size(), axisCount);
}

TEST_F(InputActionMapTest, KeepsActionNamesThroughARoundTrip)
{
    input::InputActionMap::LoadDefaults();
    const std::string firstAction = input::InputActionMap::GetActions().front().name;
    const std::string firstAxis   = input::InputActionMap::GetAxes().front().name;
    ASSERT_TRUE(input::InputActionMap::SaveToFile(File("keys.inputactions")));

    input::InputActionMap::Clear();
    ASSERT_TRUE(input::InputActionMap::LoadFromFile(File("keys.inputactions")));

    EXPECT_TRUE(HasAction(firstAction));
    EXPECT_TRUE(HasAxis(firstAxis));
}

TEST_F(InputActionMapTest, KeepsTheBindingsOfEachAction)
{
    // 名前だけ残ってバインドが空だと、«設定はあるのに反応しない» になる。
    input::InputActionMap::LoadDefaults();
    const std::size_t bindings = input::InputActionMap::GetActions().front().bindings.size();
    ASSERT_GT(bindings, 0u);
    ASSERT_TRUE(input::InputActionMap::SaveToFile(File("keys.inputactions")));

    input::InputActionMap::Clear();
    ASSERT_TRUE(input::InputActionMap::LoadFromFile(File("keys.inputactions")));

    EXPECT_EQ(input::InputActionMap::GetActions().front().bindings.size(), bindings);
}

TEST_F(InputActionMapTest, KeepsTheAxisTuningThroughARoundTrip)
{
    // デッドゾーンや戻り速度が落ちると «パッドの効きが変わった» と感じる。
    input::InputActionMap::LoadDefaults();
    const input::InputAxis original = input::InputActionMap::GetAxes().front();
    ASSERT_TRUE(input::InputActionMap::SaveToFile(File("keys.inputactions")));

    input::InputActionMap::Clear();
    ASSERT_TRUE(input::InputActionMap::LoadFromFile(File("keys.inputactions")));

    const input::InputAxis& restored = input::InputActionMap::GetAxes().front();
    EXPECT_NEAR(restored.deadZone, original.deadZone, testkit::kTolerance);
    EXPECT_NEAR(restored.gravity, original.gravity, testkit::kTolerance);
}

TEST_F(InputActionMapTest, SavingTwiceProducesTheSameFile)
{
    // 触っていないのに設定ファイルの差分が出ると、コミットのたびに紛れ込む。
    input::InputActionMap::LoadDefaults();
    ASSERT_TRUE(input::InputActionMap::SaveToFile(File("a.inputactions")));
    ASSERT_TRUE(input::InputActionMap::SaveToFile(File("b.inputactions")));

    const auto read = [](const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    EXPECT_EQ(read(File("a.inputactions")), read(File("b.inputactions")));
}

// --- 壊れた設定 -------------------------------------------------------------

TEST_F(InputActionMapTest, KeepsTheCurrentBindingsWhenTheFileIsMissing)
{
    input::InputActionMap::LoadDefaults();
    const std::size_t before = input::InputActionMap::GetActions().size();

    EXPECT_FALSE(input::InputActionMap::LoadFromFile(File("does_not_exist.inputactions")));

    EXPECT_EQ(input::InputActionMap::GetActions().size(), before);
}

TEST_F(InputActionMapTest, KeepsTheCurrentBindingsWhenTheFileIsBroken)
{
    // 壊れた設定で «全操作不能» にしない。直前のバインドで動き続ける。
    input::InputActionMap::LoadDefaults();
    const std::size_t before = input::InputActionMap::GetActions().size();
    WriteText("broken.inputactions", "{{{ this is not toml ]]]");

    EXPECT_FALSE(input::InputActionMap::LoadFromFile(File("broken.inputactions")));

    EXPECT_EQ(input::InputActionMap::GetActions().size(), before);
}

// --- 有効・無効 -------------------------------------------------------------

TEST_F(InputActionMapTest, DisablingSilencesEveryAction)
{
    // 編集中に W を押してプレイヤーが歩き出す事故を防ぐための口。
    input::InputActionMap::LoadDefaults();
    const std::string action = input::InputActionMap::GetActions().front().name;

    input::InputActionMap::SetEnabled(false);

    EXPECT_FALSE(input::InputActionMap::IsEnabled());
    EXPECT_FALSE(input::InputActionMap::GetAction(action));
    EXPECT_NEAR(input::InputActionMap::GetAxis(
                    input::InputActionMap::GetAxes().front().name),
                0.0f, testkit::kTolerance);

    input::InputActionMap::SetEnabled(true);
}

TEST_F(InputActionMapTest, UnknownNamesAreSilentRatherThanFatal)
{
    // スクリプトの綴り間違いは普通に起きる。落とさず «押されていない» を返す。
    input::InputActionMap::LoadDefaults();

    EXPECT_FALSE(input::InputActionMap::GetAction("NoSuchAction"));
    EXPECT_FALSE(input::InputActionMap::GetActionDown("NoSuchAction"));
    EXPECT_NEAR(input::InputActionMap::GetAxis("NoSuchAxis"), 0.0f, testkit::kTolerance);
}

} // namespace fbzz::tests
