/// @file    BinaryReader.hpp
/// @brief   バイナリファイルを一括メモリ読み込みしてカーソルで読み進めるヘルパー。
/// @author  Hasegawa Jin
/// @date    2026-06-18
///
/// WHY: ModelAssetImporter / AnimationImporter で同一構造が重複していたため共通化。
/// Open に util::FileSystem::ReadBinary を使うことで Windows ANSI / UTF-8 混在問題を解消する。
/// std::ifstream(std::string) は MSVC では ANSI コードページで解釈するが、
/// ReadBinary は Win32 Unicode API 経由のため UTF-8 パスを正しく扱える。
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
