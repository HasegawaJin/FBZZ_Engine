// FBZZ Engine
// ScriptAnimatorProxy.hpp | fbzz::scene
// Script から AnimatorComponent を操作するショートハンド
#pragma once

#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptAnimatorProxy {
    Script* script = nullptr;

    void SetFloat(std::string_view name, float v) const;
    void SetInt(std::string_view name, int v) const;
    void SetBool(std::string_view name, bool v) const;
    void SetTrigger(std::string_view name) const;
    bool IsInState(std::string_view name) const;
};

} // namespace fbzz::scene
