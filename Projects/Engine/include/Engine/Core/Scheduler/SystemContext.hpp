/// @file    SystemContext.hpp
/// @brief   SceneManager から Scheduler を経由して各 System に渡す実行コンテキスト。
/// @author  Hasegawa Jin
/// @date    2026-06-18
#pragma once

namespace fbzz::scene    { class Scene; }
namespace fbzz::physics  { class World; }
namespace fbzz::renderer { class ResourceManager; }
namespace fbzz::audio    { class AudioManager; }

namespace fbzz {

struct SystemContext {
    scene::Scene&               scene;
    physics::World&             world;
    renderer::ResourceManager*  resources;    ///< Update と LateUpdate で利用できる。リソース所有元が無ければ null。
    audio::AudioManager*        audioManager; ///< SetAudioManager で設定するまで null
    float                       dt;
    float                       fixedDt;      ///< 固定ステップの実行中に Scheduler が設定する秒数。
    bool                        simulating;
    /// @note Pause 中も Play セッションと Script のライフサイクルは続く。simulating だけが false になる。
    bool                        playing = false;
    float                       interpolationAlpha = 0.0f; ///< 固定ステップ間の描画補間係数 [0, 1]
};

} /// @note namespace fbzz
