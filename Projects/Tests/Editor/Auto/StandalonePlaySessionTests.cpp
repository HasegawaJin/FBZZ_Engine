/// @file    StandalonePlaySessionTests.cpp
/// @brief   Standalone Play process seam covers ownership, failures, replay, Stop and Windows argv boundaries.
/// @author  Hasegawa Jin
/// @date    2026-10-23
#include <TestKit/TestKit.hpp>
#include <Editor/StandalonePlaySession.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Engine/Util/FileSystem.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#include <memory>
#include <utility>

namespace fbzz::tests {
namespace {

struct ProcessObservation {
    int starts = 0;
    int stops = 0;
    int terminations = 0;
    bool canStart = true;
    uint32_t exitCode = 0;
    editor::IStandalonePlayProcess::PollResult result = editor::IStandalonePlayProcess::PollResult::RUNNING;
    editor::StandalonePlayRequest request;
};

class ProcessSeam final : public editor::IStandalonePlayProcess {
public:
    explicit ProcessSeam(std::shared_ptr<ProcessObservation> observation) : m_observation(std::move(observation)) {}
    bool Start(const editor::StandalonePlayRequest& request, std::string& diagnostic) override
    {
        ++m_observation->starts;
        m_observation->request = request;
        if (!m_observation->canStart) diagnostic = "CreateProcess rejected the executable.";
        return m_observation->canStart;
    }
    PollResult Poll(uint32_t& exitCode, std::string& diagnostic) override
    {
        exitCode = m_observation->exitCode;
        if (m_observation->result == PollResult::FAILED) diagnostic = "Process handle poll failed.";
        return m_observation->result;
    }
    bool RequestStop(std::string&) override { ++m_observation->stops; return true; }
    bool Terminate(std::string&) override { ++m_observation->terminations; return true; }
private:
    std::shared_ptr<ProcessObservation> m_observation;
};

} /// @note namespace

class StandalonePlaySessionTest : public testkit::Fixture {
protected:
    void SetUp() override
    {
        Fixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        m_request.executable = m_temp.Path() / L"Editor With Spaces.exe";
        m_request.project = m_temp.Path() / L"Project With Spaces";
        m_request.scene = m_request.project / L"Assets" / L"Scenes" / L"Current Scene.scene";
        m_request.log = m_request.project / L"Library" / L"StandalonePlay" / L"session.log";
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(m_request.scene.parent_path()));
        ASSERT_TRUE(util::FileSystem::WriteText(m_request.executable, "process seam executable"));
        ASSERT_TRUE(util::FileSystem::WriteText(m_request.project / L".fbzz_proj", "project fixture"));
        ASSERT_TRUE(util::FileSystem::WriteText(m_request.scene, "scene fixture remains unchanged"));
    }
    std::unique_ptr<editor::IStandalonePlayProcess> Process() { return std::make_unique<ProcessSeam>(m_observation); }
    testkit::TempDir m_temp{"standaloneplaysession"};
    editor::StandalonePlayRequest m_request;
    std::shared_ptr<ProcessObservation> m_observation = std::make_shared<ProcessObservation>();
};

TEST_F(StandalonePlaySessionTest, MissingExecutableRejectsBeforeLaunchingAndDoesNotSaveTheScene)
{
    editor::StandalonePlaySession session(Process());
    m_request.executable = m_temp.Path() / L"missing.exe";
    EXPECT_FALSE(session.Start(m_request));
    EXPECT_TRUE(session.Failed());
    EXPECT_FALSE(session.IsActive());
    EXPECT_EQ(m_observation->starts, 0);
    std::string text;
    ASSERT_TRUE(util::FileSystem::ReadText(m_request.scene, text));
    EXPECT_EQ(text, "scene fixture remains unchanged");
}

TEST_F(StandalonePlaySessionTest, FailedLaunchDoesNotAcquireOwnershipAndCanBeRetried)
{
    editor::StandalonePlaySession session(Process());
    m_observation->canStart = false;
    EXPECT_FALSE(session.Start(m_request));
    EXPECT_NE(session.Diagnostic().find("CreateProcess"), std::string::npos);
    session.Stop();
    session.Shutdown();
    EXPECT_EQ(m_observation->stops, 0);
    EXPECT_EQ(m_observation->terminations, 0);
    m_observation->canStart = true;
    EXPECT_TRUE(session.Start(m_request));
    EXPECT_EQ(m_observation->starts, 2);
    EXPECT_EQ(m_observation->request.scene, m_request.scene);
    EXPECT_EQ(m_observation->request.project, m_request.project);
    EXPECT_EQ(session.LogPath(), m_request.log);
}

