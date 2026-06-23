// FBZZ Engine
// ScriptAnimatorProxy.hpp | fbzz::scene
// Script から AnimatorComponent を操作するショートハンド
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fbzz::scene {

class Script;

struct ScriptAnimatorProxy {
    Script* script = nullptr;

    void SetFloat(std::string_view name, float v) const;
    void SetInt(std::string_view name, int v) const;
    void SetBool(std::string_view name, bool v) const;
    void SetTrigger(std::string_view name) const;
    bool IsInState(std::string_view name) const;

    float       GetFloat(std::string_view name) const;
    int         GetInt  (std::string_view name) const;
    bool        GetBool (std::string_view name) const;
    float       GetNormalizedTime() const;
    std::vector<std::pair<std::string, float>> GetCurrentBlendWeights() const;
    std::string GetCurrentState() const;
    void        SetSpeed(float speed) const;
    void        Play(std::string_view stateName) const;
};

} // namespace fbzz::scene
