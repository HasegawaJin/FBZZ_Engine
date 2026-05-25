// FBZZ Engine
// AudioSystem.cpp | fbzz::scene
// AudioSourceComponent から再生命令を発行する System
// Scene を走査し、audio::AudioSystem へ BGM / SE の操作を渡す。
// オーディオデバイス固有処理は audio モジュール側に閉じ込める。
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
