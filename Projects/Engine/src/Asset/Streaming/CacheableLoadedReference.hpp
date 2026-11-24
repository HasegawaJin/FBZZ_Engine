/// @file    CacheableLoadedReference.hpp
/// @brief   読み込み済みアセットの索引付き参照だけを再利用対象にする。
/// @author  Hasegawa Jin
/// @date    2026-11-06
#pragma once
#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace fbzz::asset::detail {
/// @note 読込成功後にのみ呼ぶ。GUID から解決した実行時の Assets パスは、版またはアセット世代が変わるまで索引上の識別を維持する。
/// @note 相対参照の代替解決、未登録ファイル、別表記では従来のファイルシステムの優先順位を使う。失敗結果は保持しない。
/// @see Docs/design/instanced-mesh.md
inline bool IsCacheableLoadedReference(const std::string& reference, std::string_view resolvedPath = {},
    std::string* cachedPath = nullptr)
{
    /// @note 起動時だけ選べるポーリングは、索引通知のないファイル駆動型ホストと同一バイナリでの計測に使う。
    static const bool polling = [] {
        const auto* mode = std::getenv("FBZZ_LOADED_REFERENCE_MODE");
        return mode && std::string_view(mode) == "poll";
    }();
    if (polling) return false;
    if (AssetDatabase::IsGuidRef(reference)) {
        if (cachedPath) *cachedPath = resolvedPath.empty() ? AssetManager::ResolveAssetPath(reference) : std::string(resolvedPath);
        return true;
    }
    if (!reference.starts_with("Assets/") || reference.find("/../") != std::string::npos
        || reference.find("/./") != std::string::npos || reference.find('\\') != std::string::npos) return false;
    const auto root = AssetDatabase::AssetsRoot();
    if (root.empty()) return false;
    const auto expected = root + reference.substr(7);
    const auto guid = AssetDatabase::TryGetGuidFromPath(expected);
    if (guid.empty()) return false;
    const auto same = [](std::string_view first, std::string_view second) {
        if (first.size() != second.size()) return false;
        for (size_t i = 0; i < first.size(); ++i) {
            const auto fold = [](unsigned char c) { return c == '\\' ? '/' : std::tolower(c); };
            if (fold(first[i]) != fold(second[i])) return false;
        }
        return true;
    };
    if (!same(AssetDatabase::PathFromGuid(guid), expected)) return false;
    const auto resolved = resolvedPath.empty() ? AssetManager::ResolveAssetPath(reference) : std::string{};
    const auto actual = resolvedPath.empty() ? std::string_view(resolved) : resolvedPath;
    if (!same(actual, expected)) return false;
    if (cachedPath) *cachedPath = actual;
    return true;
}
} /// @note namespace fbzz::asset::detail
