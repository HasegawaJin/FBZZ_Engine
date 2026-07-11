// FBZZ Engine
// UIAnimator.hpp | fbzz::scene
// UIImage 向け Tween アニメーションコンポーネント
// 色と位置の変化を時間で補間し、UIAnimatorSystem が結果を書き込む。
// UI 表示そのものは UISystem に委譲する。
#pragma once
#include <Engine/Scene/Script.hpp>
#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

enum class UIEasingType { Linear, EaseIn, EaseOut, EaseInOut };

struct UIColorTween {
    math::Vector4 from     = { 1, 1, 1, 1 };
    math::Vector4 to       = { 1, 1, 1, 0 };
    float         duration = 1.0f;
    float         elapsed  = 0.0f;
    UIEasingType  easing   = UIEasingType::Linear;
    bool          loop     = false;
    // ループごとに from/to を反転する。往復アニメーションに使う。
    // 単純な loop との違い: loop は終端でリセット、pingPong は折り返す。
    bool          pingPong = false;
    bool          active   = false; // false のとき UIAnimatorSystem はこのトゥイーンをスキップする
};

struct UIPositionTween {
    math::Vector2 from     = { 0, 0 };
    math::Vector2 to       = { 100, 0 };
    float         duration = 1.0f;
    float         elapsed  = 0.0f;
    UIEasingType  easing   = UIEasingType::Linear;
    bool          loop     = false;
    bool          pingPong = false;
    bool          active   = false;
};

struct UIScaleTween {
    math::Vector2 from     = { 1, 1 };
    math::Vector2 to       = { 1.2f, 1.2f };
    float         duration = 0.3f;
    float         elapsed  = 0.0f;
    UIEasingType  easing   = UIEasingType::EaseOut;
    bool          loop     = false;
    bool          pingPong = false;
    bool          active   = false;
};

struct UIAnimator {
    UIColorTween    colorTween;
    UIPositionTween positionTween;
    UIScaleTween    scaleTween;
    bool            enabled = true;

    // Tween 再生 API。ScriptProxy / Inspector ボタンの両方から同じ入口を使えるようにする。
    // WHY: elapsed と active の初期化規則を呼び出し側へ分散させると、途中再生・ループ・
    //      pingPong の挙動が API ごとにずれるため。
    void PlayColor(const math::Vector4& from, const math::Vector4& to,
                   float duration, UIEasingType easing = UIEasingType::Linear,
                   bool loop = false, bool pingPong = false)
    {
        colorTween.from = from;
        colorTween.to = to;
        colorTween.duration = duration;
        colorTween.elapsed = 0.0f;
        colorTween.easing = easing;
        colorTween.loop = loop;
        colorTween.pingPong = pingPong;
        colorTween.active = true;
        enabled = true;
    }

    void PlayPosition(const math::Vector2& from, const math::Vector2& to,
                      float duration, UIEasingType easing = UIEasingType::Linear,
                      bool loop = false, bool pingPong = false)
    {
        positionTween.from = from;
        positionTween.to = to;
        positionTween.duration = duration;
        positionTween.elapsed = 0.0f;
        positionTween.easing = easing;
        positionTween.loop = loop;
        positionTween.pingPong = pingPong;
        positionTween.active = true;
        enabled = true;
    }

    void PlayScale(const math::Vector2& from, const math::Vector2& to,
                   float duration, UIEasingType easing = UIEasingType::Linear,
                   bool loop = false, bool pingPong = false)
    {
        scaleTween.from = from;
        scaleTween.to = to;
        scaleTween.duration = duration;
        scaleTween.elapsed = 0.0f;
        scaleTween.easing = easing;
        scaleTween.loop = loop;
        scaleTween.pingPong = pingPong;
        scaleTween.active = true;
        enabled = true;
    }

    void StopColor() { colorTween.active = false; }
    void StopPosition() { positionTween.active = false; }
    void StopScale() { scaleTween.active = false; }
    void StopAll()
    {
        StopColor();
        StopPosition();
        StopScale();
    }

    bool IsPlaying() const
    {
        return enabled && (colorTween.active || positionTween.active || scaleTween.active);
    }

    const char* GetTypeName() const { return "UIAnimator"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        // Inspector で編集する Tween パラメーター
        r.Field("colorFrom",        colorTween.from);
        r.Field("colorTo",          colorTween.to);
        r.Field("colorDuration",    colorTween.duration);
        int colorEasing = static_cast<int>(colorTween.easing);
        r.Field("colorEasing",      colorEasing);
        colorTween.easing = static_cast<UIEasingType>(colorEasing);
        r.Field("colorLoop",        colorTween.loop);
        r.Field("colorPingPong",    colorTween.pingPong);
        r.Field("colorActive",      colorTween.active);

        r.Field("posFrom",          positionTween.from);
        r.Field("posTo",            positionTween.to);
        r.Field("posDuration",      positionTween.duration);
        int posEasing = static_cast<int>(positionTween.easing);
        r.Field("posEasing",        posEasing);
        positionTween.easing = static_cast<UIEasingType>(posEasing);
        r.Field("posLoop",          positionTween.loop);
        r.Field("posPingPong",      positionTween.pingPong);
        r.Field("posActive",        positionTween.active);

        r.Field("scaleFrom",        scaleTween.from);
        r.Field("scaleTo",          scaleTween.to);
        r.Field("scaleDuration",    scaleTween.duration);
        int scaleEasing = static_cast<int>(scaleTween.easing);
        r.Field("scaleEasing",      scaleEasing);
        scaleTween.easing = static_cast<UIEasingType>(scaleEasing);
        r.Field("scaleLoop",        scaleTween.loop);
        r.Field("scalePingPong",    scaleTween.pingPong);
        r.Field("scaleActive",      scaleTween.active);
    }
};

} // namespace fbzz::scene
