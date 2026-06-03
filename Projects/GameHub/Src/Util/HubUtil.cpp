// FBZZ Engine
// HubUtil.cpp | fbzz::hub::util
// GameHub 内で共有する Win32 / UTF-8 / filesystem 変換ユーティリティ
#include "HubUtil.hpp"

#include <Windows.h>

namespace fbzz::hub::util {

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        // WHY: 変換失敗時は filesystem 経由のフォールバックで ASCII 文字列だけでも通す。
        return std::filesystem::path(text).wstring();
    }

    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return std::filesystem::path(text).string();
    }

    std::string utf8(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::filesystem::path GetExecutableDirectory()
{
    // WHY: GetModuleFileNameW(nullptr) は現在の exe のフルパスを返す。
    //      parent_path() でディレクトリを取り出し、アセット・設定ファイルの基点として使う。
    wchar_t buffer[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

} // namespace fbzz::hub::util
