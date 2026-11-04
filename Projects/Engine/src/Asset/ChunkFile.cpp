/// @file    ChunkFile.cpp
/// @brief   チャンク索引付きコンテナの読み書き。
/// @author  Hasegawa Jin
/// @date    2026-09-19
#include <Engine/Asset/ChunkFile.hpp>
#include <Core/Util/AtomicFileReplace.hpp>
#include "AtomicChunkFileWrite.hpp"
#include <Engine/Core/Logger.hpp>
#include <Engine/Util/FileSystem.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <array>
#include <bit>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstddef>
#include <cstring>
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

/// @brief Capture a CRT failure before the next CRT operation can replace either thread-local error.
detail::AtomicWriteError CrtFailure(int code = errno)
{
    unsigned long nativeCode = 0;
    (void)_get_doserrno(&nativeCode);
    return {{code != 0 ? code : EIO, std::generic_category()}, nativeCode};
}

void ResetCrtFailure()
{
    errno = 0;
    (void)_set_doserrno(0);
}

/// @note No user callback runs while the temporary FILE is open or while a publication/cleanup error is mutable.
class NativeAtomicChunkIo {
public:
    explicit NativeAtomicChunkIo(std::filesystem::path target) : m_target(std::move(target)) {}

    detail::AtomicWriteError CreateParent()
    {
        std::error_code error;
        if (!m_target.parent_path().empty()) std::filesystem::create_directories(m_target.parent_path(), error);
        return {error};
    }

    detail::AtomicWriteError Open()
    {
        static std::atomic<uint64_t> serial{0};
        detail::AtomicWriteError failure;
        /// @note Exclusive create never truncates a residue or another writer's temporary; only name collisions get another name.
        /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fopen-s-wfopen-s C11 x exclusive-create mode.
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            m_temporary = m_target;
            m_temporary += L".tmp" + std::to_wstring(std::hash<std::thread::id>{}(std::this_thread::get_id()))
                         + L"_" + std::to_wstring(serial.fetch_add(1, std::memory_order_relaxed));
            ResetCrtFailure();
            const int code = _wfopen_s(&m_file, m_temporary.c_str(), L"wbx");
            if (code == 0 && m_file) return {};
            failure = CrtFailure(code);
            if (code != EEXIST) break;
        }
        return failure;
    }

    detail::AtomicWriteError Write(const void* bytes, std::size_t size)
    {
        if (size == 0) return {};
        ResetCrtFailure();
        if (std::fwrite(bytes, 1, size, m_file) == size) return {};
        return CrtFailure();
    }

    detail::AtomicWriteError Flush()
    {
        ResetCrtFailure();
        /// @note Flush CRT buffers before close; this does not promise persistence across power loss.
        /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fflush fflush return value.
        if (std::fflush(m_file) == 0) return {};
        return CrtFailure();
    }

    detail::AtomicWriteError Close()
    {
        ResetCrtFailure();
        /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fclose-fcloseall fclose closes the underlying handle even on failure.
        const int result = std::fclose(m_file);
        m_file = nullptr;
        return result == 0 ? detail::AtomicWriteError{} : CrtFailure();
    }

    detail::AtomicWriteError Publish()
    {
        const auto error = util::ReplaceFileAtomic(m_temporary, m_target);
        return {error, error.category() == std::system_category() ? static_cast<DWORD>(error.value()) : 0};
    }

    detail::AtomicWriteError Cleanup()
    {
        std::error_code error;
        /// @note Only the path exclusively created by this call is eligible; do not scan or delete other temporary files.
        std::filesystem::remove(m_temporary, error);
        return {error};
    }

    [[nodiscard]] const std::filesystem::path& TemporaryPath() const { return m_temporary; }

private:
    std::filesystem::path m_target;
    std::filesystem::path m_temporary;
    std::FILE* m_file = nullptr;
};

