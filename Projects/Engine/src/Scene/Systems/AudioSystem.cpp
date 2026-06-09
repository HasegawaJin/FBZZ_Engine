// FBZZ Engine
// AudioSystem.cpp | fbzz::scene
// AudioSourceComponent から再生命令を発行する System
// Scene を走査し、audio::AudioManager へ BGM / SE の操作を渡す。
// オーディオデバイス固有処理は audio モジュール側に閉じ込める。
#include "Engine/Scene/Systems/AudioSystem.hpp"
#include "Engine/Scene/Scene.hpp"
#include "Engine/Scene/Components/AudioSourceComponent.hpp"
#include "Engine/Audio/AudioManager.hpp"

namespace fbzz::scene {

void AudioSystem(Scene& scene, audio::AudioManager& audioManager, [[maybe_unused]] float dt)
{
    for (auto [asc] : scene.View<AudioSourceComponent>()) {
        if (asc.clipPath.empty()) continue;

        // 停止要求を最優先で処理する
        if (asc.m_pendingStop) {
            audioManager.StopBGM();
            asc.m_isPlaying    = false;
            asc.m_isPaused     = false;
            asc.m_pendingStop  = false;
            asc.m_pendingPlay  = false;
            asc.m_pendingPause = false;
            continue;
        }

        // 一時停止要求（BGM のみ対応。StopBGM で停止し状態を記録する）
        if (asc.m_pendingPause) {
            if (asc.m_isPlaying) {
                audioManager.StopBGM();
                asc.m_isPlaying = false;
                asc.m_isPaused  = true;
            }
            asc.m_pendingPause = false;
        }

        // 再生要求または PlayOnAwake の初回発火
        const bool playOnAwakeTrigger = asc.enabled && asc.playOnAwake && !asc.m_played;
        if (asc.m_pendingPlay || playOnAwakeTrigger) {
            if (playOnAwakeTrigger) asc.m_played = true;
            asc.m_pendingPlay = false;
            asc.m_isPaused    = false;

            if (asc.loop) {
                audioManager.SetBGMVolume(asc.volume);
                audioManager.PlayBGM(asc.clipPath, true);
            } else {
                audioManager.SetSEVolume(asc.volume);
                audioManager.PlaySE(asc.clipPath);
            }
            asc.m_isPlaying = true;
        }
    }
}

} // namespace fbzz::scene
