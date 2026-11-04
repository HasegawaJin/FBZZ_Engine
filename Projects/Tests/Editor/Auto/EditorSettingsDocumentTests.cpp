/// @file    EditorSettingsDocumentTests.cpp
/// @brief   直接API用の設定ドキュメントと通常保存の境界を検証する。
/// @author  Hasegawa Jin
/// @date    2026-11-04
#include <TestKit/TestKit.hpp>
#include <TestKit/Editor/EditorFixture.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <fstream>

namespace fbzz::tests {
class EditorSettingsDocumentTest : public testkit::EditorFixture {};
TEST_F(EditorSettingsDocumentTest, MemoryRoundTripIncludesSharedAndLocalValuesWithoutDiskWrites)
{
    editor::EditorSettings source;
    source.showGrid = false; source.cameraLastPx = 125.5f;
    source.animPreviewShowGhost = true; source.consoleCollapse = true;
    source.hotkeyOverrides.push_back({"scene.save", 600, true, false, false});
    source.assetBrowserPanels.push_back({});
    source.assetBrowserPanels[0].currentFolder = File("Assets/Models").generic_string();
    source.lastScenePath = File("Assets/Test.scene").generic_string();
    const std::string root = ProjectRoot().generic_string();
    const std::string text = source.ToToml(root);
    editor::EditorSettings loaded;
    ASSERT_TRUE(loaded.LoadFromToml(text, root));
    EXPECT_FALSE(loaded.showGrid); EXPECT_FLOAT_EQ(loaded.cameraLastPx, 125.5f);
    EXPECT_TRUE(loaded.animPreviewShowGhost); EXPECT_TRUE(loaded.consoleCollapse);
    ASSERT_EQ(loaded.hotkeyOverrides.size(), 1u); EXPECT_EQ(loaded.hotkeyOverrides[0].name, "scene.save");
    EXPECT_EQ(loaded.lastScenePath, source.lastScenePath);
    ASSERT_EQ(loaded.assetBrowserPanels.size(), 1u);
    EXPECT_EQ(loaded.assetBrowserPanels[0].currentFolder, source.assetBrowserPanels[0].currentFolder);
    EXPECT_FALSE(std::filesystem::exists(File("Library/EditorLocalState.toml")));
    EXPECT_EQ(loaded.ToToml(root), text);
}
TEST_F(EditorSettingsDocumentTest, MemoryLoadNeverOverlaysPersonalStateFromDisk)
{
    editor::EditorSettings disk; disk.cameraLastPx = 999.0f;
    const std::string root = ProjectRoot().generic_string();
    ASSERT_TRUE(disk.Save(File("Assets/editor_settings.toml").generic_string(), root));
    editor::EditorSettings source; source.cameraLastPx = 2.0f;
    editor::EditorSettings loaded;
    ASSERT_TRUE(loaded.LoadFromToml(source.ToToml(root), root));
    EXPECT_FLOAT_EQ(loaded.cameraLastPx, 2.0f);
}
TEST_F(EditorSettingsDocumentTest, SplitSerializationKeepsPersonalValuesOutOfSharedSettings)
{
    editor::EditorSettings source; source.cameraLastPx = 125.5f; source.showGrid = false;
    std::string shared, local; source.SerializeToml(shared, local, ProjectRoot().generic_string());
    EXPECT_EQ(shared.find("last_pos_x"), std::string::npos);
    EXPECT_NE(local.find("125.5"), std::string::npos);
    EXPECT_NE(shared.find("show_grid = false"), std::string::npos);
}
} /// @note namespace fbzz::tests
