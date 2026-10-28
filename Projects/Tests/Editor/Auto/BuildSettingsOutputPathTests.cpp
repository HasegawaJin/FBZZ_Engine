/// @file    BuildSettingsOutputPathTests.cpp
/// @brief   配布ビルドの出力先バリデーション。
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// @note 置換先の検査は大小文字・実体・未作成の末尾を含め、利用者データの削除を許可しない。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/BuildSettings.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fbzz::tests {
namespace {

using editor::BuildSettings;

void Touch(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "x";
}

} /// @note namespace

class BuildOutputPathTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }

    std::string ProjectRoot() const { return m_temp.Path().generic_string(); }

    /// @note ディスクを書き換えず、出力先検査の結果だけを返す。
    bool Accepts(const std::string& outputDirectory, std::string* reason = nullptr) const
    {
        BuildSettings settings;
        settings.outputDirectory = outputDirectory;
        std::string local;
        const bool ok = settings.ValidateOutputPath(ProjectRoot(), reason ? *reason : local);
        return ok;
    }

    testkit::TempDir m_temp{ "buildoutput" };
};

TEST_F(BuildOutputPathTest, AcceptsTheDefaultOutputDirectory)
{
    EXPECT_TRUE(Accepts("Builds/MyGame"));
}

TEST_F(BuildOutputPathTest, RejectsAnEmptyOutputDirectory)
{
    /// @note 空欄は projectRoot へ解決される。ここを通すとプロジェクトごと消える。
    std::string reason;
    EXPECT_FALSE(Accepts("", &reason));
    EXPECT_FALSE(reason.empty());
}

TEST_F(BuildOutputPathTest, RejectsPathsThatResolveToTheProjectRoot)
{
    EXPECT_FALSE(Accepts("."));
    EXPECT_FALSE(Accepts(ProjectRoot()));
}

TEST_F(BuildOutputPathTest, RejectsAnAncestorOfTheProject)
{
    /// @note 親を指すと remove_all がプロジェクトを巻き込む。
    EXPECT_FALSE(Accepts(".."));
}

TEST_F(BuildOutputPathTest, RejectsProjectDataDirectories)
{
    EXPECT_FALSE(Accepts("Assets"));
    /// @note 保護フォルダーの子も置換先にしない。
    EXPECT_FALSE(Accepts("Assets/Dist"));
    EXPECT_FALSE(Accepts("Library"));
    EXPECT_FALSE(Accepts("ProjectSettings"));
}

TEST_F(BuildOutputPathTest, AllowsADirectoryWhoseNameMerelySharesAPrefix)
{
    /// @note "Build" は保護対象、"Builds" は別物。前方一致で巻き込まないこと。
    EXPECT_FALSE(Accepts("Build"));
    EXPECT_TRUE(Accepts("Builds"));
}

TEST_F(BuildOutputPathTest, RejectsAnExistingFolderThatIsNotAPreviousBuild)
{
    /// @note Browse... でデスクトップや素材置き場を選んだ状況。
    Touch(m_temp.Path() / "Elsewhere" / "notes.txt");
    std::string reason;
    EXPECT_FALSE(Accepts("Elsewhere", &reason));
    EXPECT_NE(reason.find("game.manifest.toml"), std::string::npos) << reason;
}

TEST_F(BuildOutputPathTest, AcceptsAnExistingPreviousBuild)
{
    /// @note 再ビルドは通す。前回の配布物は game.manifest.toml を持つ。
    Touch(m_temp.Path() / "Dist" / "notes.txt");
    Touch(m_temp.Path() / "Dist" / "game.manifest.toml");
    EXPECT_TRUE(Accepts("Dist"));
}

TEST_F(BuildOutputPathTest, AcceptsAnExistingEmptyFolder)
{
    std::filesystem::create_directories(m_temp.Path() / "Empty");
    EXPECT_TRUE(Accepts("Empty"));
}

