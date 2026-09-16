/// @file    ImportCacheStoreTests.cpp
/// @brief   Library/ImportCache.toml の読み書きと、guid 付け替え・破棄・旧形式の受け入れ。
/// @author  Hasegawa Jin
/// @date    2026-09-10
///
/// この索引のキーは guid で、生成物の置き場所 Library/Baked/<guid>/ と対になっている。
/// 片方だけ動かすと «記録はあるのに焼き上がりが無い» / «焼き上がりはあるのに記録が無い» に
/// なり、どちらも起動のたびの焼き直しとして表に出る。対で動くことをここで固定する。
#include <TestKit/TestKit.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Editor/Import/ImportCacheStore.hpp>
#include <Engine/Asset/AssetDatabase.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fbzz::tests {
namespace {

using editor::ImportCacheStore;

constexpr const char* kGuidA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kGuidB = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ImportCacheStore::Entry MakeEntry(const char* content, const char* settings,
                                  uint64_t size = 100, int64_t mtime = 1000)
{
    ImportCacheStore::Entry entry;
    entry.contentHash  = content;
    entry.settingsHash = settings;
    entry.size  = size;
    entry.mtime = mtime;
    return entry;
}

} // namespace

class ImportCacheStoreTest : public testkit::EngineFixture {
protected:
    void SetUp() override
    {
        testkit::EngineFixture::SetUp();
        ASSERT_TRUE(m_temp.IsValid());
        std::filesystem::create_directories(AssetsRoot());
        // ImportCacheStore は AssetDatabase::ProjectRoot() から保存先を決める。
        asset::AssetDatabase::Init(AssetsRoot().generic_string());
    }

    void TearDown() override
    {
        asset::AssetDatabase::Shutdown();
        testkit::EngineFixture::TearDown();
    }

    std::filesystem::path AssetsRoot() const { return m_temp.File("Assets"); }
    std::filesystem::path CacheFile() const
    {
        return m_temp.Path() / "Library" / "ImportCache.toml";
    }

private:
    testkit::TempDir m_temp{"importcache"};
};

TEST_F(ImportCacheStoreTest, SavesAndReadsBackEveryField)
{
    ASSERT_TRUE(ImportCacheStore::Save(kGuidA, MakeEntry("c0ffee", "5e771465", 4096, 987654)));

    const ImportCacheStore::Entry loaded = ImportCacheStore::Load(kGuidA);
    EXPECT_EQ(loaded.contentHash,  "c0ffee");
    EXPECT_EQ(loaded.settingsHash, "5e771465");
    EXPECT_EQ(loaded.size,  4096u);
    EXPECT_EQ(loaded.mtime, 987654);
    EXPECT_FALSE(loaded.Empty());
}

TEST_F(ImportCacheStoreTest, WritesTheRecordToDisk)
{
    ASSERT_TRUE(ImportCacheStore::Save(kGuidA, MakeEntry("c0ffee", "5e771465")));

    ASSERT_TRUE(std::filesystem::exists(CacheFile()));
    const std::string text = ReadFile(CacheFile());
    EXPECT_NE(text.find(kGuidA), std::string::npos);
    EXPECT_NE(text.find("content = 'c0ffee'"), std::string::npos);
}

TEST_F(ImportCacheStoreTest, UnknownGuidReadsBackEmpty)
{
    EXPECT_TRUE(ImportCacheStore::Load(kGuidB).Empty());
    EXPECT_TRUE(ImportCacheStore::Load("").Empty());
}

TEST_F(ImportCacheStoreTest, RejectsAGuidThatIsNot32Hex)
{
    // guid をそのまま TOML のキーとして書くため、壊れた値を通すとファイルごと道連れになる。
    EXPECT_FALSE(ImportCacheStore::Save("not-a-guid", MakeEntry("c0ffee", "5e771465")));
    EXPECT_FALSE(ImportCacheStore::Save("'; drop = 1", MakeEntry("c0ffee", "5e771465")));
}

TEST_F(ImportCacheStoreTest, RejectsAnIncompleteEntry)
{
    ImportCacheStore::Entry noContent;
    noContent.settingsHash = "5e771465";
    EXPECT_FALSE(ImportCacheStore::Save(kGuidA, noContent));

    ImportCacheStore::Entry noSettings;
    noSettings.contentHash = "c0ffee";
    EXPECT_FALSE(ImportCacheStore::Save(kGuidA, noSettings));
}

