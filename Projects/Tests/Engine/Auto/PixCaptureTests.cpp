/// @file    PixCaptureTests.cpp
/// @brief   Deterministic PIX option and directory policy tests without loading a capturer.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <Engine/Core/PixCapture.hpp>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>

namespace fbzz::tests {

class PixCaptureTest : public testkit::EngineFixture {};

TEST_F(PixCaptureTest, OrdinaryArgumentsDoNotRequestCapture)
{
    const std::array<std::wstring_view, 2> arguments{L"--hidden", L"C:\\Project --pix-capture\\Assets"};
    core::PixCaptureOptions options{true, L"C:\\Old"};
    std::string reason = "old failure";
    ASSERT_TRUE(core::ParsePixCaptureOptions(arguments, options, reason));
    EXPECT_FALSE(options.requested);
    EXPECT_TRUE(options.installedDirectory.empty());
    EXPECT_TRUE(reason.empty());
}

TEST_F(PixCaptureTest, ExplicitCaptureSelectsThePinnedDefault)
{
    const std::array<std::wstring_view, 1> arguments{L"--pix-capture"};
    core::PixCaptureOptions options;
    std::string reason;
    ASSERT_TRUE(core::ParsePixCaptureOptions(arguments, options, reason));
    EXPECT_TRUE(options.requested);
    EXPECT_TRUE(options.installedDirectory.empty());
    std::filesystem::path directory;
    ASSERT_TRUE(core::ResolvePixCaptureDirectory(options, L"C:\\Program Files", directory, reason));
    EXPECT_EQ(directory, std::filesystem::path(L"C:\\Program Files\\Microsoft PIX\\2603.25"));
}

TEST_F(PixCaptureTest, AlreadyTokenizedDirectoryPreservesSpacesAndUnicode)
{
    const std::array<std::wstring_view, 3> arguments{L"--pix-capture", L"--pix-path", L"C:\\PIX \u30c4\u30fc\u30eb\\2603.25"};
    core::PixCaptureOptions options;
    std::string reason;
    ASSERT_TRUE(core::ParsePixCaptureOptions(arguments, options, reason));
    EXPECT_TRUE(options.requested);
    EXPECT_EQ(options.installedDirectory, std::filesystem::path(arguments.back()));
}

TEST_F(PixCaptureTest, ExplicitDirectoryCanPrecedeCaptureAndUseEqualsSyntax)
{
    const std::array<std::wstring_view, 2> arguments{L"--pix-path=C:\\Selected PIX", L"--pix-capture"};
    core::PixCaptureOptions options;
    std::string reason;
    ASSERT_TRUE(core::ParsePixCaptureOptions(arguments, options, reason));
    EXPECT_TRUE(options.requested);
    EXPECT_EQ(options.installedDirectory, std::filesystem::path(L"C:\\Selected PIX"));
}

TEST_F(PixCaptureTest, DirectoryWithoutOptInIsRejectedAtomically)
{
    const std::array<std::wstring_view, 2> arguments{L"--pix-path", L"C:\\Selected PIX"};
    core::PixCaptureOptions options{true, L"C:\\Preserved"};
    std::string reason;
    EXPECT_FALSE(core::ParsePixCaptureOptions(arguments, options, reason));
    EXPECT_TRUE(options.requested);
    EXPECT_EQ(options.installedDirectory, std::filesystem::path(L"C:\\Preserved"));
    EXPECT_FALSE(reason.empty());
}

TEST_F(PixCaptureTest, MissingEmptyRelativeAndDuplicateDirectoriesAreRejected)
{
    const std::array<std::array<std::wstring_view, 5>, 5> invalid{{
        {L"--pix-capture", L"--pix-path", L"", L"", L""},
        {L"--pix-capture", L"--pix-path=", L"", L"", L""},
        {L"--pix-capture", L"--pix-path", L"relative\\PIX", L"", L""},
        {L"--pix-capture", L"--pix-path", L"C:\\First", L"--pix-path", L"C:\\Second"},
        {L"--pix-capture=false", L"", L"", L"", L""},
    }};
    for (const auto& arguments : invalid) {
        core::PixCaptureOptions options;
        std::string reason;
        EXPECT_FALSE(core::ParsePixCaptureOptions(arguments, options, reason));
        EXPECT_FALSE(options.requested);
        EXPECT_TRUE(options.installedDirectory.empty());
        EXPECT_FALSE(reason.empty());
    }
    const std::array<std::wstring_view, 2> missing{L"--pix-capture", L"--pix-path"};
    core::PixCaptureOptions options;
    std::string reason;
    EXPECT_FALSE(core::ParsePixCaptureOptions(missing, options, reason));
}

TEST_F(PixCaptureTest, UnrequestedResolutionRequiresNoInstallationOrProgramFiles)
{
    std::filesystem::path directory = L"C:\\Old";
    std::string reason = "old failure";
    ASSERT_TRUE(core::ResolvePixCaptureDirectory({}, {}, directory, reason));
    EXPECT_TRUE(directory.empty());
    EXPECT_TRUE(reason.empty());
}

TEST_F(PixCaptureTest, ExplicitAbsoluteDirectoryDoesNotDependOnProgramFiles)
{
    std::filesystem::path directory;
    std::string reason;
    ASSERT_TRUE(core::ResolvePixCaptureDirectory({true, L"D:\\PIX\\previous\\..\\selected"}, {}, directory, reason));
    EXPECT_EQ(directory, std::filesystem::path(L"D:\\PIX\\selected"));
}

TEST_F(PixCaptureTest, MissingDefaultAndRelativeExplicitDirectoryLeaveOutputUnchanged)
{
    std::filesystem::path directory = L"C:\\Preserved";
    std::string reason;
    EXPECT_FALSE(core::ResolvePixCaptureDirectory({true, {}}, {}, directory, reason));
    EXPECT_EQ(directory, std::filesystem::path(L"C:\\Preserved"));
    EXPECT_FALSE(reason.empty());
    EXPECT_FALSE(core::ResolvePixCaptureDirectory({true, L"relative"}, L"C:\\Program Files", directory, reason));
    EXPECT_EQ(directory, std::filesystem::path(L"C:\\Preserved"));
    EXPECT_FALSE(core::ResolvePixCaptureDirectory({true, {}}, L"relative", directory, reason));
    EXPECT_EQ(directory, std::filesystem::path(L"C:\\Preserved"));
}

TEST_F(PixCaptureTest, PureParsingAndResolutionNeverPublishCaptureReadiness)
{
    const auto before = core::GetPixCaptureStatus();
    const std::array<std::wstring_view, 1> arguments{L"--pix-capture"};
    core::PixCaptureOptions options;
    std::string reason;
    std::filesystem::path directory;
    ASSERT_TRUE(core::ParsePixCaptureOptions(arguments, options, reason));
    ASSERT_TRUE(core::ResolvePixCaptureDirectory(options, L"C:\\Program Files", directory, reason));
    const auto& after = core::GetPixCaptureStatus();
    EXPECT_EQ(after.requested, before.requested);
    EXPECT_EQ(after.ready, before.ready);
    EXPECT_EQ(after.reason, before.reason);
    EXPECT_EQ(after.installedDirectory, before.installedDirectory);
    const core::PixCaptureStatus local;
    EXPECT_FALSE(local.requested);
    EXPECT_FALSE(local.ready);
    EXPECT_TRUE(local.reason.empty());
    EXPECT_TRUE(local.installedDirectory.empty());
}

} /// @note namespace fbzz::tests
