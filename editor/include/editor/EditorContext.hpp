// FBZZ Engine
// EditorContext.hpp | fbzz::editor
// パネル間で共有するエディター状態
#pragma once
#include <engine/Scene/Entity.hpp>

namespace fbzz::scene    { class Scene; }
namespace fbzz::renderer { class LightSystem; class Camera; }

namespace fbzz::editor {

struct EditorContext {
    scene::Scene*           activeScene    = nullptr;
    renderer::LightSystem*  lightSystem    = nullptr;
    renderer::Camera*       editorCamera   = nullptr;

    scene::EntityID         selectedEntity = {};

    bool viewportFocused = false;
};

} // namespace fbzz::editor
