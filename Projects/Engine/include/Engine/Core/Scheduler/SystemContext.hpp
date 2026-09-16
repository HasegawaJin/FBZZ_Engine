/// @file    SystemContext.hpp
/// @brief   全 System への統一パラメータ。SystemScheduler が毎フレーム生成して渡す。
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
    renderer::ResourceManager*  resources;    // LateUpdate のみ非 null
    audio::AudioManager*        audioManager; // SetAudioManager で設定するまで null
    float                       dt;
    float                       fixedDt;      // Physics 固定ステップ時のみ有効
    bool                        simulating;
    // Play セッションが続いているか。Pause 中も true (simulating だけが false になる)。
    // WHY simulating と分けるか: Pause は「シミュレーションを進めない」だけで、編集へ
    //     戻ったわけではない。両者を同じフラグで見ると、ScriptSystem は Pause を
    //     「編集モードへ戻った」と読み、Play 中の Script のライフサイクルを畳んでしまう。
    bool                        playing = false;
    float                       interpolationAlpha = 0.0f; // 固定ステップ間の描画補間係数 [0, 1]
};

} // namespace fbzz
