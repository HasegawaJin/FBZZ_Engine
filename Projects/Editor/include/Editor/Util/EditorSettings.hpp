/// @file    EditorSettings.hpp
/// @brief   エディター設定の永続化 (toml++ 使用)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// WHY: エディター固有の UX 設定 (カメラ速度・グリッド表示・ギズモモード等) は
/// プロジェクト設定とは独立して保存したい。
/// ProjectSettings はゲームランタイムに影響する設定であり、
/// EditorSettings はエディター操作の快適性に関するユーザー個人設定のため分離している。
#pragma once
#include <Editor/BuildSettings.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

struct EditorSettings {
    // --- カメラ -----------------------------------------------------------
    // WHY: DebugCamera の moveSpeed / mouseSens は起動のたびリセットされる。
    //      ここに保存して EditorContext 経由で DebugCamera へ適用する。
    float cameraSpeed         = 5.0f;
    float cameraSensitivity   = 0.15f;

    // WHY: シーン起動のたびカメラがデフォルト位置に戻ると操作感が悪い。
    //      前回セッション終了時の位置・回転を保存して次回起動時に復元する。
    float cameraLastPx = 0.0f, cameraLastPy = 2.5f, cameraLastPz = -8.0f;
    float cameraLastRx = 0.0f, cameraLastRy = 0.0f, cameraLastRz = 0.0f, cameraLastRw = 1.0f;
    // 平行投影で作業していたなら、その画角ごと次回へ引き継ぐ。
    bool  cameraOrthographic  = false;
    float cameraOrthoHeight   = 20.0f;

    // --- ビュー -----------------------------------------------------------
    bool  showGrid            = true;
    float gridSize            = 1.0f;
    bool  showLightRange      = true;
    // 力場の影響体積とエミッター発生形状。常時出すと邪魔なので既定は off。
    bool  showVFXGizmos       = false;
    // ラグドールの剛体・可動域・接触点。調整中だけ点ける前提で既定は off。
    bool  showRagdoll         = false;
    bool  showSkeleton        = false;
    bool  showStats           = true;  // Game Viewport の Stats オーバーレイ
    // Scene View のオクルージョンカリング。誤カリングの切り分け用なので既定は off。
    bool  sceneViewOcclusionCulling = false;
    // Ctrl+Shift ドラッグ (面スナップ) で接地面の法線へ姿勢を合わせるか。
    bool  surfaceSnapAlignToNormal  = false;

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
    int   gizmoPivot          = 0; // 0=Pivot, 1=Center

    // --- ゲームビュー -----------------------------------------------------
    // WHY: EditorContext::GameViewportAspect の整数値として保存する (上記と同理由)
    int   gameViewportAspect  = 0;
    int   playFocusMode       = 1; // 0=Focused, 1=Maximized, 2=Unfocused

    // --- その他 -----------------------------------------------------------
    bool        hotReloadEnabled = true;
    bool        aiCommandBusEnabled = false; // AI 連携 (Claude/MCP) の Named Pipe 待受を起動時に自動開始するか

    // --- ツールウィンドウ表示 ---------------------------------------------
    bool        showTerrainTool  = false;
    bool        showWaterTool    = false;

    // --- Map Mode フィルター ----------------------------------------------
    bool        mapHierarchyFilter = true;
    bool        mapInspectorFilter = true;
    // EditorContext::MapTool の整数値。0=TerrainSculpt … 5=Grid
    int         mapActiveTool      = 0;

    // --- Hierarchy --------------------------------------------------------
    bool        showGeneratedObjects = false;

    // --- Debug メニュー - レンダリングオーバーレイ ---------------------------
    // WHY: int で保存し renderer::ViewMode へキャストする (enum を TOML に直接書くと変換が複雑)
    bool showColliders        = false;
    bool showTerrainCollision = false;
    bool showDecalBounds      = false;
    bool showNavMesh          = true;
    bool showNavSensors       = false;
    // 0=Solid, 1=Transparent, 2=Areas, 3=Portals, 4=Voxels (renderer::NavMeshDrawMode)
    int   navMeshDrawMode     = 0;
    float navMeshDrawDistance = 120.0f;
    // UI 要素の矩形とピボットを Canvas 上へ重ねる。
    // WHY: 「見えているのに押せない」「思った場所に出ない」の切り分けが、
    //      これが無いと勘になる。当たり判定に使っている矩形そのものを出す。
    bool showUIRects          = false;
    int  viewMode             = 0; // 0=Lit, 1=Unlit, 2=WireframeLit, 3=WireframeUnlit


