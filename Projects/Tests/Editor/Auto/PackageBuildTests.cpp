/// @file    PackageBuildTests.cpp
/// @brief   配布ビルドの共通入口・安全確認・要求寿命と描画非依存の進行を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-05
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Ai/EditorBusDispatcher.hpp>
#include <Editor/Ai/EditorBusProtocol.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/PackageBuildService.hpp>
#include <Editor/Panels/BuildSettingsPanel.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Engine/Profiler/ScriptProfiler.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <string>

namespace fbzz::tests {

class PackageBuildTest : public testkit::EditorFixture {
protected:
    void SetUp() override
    {
        EditorFixture::SetUp();
        Context().projectRoot = util::FileSystem::PathToUtf8(ProjectRoot());
        Context().projectBuildRoot = util::FileSystem::PathToUtf8(File("NativeBuild"));
        Context().engineRoot.clear();
        Context().projectSettings.game.project.defaultScene = "Assets/Scenes/Main.scene";
        Context().packageBuild = &m_package;
        Context().buildConsole = &m_console;
        Context().undoStack = &m_undo;
        Context().operators = &m_operators;
        editor::RegisterPackageBuildOperators(m_operators);
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Assets/Scenes")));
        ASSERT_TRUE(util::FileSystem::WriteText(File("Assets/Scenes/Main.scene"), "scene = []\n"));
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("NativeBuild")));
        /// @note 有効なディスク事前確認を作るだけ。実CMakeを起動せず初回Tickの起動失敗を確認する。
        ASSERT_TRUE(util::FileSystem::WriteText(File("NativeBuild/cmake.exe"), "not an executable"));
        ASSERT_TRUE(util::FileSystem::WriteText(File("NativeBuild/build.config"),
            "cmake_exe=cmake.exe\nbuild_dir=.\nexe_debug=Debug/Game.exe\nexe_release=Release/Game.exe\n"));
        m_package.Settings().outputDirectory = "Builds/Package";
        m_package.Settings().productName = "PackageGame";
        m_package.Settings().developmentBuild = false;
    }

    void TearDown() override
    {
        m_package.Cancel();
        Context().packageBuild = nullptr;
        EditorFixture::TearDown();
    }

    editor::OpResult Invoke(const char* id, const editor::OpArgs& args = {})
    {
        return editor::InvokeOperator(Context(), id, args);
    }

    editor::ai::JsonValue Send(const char* kind, editor::ai::JsonValue payload, bool dryRun = false)
    {
        editor::ai::JsonValue request = editor::ai::JsonValue::MakeObject();
        request.Set("protocol", editor::ai::JsonValue(editor::ai::kEditorProtocol));
        request.Set("id", editor::ai::JsonValue("package-build-test"));
        request.Set("kind", editor::ai::JsonValue(kind));
        request.Set("dryRun", editor::ai::JsonValue(dryRun));
        request.Set("payload", std::move(payload));
        editor::ai::EditorBusDispatcher dispatcher(Context());
        const auto response = editor::ai::ParseJson(dispatcher.Handle(editor::ai::SerializeJson(request)));
        EXPECT_TRUE(response.has_value());
        return response.value_or(editor::ai::JsonValue{});
    }

    editor::PackageBuildService m_package;
    editor::BuildConsole m_console;
    editor::UndoStack m_undo;
    editor::OperatorRegistry m_operators;
};

TEST_F(PackageBuildTest, UnsafeOutputAndFilenameAreRejectedWithoutChangingSettingsOrDisk)
{
    const std::string before = m_package.Settings().outputDirectory;
    editor::OpArgs unsafe;
    unsafe.Set("outputDirectory", std::string("."));
    const auto preview = Invoke("build.package_check", unsafe);
    ASSERT_TRUE(preview.ok);
    EXPECT_FALSE(preview.data.Find("valid")->AsBool());
    EXPECT_FALSE(preview.data.Find("canStart")->AsBool());
    const auto rejected = Invoke("build.package", unsafe);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.errorCode, "PACKAGE_PREFLIGHT_FAILED");
    EXPECT_EQ(m_package.Settings().outputDirectory, before);
    EXPECT_TRUE(m_package.RequestId().empty());
    EXPECT_FALSE(m_package.IsRunning());
    EXPECT_TRUE(util::FileSystem::Exists(File("Assets/Scenes/Main.scene")));
    for (const char* name : { "../outside", "CON", "x\"y", "trailing." }) {
        editor::OpArgs args;
        args.Set("productName", std::string(name));
        EXPECT_FALSE(Invoke("build.package", args).ok);
    }
    EXPECT_TRUE(m_undo.GetUndoDescription().empty());
}

