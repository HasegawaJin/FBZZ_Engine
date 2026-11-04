/// @file    AssetPartialIoTests.cpp
/// @brief   チャンク索引付き形式・テクスチャ品質段キャッシュ・LOD の部分読み込み・画面サイズからの品質を検証する。
/// @author  Hasegawa Jin
/// @date    2026-09-19
/// @see Docs/design/asset-streaming.md «常駐予算とストリーミング»
#include <TestKit/TestKit.hpp>
#include <TestKit/TempDir.hpp>
#include <TestKit/Engine/EngineFixture.hpp>
#include <TestKit/Engine/LogSinks.hpp>
#include "../../../Engine/src/Asset/AtomicChunkFileWrite.hpp"
#include <Engine/Util/FileSystem.hpp>

#include <Engine/Asset/BinaryReader.hpp>
#include <Engine/Asset/ChunkFile.hpp>
#include <Engine/Asset/FzModelFormat.hpp>
#include <Engine/Asset/ModelAssetImporter.hpp>
#include <Engine/Asset/StreamedTextureResolver.hpp>
#include <Engine/Asset/TextureStreamCache.hpp>
#include <Engine/Renderer/Mesh.hpp>
#include <Engine/Renderer/TextureFileDecoder.hpp>

#include <array>
#include <barrier>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <limits>
#include <thread>
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

/// @note Dependency fake changes only filesystem outcomes; the production publication procedure owns all ordering and failure decisions.
struct AtomicChunkIoFake {
    using Error = asset::detail::AtomicWriteError;
    Error parentFailure, openFailure, writeFailure, flushFailure, closeFailure, renameFailure, cleanupFailure;
    std::vector<std::string> calls;
    bool temporaryExists = false;
    bool opened = false;
    bool targetUnchanged = true;

    Error CreateParent() { calls.emplace_back("parent"); return parentFailure; }
    Error Open() {
        calls.emplace_back("open");
        if (!openFailure) { temporaryExists = true; opened = true; }
        return openFailure;
    }
    Error Write() { calls.emplace_back("write"); return writeFailure; }
    Error Flush() { calls.emplace_back("flush"); return flushFailure; }
    Error Close() { calls.emplace_back("close"); opened = false; return closeFailure; }
    Error Publish() {
        calls.emplace_back("rename");
        if (!renameFailure) { targetUnchanged = false; temporaryExists = false; }
        return renameFailure;
    }
    Error Cleanup() {
        calls.emplace_back("cleanup");
        if (!cleanupFailure) temporaryExists = false;
        return cleanupFailure;
    }
};

asset::detail::AtomicWriteError AtomicFailure(std::errc code, unsigned long nativeCode)
{
    return {std::make_error_code(code), nativeCode};
}

std::size_t TemporaryCount(const std::filesystem::path& directory)
{
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().wstring().find(L".tmp") != std::wstring::npos) ++count;
    return count;
}

std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> values) { return values; }

TEST_F(AssetPartialIoTest, DistributionCacheSurvivesRelocationAndRejectsChangedSource)
{
    const auto source = PathOf("source.tga");
    {
        std::ofstream file(source, std::ios::binary);
        const uint8_t header[18] = {0,0,2,0,0,0,0,0,0,0,0,0,16,0,16,0,32,0x28};
        file.write(static_cast<const char*>(static_cast<const void*>(header)), sizeof(header));
        for (unsigned i = 0; i < 256; ++i) {
            const uint8_t pixel[4] = {static_cast<uint8_t>(i), 64, 128, 255};
            file.write(static_cast<const char*>(static_cast<const void*>(pixel)), sizeof(pixel));
        }
    }
    std::string error;
    ASSERT_TRUE(asset::texturecache::BakeDistributionTexture(source, error)) << error;
    ASSERT_TRUE(std::filesystem::exists(source + ".fztc"));
    const auto moved = PathOf("relocated.tga");
    std::error_code ec;
    std::filesystem::rename(source, moved, ec);
    ASSERT_FALSE(ec);
    std::filesystem::rename(source + ".fztc", moved + ".fztc", ec);
    ASSERT_FALSE(ec);
    const auto size = std::filesystem::file_size(moved, ec);
    const auto time = std::filesystem::last_write_time(moved, ec);
    ASSERT_FALSE(ec);
    const auto stamp = asset::texturecache::MakeSourceStamp(size, time.time_since_epoch().count());
    renderer::DecodedTextureRGBA8 full, cached;
    ASSERT_TRUE(renderer::DecodeTextureFileRGBA8(moved, full, &error));
    for (asset::AssetQuality quality = 0; quality <= 4; ++quality) {
        ASSERT_TRUE(asset::texturecache::ReadQuality(moved + ".fztc", stamp, quality, cached));
        const auto expected = asset::texturecache::SelectQuality(full, quality);
        ASSERT_EQ(cached.mips.size(), expected.mips.size());
        EXPECT_EQ(cached.mips[0].rgba, expected.mips[0].rgba);
        EXPECT_EQ(cached.sourceWidth, 16u);
    }
    EXPECT_FALSE(asset::texturecache::ReadQuality(moved + ".fztc",
        asset::texturecache::MakeSourceStamp(size + 1, time.time_since_epoch().count()), 0, cached));
}

