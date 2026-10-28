/// @file    DeveloperModeTests.cpp
/// @brief   開発者モードが «起動で強制 || 設定» で決まり、起動引数を引用符の規則どおりに読むことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/developer-mode.md
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/DeveloperMode.hpp>
#include "Core/DeveloperModeLaunch.hpp"
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace fbzz::tests {

class DeveloperModeTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        m_previousForced = core::DeveloperMode::IsForcedByLaunch();
        m_previousPreference = core::DeveloperMode::Preference();
        SetLastError(ERROR_SUCCESS);
        const DWORD capacity = GetEnvironmentVariableW(ENVIRONMENT_NAME, nullptr, 0);
        if (capacity == 0) {
            const DWORD error = GetLastError();
            ASSERT_TRUE(error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND) << error;
            m_hadEnvironment = error != ERROR_ENVVAR_NOT_FOUND;
        } else {
            m_hadEnvironment = true;
            m_previousEnvironment.resize(capacity);
            SetLastError(ERROR_SUCCESS);
            const DWORD length = GetEnvironmentVariableW(ENVIRONMENT_NAME, m_previousEnvironment.data(), capacity);
            ASSERT_LT(length, capacity);
            ASSERT_TRUE(length != 0 || GetLastError() == ERROR_SUCCESS);
            m_previousEnvironment.resize(length);
        }

        /// @note 環境変数だけの効果を検証するため、テスト exe に --developer を付けない。
        ASSERT_FALSE(core::detail::HasDeveloperLaunchFlag(GetCommandLineW()));
        ASSERT_TRUE(SetEnvironmentVariableW(ENVIRONMENT_NAME, nullptr));
        m_changedState = true;
        core::DeveloperMode::InitFromCommandLine();
        core::DeveloperMode::SetPreference(false);
    }

    void TearDown() override
    {
        if (m_changedState) {
            /// @note 起動時の判定は snapshot なので、通常の初期化で戻してから元の環境変数を復元する。
            EXPECT_TRUE(SetEnvironmentVariableW(ENVIRONMENT_NAME, m_previousForced ? L"1" : nullptr));
            core::DeveloperMode::InitFromCommandLine();
            core::DeveloperMode::SetPreference(m_previousPreference);
            EXPECT_EQ(core::DeveloperMode::IsForcedByLaunch(), m_previousForced);
            EXPECT_TRUE(SetEnvironmentVariableW(ENVIRONMENT_NAME, m_hadEnvironment ? m_previousEnvironment.c_str() : nullptr));
        }
        EngineFixture::TearDown();
    }

    /// @see https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-getenvironmentvariablew GetEnvironmentVariableW distinguishes a missing variable through GetLastError
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/processenv/nf-processenv-setenvironmentvariablew SetEnvironmentVariableW changes only the current process
    static constexpr const wchar_t* ENVIRONMENT_NAME = L"FBZZ_DEVELOPER_MODE";
    std::wstring m_previousEnvironment;
    bool m_hadEnvironment = false;
    bool m_previousForced = false;
    bool m_previousPreference = false;
    bool m_changedState = false;
};

class DeveloperModeLaunchTest : public testkit::Fixture {};

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
    ASSERT_TRUE(SetEnvironmentVariableW(ENVIRONMENT_NAME, L"1"));
    core::DeveloperMode::InitFromCommandLine();

    core::DeveloperMode::SetPreference(false);

    EXPECT_TRUE(core::DeveloperMode::IsEnabled());
    EXPECT_TRUE(core::DeveloperMode::IsForcedByLaunch());
    EXPECT_FALSE(core::DeveloperMode::Preference());
}

TEST_F(DeveloperModeLaunchTest, FindsTheLaunchFlagAsASeparateArgument)
{
    EXPECT_TRUE(core::detail::HasDeveloperLaunchFlag(L"FBZZEditor.exe --project C:\\p --developer"));
    EXPECT_TRUE(core::detail::HasDeveloperLaunchFlag(L"\"C:\\Program Files\\FBZZEditor.exe\" --developer"));
}

TEST_F(DeveloperModeLaunchTest, IgnoresTheFlagInsideAQuotedPathOrAsTheExecutable)
{
    EXPECT_FALSE(core::detail::HasDeveloperLaunchFlag(L"FBZZEditor.exe --project \"C:\\a --developer\""));
    EXPECT_FALSE(core::detail::HasDeveloperLaunchFlag(L"--developer --project C:\\p"));
    EXPECT_FALSE(core::detail::HasDeveloperLaunchFlag(L"FBZZEditor.exe --developer-extra"));
}

TEST_F(DeveloperModeTest, EnvironmentVariableForcesItAtStartup)
{
    ASSERT_TRUE(SetEnvironmentVariableW(ENVIRONMENT_NAME, L"1"));

    core::DeveloperMode::InitFromCommandLine();

    EXPECT_TRUE(core::DeveloperMode::IsForcedByLaunch());
    EXPECT_TRUE(core::DeveloperMode::IsEnabled());
    EXPECT_FALSE(core::DeveloperMode::Preference());
}

TEST_F(DeveloperModeTest, EnvironmentVariableIsIgnoredUnlessItIsExactlyOne)
{
    /// @note "0" も "11" も無効。うっかり «空でなければ有効» にすると配布物で開いてしまう。
    ASSERT_TRUE(SetEnvironmentVariableW(ENVIRONMENT_NAME, L"0"));
    core::DeveloperMode::InitFromCommandLine();
    EXPECT_FALSE(core::DeveloperMode::IsForcedByLaunch());

    ASSERT_TRUE(SetEnvironmentVariableW(ENVIRONMENT_NAME, L"11"));
    core::DeveloperMode::InitFromCommandLine();
    EXPECT_FALSE(core::DeveloperMode::IsForcedByLaunch());
}

} /// @note namespace fbzz::tests
