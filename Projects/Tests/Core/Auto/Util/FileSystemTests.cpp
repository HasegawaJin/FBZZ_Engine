/// @file    FileSystemTests.cpp
/// @brief   一時ディレクトリ内で列挙、コピー、失敗時の原本保護を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-21
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/TempDir.hpp>
#include <Core/Util/FileSystem.hpp>
#include <algorithm>

namespace fbzz::tests {

using util::FileSystem;

class FileSystemTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }

    testkit::TempDir m_temp{"FileSystem"};
};

TEST_F(FileSystemTest, EnumeratesUnicodeFilesAndDirectoriesWithoutDependingOnOrder)
{
    const auto root = m_temp.Path();
    const auto textFile = root / L"日本語.txt";
    const auto binaryFile = root / "other.bin";
    const auto nested = root / "child" / "nested.txt";
    ASSERT_TRUE(FileSystem::WriteText(textFile, "text"));
    ASSERT_TRUE(FileSystem::WriteText(binaryFile, "bytes"));
    ASSERT_TRUE(FileSystem::WriteText(nested, "nested"));

    auto files = FileSystem::ListFiles(root);
    auto recursive = FileSystem::ListFilesRecursive(root);
    auto directories = FileSystem::ListDirectories(root);
    const auto utf8Root = FileSystem::PathToUtf8(root);
    const auto filtered = FileSystem::ListFiles(utf8Root, ".txt");
    const auto all = FileSystem::ListAll(utf8Root);

    EXPECT_EQ(files.size(), 2u);
    EXPECT_NE(std::find(files.begin(), files.end(), textFile), files.end());
    EXPECT_NE(std::find(files.begin(), files.end(), binaryFile), files.end());
    EXPECT_EQ(recursive.size(), 3u);
    EXPECT_NE(std::find(recursive.begin(), recursive.end(), nested), recursive.end());
    ASSERT_EQ(directories.size(), 1u);
    EXPECT_EQ(directories[0], root / "child");
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_TRUE(FileSystem::SamePath(FileSystem::PathFromUtf8(filtered[0]), textFile));
    EXPECT_EQ(FileSystem::ListFiles(utf8Root).size(), 2u);
    EXPECT_EQ(all.size(), 3u);
    EXPECT_TRUE(FileSystem::ListAll(FileSystem::PathToUtf8(root / "missing")).empty());
    EXPECT_TRUE(FileSystem::ListFiles(root / "missing").empty());
    EXPECT_TRUE(FileSystem::ListFilesRecursive(root / "missing").empty());
    EXPECT_TRUE(FileSystem::ListDirectories(root / "missing").empty());
    EXPECT_TRUE(FileSystem::ListFiles(FileSystem::PathToUtf8(root / "missing")).empty());
}

TEST_F(FileSystemTest, CopyHonorsOverwriteAndCreatesDestinationParents)
{
    const auto source = m_temp.File("source.txt");
    const auto destination = m_temp.File("child/copy.txt");
    ASSERT_TRUE(FileSystem::WriteText(source, "original"));
    ASSERT_TRUE(FileSystem::CopyFile(source, destination, false));
    ASSERT_TRUE(FileSystem::WriteText(source, "changed"));

    EXPECT_FALSE(FileSystem::CopyFileA(source, destination, false));
    std::string text;
    ASSERT_TRUE(FileSystem::ReadText(destination, text));
    EXPECT_EQ(text, "original");
    ASSERT_TRUE(FileSystem::CopyFileW(source, destination, true));
    ASSERT_TRUE(FileSystem::ReadText(destination, text));
    EXPECT_EQ(text, "changed");
}

