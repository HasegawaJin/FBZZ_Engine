/// @file    EngineAssetPath.cpp
/// @brief   Engine アセットルートの探索と相対パス解決の実装。
/// @author  Hasegawa Jin
/// @date    2026-09-02
#include <Engine/Util/EngineAssetPath.hpp>
#include <Engine/Util/FileSystem.hpp>
#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>
#include <Windows.h>

namespace fbzz::util {

namespace {

bool IsRegularFile(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool IsDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

/// 先頭要素が "Assets" なら取り除く。EngineAssetRoot 自体が Assets を指すため、
/// "Assets/Shaders/..." をそのまま連結すると Assets が二重になる。
std::filesystem::path StripAssetsPrefix(const std::filesystem::path& path)
{
    auto it = path.begin();
    if (it == path.end()) return {};

    std::string head = it->string();
    std::transform(head.begin(), head.end(), head.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (head != "assets") return {};

    std::filesystem::path rest;
    for (++it; it != path.end(); ++it) rest /= *it;
    return rest;
}

std::filesystem::path Discover()
{
    const DWORD required = GetEnvironmentVariableW(L"FBZZ_ENGINE_ASSET_ROOT", nullptr, 0);
    if (required > 1) {
        std::wstring value(required, L'\0');
        const DWORD written =
            GetEnvironmentVariableW(L"FBZZ_ENGINE_ASSET_ROOT", value.data(), required);
        if (written > 0 && written < required) {
            value.resize(written);
            return std::filesystem::path(value);
        }
    }

    const std::filesystem::path executableDirectory = FileSystem::GetExecutableDirectory();
    if (executableDirectory.empty()) return {};

    const std::filesystem::path staged = executableDirectory / L"EngineAssets";
    if (IsDirectory(staged)) return staged;

    /// @note SDK Editor は `tools/<Config>/Editor` にあり、共有 asset は SDK root/share 配下にある。
    const std::filesystem::path sdk =
        executableDirectory.parent_path().parent_path().parent_path()
        / L"share" / L"fbzz" / L"Assets";
    return IsDirectory(sdk) ? sdk : std::filesystem::path{};
}

} // namespace

const std::filesystem::path& EngineAssetRoot()
{
    static const std::filesystem::path root = Discover();
    return root;
}

std::filesystem::path ResolveEngineAssetPath(const std::filesystem::path& requested)
{
    if (IsRegularFile(requested)) return requested;
    if (requested.is_absolute()) return requested;

    std::error_code error;
    std::filesystem::path current = std::filesystem::current_path(error);
    if (!error) {
        for (;;) {
            const std::filesystem::path candidate = current / requested;
            if (IsRegularFile(candidate)) return candidate;
            const std::filesystem::path parent = current.parent_path();
            if (parent.empty() || parent == current) break;
            current = parent;
        }
    }

    /// @note GameHub から起動した Editor は GameHub の CWD を引き継ぎ、Engine シェーダーは複製せず
    ///       immutable な SDK 共有 asset を使う方針のため、CWD 起点の探索が届かない構成がある。
    ///       最後に SDK ルートへ問い合わせて解決する。
    const std::filesystem::path& root = EngineAssetRoot();
    if (!root.empty()) {
        if (const std::filesystem::path stripped = StripAssetsPrefix(requested); !stripped.empty()) {
            const std::filesystem::path candidate = root / stripped;
            if (IsRegularFile(candidate)) return candidate;
        }
        const std::filesystem::path candidate = root / requested;
        if (IsRegularFile(candidate)) return candidate;
    }

    return requested;
}

} // namespace fbzz::util
