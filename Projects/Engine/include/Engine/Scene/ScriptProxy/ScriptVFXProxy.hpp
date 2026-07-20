// FBZZ Engine
// ScriptVFXProxy.hpp | fbzz::scene
// Scriptから同じGameObjectのVFXGraphComponent再生状態を制御する
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptVFXProxy {
    explicit ScriptVFXProxy(Script* owner) : script(owner) {}
    Script* script = nullptr;

    void Play(bool restart = true) const;
    void Pause() const;
    void Stop() const;
    void SetSpeed(float speed) const;
    bool SetFloat(std::string_view name, float value) const;
    bool SetInt(std::string_view name, int value) const;
    bool SetBool(std::string_view name, bool value) const;
    bool SetColor(std::string_view name, float r, float g, float b, float a) const;
    bool SetVector3(std::string_view name, float x, float y, float z) const;
    bool SetAsset(std::string_view name, std::string_view path) const;
    bool ClearOverride(std::string_view name) const;
    [[nodiscard]] bool IsPlaying() const;
    [[nodiscard]] float GetTime() const;
    [[nodiscard]] float GetDuration() const;
};

} // namespace fbzz::scene
