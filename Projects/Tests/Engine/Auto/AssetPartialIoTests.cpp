/// @file    AssetPartialIoTests.cpp
/// @brief   チャンク索引付き形式・テクスチャ品質段キャッシュ・LOD の部分読み込み・画面サイズからの品質を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/asset-streaming.md «常駐予算とストリーミング»
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/Engine/EngineFixture.hpp>

#include <Engine/Asset/ChunkFile.hpp>
#include <Engine/Asset/FzModelFormat.hpp>
#include <Engine/Asset/ModelAssetImporter.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureStreamCache.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/TextureFileDecoder.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fbzz::tests {
namespace {

using renderer::DecodedTextureRGBA8;

class AssetPartialIoTest : public testkit::EngineFixture {
protected:
    testkit::TempDir m_temp{ "AssetPartialIo" };

    [[nodiscard]] std::string PathOf(const std::string& name) const { return m_temp.File(name).string(); }
};

std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> values) { return values; }

/// @brief 1 段だけの画像。画素は (x, y, 座標和, 255) で、縮小結果を手で追える。
DecodedTextureRGBA8 MakeSingleMip(uint32_t width, uint32_t height)
{
    DecodedTextureRGBA8 image;
    image.sourceWidth = width;
    image.sourceHeight = height;
    DecodedTextureRGBA8::Mip& mip = image.mips.emplace_back();
    mip.width = width;
    mip.height = height;
    mip.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* p = mip.rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            p[0] = static_cast<uint8_t>(x * 16);
            p[1] = static_cast<uint8_t>(y * 16);
            p[2] = static_cast<uint8_t>((x + y) * 8);
            p[3] = 255;
        }
    return image;
}

std::uintmax_t FileSize(const std::string& path) { return std::filesystem::file_size(path); }

/// @brief 値のバイト列を末尾へ足す。
void AppendRaw(std::vector<uint8_t>& out, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

template<typename T>
void Append(std::vector<uint8_t>& out, const T& value) { AppendRaw(out, &value, sizeof(T)); }

void WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::FILE* file = nullptr;
    ASSERT_EQ(fopen_s(&file, path.c_str(), "wb"), 0);
    ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), file), bytes.size());
    std::fclose(file);
}

/// @brief LOD を 2 つ持つ最小の .fzasset を書く。LOD0 は 4 頂点、LOD1 は 3 頂点の静的メッシュ。
void WriteTwoLodModel(const std::string& path)
{
    std::vector<uint8_t> bytes;
    asset::FzModelHeader header{};
    std::memcpy(header.magic, "FZMD", 4);
    header.version = asset::FZMODEL_VERSION;
    header.lodCount = 2;
    Append(bytes, header);
    const uint32_t vertexCounts[2] = { 4, 3 };
    for (uint32_t lod = 0; lod < 2; ++lod) {
        Append(bytes, asset::FzLodHeader{ 0.0f, 1 });
        asset::FzSubmeshHeader submesh{};
        submesh.vertexCount = vertexCounts[lod];
        submesh.indexCount = 3;
        submesh.boundsRadius = 1.0f;
        Append(bytes, submesh);
        asset::FzSubmeshExtensionV3 extension{};
        std::snprintf(extension.name, sizeof(extension.name), "Body_LOD%u", lod);
        Append(bytes, extension);
        const std::vector<renderer::Vertex> vertices(vertexCounts[lod]);
        AppendRaw(bytes, vertices.data(), vertices.size() * sizeof(renderer::Vertex));
        const uint32_t indices[3] = { 0, 1, 2 };
        Append(bytes, indices);
    }
    WriteFileBytes(path, bytes);
}

}

/// @name チャンク索引付き形式

TEST_F(AssetPartialIoTest, Crc32MatchesTheStandardCheckValue)
{
    const std::string text = "123456789";
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    EXPECT_EQ(asset::Crc32(bytes), 0xCBF43926u);
}

TEST_F(AssetPartialIoTest, ChunkFileReadsOnlyTheRequestedChunk)
{
    const std::string path = PathOf("chunks.fztc");
    asset::ChunkFileWriter writer;
    writer.AddChunk(1, std::vector<uint8_t>(4096, 0x11));
    writer.AddChunk(2, Bytes({ 1, 2, 3 }));
    ASSERT_TRUE(writer.WriteAtomically(path, asset::MakeFourCC('T', 'E', 'S', 'T'), 42));

    asset::ChunkFileReader reader;
    ASSERT_TRUE(reader.Open(path));
    EXPECT_EQ(reader.Header().sourceStamp, 42u);
    std::vector<uint8_t> chunk;
    ASSERT_TRUE(reader.ReadChunk(2, chunk));
    EXPECT_EQ(chunk, Bytes({ 1, 2, 3 }));
    EXPECT_LT(reader.BytesRead(), FileSize(path) - 4000) << "大きいチャンクを読まずに済ませる";
    ASSERT_TRUE(reader.ReadChunk(1, chunk)) << "後ろから前へも読める";
    EXPECT_EQ(chunk.size(), 4096u);
}

