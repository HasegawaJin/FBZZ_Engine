// FBZZ Engine
// EditorContext.hpp | fbzz::editor
// パネル間で共有するエディター状態
#pragma once
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace fbzz::renderer { class Camera; }
namespace fbzz::editor   { class UndoStack; class PlayModeController; class TerrainTool; class WaterTool; }

namespace fbzz::editor {

struct EditorContext {
    // エンジンオブジェクト (非所有)
    scene::Scene*     activeScene  = nullptr;
    renderer::Camera* editorCamera = nullptr;
    std::string       projectRoot;
    std::string       currentScenePath;
    bool              sceneDirty = false;

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

    // カメラ操作設定 (EditorSettings からロードされ、main.cpp が DebugCamera へ適用する)
    // WHY: DebugCamera は Engine 層に属しパネルから直接参照できない。
    //      EditorContext を仲介とすることで、将来的にカメラ設定 UI を
    //      任意のパネルから編集できるようにしている。
    float cameraSpeed         = 5.0f;
    float cameraSensitivity   = 0.15f;

    // ビューポート
    bool  viewportFocused = false;
    float viewportWidth   = 1280.0f;
    float viewportHeight  = 720.0f;
    bool  gameViewportFocused = false;
    float gameViewportOriginX = 0.0f;
    float gameViewportOriginY = 0.0f;
    float gameViewportWidth   = 1280.0f;
    float gameViewportHeight  = 720.0f;
    bool  requestGameViewportFocus = false; // Play 開始時に Game ビューへフォーカスを移す one-shot フラグ。ViewportPanel が消費する
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

    // プロジェクト設定
    fbzz::ProjectSettings projectSettings;

    // 表示オプション (エディター固有)
    bool showLightRange  = true;
    bool showColliders   = false;
    bool showSkeleton    = false;
    bool showStats       = true;  // Game Viewport に Stats オーバーレイを表示する
    bool showTerrainTool = true;  // Terrain Tool ウィンドウを表示する
    bool showWaterTool   = true;  // Water Tool ウィンドウを表示する
    bool hotReloadEnabled = true;

    // パネル間リクエスト (one-shot フラグ: 発行側が true にセット → 受信側が処理後 false にリセット)
    bool requestOpenProjectSettings = false;
    bool requestOpenBuildSettings   = false;

    // F キーフォーカス: ViewportPanel がセット → main.cpp が DebugCamera に適用してクリア
    bool            requestFocusOnSelected = false;
    math::Vector3   focusTargetPosition    = {};

    // エディター専用: ロック中の EntityID 一覧（シリアライズしない）
    std::vector<scene::EntityID> lockedEntities;
    bool IsLocked(scene::EntityID id) const {
        return std::find(lockedEntities.begin(), lockedEntities.end(), id) != lockedEntities.end();
    }
    void ToggleLock(scene::EntityID id) {
        auto it = std::find(lockedEntities.begin(), lockedEntities.end(), id);
        if (it != lockedEntities.end()) lockedEntities.erase(it);
        else lockedEntities.push_back(id);
    }

    // Util (非所有)
    UndoStack*          undoStack   = nullptr;
    PlayModeController* playMode    = nullptr;
    TerrainTool*        terrainTool = nullptr; // EditorApp が所有、ViewportPanel が使用
    WaterTool*          waterTool   = nullptr; // EditorApp が所有、ViewportPanel が使用
    std::function<void()>                   markSceneDirty;
    std::function<void(const std::string&)> requestOpenScene;
    bool                                    requestAssetBrowserRefresh = false;
};

} // namespace fbzz::editor
