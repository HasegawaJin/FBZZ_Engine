// FBZZ Engine
// ScriptApplicationProxy.hpp | fbzz::scene
// Script から Application の基本状態を扱うショートハンド
#pragma once

#include <cstdint>

namespace fbzz::scene {

class Script;

struct ScriptApplicationProxy {
    Script* script = nullptr;

    void Quit() const;
    bool IsRunning() const;
    uint32_t GetWindowWidth() const;
    uint32_t GetWindowHeight() const;
};

} // namespace fbzz::scene
