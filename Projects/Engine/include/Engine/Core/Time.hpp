/// @file    Time.hpp
/// @brief   フレーム時間・経過時間・タイムスケール管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// Application::Run() 先頭で Time::Tick() を呼ぶ。以降は Time::deltaTime などを直接参照する。
#pragma once
#include <cstdint>

#ifdef _WIN32
#   ifdef FBZZEngine_EXPORTS
#       define FBZZ_ENGINE_API __declspec(dllexport)
#   else
#       define FBZZ_ENGINE_API __declspec(dllimport)
#   endif
#else
#   define FBZZ_ENGINE_API
#endif

namespace fbzz {

struct FBZZ_ENGINE_API Time {
    // ── 読み取り用 (エンジンが毎フレーム更新する) ─────────────────────────
    static float    deltaTime;          // TimeScale 適用済みフレーム秒
    static float    unscaledDeltaTime;  // TimeScale 未適用フレーム秒
    static float    time;               // 累積秒 (TimeScale 適用済み)
    static float    unscaledTime;       // 累積秒 (TimeScale 未適用)
    static uint64_t frameCount;         // 起動からのフレーム数
    // 固定ステップ 1 回ぶんの秒数 (Phase::Physics の設定 hz の逆数)。
    // Script::OnFixedUpdate() と PhysicsSystem がこの刻みで進む。
    // WHY deltaTime と分けるか: OnFixedUpdate は 1 描画フレームに 0 回にも複数回にも
    //     なるため、そこで deltaTime を使うと積分量が合わない。
    static float    fixedDeltaTime;

    // ── 設定 (読み書き可) ────────────────────────────────────────────────
    // 0=停止, 0.5=スローモーション, 1=通常, 2=2倍速
    //
    // ここは «ゲームと Editor の» 時間倍率。書き手はゲーム側の時間管理
    // (スクリプト / Inspector) だけで、エンジンのシステムはここを書かない。
    static float timeScale;
    // .vfx の VFXTimeScale (ヒットストップ) が要求する倍率。書き手は VFXSystem だけ。
    //
    // WHY timeScale と分けるか (2026-09-11):
    //   以前は VFXSystem が同じ `timeScale` へ直接書いていた。あちらは
    //   `Phase::LateScript` ── スクリプトより **後** ── で走るので、同じフレームに
    //   ゲームが決めたヒットストップやスローを丸ごと踏み潰す。しかも要求が消えた
    //   最初のフレームには 1.0 を書き戻すので、そこでもゲーム側の止めが 1 フレーム
    //   消える。「.vfx を 1 つ足したら別の機能が壊れる」という、原因が絵から
    //   読めない壊れ方になっていた。
    //
    //   1 つの値に書き手が 2 つある限り、順番を入れ替えても «どちらかが負ける» が
    //   残る。入力を 2 本に割って掛け合わせれば、どちらも他方を知らずに済み、
    //   後始末 (1.0 へ戻す) も自分の枠の中で完結する。
    static float vfxTimeScale;
    // 0=無制限, 正値=FPS 上限 (最低 30 FPS にクランプ)
    static int   targetFps;

    // ── エンジン内部用 ───────────────────────────────────────────────────
    static void Tick();    // Application::Run() 先頭で毎フレーム呼ぶ
    static void Reset();   // シーンリセット時に time / frameCount を 0 にする

    // Script / UI から直接 static 変数を書き換えずに済むようにする薄い API。
    // WHY: timeScale / targetFps のクランプ規則を一箇所へ集め、Editor の Inspector と
    //      ScriptProxy が同じ前提で時間制御できるようにする。
    static void  SetTimeScale(float scale);
    static float GetTimeScale();
    /// 実際に deltaTime へ掛かっている倍率 (ゲーム × VFX)。
    ///
    /// WHY 要るか: `GetTimeScale()` は «ゲームが決めた倍率» を返す (Inspector と
    ///     スクリプトが読み書きするのはそれ)。«今フレームの世界がどれだけ遅いか» を
    ///     知りたい側は 2 本の積を見なければならない。
    [[nodiscard]] static float GetEffectiveTimeScale();
    static void  SetTargetFps(int fps);
    static int   GetTargetFps();

private:
    static int64_t s_lastCount;
    static int64_t s_frequency;
};

} // namespace fbzz
