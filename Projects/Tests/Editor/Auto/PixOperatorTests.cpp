/// @file    PixOperatorTests.cpp
/// @brief   PIX operators preserve boot readiness and never launch external applications in tests.
/// @author  Hasegawa Jin
/// @date    2026-10-02
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>

#include "Op/PixOperators.hpp"
#include "Util/PixLauncher.hpp"
#include <Editor/Util/UndoStack.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace fbzz::tests {

class PixOperatorTest : public testkit::EditorFixture {
protected:
    editor::OperatorRegistry m_registry;
    editor::UndoStack m_undo;
    core::PixCaptureStatus m_status;
    std::filesystem::path m_defaultDirectory;
    std::filesystem::path m_launchedExecutable;
    int m_launchCount = 0;
    bool m_launchSucceeds = true;

    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().operators = &m_registry;
        Context().undoStack = &m_undo;
        m_defaultDirectory = File("Default PIX/2603.25");
        editor::PixOperatorServices services;
        services.readCaptureStatus = [this] { return m_status; };
        services.defaultInstalledDirectory = m_defaultDirectory;
        services.launchUi = [this](const std::filesystem::path& executable, std::string& error) {
            ++m_launchCount;
            m_launchedExecutable = executable;
            if (!m_launchSucceeds) error = "Synthetic launch failure.";
            return m_launchSucceeds;
        };
        editor::RegisterPixOperators(m_registry, std::move(services));
    }

    void WritePlaceholder(const std::filesystem::path& path)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        ASSERT_FALSE(ec);
        std::ofstream file(path, std::ios::binary);
        ASSERT_TRUE(file);
        file << "Test placeholder: never execute.";
        ASSERT_TRUE(file);
    }

    editor::OpResult Query() { return editor::InvokeOperator(Context(), "tools.pix_status"); }
    editor::OpResult Open() { return editor::InvokeOperator(Context(), "tools.pix_open"); }
};

TEST_F(PixOperatorTest, RegistersQueryAndActionWithoutUndoOrSceneRequirements)
{
    const auto* status = m_registry.Find("tools.pix_status");
    const auto* open = m_registry.Find("tools.pix_open");
    ASSERT_NE(status, nullptr);
    ASSERT_NE(open, nullptr);
    EXPECT_EQ(status->kind, editor::OpKind::Query);
    EXPECT_EQ(open->kind, editor::OpKind::Action);
    EXPECT_EQ(status->category, "Tools");
    EXPECT_EQ(open->category, "Tools");
    const auto result = Query();
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.data.Find("installed"), nullptr);
    EXPECT_FALSE(result.data.Find("installed")->AsBool());
    EXPECT_EQ(m_launchCount, 0);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_EQ(Context().activeScene, nullptr);
}

TEST_F(PixOperatorTest, NormalStartupDoesNotBecomeReadyFromExistingCapturerFiles)
{
    WritePlaceholder(m_defaultDirectory / L"WinPix.exe");
    WritePlaceholder(m_defaultDirectory / L"WinPixGpuCapturer.dll");
    const auto result = Query();
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.data.Find("installed")->AsBool());
    EXPECT_FALSE(result.data.Find("requested")->AsBool());
    EXPECT_FALSE(result.data.Find("captureReady")->AsBool());
    EXPECT_TRUE(result.data.Find("captureDirectory")->AsString().empty());
    EXPECT_TRUE(result.data.Find("requiresRestart")->AsBool());
    EXPECT_NE(result.message.find("--pix-capture"), std::string::npos);
    EXPECT_EQ(m_launchCount, 0);
    EXPECT_FALSE(m_status.ready);
}

TEST_F(PixOperatorTest, OpensOnlyCanonicalUiFromDefaultInstallationAndPreservesSession)
{
    WritePlaceholder(m_defaultDirectory / L"WinPix.exe");
    const auto projectRoot = Context().projectRoot;
    const auto result = Open();
    ASSERT_TRUE(result.ok);
    EXPECT_TRUE(result.data.Find("opened")->AsBool());
    EXPECT_FALSE(result.data.Find("captureReady")->AsBool());
    EXPECT_NE(result.message.find("--pix-capture"), std::string::npos);
    EXPECT_EQ(m_launchCount, 1);
    EXPECT_TRUE(m_launchedExecutable.is_absolute());
    EXPECT_EQ(m_launchedExecutable, std::filesystem::canonical(m_defaultDirectory / L"WinPix.exe"));
    EXPECT_EQ(Context().projectRoot, projectRoot);
    EXPECT_EQ(Context().activeScene, nullptr);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
    EXPECT_FALSE(m_status.requested);
    EXPECT_FALSE(m_status.ready);
}

