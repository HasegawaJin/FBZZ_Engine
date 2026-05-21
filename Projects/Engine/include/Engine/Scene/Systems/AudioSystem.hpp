// FBZZ Engine
// AudioSystem.hpp | fbzz::scene
// AudioSourceComponent を走査して audio::AudioSystem に再生命令を出す
#pragma once

namespace fbzz::scene { class Scene; }
namespace fbzz::audio { class AudioSystem; }

namespace fbzz::scene {

void AudioSystem(Scene& scene, audio::AudioSystem& audioSystem, float dt);

} // namespace fbzz::scene