TEST_F(ImportCacheStoreTest, RekeyMovesTheRecordToTheNewGuid)
{
    // guid を振り直すときは生成物 (Library/Baked/<guid>/) も移る。記録が付いていかないと、
    // 焼き上がりが揃っているのに «未 import» と判定される。
    ASSERT_TRUE(ImportCacheStore::Save(kGuidA, MakeEntry("c0ffee", "5e771465")));

    EXPECT_TRUE(ImportCacheStore::Rekey(kGuidA, kGuidB));

    EXPECT_TRUE(ImportCacheStore::Load(kGuidA).Empty());
    EXPECT_EQ(ImportCacheStore::Load(kGuidB).contentHash, "c0ffee");
}

TEST_F(ImportCacheStoreTest, RekeyOfAnUnknownGuidIsHarmless)
{
    EXPECT_TRUE(ImportCacheStore::Rekey(kGuidA, kGuidB));
    EXPECT_TRUE(ImportCacheStore::Load(kGuidB).Empty());
}

TEST_F(ImportCacheStoreTest, ForgetDropsOnlyTheNamedRecords)
{
    ASSERT_TRUE(ImportCacheStore::Save(kGuidA, MakeEntry("aaa", "5e771465")));
    ASSERT_TRUE(ImportCacheStore::Save(kGuidB, MakeEntry("bbb", "5e771465")));

    EXPECT_EQ(ImportCacheStore::Forget({ kGuidA }), 1u);

    EXPECT_TRUE(ImportCacheStore::Load(kGuidA).Empty());
    EXPECT_EQ(ImportCacheStore::Load(kGuidB).contentHash, "bbb");
}

TEST_F(ImportCacheStoreTest, RefreshStampMovesTheMarkerButKeepsTheHash)
{
    // 中身を読んで «変わっていなかった» と分かった直後の更新。ここで新しい時刻を覚えないと、
    // touch されたファイルを以降ずっと読み直すことになる。
    ASSERT_TRUE(ImportCacheStore::Save(kGuidA, MakeEntry("c0ffee", "5e771465", 100, 1000)));

    EXPECT_TRUE(ImportCacheStore::RefreshStamp(kGuidA, 100, 2000));

    const ImportCacheStore::Entry loaded = ImportCacheStore::Load(kGuidA);
    EXPECT_EQ(loaded.contentHash, "c0ffee");   // fingerprint は触らない
    EXPECT_EQ(loaded.mtime, 2000);
}

TEST_F(ImportCacheStoreTest, RefreshStampOfAnUnknownGuidDoesNothing)
{
    EXPECT_FALSE(ImportCacheStore::RefreshStamp(kGuidA, 1, 1));
}

TEST_F(ImportCacheStoreTest, ReadsTheLegacyFormatIntoTheMigrationSlot)
{
    // 旧形式は原本の «中身» ではなくパスと更新時刻から作られていた。値として使えないので、
    // contentHash ではなく移行用の枠で受ける。ここを取り違えると、中身が変わっていないのに
    // «一致しない» と判定して全件焼き直しになる。
    WriteFile(CacheFile(),
              "count = 1\n\n[hashes]\n"
              "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' = "
              "{ source = 'deadbeef', settings = '5e771465' }\n");

    const ImportCacheStore::Entry loaded = ImportCacheStore::Load(kGuidA);
    EXPECT_EQ(loaded.legacyStamp,  "deadbeef");
    EXPECT_EQ(loaded.settingsHash, "5e771465");
    EXPECT_TRUE(loaded.contentHash.empty());
    EXPECT_FALSE(loaded.Empty());
}

TEST_F(ImportCacheStoreTest, SurvivesACorruptCacheFile)
{
    // 壊れていても復旧は要らない。空から作り直せば、次の走査で焼き直されるだけ。
    WriteFile(CacheFile(), "this is not = = toml [[[");

    EXPECT_TRUE(ImportCacheStore::Load(kGuidA).Empty());
    EXPECT_TRUE(ImportCacheStore::Save(kGuidA, MakeEntry("c0ffee", "5e771465")));
    EXPECT_EQ(ImportCacheStore::Load(kGuidA).contentHash, "c0ffee");
}

} // namespace fbzz::tests
