/// @file    BinaryReader.hpp
/// @brief   バイナリファイルをカーソルで読み進めるヘルパー (一括読み込み / 範囲読み込み)。
/// @author  Hasegawa Jin
/// @date    2026-06-18
/// @note ModelAssetImporter / AnimationImporter が共有する一括読み込みと範囲読み込み。
/// @note Open は FileSystem::ReadBinary の Unicode path を使い、MSVC の ANSI narrow path を避ける。
#pragma once
#include <Engine/Api.hpp>
#include <Engine/Util/Files/FileSystem.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fbzz::asset {

struct BinaryReader {
    std::vector<uint8_t> data; ///< メモリ読み取り経路が所有する入力バイト列。
    size_t pos = 0; ///< 入力先頭から次に読み取る位置までのバイトオフセット。
    uint64_t bytesRead = 0; ///< 実際にディスクから読んだバイト数。範囲読み込みで読み飛ばした分は数えない。

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

    /// @pre bytes > 0 なら dst は bytes バイトを書き込める領域を指すこと。bytes == 0 なら nullptr でもよい。
    /// @return 範囲外なら false (位置と dst は未変更)。実 I/O の失敗では dst は未定義となり、ファイルを閉じる。
    /// @note 不完全な fread の後は物理位置が不定となり得るため、再接続せずに後続の読込みを続けない。
    /// @see https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/fread fread の返却件数と失敗時のファイル位置。
    bool ReadBytes(void* dst, size_t bytes) {
        if (!HasRemaining(bytes)) {
            return false;
        }
        if (bytes == 0) {
            return true;
        }
        if (m_file) {
            const size_t transferred = std::fread(dst, 1, bytes, m_file);
            bytesRead += transferred;
            if (transferred != bytes) { Close(); return false; }
        } else {
            std::memcpy(dst, data.data() + pos, bytes);
        }
        pos += bytes;
        return true;
    }

    bool Skip(size_t bytes) {
        if (!HasRemaining(bytes)) {
            return false;
        }
        if (m_file && _fseeki64(m_file, static_cast<long long>(pos + bytes), SEEK_SET) != 0) {
            return false;
        }
        pos += bytes;
        return true;
    }

    /// @brief 絶対位置へ移る。前へも後ろへも動ける。
    bool Seek(uint64_t absolute) {
        if (absolute > Size()) {
            return false;
        }
        if (m_file && _fseeki64(m_file, static_cast<long long>(absolute), SEEK_SET) != 0) {
            return false;
        }
        pos = static_cast<size_t>(absolute);
        return true;
    }

    void Close() {
        if (m_file) {
            std::fclose(m_file);
        }
        m_file = nullptr;
        m_fileSize = 0;
    }

private:
    /// @note Read と Skip の境界を残量で検査し、pos + bytes の桁あふれで範囲外を通さない。
    [[nodiscard]] bool HasRemaining(size_t bytes) const {
        const uint64_t size = Size();
        return pos <= size && bytes <= size - pos;
    }

    friend class ChunkFileReader;
    /// @brief 原子的な置換を許可するのは ChunkFileReader のみ。開いたファイルの内容は置換されても読み続けられる。
    /// @note 汎用の OpenStreaming は従来の共有ポリシーを使う。このヘッダーは Windows の型を公開しない。
    FBZZ_ENGINE_API bool OpenReplaceableChunkStream(const std::string& path);

    std::FILE* m_file = nullptr; ///< ファイル読み取り経路が所有するファイルポインター。未使用時は null。
    uint64_t   m_fileSize = 0; ///< ファイル読み取り経路で記録した入力全体の長さ [byte]。
};

} // namespace fbzz::asset
