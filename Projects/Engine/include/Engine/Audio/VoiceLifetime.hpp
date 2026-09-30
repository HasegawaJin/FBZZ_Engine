/// @file    VoiceLifetime.hpp
/// @brief   所有者と音源の寿命に再生を結び付ける、音声層の失効トークン。
/// @author  Hasegawa Jin
/// @date    2026-09-29
#pragma once

namespace fbzz::audio {
/// @note メインスレッド専用。所有者が active を下げるか、音源が最後の共有参照を返すと失効する。
struct VoiceLifetime {
    bool active = true;
};
} /// @note namespace fbzz::audio
