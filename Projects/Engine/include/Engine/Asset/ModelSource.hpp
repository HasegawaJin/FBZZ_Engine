/// @file    ModelSource.hpp
/// @brief   インポート原本として扱うモデル形式の共通判定。
/// @author  Hasegawa Jin
/// @date    2026-10-28
#pragma once
#include <array>
#include <string_view>

namespace fbzz::asset {

inline constexpr std::array<std::string_view, 4> MODEL_SOURCE_EXTENSIONS{
    ".fbx", ".obj", ".gltf", ".glb"
};

/// @note 大文字拡張子も受け付ける。GUID 参照は先に原本パスへ解決すること。
[[nodiscard]] constexpr bool IsModelSourcePath(std::string_view path)
{
    for (const auto extension : MODEL_SOURCE_EXTENSIONS) {
        if (path.size() < extension.size()) continue;
        const auto suffix = path.substr(path.size() - extension.size());
        bool matches = true;
        for (std::size_t i = 0; i < extension.size(); ++i) {
            const char c = suffix[i];
            const char lower = c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
            if (lower != extension[i]) { matches = false; break; }
        }
        if (matches) return true;
    }
    return false;
}

} /// @note namespace fbzz::asset
