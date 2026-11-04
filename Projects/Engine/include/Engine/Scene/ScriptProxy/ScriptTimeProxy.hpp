/// @file    ScriptTimeProxy.hpp
/// @brief   Script から Time の値とタイムスケールを扱うショートハンド。
/// @author  Hasegawa Jin
/// @date    2026-06-26
#pragma once
#include <Engine/Api.hpp>

#include <cstdint>

namespace fbzz::scene {

class Script;

struct ScriptTimeProxy {
    Script* script = nullptr;

    FBZZ_ENGINE_API float DeltaTime() const;
    FBZZ_ENGINE_API float UnscaledDeltaTime() const;
    /// @note 固定ステップ 1 回ぶんの秒数。OnFixedUpdate() 内での積分にはこちらを使う。
    FBZZ_ENGINE_API float FixedDeltaTime() const;
    FBZZ_ENGINE_API float Time() const;
    FBZZ_ENGINE_API float UnscaledTime() const;
    FBZZ_ENGINE_API uint64_t FrameCount() const;

    FBZZ_ENGINE_API void SetTimeScale(float scale) const;
    FBZZ_ENGINE_API float GetTimeScale() const;
    FBZZ_ENGINE_API void SetTargetFps(int fps) const;
    FBZZ_ENGINE_API int GetTargetFps() const;
};

} /// @note namespace fbzz::scene