    // --- TerrainTool ブラシ設定 ------------------------------------------
    // WHY: int で保存し TerrainTool::FalloffType / SculptMode へキャストする
    float    terrainBrushRadius   = 5.0f;
    float    terrainBrushStrength = 0.05f;
    int      terrainBrushFalloff  = 1;       // FalloffType::Smooth
    int      terrainSculptMode    = 0;       // SculptMode::Raise
    uint32_t terrainPaintLayer    = 0;

    // --- シーン -----------------------------------------------------------
    // WHY: 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になる。
    //      Load/Save 時に projectRoot との相対パスへ変換している。
    std::string lastScenePath;

    // 最近開いた/保存したシーン (新しい順, 最大 kMaxRecentScenes 件)。
    // WHY: シーン往復を File > Open Recent から素早く行えるようにする。
    //      lastScenePath と同様に projectRoot 相対で永続化し、ロード時に絶対へ戻す。
    static constexpr int         kMaxRecentScenes = 10;
    std::vector<std::string>     recentScenes;

    // --- オートセーブ -----------------------------------------------------
    // WHY: 編集中シーンを一定間隔で Library/AutoSave/ へ退避し、クラッシュ時に復旧できるようにする。
    bool  autoSaveEnabled     = true;
    int   autoSaveIntervalSec = 300; // 既定 5 分

    // --- Camera Bookmarks (最大 9 件) -------------------------------------
    struct CameraBookmark {
        float px = 0.0f, py = 0.0f, pz = 0.0f;   // position
        float rx = 0.0f, ry = 0.0f, rz = 0.0f, rw = 1.0f; // rotation quaternion
        bool  valid = false;
    };
    std::array<CameraBookmark, 9> cameraBookmarks;

    // --- Import デフォルト設定 ------------------------------------------------
    FbxImportOptions         defaultImportOptions;

    // --- Build Settings ---------------------------------------------------
    // WHY ここに置くか: 配布ビルドの構成 (シーン一覧・出力先・製品名) は
    //     プロジェクトルートに専用ファイルを増やすほどの独立性がない。
    //     エディターが覚えている他の状態と同じ 1 ファイルに集約する。
    BuildSettings            build;

    // --- ホットキーオーバーライド -------------------------------------------
    struct HotkeyOverride {
        std::string name;
        int         key   = 0;   // ImGuiKey 値
        bool        ctrl  = false;
        bool        shift = false;
        bool        alt   = false;
    };
    std::vector<HotkeyOverride> hotkeyOverrides;

    // --- UI ---------------------------------------------------------------
    float                    editorUiScale = 1.0f; // UI 全体スケール (フォント+余白)
    // パネルを OS ウィンドウとして DockSpace 外へ分離できるマルチビューポート。
    // WHY: 既定 OFF。単一ウィンドウ前提の挙動 (OLE D&D 等) を壊さないよう、opt-in で有効化する。
    bool                     multiViewportEnabled = false;

    // --- Asset Browser ----------------------------------------------------
    float                    assetBrowserIconSize = 84.0f;
    float                    assetBrowserTreeWidth = 180.0f;
    std::vector<std::string> assetBrowserBookmarks;
    int                      assetBrowserViewMode   = 0; // 0=Grid, 1=List
    int                      assetBrowserSortMode   = 0; // 0=NameAsc, 1=NameDesc, 2=Type, 3=Modified
    int                      assetBrowserTypeFilter = 0; // 0=All (AssetBrowserPanel::TypeFilter)
    bool                     assetBrowserSearchAllFolders = false;
    // 前回いたフォルダ。lastScenePath と同じく projectRoot 相対で持つ。
    std::string              assetBrowserCurrentFolder;

    // --- Console ----------------------------------------------------------
    // WHY: ログレベルの絞り込みは「今追っている問題」に紐づく。毎起動で全部 ON に
    //      戻ると、追跡中のエラーがまた INFO の洪水に埋もれるところからやり直しになる。
    bool consoleShowDebug   = false;
    bool consoleShowInfo    = true;
    bool consoleShowWarn    = true;
    bool consoleShowError   = true;
    bool consoleAutoScroll  = true;
    bool consoleCollapse    = false;
    bool consoleClearOnPlay = false;
    bool consoleShowDetail  = true;

    // --- パネル表示状態 ----------------------------------------------------
    // ウィンドウ名 → 開いているか。View > Panels に出るパネルだけを対象にする。
    // WHY: ImGui の .ini はウィンドウの位置・サイズ・ドック先しか覚えず、
    //      「閉じた」という状態は持たない。閉じたパネルが毎起動で開き直る。
    std::vector<std::pair<std::string, bool>> panelVisibility;

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
