// FBZZ Engine
// HubUtil.cpp | fbzz::hub::util
// GameHub 内で共有する Win32 / UTF-8 / filesystem 変換ユーティリティ
#include "HubUtil.hpp"
#include <Engine/Util/FileSystem.hpp>
#include <Engine/Util/StringUtils.hpp>

namespace fbzz::hub::util {

std::wstring Utf8ToWide(const std::string& text)
{
    // WHY: 文字コード変換は Engine/Util/StringUtils に集約し、Hub 独自実装との挙動差をなくす。
    return fbzz::util::StringUtils::ToWide(text);
}

std::string WideToUtf8(const std::wstring& text)
{
    // WHY: Windows API 境界の narrow 化も Engine 側の UTF-8 変換へ寄せる。
    return fbzz::util::StringUtils::ToNarrow(text);
}

std::filesystem::path GetExecutableDirectory()
{
    // WHY: 実行ファイル基準の探索は Editor / Engine と同じ FileSystem 実装を使う。
    return fbzz::util::FileSystem::GetExecutableDirectory();
}

} // namespace fbzz::hub::util
