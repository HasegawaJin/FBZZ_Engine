/// @file    FileSystemPathTests.cpp
/// @brief   パス文字列ユーティリティ (区切りの正規化・同一判定・配下判定・分解) の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08
///
/// Editor / AssetBrowser / シリアライザは «同じアセットか» をこの文字列比較だけで決める。
/// 区切りや大文字小文字の扱いがずれると、同じファイルが 2 つの参照として流通し、
/// 片方だけリロードされる・片方だけ保存される、という形で出る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/FileSystem.hpp>

namespace fbzz::tests {

using util::FileSystem;

class FileSystemPathTest : public testkit::EngineFixture {};

// --- 分解 -------------------------------------------------------------------

TEST_F(FileSystemPathTest, ExtensionIncludesTheDot)
{
    EXPECT_EQ(FileSystem::GetExtension("Assets/Scenes/Main.scene"), ".scene");
    EXPECT_EQ(FileSystem::GetExtension("Main.scene"), ".scene");
}

TEST_F(FileSystemPathTest, ExtensionIsEmptyWhenThereIsNoDot)
{
    EXPECT_EQ(FileSystem::GetExtension("Assets/Scenes/LICENSE"), "");
}

TEST_F(FileSystemPathTest, ExtensionTakesTheLastDot)
{
    EXPECT_EQ(FileSystem::GetExtension("Assets/Player.generated.hpp"), ".hpp");
}

TEST_F(FileSystemPathTest, FilenameAcceptsBothSeparators)
{
    EXPECT_EQ(FileSystem::GetFilename("Assets/Scenes/Main.scene"), "Main.scene");
    EXPECT_EQ(FileSystem::GetFilename("Assets\\Scenes\\Main.scene"), "Main.scene");
}

TEST_F(FileSystemPathTest, FilenameOfABareNameIsTheNameItself)
{
    EXPECT_EQ(FileSystem::GetFilename("Main.scene"), "Main.scene");
}

TEST_F(FileSystemPathTest, DirectoryKeepsTheTrailingSeparator)
{
    // 呼び出し側は連結して使う。末尾を落とすと "Assets/ScenesMain.scene" になる。
    EXPECT_EQ(FileSystem::GetDirectory("Assets/Scenes/Main.scene"), "Assets/Scenes/");
    EXPECT_EQ(FileSystem::GetDirectory("Assets\\Scenes\\Main.scene"), "Assets\\Scenes\\");
}

TEST_F(FileSystemPathTest, DirectoryIsEmptyForABareName)
{
    EXPECT_EQ(FileSystem::GetDirectory("Main.scene"), "");
}

// --- 区切りの正規化 ---------------------------------------------------------

TEST_F(FileSystemPathTest, NormalisesBackslashesToForwardSlashes)
{
    EXPECT_EQ(FileSystem::NormalizePathSeparators("Assets\\Scenes\\Main.scene"),
              "Assets/Scenes/Main.scene");
}

TEST_F(FileSystemPathTest, TrimsTrailingSlashesByDefault)
{
    EXPECT_EQ(FileSystem::NormalizePathSeparators("Assets/Scenes///"), "Assets/Scenes");
}

TEST_F(FileSystemPathTest, KeepsTrailingSlashesWhenAsked)
{
    // ルート表現 ("<proj>/Assets/") は末尾のスラッシュ込みで意味を持つ。
    EXPECT_EQ(FileSystem::NormalizePathSeparators("Assets/Scenes/", false), "Assets/Scenes/");
}

TEST_F(FileSystemPathTest, KeepsALoneSlash)
{
    EXPECT_EQ(FileSystem::NormalizePathSeparators("/"), "/");
}

TEST_F(FileSystemPathTest, NormalisationIsIdempotent)
{
    const std::string once = FileSystem::NormalizePathSeparators("Assets\\Scenes\\");

    EXPECT_EQ(FileSystem::NormalizePathSeparators(once), once);
}

// --- 同一判定 ---------------------------------------------------------------

TEST_F(FileSystemPathTest, SamePathIgnoresSeparatorStyleAndCase)
{
    // Windows のパスは大文字小文字を区別しない。同じ実体を別参照にしない。
    EXPECT_TRUE(FileSystem::SamePathText("Assets/Scenes/Main.scene",
                                         "assets\\scenes\\MAIN.SCENE"));
}

TEST_F(FileSystemPathTest, SamePathIgnoresATrailingSlash)
{
    EXPECT_TRUE(FileSystem::SamePathText("Assets/Scenes/", "Assets/Scenes"));
}

TEST_F(FileSystemPathTest, SamePathRejectsDifferentFiles)
{
    EXPECT_FALSE(FileSystem::SamePathText("Assets/Scenes/Main.scene",
                                          "Assets/Scenes/Title.scene"));
}

// --- 配下判定 ---------------------------------------------------------------

TEST_F(FileSystemPathTest, ChildPathAcceptsTheRootItself)
{
    EXPECT_TRUE(FileSystem::IsChildPathText("Assets/Scenes", "Assets/Scenes"));
}

TEST_F(FileSystemPathTest, ChildPathAcceptsNestedPaths)
{
    EXPECT_TRUE(FileSystem::IsChildPathText("Assets/Scenes/Stage/Main.scene", "Assets"));
    EXPECT_TRUE(FileSystem::IsChildPathText("assets\\scenes\\main.scene", "Assets/Scenes"));
}

TEST_F(FileSystemPathTest, ChildPathRejectsASiblingWithASharedPrefix)
{
    // "Assets/Scenes2" は "Assets/Scenes" の配下ではない。素の前方一致だと通ってしまう。
    EXPECT_FALSE(FileSystem::IsChildPathText("Assets/Scenes2/Main.scene", "Assets/Scenes"));
}

TEST_F(FileSystemPathTest, ChildPathRejectsAnythingWhenTheRootIsEmpty)
{
    // 空ルートを «全部が配下» と解釈すると、マウント外のファイルまで書き込み対象になる。
    EXPECT_FALSE(FileSystem::IsChildPathText("Assets/Scenes/Main.scene", ""));
}

TEST_F(FileSystemPathTest, ChildPathRejectsAParentDirectory)
{
    EXPECT_FALSE(FileSystem::IsChildPathText("Assets", "Assets/Scenes"));
}

} // namespace fbzz::tests
