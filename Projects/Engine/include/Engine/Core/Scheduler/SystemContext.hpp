// FBZZ Engine
// SystemContext.hpp | fbzz
// 全 System への統一パラメータ。SystemScheduler が毎フレーム生成して渡す。
#pragma once

namespace fbzz::scene    { class Scene; }
namespace fbzz::physics  { class World; }
namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::audio    { class AudioManager; }

namespace fbzz {

struct SystemContext {
    scene::Scene&               scene;
    physics::World&             world;
    renderer::ResourceManager*  resources;    // LateUpdate のみ非 null
    audio::AudioManager*        audioManager; // SetAudioManager で設定するまで null
    float                       dt;
    float                       fixedDt;      // Physics 固定ステップ時のみ有効
    bool                        simulating;
    float                       interpolationAlpha = 0.0f; // 固定ステップ間の描画補間係数 [0, 1]
};

} // namespace fbzz