TEST_F(AssetPartialIoTest, DistributionBakeReportsBrokenTexturesButSkipsNonTextures)
{
    std::string error;
    EXPECT_TRUE(asset::texturecache::BakeDistributionTexture(PathOf("scene.scene"), error));
    EXPECT_FALSE(asset::texturecache::BakeDistributionTexture(PathOf("missing.png"), error));
    EXPECT_FALSE(error.empty());
}

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

} /// @note namespace

/// @name チャンク索引付き形式

TEST_F(AssetPartialIoTest, BinaryReaderRejectsWrappedSkipWithoutMovingEitherCursor)
{
    const auto path = PathOf("reader-range.bin");
    WriteFileBytes(path, Bytes({11, 22, 33, 44}));
    for (const bool streaming : {false, true}) {
        SCOPED_TRACE(streaming);
        asset::BinaryReader reader;
        ASSERT_TRUE(streaming ? reader.OpenStreaming(path) : reader.Open(path));
        ASSERT_TRUE(reader.Seek(1));

        EXPECT_FALSE(reader.Skip(std::numeric_limits<size_t>::max()));

        EXPECT_EQ(reader.pos, 1u);
        uint8_t value = 0;
        ASSERT_TRUE(reader.Read(value));
        EXPECT_EQ(value, 22);
    }
}

TEST_F(AssetPartialIoTest, BinaryReaderRejectedRangePreservesDestinationAndBothCursors)
{
    const auto path = PathOf("reader-range-failure.bin");
    WriteFileBytes(path, Bytes({11, 22, 33, 44}));
    for (const bool streaming : {false, true}) {
        SCOPED_TRACE(streaming);
        asset::BinaryReader reader;
        ASSERT_TRUE(streaming ? reader.OpenStreaming(path) : reader.Open(path));
        ASSERT_TRUE(reader.Seek(1));
        const auto previousBytesRead = reader.bytesRead;
        std::array<uint8_t, 4> output{99, 99, 99, 99};

        EXPECT_FALSE(reader.ReadBytes(output.data(), output.size()));

        EXPECT_EQ(output, (std::array<uint8_t, 4>{99, 99, 99, 99}));
        EXPECT_EQ(reader.pos, 1u);
        EXPECT_EQ(reader.bytesRead, previousBytesRead);
        uint8_t value = 0;
        ASSERT_TRUE(reader.Read(value));
        EXPECT_EQ(value, 22);
    }
}

TEST_F(AssetPartialIoTest, BinaryReaderKeepsEndAndZeroLengthOperationsWithinBounds)
{
    const auto path = PathOf("reader-end.bin");
    WriteFileBytes(path, Bytes({11, 22}));
    for (const bool streaming : {false, true}) {
        SCOPED_TRACE(streaming);
        asset::BinaryReader reader;
        ASSERT_TRUE(streaming ? reader.OpenStreaming(path) : reader.Open(path));
        ASSERT_TRUE(reader.Seek(reader.Size()));
        uint8_t value = 99;

        EXPECT_FALSE(reader.Read(value));
        EXPECT_FALSE(reader.Skip(1));
        EXPECT_TRUE(reader.ReadBytes(nullptr, 0));
        EXPECT_TRUE(reader.Skip(0));

        EXPECT_EQ(value, 99);
        EXPECT_EQ(reader.pos, 2u);
    }
}

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

