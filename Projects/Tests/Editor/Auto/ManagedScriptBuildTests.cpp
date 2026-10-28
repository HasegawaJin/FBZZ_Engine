/// @file    ManagedScriptBuildTests.cpp
/// @brief   C# ビルド要求の版固定、終端状態と AI 境界の互換性を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/ManagedScriptEditor.hpp>
#include "ManagedScriptPublicationState.hpp"
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Engine/Scene/Managed/ManagedScriptRuntime.hpp>
#include <Engine/Core/Concurrency/TaskSystem.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <Core/Profiler/Profiler.hpp>
#include <algorithm>
#include <string>
#include <filesystem>
#include <fstream>
#include <semaphore>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace fbzz::tests {

namespace {

class ManualManagedSourceWatcher final : public editor::IManagedSourceWatcher {
public:
    bool hold = false;
    bool usePublishedRevision = false;
    std::uint64_t publishedRevision = 0;
    std::uint64_t requestedRevision = 0;
    std::filesystem::path startedManifest;

    void Start(const std::filesystem::path& project, const std::filesystem::path& inputManifest) override
    {
        m_project = project;
        m_manifest = inputManifest;
        startedManifest = inputManifest;
        m_started = true;
    }
    void Stop() override { m_started = false; }
    std::uint64_t RequestRefresh() override { return ++requestedRevision; }
    bool Poll(editor::ManagedSourceSnapshot& snapshot) override
    {
        if (!m_started || hold) return false;
        snapshot.revision = usePublishedRevision ? publishedRevision : requestedRevision;
        snapshot.valid = editor::ComputeManagedSourceSnapshot(m_project, m_manifest, snapshot);
        return true;
    }
private:
    std::filesystem::path m_project;
    std::filesystem::path m_manifest;
    bool m_started = false;
};

class ScopedManagedCacheEnvironment {
public:
    explicit ScopedManagedCacheEnvironment(const wchar_t* value)
    {
        const DWORD length = GetEnvironmentVariableW(L"FBZZ_MANAGED_CACHE_SCOPE", nullptr, 0);
        if (length) {
            m_previous.resize(length);
            const DWORD written = GetEnvironmentVariableW(L"FBZZ_MANAGED_CACHE_SCOPE", m_previous.data(), length);
            m_previous.resize(written);
            m_hadValue = true;
        }
        EXPECT_NE(SetEnvironmentVariableW(L"FBZZ_MANAGED_CACHE_SCOPE", value), FALSE);
    }
    ~ScopedManagedCacheEnvironment()
    {
        EXPECT_NE(SetEnvironmentVariableW(L"FBZZ_MANAGED_CACHE_SCOPE", m_hadValue ? m_previous.c_str() : nullptr), FALSE);
    }
private:
    std::wstring m_previous;
    bool m_hadValue = false;
};

} /// @note namespace

class ManagedScriptBuildTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().buildConsole = &m_console;
        Context().operators = &m_operators;
        Context().undoStack = &m_undo;
        editor::ManagedScriptEditor::RegisterOperator(m_operators);
    }

    editor::ai::JsonValue Send(const char* kind, editor::ai::JsonValue payload)
    {
        editor::ai::JsonValue request = editor::ai::JsonValue::MakeObject();
        request.Set("protocol", editor::ai::JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", editor::ai::JsonValue("managed-build-test"));
        request.Set("kind", editor::ai::JsonValue(kind));
        request.Set("dryRun", editor::ai::JsonValue(false));
        request.Set("payload", std::move(payload));
        editor::ai::EditorBusDispatcher dispatcher(Context());
        const auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(request)));
        EXPECT_TRUE(response.has_value());
        return response.value_or(editor::ai::JsonValue{});
    }

    editor::BuildConsole m_console;
    editor::OperatorRegistry m_operators;
    editor::UndoStack m_undo;
};

TEST_F(ManagedScriptBuildTest, SupersededCompilationCannotPublishOrReviveTheOldRequest)
{
    editor::ManagedBuildHistory history;
    const std::string first = history.Queue(101, false, 7);
    history.BeginCompile(first, "generation-first");
    const std::string second = history.Queue(102, true, 7);
    ASSERT_NE(first, second);
    ASSERT_NE(history.Find(first), nullptr);
    EXPECT_EQ(history.Find(first)->state, editor::ManagedBuildState::SUPERSEDED);
    EXPECT_FALSE(history.CompleteCompile(first, 102, true, true, 0, "RESULT success\n"));
    history.MarkApplied(first, 8);
    EXPECT_EQ(history.Find(first)->state, editor::ManagedBuildState::SUPERSEDED);
    EXPECT_EQ(history.Find(first)->appliedGeneration, 0u);
    history.BeginCompile(second, "generation-second");
    EXPECT_TRUE(history.CompleteCompile(second, 102, true, true, 0, ""));
    history.Transition(second, editor::ManagedBuildState::DEFERRED);
    EXPECT_FALSE(editor::ManagedBuildHistory::IsTerminal(history.Find(second)->state));
    EXPECT_EQ(history.Find(second)->previousGeneration, 7u);
    EXPECT_EQ(history.Find(second)->appliedGeneration, 0u);
    history.Transition(second, editor::ManagedBuildState::APPLYING);
    history.MarkApplied(second, 8);
    EXPECT_EQ(history.Find(second)->state, editor::ManagedBuildState::APPLIED);
    EXPECT_EQ(history.Find(second)->appliedGeneration, 8u);
}

TEST_F(ManagedScriptBuildTest, ContentFingerprintIgnoresOutputAndDetectsSameTimestampImports)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Library/Candidate")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    const std::filesystem::path source = File("Assets/Scripts/Probe.cs");
    ASSERT_TRUE(util::FileSystem::WriteText(source, "class One {}"));
    std::uint64_t initial = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(File("Fixture.Scripts.csproj"), File("inputs.txt"), initial));
    const auto modified = std::filesystem::last_write_time(source);
    ASSERT_TRUE(util::FileSystem::WriteText(File("Library/Candidate/Generated.cs"), "class Output {}"));
    std::uint64_t unchanged = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(File("Fixture.Scripts.csproj"), File("inputs.txt"), unchanged));
    EXPECT_EQ(initial, unchanged);
    ASSERT_TRUE(util::FileSystem::WriteText(source, "class Two {}"));
    std::filesystem::last_write_time(source, modified);
    std::uint64_t changed = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(File("Fixture.Scripts.csproj"), File("inputs.txt"), changed));
    EXPECT_NE(initial, changed);
}

