/// @file    BuildSettingsOutputPathTests.cpp
/// @brief   配布ビルドの出力先バリデーション。
/// @author  Hasegawa Jin
/// @date    2026-09-14
///
/// BuildPipeline のコミットは出力先を remove_all してから _tmp を rename する。
/// つまり «出力先に選べるもの» はそのまま «消してよいもの» の定義になる。
/// 空欄がプロジェクトルートへ解決されると、ビルドを押した瞬間にプロジェクトが消える。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/BuildSettings.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fbzz::tests {
namespace {

using editor::BuildSettings;

void Touch(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << "x";
}

} // namespace

class BuildOutputPathTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }

    std::string ProjectRoot() const { return m_temp.Path().generic_string(); }

    /// outputDirectory を差し替えて検証結果だけを返す。
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
    /// @note Assets の中も同じく消える
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
    EXPECT_NE(reason.find("game.manifest.toml"), std::string::npos);
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

} // namespace fbzz::tests