void ReportAtomicFailure(const std::string& path, const NativeAtomicChunkIo& io,
                         const detail::AtomicWriteResult& result)
{
    const std::string target = path;
    const std::string temporary = util::FileSystem::PathToUtf8(io.TemporaryPath());
    const auto report = [&](const char* stage, const detail::AtomicWriteError& error) {
        if (!error) return;
        FBZZ_LOG_WARN("ChunkFileWriter: stage=%s target='%s' temporary='%s' error=%d category=%s native=%lu message='%s'",
            stage, target.c_str(), temporary.c_str(), error.code.value(), error.code.category().name(),
            error.nativeCode, error.code.message().c_str());
    };
    report("create-parent", result.createParent);
    report("open-exclusive", result.open);
    report("write", result.write);
    report("flush", result.flush);
    report("close", result.close);
    report("rename", result.publish);
    report("cleanup", result.cleanup);
}

} /// @note namespace

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

    NativeAtomicChunkIo io(util::FileSystem::PathFromUtf8(path));
    const auto result = detail::WriteAtomicChunkFile(io, [&](NativeAtomicChunkIo& output) {
        auto error = output.Write(&header, sizeof(header));
        if (!error) error = output.Write(entries.data(), sizeof(FzChunkEntry) * entries.size());
        for (const auto& chunk : m_chunks) {
            if (error) break;
            error = output.Write(chunk.bytes.data(), chunk.bytes.size());
        }
        return error;
    });
    if (!result.published) ReportAtomicFailure(path, io, result);
    return result.published;
}

bool BinaryReader::OpenReplaceableChunkStream(const std::string& path)
{
    Close();
    data.clear();
    pos = 0;
    bytesRead = 0;
    const auto nativePath = util::FileSystem::PathFromUtf8(path);
    /// @note Readers permit delete/rename but not in-place writes; an old reader keeps its old file after publication.
    /// @see https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew FILE_SHARE_DELETE also permits rename.
    const HANDLE file = ::CreateFileW(nativePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    /// @note Ownership transfers once: HANDLE -> CRT descriptor -> FILE; the successful owner alone closes it.
    /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/open-osfhandle _open_osfhandle ownership transfer.
    const int descriptor = _open_osfhandle(std::bit_cast<std::intptr_t>(file), _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        ::CloseHandle(file);
        return false;
    }
    /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fdopen-wfdopen _fdopen shares descriptor ownership with fclose.
    m_file = _fdopen(descriptor, "rb");
    if (!m_file) {
        _close(descriptor);
        return false;
    }
    if (_fseeki64(m_file, 0, SEEK_END) != 0) { Close(); return false; }
    const long long size = _ftelli64(m_file);
    if (size < 0 || _fseeki64(m_file, 0, SEEK_SET) != 0) { Close(); return false; }
    m_fileSize = static_cast<uint64_t>(size);
    return true;
}

bool ChunkFileReader::Open(const std::string& path)
{
    m_entries.clear();
    m_header = {};
    if (!m_reader.OpenReplaceableChunkStream(path)) return false;
    const auto fail = [&] { m_reader.Close(); return false; };
    FzChunkFileHeader header{};
    if (!m_reader.Read(header)) return fail();
    if (header.magic[0] != 'F' || header.magic[1] != 'Z' || header.magic[2] != 'C' || header.magic[3] != 'K'
        || header.version != FZCHUNK_VERSION)
        return fail();
    /// @note 壊れた数で巨大な確保をしない。索引がファイルに収まるかを先に確かめる。
    const uint64_t indexBytes = static_cast<uint64_t>(header.chunkCount) * sizeof(FzChunkEntry);
    const uint64_t payloadStart = sizeof(FzChunkFileHeader) + indexBytes;
    const uint64_t fileSize = m_reader.Size();
    if (payloadStart > fileSize) return fail();
    std::vector<FzChunkEntry> entries(header.chunkCount);
    if (!m_reader.ReadBytes(entries.data(), static_cast<std::size_t>(indexBytes))) return fail();
    /// @note 非負の残量で比較し、offset + storedSize の桁あふれを避ける。v1 の無圧縮本体は索引の後に置く。
    for (const FzChunkEntry& entry : entries)
        if (entry.offset < payloadStart || entry.offset > fileSize || entry.storedSize > fileSize - entry.offset
            || entry.flags != FZCHUNK_FLAG_STORED || entry.rawSize != entry.storedSize) return fail();
    /// @note 一件でも壊れていたら Find / ReadChunk から検証途中の索引を参照させない。
    m_header = header;
    m_entries = std::move(entries);
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

} /// @note namespace fbzz::asset
