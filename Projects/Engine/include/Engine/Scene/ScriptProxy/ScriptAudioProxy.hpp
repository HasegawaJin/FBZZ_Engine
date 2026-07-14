// FBZZ Engine
// ScriptAudioProxy.hpp | fbzz::scene
// Script から AudioSourceComponent を操作するショートハンド
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptAudioProxy {
    Script* script = nullptr;

    void Play(std::string_view clipPath) const;
    void Stop() const;
    void Pause() const;
    void SetVolume(float v) const;
    void SetLoop(bool loop) const;

    bool  IsPlaying() const;
    float GetVolume() const;
    void  SetPitch(float pitch) const;
    void  SetSpatialBlend(float blend) const;
    void  Set3DDistances(float minDistance, float maxDistance, float rolloff = 1.0f) const;
    void  PlayOneShot(std::string_view clipPath) const;
};

} // namespace fbzz::scene
