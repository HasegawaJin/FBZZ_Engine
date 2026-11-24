/// @file    FileDialog.hpp
/// @brief   Win32 ネイティブのファイル開く/保存ダイアログ。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <string>
#include <vector>

namespace fbzz::editor {

struct FileFilter {
    std::string name;  ///< 例: "FBZZ Scene"
    std::string spec;  ///< 例: "*.fbzz"
};

class FileDialog {
public:
    static bool OpenFile(void* hwnd, const std::vector<FileFilter>& filters,
                         std::string& outPath);
    static bool SaveFile(void* hwnd, const std::vector<FileFilter>& filters,
                         std::string& outPath);
};

} // namespace fbzz::editor
