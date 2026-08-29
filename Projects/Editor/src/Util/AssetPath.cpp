/// @file    AssetPath.cpp
/// @brief   Editor 内で共有する Assets 起点パスの正規化ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include <Editor/Util/AssetPath.hpp>

#include <Engine/Util/FileSystem.hpp>

#include <utility>

namespace fbzz::editor {

std::string NormalizeAssetPath(std::string path)
{
    path = util::FileSystem::NormalizePathSeparators(std::move(path));

    if (path.rfind("Assets/", 0) == 0)
        return path;

    if (path.rfind("assets/", 0) == 0) {
        path[0] = 'A';
        return path;
    }

    const std::string marker = "/Assets/";
    const size_t assetsPos = path.find(marker);
    if (assetsPos != std::string::npos)
        return path.substr(assetsPos + 1);

    const std::string lowerMarker = "/assets/";
    const size_t lowerAssetsPos = path.find(lowerMarker);
    if (lowerAssetsPos != std::string::npos) {
        std::string result = path.substr(lowerAssetsPos + 1);
        result[0] = 'A';
        return result;
    }

    return path;
}

std::string ToProjectAssetDiskPath(std::string_view projectRoot, std::string_view assetPath)
{
    std::string normalized = NormalizeAssetPath(std::string(assetPath));
    if (normalized.rfind("Assets/", 0) == 0 && !projectRoot.empty())
        return std::string(projectRoot) + "/" + normalized;
    return normalized;
}

} // namespace fbzz::editor
