// FBZZ Engine
// EditorContext.hpp | fbzz::editor
// パネル間で共有するエディター状態
#pragma once
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Renderer/RenderSettings.hpp>
#include <vector>

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

    bool HasActiveScene() const { return activeScene != nullptr; }

    scene::GameObject* GetSelectedGO() const
    {
        auto sel = PrimarySelected();
        if (!sel.IsValid() || !activeScene) return nullptr;
        return activeScene->GetGameObject(sel);
    }

    // ビューポート
    bool  viewportFocused = false;
    float viewportWidth   = 1280.0f;
    float viewportHeight  = 720.0f;
    bool  gameViewportFocused = false;
    float gameViewportOriginX = 0.0f;
    float gameViewportOriginY = 0.0f;
    float gameViewportWidth   = 1280.0f;
    float gameViewportHeight  = 720.0f;
    bool  requestGameViewportFocus = false;
    bool  uiViewportFocused = false;
    float uiViewportOriginX = 0.0f;
    float uiViewportOriginY = 0.0f;
    float uiViewportWidth = 1280.0f;
    float uiViewportHeight = 720.0f;

    enum class GameViewportAspect {
        Free,
        Ratio16x9,
        Ratio4x3,
        Ratio1x1,
        Ratio9x16,
        HD,
        FullHD,
        QHD,
        UHD4K,
        WXGA,
        WUXGA,
        iPhonePortrait,
        iPhoneLandscape
    };
    GameViewportAspect gameViewportAspect = GameViewportAspect::Free;

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

    // プロジェクト設定
    fbzz::ProjectSettings projectSettings;

    // 表示オプション (エディター固有)
    bool showLightRange  = true;
    bool showColliders   = false;
    bool showSkeleton    = false;
    bool showSceneStats  = true;
    bool hotReloadEnabled = true;

    // パネル間リクエスト
    bool requestOpenProjectSettings = false;

    // Util (非所有)
    UndoStack*          undoStack = nullptr;
    PlayModeController* playMode  = nullptr;
};

} // namespace fbzz::editor
