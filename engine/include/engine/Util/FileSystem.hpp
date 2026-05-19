// FBZZ Engine
// FileSystem.hpp | fbzz::util
// ファイル・ディレクトリ操作ユーティリティ (Win32)
#pragma once
#include <string>
#include <vector>

namespace fbzz::util {

class FileSystem {
public:
    static bool        Exists(const std::string& path);
    static bool        IsDirectory(const std::string& path);
    static std::string GetExtension(const std::string& path);  // 例: ".fbzz"
    static std::string GetFilename(const std::string& path);   // 例: "scene.fbzz"
    static std::string GetDirectory(const std::string& path);  // 例: "Assets/Scenes/"

    static std::vector<std::string> ListFiles(const std::string& dir,
                                               const std::string& ext = "");

    static bool EnsureDirectory(const std::string& path);

    static bool ReadText(const std::string& path, std::string& out);
    static bool WriteText(const std::string& path, const std::string& text);
};

} // namespace fbzz::util
