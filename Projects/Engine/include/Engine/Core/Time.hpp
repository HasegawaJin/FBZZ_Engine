/// @file    Time.hpp
/// @brief   フレーム時間・経過時間・タイムスケール管理。
/// @author  Hasegawa Jin
/// @date    2026-05-21
/// @note    Application::Run() 先頭で Time::Tick() を呼ぶ。以降は Time::deltaTime などを直接参照する。
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
    /// @name 読み取り用 (エンジンが毎フレーム更新する)
    /// @{
    static float    deltaTime;          ///< TimeScale 適用済みフレーム秒
    static float    unscaledDeltaTime;  ///< TimeScale 未適用フレーム秒
    static float    time;               ///< 累積秒 (TimeScale 適用済み)
    static float    unscaledTime;       ///< 累積秒 (TimeScale 未適用)
    static uint64_t frameCount;         ///< 起動からのフレーム数
    /// @brief 固定ステップ 1 回ぶんの秒数 (Phase::Physics の hz の逆数)。
    /// @note OnFixedUpdate は 1 描画フレームに 0 回にも複数回にもなるため、deltaTime で積分すると量が合わない。
    static float    fixedDeltaTime;
    /// @}

    /// @name 設定 (読み書き可)
    /// @{
    /// @brief ゲームと Editor の時間倍率。0=停止 / 1=通常。書き手はスクリプトと Inspector だけ。
    static float timeScale;
    /// @brief .vfx の VFXTimeScale (ヒットストップ) の倍率。書き手は VFXSystem だけ。
    /// @note timeScale と分けるのは、書き手が 2 つある 1 変数は順番を入れ替えても «どちらかが負ける» ため。2 本の積で掛ける。
    static float vfxTimeScale;
    /// @brief 0=無制限、正値=FPS 上限 (最低 30 にクランプ)。
    static int   targetFps;
    /// @}

    /// @brief 毎フレームの時間を進める。Application::Run() 先頭で呼ぶ。
    static void Tick();
    /// @brief time / frameCount を 0 に戻す。timeScale は Inspector のスロー再生を黙って解除しないよう残す。
    static void Reset();

    /// @note クランプ規則を 1 か所に集め、Inspector と ScriptProxy が同じ前提で時間を制御できるようにする。
    static void  SetTimeScale(float scale);
    static float GetTimeScale();
    /// @brief 実際に deltaTime へ掛かっている倍率 (ゲーム × VFX)。
    [[nodiscard]] static float GetEffectiveTimeScale();
    static void  SetTargetFps(int fps);
    static int   GetTargetFps();

    /// @brief 実時間を測らず、毎フレーム固定の unscaledDeltaTime で進める (Playtest のロックステップ)。
    /// @param seconds 0 以下で解除。正値の間は FPS 上限の待機もしない。
    /// @note «N フレーム後» と «N×seconds 秒後» を一致させ、描画の遅い機械 (WARP / CI) でもゲーム内の進みを揃える。
    /// @see Docs/design/ai-verification-loop.md
    static void  SetLockstepDelta(float seconds);
    [[nodiscard]] static float GetLockstepDelta();

private:
    static int64_t s_lastCount;
    static int64_t s_frequency;
    static float   s_lockstepDelta;
};

} // namespace fbzz