TEST_F(ManagedScriptBuildTest, EvaluatedReferenceSourcesAndImportedPropsInvalidateTheFingerprint)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Game/Assets")));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Utility")));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Imports")));
    const auto project = File("Game/Fixture.Scripts.csproj");
    const auto dependency = File("Utility/Utility.csproj");
    const auto props = File("Imports/Custom.props");
    ASSERT_TRUE(util::FileSystem::WriteText(project, "<Project />"));
    ASSERT_TRUE(util::FileSystem::WriteText(dependency, "<Project />"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Utility/Helper.cs"), "class Helper {}"));
    ASSERT_TRUE(util::FileSystem::WriteText(props, "<Project />"));
    const std::string manifest = "P|" + util::FileSystem::PathToUtf8(project) + "\nP|" +
        util::FileSystem::PathToUtf8(dependency) + "\nF|" + util::FileSystem::PathToUtf8(props) + "\n";
    ASSERT_TRUE(util::FileSystem::WriteText(File("inputs.txt"), manifest));
    std::uint64_t initial = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), initial));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Utility/NewHelper.cs"), "class NewHelper {}"));
    std::uint64_t added = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), added));
    EXPECT_NE(initial, added);
    const auto modified = std::filesystem::last_write_time(props);
    ASSERT_TRUE(util::FileSystem::WriteText(props, "<Project><PropertyGroup /></Project>"));
    std::filesystem::last_write_time(props, modified);
    std::uint64_t imported = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), imported));
    EXPECT_NE(added, imported);
    ASSERT_TRUE(std::filesystem::remove(props));
    std::uint64_t deleted = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), deleted));
    EXPECT_NE(imported, deleted);
    std::uint64_t stable = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), stable));
    EXPECT_EQ(deleted, stable);
}

TEST_F(ManagedScriptBuildTest, ImportDirectoryDescriptorsAreBoundedAndCaseInsensitive)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Game")));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Imports/Nested")));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Imports/obj")));
    const auto project = File("Game/Fixture.Scripts.csproj");
    ASSERT_TRUE(util::FileSystem::WriteText(project, "<Project />"));
    const std::string directory = util::FileSystem::PathToUtf8(File("Imports"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("inputs.txt"), "D|" + directory + "\n"));
    std::uint64_t before = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), before));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Imports/Nested/New.props"), "<Project />"));
    std::uint64_t nested = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), nested));
    EXPECT_EQ(before, nested);
    ASSERT_TRUE(util::FileSystem::WriteText(File("Imports/NEW.PROPS"), "<Project />"));
    std::uint64_t direct = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), direct));
    EXPECT_NE(nested, direct);
    ASSERT_TRUE(util::FileSystem::WriteText(File("inputs.txt"), "R|" + directory + "\n"));
    std::uint64_t recursive = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), recursive));
    EXPECT_NE(direct, recursive);
    ASSERT_TRUE(util::FileSystem::WriteText(File("Imports/obj/Generated.targets"), "<Project />"));
    std::uint64_t generated = 0;
    ASSERT_TRUE(editor::ComputeManagedSourceFingerprint(project, File("inputs.txt"), generated));
    EXPECT_EQ(recursive, generated);
}

TEST_F(ManagedScriptBuildTest, SourceChangeAlsoTerminatesAPlayDeferredRequest)
{
    editor::ManagedBuildHistory history;
    const std::string oldRequest = history.Queue(200, false, 4);
    history.BeginCompile(oldRequest, "deferred-candidate");
    ASSERT_TRUE(history.CompleteCompile(oldRequest, 200, true, true, 0, ""));
    history.Transition(oldRequest, editor::ManagedBuildState::DEFERRED);
    const std::string replacement = history.Queue(201, true, 4);
    EXPECT_EQ(history.Find(oldRequest)->state, editor::ManagedBuildState::SUPERSEDED);
    history.Transition(oldRequest, editor::ManagedBuildState::APPLYING);
    history.MarkApplied(oldRequest, 5);
    EXPECT_EQ(history.Find(oldRequest)->appliedGeneration, 0u);
    EXPECT_EQ(history.Find(replacement)->state, editor::ManagedBuildState::QUEUED);
}

TEST_F(ManagedScriptBuildTest, CompileFailureKeepsPreviousGenerationAndStructuredCSharpDiagnostics)
{
    editor::ManagedBuildHistory history;
    const std::string request = history.Queue(300, false, 12);
    history.BeginCompile(request, "bad-candidate");
    const std::string log = "ERROR Assets/Scripts/Enemy Component.cs:23:7 CS1002 Expected semicolon\r\n"
        "WARN Assets/Scripts/Enemy Component.cs:11 CS0649 Field never assigned\nRESULT failed errors=1 warnings=1\n";
    EXPECT_FALSE(history.CompleteCompile(request, 300, false, false, 1, log));
    const editor::ManagedBuildRequest* result = history.Find(request);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->state, editor::ManagedBuildState::FAILED);
    EXPECT_EQ(result->previousGeneration, 12u);
    EXPECT_EQ(result->appliedGeneration, 0u);
    EXPECT_EQ(result->errorCode, "COMPILE_FAILED");
    ASSERT_EQ(result->diagnostics.size(), 2u);
    EXPECT_EQ(result->diagnostics[0].file, "Assets/Scripts/Enemy Component.cs");
    EXPECT_EQ(result->diagnostics[0].line, 23);
    EXPECT_EQ(result->diagnostics[0].column, 7);
    EXPECT_EQ(result->diagnostics[0].code, "CS1002");
    EXPECT_FALSE(result->diagnostics[0].warning);
    EXPECT_TRUE(result->diagnostics[1].warning);
    EXPECT_EQ(result->diagnostics[1].line, 11);
    history.MarkApplied(request, 13);
    EXPECT_EQ(result->state, editor::ManagedBuildState::FAILED);
}