TEST_F(PackageBuildTest, SharedPanelSettingsStartCancelAndExactRequestIdsUseOnePipeline)
{
    editor::BuildSettingsPanel panel(m_package);
    editor::EditorSettings settings;
    panel.OnSaveSettings(settings);
    EXPECT_EQ(settings.build.productName, "PackageGame");
    editor::OpArgs args;
    args.Set("productName", std::string("ChangedGame"));
    const auto start = Invoke("build.package", args);
    ASSERT_TRUE(start.ok) << start.message;
    const std::string first = start.data.Find("packageRequestId")->AsString();
    EXPECT_FALSE(start.data.Find("terminal")->AsBool());
    EXPECT_TRUE(m_package.IsRunning());
    panel.OnSaveSettings(settings);
    EXPECT_EQ(settings.build.productName, "ChangedGame");
    EXPECT_FALSE(Invoke("build.package").ok);
    editor::OpArgs wrong;
    wrong.Set("packageRequestId", std::string("package:999"));
    EXPECT_EQ(Invoke("build.package_cancel", wrong).errorCode, "PACKAGE_REQUEST_NOT_FOUND");
    EXPECT_TRUE(m_package.IsRunning());
    editor::OpArgs exact;
    exact.Set("packageRequestId", first);
    const auto cancelled = Invoke("build.package_cancel", exact);
    ASSERT_TRUE(cancelled.ok);
    EXPECT_EQ(cancelled.data.Find("state")->AsString(), "cancelled");
    EXPECT_TRUE(cancelled.data.Find("terminal")->AsBool());
    EXPECT_FALSE(cancelled.data.Find("outputReady")->AsBool());
    const auto second = Invoke("build.package");
    ASSERT_TRUE(second.ok);
    EXPECT_NE(second.data.Find("packageRequestId")->AsString(), first);
    EXPECT_EQ(Invoke("build.package_status", exact).errorCode, "PACKAGE_REQUEST_NOT_FOUND");
}

TEST_F(PackageBuildTest, EditorTickWithoutRenderingTerminatesFailureAndPreservesPreviousOutput)
{
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(File("Builds/Package/Managed")));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Builds/Package/game.manifest.toml"), "previous package"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Builds/Package/PackageGame.exe"), "previous executable"));
    ASSERT_TRUE(util::FileSystem::WriteText(File("Builds/Package/Managed/game.managed.toml"), "previous managed manifest"));
    ASSERT_TRUE(Invoke("build.package").ok);
    /// @note 描画やOnRenderContentは呼ばず、Editor updateと同じTickだけで進行する。
    m_package.Tick();
    const auto status = Invoke("build.package_status");
    ASSERT_TRUE(status.ok);
    EXPECT_EQ(status.data.Find("state")->AsString(), "failed");
    EXPECT_TRUE(status.data.Find("terminal")->AsBool());
    EXPECT_TRUE(status.data.Find("outputExeExists")->AsBool());
    EXPECT_TRUE(status.data.Find("managedManifestExists")->AsBool());
    EXPECT_FALSE(status.data.Find("outputReady")->AsBool());
    EXPECT_FALSE(status.data.Find("error")->AsString().empty());
    std::string content;
    ASSERT_TRUE(util::FileSystem::ReadText(File("Builds/Package/PackageGame.exe"), content));
    EXPECT_EQ(content, "previous executable");
}

