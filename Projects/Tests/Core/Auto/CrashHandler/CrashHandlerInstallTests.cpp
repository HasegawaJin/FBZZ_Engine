/// @file    CrashHandlerInstallTests.cpp
/// @brief   Install が受け口を 1 度だけ差し替え、置き場所と名前だけ後から差し替わること、置けない根を断ることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
/// @see Docs/design/crash-report.md
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/CrashHandler.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Engine/Core/Logger.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fbzz::tests {
namespace {

std::string ReadAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

} // namespace

class CrashHandlerInstallTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        /// @note ログの末尾が Install を跨いで残ることを見るので INFO を通す。TearDown で既定へ戻る。
        SetLogLevel(core::LogLevel::INFO);
        ASSERT_TRUE(first.IsValid());
        ASSERT_TRUE(second.IsValid());
    }

    void TearDown() override
    {
        /// @note 例外フィルターと開発者モードはプロセス全体の状態。次のテストへ持ち越さない。
        core::CrashHandler::Uninstall();
        core::DeveloperMode::SetPreference(false);
        EngineFixture::TearDown();
    }

    testkit::TempDir first{ "CrashInstallFirst" };
    testkit::TempDir second{ "CrashInstallSecond" };
};

TEST_F(CrashHandlerInstallTest, IsInstalledFollowsInstallAndUninstall)
{
    EXPECT_FALSE(core::CrashHandler::IsInstalled());

    ASSERT_TRUE(core::CrashHandler::Install(first.Path(), "InstallTest"));
    EXPECT_TRUE(core::CrashHandler::IsInstalled());

    core::CrashHandler::Uninstall();
    EXPECT_FALSE(core::CrashHandler::IsInstalled());
}

TEST_F(CrashHandlerInstallTest, RefusesARootWhosePathDoesNotFitTheWriterBuffer)
{
    /// @note 書き出しはヒープを使えないので固定長で持つ。入らない根は Install の時点で断る。
    const std::filesystem::path tooLong(std::wstring(980, L'a'));

    EXPECT_FALSE(core::CrashHandler::Install(tooLong, "InstallTest"));
    EXPECT_FALSE(core::CrashHandler::IsInstalled());
}

TEST_F(CrashHandlerInstallTest, SecondInstallMovesTheReportsWithoutLosingTheLogTail)
{
    ASSERT_TRUE(core::CrashHandler::Install(first.Path(), "FirstApp"));
    core::Logger::Info("logged-before-the-second-install");

    ASSERT_TRUE(core::CrashHandler::Install(second.Path(), "SecondApp"));
    const std::filesystem::path dir = core::CrashHandler::WriteReport("after moving the root");

    ASSERT_FALSE(dir.empty());
    EXPECT_EQ(dir.parent_path(), second.Path() / "Saved" / "Crashes");
    EXPECT_FALSE(std::filesystem::exists(first.Path() / "Saved" / "Crashes"));
    const std::string report = ReadAll(dir / "report.txt");
    EXPECT_NE(report.find("app:     SecondApp"), std::string::npos);
    /// @note 受け口は差し替えないので、輪も空にならない。
    EXPECT_NE(report.find("logged-before-the-second-install"), std::string::npos);
}

TEST_F(CrashHandlerInstallTest, TriggerRefusesBeforeInstallEvenInDeveloperMode)
{
    core::DeveloperMode::SetPreference(true);

    EXPECT_FALSE(core::CrashHandler::Trigger(core::CrashTrigger::Report));
}

} // namespace fbzz::tests
