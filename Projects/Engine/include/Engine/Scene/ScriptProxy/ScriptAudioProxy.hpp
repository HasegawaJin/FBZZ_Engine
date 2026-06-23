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
    void  PlayOneShot(std::string_view clipPath) const;
};

} // namespace fbzz::scene
