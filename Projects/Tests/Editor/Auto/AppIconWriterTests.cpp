/// @file    AppIconWriterTests.cpp
/// @brief   Build Settings のアイコン画像 → exe のアイコンリソース。
/// @author  Hasegawa Jin
/// @date    2026-09-15
///
/// アイコンの間違いは «配った exe を Explorer で見た» ときにしか表に出ず、
/// そこまで来ると作り直しはビルドのやり直しになる。書いたリソースが
/// Windows から読み戻せる形になっているかをここで固定する。
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>

#include <Editor/Util/AppIconWriter.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using editor::AppIconWriter;

#pragma pack(push, 1)
struct TestGroupHeader {
    std::uint16_t reserved;
    std::uint16_t type;
    std::uint16_t count;
};
struct TestGroupEntry {
    std::uint8_t  width;
    std::uint8_t  height;
    std::uint8_t  colorCount;
    std::uint8_t  reserved;
    std::uint16_t planes;
    std::uint16_t bitCount;
    std::uint32_t bytesInRes;
    std::uint16_t id;
};
#pragma pack(pop)

/// 無圧縮 32bit の TGA を書く。stb_image が読める一番単純な形で、PNG の符号化器を要らなくする。
void WriteTga(const std::filesystem::path& path, int width, int height)
{
    std::vector<std::uint8_t> bytes(18, 0);
    /// @note 無圧縮トゥルーカラー
    bytes[2]  = 2;
    bytes[12] = static_cast<std::uint8_t>(width  & 0xFF);
    bytes[13] = static_cast<std::uint8_t>((width  >> 8) & 0xFF);
    bytes[14] = static_cast<std::uint8_t>(height & 0xFF);
    bytes[15] = static_cast<std::uint8_t>((height >> 8) & 0xFF);
    /// @note bpp
    bytes[16] = 32;
    /// @note 左上原点 + アルファ 8bit
    bytes[17] = 0x28;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            /// @note 中央の四角だけ不透明にして、縮小で潰れない絵にする。
            const bool inside = x > width / 4 && x < width * 3 / 4
                             && y > height / 4 && y < height * 3 / 4;
            /// @note B
            bytes.push_back(static_cast<std::uint8_t>(inside ? 40 : 0));
            /// @note G
            bytes.push_back(static_cast<std::uint8_t>(inside ? 200 : 0));
            /// @note R
            bytes.push_back(static_cast<std::uint8_t>(inside ? 255 : 0));
            /// @note A
            bytes.push_back(static_cast<std::uint8_t>(inside ? 255 : 0));
        }
    }

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// 実行中の exe は書き換えられないため、テスト自身のコピーを作って対象にする。
std::filesystem::path CopyTestExe(const std::filesystem::path& dst)
{
    wchar_t self[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) return {};
    std::error_code ec;
    std::filesystem::copy_file(self, dst, std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? std::filesystem::path{} : dst;
}

/// exe から RT_GROUP_ICON (ID 101) を読み戻す。空なら見つからなかった。
std::vector<std::uint8_t> ReadIconGroup(const std::filesystem::path& exePath)
{
    std::vector<std::uint8_t> out;
    HMODULE module = LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!module) return out;

    HRSRC found = FindResourceW(module,
                                MAKEINTRESOURCEW(AppIconWriter::kIconResourceId),
                                /// @note RT_GROUP_ICON
                                MAKEINTRESOURCEW(14));
    if (found) {
        const DWORD size = SizeofResource(module, found);
        if (HGLOBAL loaded = LoadResource(module, found); loaded && size > 0) {
            const auto* bytes = static_cast<const std::uint8_t*>(LockResource(loaded));
            if (bytes) out.assign(bytes, bytes + size);
        }
    }
    FreeLibrary(module);
    return out;
}

/// RT_ICON の 1 枚を読み戻す。
std::vector<std::uint8_t> ReadIconImage(const std::filesystem::path& exePath, int id)
{
    std::vector<std::uint8_t> out;
    HMODULE module = LoadLibraryExW(exePath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!module) return out;

    /// @note RT_ICON
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3));
    if (found) {
        const DWORD size = SizeofResource(module, found);
        if (HGLOBAL loaded = LoadResource(module, found); loaded && size > 0) {
            const auto* bytes = static_cast<const std::uint8_t*>(LockResource(loaded));
            if (bytes) out.assign(bytes, bytes + size);
        }
    }
    FreeLibrary(module);
    return out;
}

} // namespace

class AppIconWriterTest : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(m_temp.IsValid()); }

    testkit::TempDir m_temp{ "appicon" };
};

TEST_F(AppIconWriterTest, ReportsTheSourceSizeWithoutDecoding)
{
    const std::filesystem::path image = m_temp.File("icon.tga");
    WriteTga(image, 256, 256);

    AppIconWriter::SourceInfo info;
    std::string error;
    ASSERT_TRUE(AppIconWriter::Inspect(image, info, error)) << error;
    EXPECT_EQ(info.width, 256);
    EXPECT_EQ(info.height, 256);
    EXPECT_FALSE(info.isIco);
}

