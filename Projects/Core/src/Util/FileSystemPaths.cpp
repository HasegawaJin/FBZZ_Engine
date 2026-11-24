/// @file    FileSystemPaths.cpp
/// @brief   パスの変換・正規化・同一性判定。
/// @author  Hasegawa Jin
/// @date    2026-11-21

#include <Core/Util/FileSystem.hpp>
#include <Core/Util/StringUtils.hpp>
#include <cctype>

namespace fbzz::util {

std::string FileSystem::GetExtension(const std::string& path)
{
    size_t extensionStart = path.rfind('.');
    if (extensionStart == std::string::npos) return "";
    return path.substr(extensionStart);
}

std::string FileSystem::GetFilename(const std::string& path)
{
    size_t lastSeparator = path.find_last_of("/\\");
    if (lastSeparator == std::string::npos) return path;
    return path.substr(lastSeparator + 1);
}

std::string FileSystem::GetDirectory(const std::string& path)
{
    size_t lastSeparator = path.find_last_of("/\\");
    if (lastSeparator == std::string::npos) return "";
    return path.substr(0, lastSeparator + 1);
}

std::string FileSystem::NormalizePathSeparators(std::string path, bool trimTrailingSlash)
{
    for (char& character : path) {
        if (character == '\\') character = '/';
    }
    if (trimTrailingSlash) {
        while (path.size() > 1 && path.back() == '/')
            path.pop_back();
    }
    return path;
}

bool FileSystem::SamePathText(const std::string& firstPath, const std::string& secondPath)
{
    return SamePathText(firstPath, secondPath, true);
}

bool FileSystem::SamePathText(std::string_view firstPath, std::string_view secondPath, bool trimTrailingSlash)
{
    const auto isSeparator = [](char value) { return value == '/' || value == '\\'; };
    if (trimTrailingSlash) {
        while (firstPath.size() > 1 && isSeparator(firstPath.back())) firstPath.remove_suffix(1);
        while (secondPath.size() > 1 && isSeparator(secondPath.back())) secondPath.remove_suffix(1);
    }
    if (firstPath.size() != secondPath.size()) return false;
    const auto normalizeCharacter = [](char value) {
        if (value == '\\') return '/';
        return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    };
    for (size_t index = 0; index < firstPath.size(); ++index)
        if (normalizeCharacter(firstPath[index]) != normalizeCharacter(secondPath[index])) return false;
    return true;
}

bool FileSystem::IsChildPathText(const std::string& path, const std::string& root)
{
    const std::string normalizedPath = StringUtils::ToLowerCopy(NormalizePathSeparators(path));
    std::string normalizedRoot = StringUtils::ToLowerCopy(NormalizePathSeparators(root));
    if (normalizedRoot.empty()) return false;
    if (normalizedPath == normalizedRoot) return true;

    normalizedRoot += '/';
    return normalizedPath.starts_with(normalizedRoot);
}

std::filesystem::path FileSystem::PathFromUtf8(const std::string& path)
{
    return std::filesystem::path(StringUtils::ToWide(path));
}

std::string FileSystem::PathToUtf8(const std::filesystem::path& path)
{
    /// @note StringUtils::PathToUtf8 が正規化まで担うため、重複実装を排除して委譲する。
    return util::StringUtils::PathToUtf8(path);
}

std::filesystem::path FileSystem::RelativePath(const std::filesystem::path& path, const std::filesystem::path& root)
{
    std::error_code error;
    const std::filesystem::path relativePath = std::filesystem::relative(path, root, error);
    return error ? std::filesystem::path{} : relativePath;
}

std::filesystem::path FileSystem::MakeAbsolute(const std::filesystem::path& path)
{
    /// @note 失敗時は入力をそのまま返し、呼び出し元にフォールバック処理を課さない。
    std::error_code error;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path, error);
    return error ? path : absolutePath.lexically_normal();
}

bool FileSystem::SamePath(const std::filesystem::path& firstPath, const std::filesystem::path& secondPath)
{
    std::error_code error;
    if (std::filesystem::exists(firstPath, error) && std::filesystem::exists(secondPath, error)) {
        error.clear();
        const bool equivalent = std::filesystem::equivalent(firstPath, secondPath, error);
        if (!error) return equivalent;
    }

    return SamePathText(PathToUtf8(firstPath.lexically_normal()), PathToUtf8(secondPath.lexically_normal()));
}

}