TEST_F(AssetPartialIoTest, ChunkFileRejectsOverflowingPayloadExtent)
{
    const auto path = PathOf("overflow.fztc");
    asset::FzChunkFileHeader header{};
    std::memcpy(header.magic, "FZCK", 4);
    header.version = asset::FZCHUNK_VERSION;
    header.chunkCount = 1;
    asset::FzChunkEntry entry{};
    entry.id = 7;
    entry.offset = sizeof(header) + sizeof(entry);
    entry.storedSize = entry.rawSize = std::numeric_limits<uint64_t>::max();
    std::vector<uint8_t> bytes;
    Append(bytes, header);
    Append(bytes, entry);
    WriteFileBytes(path, bytes);
    asset::ChunkFileReader reader;

    EXPECT_FALSE(reader.Open(path));

    EXPECT_EQ(reader.Find(7), nullptr);
}

TEST_F(AssetPartialIoTest, ChunkFileFailedIndexDoesNotPublishEarlierValidEntries)
{
    const auto path = PathOf("partial-index.fztc");
    asset::FzChunkFileHeader header{};
    std::memcpy(header.magic, "FZCK", 4);
    header.version = asset::FZCHUNK_VERSION;
    header.chunkCount = 2;
    const auto payload = Bytes({42});
    asset::FzChunkEntry first{};
    first.id = 7;
    first.offset = sizeof(header) + 2 * sizeof(first);
    first.storedSize = first.rawSize = payload.size();
    first.checksum = asset::Crc32(payload);
    auto invalid = first;
    invalid.id = 8;
    invalid.flags = 1;
    std::vector<uint8_t> bytes;
    Append(bytes, header);
    Append(bytes, first);
    Append(bytes, invalid);
    AppendRaw(bytes, payload.data(), payload.size());
    WriteFileBytes(path, bytes);
    asset::ChunkFileReader reader;

    EXPECT_FALSE(reader.Open(path));

    EXPECT_EQ(reader.Find(7), nullptr);
    std::vector<uint8_t> chunk;
    EXPECT_FALSE(reader.ReadChunk(7, chunk));
    EXPECT_EQ(reader.Header().chunkCount, 0u);
    EXPECT_TRUE(util::FileSystem::WriteBinary(util::FileSystem::PathFromUtf8(path), payload.data(), payload.size()));
}

TEST_F(AssetPartialIoTest, ChunkFileRejectsPayloadInsideIndexAndInconsistentStoredSize)
{
    const auto path = PathOf("invalid-layout.fztc");
    asset::FzChunkFileHeader header{};
    std::memcpy(header.magic, "FZCK", 4);
    header.version = asset::FZCHUNK_VERSION;
    header.chunkCount = 1;
    for (const bool insideIndex : {true, false}) {
        SCOPED_TRACE(insideIndex);
        asset::FzChunkEntry entry{};
        entry.id = 7;
        entry.offset = insideIndex ? sizeof(header) : sizeof(header) + sizeof(entry);
        entry.storedSize = 1;
        entry.rawSize = insideIndex ? 1 : 2;
        std::vector<uint8_t> bytes;
        Append(bytes, header);
        Append(bytes, entry);
        bytes.push_back(42);
        WriteFileBytes(path, bytes);
        asset::ChunkFileReader reader;

        EXPECT_FALSE(reader.Open(path));
    }
}

TEST_F(AssetPartialIoTest, ChunkFileFailedReopenClearsOldHeaderAndReleasesRejectedFile)
{
    const auto goodPath = PathOf("valid.fztc");
    const auto badPath = PathOf("bad-magic.fztc");
    asset::ChunkFileWriter writer;
    writer.AddChunk(7, Bytes({42}));
    ASSERT_TRUE(writer.WriteAtomically(goodPath, 1, 91));
    asset::ChunkFileReader reader;
    ASSERT_TRUE(reader.Open(goodPath));
    auto invalidHeader = reader.Header();
    invalidHeader.magic[0] = 'X';
    std::vector<uint8_t> bytes;
    Append(bytes, invalidHeader);
    WriteFileBytes(badPath, bytes);

    EXPECT_FALSE(reader.Open(badPath));

    EXPECT_EQ(reader.Header().sourceStamp, 0u);
    EXPECT_EQ(reader.Header().chunkCount, 0u);
    EXPECT_EQ(reader.Find(7), nullptr);
    EXPECT_TRUE(util::FileSystem::WriteBinary(util::FileSystem::PathFromUtf8(badPath), bytes.data(), bytes.size()));
    ASSERT_TRUE(reader.Open(goodPath));
    std::vector<uint8_t> chunk;
    ASSERT_TRUE(reader.ReadChunk(7, chunk));
    EXPECT_EQ(chunk, Bytes({42}));
}

