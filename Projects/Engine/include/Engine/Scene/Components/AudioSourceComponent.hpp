// FBZZ Engine
// AudioSourceComponent.hpp | fbzz::scene
// Audio source data component
#pragma once
#include <Engine/Scene/Script.hpp>
#include <string>

namespace fbzz::scene {

struct AudioSourceComponent {
    std::string clipPath;
    bool        playOnAwake = false;
    bool        loop        = false;
    float       volume      = 1.0f;
    bool        enabled     = true;
    bool        m_played    = false;

    const char* GetTypeName() const { return "Audio Source"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        r.Field("clipPath", clipPath);
        r.Field("playOnAwake", playOnAwake);
        r.Field("loop", loop);
        r.Field("volume", volume);
    }
};

} // namespace fbzz::scene