TEST_F(ManagedScriptBuildTest, RetainsEveryParsedDiagnosticAndReportsTheRetainedLogTail)
{
    editor::ManagedBuildHistory history;
    const std::string request = history.Queue(301, false, 12);
    history.BeginCompile(request, "many-diagnostics");
    std::string log(70u * 1024u, 'x');
    log.push_back('\n');
    constexpr std::size_t diagnosticCount = 137;
    for (std::size_t index = 0; index < diagnosticCount; ++index)
        log += "ERROR Assets/Scripts/Many.cs:" + std::to_string(index + 1) + ":2 CS0001 Diagnostic " + std::to_string(index) + "\n";
    log += "RESULT failed errors=137 warnings=0 log=build/agent/full-output.txt\n";
    EXPECT_FALSE(history.CompleteCompile(request, 301, false, false, 1, log));
    const editor::ManagedBuildRequest* result = history.Find(request);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->diagnostics.size(), diagnosticCount);
    EXPECT_EQ(result->diagnostics.back().line, static_cast<int>(diagnosticCount));
    EXPECT_EQ(result->logOriginalBytes, log.size());
    EXPECT_EQ(result->logRetainedBytes, result->log.size());
    EXPECT_LT(result->logRetainedBytes, result->logOriginalBytes);
    EXPECT_EQ(result->logFilePath, "build/agent/full-output.txt");
    EXPECT_NE(result->log.find("[earlier output truncated]"), std::string::npos);
}

TEST_F(ManagedScriptBuildTest, SharedBuildContentionRetriesOneRequestInsteadOfReportingACompileFailure)
{
    editor::ManagedBuildHistory history;
    const std::string request = history.Queue(400, false, 2);
    EXPECT_EQ(history.Queue(400, true, 2), request);
    history.BeginCompile(request, "busy-attempt");
    EXPECT_FALSE(history.CompleteCompile(request, 400, false, false, 2, "RESULT busy running=1\n"));
    EXPECT_EQ(history.Find(request)->state, editor::ManagedBuildState::QUEUED);
    EXPECT_FALSE(editor::ManagedBuildHistory::IsTerminal(history.Find(request)->state));
    EXPECT_EQ(history.Queue(400, false, 2), request);
    history.BeginCompile(request, "second-attempt");
    ASSERT_TRUE(history.CompleteCompile(request, 400, true, true, 0, ""));
    EXPECT_EQ(history.History().size(), 1u);
    EXPECT_EQ(history.Find(request)->candidateId, "second-attempt");
}

TEST_F(ManagedScriptBuildTest, GeneratedMetadataChangesEffectiveVersionWithoutChangingTheRequestedVersion)
{
    editor::ManagedBuildHistory history;
    const std::string request = history.Queue(500, false, 8);
    history.BeginCompile(request, "metadata-candidate");
    ASSERT_TRUE(history.CompleteCompile(request, 500, true, true, 0, ""));
    history.SetEffectiveFingerprint(request, 501);
    EXPECT_EQ(history.Queue(501, true, 8), request);
    EXPECT_EQ(history.Find(request)->sourceFingerprint, 500u);
    EXPECT_EQ(history.Find(request)->effectiveSourceFingerprint, 501u);
    history.Transition(request, editor::ManagedBuildState::APPLYING);
    history.MarkApplied(request, 9);
    EXPECT_EQ(history.Find(request)->state, editor::ManagedBuildState::APPLIED);
}

TEST_F(ManagedScriptBuildTest, EvaluatedGraphExpansionPreservesRootInputVersionButSameMtimeRootEditsDoNot)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    const auto project = File("Fixture.Scripts.csproj");
    const auto source = File("Assets/Scripts/Probe.cs");
    const auto manifest = File("inputs.txt");
    ASSERT_TRUE(util::FileSystem::WriteText(project, "<Project />"));
    ASSERT_TRUE(util::FileSystem::WriteText(source, "class Probe { int first; }"));
    testkit::TempDir external{"managed-cold-dependency"}; ASSERT_TRUE(external.IsValid());
    const auto referenced = external.File("Dependency.csproj");
    const auto referencedSource = external.File("Dependency.cs");
    ASSERT_TRUE(util::FileSystem::WriteText(referenced, "<Project />"));
    ASSERT_TRUE(util::FileSystem::WriteText(referencedSource, "class Dependency { int first; }"));
    editor::ManagedSourceSnapshot cold;
    ASSERT_TRUE(editor::ComputeManagedSourceSnapshot(project, manifest, cold)); EXPECT_FALSE(cold.hasInputManifest);
    ASSERT_TRUE(util::FileSystem::WriteText(manifest, "P|" + util::FileSystem::PathToUtf8(project) + "\nP|" + util::FileSystem::PathToUtf8(referenced) + "\n"));
    editor::ManagedSourceSnapshot graph;
    ASSERT_TRUE(editor::ComputeManagedSourceSnapshot(project, manifest, graph)); EXPECT_TRUE(graph.hasInputManifest);
    EXPECT_NE(graph.fingerprint, cold.fingerprint); EXPECT_EQ(graph.rootInputFingerprint, cold.rootInputFingerprint);
    const auto referencedTime = std::filesystem::last_write_time(referencedSource);
    ASSERT_TRUE(util::FileSystem::WriteText(referencedSource, "class Dependency { int other; }"));
    std::filesystem::last_write_time(referencedSource, referencedTime);
    editor::ManagedSourceSnapshot dependencyChanged;
    ASSERT_TRUE(editor::ComputeManagedSourceSnapshot(project, manifest, dependencyChanged));
    EXPECT_NE(dependencyChanged.fingerprint, graph.fingerprint);
    EXPECT_EQ(dependencyChanged.rootInputFingerprint, graph.rootInputFingerprint);
    const auto sourceTime = std::filesystem::last_write_time(source);
    ASSERT_TRUE(util::FileSystem::WriteText(source, "class Probe { int other; }"));
    std::filesystem::last_write_time(source, sourceTime);
    editor::ManagedSourceSnapshot rootChanged;
    ASSERT_TRUE(editor::ComputeManagedSourceSnapshot(project, manifest, rootChanged));
    EXPECT_NE(rootChanged.rootInputFingerprint, graph.rootInputFingerprint);
    EXPECT_NE(rootChanged.fingerprint, dependencyChanged.fingerprint);
    std::error_code error; ASSERT_TRUE(std::filesystem::remove(source, error)); ASSERT_FALSE(error);
    editor::ManagedSourceSnapshot deleted;
    ASSERT_TRUE(editor::ComputeManagedSourceSnapshot(project, manifest, deleted));
    EXPECT_NE(deleted.rootInputFingerprint, rootChanged.rootInputFingerprint);
}

