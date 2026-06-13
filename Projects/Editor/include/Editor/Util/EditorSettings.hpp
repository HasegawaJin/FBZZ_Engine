// FBZZ Engine
// EditorSettings.hpp | fbzz::editor
// エディター設定の永続化 (toml++ 使用)
//
// WHY: エディター固有の UX 設定 (カメラ速度・グリッド表示・ギズモモード等) は
//      プロジェクト設定とは独立して保存したい。
//      ProjectSettings はゲームランタイムに影響する設定であり、
//      EditorSettings はエディター操作の快適性に関するユーザー個人設定のため分離している。
#pragma once
#include <string>

namespace fbzz::editor {

struct EditorSettings {
    // --- カメラ -----------------------------------------------------------
    // WHY: DebugCamera の moveSpeed / mouseSens は起動のたびリセットされる。
    //      ここに保存して EditorContext 経由で DebugCamera へ適用する。
    float cameraSpeed         = 5.0f;
    float cameraSensitivity   = 0.15f;

    // --- ビュー -----------------------------------------------------------
    bool  showGrid            = true;
    float gridSize            = 1.0f;
    bool  showLightRange      = true;
    bool  showSkeleton        = false;
    bool  showStats           = true;  // Game Viewport の Stats オーバーレイ

    // --- スナップ ---------------------------------------------------------
    bool  snapEnabled         = false;
    float snapDistance        = 1.0f;

    // --- ギズモ -----------------------------------------------------------
    // WHY: int で保存し EditorContext::GizmoMode / GizmoSpace へキャストする。
    //      enum クラスを TOML に直接書くと tomlplusplus の型変換が複雑になるため。
    int   gizmoMode           = 0; // 0=Translate, 1=Rotate, 2=Scale
    int   gizmoSpace          = 0; // 0=World, 1=Local

    // --- ゲームビュー -----------------------------------------------------
    // WHY: EditorContext::GameViewportAspect の整数値として保存する (上記と同理由)
    int   gameViewportAspect  = 0;

    // --- その他 -----------------------------------------------------------
    bool        hotReloadEnabled = true;
    std::string lastScenePath;

    // --- Asset Browser ----------------------------------------------------
    float assetBrowserIconSize = 84.0f;

    // projectRoot を渡すと lastScenePath を相対パスで保存し、ロード時に絶対パスに戻す。
    // WHY: 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になる。
    bool Load(const std::string& path, const std::string& projectRoot = "");
    bool Save(const std::string& path, const std::string& projectRoot = "") const;
};

} // namespace fbzz::editor
