/// @file    EditorSettingsTests.cpp
/// @brief   エディター設定の往復と、共有設定 / 個人状態の分離。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 設定は 2 か所に分かれている。チームで揃えたい値は git 追跡下の
/// Assets/EditorConfig/editor_settings.toml、カメラ位置のように人ごとに違う値は
/// Library/EditorLocalState.toml (追跡外)。混ざると «開くたびに他人のカメラへ飛ぶ»
/// あるいは «全員の手元が常に差分» になる。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/Util/EditorSettings.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fbzz::tests {
namespace {

using editor::EditorSettings;

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

class EditorSettingsTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::filesystem::create_directories(ConfigPath().parent_path());
    }

    std::filesystem::path ProjectRoot() const { return m_temp.Path(); }
    std::string ProjectRootUtf8() const { return ProjectRoot().generic_string(); }

    std::filesystem::path ConfigPath() const
    {
        return ProjectRoot() / "Assets" / "EditorConfig" / "editor_settings.toml";
    }
    std::string ConfigPathUtf8() const { return ConfigPath().generic_string(); }

    std::filesystem::path LocalStatePath() const
    {
        return ProjectRoot() / "Library" / "EditorLocalState.toml";
    }

private:
    testkit::TempDir m_temp{"editorsettings"};
};

TEST_F(EditorSettingsTest, LoadingAMissingFileKeepsDefaults)
{
    // 新規プロジェクトには設定が無い。既定値のまま素通りできること。
    EditorSettings settings;
    const bool showGridDefault = settings.showGrid;

    settings.Load(ConfigPathUtf8(), ProjectRootUtf8());

    EXPECT_EQ(settings.showGrid, showGridDefault);
}

TEST_F(EditorSettingsTest, RejectsACorruptFile)
{
    WriteFile(ConfigPath(), "this is not = = toml [[[");

    EditorSettings settings;
    EXPECT_FALSE(settings.Load(ConfigPathUtf8(), ProjectRootUtf8()));
}

TEST_F(EditorSettingsTest, RoundTripsSharedViewSettings)
{
    EditorSettings saved;
    saved.showGrid       = false;
    saved.showSkeleton   = true;
    saved.showStats      = false;
    saved.gridSize       = 2.5f;

    ASSERT_TRUE(saved.Save(ConfigPathUtf8(), ProjectRootUtf8()));

    EditorSettings loaded;
    ASSERT_TRUE(loaded.Load(ConfigPathUtf8(), ProjectRootUtf8()));

    EXPECT_FALSE(loaded.showGrid);
    EXPECT_TRUE(loaded.showSkeleton);
    EXPECT_FALSE(loaded.showStats);
    EXPECT_FLOAT_EQ(loaded.gridSize, 2.5f);
}

TEST_F(EditorSettingsTest, RoundTripsTheBakedSweepToggle)
{
    // 起動時の Library/Baked 掃除。既定は on で、off にしたら残ること。
    EditorSettings defaults;
    EXPECT_TRUE(defaults.sweepOrphanedBakedOnOpen);

    EditorSettings saved;
    saved.sweepOrphanedBakedOnOpen = false;
    ASSERT_TRUE(saved.Save(ConfigPathUtf8(), ProjectRootUtf8()));

    EditorSettings loaded;
    ASSERT_TRUE(loaded.Load(ConfigPathUtf8(), ProjectRootUtf8()));
    EXPECT_FALSE(loaded.sweepOrphanedBakedOnOpen);
}

TEST_F(EditorSettingsTest, RoundTripsTheHotReloadToggle)
{
    EditorSettings saved;
    saved.hotReloadEnabled = false;
    ASSERT_TRUE(saved.Save(ConfigPathUtf8(), ProjectRootUtf8()));

    EditorSettings loaded;
    ASSERT_TRUE(loaded.Load(ConfigPathUtf8(), ProjectRootUtf8()));
    EXPECT_FALSE(loaded.hotReloadEnabled);
}

TEST_F(EditorSettingsTest, KeepsTheCameraOutOfTheSharedFile)
{
    // カメラ位置は人ごとに違う。共有ファイルへ書くと全員の手元が常に差分になる。
    EditorSettings saved;
    saved.cameraLastPx = 123.5f;
    ASSERT_TRUE(saved.Save(ConfigPathUtf8(), ProjectRootUtf8()));

    const std::string shared = ReadFile(ConfigPath());
    EXPECT_EQ(shared.find("123.5"), std::string::npos) << shared;
}