TEST_F(ManagedScriptBuildTest, ColdDependencyDiscoveryRetriesTheOriginalRequestExactlyOnce)
{
    editor::ManagedBuildHistory history;
    editor::ManagedSourceSnapshot captured;
    captured.valid = true; captured.fingerprint = 10; captured.rootInputFingerprint = 20;
    editor::ManagedSourceSnapshot evaluated = captured;
    evaluated.hasInputManifest = true; evaluated.fingerprint = 30;
    const auto request = history.Queue(captured.fingerprint, false, 7);
    const auto queuedAt = history.Find(request)->queuedAt;
    history.BeginCompile(request, "generation-discovery");
    ASSERT_TRUE(history.CanRetryDiscoveredGraph(request, captured, evaluated));
    ASSERT_TRUE(history.RetryDiscoveredGraph(request, captured, evaluated, true, true, 0, "RESULT success\n"));
    const auto* retried = history.Find(request);
    ASSERT_NE(retried, nullptr);
    EXPECT_EQ(retried->state, editor::ManagedBuildState::QUEUED);
    EXPECT_EQ(retried->queuedAt, queuedAt);
    EXPECT_EQ(retried->errorCode, "INPUT_GRAPH_DISCOVERED");
    EXPECT_EQ(retried->inputDiscoveryRetries, 1U);
    EXPECT_EQ(retried->sourceFingerprint, 10U); EXPECT_EQ(retried->effectiveSourceFingerprint, 30U);
    EXPECT_TRUE(retried->candidateId.empty()); EXPECT_EQ(retried->appliedGeneration, 0U);
    EXPECT_EQ(history.Queue(30, true, 7), request);
    EXPECT_FALSE(history.CanRetryDiscoveredGraph(request, captured, evaluated));
    history.BeginCompile(request, "generation-full-graph");
    ASSERT_TRUE(history.CompleteCompile(request, 30, true, true, 0, "RESULT success\n"));
    history.MarkApplied(request, 8);
    EXPECT_EQ(history.Find(request)->state, editor::ManagedBuildState::APPLIED);
    EXPECT_EQ(history.Find(request)->appliedGeneration, 8U);
}

TEST_F(ManagedScriptBuildTest, ColdGraphDiscoveryCannotHideKnownRootEditsOrFailedReceipts)
{
    editor::ManagedBuildHistory history;
    editor::ManagedSourceSnapshot captured;
    captured.valid = true; captured.fingerprint = 10; captured.rootInputFingerprint = 20;
    editor::ManagedSourceSnapshot changed = captured;
    changed.hasInputManifest = true; changed.fingerprint = 30; changed.rootInputFingerprint = 21;
    const auto request = history.Queue(10, false, 7); history.BeginCompile(request, "generation-discovery");
    EXPECT_FALSE(history.CanRetryDiscoveredGraph(request, captured, changed));
    EXPECT_FALSE(history.RetryDiscoveredGraph(request, captured, changed, true, true, 0, "RESULT success\n"));
    EXPECT_FALSE(history.CompleteCompile(request, changed.fingerprint, true, true, 0, "RESULT success\n"));
    EXPECT_EQ(history.Find(request)->state, editor::ManagedBuildState::SUPERSEDED);
    const auto next = history.Queue(40, false, 7); captured.fingerprint = 40;
    changed.rootInputFingerprint = captured.rootInputFingerprint;
    EXPECT_FALSE(history.RetryDiscoveredGraph(next, captured, changed, false, true, 1, "ERROR managed FBZZ0102 changed\n"));
    EXPECT_EQ(history.Find(next)->inputDiscoveryRetries, 0U);
    changed.valid = false;
    EXPECT_FALSE(history.CanRetryDiscoveredGraph(next, captured, changed));
}

TEST_F(ManagedScriptBuildTest, ADiscoveredGraphStillRejectsLaterDependencyChanges)
{
    editor::ManagedBuildHistory history;
    editor::ManagedSourceSnapshot captured;
    captured.valid = true; captured.fingerprint = 10; captured.rootInputFingerprint = 20;
    editor::ManagedSourceSnapshot evaluated = captured;
    evaluated.hasInputManifest = true; evaluated.fingerprint = 30;
    const auto request = history.Queue(10, false, 7); history.BeginCompile(request, "generation-discovery");
    ASSERT_TRUE(history.RetryDiscoveredGraph(request, captured, evaluated, true, true, 0, "RESULT success\n"));
    history.BeginCompile(request, "generation-full-graph");
    EXPECT_FALSE(history.CompleteCompile(request, 31, true, true, 0, "RESULT success\n"));
    EXPECT_EQ(history.Find(request)->state, editor::ManagedBuildState::SUPERSEDED);
    EXPECT_EQ(history.Find(request)->inputDiscoveryRetries, 1U);
    EXPECT_EQ(history.Find(request)->appliedGeneration, 0U);
}

TEST_F(ManagedScriptBuildTest, BoundedHistoryDoesNotResolveAnExpiredRequestAsTheLatest)
{
    editor::ManagedBuildHistory history;
    const std::string oldest = history.Queue(600, true, 1);
    for (std::size_t index = 1; index <= editor::ManagedBuildHistory::MAX_HISTORY; ++index)
        static_cast<void>(history.Queue(600 + index, true, 1));
    EXPECT_EQ(history.History().size(), editor::ManagedBuildHistory::MAX_HISTORY);
    EXPECT_EQ(history.Find(oldest), nullptr);
    ASSERT_NE(history.Latest(), nullptr);
    EXPECT_NE(history.Latest()->managedRequestId, oldest);
}

TEST_F(ManagedScriptBuildTest, MissingManagedServiceAndUnknownRequestAreExplicitErrors)
{
    editor::ai::JsonValue run = editor::ai::JsonValue::MakeObject();
    run.Set("t", editor::ai::JsonValue("build.run"));
    run.Set("target", editor::ai::JsonValue("managed"));
    const editor::ai::JsonValue rejected = Send("command", std::move(run));
    ASSERT_NE(rejected.Find("error"), nullptr);
    EXPECT_EQ(rejected.Find("error")->Find("code")->AsString(), "MANAGED_BUILD_UNAVAILABLE");
    EXPECT_EQ(m_undo.GetUndoDescription(), "");
    editor::ai::JsonValue status = editor::ai::JsonValue::MakeObject();
    status.Set("t", editor::ai::JsonValue("build.status"));
    status.Set("managedRequestId", editor::ai::JsonValue("managed:99999999"));
    const editor::ai::JsonValue unknown = Send("query", std::move(status));
    ASSERT_NE(unknown.Find("error"), nullptr);
    EXPECT_EQ(unknown.Find("error")->Find("code")->AsString(), "MANAGED_REQUEST_NOT_FOUND");
}