TEST_F(BuildOutputPathTest, RejectsProtectedRootsWithDifferentWindowsCasing)
{
    for (const char* path : { "assets", "aSSeTs/Dist", "library/ManagedScripts/New", "PROJECTSETTINGS/Dist",
        "src/New", "include/New", "binaries/New", "build/New" }) {
        std::string reason;
        EXPECT_FALSE(Accepts(path, &reason));
        EXPECT_FALSE(reason.empty());
    }
    EXPECT_TRUE(Accepts("builds/ManagedSmoke/New"));
}

TEST_F(BuildOutputPathTest, RejectsCanonicalProjectAndAncestorAliases)
{
    std::string lower = ProjectRoot();
    for (char& value : lower) if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    EXPECT_FALSE(Accepts(lower + "/"));
    EXPECT_FALSE(Accepts(lower + "/../"));
    EXPECT_FALSE(Accepts("Builds/../../"));
    EXPECT_TRUE(Accepts((m_temp.Path() / "Builds/ManagedSmoke/New").generic_string()));
}

TEST_F(BuildOutputPathTest, RejectsNulAndExistingFileParentsWithoutChangingThem)
{
    EXPECT_FALSE(Accepts(std::string("Builds/Game\0Ignored", 19)));
    const std::filesystem::path file = m_temp.File("Occupied");
    Touch(file);
    EXPECT_FALSE(Accepts("Occupied"));
    EXPECT_FALSE(Accepts("Occupied/Game"));
    std::ifstream input(file, std::ios::binary);
    std::string text;
    input >> text;
    EXPECT_EQ(text, "x");
}

TEST_F(BuildOutputPathTest, DoesNotTreatAManifestDirectoryAsAPreviousPackage)
{
    std::filesystem::create_directories(m_temp.File("Dist/game.manifest.toml"));
    EXPECT_FALSE(Accepts("Dist"));
    EXPECT_TRUE(std::filesystem::is_directory(m_temp.File("Dist/game.manifest.toml")));
}

TEST_F(BuildOutputPathTest, PreservesAnExternalManagedFolderAndAllowsAnExternalPreviousPackage)
{
    testkit::TempDir external{ "external-managed-output" };
    ASSERT_TRUE(external.IsValid());
    const std::filesystem::path host = external.File("Runtime/Managed/Fbzz.ManagedHost.dll");
    Touch(host);
    std::string reason;
    EXPECT_FALSE(Accepts(external.File("Runtime").generic_string(), &reason));
    EXPECT_NE(reason.find("game.manifest.toml"), std::string::npos) << reason;
    std::ifstream input(host, std::ios::binary);
    std::string text;
    input >> text;
    EXPECT_EQ(text, "x");
    Touch(external.File("Package/game.manifest.toml"));
    Touch(external.File("Package/Managed/Project.Scripts.dll"));
    EXPECT_TRUE(Accepts(external.File("Package").generic_string(), &reason)) << reason;
    EXPECT_TRUE(std::filesystem::is_regular_file(external.File("Package/Managed/Project.Scripts.dll")));
}

/// @note Windows の symlink 作成権限が無い環境では明示的に skip し、字面の検査成功を実体検査と取り違えない。
/// @see https://learn.microsoft.com/cpp/standard-library/filesystem-functions#create_directory_symlink ディレクトリ参照の作成。
TEST_F(BuildOutputPathTest, RejectsLinksIntoProtectedFoldersAndTheProjectRoot)
{
    std::filesystem::create_directories(m_temp.File("Library"));
    std::error_code error;
    std::filesystem::create_directory_symlink(m_temp.File("Library"), m_temp.File("LibraryAlias"), error);
    if (error == std::errc::operation_not_permitted || error == std::errc::permission_denied)
        GTEST_SKIP() << "Directory symlink creation requires a Windows capability: " << error.message();
    ASSERT_FALSE(static_cast<bool>(error));
    EXPECT_FALSE(Accepts("LibraryAlias/NewPackage"));
    error.clear();
    std::filesystem::create_directory_symlink(m_temp.Path(), m_temp.File("ProjectAlias"), error);
    ASSERT_FALSE(static_cast<bool>(error));
    EXPECT_FALSE(Accepts("ProjectAlias"));
    EXPECT_FALSE(Accepts("ProjectAlias/Assets/NewPackage"));
}

} /// @note namespace fbzz::tests