TEST_F(AssetPartialIoTest, ChunkFileAcceptsEmptyIndexAndEmptyPayloadAtEndOfFile)
{
    const auto path = PathOf("empty.fztc");
    asset::ChunkFileWriter empty;
    ASSERT_TRUE(empty.WriteAtomically(path, 1, 1));
    asset::ChunkFileReader reader;
    ASSERT_TRUE(reader.Open(path));
    EXPECT_EQ(reader.Header().chunkCount, 0u);
    asset::ChunkFileWriter writer;
    writer.AddChunk(7, {});
    ASSERT_TRUE(writer.WriteAtomically(path, 1, 2));

    ASSERT_TRUE(reader.Open(path));

    std::vector<uint8_t> chunk{42};
    EXPECT_TRUE(reader.ReadChunk(7, chunk));
    EXPECT_TRUE(chunk.empty());
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

TEST_F(AssetPartialIoTest, AtomicWriteAllowsRetainedReadersToReadTheirOldSnapshotAfterReplacement)
{
    const std::string path = PathOf("retained.fztc");
    asset::ChunkFileWriter first;
    /// @note Seek beyond the stdio read buffer after replacement so the retained OS handle, not only buffered bytes, is exercised.
    first.AddChunk(99, std::vector<uint8_t>(8192, 0x55));
    first.AddChunk(1, Bytes({1, 2, 3}));
    ASSERT_TRUE(first.WriteAtomically(path, 1, 11));
    asset::ChunkFileReader retained;
    ASSERT_TRUE(retained.Open(path));
    asset::ChunkFileWriter replacement;
    replacement.AddChunk(1, Bytes({7, 8, 9, 10}));

    ASSERT_TRUE(replacement.WriteAtomically(path, 1, 22));

    std::vector<uint8_t> oldBytes, newBytes;
    ASSERT_TRUE(retained.ReadChunk(1, oldBytes));
    EXPECT_EQ(retained.Header().sourceStamp, 11u);
    EXPECT_EQ(oldBytes, Bytes({1, 2, 3}));
    asset::ChunkFileReader current;
    ASSERT_TRUE(current.Open(path));
    ASSERT_TRUE(current.ReadChunk(1, newBytes));
    EXPECT_EQ(current.Header().sourceStamp, 22u);
    EXPECT_EQ(newBytes, Bytes({7, 8, 9, 10}));
    EXPECT_EQ(TemporaryCount(m_temp.Path()), 0u);
}

TEST_F(AssetPartialIoTest, AtomicWriteKeepsValidTargetAndUnrelatedTemporaryWhenAnExternalReaderBlocksRename)
{
    SetLogLevel(core::LogLevel::WARNING);
    testkit::RecordingLogSink sink;
    testkit::ScopedLogSink recording(&sink);
    const std::string path = PathOf("externally-locked.fztc");
    asset::ChunkFileWriter first;
    first.AddChunk(1, Bytes({2, 4, 6}));
    ASSERT_TRUE(first.WriteAtomically(path, 1, 11));
    const auto unrelated = m_temp.File("other.fztc.tmp123_456");
    WriteFileBytes(unrelated.string(), Bytes({42}));
    std::FILE* raw = nullptr;
    ASSERT_EQ(_wfopen_s(&raw, util::FileSystem::PathFromUtf8(path).c_str(), L"rb"), 0);
    ASSERT_NE(raw, nullptr);
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> blocker(raw, &std::fclose);
    asset::ChunkFileWriter replacement;
    replacement.AddChunk(1, Bytes({8, 10}));

    EXPECT_FALSE(replacement.WriteAtomically(path, 1, 22));

    asset::ChunkFileReader current;
    ASSERT_TRUE(current.Open(path));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(current.ReadChunk(1, bytes));
    EXPECT_EQ(current.Header().sourceStamp, 11u);
    EXPECT_EQ(bytes, Bytes({2, 4, 6}));
    const auto* failure = sink.Find("stage=rename");
    ASSERT_NE(failure, nullptr);
    EXPECT_NE(failure->message.find("category=system"), std::string::npos);
    EXPECT_NE(failure->message.find("externally-locked.fztc"), std::string::npos);
    EXPECT_EQ(TemporaryCount(m_temp.Path()), 1u);
    std::vector<uint8_t> preserved;
    ASSERT_TRUE(util::FileSystem::ReadBinary(unrelated, preserved));
    EXPECT_EQ(preserved, Bytes({42}));
}

TEST_F(AssetPartialIoTest, AtomicWriteReportsParentCreationFailureWithoutOpeningATemporary)
{
    SetLogLevel(core::LogLevel::WARNING);
    testkit::RecordingLogSink sink;
    testkit::ScopedLogSink recording(&sink);
    const auto parentFile = m_temp.File("not-a-directory");
    WriteFileBytes(parentFile.string(), Bytes({1, 3, 5}));
    asset::ChunkFileWriter writer;
    writer.AddChunk(1, Bytes({2}));

    EXPECT_FALSE(writer.WriteAtomically(util::FileSystem::PathToUtf8(parentFile / "blocked.fztc"), 1, 1));

    ASSERT_NE(sink.Find("stage=create-parent"), nullptr);
    std::vector<uint8_t> preserved;
    ASSERT_TRUE(util::FileSystem::ReadBinary(parentFile, preserved));
    EXPECT_EQ(preserved, Bytes({1, 3, 5}));
    EXPECT_EQ(TemporaryCount(m_temp.Path()), 0u);
}

TEST_F(AssetPartialIoTest, AtomicWriteSupportsUnicodePathsWithoutChangingThePublishedBytes)
{
    const auto path = m_temp.Path() / std::filesystem::path(L"共有読み込み_キャッシュ.fztc");
    asset::ChunkFileWriter writer;
    writer.AddChunk(1, Bytes({0, 128, 255}));

    ASSERT_TRUE(writer.WriteAtomically(util::FileSystem::PathToUtf8(path), 1, 91));

    asset::ChunkFileReader reader;
    ASSERT_TRUE(reader.Open(util::FileSystem::PathToUtf8(path)));
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(reader.ReadChunk(1, bytes));
    EXPECT_EQ(bytes, Bytes({0, 128, 255}));
}

TEST_F(AssetPartialIoTest, ConcurrentAtomicWritersPublishOneCompletePayloadWithoutLeavingTemporaries)
{
    const std::string path = PathOf("concurrent.fztc");
    asset::ChunkFileWriter first, second;
    first.AddChunk(1, std::vector<uint8_t>(8192, 0x11));
    first.AddChunk(2, Bytes({1, 2}));
    second.AddChunk(1, std::vector<uint8_t>(16384, 0x77));
    second.AddChunk(2, Bytes({7, 8, 9}));
    std::barrier start(3);
    std::array<bool, 2> completed{};
    std::jthread a([&] { start.arrive_and_wait(); completed[0] = first.WriteAtomically(path, 1, 11); });
    std::jthread b([&] { start.arrive_and_wait(); completed[1] = second.WriteAtomically(path, 1, 77); });

    start.arrive_and_wait();
    a.join(); b.join();

    EXPECT_TRUE(completed[0] || completed[1]);
    asset::ChunkFileReader reader;
    ASSERT_TRUE(reader.Open(path));
    const bool firstWon = reader.Header().sourceStamp == 11;
    EXPECT_TRUE(firstWon || reader.Header().sourceStamp == 77);
    std::vector<uint8_t> large, small;
    ASSERT_TRUE(reader.ReadChunk(1, large));
    ASSERT_TRUE(reader.ReadChunk(2, small));
    EXPECT_EQ(large, std::vector<uint8_t>(firstWon ? 8192 : 16384, firstWon ? 0x11 : 0x77));
    EXPECT_EQ(small, firstWon ? Bytes({1, 2}) : Bytes({7, 8, 9}));
    EXPECT_EQ(TemporaryCount(m_temp.Path()), 0u);
}

TEST_F(AssetPartialIoTest, AtomicPublicationDoesNotCleanAnUnownedTemporaryAfterExclusiveOpenFailure)
{
    AtomicChunkIoFake io;
    io.temporaryExists = true;
    io.openFailure = AtomicFailure(std::errc::file_exists, 80);

    const auto result = asset::detail::WriteAtomicChunkFile(io, [](auto& output) { return output.Write(); });

    EXPECT_FALSE(result.published);
    EXPECT_EQ(result.open.code, std::make_error_code(std::errc::file_exists));
    EXPECT_EQ(result.open.nativeCode, 80u);
    EXPECT_FALSE(result.cleanupAttempted);
    EXPECT_TRUE(io.targetUnchanged);
    EXPECT_TRUE(io.temporaryExists);
    EXPECT_EQ(io.calls, (std::vector<std::string>{"parent", "open"}));
}

TEST_F(AssetPartialIoTest, AtomicPublicationPreservesWriteCloseAndCleanupErrorsIndependently)
{
    AtomicChunkIoFake io;
    io.writeFailure = AtomicFailure(std::errc::no_space_on_device, 112);
    io.closeFailure = AtomicFailure(std::errc::io_error, 1117);
    io.cleanupFailure = AtomicFailure(std::errc::permission_denied, 5);

    const auto result = asset::detail::WriteAtomicChunkFile(io, [](auto& output) { return output.Write(); });

    EXPECT_FALSE(result.published);
    EXPECT_EQ(result.write.code, std::make_error_code(std::errc::no_space_on_device));
    EXPECT_EQ(result.write.nativeCode, 112u);
    EXPECT_EQ(result.close.code, std::make_error_code(std::errc::io_error));
    EXPECT_EQ(result.close.nativeCode, 1117u);
    EXPECT_EQ(result.cleanup.code, std::make_error_code(std::errc::permission_denied));
    EXPECT_EQ(result.cleanup.nativeCode, 5u);
    EXPECT_FALSE(io.opened);
    EXPECT_TRUE(io.targetUnchanged);
    EXPECT_TRUE(io.temporaryExists);
    EXPECT_EQ(io.calls, (std::vector<std::string>{"parent", "open", "write", "close", "cleanup"}));
}

TEST_F(AssetPartialIoTest, AtomicPublicationRejectsFlushFailureBeforePublishingAndStillCleansTheClosedTemporary)
{
    AtomicChunkIoFake io;
    io.flushFailure = AtomicFailure(std::errc::no_space_on_device, 112);

    const auto result = asset::detail::WriteAtomicChunkFile(io, [](auto& output) { return output.Write(); });

    EXPECT_FALSE(result.published);
    EXPECT_EQ(result.flush.code, std::make_error_code(std::errc::no_space_on_device));
    EXPECT_EQ(result.flush.nativeCode, 112u);
    EXPECT_TRUE(result.cleanupAttempted);
    EXPECT_FALSE(io.opened);
    EXPECT_TRUE(io.targetUnchanged);
    EXPECT_FALSE(io.temporaryExists);
    EXPECT_EQ(io.calls, (std::vector<std::string>{"parent", "open", "write", "flush", "close", "cleanup"}));
}

TEST_F(AssetPartialIoTest, AtomicPublicationRetainsRenameFailureWhenTemporaryCleanupAlsoFails)
{
    AtomicChunkIoFake io;
    io.renameFailure = AtomicFailure(std::errc::permission_denied, 32);
    io.cleanupFailure = AtomicFailure(std::errc::device_or_resource_busy, 32);

    const auto result = asset::detail::WriteAtomicChunkFile(io, [](auto& output) { return output.Write(); });

    EXPECT_FALSE(result.published);
    EXPECT_EQ(result.publish.code, std::make_error_code(std::errc::permission_denied));
    EXPECT_EQ(result.publish.nativeCode, 32u);
    EXPECT_EQ(result.cleanup.code, std::make_error_code(std::errc::device_or_resource_busy));
    EXPECT_TRUE(io.targetUnchanged);
    EXPECT_TRUE(io.temporaryExists);
    EXPECT_FALSE(io.opened);
    EXPECT_EQ(io.calls, (std::vector<std::string>{"parent", "open", "write", "flush", "close", "rename", "cleanup"}));
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

TEST_F(AssetPartialIoTest, EmptyTextureCacheWritePreservesTheExistingValidCacheAndReportsFailure)
{
    SetLogLevel(core::LogLevel::WARNING);
    testkit::RecordingLogSink sink;
    testkit::ScopedLogSink recording(&sink);
    const auto path = PathOf("keep-valid.fztc");
    const auto full = MakeSingleMip(4, 4);
    ASSERT_TRUE(asset::texturecache::Write(path, 7, full, 2));

    EXPECT_FALSE(asset::texturecache::Write(path, 8, {}, 2));

    ASSERT_NE(sink.Find("has no decoded mip data"), nullptr);
    DecodedTextureRGBA8 current;
    ASSERT_TRUE(asset::texturecache::ReadQuality(path, 7, 0, current));
    ASSERT_EQ(current.mips.size(), 1u);
    EXPECT_EQ(current.mips[0].rgba, full.mips[0].rgba);
    EXPECT_EQ(TemporaryCount(m_temp.Path()), 0u);
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

} /// @note namespace fbzz::tests
