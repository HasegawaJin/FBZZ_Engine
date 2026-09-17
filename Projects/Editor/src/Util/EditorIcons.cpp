/// @file    EditorIcons.cpp
/// @brief   記号フォントが読めたかどうかの記録と、読めないときの文字への落とし方。
/// @author  Hasegawa Jin
/// @date    2026-09-12
#include <Editor/Util/EditorIcons.hpp>

namespace fbzz::editor::icons {

namespace detail {

/// EditorTheme::Apply() が merge の成否を書く。
/// @note ヘッダーに出さない: 書くのはフォントを積む 1 か所だけで、それ以外から触れると «アイコンがあることにする» 抜け道ができる。
bool g_iconFontLoaded = false;

} // namespace detail

bool Available()
{
    return detail::g_iconFontLoaded;
}

const char* Or(const detail::Glyph& glyph, const char* fallback)
{
    return detail::g_iconFontLoaded ? glyph.text : fallback;
}

} // namespace fbzz::editor::icons