TEST_F(StandalonePlaySessionTest, DoubleStartAndStopAreIdempotentAndStopPollsWithoutWaiting)
{
    editor::StandalonePlaySession session(Process());
    ASSERT_TRUE(session.Start(m_request));
    EXPECT_FALSE(session.Start(m_request));
    EXPECT_EQ(m_observation->starts, 1);
    session.Stop();
    session.Stop();
    EXPECT_EQ(m_observation->stops, 1);
    EXPECT_TRUE(session.IsStopping());
    EXPECT_FALSE(session.Tick(1.0f));
    EXPECT_EQ(m_observation->terminations, 0);
    EXPECT_FALSE(session.Tick(1.0f));
    EXPECT_EQ(m_observation->terminations, 1);
    EXPECT_FALSE(session.Tick(1.0f));
    EXPECT_EQ(m_observation->terminations, 1);
    m_observation->exitCode = 1;
    m_observation->result = editor::IStandalonePlayProcess::PollResult::EXITED;
    EXPECT_TRUE(session.Tick(0.0f));
    EXPECT_FALSE(session.IsActive());
    EXPECT_FALSE(session.Failed());
}

TEST_F(StandalonePlaySessionTest, NormalExitAllowsReplayAndAbnormalExitRetainsCodeAndLog)
{
    editor::StandalonePlaySession session(Process());
    ASSERT_TRUE(session.Start(m_request));
    m_observation->result = editor::IStandalonePlayProcess::PollResult::EXITED;
    EXPECT_TRUE(session.Tick(0.0f));
    EXPECT_FALSE(session.IsActive());
    EXPECT_FALSE(session.Failed());
    EXPECT_EQ(m_observation->terminations, 0);
    ASSERT_TRUE(session.Start(m_request));
    m_observation->exitCode = 0xC0000005u;
    EXPECT_TRUE(session.Tick(0.0f));
    EXPECT_TRUE(session.Failed());
    EXPECT_EQ(session.ExitCode(), 0xC0000005u);
    EXPECT_EQ(session.LogPath(), m_request.log);
    EXPECT_NE(session.Diagnostic().find("exited with code"), std::string::npos);
    EXPECT_FALSE(session.Tick(10.0f));
}

TEST_F(StandalonePlaySessionTest, PollFailureRetainsOwnershipUntilExplicitShutdown)
{
    editor::StandalonePlaySession session(Process());
    ASSERT_TRUE(session.Start(m_request));
    m_observation->result = editor::IStandalonePlayProcess::PollResult::FAILED;
    EXPECT_TRUE(session.Tick(0.0f));
    EXPECT_TRUE(session.IsActive());
    EXPECT_TRUE(session.Failed());
    session.Shutdown();
    EXPECT_EQ(m_observation->terminations, 1);
    EXPECT_FALSE(session.IsActive());
}

TEST_F(StandalonePlaySessionTest, EditorExitTerminatesOnlyItsOwnLaunch)
{
    auto other = std::make_shared<ProcessObservation>();
    editor::StandalonePlaySession otherSession(std::make_unique<ProcessSeam>(other));
    ASSERT_TRUE(otherSession.Start(m_request));
    {
        editor::StandalonePlaySession session(Process());
        ASSERT_TRUE(session.Start(m_request));
    }
    EXPECT_EQ(m_observation->terminations, 1);
    EXPECT_EQ(other->terminations, 0);
    EXPECT_EQ(other->stops, 0);
    EXPECT_TRUE(otherSession.IsActive());
}

TEST_F(StandalonePlaySessionTest, WindowsCommandLinePreservesExactProjectSceneAndLogOperands)
{
    m_request.project = L"C:\\Project With Spaces\\";
    m_request.scene = L"C:\\Project With Spaces\\a\"quoted.scene";
    const auto command = editor::StandalonePlaySession::CommandLine(m_request);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command.c_str(), &argc);
    ASSERT_NE(argv, nullptr);
    if (argc == 9) {
        EXPECT_EQ(std::wstring(argv[0]), m_request.executable.wstring());
        EXPECT_EQ(std::wstring(argv[1]), L"--standalone");
        EXPECT_EQ(std::wstring(argv[2]), L"--editor-play");
        EXPECT_EQ(std::wstring(argv[4]), m_request.project.wstring());
        EXPECT_EQ(std::wstring(argv[6]), m_request.scene.wstring());
        EXPECT_EQ(std::wstring(argv[8]), m_request.log.wstring());
    }
    EXPECT_EQ(argc, 9);
    LocalFree(argv);
}

