/// @file    ImportFingerprintTests.cpp
/// @brief   再インポート判定に使う fingerprint が «中身» だけを見ていることを検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// 以前この値は「パス + サイズ + 更新時刻」から作られていた。どれも中身とは無関係に動くため、
/// git pull・clone・コピーのたびに全 FBX の焼き直しが走っていた。
/// «触られたか» ではなく «変わったか» を見ていることを、ここで固定する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/Import/FbxMetaSerializer.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fbzz::tests {
namespace {

using editor::FbxMetaSerializer;

void WriteFile(const std::filesystem::path& path, const std::string& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

// 更新時刻だけを動かす。中身は 1 バイトも変えない (git pull / コピーが起こす状況)。
// 実際に «動いた» ことまで確かめる。動かせなければテストの前提が崩れるので、
// 呼び出し側で ASSERT すること (黙って通ると «変わらない» を検証できていない)。
[[nodiscard]] bool TouchOnly(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto before = std::filesystem::last_write_time(path, ec);
    if (ec) return false;

    std::filesystem::last_write_time(path, before + std::chrono::hours(1), ec);
    if (ec) return false;

    const auto after = std::filesystem::last_write_time(path, ec);
    return !ec && after != before;
}

} // namespace

class ImportFingerprintTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
    }

    std::filesystem::path Model(const std::string& name) const { return m_temp.File(name); }
    std::string Utf8(const std::filesystem::path& p) const { return p.generic_string(); }

private:
    testkit::TempDir m_temp{"importfp"};
};

TEST_F(ImportFingerprintTest, SameBytesGiveTheSameHash)
{
    const auto a = Model("A.fbx");
    const auto b = Model("B.fbx");
    WriteFile(a, "the same bytes");
    WriteFile(b, "the same bytes");

    EXPECT_EQ(FbxMetaSerializer::SourceContentHash(Utf8(a)),
              FbxMetaSerializer::SourceContentHash(Utf8(b)));
}

TEST_F(ImportFingerprintTest, DifferentBytesGiveDifferentHashes)
{
    const auto a = Model("A.fbx");
    const auto b = Model("B.fbx");
    WriteFile(a, "aaaa");
    WriteFile(b, "aaab");

    EXPECT_NE(FbxMetaSerializer::SourceContentHash(Utf8(a)),
              FbxMetaSerializer::SourceContentHash(Utf8(b)));
}

TEST_F(ImportFingerprintTest, TouchingTheFileDoesNotChangeTheHash)
{
    // これが修正の核心。git pull は中身を変えずに更新時刻だけ動かす。
    const auto model = Model("Player.fbx");
    WriteFile(model, "unchanged content");
    const std::string before = FbxMetaSerializer::SourceContentHash(Utf8(model));

    ASSERT_TRUE(TouchOnly(model));

    EXPECT_EQ(FbxMetaSerializer::SourceContentHash(Utf8(model)), before);
}

TEST_F(ImportFingerprintTest, MovingTheFileDoesNotChangeTheHash)
{
    // 以前はパスを種に混ぜていたため、フォルダを整理しただけで焼き直しが走っていた。
    const auto from = Model("Player.fbx");
    const auto to   = Model("Models/Player.fbx");
    WriteFile(from, "unchanged content");
    const std::string before = FbxMetaSerializer::SourceContentHash(Utf8(from));

    std::filesystem::create_directories(to.parent_path());
    std::filesystem::rename(from, to);

    EXPECT_EQ(FbxMetaSerializer::SourceContentHash(Utf8(to)), before);
}

TEST_F(ImportFingerprintTest, ContentHashIsEmptyForAMissingFile)
{
    EXPECT_EQ(FbxMetaSerializer::SourceContentHash(Utf8(Model("Nope.fbx"))), "");
}

TEST_F(ImportFingerprintTest, HashSpansTheWholeFileNotJustTheHead)
{
    // 先頭だけを見ていると、末尾を差し替えた FBX を «変わっていない» と誤判定する。
    const std::string head(128 * 1024, 'x');
    const auto a = Model("A.fbx");
    const auto b = Model("B.fbx");
    WriteFile(a, head + "tail-one");
    WriteFile(b, head + "tail-two");

    EXPECT_NE(FbxMetaSerializer::SourceContentHash(Utf8(a)),
              FbxMetaSerializer::SourceContentHash(Utf8(b)));
}

TEST_F(ImportFingerprintTest, StampReportsSizeAndMovesWithTheWriteTime)
{
    const auto model = Model("Player.fbx");
    WriteFile(model, "0123456789");

    uint64_t size = 0;
    int64_t  mtime = 0;
    ASSERT_TRUE(FbxMetaSerializer::SourceStamp(Utf8(model), size, mtime));
    EXPECT_EQ(size, 10u);

    ASSERT_TRUE(TouchOnly(model));

    uint64_t size2 = 0;
    int64_t  mtime2 = 0;
    ASSERT_TRUE(FbxMetaSerializer::SourceStamp(Utf8(model), size2, mtime2));
    EXPECT_EQ(size2, size);
    // 目印は «触られたか» を表す。中身のハッシュと違い、ここは動いてよい。
    EXPECT_NE(mtime2, mtime);
}

TEST_F(ImportFingerprintTest, StampFailsForAMissingFile)
{
    uint64_t size = 0;
    int64_t  mtime = 0;
    EXPECT_FALSE(FbxMetaSerializer::SourceStamp(Utf8(Model("Nope.fbx")), size, mtime));
}

TEST_F(ImportFingerprintTest, LegacyStampStillReactsToTheWriteTime)
{
    // 旧形式は «触られたか» を見る値。移行判定に使うので、その性質のまま残っている必要がある。
    const auto model = Model("Player.fbx");
    WriteFile(model, "unchanged content");
    const std::string before = FbxMetaSerializer::LegacyStampHash(Utf8(model));

    ASSERT_TRUE(TouchOnly(model));

    EXPECT_NE(FbxMetaSerializer::LegacyStampHash(Utf8(model)), before);
}

} // namespace fbzz::tests
