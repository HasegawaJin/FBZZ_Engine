/// @file    AssetPathTests.cpp
/// @brief   Editor 内で共有する "Assets/..." 起点パスの正規化。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 参照は保存時に "Assets/..." 形式へ揃える約束になっている。ここがぶれると、
/// 同じアセットが 2 通りの綴りで索引に載り、guid 重複として表に出る。
#include <TestKit/TestKit.hpp>

#include <Editor/Util/AssetPath.hpp>

#include <string>

namespace fbzz::tests {

using editor::NormalizeAssetPath;
using editor::ToProjectAssetDiskPath;

TEST(AssetPath, KeepsAPathThatIsAlreadyRelativeToAssets)
{
    EXPECT_EQ(NormalizeAssetPath("Assets/Models/Player.fbx"), "Assets/Models/Player.fbx");
}

TEST(AssetPath, ConvertsBackslashesToForwardSlashes)
{
    EXPECT_EQ(NormalizeAssetPath("Assets\\Models\\Player.fbx"), "Assets/Models/Player.fbx");
}

TEST(AssetPath, CapitalisesALowerCaseAssetsPrefix)
{
    // 索引のキーは大小を吸収するが、保存される文字列は 1 通りに揃えたい。
    EXPECT_EQ(NormalizeAssetPath("assets/Models/Player.fbx"), "Assets/Models/Player.fbx");
}

TEST(AssetPath, TrimsAnAbsolutePathDownToTheAssetsRoot)
{
    EXPECT_EQ(NormalizeAssetPath("C:/proj/GreenWare/Assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
    EXPECT_EQ(NormalizeAssetPath("C:\\proj\\GreenWare\\Assets\\Models\\Player.fbx"),
              "Assets/Models/Player.fbx");
}

TEST(AssetPath, TrimsAnAbsolutePathWithALowerCaseAssetsSegment)
{
    EXPECT_EQ(NormalizeAssetPath("C:/proj/greenware/assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
}

TEST(AssetPath, LeavesAPathWithNoAssetsSegmentAlone)
{
    // Assets の外 (エンジン内蔵の素材など) は勝手に切り詰めない。
    EXPECT_EQ(NormalizeAssetPath("C:/proj/Library/Baked/abc/Player.fzasset"),
              "C:/proj/Library/Baked/abc/Player.fzasset");
    EXPECT_EQ(NormalizeAssetPath(""), "");
}

TEST(AssetPath, JoinsAnAssetsPathOntoTheProjectRoot)
{
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "Assets/Models/Player.fbx"),
              "C:/proj/GreenWare/Assets/Models/Player.fbx");
}

TEST(AssetPath, NormalisesBeforeJoining)
{
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "assets\\Models\\Player.fbx"),
              "C:/proj/GreenWare/Assets/Models/Player.fbx");
}

TEST(AssetPath, ReturnsThePathUnchangedWithoutAProjectRoot)
{
    EXPECT_EQ(ToProjectAssetDiskPath("", "Assets/Models/Player.fbx"),
              "Assets/Models/Player.fbx");
}

TEST(AssetPath, DoesNotJoinAPathOutsideAssets)
{
    // "Assets/" 起点でないものにプロジェクトルートを足すと、存在しない場所を指す。
    EXPECT_EQ(ToProjectAssetDiskPath("C:/proj/GreenWare", "C:/elsewhere/Player.fbx"),
              "C:/elsewhere/Player.fbx");
}

} // namespace fbzz::tests
