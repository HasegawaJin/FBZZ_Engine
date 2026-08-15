// FBZZ Engine
// ScriptTimeProxy.hpp | fbzz::scene
// Script から Time の値とタイムスケールを扱うショートハンド
#pragma once

#include <cstdint>

namespace fbzz::scene {

class Script;

struct ScriptTimeProxy {
    Script* script = nullptr;

    float DeltaTime() const;
    float UnscaledDeltaTime() const;
    // 固定ステップ 1 回ぶんの秒数。OnFixedUpdate() 内での積分にはこちらを使う。
    float FixedDeltaTime() const;
    float Time() const;
    float UnscaledTime() const;
    uint64_t FrameCount() const;

    void SetTimeScale(float scale) const;
    float GetTimeScale() const;
    void SetTargetFps(int fps) const;
    int GetTargetFps() const;
};

} // namespace fbzz::scene
