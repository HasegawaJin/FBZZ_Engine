// FBZZ Engine
// AudioSystem.cpp | fbzz::scene
// AudioSourceComponent を走査して audio::AudioSystem に再生命令を出す
#include "engine/Scene/Systems/AudioSystem.hpp"
#include "engine/Scene/Scene.hpp"
#include "engine/Scene/Components/AudioSourceComponent.hpp"
#include "engine/Audio/AudioSystem.hpp"

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
