/// @file    ScriptUIAnimatorProxy.hpp
/// @brief   Script から UIAnimator の Tween を再生・停止するショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once
#include <Engine/Api.hpp>

#include <Math/Vector2.hpp>
#include <Math/Vector4.hpp>

namespace fbzz::scene {

class Script;
enum class UIEasingType;

struct ScriptUIAnimatorProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API void PlayColor(const math::Vector4& from, const math::Vector4& to, float duration) const;
    FBZZ_ENGINE_API void PlayColor(const math::Vector4& from, const math::Vector4& to,
                   float duration, UIEasingType easing,
                   bool loop = false, bool pingPong = false) const;
    FBZZ_ENGINE_API void PlayPosition(const math::Vector2& from, const math::Vector2& to, float duration) const;
    FBZZ_ENGINE_API void PlayPosition(const math::Vector2& from, const math::Vector2& to,
                      float duration, UIEasingType easing,
                      bool loop = false, bool pingPong = false) const;
    FBZZ_ENGINE_API void PlayScale(const math::Vector2& from, const math::Vector2& to, float duration) const;
    FBZZ_ENGINE_API void PlayScale(const math::Vector2& from, const math::Vector2& to,
                   float duration, UIEasingType easing,
                   bool loop = false, bool pingPong = false) const;
    FBZZ_ENGINE_API void StopColor() const;
    FBZZ_ENGINE_API void StopPosition() const;
    FBZZ_ENGINE_API void StopScale() const;
    FBZZ_ENGINE_API void StopAll() const;
    FBZZ_ENGINE_API bool IsPlaying() const;
};

} /// @note namespace fbzz::scene