TEST_F(FileSystemTest, CopiesNestedDirectoriesAndRemovesOnlyTheCopy)
{
    const auto source = m_temp.File("source");
    const auto destination = m_temp.File("copy");
    ASSERT_TRUE(FileSystem::WriteText(source / "nested/file.txt", "original"));

    ASSERT_TRUE(FileSystem::CopyDirectoryRecursive(source, destination, false));
    EXPECT_FALSE(FileSystem::CopyDirectoryRecursive(source, destination, false));
    ASSERT_TRUE(FileSystem::WriteText(source / "nested/file.txt", "changed"));
    ASSERT_TRUE(FileSystem::CopyDirectoryRecursive(source, destination, true));

    std::string text;
    ASSERT_TRUE(FileSystem::ReadText(destination / "nested/file.txt", text));
    EXPECT_EQ(text, "changed");
    EXPECT_TRUE(FileSystem::RemoveAll(destination));
    EXPECT_TRUE(FileSystem::RemoveAll(destination));
    EXPECT_FALSE(FileSystem::Exists(destination));
    EXPECT_TRUE(FileSystem::Exists(source / "nested/file.txt"));
}

TEST_F(FileSystemTest, RejectsFileOperationsWhenAParentIsAFile)
{
    const auto blocker = m_temp.File("blocker");
    const auto source = m_temp.File("source.txt");
    const auto destination = blocker / "child.txt";
    ASSERT_TRUE(FileSystem::WriteText(blocker, "keep"));
    ASSERT_TRUE(FileSystem::WriteText(source, "source"));

    EXPECT_FALSE(FileSystem::CopyFile(source, destination));
    EXPECT_FALSE(FileSystem::CopyDirectoryRecursive(m_temp.File("missing"), destination));
    EXPECT_FALSE(FileSystem::Rename(source, destination));
    EXPECT_FALSE(FileSystem::OpenBinaryWriter(destination).is_open());
    EXPECT_FALSE(FileSystem::WriteTextAtomic(FileSystem::PathToUtf8(destination), "replacement"));

    std::string text;
    ASSERT_TRUE(FileSystem::ReadText(blocker, text));
    EXPECT_EQ(text, "keep");
    EXPECT_TRUE(FileSystem::Exists(source));
}

TEST_F(FileSystemTest, FailedAtomicReplacementPreservesTheTargetAndRemovesTheTemporaryFile)
{
    const auto target = m_temp.File("target");
    ASSERT_TRUE(FileSystem::WriteText(target / "keep.txt", "original"));

    EXPECT_FALSE(FileSystem::WriteTextAtomic(FileSystem::PathToUtf8(target), "replacement"));

    EXPECT_FALSE(FileSystem::Exists(m_temp.File(".target.tmp")));
    std::string text;
    ASSERT_TRUE(FileSystem::ReadText(target / "keep.txt", text));
    EXPECT_EQ(text, "original");
}

TEST_F(FileSystemTest, ResolvesExistingAndMissingPathsConsistently)
{
    const auto file = m_temp.File("file.txt");
    ASSERT_TRUE(FileSystem::WriteText(file, "text"));

    EXPECT_TRUE(FileSystem::SamePath(file, m_temp.Path() / "." / "file.txt"));
    EXPECT_FALSE(FileSystem::SamePath(file, m_temp.Path()));
    EXPECT_TRUE(FileSystem::SamePath(m_temp.File("missing.txt"), m_temp.File("MISSING.TXT")));
    EXPECT_EQ(FileSystem::RelativePath(file, m_temp.Path()), std::filesystem::path("file.txt"));
    EXPECT_EQ(FileSystem::MakeAbsolute(m_temp.Path() / "." / "file.txt"), file);
    EXPECT_EQ(FileSystem::GetCurrentDirectoryA(), FileSystem::GetCurrentDirectory());
    EXPECT_EQ(FileSystem::GetCurrentDirectoryW(), FileSystem::GetCurrentDirectory());
    EXPECT_FALSE(FileSystem::GetExecutableDirectory().empty());
    EXPECT_TRUE(FileSystem::IsDirectory(FileSystem::PathToUtf8(FileSystem::GetExecutableDirectory())));
    EXPECT_EQ(FileSystem::LastWriteTime(m_temp.File("missing")), std::filesystem::file_time_type{});
    std::error_code error;
    const auto expectedTime = std::filesystem::last_write_time(file, error);
    ASSERT_FALSE(error);
    EXPECT_EQ(FileSystem::LastWriteTime(file), expectedTime);
}

} /// @note namespace fbzz::tests
