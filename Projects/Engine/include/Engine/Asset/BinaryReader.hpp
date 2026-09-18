/// @file    BinaryReader.hpp
/// @brief   バイナリファイルを一括メモリ読み込みしてカーソルで読み進めるヘルパー。
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
#include <cstring>
#include <string>
#include <vector>

namespace fbzz::asset {

struct BinaryReader {
    std::vector<uint8_t> data;
    size_t pos = 0;

    bool Open(const std::string& path) {
        return util::FileSystem::ReadBinary(
            util::FileSystem::PathFromUtf8(path), data);
    }

    template<typename T>
    bool Read(T& out) {
        if (pos + sizeof(T) > data.size()) return false;
        std::memcpy(&out, data.data() + pos, sizeof(T));
        pos += sizeof(T);
        return true;
    }

    bool ReadBytes(void* dst, size_t bytes) {
        if (pos + bytes > data.size()) return false;
        std::memcpy(dst, data.data() + pos, bytes);
        pos += bytes;
        return true;
    }

    bool Skip(size_t bytes) {
        if (pos + bytes > data.size()) return false;
        pos += bytes;
        return true;
    }
};

} // namespace fbzz::asset
