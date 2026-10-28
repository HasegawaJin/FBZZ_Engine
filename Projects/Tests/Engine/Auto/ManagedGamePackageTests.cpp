/// @file    ManagedGamePackageTests.cpp
/// @brief   C# 配布起動 manifest と開発済み世代の解決を検証する。
/// @author  Hasegawa Jin
/// @date    2026-10-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Engine/Scene/Managed/ManagedGamePackage.hpp>
#include <Engine/Util/FileSystem.hpp>

namespace fbzz::tests {
class ManagedGamePackageTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{"managed-package"};
    void Write(const std::filesystem::path& path, const std::string& text = "fixture")
    {
        ASSERT_TRUE(util::FileSystem::EnsureParentDirectory(path));
        ASSERT_TRUE(util::FileSystem::WriteText(path, text));
    }
    void Assembly(const std::filesystem::path& path)
    {
        Write(path); auto deps = path; deps.replace_extension(L".deps.json");
        Write(deps); Write(path.wstring() + L".scripts.json");
    }
};

TEST_F(ManagedGamePackageTest, NativeOnlyProjectDoesNotRequireDotNet)
{
    std::filesystem::path output = "unchanged";
    std::string diagnostic;
    ASSERT_TRUE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic)) << diagnostic;
    EXPECT_TRUE(output.empty());
    bool loaded = true;
    EXPECT_TRUE(scene::InitializeManagedGame(m_temp.Path(), m_temp.File("absent-host"), "Release", loaded, diagnostic));
    EXPECT_FALSE(loaded);
}

TEST_F(ManagedGamePackageTest, DistributionManifestRequiresDependenciesAndTypeSidecarAndRejectsTraversal)
{
    const auto manifest = m_temp.File("Managed/game.managed.toml");
    Write(manifest, "schema_version=1\nassembly='Game.Scripts.dll'\n");
    std::filesystem::path output = "unchanged";
    std::string diagnostic;
    EXPECT_FALSE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic));
    EXPECT_EQ(output, std::filesystem::path("unchanged"));
    Assembly(m_temp.File("Managed/Game.Scripts.dll"));
    ASSERT_TRUE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic)) << diagnostic;
    EXPECT_EQ(output, m_temp.File("Managed/Game.Scripts.dll"));
    for (const char* invalid : {"../Game.Scripts.dll", "C:/Game.Scripts.dll", "Game.Scripts.exe"}) {
        Write(manifest, "schema_version=1\nassembly='" + std::string(invalid) + "'\n");
        EXPECT_FALSE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic));
    }
    Write(manifest, "schema_version=2\nassembly='Game.Scripts.dll'\n");
    EXPECT_FALSE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic));
}

TEST_F(ManagedGamePackageTest, DevelopmentUsesOnlyTheAppliedGenerationAndNeverGuessesNewestCandidate)
{
    Write(m_temp.File("Game.Scripts.csproj"));
    const auto applied = m_temp.File("Library/ManagedScripts/Release/generation-1/Game.Scripts.dll");
    Assembly(applied);
    Assembly(m_temp.File("Library/ManagedScripts/Release/generation-9/Game.Scripts.dll"));
    std::filesystem::path output;
    std::string diagnostic;
    EXPECT_FALSE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic));
    Write(m_temp.File("Library/ManagedScripts/Release/current.txt"), "generation-1\n");
    ASSERT_TRUE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic)) << diagnostic;
    EXPECT_EQ(output, applied);
    Write(m_temp.File("Library/ManagedScripts/Release/current.txt"), "../generation-9\n");
    EXPECT_FALSE(scene::ResolveManagedGameAssembly(m_temp.Path(), "Release", output, diagnostic));
    Write(m_temp.File("Other.Scripts.csproj"));
    std::filesystem::path project;
    EXPECT_FALSE(scene::FindManagedGameProject(m_temp.Path(), project, diagnostic));
}
} /// @note namespace fbzz::tests
