// FBZZ Engine
// FileDialog.hpp | fbzz::editor
// Win32 ネイティブのファイル開く/保存ダイアログ
#pragma once
#include <string>
#include <vector>

namespace fbzz::editor {

struct FileFilter {
    std::string name;  // 例: "FBZZ Scene"
    std::string spec;  // 例: "*.fbzz"
};

class FileDialog {
public:
    static bool OpenFile(void* hwnd, const std::vector<FileFilter>& filters,
                         std::string& outPath);
    static bool SaveFile(void* hwnd, const std::vector<FileFilter>& filters,
                         std::string& outPath);
};

} // namespace fbzz::editor
