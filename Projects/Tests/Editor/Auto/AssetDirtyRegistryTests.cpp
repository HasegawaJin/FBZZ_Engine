/// @file    AssetDirtyRegistryTests.cpp
/// @brief   未保存アセットの登録簿と、既定マテリアルパスの境界。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 登録簿は «閉じる前に保存を促す» の根拠になる。保存に失敗したものを clean 扱いすると
/// 編集内容が黙って消えるので、失敗は必ず dirty のまま残らなければならない。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/AssetDirtyRegistry.hpp>
#include <Editor/Util/TerrainWaterDefaults.hpp>

#include <string>

namespace fbzz::tests {
namespace {

using editor::AssetDirtyRegistry;

} // namespace

/// 静的な登録簿。テスト間で持ち越さないよう毎回空にする。
class AssetDirtyRegistryTest : public testkit::Fixture {
protected:
    void SetUp() override    { AssetDirtyRegistry::DiscardAll(); }
    void TearDown() override { AssetDirtyRegistry::DiscardAll(); }

    static void RegisterOk(const std::string& path, bool* flag = nullptr)
    {
        AssetDirtyRegistry::Register(path, path, "MAT", [flag] {
            if (flag != nullptr) *flag = true;
            return true;
        });
    }

    static void RegisterFailing(const std::string& path)
    {
        AssetDirtyRegistry::Register(path, path, "MAT", [] { return false; });
    }
};

TEST_F(AssetDirtyRegistryTest, StartsEmpty)
{
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, RegisterMarksTheAssetDirty)
{
    RegisterOk("C:/a.mat");

    EXPECT_TRUE(AssetDirtyRegistry::HasAny());
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
    EXPECT_EQ(AssetDirtyRegistry::GetAll().size(), 1u);
}

TEST_F(AssetDirtyRegistryTest, RegisteringTheSamePathTwiceKeepsOneEntry)
{
    /// @note 編集のたびに登録されるので、同じアセットが何度も並んではいけない。
    RegisterOk("C:/a.mat");
    RegisterOk("C:/a.mat");

    EXPECT_EQ(AssetDirtyRegistry::GetAll().size(), 1u);
}

TEST_F(AssetDirtyRegistryTest, ReRegisteringReplacesTheSaveAction)
{
    bool firstCalled  = false;
    bool secondCalled = false;
    RegisterOk("C:/a.mat", &firstCalled);
    RegisterOk("C:/a.mat", &secondCalled);

    EXPECT_TRUE(AssetDirtyRegistry::Save("C:/a.mat"));
    EXPECT_FALSE(firstCalled);
    EXPECT_TRUE(secondCalled);
}

TEST_F(AssetDirtyRegistryTest, MarkCleanRemovesTheEntry)
{
    RegisterOk("C:/a.mat");
    AssetDirtyRegistry::MarkClean("C:/a.mat");

    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
}

TEST_F(AssetDirtyRegistryTest, MarkCleanOfAnUnknownPathIsHarmless)
{
    RegisterOk("C:/a.mat");
    AssetDirtyRegistry::MarkClean("C:/other.mat");

    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, SaveRunsTheActionAndClearsTheEntry)
{
    bool called = false;
    RegisterOk("C:/a.mat", &called);

    EXPECT_TRUE(AssetDirtyRegistry::Save("C:/a.mat"));
    EXPECT_TRUE(called);
    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, AFailedSaveLeavesTheAssetDirty)
{
    /// @note ここを clean にすると «保存したつもり» で閉じられ、編集内容が消える。
    RegisterFailing("C:/a.mat");

    EXPECT_FALSE(AssetDirtyRegistry::Save("C:/a.mat"));
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
}

TEST_F(AssetDirtyRegistryTest, SavingAnUnknownPathFails)
{
    EXPECT_FALSE(AssetDirtyRegistry::Save("C:/nope.mat"));
}

TEST_F(AssetDirtyRegistryTest, SaveAllReportsHowManyFailed)
{
    RegisterOk("C:/a.mat");
    RegisterFailing("C:/b.mat");
    RegisterOk("C:/c.mat");

    EXPECT_EQ(AssetDirtyRegistry::SaveAll(), 1);
}

TEST_F(AssetDirtyRegistryTest, SaveAllKeepsOnlyTheFailedOnesDirty)
{
    RegisterOk("C:/a.mat");
    RegisterFailing("C:/b.mat");

    AssetDirtyRegistry::SaveAll();

    EXPECT_FALSE(AssetDirtyRegistry::IsDirty("C:/a.mat"));
    EXPECT_TRUE(AssetDirtyRegistry::IsDirty("C:/b.mat"));
}

TEST_F(AssetDirtyRegistryTest, DiscardAllDropsEverythingWithoutSaving)
{
    bool called = false;
    RegisterOk("C:/a.mat", &called);

    AssetDirtyRegistry::DiscardAll();

    EXPECT_FALSE(AssetDirtyRegistry::HasAny());
    /// @note 破棄なので保存は走らない
    EXPECT_FALSE(called);
}

TEST_F(AssetDirtyRegistryTest, KeepsTheTypeLabelForDisplay)
{
    AssetDirtyRegistry::Register("C:/a.animcontroller", "Assets/a.animcontroller",
                                 "CTRL", [] { return true; });

    ASSERT_EQ(AssetDirtyRegistry::GetAll().size(), 1u);
    EXPECT_EQ(AssetDirtyRegistry::GetAll()[0].typeLabel, "CTRL");
    EXPECT_EQ(AssetDirtyRegistry::GetAll()[0].displayPath, "Assets/a.animcontroller");
}

/// @name 既定マテリアルパス

TEST(TerrainWaterDefaults, ReturnsAPathForEveryTerrainLayer)
{
    for (int layer = 0; layer < 4; ++layer) {
        const char* path = editor::DefaultTerrainLayerMaterialPath(layer);
        ASSERT_NE(path, nullptr);
        EXPECT_STRNE(path, "") << "layer " << layer;
    }
}

TEST(TerrainWaterDefaults, LayersHaveDistinctPaths)
{
    /// @note 全レイヤーが同じマテリアルを指すと、塗り分けても見た目が変わらない。
    EXPECT_STRNE(editor::DefaultTerrainLayerMaterialPath(0),
                 editor::DefaultTerrainLayerMaterialPath(1));
    EXPECT_STRNE(editor::DefaultTerrainLayerMaterialPath(2),
                 editor::DefaultTerrainLayerMaterialPath(3));
}

TEST(TerrainWaterDefaults, ReturnsAnEmptyStringOutsideTheLayerRange)
{
    /// @note 範囲外で配列の外を読むと、そのまま «存在しないパス» を掴んで落ちる。
    EXPECT_STREQ(editor::DefaultTerrainLayerMaterialPath(-1), "");
    EXPECT_STREQ(editor::DefaultTerrainLayerMaterialPath(4), "");
    EXPECT_STREQ(editor::DefaultTerrainLayerMaterialPath(9999), "");
}

TEST(TerrainWaterDefaults, WaterMaterialPathIsAnAssetsPath)
{
    const std::string path = editor::DefaultWaterMaterialPath();
    EXPECT_FALSE(path.empty());
    EXPECT_EQ(path.rfind("Assets/", 0), 0u);
}

TEST(TerrainWaterDefaults, WaterPresetsStartWithTheDefaultAndPointAtDistinctMaterials)
{
    const auto presets = editor::WaterMaterialPresets();
    ASSERT_FALSE(presets.empty());
    /// @note Create Water が既定と違う .mat を差すと、作った直後の Inspector でどのプリセットも選ばれていない。
    EXPECT_STREQ(presets[0].path, editor::DefaultWaterMaterialPath());
    for (size_t i = 0; i < presets.size(); ++i) {
        EXPECT_EQ(std::string(presets[i].path).rfind("Assets/Materials/Water/", 0), 0u) << presets[i].label;
        for (size_t j = i + 1; j < presets.size(); ++j)
            EXPECT_STRNE(presets[i].path, presets[j].path);
    }
}

} // namespace fbzz::tests
