// FBZZ Engine
// ScriptUIProxy.hpp | fbzz::scene
// Script から UI コンポーネントを操作するショートハンド
#pragma once

#include <Math/Vector4.hpp>
#include <string_view>

namespace fbzz::scene {

class Script;

struct ScriptUIProxy {
    Script* script = nullptr;

    void SetButtonInteractable(bool v) const;
    void SetImageColor(const math::Vector4& color) const;
    void SetText(std::string_view text) const;
    void SetCanvasSortOrder(int order) const;
};

} // namespace fbzz::scene
