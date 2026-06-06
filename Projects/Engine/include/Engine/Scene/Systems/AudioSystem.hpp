// FBZZ Engine
// AudioSystem.hpp | fbzz::scene
// AudioSourceComponent から再生命令を発行する Scene System
// Scene のデータを走査し、audio::AudioManager へ高レベル操作を渡す。
// デバイス API 依存は audio モジュール内に閉じ込める。
#pragma once

namespace fbzz::scene { class Scene; }
namespace fbzz::audio { class AudioManager; }

namespace fbzz::scene {

void AudioSystem(Scene& scene, audio::AudioManager& audioManager, float dt);

} // namespace fbzz::scene
