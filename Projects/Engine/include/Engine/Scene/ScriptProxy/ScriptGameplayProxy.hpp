// FBZZ Engine
// ScriptGameplayProxy.hpp | fbzz::scene
// 新しい汎用ComponentをScript DLL境界から基本型だけで操作するProxy
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptGameplayProxy {
    explicit ScriptGameplayProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    bool SetSprite(std::string_view assetPath) const;
    bool SetSpriteColor(float r, float g, float b, float a) const;
    bool PlaySpline(bool restart = false) const;
    bool PauseSpline() const;
    bool SetSplinePosition(float normalizedPosition) const;
    bool SetSliderValue(float value) const;
    [[nodiscard]] float GetSliderValue() const;
    bool SetToggle(bool value) const;
    [[nodiscard]] bool GetToggle() const;
    bool SetInputText(std::string_view text) const;
    [[nodiscard]] std::string_view GetInputText() const;
    bool StartCameraShake(float amplitude, float duration, int seed = 1) const;
    bool SetVirtualCameraPriority(int priority) const;
    bool SetAudioMixerSend(std::string_view busName, float level) const;
};

} // namespace fbzz::scene
