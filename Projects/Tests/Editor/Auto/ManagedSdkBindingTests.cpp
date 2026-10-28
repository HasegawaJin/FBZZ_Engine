/// @file    ManagedSdkBindingTests.cpp
/// @brief   SDK実行file・manifest・managed対の一致とsource fallbackの境界を実filesystemで検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-06
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/ManagedSdkBinding.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <array>
#include <fstream>
#include <sstream>
namespace fbzz::tests {
class ManagedSdkBindingTest : public testkit::EditorFixture {
protected:
    const std::string m_emptyHash = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    void Write(const std::filesystem::path& path, std::string_view bytes = {})
    {
        ASSERT_TRUE(util::FileSystem::EnsureDirectory(path.parent_path()));
        ASSERT_TRUE(util::FileSystem::WriteText(path, std::string(bytes)));
    }
    std::filesystem::path Repository(const char* name)
    {
        const auto root = ProjectRoot() / name; Write(root / "Tools/AgentBuild.ps1"); Write(root / "CMakeLists.txt"); return root;
    }
    std::filesystem::path Sdk(const std::filesystem::path& root)
    {
        std::vector<std::string> files{"tools/Debug/Editor/FBZZEditor.exe", "Tools/AgentBuild.ps1", "Projects/Managed/global.json",
            "share/fbzz/managed/ManagedStarter/Starter.Scripts.csproj", "share/fbzz/managed/ManagedStarter/Assets/Scripts/Managed/MoveForwardComponent.cs",
            "share/fbzz/managed/ManagedStarter/AGENTS.md", "share/fbzz/managed/ManagedStarter/.vscode/launch.json", "share/fbzz/managed/ManagedStarter/global.json"};
        for (const std::string& relative : std::array<std::string, 9>{"nethost.dll", "Managed/Fbzz.ScriptApi.dll", "Managed/Fbzz.ScriptApi.xml",
            "Managed/Fbzz.ScriptGenerator.dll", "Managed/Fbzz.ManagedBuildTasks.dll", "Managed/ManagedIncremental.targets",
            "Managed/Fbzz.ManagedHost.dll", "Managed/Fbzz.ManagedHost.runtimeconfig.json", "Managed/Fbzz.ManagedHost.deps.json"})
            for (const std::string prefix : {"bin/Debug/", "tools/Debug/Editor/"}) files.push_back(prefix + relative);
        std::ostringstream text;
        text << "[sdk]\nschema=2\nid='fixture'\n[runtime]\nfingerprint='" << std::string(64, 'a')
            << "'\n[configurations.Debug]\nvalidated=true\nfingerprint='" << std::string(64, 'a') << "'\n[files.Debug]\n";
        for (const auto& relative : files) { Write(root / util::FileSystem::PathFromUtf8(relative)); text << '\"' << relative << "\"='" << m_emptyHash << "'\n"; }
        Write(root / "fbzz-sdk.toml", text.str()); return root / "tools/Debug/Editor/FBZZEditor.exe";
    }
    void Remove(const std::filesystem::path& path)
    { std::error_code error; ASSERT_TRUE(std::filesystem::remove(path, error)); ASSERT_FALSE(error); }
    void Replace(const std::filesystem::path& path, const std::string& before, const std::string& after)
    {
        std::string text; ASSERT_TRUE(util::FileSystem::ReadText(path, text)); const auto offset = text.find(before); ASSERT_NE(offset, std::string::npos);
        text.replace(offset, before.size(), after); Write(path, text);
    }
};
TEST_F(ManagedSdkBindingTest, PublishedEditorInfersOwnUnicodeSdkAndActualConfigurationInsideRepository)
{
    const auto repo = Repository("Repo"); const auto root = repo / util::FileSystem::PathFromUtf8("SDK/工房/0.9.1"); const auto executable = Sdk(root);
    editor::ManagedSdkBinding result; std::string diagnostic;
    ASSERT_TRUE(editor::ResolveManagedSdkBinding(executable, repo / "Game", {}, "Development", result, diagnostic)) << diagnostic;
    EXPECT_TRUE(result.published); EXPECT_EQ(result.root, std::filesystem::weakly_canonical(root)); EXPECT_EQ(result.configuration, "Debug");
    EXPECT_EQ(result.agentBuild, result.root / "Tools/AgentBuild.ps1"); EXPECT_EQ(result.hostDirectory, executable.parent_path() / "Managed");
    EXPECT_EQ(result.apiPath, result.hostDirectory / "Fbzz.ScriptApi.dll");
}
TEST_F(ManagedSdkBindingTest, MissingManifestOrOwnedToolNeverFallsBackToAncestorRepository)
{
    const auto repo = Repository("Repo"); const auto root = repo / "SDK/0.9.1"; const auto executable = Sdk(root);
    editor::ManagedSdkBinding result; result.root = "unchanged"; std::string diagnostic;
    Remove(root / "Tools/AgentBuild.ps1");
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, repo, {}, "Debug", result, diagnostic)); EXPECT_EQ(result.root, "unchanged");
    EXPECT_NE(diagnostic.find("SDK managed publication mismatch"), std::string::npos);
    Write(root / "Tools/AgentBuild.ps1"); Remove(root / "fbzz-sdk.toml");
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, repo, {}, "Debug", result, diagnostic)); EXPECT_EQ(result.root, "unchanged");
    EXPECT_NE(diagnostic.find("SDK manifest unavailable"), std::string::npos);
}
TEST_F(ManagedSdkBindingTest, ExplicitDifferentSdkAndAncestorRepositoryAreRejected)
{
    const auto repo = Repository("Repo"); const auto first = repo / "SDK/First", second = repo / "SDK/Second";
    const auto executable = Sdk(first); (void)Sdk(second);
    editor::ManagedSdkBinding result; std::string diagnostic;
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, repo, second, "Debug", result, diagnostic));
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, repo, repo, "Debug", result, diagnostic));
    ASSERT_TRUE(editor::ResolveManagedSdkBinding(executable, repo, first / ".", "Debug", result, diagnostic)) << diagnostic;
    EXPECT_EQ(result.root, std::filesystem::weakly_canonical(first));
}
TEST_F(ManagedSdkBindingTest, ChangedApiAndPairRecordsRejectBeforeChoosingHost)
{
    const auto root = ProjectRoot() / "Sdk"; const auto executable = Sdk(root);
    editor::ManagedSdkBinding result; std::string diagnostic;
    Write(root / "bin/Debug/Managed/Fbzz.ScriptApi.dll", "incompatible API");
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, ProjectRoot(), {}, "Debug", result, diagnostic));
    EXPECT_NE(diagnostic.find("bin/Debug/Managed/Fbzz.ScriptApi.dll"), std::string::npos);
    Write(root / "bin/Debug/Managed/Fbzz.ScriptApi.dll");
    Replace(root / "fbzz-sdk.toml", "\"tools/Debug/Editor/Managed/Fbzz.ScriptApi.dll\"='" + m_emptyHash + "'",
        "\"tools/Debug/Editor/Managed/Fbzz.ScriptApi.dll\"='" + std::string(64, 'a') + "'");
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, ProjectRoot(), {}, "Debug", result, diagnostic));
    EXPECT_NE(diagnostic.find("pair records disagree"), std::string::npos);
}
TEST_F(ManagedSdkBindingTest, UnvalidatedConfigurationAndCompilerPinMismatchAreRejected)
{
    const auto root = ProjectRoot() / "Sdk"; const auto executable = Sdk(root);
    editor::ManagedSdkBinding result; std::string diagnostic;
    Replace(root / "fbzz-sdk.toml", "validated=true", "validated=false");
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, ProjectRoot(), {}, "Debug", result, diagnostic));
    EXPECT_NE(diagnostic.find("not validated"), std::string::npos);
    Replace(root / "fbzz-sdk.toml", "validated=false", "validated=true");
    Replace(root / "fbzz-sdk.toml", "\"share/fbzz/managed/ManagedStarter/global.json\"='" + m_emptyHash + "'",
        "\"share/fbzz/managed/ManagedStarter/global.json\"='" + std::string(64, 'b') + "'");
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, ProjectRoot(), {}, "Debug", result, diagnostic));
    EXPECT_NE(diagnostic.find("compiler pin"), std::string::npos);
}
TEST_F(ManagedSdkBindingTest, SourceRepositoryDiscoveryRemainsAvailableBeforeManagedHostBuild)
{
    const auto repo = Repository("Source"); const auto executable = repo / "build/Debug/FBZZEditor.exe"; Write(executable);
    editor::ManagedSdkBinding result; std::string diagnostic;
    ASSERT_TRUE(editor::ResolveManagedSdkBinding(executable, ProjectRoot(), {}, "Debug", result, diagnostic)) << diagnostic;
    EXPECT_FALSE(result.published); EXPECT_EQ(result.root, std::filesystem::weakly_canonical(repo));
    EXPECT_EQ(result.hostDirectory, result.root / "bin/Debug/Managed");
    const auto other = Repository("Other");
    EXPECT_FALSE(editor::ResolveManagedSdkBinding(executable, ProjectRoot(), other, "Debug", result, diagnostic));
}
TEST_F(ManagedSdkBindingTest, EnvironmentSelectionWinsAndProjectLegacyRootIsValidatedWhenItIsUsed)
{
    const auto root = ProjectRoot() / "Sdk"; const auto executable = Sdk(root);
    const auto project = ProjectRoot() / "Game"; Write(project / ".fbzz_proj", "[engine]\nsdk_root='../Other'\n");
    editor::ManagedSdkBinding result; std::string diagnostic;
    EXPECT_FALSE(editor::ResolveProjectManagedSdkBinding(executable, project, {}, "Debug", result, diagnostic));
    ASSERT_TRUE(editor::ResolveProjectManagedSdkBinding(executable, project, root, "Debug", result, diagnostic)) << diagnostic;
    EXPECT_EQ(result.root, std::filesystem::weakly_canonical(root));
}
} /// @note namespace fbzz::tests