TEST_F(AppIconWriterTest, RejectsAFileThatIsNotAnImage)
{
    const std::filesystem::path notAnImage = m_temp.File("notes.txt");
    std::ofstream(notAnImage, std::ios::binary) << "not an image";

    AppIconWriter::SourceInfo info;
    std::string error;
    EXPECT_FALSE(AppIconWriter::Inspect(notAnImage, info, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(AppIconWriterTest, RejectsAMissingFile)
{
    AppIconWriter::SourceInfo info;
    std::string error;
    EXPECT_FALSE(AppIconWriter::Inspect(m_temp.File("gone.png"), info, error));
}

TEST_F(AppIconWriterTest, WritesEveryGeneratedSizeIntoTheExe)
{
    const std::filesystem::path image = m_temp.File("icon.tga");
    WriteTga(image, 512, 512);

    const std::filesystem::path exe = CopyTestExe(m_temp.File("Game.exe"));
    ASSERT_FALSE(exe.empty());

    std::string error;
    ASSERT_TRUE(AppIconWriter::Apply(image, exe, error)) << error;

    const std::vector<std::uint8_t> group = ReadIconGroup(exe);
    ASSERT_GE(group.size(), sizeof(TestGroupHeader));

    TestGroupHeader header{};
    std::memcpy(&header, group.data(), sizeof(header));
    EXPECT_EQ(header.type, 1);
    ASSERT_EQ(header.count, 6);
    ASSERT_EQ(group.size(), sizeof(TestGroupHeader) + header.count * sizeof(TestGroupEntry));

    /// @note 1 枚目は 256。1 バイトに収まらないので 0 と書く約束になっている。
    TestGroupEntry first{};
    std::memcpy(&first, group.data() + sizeof(TestGroupHeader), sizeof(first));
    EXPECT_EQ(first.width, 0);
    EXPECT_EQ(first.height, 0);
    EXPECT_EQ(first.bitCount, 32);
    EXPECT_EQ(first.id, 1);

    /// @note 実体は «XOR + AND» を積んだ DIB。高さが 2 倍でないと Windows は絵を切り出せない。
    const std::vector<std::uint8_t> image256 = ReadIconImage(exe, first.id);
    ASSERT_GE(image256.size(), sizeof(BITMAPINFOHEADER));
    EXPECT_EQ(image256.size(), first.bytesInRes);

    BITMAPINFOHEADER dib{};
    std::memcpy(&dib, image256.data(), sizeof(dib));
    EXPECT_EQ(dib.biWidth, 256);
    EXPECT_EQ(dib.biHeight, 512);
    EXPECT_EQ(dib.biBitCount, 32);

    /// @note 最小サイズまで全部そろっていること (欠けると小さい表示だけ拡大でぼける)。
    TestGroupEntry last{};
    std::memcpy(&last, group.data() + sizeof(TestGroupHeader) + 5 * sizeof(TestGroupEntry),
                sizeof(last));
    EXPECT_EQ(last.width, 16);
    EXPECT_EQ(last.height, 16);
    EXPECT_FALSE(ReadIconImage(exe, last.id).empty());
}

TEST_F(AppIconWriterTest, KeepsTheExeRunnableShapeWhenAppliedTwice)
{
    const std::filesystem::path image = m_temp.File("icon.tga");
    /// @note 横長。正方形へ収めて焼けること
    WriteTga(image, 300, 120);

    const std::filesystem::path exe = CopyTestExe(m_temp.File("Game.exe"));
    ASSERT_FALSE(exe.empty());

    std::string error;
    ASSERT_TRUE(AppIconWriter::Apply(image, exe, error)) << error;
    ASSERT_TRUE(AppIconWriter::Apply(image, exe, error)) << error;

    /// @note 2 回目が 1 回目の置き去りを拾っていないこと。
    const std::vector<std::uint8_t> group = ReadIconGroup(exe);
    ASSERT_GE(group.size(), sizeof(TestGroupHeader));
    TestGroupHeader header{};
    std::memcpy(&header, group.data(), sizeof(header));
    EXPECT_EQ(header.count, 6);
}

TEST_F(AppIconWriterTest, WritesIconsWhenTheExeHasNoExistingResources)
{
    const std::filesystem::path image = m_temp.File("icon.tga");
    WriteTga(image, 32, 32);
    const std::filesystem::path exe = CopyTestExe(m_temp.File("NoIcons.exe"));
    ASSERT_FALSE(exe.empty());

    /// @note テスト exe のリンク設定に依存せず、初回のアイコン適用を再現する。
    HANDLE update = BeginUpdateResourceW(exe.c_str(), TRUE);
    ASSERT_NE(update, nullptr) << GetLastError();
    ASSERT_TRUE(EndUpdateResourceW(update, FALSE)) << GetLastError();
    ASSERT_TRUE(ReadIconGroup(exe).empty());
    ASSERT_TRUE(ReadIconImage(exe, 1).empty());

    std::string error;
    ASSERT_TRUE(AppIconWriter::Apply(image, exe, error)) << error;
    const std::vector<std::uint8_t> group = ReadIconGroup(exe);
    ASSERT_GE(group.size(), sizeof(TestGroupHeader));
    TestGroupHeader header{};
    std::memcpy(&header, group.data(), sizeof(header));
    ASSERT_EQ(header.count, 6);
    for (int id = 1; id <= header.count; ++id) {
        EXPECT_FALSE(ReadIconImage(exe, id).empty()) << id;
    }
}

} // namespace fbzz::tests
