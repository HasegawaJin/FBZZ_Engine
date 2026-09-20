/// @file    ChunkFile.cpp
/// @brief   チャンク索引付きコンテナの読み書き。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Asset/ChunkFile.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <thread>

namespace fbzz::asset {

namespace {

constexpr std::array<uint32_t, 256> MakeCrcTable()
{
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[n] = c;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

bool WriteAll(std::FILE* file, const void* bytes, std::size_t size)
{
    return size == 0 || std::fwrite(bytes, 1, size, file) == size;
}

} // namespace

uint32_t Crc32(std::span<const uint8_t> bytes)
{
    uint32_t c = 0xFFFFFFFFu;
    for (const uint8_t b : bytes) c = kCrcTable[(c ^ b) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void ChunkFileWriter::AddChunk(uint32_t id, std::vector<uint8_t> bytes)
{
    m_chunks.push_back({ id, std::move(bytes) });
}

bool ChunkFileWriter::WriteAtomically(const std::string& path, uint32_t contentKind, uint64_t sourceStamp) const
{
    namespace fs = std::filesystem;
    const fs::path target = util::FileSystem::PathFromUtf8(path);
    std::error_code error;
    fs::create_directories(target.parent_path(), error);

    /// @note 一時名は書き手ごとに変える。同じキャッシュを 2 本のジョブが同時に書いても互いを壊さない。
    static std::atomic<uint64_t> s_serial{ 0 };
    fs::path temporary = target;
    temporary += L".tmp" + std::to_wstring(std::hash<std::thread::id>{}(std::this_thread::get_id()))
               + L"_" + std::to_wstring(s_serial.fetch_add(1));

    FzChunkFileHeader header{};
    header.magic[0] = 'F'; header.magic[1] = 'Z'; header.magic[2] = 'C'; header.magic[3] = 'K';
    header.version = FZCHUNK_VERSION;
    header.chunkCount = static_cast<uint32_t>(m_chunks.size());
    header.contentKind = contentKind;
    header.sourceStamp = sourceStamp;

    std::vector<FzChunkEntry> entries(m_chunks.size());
    uint64_t offset = sizeof(FzChunkFileHeader) + sizeof(FzChunkEntry) * m_chunks.size();
    for (std::size_t i = 0; i < m_chunks.size(); ++i) {
        entries[i].id = m_chunks[i].id;
        entries[i].flags = FZCHUNK_FLAG_STORED;
        entries[i].offset = offset;
        entries[i].storedSize = m_chunks[i].bytes.size();
        entries[i].rawSize = m_chunks[i].bytes.size();
        entries[i].checksum = Crc32(m_chunks[i].bytes);
        offset += m_chunks[i].bytes.size();
    }

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.wstring().c_str(), L"wb") != 0 || !file) return false;
    bool ok = WriteAll(file, &header, sizeof(header))
           && WriteAll(file, entries.data(), sizeof(FzChunkEntry) * entries.size());
    for (const auto& chunk : m_chunks) ok = ok && WriteAll(file, chunk.bytes.data(), chunk.bytes.size());
    ok = (std::fclose(file) == 0) && ok;
    if (!ok) {
        fs::remove(temporary, error);
        return false;
    }
    /// @note Windows の rename は既存を置き換える (MoveFileExW + MOVEFILE_REPLACE_EXISTING)。
    /// @see https://en.cppreference.com/w/cpp/filesystem/rename std::filesystem::rename
    fs::rename(temporary, target, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

bool ChunkFileReader::Open(const std::string& path)
{
    m_entries.clear();
    if (!m_reader.OpenStreaming(path)) return false;
    if (!m_reader.Read(m_header)) return false;
    if (m_header.magic[0] != 'F' || m_header.magic[1] != 'Z' || m_header.magic[2] != 'C' || m_header.magic[3] != 'K'
        || m_header.version != FZCHUNK_VERSION)
        return false;
    /// @note 壊れた数で巨大な確保をしない。索引がファイルに収まるかを先に確かめる。
    const uint64_t indexBytes = static_cast<uint64_t>(m_header.chunkCount) * sizeof(FzChunkEntry);
    if (sizeof(FzChunkFileHeader) + indexBytes > m_reader.Size()) return false;
    m_entries.resize(m_header.chunkCount);
    if (!m_reader.ReadBytes(m_entries.data(), static_cast<std::size_t>(indexBytes))) return false;
    for (const FzChunkEntry& entry : m_entries)
        if (entry.offset + entry.storedSize > m_reader.Size() || entry.flags != FZCHUNK_FLAG_STORED) return false;
    return true;
}

const FzChunkEntry* ChunkFileReader::Find(uint32_t id) const
{
    for (const FzChunkEntry& entry : m_entries)
        if (entry.id == id) return &entry;
    return nullptr;
}

bool ChunkFileReader::ReadChunk(uint32_t id, std::vector<uint8_t>& out)
{
    const FzChunkEntry* entry = Find(id);
    if (!entry || !m_reader.Seek(entry->offset)) return false;
    out.resize(static_cast<std::size_t>(entry->storedSize));
    if (!m_reader.ReadBytes(out.data(), out.size())) return false;
    return Crc32(out) == entry->checksum;
}

} // namespace fbzz::asset