TEST_F(StandalonePlaySessionTest, AutomatedPlayCommandLinePreservesOperandsAndDisablesPhysicalInput)
{
    m_request.allowPhysicalInput = false;
    const auto command = editor::StandalonePlaySession::CommandLine(m_request);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(command.c_str(), &argc);
    ASSERT_NE(argv, nullptr);
    if (argc == 10) {
        EXPECT_EQ(std::wstring(argv[0]), m_request.executable.wstring());
        EXPECT_EQ(std::wstring(argv[1]), L"--standalone");
        EXPECT_EQ(std::wstring(argv[2]), L"--editor-play");
        EXPECT_EQ(std::wstring(argv[4]), m_request.project.wstring());
        EXPECT_EQ(std::wstring(argv[6]), m_request.scene.wstring());
        EXPECT_EQ(std::wstring(argv[8]), m_request.log.wstring());
        EXPECT_EQ(std::wstring(argv[9]), L"--play-no-physical-input");
    }
    EXPECT_EQ(argc, 10);
    LocalFree(argv);
}

TEST_F(StandalonePlaySessionTest, AutomatedInputPolicyReachesProcessWithoutChangingLaterManualPlay)
{
    editor::StandalonePlaySession session(Process());
    const auto manualRequest = m_request;
    EXPECT_TRUE(manualRequest.allowPhysicalInput);
    m_request.allowPhysicalInput = false;

    ASSERT_TRUE(session.Start(m_request));
    EXPECT_FALSE(m_observation->request.allowPhysicalInput);
    m_observation->result = editor::IStandalonePlayProcess::PollResult::EXITED;
    ASSERT_TRUE(session.Tick(0.0f));

    ASSERT_TRUE(session.Start(manualRequest));
    EXPECT_TRUE(m_observation->request.allowPhysicalInput);
    EXPECT_EQ(m_observation->starts, 2);
    EXPECT_EQ(editor::StandalonePlaySession::CommandLine(m_observation->request).find(
        L"--play-no-physical-input"), std::wstring::npos);
}

TEST_F(StandalonePlaySessionTest, SettingsKeepExecutionModeIndependentOfMaximizedDisplayPolicy)
{
    const auto config = m_request.project / L"Assets" / L"EditorConfig" / L"editor_settings.toml";
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(config.parent_path()));
    editor::EditorSettings original;
    original.playFocusMode = 1;
    original.playExecutionMode = static_cast<int>(editor::PlayExecutionMode::STANDALONE);
    ASSERT_TRUE(original.Save(util::FileSystem::PathToUtf8(config), util::FileSystem::PathToUtf8(m_request.project)));
    editor::EditorSettings restored;
    ASSERT_TRUE(restored.Load(util::FileSystem::PathToUtf8(config), util::FileSystem::PathToUtf8(m_request.project)));
    EXPECT_EQ(restored.playFocusMode, 1);
    EXPECT_EQ(restored.playExecutionMode, 1);
}

TEST_F(StandalonePlaySessionTest, ExecutionModeCannotSwitchWhileTheOwnedChildIsRunning)
{
    editor::StandalonePlaySession session(Process());
    editor::EditorContext context;
    editor::UndoStack undo;
    editor::OperatorRegistry operations;
    context.undoStack = &undo;
    context.operators = &operations;
    context.standalonePlay = &session;
    context.playFocusMode = editor::EditorContext::PlayFocusMode::Maximized;
    editor::RegisterStandalonePlayOperators(operations);
    editor::OpArgs mode;
    mode.Set("mode", std::string("standalone"));
    ASSERT_TRUE(editor::InvokeOperator(context, "play.set_execution_mode", mode).ok);
    ASSERT_TRUE(session.Start(m_request));
    mode.Set("mode", std::string("inEditor"));
    EXPECT_FALSE(editor::InvokeOperator(context, "play.set_execution_mode", mode).ok);
    EXPECT_EQ(context.playExecutionMode, editor::PlayExecutionMode::STANDALONE);
    EXPECT_EQ(context.playFocusMode, editor::EditorContext::PlayFocusMode::Maximized);
    m_observation->result = editor::IStandalonePlayProcess::PollResult::EXITED;
    ASSERT_TRUE(session.Tick(0.0f));
    EXPECT_TRUE(editor::InvokeOperator(context, "play.set_execution_mode", mode).ok);
    EXPECT_EQ(context.playExecutionMode, editor::PlayExecutionMode::IN_EDITOR);
    EXPECT_EQ(context.playFocusMode, editor::EditorContext::PlayFocusMode::Maximized);
}

} /// @note namespace fbzz::tests