TEST_F(ManagedScriptBuildTest, DefaultManagedBuildOpReturnsTheRequestThatStatusObserves)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project Sdk=\"Microsoft.NET.Sdk\" />"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Assets/Scripts/Probe.cs"), "class Probe {}\n"));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    editor::ManagedScriptEditor service(std::make_unique<ManualManagedSourceWatcher>());
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable()) << service.AvailabilityDiagnostic();
    Context().managedScripts = &service;
    editor::ai::JsonValue run = editor::ai::JsonValue::MakeObject();
    run.Set("t", editor::ai::JsonValue("build.run"));
    const editor::ai::JsonValue requested = Send("command", std::move(run));
    ASSERT_NE(requested.Find("result"), nullptr) << editor::ai::SerializeJson(requested);
    const std::string requestId = requested.Find("result")->Find("managedRequestId")->AsString();
    const std::string fingerprint = requested.Find("result")->Find("sourceFingerprint")->AsString();
    EXPECT_EQ(fingerprint.size(), 16u);
    editor::ai::JsonValue query = editor::ai::JsonValue::MakeObject();
    query.Set("t", editor::ai::JsonValue("build.status"));
    query.Set("managedRequestId", editor::ai::JsonValue(requestId));
    const editor::ai::JsonValue response = Send("query", std::move(query));
    ASSERT_NE(response.Find("result"), nullptr) << editor::ai::SerializeJson(response);
    const editor::ai::JsonValue* managed = response.Find("result")->Find("managed");
    ASSERT_NE(managed, nullptr);
    const editor::ai::JsonValue* record = managed->Find("request");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->Find("managedRequestId")->AsString(), requestId);
    EXPECT_EQ(record->Find("sourceFingerprint")->AsString(), fingerprint);
    EXPECT_EQ(record->Find("state")->AsString(), "scanning");
    EXPECT_FALSE(record->Find("terminal")->AsBool());
    EXPECT_TRUE(managed->Find("busy")->AsBool());
    /// @note 他のビルドを実行中として監視だけを進め、このテストは子プロセスを開始しない。
    service.Tick(nullptr, true, 0.5f, true);
    EXPECT_EQ(service.Builds().Find(requestId)->state, editor::ManagedBuildState::QUEUED);
    service.Shutdown(nullptr);
    EXPECT_EQ(service.Builds().Find(requestId)->state, editor::ManagedBuildState::FAILED);
    EXPECT_EQ(service.Builds().Find(requestId)->errorCode, "MANAGED_BUILD_SHUTDOWN");
    Context().managedScripts = nullptr;
}

TEST_F(ManagedScriptBuildTest, AutomaticSourceMonitoringSupersedesTheExplicitRequestBeforeStartingAnyBuild)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project Sdk=\"Microsoft.NET.Sdk\" />"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Assets/Scripts/Probe.cs"), "class Probe {}\n"));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    editor::ManagedScriptEditor service(std::make_unique<ManualManagedSourceWatcher>());
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable()) << service.AvailabilityDiagnostic();
    std::string explicitRequest;
    ASSERT_TRUE(service.RequestBuild(explicitRequest));
    service.Tick(nullptr, false, 0.0f, true);
    const std::uint64_t requestedVersion = service.Builds().Find(explicitRequest)->sourceFingerprint;
    ASSERT_TRUE(util::FileSystem::WriteText(File("Assets/Scripts/Probe.cs"), "class Probe { int changed; }\n"));
    /// @note コンパイルを保留したまま自動監視だけを進め、明示要求の版固定を検証する。
    service.Tick(nullptr, false, 0.5f, true);
    const editor::ManagedBuildRequest* old = service.Builds().Find(explicitRequest);
    ASSERT_NE(old, nullptr);
    EXPECT_EQ(old->state, editor::ManagedBuildState::SUPERSEDED);
    EXPECT_EQ(old->sourceFingerprint, requestedVersion);
    const editor::ManagedBuildRequest* replacement = service.Builds().Latest();
    ASSERT_NE(replacement, nullptr);
    EXPECT_TRUE(replacement->automatic);
    EXPECT_NE(replacement->managedRequestId, explicitRequest);
    EXPECT_NE(replacement->sourceFingerprint, requestedVersion);
    EXPECT_EQ(replacement->state, editor::ManagedBuildState::QUEUED);
    std::string joined;
    ASSERT_TRUE(service.RequestBuild(joined));
    EXPECT_EQ(joined, replacement->managedRequestId);
    service.Shutdown(nullptr);
}

TEST_F(ManagedScriptBuildTest, SourceRequestWaitsForItsRefreshTicketAndNeverStartsFromAnOlderSnapshot)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Assets/Scripts/Probe.cs"), "class Probe {}"));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    auto watcher = std::make_unique<ManualManagedSourceWatcher>();
    ManualManagedSourceWatcher* control = watcher.get();
    editor::ManagedScriptEditor service(std::move(watcher));
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable());
    std::string request;
    ASSERT_TRUE(service.RequestBuild(request));
    control->hold = true;
    service.Tick(nullptr, false, 1.0f, false);
    EXPECT_EQ(service.Builds().Find(request)->state, editor::ManagedBuildState::SCANNING);
    EXPECT_EQ(service.Builds().Find(request)->candidateId, "");
    EXPECT_EQ(service.Builds().Find(request)->sourceFingerprint, 0u);
    control->hold = false;
    control->usePublishedRevision = true;
    control->publishedRevision = control->requestedRevision - 1;
    service.Tick(nullptr, false, 1.0f, false);
    EXPECT_EQ(service.Builds().Find(request)->state, editor::ManagedBuildState::SCANNING);
    EXPECT_EQ(service.Builds().Find(request)->candidateId, "");
    control->usePublishedRevision = false;
    service.Tick(nullptr, false, 0.0f, true);
    EXPECT_EQ(service.Builds().Find(request)->state, editor::ManagedBuildState::QUEUED);
    EXPECT_NE(service.Builds().Find(request)->sourceFingerprint, 0u);
    service.Shutdown(nullptr);
}

