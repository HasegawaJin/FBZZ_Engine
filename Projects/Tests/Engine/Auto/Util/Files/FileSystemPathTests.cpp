/// @file    FileSystemPathTests.cpp
/// @brief   パス文字列ユーティリティ (区切りの正規化・同一判定・配下判定・分解) の契約を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-08

/// @note Editor / AssetBrowser / シリアライザは «同じアセットか» をこの文字列比較だけで決める。
/// @note 区切りや大文字小文字の扱いがずれると、同じファイルが 2 つの参照として流通し、
/// @note 片方だけリロードされる・片方だけ保存される、という形で出る。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Util/Files/FileSystem.hpp>

namespace fbzz::tests {

using util::FileSystem;

class FileSystemPathTest : public testkit::EngineFixture {};

/// @name 分解

/// @brief Extension includes the dot.
TEST_F(FileSystemPathTest, ExtensionIncludesTheDot)
{
    EXPECT_EQ(FileSystem::GetExtension("Assets/Scenes/Main.scene"), ".scene");
    EXPECT_EQ(FileSystem::GetExtension("Main.scene"), ".scene");
}

/// @brief Extension is empty when there is no dot.
TEST_F(FileSystemPathTest, ExtensionIsEmptyWhenThereIsNoDot)
{
    EXPECT_EQ(FileSystem::GetExtension("Assets/Scenes/LICENSE"), "");
}

/// @brief Extension takes the last dot.
TEST_F(FileSystemPathTest, ExtensionTakesTheLastDot)
{
    EXPECT_EQ(FileSystem::GetExtension("Assets/Player.generated.hpp"), ".hpp");
}

/// @brief Filename accepts both separators.
TEST_F(FileSystemPathTest, FilenameAcceptsBothSeparators)
{
    EXPECT_EQ(FileSystem::GetFilename("Assets/Scenes/Main.scene"), "Main.scene");
    EXPECT_EQ(FileSystem::GetFilename("Assets\\Scenes\\Main.scene"), "Main.scene");
}

/// @brief Filename of a bare name is the name itself.
TEST_F(FileSystemPathTest, FilenameOfABareNameIsTheNameItself)
{
    EXPECT_EQ(FileSystem::GetFilename("Main.scene"), "Main.scene");
}

/// @brief Directory keeps the trailing separator.
TEST_F(FileSystemPathTest, DirectoryKeepsTheTrailingSeparator)
{
    // 呼び出し側は連結して使う。末尾を落とすと "Assets/ScenesMain.scene" になる。
    EXPECT_EQ(FileSystem::GetDirectory("Assets/Scenes/Main.scene"), "Assets/Scenes/");
    EXPECT_EQ(FileSystem::GetDirectory("Assets\\Scenes\\Main.scene"), "Assets\\Scenes\\");
}

/// @brief Directory is empty for a bare name.
TEST_F(FileSystemPathTest, DirectoryIsEmptyForABareName)
{
    EXPECT_EQ(FileSystem::GetDirectory("Main.scene"), "");
}

/// @name 区切りの正規化

/// @brief Normalises backslashes to forward slashes.
TEST_F(FileSystemPathTest, NormalisesBackslashesToForwardSlashes)
{
    EXPECT_EQ(FileSystem::NormalizePathSeparators("Assets\\Scenes\\Main.scene"),
              "Assets/Scenes/Main.scene");
}

/// @brief Trims trailing slashes by default.
TEST_F(FileSystemPathTest, TrimsTrailingSlashesByDefault)
{
    EXPECT_EQ(FileSystem::NormalizePathSeparators("Assets/Scenes///"), "Assets/Scenes");
}

/// @brief Keeps trailing slashes when asked.
TEST_F(FileSystemPathTest, KeepsTrailingSlashesWhenAsked)
{
    // ルート表現 (`"<proj>/Assets/"`) は末尾のスラッシュ込みで意味を持つ。
    EXPECT_EQ(FileSystem::NormalizePathSeparators("Assets/Scenes/", false), "Assets/Scenes/");
}

/// @brief Keeps a lone slash.
TEST_F(FileSystemPathTest, KeepsALoneSlash)
{
    EXPECT_EQ(FileSystem::NormalizePathSeparators("/"), "/");
}

/// @brief Normalisation is idempotent.
TEST_F(FileSystemPathTest, NormalisationIsIdempotent)
{
    const std::string once = FileSystem::NormalizePathSeparators("Assets\\Scenes\\");

    EXPECT_EQ(FileSystem::NormalizePathSeparators(once), once);
}

/// @name 同一判定

/// @brief Same path ignores separator style and case.
TEST_F(FileSystemPathTest, SamePathIgnoresSeparatorStyleAndCase)
{
    // Windows のパスは大文字小文字を区別しない。同じ実体を別参照にしない。
    EXPECT_TRUE(FileSystem::SamePathText("Assets/Scenes/Main.scene",
                                         "assets\\scenes\\MAIN.SCENE"));
}

/// @brief Same path ignores a trailing slash.
TEST_F(FileSystemPathTest, SamePathIgnoresATrailingSlash)
{
    EXPECT_TRUE(FileSystem::SamePathText("Assets/Scenes/", "Assets/Scenes"));
}

/// @brief Untrimmed comparison keeps the final separator significant.
TEST_F(FileSystemPathTest, UntrimmedComparisonKeepsTheFinalSeparatorSignificant)
{
    EXPECT_TRUE(FileSystem::SamePathText("Assets/Scenes/", "assets\\scenes\\", false));
    EXPECT_FALSE(FileSystem::SamePathText("Assets/Scenes/", "Assets/Scenes", false));
    EXPECT_TRUE(FileSystem::SamePathText("/", "\\", true));
    EXPECT_FALSE(FileSystem::SamePathText("/", "", true));
}

/// @brief Same path uses only the borrowed slice.
TEST_F(FileSystemPathTest, SamePathUsesOnlyTheBorrowedSlice)
{
    const std::string_view left = "Assets/Scenes/Main.scene/unused";
    const std::string_view right = "assets\\scenes\\MAIN.SCENE-other";
    const auto length = std::string_view("Assets/Scenes/Main.scene").size();
    EXPECT_TRUE(FileSystem::SamePathText(left.substr(0, length), right.substr(0, length), false));
    EXPECT_FALSE(FileSystem::SamePathText(left, right, false));
    EXPECT_TRUE(FileSystem::SamePathText(std::string_view{}, std::string_view{}, false));
}

/// @brief Same path rejects different files.
TEST_F(FileSystemPathTest, SamePathRejectsDifferentFiles)
{
    EXPECT_FALSE(FileSystem::SamePathText("Assets/Scenes/Main.scene",
                                          "Assets/Scenes/Title.scene"));
}

/// @name 配下判定

/// @brief Child path accepts the root itself.
TEST_F(FileSystemPathTest, ChildPathAcceptsTheRootItself)
{
    EXPECT_TRUE(FileSystem::IsChildPathText("Assets/Scenes", "Assets/Scenes"));
}

/// @brief Child path accepts nested paths.
TEST_F(FileSystemPathTest, ChildPathAcceptsNestedPaths)
{
    EXPECT_TRUE(FileSystem::IsChildPathText("Assets/Scenes/Stage/Main.scene", "Assets"));
    EXPECT_TRUE(FileSystem::IsChildPathText("assets\\scenes\\main.scene", "Assets/Scenes"));
}

/// @brief Child path rejects a sibling with a shared prefix.
TEST_F(FileSystemPathTest, ChildPathRejectsASiblingWithASharedPrefix)
{
    // "Assets/Scenes2" は "Assets/Scenes" の配下ではない。素の前方一致だと通ってしまう。
    EXPECT_FALSE(FileSystem::IsChildPathText("Assets/Scenes2/Main.scene", "Assets/Scenes"));
}

/// @brief Child path rejects anything when the root is empty.
TEST_F(FileSystemPathTest, ChildPathRejectsAnythingWhenTheRootIsEmpty)
{
    // 空ルートを «全部が配下» と解釈すると、マウント外のファイルまで書き込み対象になる。
    EXPECT_FALSE(FileSystem::IsChildPathText("Assets/Scenes/Main.scene", ""));
}

/// @brief Child path rejects a parent directory.
TEST_F(FileSystemPathTest, ChildPathRejectsAParentDirectory)
{
    EXPECT_FALSE(FileSystem::IsChildPathText("Assets", "Assets/Scenes"));
}

} /// @note namespace fbzz::tests
