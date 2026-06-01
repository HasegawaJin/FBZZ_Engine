// FBZZ Engine
// ScriptComponent.hpp | fbzz::scene
// GameObject に紐付く Script インスタンスの所有者
// std::unique_ptr で 1 つの Script を保持し、owner を注入する。
// 実行順とライフサイクル呼び出しは ScriptSystem が担う。
#pragma once

#include <Engine/Scene/Script.hpp>
#include <memory>

namespace fbzz::scene {

struct ScriptComponent {
    std::unique_ptr<Script> script;
    bool m_awoken = false;
    bool m_started = false;
};

} // namespace fbzz::scene
