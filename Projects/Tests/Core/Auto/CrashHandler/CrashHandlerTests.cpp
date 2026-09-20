/// @file    CrashHandlerTests.cpp
/// @brief   クラッシュレポートがダンプとログの末尾を残し、未報告のものだけを新しい順に知らせることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
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

class CrashHandlerTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        /// @note ログの末尾が残ることを見るので INFO を通す。TearDown で既定へ戻る。
        SetLogLevel(core::LogLevel::INFO);
    }

    void TearDown() override
    {
        /// @note 例外フィルターと SIGABRT はプロセス全体の状態。次のテストへ持ち越さない。
        core::CrashHandler::Uninstall();
        core::DeveloperMode::SetPreference(false);
        EngineFixture::TearDown();
    }

    /// @brief `Saved/Crashes/<name>/report.txt` を置く。
    void PlaceReport(const std::string& name, const std::string& what, bool reported = false) const
    {
        const std::filesystem::path dir = CrashesDir() / name;
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "report.txt") << "FBZZ crash report\r\nwhat:    " << what << "\r\n";
        if (reported) std::ofstream(dir / "reported");
    }

    [[nodiscard]] std::filesystem::path CrashesDir() const { return temp.Path() / "Saved" / "Crashes"; }

    testkit::TempDir temp{ "CrashHandler" };
};

/// @name 書き出し

TEST_F(CrashHandlerTest, WriteReportLeavesDumpReasonAndLogTail)
{
    ASSERT_TRUE(temp.IsValid());
    ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashTest"));
    core::Logger::Info("marker-before-report %d", 42);

    const std::filesystem::path dir = core::CrashHandler::WriteReport("device lost in test");

    ASSERT_FALSE(dir.empty());
    EXPECT_EQ(dir.parent_path(), CrashesDir());
    EXPECT_GT(std::filesystem::file_size(dir / "crash.dmp"), 0u);
    const std::string report = ReadAll(dir / "report.txt");
    EXPECT_NE(report.find("app:     CrashTest"), std::string::npos);
    EXPECT_NE(report.find("what:    device lost in test"), std::string::npos);
    EXPECT_NE(report.find("marker-before-report 42"), std::string::npos);
}

TEST_F(CrashHandlerTest, WriteReportReturnsEmptyWhenNotInstalled)
{
    const std::filesystem::path dir = core::CrashHandler::WriteReport("not installed");

    EXPECT_TRUE(dir.empty());
    EXPECT_FALSE(std::filesystem::exists(CrashesDir()));
}

TEST_F(CrashHandlerTest, LogTailStopsAfterUninstall)
{
    ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashTest"));
    core::Logger::Info("logged-while-first-install");
    core::CrashHandler::Uninstall();
    ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashTest"));

    const std::filesystem::path dir = core::CrashHandler::WriteReport("second install");

    ASSERT_FALSE(dir.empty());
    EXPECT_EQ(ReadAll(dir / "report.txt").find("logged-while-first-install"), std::string::npos);
}

/// @name わざと落とす

TEST_F(CrashHandlerTest, TriggerRefusesOutsideDeveloperMode)
{
    ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashTest"));

    const bool triggered = core::CrashHandler::Trigger(core::CrashTrigger::AccessViolation);

    EXPECT_FALSE(triggered);
}

TEST_F(CrashHandlerTest, ReportTriggerWritesWithoutCrashingInDeveloperMode)
{
    ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashTest"));
    core::DeveloperMode::SetPreference(true);

    const bool triggered = core::CrashHandler::Trigger(core::CrashTrigger::Report);

    EXPECT_TRUE(triggered);
    EXPECT_EQ(core::CrashHandler::FindUnreported(temp.Path()).size(), 1u);
}

TEST_F(CrashHandlerTest, TriggerNamesRoundTripThroughParse)
{
    const std::vector<std::string>& names = core::CrashHandler::TriggerNames();
    for (size_t i = 0; i < names.size(); ++i) {
        core::CrashTrigger parsed = core::CrashTrigger::Report;
        ASSERT_TRUE(core::CrashHandler::ParseTrigger(names[i], parsed)) << names[i];
        EXPECT_EQ(static_cast<size_t>(parsed), i) << names[i];
    }
    core::CrashTrigger untouched = core::CrashTrigger::Abort;
    EXPECT_FALSE(core::CrashHandler::ParseTrigger("segfault", untouched));
    EXPECT_EQ(untouched, core::CrashTrigger::Abort);
}

/// @name 次回起動の通知

TEST_F(CrashHandlerTest, FindUnreportedListsNewestFirstAndSkipsReported)
{
    PlaceReport("2026-09-18_10-00-00_100", "older");
    PlaceReport("2026-09-19_10-00-00_200", "newer");
    PlaceReport("2026-09-19_11-00-00_300", "already shown", true);

    const std::vector<core::CrashRecord> records = core::CrashHandler::FindUnreported(temp.Path());

    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].summary, "newer");
    EXPECT_EQ(records[1].summary, "older");
}

TEST_F(CrashHandlerTest, MarkReportedHidesTheRecordFromTheNextSearch)
{
    PlaceReport("2026-09-19_10-00-00_200", "shown once");
    const std::vector<core::CrashRecord> before = core::CrashHandler::FindUnreported(temp.Path());
    ASSERT_EQ(before.size(), 1u);

    core::CrashHandler::MarkReported(before.front());

    EXPECT_TRUE(core::CrashHandler::FindUnreported(temp.Path()).empty());
}

TEST_F(CrashHandlerTest, NonInteractiveNotifyLeavesRecordsUnreported)
{
    PlaceReport("2026-09-19_10-00-00_200", "seen by batch");

    const size_t count = core::CrashHandler::NotifyUnreported(temp.Path(), "CrashTest", false);

    EXPECT_EQ(count, 1u);
    EXPECT_EQ(core::CrashHandler::FindUnreported(temp.Path()).size(), 1u);
}

TEST_F(CrashHandlerTest, FindUnreportedIsEmptyWithoutCrashDirectory)
{
    EXPECT_TRUE(core::CrashHandler::FindUnreported(temp.Path()).empty());
}

} // namespace fbzz::tests