TEST_F(AssetPartialIoTest, ChunkFileRejectsCorruptedChunk)
{
    const std::string path = PathOf("corrupt.fztc");
    asset::ChunkFileWriter writer;
    writer.AddChunk(7, Bytes({ 9, 9, 9, 9 }));
    ASSERT_TRUE(writer.WriteAtomically(path, 1, 1));
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(-1, std::ios::end);
        file.put(static_cast<char>(0));
    }

    asset::ChunkFileReader reader;
    ASSERT_TRUE(reader.Open(path));
    std::vector<uint8_t> chunk;
    EXPECT_FALSE(reader.ReadChunk(7, chunk)) << "CRC32 が合わないチャンクは使わない";
}

TEST_F(AssetPartialIoTest, AtomicWriteReplacesAnExistingFile)
{
    const std::string path = PathOf("replace.fztc");
    asset::ChunkFileWriter first;
    first.AddChunk(1, Bytes({ 1 }));
    ASSERT_TRUE(first.WriteAtomically(path, 1, 1));
    asset::ChunkFileWriter second;
    second.AddChunk(1, Bytes({ 2 }));
    ASSERT_TRUE(second.WriteAtomically(path, 1, 2));

    asset::ChunkFileReader reader;
    ASSERT_TRUE(reader.Open(path));
    EXPECT_EQ(reader.Header().sourceStamp, 2u);
    std::size_t leftovers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(m_temp.Path()))
        if (entry.path().wstring().find(L".tmp") != std::wstring::npos) ++leftovers;
    EXPECT_EQ(leftovers, 0u) << "一時ファイルを残さない";
}

/// @name テクスチャの品質段キャッシュ

TEST_F(AssetPartialIoTest, GreenFlipChangesEveryMipWithoutChangingOtherChannels)
{
    DecodedTextureRGBA8 texture;
    texture.mips.push_back({ 2, 1, Bytes({ 10, 20, 30, 40, 50, 200, 70, 80 }) });
    texture.mips.push_back({ 1, 1, Bytes({ 90, 0, 110, 120 }) });
    const auto original = texture.mips;

    renderer::FlipTextureGreen(texture);
    EXPECT_EQ(texture.mips[0].rgba, Bytes({ 10, 235, 30, 40, 50, 55, 70, 80 }));
    EXPECT_EQ(texture.mips[1].rgba, Bytes({ 90, 255, 110, 120 }));

    renderer::FlipTextureGreen(texture);
    EXPECT_EQ(texture.mips[0].rgba, original[0].rgba);
    EXPECT_EQ(texture.mips[1].rgba, original[1].rgba);
}

TEST_F(AssetPartialIoTest, BoxDownsampleAveragesAndRounds)
{
    DecodedTextureRGBA8::Mip mip;
    mip.width = 2;
    mip.height = 2;
    mip.rgba = { 0, 0, 0, 0,   1, 0, 0, 0,
                 2, 0, 0, 0,   2, 0, 0, 255 };
    const auto half = renderer::DownsampleRGBA8Box2x(mip);
    ASSERT_EQ(half.width, 1u);
    ASSERT_EQ(half.height, 1u);
    EXPECT_EQ(half.rgba[0], 1) << "(0+1+2+2+2)/4 = 1.25 を四捨五入";
    EXPECT_EQ(half.rgba[3], 64) << "(255+2)/4 = 64.25 を四捨五入";
}

TEST_F(AssetPartialIoTest, CacheReturnsTheSamePixelsAsSelectingFromTheFullImage)
{
    const std::string path = PathOf("single.fztc");
    const DecodedTextureRGBA8 full = MakeSingleMip(8, 8);
    ASSERT_TRUE(asset::texturecache::Write(path, 7, full, 3));

    DecodedTextureRGBA8 cached;
    uint64_t bytesRead = 0;
    ASSERT_TRUE(asset::texturecache::ReadQuality(path, 7, 2, cached, &bytesRead));
    const DecodedTextureRGBA8 expected = asset::texturecache::SelectQuality(full, 2);
    ASSERT_EQ(cached.mips.size(), 1u);
    EXPECT_EQ(cached.mips[0].width, 2u);
    EXPECT_EQ(cached.mips[0].rgba, expected.mips[0].rgba);
    EXPECT_EQ(cached.sourceWidth, 8u) << "元画像の寸法を持ち続ける";
    EXPECT_LT(bytesRead, FileSize(path) / 2) << "低い品質段だけを読む";
}

