// FBZZ Engine
// UIAnimator.hpp | fbzz::scene
// Tween-based color and position animation for UIImage
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
    bool          pingPong = false; // reverse on each loop iteration
    bool          active   = false;
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
        // Tween params exposed for Inspector editing
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