TEST_F(PackageBuildTest, PlayPausedRestoreAndPendingCompilersRejectBeforeAnyStart)
{
    editor::PlayModeController play;
    Context().playMode = &play;
    auto& scene = AttachScene();
    play.Play(scene);
    EXPECT_FALSE(Invoke("build.package").ok);
    play.Pause();
    EXPECT_FALSE(Invoke("build.package").ok);
    play.Stop(scene);
    EXPECT_TRUE(play.HasPendingRestore());
    EXPECT_FALSE(Invoke("build.package").ok);
    /// @note 配布開始の検問だけを検証し、ResourceManagerを必要とする復元I/Oはこのfixtureで駆動しない。
    Context().playMode = nullptr;
    scene::ScriptProfiler::SetExecutionMode(false);
    Context().packageOtherBuildBusy = [] { return true; };
    EXPECT_FALSE(Invoke("build.package").ok);
    Context().packageOtherBuildBusy = {};
    Context().managedBuildBusy = true;
    EXPECT_FALSE(Invoke("build.package").ok);
    Context().managedBuildBusy = false;
    m_console.BeginBuild(editor::BuildRecord::Kind::Hlsl);
    EXPECT_FALSE(Invoke("build.package").ok);
    EXPECT_TRUE(m_package.RequestId().empty());
    Context().playMode = nullptr;
}

TEST_F(PackageBuildTest, SharedManagedBusyAndShaderBusyBlockChecksAndStartWithoutAllocatingRequests)
{
    const std::string originalName = m_package.Settings().productName;
    editor::BuildSettings candidate = m_package.Settings();
    candidate.productName = "AfterBusyGame";
    editor::OpArgs args;
    args.Set("productName", candidate.productName);
    auto expectBlocked = [&](const char* source) {
        SCOPED_TRACE(source);
        std::string code;
        std::string reason;
        EXPECT_FALSE(m_package.CanStart(Context(), code, reason));
        EXPECT_EQ(code, "BUILD_BUSY");
        EXPECT_FALSE(reason.empty());
        const auto check = Invoke("build.package_check", args);
        ASSERT_TRUE(check.ok);
        ASSERT_NE(check.data.Find("canStart"), nullptr);
        EXPECT_FALSE(check.data.Find("canStart")->AsBool());
        ASSERT_NE(check.data.Find("blockedCode"), nullptr);
        EXPECT_EQ(check.data.Find("blockedCode")->AsString(), "BUILD_BUSY");
        EXPECT_FALSE(Invoke("build.package", args).ok);
        EXPECT_FALSE(m_package.Start(Context(), candidate, false, code, reason));
        EXPECT_EQ(code, "BUILD_BUSY");
        EXPECT_TRUE(m_package.RequestId().empty());
        EXPECT_FALSE(m_package.IsRunning());
        EXPECT_EQ(m_package.Settings().productName, originalName);
    };
    Context().managedScripts = nullptr;
    Context().managedBuildBusy = true;
    expectBlocked("shared managed state without a connected service");
    Context().managedBuildBusy = false;
    std::string code;
    std::string reason;
    EXPECT_TRUE(m_package.CanStart(Context(), code, reason));
    m_console.BeginBuild(editor::BuildRecord::Kind::Hlsl);
    expectBlocked("HLSL compiler state");
    m_console.EndBuildCancelled();
    EXPECT_TRUE(m_package.CanStart(Context(), code, reason));
    const auto started = Invoke("build.package", args);
    ASSERT_TRUE(started.ok) << started.message;
    EXPECT_FALSE(m_package.RequestId().empty());
    EXPECT_EQ(m_package.Settings().productName, candidate.productName);
    m_package.Cancel();
}

