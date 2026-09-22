/// @file    CrashHandlerFatalTests.cpp
/// @brief   致命経路を子プロセスへ隔離し、終了コード・ダンプ・診断を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
/// @see Docs/design/crash-report.md
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Core/CrashHandler.hpp>
#include <Engine/Core/DeveloperMode.hpp>
#include <Engine/Core/Logger.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace fbzz::tests {
namespace {

constexpr wchar_t CHILD_ROOT_ARGUMENT[] = L" --fbzz-crash-root=\"";
constexpr DWORD CHILD_TIMEOUT_MS = 15000;

struct HandleCloser {
    void operator()(void* handle) const { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

/// @note 計測デバッガー下では未処理例外フィルターが自動配送されないため、実際の SEH 情報を登録済みフィルターへ渡す。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setunhandledexceptionfilter SetUnhandledExceptionFilter (Remarks)
/// @see https://learn.microsoft.com/en-us/windows/win32/debug/getexceptioninformation GetExceptionInformation (filter expression)
void TriggerSeh(core::CrashTrigger trigger, LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    __try {
        core::CrashHandler::Trigger(trigger);
    } __except (filter(GetExceptionInformation())) {
    }
}

std::string ReadReport(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

} /// @note namespace

class CrashHandlerFatalTest : public testkit::EngineFixture {
protected:
    /// @note 子は同じ TEST_F だけを再実行する。TempDir は親だけが所有し、子の強制終了後も回収する。
    template<class Crash>
    void VerifyCrash(Crash crash, DWORD expectedExit, const char* expectedReason,
                     const char* expectedDetail = nullptr)
    {
        const std::wstring commandLine = GetCommandLineW();
        const auto argument = commandLine.find(CHILD_ROOT_ARGUMENT);
        if (argument != std::wstring::npos) {
            const auto begin = argument + std::size(CHILD_ROOT_ARGUMENT) - 1;
            const auto end = commandLine.find(L'"', begin);
            ASSERT_NE(end, std::wstring::npos);
            ASSERT_TRUE(core::CrashHandler::Install(commandLine.substr(begin, end - begin), "FatalChild"));
            core::DeveloperMode::SetPreference(true);
            core::Logger::SetMinLevel(core::LogLevel::DEBUG);
            core::Logger::Debug("fatal-debug-tail");
            core::Logger::Error("fatal-error-tail");
            crash();
            FAIL() << "Fatal handler returned without terminating the child";
            return;
        }

        testkit::TempDir temp("CrashFatal");
        ASSERT_TRUE(temp.IsValid());
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        ASSERT_GT(length, 0u);
        ASSERT_LT(length, std::size(executable));
        const auto* test = testing::UnitTest::GetInstance()->current_test_info();
        const std::string testName = std::string(test->test_suite_name()) + "." + test->name();
        std::wstring childCommand = L"\"" + std::wstring(executable) + L"\" --gtest_filter=";
        childCommand.append(testName.begin(), testName.end());
        /// @note レポート指定で TestKit の自動実行モードを明示し、親の起動方法にかかわらず CRT ダイアログと入力待ちを抑止する。
        childCommand += L" --gtest_output=xml:\"" + (temp.Path() / L"child.xml").native() + L"\"";
        childCommand += CHILD_ROOT_ARGUMENT + temp.Path().native() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        ASSERT_TRUE(CreateProcessW(executable, childCommand.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
        UniqueHandle processHandle(process.hProcess);
        UniqueHandle threadHandle(process.hThread);

        const DWORD wait = WaitForSingleObject(processHandle.get(), CHILD_TIMEOUT_MS);
        if (wait != WAIT_OBJECT_0) {
            /// @note 自分が生成した子だけを回収し、タイムアウトを成功扱いしない。
            TerminateProcess(processHandle.get(), 99);
            WaitForSingleObject(processHandle.get(), CHILD_TIMEOUT_MS);
        }
        ASSERT_EQ(wait, WAIT_OBJECT_0);
        DWORD exitCode = STILL_ACTIVE;
        ASSERT_TRUE(GetExitCodeProcess(processHandle.get(), &exitCode));
        EXPECT_EQ(exitCode, expectedExit);

        const auto reports = core::CrashHandler::FindUnreported(temp.Path());
        ASSERT_EQ(reports.size(), 1u);
        EXPECT_NE(reports[0].summary.find(expectedReason), std::string::npos);
        const auto text = ReadReport(reports[0].directory / "report.txt");
        EXPECT_NE(text.find("app:     FatalChild"), std::string::npos);
        EXPECT_NE(text.find("dump:    crash.dmp"), std::string::npos);
        EXPECT_NE(text.find("[DEBUG] fatal-debug-tail"), std::string::npos);
        EXPECT_NE(text.find("[ERROR] fatal-error-tail"), std::string::npos);
        if (expectedDetail) EXPECT_NE(text.find(expectedDetail), std::string::npos);
        std::ifstream dump(reports[0].directory / "crash.dmp", std::ios::binary);
        char signature[4]{};
        ASSERT_TRUE(dump.read(signature, sizeof(signature)));
        EXPECT_EQ(std::string(signature, sizeof(signature)), "MDMP");
    }

    static LPTOP_LEVEL_EXCEPTION_FILTER InstalledFilter()
    {
        const auto filter = SetUnhandledExceptionFilter(nullptr);
        SetUnhandledExceptionFilter(filter);
        return filter;
    }
};

TEST_F(CrashHandlerFatalTest, AccessViolationWritesTheFaultAddressAndTerminates)
{
    VerifyCrash([] {
        const auto filter = InstalledFilter();
        ASSERT_NE(filter, nullptr);
        TriggerSeh(core::CrashTrigger::AccessViolation, filter);
    }, EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION", "detail:  write to 0x0000000000000000");
}

TEST_F(CrashHandlerFatalTest, StackOverflowWritesAReportWithTheReservedStack)
{
    VerifyCrash([] {
        const auto filter = InstalledFilter();
        ASSERT_NE(filter, nullptr);
        TriggerSeh(core::CrashTrigger::StackOverflow, filter);
    }, EXCEPTION_STACK_OVERFLOW, "EXCEPTION_STACK_OVERFLOW");
}

TEST_F(CrashHandlerFatalTest, AbortWritesItsReasonAndTerminates)
{
    VerifyCrash([] { core::CrashHandler::Trigger(core::CrashTrigger::Abort); }, 3, "abort()");
}

TEST_F(CrashHandlerFatalTest, PureCallWritesItsReasonAndTerminates)
{
    VerifyCrash([] { core::CrashHandler::Trigger(core::CrashTrigger::PureCall); }, 3, "pure virtual function call");
}

TEST_F(CrashHandlerFatalTest, InvalidParameterWritesItsReasonAndTerminates)
{
    VerifyCrash([] { core::CrashHandler::Trigger(core::CrashTrigger::InvalidParameter); }, 3,
                "invalid parameter passed to a CRT function");
}

TEST_F(CrashHandlerFatalTest, MissingExceptionInformationUsesTheFallbackReasonAndExitCode)
{
    VerifyCrash([] {
        const auto filter = InstalledFilter();
        ASSERT_NE(filter, nullptr);
        filter(nullptr);
    }, 3, "unknown");
}

struct ExceptionCase {
    DWORD code;
    const char* name;
    ULONG_PTR access = 0;
    const char* detail = nullptr;
};

class CrashExceptionRecordTest : public CrashHandlerFatalTest,
                                public testing::WithParamInterface<ExceptionCase> {};

/// @note 合成レコードは例外名とアクセス種別の診断契約を検証する。実際の AV・スタック枯渇は別の TEST_F で起こす。
/// @see https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record EXCEPTION_RECORD (ExceptionInformation)
TEST_P(CrashExceptionRecordTest, PreservesExceptionCodeNameAndAddressInTheReport)
{
    const auto scenario = GetParam();
    VerifyCrash([scenario] {
        const auto filter = InstalledFilter();
        ASSERT_NE(filter, nullptr);
        CONTEXT context{};
        RtlCaptureContext(&context);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = scenario.code;
        /// @note 有効なモジュール先頭ならオフセットは常に 0。未知コードだけ未所属アドレスの表記を検証する。
        record.ExceptionAddress = scenario.code == 0xE0123456u ? nullptr : GetModuleHandleW(nullptr);
        record.NumberParameters = scenario.code == EXCEPTION_ACCESS_VIOLATION ? 2 : 0;
        record.ExceptionInformation[0] = scenario.access;
        record.ExceptionInformation[1] = 0x1234;
        EXCEPTION_POINTERS pointers{&record, &context};
        filter(&pointers);
    }, scenario.code, scenario.name, scenario.detail ? scenario.detail : "FBZZTestsCoreAuto.exe+0x0");
}

INSTANTIATE_TEST_SUITE_P(FatalRecords, CrashExceptionRecordTest, testing::Values(
    ExceptionCase{EXCEPTION_INT_DIVIDE_BY_ZERO, "EXCEPTION_INT_DIVIDE_BY_ZERO"},
    ExceptionCase{EXCEPTION_INT_OVERFLOW, "EXCEPTION_INT_OVERFLOW"},
    ExceptionCase{EXCEPTION_ILLEGAL_INSTRUCTION, "EXCEPTION_ILLEGAL_INSTRUCTION"},
    ExceptionCase{EXCEPTION_PRIV_INSTRUCTION, "EXCEPTION_PRIV_INSTRUCTION"},
    ExceptionCase{EXCEPTION_IN_PAGE_ERROR, "EXCEPTION_IN_PAGE_ERROR"},
    ExceptionCase{EXCEPTION_ARRAY_BOUNDS_EXCEEDED, "EXCEPTION_ARRAY_BOUNDS_EXCEEDED"},
    ExceptionCase{EXCEPTION_DATATYPE_MISALIGNMENT, "EXCEPTION_DATATYPE_MISALIGNMENT"},
    ExceptionCase{EXCEPTION_BREAKPOINT, "EXCEPTION_BREAKPOINT"},
    ExceptionCase{EXCEPTION_FLT_DIVIDE_BY_ZERO, "EXCEPTION_FLT_DIVIDE_BY_ZERO"},
    ExceptionCase{EXCEPTION_FLT_INVALID_OPERATION, "EXCEPTION_FLT_INVALID_OPERATION"},
    ExceptionCase{EXCEPTION_FLT_OVERFLOW, "EXCEPTION_FLT_OVERFLOW"},
    ExceptionCase{EXCEPTION_NONCONTINUABLE_EXCEPTION, "EXCEPTION_NONCONTINUABLE_EXCEPTION"},
    ExceptionCase{0xC0000374u, "STATUS_HEAP_CORRUPTION"},
    ExceptionCase{0xE06D7363u, "C++ exception"},
    ExceptionCase{0xE0123456u, "unknown exception (0xE0123456)", 0, "at 0x0000000000000000"},
    ExceptionCase{EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION", 0, "read from 0x0000000000001234"},
    ExceptionCase{EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION", 8, "execute at 0x0000000000001234"},
    ExceptionCase{EXCEPTION_ACCESS_VIOLATION, "EXCEPTION_ACCESS_VIOLATION", 7, "access 0x0000000000001234"}
));

} /// @note namespace fbzz::tests
