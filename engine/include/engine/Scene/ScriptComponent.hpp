// FBZZ Engine
// ScriptComponent.hpp | fbzz::scene
// Owns one user script instance attached to a GameObject
#pragma once

#include <engine/Scene/Script.hpp>
#include <memory>

namespace fbzz::scene {

struct ScriptComponent {
    std::unique_ptr<Script> script;
    bool m_started = false;
};

} // namespace fbzz::scene