TEST_F(AssetPartialIoTest, CacheWithMipChainReadsFromTheRequestedLevel)
{
    const std::string path = PathOf("chain.fztc");
    DecodedTextureRGBA8 full = MakeSingleMip(8, 8);
    full.mips.push_back(renderer::DownsampleRGBA8Box2x(full.mips[0]));
    full.mips.push_back(renderer::DownsampleRGBA8Box2x(full.mips[1]));
    ASSERT_TRUE(asset::texturecache::Write(path, 1, full, 4));

    DecodedTextureRGBA8 cached;
    ASSERT_TRUE(asset::texturecache::ReadQuality(path, 1, 1, cached));
    ASSERT_EQ(cached.mips.size(), 2u);
    EXPECT_EQ(cached.mips[0].width, 4u);
    ASSERT_TRUE(asset::texturecache::ReadQuality(path, 1, 9, cached));
    EXPECT_EQ(cached.mips.size(), 1u) << "最後の段より先へは落とさない";
}

TEST_F(AssetPartialIoTest, StaleOrMissingCacheIsNotUsed)
{
    const std::string path = PathOf("stale.fztc");
    ASSERT_TRUE(asset::texturecache::Write(path, 5, MakeSingleMip(4, 4), 1));

    DecodedTextureRGBA8 cached;
    EXPECT_FALSE(asset::texturecache::ReadQuality(path, 6, 0, cached)) << "元画像が変わったら使わない";
    EXPECT_FALSE(asset::texturecache::ReadQuality(path, 5, 2, cached)) << "書いていない品質段は元画像から作り直させる";
    EXPECT_FALSE(asset::texturecache::ReadQuality(PathOf("none.fztc"), 5, 0, cached));
}

TEST_F(AssetPartialIoTest, CacheFileNameIgnoresCaseAndSeparators)
{
    EXPECT_EQ(asset::texturecache::CacheFileName("C:\\Proj\\Assets\\A.png"),
              asset::texturecache::CacheFileName("c:/proj/assets/a.PNG"));
    EXPECT_NE(asset::texturecache::MakeSourceStamp(10, 1), asset::texturecache::MakeSourceStamp(10, 2));
}

/// @name モデルの LOD 部分読み込み

TEST_F(AssetPartialIoTest, PartialImportSkipsHigherQualityLodsOnDisk)
{
    const std::string path = PathOf("two_lods.fzasset");
    WriteTwoLodModel(path);

    asset::ModelAssetImporter importer;
    uint64_t bytesRead = 0;
    const auto partial = importer.ImportPartial(path, nullptr, 1, &bytesRead);
    ASSERT_NE(partial, nullptr);
    ASSERT_EQ(partial->LodCount(), 2u);
    EXPECT_EQ(partial->lods[0].submeshes[0].mesh, nullptr) << "LOD0 は枠だけ";
    EXPECT_EQ(partial->lods[0].submeshes[0].name, "Body_LOD0");
    ASSERT_NE(partial->lods[1].submeshes[0].mesh, nullptr);
    EXPECT_EQ(partial->lods[1].submeshes[0].mesh->vertexCount, 3u);
    EXPECT_EQ(partial->FirstResidentLod(), 1u);

    const uint64_t lod0Payload = 4 * sizeof(renderer::Vertex) + 3 * sizeof(uint32_t);
    EXPECT_EQ(bytesRead, FileSize(path) - lod0Payload) << "読まない LOD の本体はディスクから読まない";
}

TEST_F(AssetPartialIoTest, PartialImportKeepsTheLowestLodEvenIfAskedForLess)
{
    const std::string path = PathOf("clamp.fzasset");
    WriteTwoLodModel(path);

    asset::ModelAssetImporter importer;
    const auto partial = importer.ImportPartial(path, nullptr, 9);
    ASSERT_NE(partial, nullptr);
    EXPECT_NE(partial->lods[1].submeshes[0].mesh, nullptr);
    const auto full = importer.ImportPartial(path, nullptr, 0);
    ASSERT_NE(full, nullptr);
    EXPECT_EQ(full->FirstResidentLod(), 0u);
}

/// @name 画面サイズからの品質

TEST_F(AssetPartialIoTest, QualityFollowsScreenSizeWithOneLevelOfHeadroom)
{
    using asset::StreamedTextureResolver;
    EXPECT_EQ(StreamedTextureResolver::QualityForScreenSize(1024, 1024, 2048.0f, 4), 0);
    EXPECT_EQ(StreamedTextureResolver::QualityForScreenSize(1024, 1024, 512.0f, 4), 0) << "2 倍は余裕の範囲";
    EXPECT_EQ(StreamedTextureResolver::QualityForScreenSize(1024, 1024, 256.0f, 4), 1);
    EXPECT_EQ(StreamedTextureResolver::QualityForScreenSize(1024, 512, 16.0f, 4), 4) << "最低品質で止める";
    EXPECT_EQ(StreamedTextureResolver::QualityForScreenSize(1024, 1024, 0.0f, 4), 0) << "大きさ不明は最高品質";
}

}
