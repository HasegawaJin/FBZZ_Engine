// FBZZ Engine
// EditorSettings.hpp | fbzz::editor
// エディター設定の永続化 (toml++ 使用)
//
// WHY: エディター固有の UX 設定 (カメラ速度・グリッド表示・ギズモモード等) は
//      プロジェクト設定とは独立して保存したい。
//      ProjectSettings はゲームランタイムに影響する設定であり、
//      EditorSettings はエディター操作の快適性に関するユーザー個人設定のため分離している。
#pragma once
#include <Editor/Import/FbxImportTool.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
    float snapPos             = 1.0f;
    float snapRot             = 15.0f;
    float snapScale           = 0.25f;

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

    // --- ツールウィンドウ表示 ---------------------------------------------
    bool        showTerrainTool  = false;
    bool        showWaterTool    = false;
    bool        showDetailTool   = false;
    bool        showFoliageTool  = false;

    // --- Map Mode フィルター ----------------------------------------------
    bool        mapHierarchyFilter = true;
    bool        mapInspectorFilter = true;

    // --- Debug メニュー - レンダリングオーバーレイ ---------------------------
    // WHY: int で保存し renderer::ViewMode へキャストする (enum を TOML に直接書くと変換が複雑)
    bool showColliders        = false;
    bool showTerrainCollision = false;
    bool showDecalBounds      = false;
    bool showNavMesh          = true;
    bool showNavSensors       = false;
    int  viewMode        = 0; // 0=Lit, 1=Unlit, 2=WireframeLit, 3=WireframeUnlit

    // --- Debug メニュー - Post Process ------------------------------------
    bool  shadowEnabled              = true;
    bool  ppFxaaEnabled              = true;
    float ppExposure                 = 1.0f;
    bool  ppBloomEnabled             = true;
    float ppBloomIntensity           = 0.8f;
    bool  ppAoEnabled                = true;
    bool  ppFogEnabled               = false;
    float ppFogDensity               = 0.06f;
    float ppFogFar                   = 10.0f;
    bool  ppColorGradingEnabled      = true;
    float ppContrast                 = 0.0f;
    float ppSaturation               = 1.0f;
    float ppHueShift                 = 0.0f;
    bool  ppVignetteEnabled          = false;
    bool  ppFilmGrainEnabled         = false;
    bool  ppSharpenEnabled           = false;
    float ppSharpenStrength          = 0.35f;
    bool  ppDofEnabled               = false;
    float ppDofFocus                 = 8.0f;
    float ppDofBlur                  = 3.0f;
    bool  ppChromaticAberrationEnabled = false;
    bool  ppLensDistortionEnabled    = false;
    bool  ppSepiaEnabled             = false;
    bool  ppInvertEnabled            = false;
    bool  ppPosterizeEnabled         = false;
    bool  ppPixelateEnabled          = false;
    float ppPosterizeLevels          = 6.0f;
    float ppPixelSize                = 4.0f;

    // --- TerrainTool ブラシ設定 ------------------------------------------
    // WHY: int で保存し TerrainTool::FalloffType / SculptMode へキャストする
    float    terrainBrushRadius   = 5.0f;
    float    terrainBrushStrength = 0.05f;
    int      terrainBrushFalloff  = 1;       // FalloffType::Smooth
    int      terrainSculptMode    = 0;       // SculptMode::Raise
    uint32_t terrainPaintLayer    = 0;

    // --- DetailTool ブラシ設定 -------------------------------------------
    float    detailBrushRadius    = 6.0f;
    float    detailBrushStrength  = 0.6f;
    int      detailMode           = 0;       // Mode::Paint
    int      detailLayerIndex     = 0;
    bool     detailShowChunkBounds = false;
    bool     detailShowCounts      = false;

    // --- シーン -----------------------------------------------------------
    // WHY: 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になる。
    //      Load/Save 時に projectRoot との相対パスへ変換している。
    std::string lastScenePath;

    // --- Camera Bookmarks (最大 9 件) -------------------------------------
    struct CameraBookmark {
        float px = 0.0f, py = 0.0f, pz = 0.0f;   // position
        float rx = 0.0f, ry = 0.0f, rz = 0.0f, rw = 1.0f; // rotation quaternion
        bool  valid = false;
    };
    std::array<CameraBookmark, 9> cameraBookmarks;

    // --- Import デフォルト設定 ------------------------------------------------
    FbxImportOptions         defaultImportOptions;

    // --- ホットキーオーバーライド -------------------------------------------
    struct HotkeyOverride {
        std::string name;
        int         key   = 0;   // ImGuiKey 値
        bool        ctrl  = false;
        bool        shift = false;
        bool        alt   = false;
    };
    std::vector<HotkeyOverride> hotkeyOverrides;

    // --- Asset Browser ----------------------------------------------------
    float                    assetBrowserIconSize = 84.0f;
    std::vector<std::string> assetBrowserBookmarks;

    // --- Inspector セクション折り畳み状態 -----------------------------------
    // ImGui の CollapsingHeader が使う ImGuiID (uint32) と open フラグのペアを保存する。
    // WHY: ImGui の .ini はウィンドウ位置・サイズしか保存しない。
    //      ここで StateStorage を丸ごとスナップショットして永続化する。
    std::vector<std::pair<uint32_t, bool>> inspectorSectionState;

    // projectRoot を渡すと lastScenePath を相対パスで保存し、ロード時に絶対パスに戻す。
    // WHY: 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になる。
    bool Load(const std::string& path, const std::string& projectRoot = "");
    bool Save(const std::string& path, const std::string& projectRoot = "") const;
};

} // namespace fbzz::editor
