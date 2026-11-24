/// @file    CrashHandlerTests.cpp
/// @brief   クラッシュレポートがダンプとログの末尾を残し、未報告のものだけを新しい順に知らせることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/crash-report.md
#include <TestKit/TestKit.hpp>
#include <TestKit/Support/TempDir.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/Diagnostics/CrashHandler.hpp>
#include <Engine/Core/Platform/DeveloperMode.hpp>
#include <Engine/Core/Diagnostics/Logger.hpp>

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

/// @brief Write report leaves dump reason and log tail.
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

/// @brief Write report returns empty when not installed.
TEST_F(CrashHandlerTest, WriteReportReturnsEmptyWhenNotInstalled)
{
    const std::filesystem::path dir = core::CrashHandler::WriteReport("not installed");

    EXPECT_TRUE(dir.empty());
    EXPECT_FALSE(std::filesystem::exists(CrashesDir()));
}

/// @brief Log tail stops after uninstall.
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

/// @brief Trigger refuses outside developer mode.
TEST_F(CrashHandlerTest, TriggerRefusesOutsideDeveloperMode)
{
    ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashTest"));

    const bool triggered = core::CrashHandler::Trigger(core::CrashTrigger::AccessViolation);

    EXPECT_FALSE(triggered);
}

/// @brief Report trigger writes without crashing in developer mode.
TEST_F(CrashHandlerTest, ReportTriggerWritesWithoutCrashingInDeveloperMode)
{
    ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashTest"));
    core::DeveloperMode::SetPreference(true);

    const bool triggered = core::CrashHandler::Trigger(core::CrashTrigger::Report);

    EXPECT_TRUE(triggered);
    EXPECT_EQ(core::CrashHandler::FindUnreported(temp.Path()).size(), 1u);
}

/// @brief Trigger names round trip through parse.
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

/// @brief Checks find unreported lists newest first and skips reported.
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

/// @brief Mark reported hides the record from the next search.
TEST_F(CrashHandlerTest, MarkReportedHidesTheRecordFromTheNextSearch)
{
    PlaceReport("2026-09-19_10-00-00_200", "shown once");
    const std::vector<core::CrashRecord> before = core::CrashHandler::FindUnreported(temp.Path());
    ASSERT_EQ(before.size(), 1u);

    core::CrashHandler::MarkReported(before.front());

    EXPECT_TRUE(core::CrashHandler::FindUnreported(temp.Path()).empty());
}

/// @brief Non interactive notify leaves records unreported.
TEST_F(CrashHandlerTest, NonInteractiveNotifyLeavesRecordsUnreported)
{
    PlaceReport("2026-09-19_10-00-00_200", "seen by batch");

    const size_t count = core::CrashHandler::NotifyUnreported(temp.Path(), "CrashTest", false);

    EXPECT_EQ(count, 1u);
    EXPECT_EQ(core::CrashHandler::FindUnreported(temp.Path()).size(), 1u);
}

/// @brief Find unreported is empty without crash directory.
TEST_F(CrashHandlerTest, FindUnreportedIsEmptyWithoutCrashDirectory)
{
    EXPECT_TRUE(core::CrashHandler::FindUnreported(temp.Path()).empty());
}

/// @brief Find unreported skips entries that hold no report.
TEST_F(CrashHandlerTest, FindUnreportedSkipsEntriesThatHoldNoReport)
{
    PlaceReport("2026-09-19_10-00-00_200", "the only real one");
    /// @note 書き出しの途中で落ちた残骸と、人が置いたメモ。どちらも 1 件として数えない。
    std::filesystem::create_directories(CrashesDir() / "2026-09-19_11-00-00_300");
    std::ofstream(CrashesDir() / "notes.txt") << "not a report";

    const std::vector<core::CrashRecord> records = core::CrashHandler::FindUnreported(temp.Path());

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().summary, "the only real one");
}

/// @brief Summary is empty when the report carries no reason.
TEST_F(CrashHandlerTest, SummaryIsEmptyWhenTheReportCarriesNoReason)
{
    /// @note 書き出しの途中で切れたレポート。`what:` の後ろが空白だけでも CR を «何が起きたか» に混ぜない。
    PlaceReport("2026-09-19_10-00-00_200", "");

    const std::vector<core::CrashRecord> records = core::CrashHandler::FindUnreported(temp.Path());

    ASSERT_EQ(records.size(), 1u);
    EXPECT_TRUE(records.front().summary.empty()) << "summary=" << records.front().summary;
}

/// @brief Notify unreported counts nothing without records.
TEST_F(CrashHandlerTest, NotifyUnreportedCountsNothingWithoutRecords)
{
    EXPECT_EQ(core::CrashHandler::NotifyUnreported(temp.Path(), "CrashTest", false), 0u);
}

} // namespace fbzz::tests