TEST_F(ManagedScriptBuildTest, StopRequiresExplicitEditApplyForScanningAndQueuedPlayRequests)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Assets/Scripts/Probe.cs"), "class Probe {}"));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    for (const bool queued : {false, true}) {
        auto watcher = std::make_unique<ManualManagedSourceWatcher>();
        ManualManagedSourceWatcher* control = watcher.get();
        editor::ManagedScriptEditor service(std::move(watcher));
        service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
        ASSERT_TRUE(service.IsAvailable());
        service.BeginPlaySession();
        std::string request;
        ASSERT_TRUE(service.RequestBuild(request));
        if (queued) service.Tick(nullptr, false, 0.0f, true);
        ASSERT_EQ(service.Builds().Find(request)->state,
            queued ? editor::ManagedBuildState::QUEUED : editor::ManagedBuildState::SCANNING);
        control->hold = true;
        service.EndPlaySession();
        EXPECT_TRUE(service.RequiresExplicitEditApply());
        service.Tick(nullptr, true, 1.0f, true);
        EXPECT_TRUE(service.RequiresExplicitEditApply());
        EXPECT_EQ(service.Builds().Find(request)->appliedGeneration, 0U);
        EXPECT_EQ(service.AppliedGeneration(), 0U);
        const auto stopRevision = control->requestedRevision;
        control->hold = false;
        control->usePublishedRevision = true;
        control->publishedRevision = stopRevision - 1;
        service.Tick(nullptr, true, 0.0f, true);
        EXPECT_TRUE(service.IsBusy());
        EXPECT_TRUE(service.RequiresExplicitEditApply());
        EXPECT_EQ(service.AppliedGeneration(), 0U);
        EXPECT_EQ(service.AvailabilityDiagnostic(),
            "Waiting for a fresh post-Stop source snapshot before automatic Edit apply.");
        control->usePublishedRevision = false;
        service.Tick(nullptr, true, 0.0f, true);
        EXPECT_TRUE(service.RequiresExplicitEditApply());
        EXPECT_EQ(service.Builds().Find(request)->appliedGeneration, 0U);
        EXPECT_TRUE(service.AvailabilityDiagnostic().empty());
        std::string joined;
        ASSERT_TRUE(service.RequestBuild(joined));
        EXPECT_EQ(joined, request);
        EXPECT_FALSE(service.RequiresExplicitEditApply());
        service.Shutdown(nullptr);
    }
}

TEST_F(ManagedScriptBuildTest, StopRequiresExplicitEditApplyWhileCompiledPlayOutputAwaitsVerification)
{
    editor::ManagedScriptPublicationState publication;
    editor::ManagedSourceSnapshot source;
    source.fingerprint = 701;
    source.revision = 1;
    source.valid = true;
    publication.BeginPlay(source);
    publication.AwaitCompileVerification(2);
    ASSERT_TRUE(publication.HasPendingVerification());

    publication.EndPlay(false, false, 3);

    EXPECT_TRUE(publication.RequiresExplicitEditApply());
    EXPECT_TRUE(publication.HasCompileVerification());
    EXPECT_FALSE(publication.CanAutomaticallyApply(true));
    EXPECT_FALSE(publication.TryCompleteCompileVerification(1));
    EXPECT_TRUE(publication.TryCompleteCompileVerification(2));
    EXPECT_TRUE(publication.HasPendingVerification());
    source.revision = 3;
    EXPECT_EQ(publication.VerifyStoppedSource(source), editor::ManagedStopVerification::UNCHANGED);
    EXPECT_FALSE(publication.HasPendingVerification());
    EXPECT_FALSE(publication.CanAutomaticallyApply(true));
    EXPECT_TRUE(publication.RequestExplicitApply());
    EXPECT_TRUE(publication.CanAutomaticallyApply(true));

    editor::ManagedScriptEditor unchanged;
    unchanged.BeginPlaySession();
    unchanged.EndPlaySession();
    EXPECT_FALSE(unchanged.RequiresExplicitEditApply());
}

TEST_F(ManagedScriptBuildTest, StopVerifiesUnobservedPlaySourceEditsBeforeAutomaticEditApply)
{
    editor::ManagedScriptPublicationState publication;
    editor::ManagedSourceSnapshot source;
    source.fingerprint = 701;
    source.revision = 1;
    source.valid = true;
    publication.BeginPlay(source);
    publication.EndPlay(false, false, 2);
    ASSERT_TRUE(publication.HasPendingVerification());
    EXPECT_FALSE(publication.RequiresExplicitEditApply());
    EXPECT_FALSE(publication.CanAutomaticallyApply(true));
    source.revision = 2;
    EXPECT_EQ(publication.VerifyStoppedSource(source), editor::ManagedStopVerification::UNCHANGED);
    EXPECT_FALSE(publication.HasPendingVerification());
    EXPECT_TRUE(publication.CanAutomaticallyApply(true));

    publication.BeginPlay(source);
    publication.EndPlay(false, false, 3);
    source.fingerprint = 702;

    EXPECT_EQ(publication.VerifyStoppedSource(source), editor::ManagedStopVerification::WAITING);
    EXPECT_TRUE(publication.HasPendingVerification());
    EXPECT_FALSE(publication.RequiresExplicitEditApply());
    EXPECT_FALSE(publication.CanAutomaticallyApply(true));
    source.revision = 3;
    source.valid = false;
    EXPECT_EQ(publication.VerifyStoppedSource(source), editor::ManagedStopVerification::WAITING);
    EXPECT_TRUE(publication.HasPendingVerification());
    EXPECT_FALSE(publication.CanAutomaticallyApply(true));
    source.valid = true;
    EXPECT_EQ(publication.VerifyStoppedSource(source), editor::ManagedStopVerification::CHANGED);
    EXPECT_FALSE(publication.HasPendingVerification());
    EXPECT_TRUE(publication.RequiresExplicitEditApply());
    EXPECT_FALSE(publication.CanAutomaticallyApply(true));
}

class ManagedGenerationCleanupTest : public ManagedScriptBuildTest {
protected:
    void SetUp() override
    {
        ManagedScriptBuildTest::SetUp();
        TaskSystem::Init(1);
    }
    void TearDown() override
    {
        TaskSystem::Shutdown();
        ManagedScriptBuildTest::TearDown();
    }
    void DrainCleanup(editor::ManagedScriptEditor& service)
    {
        TaskSystem::Submit([] {}).get();
        service.Tick(nullptr, true, 0.0f, true);
        TaskSystem::Submit([] {}).get();
        service.Tick(nullptr, true, 0.0f, true);
    }
};

