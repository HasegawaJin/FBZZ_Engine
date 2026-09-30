/// @file    LoopVoice.hpp
/// @brief   GreenWare のループ音の出力設定を Engine の所有音声 API へ渡す。
/// @author  Hasegawa Jin
/// @date    2026-08-26
#pragma once
#include <Engine/Scene/Script.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace sandbox::se {
class LoopVoice {
public:
    void SetKey(std::string key) { m_settings.label = std::move(key); }
    void SetOutput(std::string bus, float spatial)
    {
        m_settings.bus = std::move(bus);
        m_settings.spatialBlend = spatial;
    }
    void Update(fbzz::scene::Script& owner, std::string_view path, float volume, float pitch = 1.0f)
    {
        (void)owner.audio.UpdateLoop(m_loop, path, volume, pitch, m_settings);
    }
    void Stop(const fbzz::scene::Script& owner) { owner.audio.StopLoop(m_loop); }

private:
    fbzz::scene::AudioLoop m_loop;
    fbzz::scene::AudioLoopSettings m_settings;
};
} /// @note namespace sandbox::se
