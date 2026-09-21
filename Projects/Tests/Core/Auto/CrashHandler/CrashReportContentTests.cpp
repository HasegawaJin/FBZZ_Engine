/// @file    CrashReportContentTests.cpp
/// @brief   report.txt が突き合わせに要る見出し (スレッド・ダンプ・版) を残し、連続した書き出しが互いを潰さないことを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
/// @see Docs/design/crash-report.md
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Core/CrashHandler.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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

class CrashReportContentTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(temp.IsValid());
        ASSERT_TRUE(core::CrashHandler::Install(temp.Path(), "CrashReportTest"));
    }

    void TearDown() override
    {
        /// @note 例外フィルターはプロセス全体の状態。次のテストへ持ち越さない。
        core::CrashHandler::Uninstall();
        EngineFixture::TearDown();
    }

    testkit::TempDir temp{ "CrashReportContent" };
};

TEST_F(CrashReportContentTest, RecordsTheThreadThatAskedForTheReport)
{
    const std::filesystem::path dir = core::CrashHandler::WriteReport("thread on the main thread");

    ASSERT_FALSE(dir.empty());
    std::ostringstream expected;
    expected << "thread:  " << GetCurrentThreadId();
    EXPECT_NE(ReadAll(dir / "report.txt").find(expected.str()), std::string::npos);
}

TEST_F(CrashReportContentTest, RecordsTheWorkerThreadWhenTheReportIsAskedForThere)
{
    /// @note ダンプは «落ちたスレッド» を軸に開く。呼んだスレッドが載らないと突き合わせられない。
    std::filesystem::path dir;
    DWORD                 workerId = 0;
    std::thread worker([&dir, &workerId] {
        workerId = GetCurrentThreadId();
        dir      = core::CrashHandler::WriteReport("thread on a worker");
    });
    worker.join();

    ASSERT_FALSE(dir.empty());
    std::ostringstream expected;
    expected << "thread:  " << workerId;
    EXPECT_NE(workerId, GetCurrentThreadId());
    EXPECT_NE(ReadAll(dir / "report.txt").find(expected.str()), std::string::npos);
}

TEST_F(CrashReportContentTest, NamesTheDumpFileWhenTheDumpWasWritten)
{
    const std::filesystem::path dir = core::CrashHandler::WriteReport("dump heading");

    ASSERT_FALSE(dir.empty());
    const std::string report = ReadAll(dir / "report.txt");
    EXPECT_NE(report.find("dump:    crash.dmp"), std::string::npos);
    EXPECT_EQ(report.find("dump:    failed"), std::string::npos);
}

TEST_F(CrashReportContentTest, RecordsTheEngineVersionAndTheCommandLine)
{
    const std::filesystem::path dir = core::CrashHandler::WriteReport("build identity");

    ASSERT_FALSE(dir.empty());
    const std::string report = ReadAll(dir / "report.txt");
    EXPECT_EQ(report.rfind("FBZZ crash report", 0), 0u);
    EXPECT_NE(report.find("engine:  "), std::string::npos);
    /// @note 起動コマンドはテスト exe 自身のもの。空行だと «どの起動で落ちたか» が残らない。
    const size_t command = report.find("command: ");
    ASSERT_NE(command, std::string::npos);
    EXPECT_GT(report.find("\r\n", command), command + std::string("command: ").size());
}

TEST_F(CrashReportContentTest, DoesNotOverwriteAnEarlierReportWrittenInTheSameSecond)
{
    const std::filesystem::path first  = core::CrashHandler::WriteReport("first report");
    const std::filesystem::path second = core::CrashHandler::WriteReport("second report");

    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_NE(first, second);
    EXPECT_NE(ReadAll(first / "report.txt").find("what:    first report"), std::string::npos);
    EXPECT_NE(ReadAll(second / "report.txt").find("what:    second report"), std::string::npos);
}

} // namespace fbzz::tests
