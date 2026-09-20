/// @file    BinaryReader.hpp
/// @brief   バイナリファイルをカーソルで読み進めるヘルパー (一括読み込み / 範囲読み込み)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// ModelAssetImporter / AnimationImporter が重複して持っていた構造を共通化したもの。
/// Open は util::FileSystem::ReadBinary 経由 (Win32 Unicode API) で開く。
/// std::ifstream(std::string) は MSVC では ANSI コードページで解釈し UTF-8 パスを壊すため使わない。
#pragma once
#include <Engine/Util/FileSystem.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbzz::asset {

struct BinaryReader {
    std::vector<uint8_t> data;
    size_t pos = 0;
    /// 実際にディスクから読んだバイト数。範囲読み込みで読み飛ばした分は数えない。
    uint64_t bytesRead = 0;

    BinaryReader() = default;
    ~BinaryReader() { Close(); }
    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;

    /// @brief ファイル全体をメモリへ読む。
    bool Open(const std::string& path) {
        Close();
        pos = 0;
        const bool ok = util::FileSystem::ReadBinary(util::FileSystem::PathFromUtf8(path), data);
        bytesRead = ok ? data.size() : 0;
        return ok;
    }

    /// @brief ファイルを開くだけで読まない。Read / ReadBytes が要る範囲だけを読み、Skip はシークで飛ばす。
    /// @note 部分常駐 (高品質 LOD を読まない) のための入口。パスは UTF-8 (_wfopen_s で開く)。
    /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fseek-fseeki64 _fseeki64
    bool OpenStreaming(const std::string& path) {
        Close();
        data.clear();
        pos = 0;
        bytesRead = 0;
        const std::wstring wide = util::FileSystem::PathFromUtf8(path).wstring();
        if (_wfopen_s(&m_file, wide.c_str(), L"rb") != 0 || !m_file) {
            m_file = nullptr;
            return false;
        }
        if (_fseeki64(m_file, 0, SEEK_END) != 0) { Close(); return false; }
        const long long size = _ftelli64(m_file);
        if (size < 0 || _fseeki64(m_file, 0, SEEK_SET) != 0) { Close(); return false; }
        m_fileSize = static_cast<uint64_t>(size);
        return true;
    }

    [[nodiscard]] uint64_t Size() const { return m_file ? m_fileSize : data.size(); }

    template<typename T>
    bool Read(T& out) {
        return ReadBytes(&out, sizeof(T));
    }

    bool ReadBytes(void* dst, size_t bytes) {
        if (pos + bytes > Size()) return false;
        if (m_file) {
            if (bytes != 0 && std::fread(dst, 1, bytes, m_file) != bytes) return false;
            bytesRead += bytes;
        } else {
            std::memcpy(dst, data.data() + pos, bytes);
        }
        pos += bytes;
        return true;
    }

    bool Skip(size_t bytes) {
        if (pos + bytes > Size()) return false;
        if (m_file && _fseeki64(m_file, static_cast<long long>(pos + bytes), SEEK_SET) != 0) return false;
        pos += bytes;
        return true;
    }

    /// @brief 絶対位置へ移る。前へも後ろへも動ける。
    bool Seek(uint64_t absolute) {
        if (absolute > Size()) return false;
        if (m_file && _fseeki64(m_file, static_cast<long long>(absolute), SEEK_SET) != 0) return false;
        pos = static_cast<size_t>(absolute);
        return true;
    }

    void Close() {
        if (m_file) std::fclose(m_file);
        m_file = nullptr;
        m_fileSize = 0;
    }

private:
    std::FILE* m_file = nullptr;
    uint64_t   m_fileSize = 0;
};

} // namespace fbzz::asset
