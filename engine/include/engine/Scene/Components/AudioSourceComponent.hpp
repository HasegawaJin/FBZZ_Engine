// FBZZ Engine
// AudioSourceComponent.hpp | fbzz::scene
// 音源データ Component。再生ロジックは scene::AudioSystem が担う
#pragma once
#include <string>

namespace fbzz::scene {

struct AudioSourceComponent {
    std::string clipPath;
    bool        playOnAwake = false;
    bool        loop        = false;
    float       volume      = 1.0f;
    bool        enabled     = true;
    bool        m_played    = false;  // playOnAwake が発火済みか (AudioSystem が管理)
};

} // namespace fbzz::scene
