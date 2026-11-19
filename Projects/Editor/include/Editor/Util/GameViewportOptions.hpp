/// @file    GameViewportOptions.hpp
/// @brief   Game Viewの保存済み整数値とUI・直接APIの選択肢を対応させる。
/// @author  Hasegawa Jin
/// @date    2026-11-18
#pragma once
#include <array>

namespace fbzz::editor {
struct GameViewportAspectOption {
    const char* key;
    const char* label;
    float ratio;
};
/// @note EditorContext::GameViewportAspectと旧TOMLの整数値の順序を維持する。
inline constexpr std::array<GameViewportAspectOption, 13> GAME_VIEWPORT_ASPECT_OPTIONS{{
    {"free", "Free", 0.0f},
    {"16:9", "16:9", 16.0f / 9.0f},
    {"4:3", "4:3", 4.0f / 3.0f},
    {"1:1", "1:1", 1.0f},
    {"9:16", "9:16", 9.0f / 16.0f},
    {"hd", "HD (1280x720)", 16.0f / 9.0f},
    {"fullHd", "Full HD (1920x1080)", 16.0f / 9.0f},
    {"qhd", "QHD (2560x1440)", 16.0f / 9.0f},
    {"uhd4k", "4K UHD (3840x2160)", 16.0f / 9.0f},
    {"wxga", "WXGA (1280x800)", 16.0f / 10.0f},
    {"wuxga", "WUXGA (1920x1200)", 16.0f / 10.0f},
    {"iPhonePortrait", "iPhone Portrait", 9.0f / 16.0f},
    {"iPhoneLandscape", "iPhone Landscape", 16.0f / 9.0f},
}};
inline constexpr std::array<const char*, 3> PLAY_FOCUS_KEYS{{"focused", "maximized", "unfocused"}};
} /// @note namespace fbzz::editor
