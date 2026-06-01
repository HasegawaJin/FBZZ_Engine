// FBZZ Engine
// ScriptPostProcessProxy.hpp | fbzz::scene
// Script からランタイム PostProcess 設定を操作するショートハンド
#pragma once

namespace fbzz::renderer {
struct PostProcessSettings;
}

namespace fbzz::scene {

class Script;

struct ScriptPostProcessProxy {
    Script* script = nullptr;

    renderer::PostProcessSettings& Get() const;
    const renderer::PostProcessSettings* TryGet() const;
    void Set(const renderer::PostProcessSettings& settings) const;
    void Clear() const;
};

} // namespace fbzz::scene