TEST_F(ManagedGenerationCleanupTest, GenerationCleanupDoesNotWaitForDiskAndRemovesOnlyRecordedOutputsInsideTheExactOutputRoot)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    const std::filesystem::path output = File("Library/ManagedScripts") /
        std::filesystem::path(FBZZ_MANAGED_HOST_TEST_DIRECTORY).parent_path().filename();
    const std::filesystem::path retired = output / "generation-1-1";
    const std::filesystem::path pending = output / "generation-1-1.pending";
    const std::filesystem::path unowned = output / "generation-9-9";
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(retired));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(pending));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(unowned));
    const std::string ledger = util::FileSystem::PathToUtf8(retired) + '\n' +
        util::FileSystem::PathToUtf8(pending) + '\n' + util::FileSystem::PathToUtf8(File("Assets")) + '\n';
    ASSERT_TRUE(util::FileSystem::WriteText(output / "owned-generations.txt", ledger));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    auto watcher = std::make_unique<ManualManagedSourceWatcher>();
    watcher->hold = true;
    editor::ManagedScriptEditor service(std::move(watcher));
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable());
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
    auto blocker = TaskSystem::Submit([&] { entered.release(); resume.acquire(); });
    entered.acquire();
    const bool previousRecording = profiler::Profiler::IsEnabled();
    const bool previousRequested = profiler::Profiler::IsRecordingRequested();
    profiler::Profiler::SetEnabled(true);
    profiler::Profiler::BeginFrame();
    service.Tick(nullptr, false, 1.0f, true);
    profiler::Profiler::EndFrame();
    const auto cleanupRecorded = [] {
        const auto& records = profiler::Profiler::GetLastFrameRecords();
        return std::any_of(records.begin(), records.end(), [](const auto& record) {
            return std::string_view(record.name) == "ManagedHotReload::CleanupGenerations";
        });
    };
    EXPECT_FALSE(cleanupRecorded());
    EXPECT_TRUE(util::FileSystem::Exists(retired));
    EXPECT_TRUE(util::FileSystem::Exists(pending));
    profiler::Profiler::BeginFrame();
    service.Tick(nullptr, true, 1.0f, true);
    profiler::Profiler::EndFrame();
    EXPECT_FALSE(cleanupRecorded());
    EXPECT_TRUE(util::FileSystem::Exists(retired));
    EXPECT_TRUE(util::FileSystem::Exists(pending));
    service.Tick(nullptr, true, 10.0f, true);
    EXPECT_EQ(TaskSystem::GetStats().queuedCount, 1);
    resume.release();
    blocker.get();
    /// @note 単一 worker の FIFO 後続タスクで削除完了を確定し、実時間や sleep で推測しない。
    DrainCleanup(service);
    EXPECT_FALSE(util::FileSystem::Exists(retired));
    EXPECT_FALSE(util::FileSystem::Exists(pending));
    EXPECT_TRUE(util::FileSystem::Exists(unowned));
    EXPECT_TRUE(util::FileSystem::Exists(File("Assets")));
    /// @note 保存された所有世代が空になれば、待機フレームに削除・再確認の区間を記録しない。
    for (int idleFrame = 0; idleFrame < 3; ++idleFrame) {
        profiler::Profiler::BeginFrame();
        service.Tick(nullptr, false, 1.0f, true);
        profiler::Profiler::EndFrame();
        EXPECT_FALSE(cleanupRecorded());
    }
    profiler::Profiler::SetEnabled(previousRecording);
    profiler::Profiler::RequestRecording(previousRequested);
    service.Shutdown(nullptr);
}

TEST_F(ManagedGenerationCleanupTest, LockedGenerationRemainsOwnedAndIsRetriedAfterTheFileCloses)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    const std::filesystem::path output = File("Library/ManagedScripts") /
        std::filesystem::path(FBZZ_MANAGED_HOST_TEST_DIRECTORY).parent_path().filename();
    const std::filesystem::path retired = output / "generation-2-1";
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(retired));
    ASSERT_TRUE(util::FileSystem::WriteText(retired / "Locked.dll", "fixture"));
    const std::string ledger = util::FileSystem::PathToUtf8(retired) + '\n';
    ASSERT_TRUE(util::FileSystem::WriteText(output / "owned-generations.txt", ledger));
    std::ifstream locked(retired / "Locked.dll", std::ios::binary);
    ASSERT_TRUE(locked.is_open());
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    auto watcher = std::make_unique<ManualManagedSourceWatcher>();
    watcher->hold = true;
    editor::ManagedScriptEditor service(std::move(watcher));
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable());

    service.Tick(nullptr, true, 1.0f, true);
    DrainCleanup(service);
    EXPECT_TRUE(util::FileSystem::Exists(retired / "Locked.dll"));
    std::string saved;
    ASSERT_TRUE(util::FileSystem::ReadText(output / "owned-generations.txt", saved));
    EXPECT_EQ(saved, ledger);

    locked.close();
    service.Tick(nullptr, true, 1.0f, true);
    DrainCleanup(service);
    EXPECT_FALSE(util::FileSystem::Exists(retired));
    ASSERT_TRUE(util::FileSystem::ReadText(output / "owned-generations.txt", saved));
    EXPECT_TRUE(saved.empty());
    service.Shutdown(nullptr);
}

TEST_F(ManagedGenerationCleanupTest, ShutdownDrainsQueuedCleanupBeforeReturning)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    const std::filesystem::path output = File("Library/ManagedScripts") /
        std::filesystem::path(FBZZ_MANAGED_HOST_TEST_DIRECTORY).parent_path().filename();
    const std::filesystem::path retired = output / "generation-3-1";
    const std::filesystem::path unowned = output / "generation-3-2";
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(retired));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(unowned));
    ASSERT_TRUE(util::FileSystem::WriteText(retired / "Retired.dll", "fixture"));
    ASSERT_TRUE(util::FileSystem::WriteText(output / "owned-generations.txt",
        util::FileSystem::PathToUtf8(retired) + '\n'));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    auto watcher = std::make_unique<ManualManagedSourceWatcher>();
    watcher->hold = true;
    editor::ManagedScriptEditor service(std::move(watcher));
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable());

    service.Tick(nullptr, true, 1.0f, true);
    service.Shutdown(nullptr);

    EXPECT_FALSE(util::FileSystem::Exists(retired));
    EXPECT_TRUE(util::FileSystem::Exists(unowned));
    std::string saved;
    ASSERT_TRUE(util::FileSystem::ReadText(output / "owned-generations.txt", saved));
    EXPECT_TRUE(saved.empty());
}