TEST_F(PixOperatorTest, SelectedBootDirectoryWinsWithoutSilentDefaultFallback)
{
    WritePlaceholder(m_defaultDirectory / L"WinPix.exe");
    m_status.requested = true;
    m_status.installedDirectory = File("Selected PIX");
    m_status.reason = "Selected capturer could not be loaded.";
    const auto missing = Open();
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(missing.errorCode, "NOT_AVAILABLE");
    EXPECT_EQ(m_launchCount, 0);
    WritePlaceholder(m_status.installedDirectory / L"WinPix.exe");
    const auto result = Open();
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(m_launchCount, 1);
    EXPECT_EQ(m_launchedExecutable, std::filesystem::canonical(m_status.installedDirectory / L"WinPix.exe"));
    EXPECT_FALSE(result.data.Find("captureReady")->AsBool());
    EXPECT_EQ(result.data.Find("reason")->AsString(), m_status.reason);
}

TEST_F(PixOperatorTest, ReportsBootReadinessIndependentlyOfUiInstallation)
{
    m_status.requested = true;
    m_status.ready = true;
    m_status.installedDirectory = File("Boot PIX");
    const auto noUi = Query();
    ASSERT_TRUE(noUi.ok);
    EXPECT_TRUE(noUi.data.Find("captureReady")->AsBool());
    EXPECT_FALSE(noUi.data.Find("requiresRestart")->AsBool());
    EXPECT_FALSE(noUi.data.Find("installed")->AsBool());
    WritePlaceholder(m_status.installedDirectory / L"WinPix.exe");
    const auto opened = Open();
    ASSERT_TRUE(opened.ok);
    EXPECT_TRUE(opened.data.Find("captureReady")->AsBool());
    EXPECT_EQ(m_launchCount, 1);
    EXPECT_TRUE(m_status.ready);
}

TEST_F(PixOperatorTest, FailedLaunchDoesNotChangeReadinessOrPublishSuccess)
{
    WritePlaceholder(m_defaultDirectory / L"WinPix.exe");
    m_status.requested = true;
    m_status.reason = "Capturer loading failed.";
    m_launchSucceeds = false;
    const auto result = Open();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "PIX_LAUNCH_FAILED");
    EXPECT_EQ(result.message, "Synthetic launch failure.");
    EXPECT_EQ(result.data.Find("opened"), nullptr);
    EXPECT_FALSE(result.data.Find("captureReady")->AsBool());
    EXPECT_EQ(m_launchCount, 1);
    EXPECT_FALSE(m_status.ready);
    EXPECT_EQ(Query().data.Find("reason")->AsString(), m_status.reason);
    EXPECT_EQ(m_undo.GetHistorySize(), 0u);
}

TEST_F(PixOperatorTest, MissingLaunchServiceNeverFallsBackToARealApplication)
{
    WritePlaceholder(m_defaultDirectory / L"WinPix.exe");
    editor::PixOperatorServices services;
    services.readCaptureStatus = [this] { return m_status; };
    services.defaultInstalledDirectory = m_defaultDirectory;
    editor::RegisterPixOperators(m_registry, std::move(services));
    EXPECT_TRUE(Query().ok);
    EXPECT_FALSE(Open().ok);
    EXPECT_EQ(m_launchCount, 0);
}

TEST_F(PixOperatorTest, UiResolutionRejectsRelativeMissingAndDirectoryTargetsAtomically)
{
    std::filesystem::path executable = File("Unchanged.exe");
    const auto original = executable;
    std::string error;
    EXPECT_FALSE(editor::ResolvePixUiExecutable("Relative PIX", executable, error));
    EXPECT_EQ(executable, original);
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(editor::ResolvePixUiExecutable(m_defaultDirectory, executable, error));
    EXPECT_EQ(executable, original);
    std::error_code ec;
    std::filesystem::create_directories(m_defaultDirectory / L"WinPix.exe", ec);
    ASSERT_FALSE(ec);
    EXPECT_FALSE(editor::ResolvePixUiExecutable(m_defaultDirectory, executable, error));
    EXPECT_EQ(executable, original);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(m_launchCount, 0);
}

} /// @note namespace fbzz::tests
