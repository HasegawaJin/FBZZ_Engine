/// @file    FileDialog.cpp
/// @brief   Win32 ネイティブのファイルダイアログ実装。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#include <Editor/Util/FileDialog.hpp>
#include <Engine/Util/StringUtils.hpp>
#include <Windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

namespace fbzz::editor {

namespace {
// FileFilter 配列を OPENFILENAME.lpstrFilter 形式に変換する
// 形式: "名前\0*.ext\0...\0\0"
std::wstring BuildFilterString(const std::vector<FileFilter>& filters)
{
    std::wstring result;
    for (const auto& f : filters) {
        result += util::StringUtils::ToWide(f.name) + L'\0';
        result += util::StringUtils::ToWide(f.spec) + L'\0';
    }
    result += L'\0';
    return result;
}
} // namespace

bool FileDialog::OpenFile(void* hwnd, const std::vector<FileFilter>& filters,
                          std::string& outPath)
{
    wchar_t buf[MAX_PATH] = {};
    std::wstring filterStr = BuildFilterString(filters);

    OPENFILENAMEW ofn  = {};
    ofn.lStructSize    = sizeof(ofn);
    ofn.hwndOwner      = static_cast<HWND>(hwnd);
    ofn.lpstrFilter    = filterStr.c_str();
    ofn.lpstrFile      = buf;
    ofn.nMaxFile       = MAX_PATH;
    ofn.Flags          = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return false;
    outPath = util::StringUtils::ToNarrow(buf);
    return true;
}

bool FileDialog::SaveFile(void* hwnd, const std::vector<FileFilter>& filters,
                          std::string& outPath)
{
    wchar_t buf[MAX_PATH] = {};
    std::wstring filterStr = BuildFilterString(filters);

    OPENFILENAMEW ofn  = {};
    ofn.lStructSize    = sizeof(ofn);
    ofn.hwndOwner      = static_cast<HWND>(hwnd);
    ofn.lpstrFilter    = filterStr.c_str();
    ofn.lpstrFile      = buf;
    ofn.nMaxFile       = MAX_PATH;
    ofn.Flags          = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) return false;
    outPath = util::StringUtils::ToNarrow(buf);
    return true;
}

} // namespace fbzz::editor
