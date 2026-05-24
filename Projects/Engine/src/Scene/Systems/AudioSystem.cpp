// FBZZ Engine
// AudioSystem.cpp | fbzz::scene
// AudioSourceComponent を走査して audio::AudioSystem に再生命令を出す
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/AudioSourceComponent.hpp"
#include "Engine/Audio/AudioSystem.hpp"

namespace fbzz::scene {

void AudioSystem(Scene& scene, audio::AudioSystem& audioSystem, float dt)
{
    for (auto [asc] : scene.View<AudioSourceComponent>()) {
        if (!asc.enabled || asc.clipPath.empty()) continue;
        if (asc.playOnAwake && !asc.m_played) {
            asc.m_played = true;
            if (asc.loop)
                audioSystem.PlayBGM(asc.clipPath, true);
            else
                audioSystem.PlaySE(asc.clipPath);
        }
    }
}

} // namespace fbzz::scene