TEST_F(EditorSettingsTest, WritesTheCameraToTheLocalStateFile)
{
    EditorSettings saved;
    saved.cameraLastPx = 123.5f;
    ASSERT_TRUE(saved.Save(ConfigPathUtf8(), ProjectRootUtf8()));

    ASSERT_TRUE(std::filesystem::exists(LocalStatePath()));
    EXPECT_NE(ReadFile(LocalStatePath()).find("123.5"), std::string::npos);
}

TEST_F(EditorSettingsTest, RoundTripsTheCameraThroughTheLocalStateFile)
{
    EditorSettings saved;
    saved.cameraLastPx      = 1.5f;
    saved.cameraLastPy      = 2.5f;
    saved.cameraSpeed       = 12.0f;
    saved.cameraOrthographic = true;
    ASSERT_TRUE(saved.Save(ConfigPathUtf8(), ProjectRootUtf8()));

    EditorSettings loaded;
    loaded.Load(ConfigPathUtf8(), ProjectRootUtf8());

    EXPECT_FLOAT_EQ(loaded.cameraLastPx, 1.5f);
    EXPECT_FLOAT_EQ(loaded.cameraLastPy, 2.5f);
    EXPECT_FLOAT_EQ(loaded.cameraSpeed, 12.0f);
    EXPECT_TRUE(loaded.cameraOrthographic);
}

TEST_F(EditorSettingsTest, LocalStateWinsOverTheSharedFile)
{
    // 旧 editor_settings.toml に [camera] が残っていても、個人状態が勝つ。
    // これが逆だと «開くたびに他人のカメラ位置へ飛ぶ»。
    WriteFile(ConfigPath(), "[camera]\nspeed = 99.0\n");
    WriteFile(LocalStatePath(), "[camera]\nspeed = 7.0\n");

    EditorSettings loaded;
    loaded.Load(ConfigPathUtf8(), ProjectRootUtf8());

    EXPECT_FLOAT_EQ(loaded.cameraSpeed, 7.0f);
}

TEST_F(EditorSettingsTest, FallsBackToTheSharedFileWhenThereIsNoLocalState)
{
    // 個人状態がまだ無い手元では、旧形式の値を拾って移行できる必要がある。
    WriteFile(ConfigPath(), "[camera]\nspeed = 99.0\n");

    EditorSettings loaded;
    loaded.Load(ConfigPathUtf8(), ProjectRootUtf8());

    EXPECT_FLOAT_EQ(loaded.cameraSpeed, 99.0f);
}

TEST_F(EditorSettingsTest, UnknownKeysAreIgnoredNotFatal)
{
    // 新しいエディターが書いた設定を古いエディターが読む場合。読める分だけ拾う。
    WriteFile(ConfigPath(),
              "[view]\nshow_grid = false\nsome_future_flag = true\n"
              "[a_whole_new_section]\nx = 1\n");

    EditorSettings loaded;
    ASSERT_TRUE(loaded.Load(ConfigPathUtf8(), ProjectRootUtf8()));
    EXPECT_FALSE(loaded.showGrid);
}

TEST_F(EditorSettingsTest, MissingKeysKeepTheirDefaults)
{
    EditorSettings defaults;
    WriteFile(ConfigPath(), "[view]\nshow_grid = false\n");

    EditorSettings loaded;
    ASSERT_TRUE(loaded.Load(ConfigPathUtf8(), ProjectRootUtf8()));

    EXPECT_FALSE(loaded.showGrid);
    EXPECT_EQ(loaded.showSkeleton, defaults.showSkeleton);
    EXPECT_EQ(loaded.hotReloadEnabled, defaults.hotReloadEnabled);
}

TEST_F(EditorSettingsTest, SaveCreatesTheLocalStateDirectory)
{
    // Library/ がまだ無い新規プロジェクトでも、個人状態を書けること。
    ASSERT_FALSE(std::filesystem::exists(LocalStatePath().parent_path()));

    EditorSettings saved;
    EXPECT_TRUE(saved.Save(ConfigPathUtf8(), ProjectRootUtf8()));

    EXPECT_TRUE(std::filesystem::exists(ConfigPath()));
    EXPECT_TRUE(std::filesystem::exists(LocalStatePath()));
}

TEST_F(EditorSettingsTest, SurvivesAnEmptyProjectRoot)
{
    // プロジェクト未確定の段階でも落ちないこと (個人状態は素通りする)。
    EditorSettings settings;
    settings.Load(ConfigPathUtf8(), "");
    EXPECT_TRUE(settings.Save(ConfigPathUtf8(), ""));
}

} // namespace fbzz::tests
