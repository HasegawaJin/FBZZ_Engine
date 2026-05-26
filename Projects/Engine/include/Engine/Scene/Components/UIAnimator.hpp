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

struct UIAnimator {
    UIColorTween    colorTween;
    UIPositionTween positionTween;
    bool            enabled = true;

    const char* GetTypeName() const { return "UIAnimator"; }
    void Reflect(IReflector& r)
    {
        r.Field("enabled", enabled);
        // Inspector で編集する Tween パラメーター
        r.Field("colorFrom",        colorTween.from);
        r.Field("colorTo",          colorTween.to);
        r.Field("colorDuration",    colorTween.duration);
        r.Field("colorLoop",        colorTween.loop);
        r.Field("colorPingPong",    colorTween.pingPong);
        r.Field("colorActive",      colorTween.active);
        r.Field("posFrom",          positionTween.from);
        r.Field("posTo",            positionTween.to);
        r.Field("posDuration",      positionTween.duration);
        r.Field("posLoop",          positionTween.loop);
        r.Field("posPingPong",      positionTween.pingPong);
        r.Field("posActive",        positionTween.active);
    }
};

} // namespace fbzz::scene
