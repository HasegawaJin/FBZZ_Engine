/// @file    EditorSettings.hpp
/// @brief   エディター設定の永続化 (toml++ 使用)。
/// @author  Hasegawa Jin
/// @date    2026-05-21
///
/// @note 設定は 2 か所に分かれる (Load/Save が振り分ける)。`Assets/EditorConfig/editor_settings.toml`
///       — チーム共有 (git 追跡: ビュー/スナップ/ギズモ/ホットキー/インポート既定/ビルド設定)。
///       `<projectRoot>/Library/EditorLocalState.toml` — 個人の作業状態 (git 管理外、触るたびに
///       書き換わる)。共有ファイルに混ぜると全員が同じ行で衝突し続ける (Unity の UserSettings/ と同じ)。
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
    /// @name カメラ (EditorLocalState.toml)
    /// @{
    /// @note DebugCamera の moveSpeed / mouseSens は起動のたびリセットされるため、
    ///       ここに保存して EditorContext 経由で DebugCamera へ適用する。
    float cameraSpeed         = 5.0f;
    float cameraSensitivity   = 0.15f;

    /// @note 前回セッション終了時の位置・回転を保存して次回起動時に復元する。
    float cameraLastPx = 0.0f, cameraLastPy = 2.5f, cameraLastPz = -8.0f;
    float cameraLastRx = 0.0f, cameraLastRy = 0.0f, cameraLastRz = 0.0f, cameraLastRw = 1.0f;
    /// 平行投影で作業していたなら、その画角ごと次回へ引き継ぐ。
    bool  cameraOrthographic  = false;
    float cameraOrthoHeight   = 20.0f;
    /// @}

    /// @name ビュー
    /// @{
    bool  showGrid            = true;
    float gridSize            = 1.0f;
    bool  showLightRange      = true;
    /// エミッター発生形状。常時出すと邪魔なので既定は off。
    bool  showVFXGizmos       = false;
    /// @note show_flow_fields が無い旧設定では showVFXGizmos を引き継ぐ (以前は同じスイッチだった)。
    bool  showFlowFields      = false;
    bool  showFlowSamples     = false;
    bool  showPhysicsVolumes  = false;
    bool  showWaterFlow       = false;
    /// ラグドールの剛体・可動域・接触点。調整中だけ点ける前提で既定は off。
    bool  showRagdoll         = false;
    bool  showSkeleton        = false;
    /// @name Scene View の診断オーバーレイ。既定値は EditorContext と揃える。
    ///@{
    bool  skeletonSelectedOnly = true;
    bool  showScriptGizmos     = true;
    bool  showConstraints      = false;
    bool  showRigidBodies      = false;
    bool  showIK               = false;
    bool  showSpringBones      = false;
    bool  showAttachments      = false;
    bool  showVFXPaths         = false;
    bool  showTerrainBounds    = false;
    bool  showLODBounds        = false;
    ///@}
    /// @brief Scene View のコンポーネントアイコン全体の表示。
    bool  showSceneIcons      = true;
    /// @brief 非表示のアイコン種別 (SceneIconTypeKey の値)。未知のキーは読み飛ばされず残る。
    std::vector<std::string> hiddenSceneIcons;
    bool  showStats           = true;  ///< Game Viewport の Stats オーバーレイ
    /// Scene View のオクルージョンカリング。誤カリングの切り分け用なので既定は off。
    bool  sceneViewOcclusionCulling = false;
    /// Ctrl+Shift ドラッグ (面スナップ) で接地面の法線へ姿勢を合わせるか。
    bool  surfaceSnapAlignToNormal  = false;
    /// @}

    /// @name スナップ
    /// @{
    bool  snapEnabled         = false;
    float snapPos             = 1.0f;
    float snapRot             = 15.0f;
    float snapScale           = 0.25f;
    /// @}

    /// @name ギズモ
    /// @{
    /// @note int で保存し EditorContext::GizmoMode / GizmoSpace へキャストする。
    ///       enum クラスを TOML に直接書くと tomlplusplus の型変換が複雑になるため。
    int   gizmoMode           = 0; ///< 0=Translate, 1=Rotate, 2=Scale
    int   gizmoSpace          = 0; ///< 0=World, 1=Local
    int   gizmoPivot          = 0; ///< 0=Pivot, 1=Center
    /// @}

    /// @name ゲームビュー
    /// @{
    /// @note EditorContext::GameViewportAspect の整数値として保存する (上記と同理由)。
    int   gameViewportAspect  = 0;
    int   playFocusMode       = 1; ///< 0=Focused, 1=Maximized, 2=Unfocused
    /// @}

    /// @name その他
    /// @{
    bool        hotReloadEnabled = true;
    /// @brief ホットリロードの完了・失敗を音で知らせるか。
    bool        hotReloadSound   = true;
    bool        aiCommandBusEnabled = false; ///< AI 連携 (Claude/MCP) の Named Pipe 待受を起動時に自動開始するか
    /// プロジェクトを開いたとき、参照を失った `Library/Baked/<guid>/` を消すか。
    /// 中身は原本から焼き直せるので、消しても失うのは «次回 import の時間» だけ。
    bool        sweepOrphanedBakedOnOpen = true;
    /// @}

    /// @name ツールウィンドウ表示
    /// @{
    bool        showTerrainTool  = false;
    /// @}

    /// @name Map Mode フィルター
    /// @{
    bool        mapHierarchyFilter = true;
    bool        mapInspectorFilter = true;
    /// EditorContext::MapTool の整数値。0=TerrainSculpt 1=TerrainPaint 2=Grid
    int         mapActiveTool      = 0;
    /// @}

    /// @name Hierarchy
    /// @{
    bool        showGeneratedObjects = false;
    /// @}

    /// @name Debug メニュー - レンダリングオーバーレイ
    /// @{
    /// @note int で保存し renderer::ViewMode へキャストする (enum を TOML に直接書くと変換が複雑)。
    bool showColliders        = false;
    bool showTerrainCollision = false;
    bool showDecalBounds      = false;
    bool showNavMesh          = true;
    bool showNavSensors       = false;
    /// 0=Solid, 1=Transparent, 2=Areas, 3=Portals, 4=Voxels (renderer::NavMeshDrawMode)
    int   navMeshDrawMode     = 0;
    float navMeshDrawDistance = 120.0f;
    /// UI 要素の矩形とピボットを Canvas 上へ重ねる。
    /// @note 当たり判定に使っている矩形そのものを出す。「見えているのに押せない」
    ///       「思った場所に出ない」の切り分けが、これが無いと勘になる。
    bool showUIRects          = false;
    int  viewMode             = 0; ///< 0=Lit, 1=Unlit, 2=WireframeLit, 3=WireframeUnlit
    /// @}


    /// @name TerrainTool ブラシ設定
    /// @{
    /// @note int で保存し TerrainTool::FalloffType / SculptMode へキャストする。
    float    terrainBrushRadius   = 5.0f;
    float    terrainBrushStrength = 0.05f;
    int      terrainBrushFalloff  = 1;       ///< FalloffType::Smooth
    int      terrainSculptMode    = 0;       ///< SculptMode::Raise
    uint32_t terrainPaintLayer    = 0;
    /// @}

    /// @name シーン (EditorLocalState.toml)
    /// @{
    /// @note 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になるため、
    ///       Load/Save 時に projectRoot との相対パスへ変換している。
    std::string lastScenePath;

    /// 最近開いた/保存したシーン (新しい順, 最大 kMaxRecentScenes 件)。
    /// @note lastScenePath と同様に projectRoot 相対で永続化し、ロード時に絶対へ戻す。
    static constexpr int         kMaxRecentScenes = 10;
    std::vector<std::string>     recentScenes;
    /// @}

    /// @name オートセーブ
    /// @{
    /// @note 編集中シーンを一定間隔で `Library/AutoSave/` へ退避し、クラッシュ時に復旧できるようにする。
    bool  autoSaveEnabled     = true;
    int   autoSaveIntervalSec = 300; ///< 既定 5 分
    /// Fluid Editor で編集中の .fluid を、手を止めてしばらくしたら保存するか。
    /// @note 既定 off。.fluid は焼きと AI (fluid.set) も読み書きする共有ファイルで、
    ///       黙って書くと «誰が書いたか» が追えなくなるため、使う人が選んで点ける。
    bool  fluidEditorAutoSave = false;
    /// Asset Browser で .fluid を選んだら Fluid Editor の編集対象を切り替えるか。
    /// 切り替えるのは «未保存の変更が無いとき» だけ (Animation Graph が追従で変更を飛ばした前例がある)。
    bool  fluidEditorFollowSelection = true;
    /// Fluid Editor で最近開いた .fluid (新しい順, 最大 kMaxRecentFluids 件)。
    /// @note recentScenes と同じく projectRoot 相対で永続化する。保存先は EditorLocalState.toml の
    ///       `[fluid_editor_state] recent` — 共有側 `[fluid_editor]` と同名だと Overlay がセクション
    ///       ごと差し替え follow_selection を隠すため、節を分けてある。
    static constexpr int     kMaxRecentFluids = 10;
    std::vector<std::string> recentFluids;
    /// @}

    /// @name Camera Bookmarks (最大 9 件 / EditorLocalState.toml)
    /// @{
    struct CameraBookmark {
        float px = 0.0f, py = 0.0f, pz = 0.0f;   ///< position
        float rx = 0.0f, ry = 0.0f, rz = 0.0f, rw = 1.0f; ///< rotation quaternion
        bool  valid = false;
    };
    std::array<CameraBookmark, 9> cameraBookmarks;
    /// @}

    /// @name Import デフォルト設定
    /// @{
    FbxImportOptions         defaultImportOptions;
    /// @}

    /// @name Build Settings
    /// @{
    /// @note 配布ビルドの構成 (シーン一覧・出力先・製品名) は専用ファイルを増やすほどの
    ///       独立性がないため、エディターが覚えている他の状態と同じ 1 ファイルに集約する。
    BuildSettings            build;
    /// @}

    /// @name ホットキーオーバーライド
    /// @{
    struct HotkeyOverride {
        std::string name;
        int         key   = 0;   ///< ImGuiKey 値
        bool        ctrl  = false;
        bool        shift = false;
        bool        alt   = false;
    };
    std::vector<HotkeyOverride> hotkeyOverrides;
    /// @}

    /// @name UI
    /// @{
    /// エディター UI の表示言語 ("en" / "ja")。loc::Id / loc::FromId と対。
    /// @note 表示言語は個人設定であり、ランタイムにも配られる ProjectSettings には持たせない。
    ///       切り替え UI は Project Settings パネルの Editor セクションに置く。
    std::string              language = "ja";
    float                    editorUiScale = 1.0f; ///< UI 全体スケール (フォント+余白)
    /// パネルを OS ウィンドウとして DockSpace 外へ分離できるマルチビューポート。
    /// @note 既定 OFF。単一ウィンドウ前提の挙動 (OLE D&D 等) を壊さないよう opt-in で有効化する。
    bool                     multiViewportEnabled = false;
    /// @}

    /// @name Asset Browser (EditorLocalState.toml)
    /// @{
    /// Asset Browser は Unity の Project ウィンドウと同じく複数開けるので、
    /// 1 パネルぶんの状態はここにまとめて配列で持つ (添字 = パネルのインスタンス番号)。
    /// @note お気に入り・フォルダ色・最近使った色は「プロジェクトの見え方」であって個々の
    ///       ウィンドウの状態ではないため、この配列には含めない (別々だと片方の変更が他方に出ない)。
    struct AssetBrowserPanelState {
        float        iconSize         = 84.0f;
        float        treeWidth        = 180.0f;
        int          viewMode         = 0; ///< 0=Grid, 1=List
        int          sortMode         = 0; ///< 0=NameAsc, 1=NameDesc, 2=Type, 3=Modified
        /// 有効なタイプフィルタのビット集合 (bit N = TypeFilter N)。0 = 絞り込みなし。
        unsigned int typeFilterMask   = 0;
        bool         searchAllFolders = false;
        /// 左の階層ツリーにファイルも並べるか。
        bool         treeShowFiles    = false;
        /// 前回いたフォルダ。lastScenePath と同じく projectRoot 相対で持つ。
        std::string  currentFolder;
    };
    std::vector<AssetBrowserPanelState> assetBrowserPanels;

    std::vector<std::string> assetBrowserBookmarks;
    /// フォルダの色分け (Unreal の Set Color 相当)。projectRoot 相対パス → IM_COL32 値。
    std::vector<std::pair<std::string, unsigned int>> assetBrowserFolderColors;
    /// Set Color で最近使った色 (新しい順)。プロジェクトをまたいでも使い回せるよう保存する。
    std::vector<unsigned int> assetBrowserRecentFolderColors;

    /// 添字 index のパネル状態。足りなければ既定値のまま伸ばして返す。
    AssetBrowserPanelState& AssetBrowserPanelAt(std::size_t index)
    {
        if (assetBrowserPanels.size() <= index) assetBrowserPanels.resize(index + 1);
        return assetBrowserPanels[index];
    }
    [[nodiscard]] AssetBrowserPanelState AssetBrowserPanelAt(std::size_t index) const
    {
        return index < assetBrowserPanels.size() ? assetBrowserPanels[index]
                                                 : AssetBrowserPanelState{};
    }
    /// @}

    /// @name Console (EditorLocalState.toml)
    /// @{
    /// @note ログレベルの絞り込みは「今追っている問題」に紐づく。毎起動で全部 ON に戻ると、
    ///       追跡中のエラーがまた INFO の洪水に埋もれるところからやり直しになる。
    bool consoleShowDebug   = false;
    bool consoleShowInfo    = true;
    bool consoleShowWarn    = true;
    bool consoleShowError   = true;
    bool consoleAutoScroll  = true;
    bool consoleCollapse    = false;
    bool consoleClearOnPlay = false;
    bool consoleShowDetail  = true;
    /// 詳細ペインが占める高さの比率。スプリッタのドラッグで動く。
    float consoleDetailRatio = 0.30f;
    /// @}

    /// @name パネル表示状態 (EditorLocalState.toml)
    /// @{
    /// ウィンドウ名 → 開いているか。View > Panels に出るパネルだけを対象にする。
    /// @note ImGui の .ini はウィンドウの位置・サイズ・ドック先しか覚えず「閉じた」状態は
    ///       持たないため、無いと閉じたパネルが毎起動で開き直る。
    std::vector<std::pair<std::string, bool>> panelVisibility;
    /// @}

    /// @name Animation Preview (EditorLocalState.toml)
    /// @{
    /// @note トグルは「今どのクリップの何を疑っているか」で選ぶもので、毎起動で既定へ戻ると
    ///       選び直しから作業が始まる。カメラ角も含むのは同じ向きで見比べたいことが常なため。
    bool  animPreviewShowMesh       = true;
    bool  animPreviewShowBones      = false;
    bool  animPreviewShowBoneNames  = false;
    bool  animPreviewShowTrail      = false;
    bool  animPreviewShowGhost      = false;
    bool  animPreviewShowInfo       = false;
    bool  animPreviewShowCurves     = false;
    bool  animPreviewShowRootMotion = false;
    int   animPreviewLabelMode      = 1;    ///< 0=選択/ホバー 1=アニメ有 2=全部
    float animPreviewGhostOffset    = 0.0f; ///< 0 = タイムライン長依存の自動
    bool  animPreviewLoop           = true;
    float animPreviewSpeed          = 1.0f;
    float animPreviewCameraYaw      = 2.55f;
    float animPreviewCameraPitch    = 0.30f;
    bool  animPreviewShowGrid       = true;
    bool  animPreviewShowGroundRing = true;
    bool  animPreviewWireframe      = false;
    bool  animPreviewShowAxisGizmo  = true;
    int   animPreviewBackground     = 0;
    float animPreviewFov            = 40.0f;
    float animPreviewLightYaw       = 0.0f;
    /// @}

    /// @name Inspector セクション折り畳み状態 (EditorLocalState.toml)
    /// @{
    /// ImGui の CollapsingHeader が使う ImGuiID (uint32) と open フラグのペアを保存する。
    /// @note ImGui の .ini はウィンドウ位置・サイズしか保存しないため、
    ///       ここで StateStorage を丸ごとスナップショットして永続化する。
    std::vector<std::pair<uint32_t, bool>> inspectorSectionState;

    /// projectRoot を渡すと lastScenePath を相対パスで保存し、ロード時に絶対パスに戻す。
    /// @note 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になるため。
    bool Load(const std::string& path, const std::string& projectRoot = "");
    bool Save(const std::string& path, const std::string& projectRoot = "") const;
    /// @}
};

} // namespace fbzz::editor
