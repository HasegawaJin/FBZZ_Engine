/// @file    DeveloperModeTests.cpp
/// @brief   開発者モードが «起動で強制 || 設定» で決まり、起動引数を引用符の規則どおりに読むことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/developer-mode.md
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/DeveloperMode.hpp>

namespace fbzz::tests {

class DeveloperModeTest : public testkit::EngineFixture {
protected:
    void TearDown() override
    {
        /// @note プロセスに 1 つの状態。次のテストへ持ち越さない。
        core::DeveloperMode::SetForcedByLaunch(false);
        core::DeveloperMode::SetPreference(false);
        EngineFixture::TearDown();
    }
};

TEST_F(DeveloperModeTest, IsDisabledByDefault)
{
    EXPECT_FALSE(core::DeveloperMode::IsEnabled());
}

TEST_F(DeveloperModeTest, PreferenceEnablesIt)
{
    core::DeveloperMode::SetPreference(true);

    EXPECT_TRUE(core::DeveloperMode::IsEnabled());
    EXPECT_FALSE(core::DeveloperMode::IsForcedByLaunch());
}

TEST_F(DeveloperModeTest, LaunchForceKeepsItEnabledWithoutChangingThePreference)
{
    core::DeveloperMode::SetForcedByLaunch(true);

    core::DeveloperMode::SetPreference(false);

    EXPECT_TRUE(core::DeveloperMode::IsEnabled());
    EXPECT_FALSE(core::DeveloperMode::Preference());
}

TEST_F(DeveloperModeTest, FindsTheLaunchFlagAsASeparateArgument)
{
    EXPECT_TRUE(core::DeveloperMode::HasLaunchFlag(L"FBZZEditor.exe --project C:\\p --developer"));
    EXPECT_TRUE(core::DeveloperMode::HasLaunchFlag(L"\"C:\\Program Files\\FBZZEditor.exe\" --developer"));
}

TEST_F(DeveloperModeTest, IgnoresTheFlagInsideAQuotedPathOrAsTheExecutable)
{
    EXPECT_FALSE(core::DeveloperMode::HasLaunchFlag(L"FBZZEditor.exe --project \"C:\\a --developer\""));
    EXPECT_FALSE(core::DeveloperMode::HasLaunchFlag(L"--developer --project C:\\p"));
    EXPECT_FALSE(core::DeveloperMode::HasLaunchFlag(L"FBZZEditor.exe --developer-extra"));
}

} // namespace fbzz::tests
