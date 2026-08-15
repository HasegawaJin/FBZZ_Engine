// FBZZ Engine
// ScriptTweenProxy.hpp | fbzz::scene
// Script から 3D オブジェクトの補間アニメーションをコルーチンとして回す
//
// 設計意図 (WHY):
//   Tween は UIAnimator に既にあるが、あれは UI 要素 (RectTransform / 色) 専用で、
//   3D の Transform には使えなかった。そのため「扉が 0.4 秒かけて開く」のような
//   演出を毎回 OnUpdate に経過時間の変数を足して手書きすることになっていた。
//
//   コルーチンを返す形にするのは、Tween 専用のマネージャーとその更新順序を
//   新設せずに済ませるため。既にある StartCoroutine / TickCoroutines に乗るだけで、
//   Script が破棄されれば動作中の Tween も一緒に止まる (寿命管理が増えない)。
//
//     void OnStart() override { StartCoroutine(tween.MoveTo({ 0, 3, 0 }, 0.4f,
//                                                           TweenEase::OutCubic)); }
//
//   順番に繋ぎたい場合は、自前のコルーチンから所要時間ぶん待つ。
//   WHY co_await tween.MoveTo(...) と書けないか: Coroutine 自体は awaiter ではなく、
//   co_await できるのは WaitForSeconds などの待機命令だけだから。
//
//     Coroutine OpenDoor() {
//         StartCoroutine(tween.MoveTo({ 0, 3, 0 }, 0.4f, TweenEase::OutCubic));
//         co_await WaitForSeconds(0.4f);
//         audio.Play("Assets/Audio/door_locked.wav");
//     }
#pragma once

#include <Engine/Scene/Coroutine.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <functional>

namespace fbzz::scene {

class Script;

// 補間曲線。util::Easing の関数へ .cpp 側で対応付ける。
// WHY 関数ポインタを直接受けないか: スクリプト DLL 側の関数アドレスを
//     エンジンのコルーチンが保持すると、ホットリロードで無効なアドレスを呼ぶ。
//     値として渡せる enum に閉じておけば、その経路が生まれない。
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

// 補間に使う時間軸。ヒットストップ (timeScale = 0) 中も動かしたい UI 的な演出は Unscaled。
enum class TweenClock {
    Scaled,
    Unscaled,
};

struct ScriptTweenProxy {
    Script* script = nullptr;

    // ── Transform ─────────────────────────────────────────────────────────
    // すべてローカル空間。duration <= 0 なら即座に終値へ飛ぶ。
    [[nodiscard]] Coroutine MoveTo(math::Vector3 target, float duration,
                                   TweenEase ease = TweenEase::Linear,
                                   TweenClock clock = TweenClock::Scaled) const;
    [[nodiscard]] Coroutine MoveBy(math::Vector3 delta, float duration,
                                   TweenEase ease = TweenEase::Linear,
                                   TweenClock clock = TweenClock::Scaled) const;
    [[nodiscard]] Coroutine ScaleTo(math::Vector3 target, float duration,
                                    TweenEase ease = TweenEase::Linear,
                                    TweenClock clock = TweenClock::Scaled) const;
    [[nodiscard]] Coroutine RotateTo(math::Quaternion target, float duration,
                                     TweenEase ease = TweenEase::Linear,
                                     TweenClock clock = TweenClock::Scaled) const;

    // ── 任意の値 ──────────────────────────────────────────────────────────
    // from → to を補間し、毎フレーム apply へ渡す。マテリアルのディゾルブ量、
    // ライトの強度、UI のフィル量など Transform 以外の対象に使う。
    [[nodiscard]] Coroutine Value(float from, float to, float duration,
                                  std::function<void(float)> apply,
                                  TweenEase ease = TweenEase::Linear,
                                  TweenClock clock = TweenClock::Scaled) const;

    // ── 揺らし ────────────────────────────────────────────────────────────
    // 開始位置を中心に duration 秒かけて減衰しながら振動させ、最後に元の位置へ戻す。
    // 被弾表現やスイッチの手応えなど、カメラではなくオブジェクト自身を揺らす用途。
    [[nodiscard]] Coroutine ShakePosition(float amplitude, float duration,
                                          float frequency = 25.0f,
                                          TweenClock clock = TweenClock::Scaled) const;

    // 補間係数の評価。自前で補間を書くときに曲線だけ借りる用途。
    [[nodiscard]] static float Evaluate(TweenEase ease, float t);
};

} // namespace fbzz::scene
