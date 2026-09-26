/// @file    TempDirTests.cpp
/// @brief   一時ディレクトリが別プロセスのファイルを削除せず、自分の生成物だけを回収することを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <fstream>
#include <memory>
#include <string>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fbzz::tests {
namespace {
constexpr wchar_t PARENT_ARGUMENT[] = L" --fbzz-temp-parent=\"";
constexpr DWORD CHILD_TIMEOUT_MS = 15000;

struct HandleCloser {
    void operator()(void* handle) const { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
}

class TempDirTest : public testkit::EngineFixture {};

TEST_F(TempDirTest, SameLabelInAnotherProcessPreservesTheParentFiles)
{
    const std::wstring commandLine = GetCommandLineW();
    const auto argument = commandLine.find(PARENT_ARGUMENT);
    if (argument != std::wstring::npos) {
        const auto begin = argument + std::size(PARENT_ARGUMENT) - 1;
        const auto end = commandLine.find(L'"', begin);
        ASSERT_NE(end, std::wstring::npos);
        const std::filesystem::path parent = commandLine.substr(begin, end - begin);
        {
            testkit::TempDir child("ProcessIsolation");
            ASSERT_TRUE(child.IsValid());
            EXPECT_NE(child.Path(), parent);
            EXPECT_TRUE(std::filesystem::exists(parent / "sentinel.txt"));
        }
        EXPECT_TRUE(std::filesystem::exists(parent / "sentinel.txt"));
        return;
    }

    testkit::TempDir parent("ProcessIsolation");
    ASSERT_TRUE(parent.IsValid());
    { std::ofstream sentinel(parent.File("sentinel.txt")); sentinel << "parent"; }
    wchar_t executable[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, std::size(executable));
    std::wstring childCommand = L"\"" + std::wstring(executable)
        + L"\" --gtest_filter=TempDirTest.SameLabelInAnotherProcessPreservesTheParentFiles";
    /// @note 自動実行を明示して子の終了時の入力待ちを抑止する。
    childCommand += L" --gtest_output=xml:\"" + parent.File("child.xml").native() + L"\"";
    childCommand += PARENT_ARGUMENT + parent.Path().native() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    ASSERT_TRUE(CreateProcessW(executable, childCommand.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(processHandle.get(), CHILD_TIMEOUT_MS);
    if (wait != WAIT_OBJECT_0) {
        /// @note このテストが生成した子だけを回収する。
        TerminateProcess(processHandle.get(), 99);
        WaitForSingleObject(processHandle.get(), CHILD_TIMEOUT_MS);
    }

    ASSERT_EQ(wait, WAIT_OBJECT_0);
    DWORD exitCode = STILL_ACTIVE;
    ASSERT_TRUE(GetExitCodeProcess(processHandle.get(), &exitCode));
    EXPECT_EQ(exitCode, 0u);
    std::ifstream sentinel(parent.File("sentinel.txt"));
    std::string content;
    sentinel >> content;
    EXPECT_EQ(content, "parent");
}

TEST_F(TempDirTest, DestructionRemovesOnlyItsOwnDirectory)
{
    testkit::TempDir first("SameLabel");
    ASSERT_TRUE(first.IsValid());
    std::filesystem::path secondPath;
    {
        testkit::TempDir second("SameLabel");
        ASSERT_TRUE(second.IsValid());
        secondPath = second.Path();
        EXPECT_NE(first.Path(), secondPath);
    }
    EXPECT_TRUE(std::filesystem::exists(first.Path()));
    EXPECT_FALSE(std::filesystem::exists(secondPath));
}
}
