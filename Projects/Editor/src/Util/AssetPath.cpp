/// @file    AssetPath.cpp
/// @brief   Editor 内で共有する Assets 起点パスの正規化ユーティリティ。
/// @author  Hasegawa Jin
/// @date    2026-06-07
#include <Editor/Util/AssetPath.hpp>

#include <Engine/Asset/AssetDatabase.hpp>
#include <Engine/Asset/AssetManager.hpp>
#include <Engine/Util/FileSystem.hpp>

#include <cctype>
#include <utility>

namespace fbzz::editor {

std::string SanitizeMaterialFileName(std::string name, uint32_t fallbackIndex)
{
    if (name.empty())
        name = "Material_" + std::to_string(fallbackIndex);
    for (char& c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-' && c != '.')
            c = '_';
    }
    if (name == "." || name == "..")
        name = "Material_" + std::to_string(fallbackIndex);
    return name;
}

std::string SanitizePrefabFileName(const std::string& name)
{
    std::string result = name.empty() ? "Prefab" : name;
    for (char& c : result) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ' ';
        if (!ok) c = '_';
    }
    return result;
}

std::string AssetDirectory(const std::string& path)
{
    std::string directory = util::FileSystem::GetDirectory(path);
    while (directory.size() > 1 && (directory.back() == '/' || directory.back() == '\\'))
        directory.pop_back();
    return directory;
}

std::string NormalizeAssetPath(std::string path)
{
    if (asset::AssetDatabase::IsGuidRef(path)) return path;
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
    if (asset::AssetDatabase::IsGuidRef(assetPath))
        return asset::AssetManager::ResolveAssetPath(std::string(assetPath));
    std::string normalized = NormalizeAssetPath(std::string(assetPath));
    if (normalized.rfind("Assets/", 0) == 0 && !projectRoot.empty())
        return std::string(projectRoot) + "/" + normalized;
    return normalized;
}

} /// @note namespace fbzz::editor