TEST_F(PackageBuildTest, UnownedLegacyTempIsPreservedAndNewTempDirectoriesAreExclusive)
{
    const auto foreign = File("Builds/Package_tmp");
    ASSERT_TRUE(util::FileSystem::EnsureDirectory(foreign));
    ASSERT_TRUE(util::FileSystem::WriteText(foreign / L"keep.txt", "user document"));
    ASSERT_TRUE(Invoke("build.package").ok);
    m_package.Cancel();
    EXPECT_TRUE(util::FileSystem::Exists(foreign / L"keep.txt"));
    std::filesystem::path first;
    std::filesystem::path second;
    std::string reason;
    ASSERT_TRUE(editor::BuildPipeline::CreateOwnedTempDirectory(File("Builds/Package"), first, reason)) << reason;
    ASSERT_TRUE(editor::BuildPipeline::CreateOwnedTempDirectory(File("Builds/Package"), second, reason)) << reason;
    EXPECT_NE(first, second);
    EXPECT_NE(first, foreign);
    EXPECT_TRUE(util::FileSystem::Exists(first));
    EXPECT_TRUE(util::FileSystem::Exists(second));
    std::string document;
    ASSERT_TRUE(util::FileSystem::ReadText(foreign / L"keep.txt", document));
    EXPECT_EQ(document, "user document");
}

TEST_F(PackageBuildTest, ProjectLocalAbsoluteScenesAreNormalizedAndExternalOrTraversalScenesAreRejected)
{
    editor::OpArgs args;
    editor::OpData scenes = editor::OpData::MakeArray();
    editor::OpData entry = editor::OpData::MakeObject();
    entry.Set("path", util::FileSystem::PathToUtf8(File("Assets/Scenes/Main.scene")));
    scenes.Push(entry);
    args.Set("scenes", scenes);
    ASSERT_TRUE(Invoke("build.package", args).ok);
    ASSERT_EQ(m_package.Settings().scenes.size(), 1u);
    EXPECT_FALSE(util::FileSystem::PathFromUtf8(m_package.Settings().scenes[0].path).is_absolute());
    EXPECT_EQ(util::FileSystem::PathFromUtf8(m_package.Settings().scenes[0].path).generic_string(), "Assets/Scenes/Main.scene");
    m_package.Cancel();
    for (const char* path : { "../outside.scene", "Assets/Scenes/../Scenes/Main.scene", "C:/outside.scene" }) {
        editor::OpData invalid = editor::OpData::MakeArray();
        entry.Set("path", path);
        invalid.Push(entry);
        args.Set("scenes", invalid);
        const auto result = Invoke("build.package", args);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.errorCode, "PACKAGE_PREFLIGHT_FAILED");
    }
}

TEST_F(PackageBuildTest, ExistingBusProjectionReturnsTheSamePackageRequestAndRejectsUnknownFields)
{
    editor::ai::JsonValue payload = editor::ai::JsonValue::MakeObject();
    payload.Set("t", editor::ai::JsonValue("build.run"));
    payload.Set("target", editor::ai::JsonValue("package"));
    editor::ai::JsonValue options = editor::ai::JsonValue::MakeObject();
    options.Set("outputDirectory", editor::ai::JsonValue("Builds/BusPackage"));
    options.Set("productName", editor::ai::JsonValue("BusGame"));
    payload.Set("package", options);
    const auto dry = Send("command", payload, true);
    ASSERT_NE(dry.Find("result"), nullptr);
    EXPECT_TRUE(m_package.RequestId().empty());
    const auto started = Send("command", payload);
    ASSERT_NE(started.Find("result"), nullptr) << editor::ai::SerializeJson(started);
    const std::string request = started.Find("result")->Find("packageRequestId")->AsString();
    editor::ai::JsonValue query = editor::ai::JsonValue::MakeObject();
    query.Set("t", editor::ai::JsonValue("build.status"));
    query.Set("packageRequestId", editor::ai::JsonValue(request));
    const auto response = Send("query", query);
    ASSERT_NE(response.Find("result"), nullptr);
    const auto* package = response.Find("result")->Find("package");
    ASSERT_NE(package, nullptr);
    EXPECT_EQ(package->Find("packageRequestId")->AsString(), request);
    EXPECT_EQ(package->Find("state")->AsString(), "running");
    m_package.Cancel();
    options.Set("unknownOption", editor::ai::JsonValue(true));
    payload.Set("package", options);
    const auto rejected = Send("command", payload);
    ASSERT_NE(rejected.Find("error"), nullptr);
    EXPECT_EQ(rejected.Find("error")->Find("code")->AsString(), "BAD_ARG");
}

} /// @note namespace fbzz::tests
