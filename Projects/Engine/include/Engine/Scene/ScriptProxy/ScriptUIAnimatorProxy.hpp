// FBZZ Engine
// ScriptUIAnimatorProxy.hpp | fbzz::scene
// Script から UIAnimator の Tween を再生・停止するショートハンド
#pragma once

#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

class Script;
enum class UIEasingType;

struct ScriptUIAnimatorProxy {
    Script* script = nullptr;

    void PlayColor(const math::Vector4& from, const math::Vector4& to, float duration) const;
    void PlayColor(const math::Vector4& from, const math::Vector4& to,
                   float duration, UIEasingType easing,
                   bool loop = false, bool pingPong = false) const;
    void PlayPosition(const math::Vector2& from, const math::Vector2& to, float duration) const;
    void PlayPosition(const math::Vector2& from, const math::Vector2& to,
                      float duration, UIEasingType easing,
                      bool loop = false, bool pingPong = false) const;
    void PlayScale(const math::Vector2& from, const math::Vector2& to, float duration) const;
    void PlayScale(const math::Vector2& from, const math::Vector2& to,
                   float duration, UIEasingType easing,
                   bool loop = false, bool pingPong = false) const;
    void StopColor() const;
    void StopPosition() const;
    void StopScale() const;
    void StopAll() const;
    bool IsPlaying() const;
};

} // namespace fbzz::scene
