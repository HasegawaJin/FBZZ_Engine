/// @file    ScriptTweenProxy.hpp
/// @brief   Script から 3D オブジェクトの補間アニメーションをコルーチンとして回す。
/// @author  Hasegawa Jin
/// @date    2026-08-16

/// @note UIAnimator の Tween は UI (RectTransform/色) 専用で 3D Transform には使えないため別に
/// @note 用意した。既存の StartCoroutine/UpdateCoroutines に乗るので専用マネージャーが増えず、
/// @note Script 破棄で動作中の Tween も自動的に止まる。
/// @note Coroutine 自体は awaiter ではなく co_await tween.MoveTo(...) とは書けない。順番に
/// @note 繋ぐには自前のコルーチンから WaitForSeconds で所要時間だけ待つこと。
#pragma once
#include <Engine/Api.hpp>

#include <Engine/Scene/Coroutine.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <functional>

namespace fbzz::scene {

class Script;

/// @note 補間曲線。util::Easing の関数へ .cpp 側で対応付ける。
/// @note 関数ポインタを直接受けない理由: スクリプト DLL 側のアドレスをコルーチンが保持
/// @note すると、ホットリロードで無効なアドレスを呼ぶため、値として渡せる enum に閉じる。
enum class TweenEase {
    Linear,
    InQuad,    OutQuad,    InOutQuad,
    InCubic,   OutCubic,   InOutCubic,
    InSine,    OutSine,    InOutSine,
    InExpo,    OutExpo,    InOutExpo,
    InBack,    OutBack,    InOutBack,
    OutElastic,
    OutBounce,
};

/// @note 補間に使う時間軸。ヒットストップ (timeScale = 0) 中も動かしたい UI 的な演出は Unscaled。
enum class TweenClock {
    Scaled,
    Unscaled,
};

struct ScriptTweenProxy {
    Script* script = nullptr;

    /// @name Transform
    /// @{
    /// @note すべてローカル空間。duration <= 0 なら即座に終値へ飛ぶ。
    [[nodiscard]] FBZZ_ENGINE_API Coroutine MoveTo(math::Vector3 target, float duration,
                                   TweenEase ease = TweenEase::Linear,
                                   TweenClock clock = TweenClock::Scaled) const;
    [[nodiscard]] FBZZ_ENGINE_API Coroutine MoveBy(math::Vector3 delta, float duration,
                                   TweenEase ease = TweenEase::Linear,
                                   TweenClock clock = TweenClock::Scaled) const;
    [[nodiscard]] FBZZ_ENGINE_API Coroutine ScaleTo(math::Vector3 target, float duration,
                                    TweenEase ease = TweenEase::Linear,
                                    TweenClock clock = TweenClock::Scaled) const;
    [[nodiscard]] FBZZ_ENGINE_API Coroutine RotateTo(math::Quaternion target, float duration,
                                     TweenEase ease = TweenEase::Linear,
                                     TweenClock clock = TweenClock::Scaled) const;
    /// @}

    /// @name 任意の値
    /// @{
    /// @note from → to を補間し、更新フレームごとに apply へ1回渡す。終了フレームは正確な to を1回渡す。
    /// @note duration が0以下・NaNなら生成時に to を1回渡す。マテリアルのディゾルブ量、
    /// @note ライトの強度、UI のフィル量など Transform 以外の対象に使う。
    [[nodiscard]] FBZZ_ENGINE_API Coroutine Value(float from, float to, float duration,
                                  std::function<void(float)> apply,
                                  TweenEase ease = TweenEase::Linear,
                                  TweenClock clock = TweenClock::Scaled) const;
    /// @}

    /// @name 揺らし
    /// @{
    /// @note 開始位置を中心に duration 秒かけて減衰しながら振動させ、最後に元の位置へ戻す。
    /// @note 被弾表現やスイッチの手応えなど、カメラではなくオブジェクト自身を揺らす用途。
    [[nodiscard]] FBZZ_ENGINE_API Coroutine ShakePosition(float amplitude, float duration,
                                          float frequency = 25.0f,
                                          TweenClock clock = TweenClock::Scaled) const;

    /// @note 補間係数の評価。自前で補間を書くときに曲線だけ借りる用途。
    [[nodiscard]] FBZZ_ENGINE_API static float Evaluate(TweenEase ease, float t);
    /// @}
};

} /// @note namespace fbzz::scene