TEST_F(ManagedScriptBuildTest, ContentReceiptFailureRetriesTheRequestAndPreservesThePreviousGeneration)
{
    editor::ManagedBuildHistory history;
    const std::string request = history.Queue(450, false, 3);
    history.BeginCompile(request, "changed-during-build");
    EXPECT_FALSE(history.CompleteCompile(request, 450, false, false, 1,
        "ERROR managed FBZZ0102 Managed inputs changed during compilation\n"));
    ASSERT_NE(history.Find(request), nullptr);
    EXPECT_EQ(history.Find(request)->state, editor::ManagedBuildState::QUEUED);
    EXPECT_EQ(history.Find(request)->previousGeneration, 3u);
    EXPECT_EQ(history.Find(request)->appliedGeneration, 0u);
    EXPECT_EQ(history.Find(request)->errorCode, "SOURCE_CHANGED_DURING_BUILD");
}

TEST_F(ManagedScriptBuildTest, ProjectAddedAfterInitializationBecomesAvailableWithoutRestart)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scripts")));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    editor::ManagedScriptEditor service(std::make_unique<ManualManagedSourceWatcher>());
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    EXPECT_FALSE(service.IsAvailable());
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    service.Tick(nullptr, false, 0.5f, true);
    EXPECT_TRUE(service.IsAvailable());
    service.Shutdown(nullptr);
}

TEST_F(ManagedScriptBuildTest, DefaultCacheScopeKeepsLegacyManifestRoot)
{
    ScopedManagedCacheEnvironment environment(nullptr);
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    auto watcher = std::make_unique<ManualManagedSourceWatcher>();
    auto* control = watcher.get();
    editor::ManagedScriptEditor service(std::move(watcher));
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable()) << service.AvailabilityDiagnostic();
    const auto expected = File("Library/ManagedScripts") /
        std::filesystem::path(FBZZ_MANAGED_HOST_TEST_DIRECTORY).parent_path().filename() / "inputs.txt";
    EXPECT_EQ(control->startedManifest, expected);
    service.Shutdown(nullptr);
}

TEST_F(ManagedScriptBuildTest, ExplicitCacheScopeUsesSeparateManifestAndPreservesLegacyReceipt)
{
    ScopedManagedCacheEnvironment environment(L"world-ui-preview");
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    const auto legacy = File("Library/ManagedScripts") /
        std::filesystem::path(FBZZ_MANAGED_HOST_TEST_DIRECTORY).parent_path().filename();
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(legacy));
    ASSERT_TRUE(util::FileSystem::WriteText(legacy / "inputs.txt", "legacy inputs"));
    ASSERT_TRUE(util::FileSystem::WriteText(legacy / "current.txt", "legacy-generation"));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    auto watcher = std::make_unique<ManualManagedSourceWatcher>();
    auto* control = watcher.get();
    editor::ManagedScriptEditor service(std::move(watcher));
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable()) << service.AvailabilityDiagnostic();
    EXPECT_EQ(control->startedManifest, legacy / "world-ui-preview" / "inputs.txt");
    std::string value;
    ASSERT_TRUE(util::FileSystem::ReadText(legacy / "inputs.txt", value));
    EXPECT_EQ(value, "legacy inputs");
    ASSERT_TRUE(util::FileSystem::ReadText(legacy / "current.txt", value));
    EXPECT_EQ(value, "legacy-generation");
    service.Shutdown(nullptr);
}

TEST_F(ManagedScriptBuildTest, InvalidCacheScopeNeverStartsTheSourceWatcher)
{
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    for (const std::wstring scope : {std::wstring(L".."), std::wstring(L"../outside"), std::wstring(L"a\\b"),
            std::wstring(L"K:\\outside"), std::wstring(L"a/b"), std::wstring(L"bad scope"),
            std::wstring(L"CON"), std::wstring(L"generation-1-1"), std::wstring(65, L'x')}) {
        ScopedManagedCacheEnvironment environment(scope.c_str());
        auto watcher = std::make_unique<ManualManagedSourceWatcher>();
        auto* control = watcher.get();
        editor::ManagedScriptEditor service(std::move(watcher));
        service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
        EXPECT_FALSE(service.IsAvailable());
        EXPECT_TRUE(control->startedManifest.empty());
        EXPECT_NE(service.AvailabilityDiagnostic().find("FBZZ_MANAGED_CACHE_SCOPE"), std::string::npos);
        service.Shutdown(nullptr);
    }
}

TEST_F(ManagedGenerationCleanupTest, ScopedCleanupNeverRemovesAnotherEditorsLegacyGeneration)
{
    ScopedManagedCacheEnvironment environment(L"world-ui-preview");
    ASSERT_TRUE(util::FileSystem::WriteText(File("Fixture.Scripts.csproj"), "<Project />"));
    const auto legacy = File("Library/ManagedScripts") /
        std::filesystem::path(FBZZ_MANAGED_HOST_TEST_DIRECTORY).parent_path().filename();
    const auto output = legacy / "world-ui-preview";
    const auto legacyGeneration = legacy / "generation-8-1";
    const auto scopedGeneration = output / "generation-9-1";
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(legacyGeneration));
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(scopedGeneration));
    const auto legacyLedger = util::FileSystem::PathToUtf8(legacyGeneration) + '\n';
    ASSERT_TRUE(util::FileSystem::WriteText(legacy / "owned-generations.txt", legacyLedger));
    ASSERT_TRUE(util::FileSystem::WriteText(output / "owned-generations.txt",
        legacyLedger + util::FileSystem::PathToUtf8(scopedGeneration) + '\n'));
    ASSERT_TRUE(scene::ManagedScriptRuntime::Instance().Initialize(FBZZ_MANAGED_HOST_TEST_DIRECTORY));
    auto watcher = std::make_unique<ManualManagedSourceWatcher>();
    watcher->hold = true;
    editor::ManagedScriptEditor service(std::move(watcher));
    service.Init(ProjectRoot(), FBZZ_SOURCE_DIR);
    ASSERT_TRUE(service.IsAvailable()) << service.AvailabilityDiagnostic();
    service.Tick(nullptr, true, 10.0f, true);
    DrainCleanup(service);
    EXPECT_FALSE(util::FileSystem::Exists(scopedGeneration));
    EXPECT_TRUE(util::FileSystem::Exists(legacyGeneration));
    std::string value;
    ASSERT_TRUE(util::FileSystem::ReadText(legacy / "owned-generations.txt", value));
    EXPECT_EQ(value, legacyLedger);
    service.Shutdown(nullptr);
}

} /// @note namespace fbzz::tests
