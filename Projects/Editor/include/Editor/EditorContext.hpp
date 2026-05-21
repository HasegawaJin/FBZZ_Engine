// FBZZ Engine
// EditorContext.hpp | fbzz::editor
// パネル間で共有するエディター状態
#pragma once
#include <Engine/Scene/Entity.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <vector>

namespace fbzz::scene    { class Scene; }
namespace fbzz::renderer { class Camera; }
namespace fbzz::editor   { class UndoStack; class PlayModeController; }

namespace fbzz::editor {

struct EditorContext {
    // エンジンオブジェクト (非所有)
    scene::Scene*     activeScene  = nullptr;
    renderer::Camera* editorCamera = nullptr;

    // 選択状態 (Multi-select 対応)
    std::vector<scene::EntityID> selectedEntities;
    scene::EntityID PrimarySelected() const
    {
        return selectedEntities.empty() ? scene::EntityID{} : selectedEntities.front();
    }

    // ビューポート
    bool  viewportFocused = false;
    float viewportWidth   = 1280.0f;
    float viewportHeight  = 720.0f;

    // ギズモ
    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };
    GizmoMode  gizmoMode  = GizmoMode::Translate;
    GizmoSpace gizmoSpace = GizmoSpace::World;

    // グリッド・スナップ
    bool  showGrid     = true;
    float gridSize     = 1.0f;
    bool  snapEnabled  = false;
    float snapDistance = 1.0f;

    // レンダリング設定 (RenderSystem に渡す)
    renderer::RenderSettings renderSettings;

    // 表示オプション (エディター固有)
    bool showLightRange = true;
    bool showColliders  = false;
    bool showSceneStats = true;

    // Util (非所有)
    UndoStack*          undoStack = nullptr;
    PlayModeController* playMode  = nullptr;
};

} // namespace fbzz::editor
