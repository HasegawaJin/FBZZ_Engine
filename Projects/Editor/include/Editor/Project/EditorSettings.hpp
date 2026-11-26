/// @file    EditorSettings.hpp
/// @brief   エディター設定の永続化 (toml++ 使用)。
/// @author  Hasegawa Jin
/// @date    2026-05-21

/// @note 設定は 2 か所に分かれる (Load/Save が振り分ける)。`Assets/EditorConfig/editor_settings.toml`
/// @note — チーム共有 (git 追跡: ビュー/スナップ/ギズモ/ホットキー/インポート既定/ビルド設定)。
/// @note `<projectRoot>/Library/EditorLocalState.toml` — 個人の作業状態 (git 管理外、触るたびに
/// @note 書き換わる)。共有ファイルに混ぜると全員が同じ行で衝突し続ける (Unity の UserSettings/ と同じ)。
#pragma once
#include <Editor/BuildPipeline/BuildSettings.hpp>
#include <Editor/Import/Fbx/FbxImportTool.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbzz::editor {

struct EditorSettings {
    /// @name カメラ (EditorLocalState.toml)

    /// @note DebugCamera の moveSpeed / mouseSens は起動のたびリセットされるため、
    /// @note ここに保存して EditorContext 経由で DebugCamera へ適用する。
    float cameraSpeed         = 5.0f; ///< Scene View のデバッグカメラの移動速度設定。
    float cameraSensitivity   = 0.15f; ///< Scene View のカメラをマウスで回す感度。

    float cameraLastPx = 0.0f, cameraLastPy = 2.5f, cameraLastPz = -8.0f; ///< 前回終了時の Scene View カメラのワールド位置 X、Y、Z [m]。
    float cameraLastRx = 0.0f, cameraLastRy = 0.0f, cameraLastRz = 0.0f, cameraLastRw = 1.0f; ///< 前回終了時の Scene View カメラのワールド回転のクォータニオン X、Y、Z、W。
    bool  cameraOrthographic  = false; ///< Scene View を正投影で開いていたか。
    float cameraOrthoHeight   = 20.0f; ///< 復元する正投影の垂直方向の表示範囲 [m]。


    /// @name ビュー

    bool  showGrid            = true; ///< Scene View のワールドグリッドを表示するか。
    float gridSize            = 1.0f; ///< 隣接するワールドグリッド線の基本間隔 [m]。
    bool  showLightRange      = true; ///< Scene View に光源の有効範囲を表示するか。
    /// @note エミッター発生形状。常時出すと邪魔なので既定は off。
    bool  showVFXGizmos       = false; ///< Scene View に VFX の発生形状と初速を表示するか。
    /// @note show_flow_fields が無い旧設定では showVFXGizmos を引き継ぐ (以前は同じスイッチだった)。
    bool  showFlowFields      = false; ///< Scene View に FlowField の範囲・減衰・向きを表示するか。
    bool  showFlowSamples     = false; ///< Scene View に格子点で評価した実効流速の矢印を表示するか。
    bool  showPhysicsVolumes  = false; ///< Scene View に物理 Volume のトリガー形状と効果を表示するか。
    bool  showWaterFlow       = false; ///< Scene View に WaterComponent の水流、渦と浮力の有効範囲を表示するか。
    /// @note ラグドールの剛体・可動域・接触点。調整中だけ点ける前提で既定は off。
    bool  showRagdoll         = false; ///< Scene View にラグドールの剛体、関節可動域と接触点を表示するか。
    bool  showSkeleton        = false; ///< Scene View にアニメーションのスケルトンを表示するか。
    /// @name Scene View の診断オーバーレイ。既定値は EditorContext と揃える。

    bool  skeletonSelectedOnly = true; ///< スケルトン表示を選択中のオブジェクトに絞るか。
    bool  showScriptGizmos     = true; ///< Scene View に C# スクリプトから依頼されたギズモを表示するか。
    bool  showConstraints      = false; ///< Scene View に制約の診断オーバーレイを表示するか。
    bool  showRigidBodies      = false; ///< Scene View に剛体の診断オーバーレイを表示するか。
    bool  showIK               = false; ///< Scene View に IK の診断オーバーレイを表示するか。
    bool  showSpringBones      = false; ///< Scene View に揺れ物のボーンの診断オーバーレイを表示するか。
    bool  showAttachments      = false; ///< Scene View にアタッチメントの診断オーバーレイを表示するか。
    bool  showVFXPaths         = false; ///< Scene View に VFX の経路の診断オーバーレイを表示するか。
    bool  showTerrainBounds    = false; ///< Scene View に Terrain の境界の診断オーバーレイを表示するか。
    bool  showLODBounds        = false; ///< Scene View に LOD の境界の診断オーバーレイを表示するか。

    /// @brief Scene View のコンポーネントアイコン全体の表示。
    bool  showSceneIcons      = true; ///< Scene View のコンポーネントアイコンを表示し、クリック選択を許可するか。
    /// @brief 非表示のアイコン種別 (SceneIconTypeKey の値)。未知のキーは読み飛ばされず残る。
    std::vector<std::string> hiddenSceneIcons; ///< Scene View で非表示にしたアイコン種別の永続化キー。
    bool  showStats           = true;  ///< Scene / Game Viewport の Stats オーバーレイ。
    /// @note Scene View のオクルージョンカリング。誤カリングの切り分け用なので既定は off。
    bool  sceneViewOcclusionCulling = false; ///< Scene View のデバッグカメラで CPU ソフトウェアオクルージョンカリングを使うか。
    /// @note Ctrl+Shift ドラッグ (面スナップ) で接地面の法線へ姿勢を合わせるか。
    bool  surfaceSnapAlignToNormal  = false; ///< 面スナップ時にオブジェクトの上方向を面の法線へ合わせるか。


    /// @name スナップ

    bool  snapEnabled         = false; ///< Scene View の変換ギズモで刻み値へのスナップを有効にするか。
    float snapPos             = 1.0f; ///< 位置スナップの刻み幅 [m]。
    float snapRot             = 15.0f; ///< 回転スナップの刻み幅 [度]。
    float snapScale           = 0.25f; ///< スケールスナップの刻み幅。元の寸法に対する倍率。


    /// @name ギズモ

    /// @note int で保存し EditorContext::GizmoMode / GizmoSpace へキャストする。
    /// @note enum クラスを TOML に直接書くと tomlplusplus の型変換が複雑になるため。
    int   gizmoMode           = 0; ///< 0=Translate, 1=Rotate, 2=Scale
    int   gizmoSpace          = 0; ///< 0=World, 1=Local
    int   gizmoPivot          = 0; ///< 0=Pivot, 1=Center


    /// @name ゲームビュー

    /// @note EditorContext::GameViewportAspect の整数値として保存する (上記と同理由)。
    int   gameViewportAspect  = 0; ///< EditorContext::GameViewportAspect の保存値。次回起動時の Game View に適用する。
    int   playExecutionMode   = 0; ///< 0=In Editor, 1=Standalone process
    int   playFocusMode       = 1; ///< 0=Focused, 1=Maximized, 2=Unfocused


    /// @name その他

    bool        hotReloadEnabled = true; ///< ディスク上の変更を実行中の Editor へ自動で取り込むか。
    /// @brief ホットリロードの完了・失敗を音で知らせるか。
    bool        hotReloadSound   = true; ///< ホットリロードの完了と失敗を効果音で知らせるか。
    bool        aiCommandBusEnabled = false; ///< AI 連携 (Claude/MCP) の Named Pipe 待受を起動時に自動開始するか
    /// @brief DeveloperMode の設定値。起動引数での強制は含めない (Docs/design/developer-mode.md)。
    bool        developerMode = false; ///< Editor の開発者モードの保存値。起動引数による一時指定は別に扱う。
    /// @note プロジェクトを開いたとき、参照を失った `Library/Baked/<guid>/` を消すか。
    /// @note 中身は原本から焼き直せるので、消しても失うのは «次回 import の時間» だけ。
    bool        sweepOrphanedBakedOnOpen = true; ///< プロジェクトを開くとき、原本の参照を失ったベイク出力を掃除するか。


    /// @name ツールウィンドウ表示

    bool        showTerrainTool  = false; ///< Terrain Tool のウィンドウを表示するか。


    /// @name Map Mode フィルター

    bool        mapHierarchyFilter = true; ///< Map Mode 中の Hierarchy を地形・グリッド・水のオブジェクトだけに絞るか。
    bool        mapInspectorFilter = true; ///< Map Mode 中の Inspector をマップ制作用コンポーネントに絞るか。
    /// @note EditorContext::MapTool の整数値。0=TerrainSculpt 1=TerrainPaint 2=Grid
    int         mapActiveTool      = 0; ///< EditorContext::MapTool の保存値。Map Mode を開いたときの作業ツールを選ぶ。


    /// @name Hierarchy

    bool        showGeneratedObjects = false; ///< VFX 等が実行時に生成した GameObject を Hierarchy に表示するか。


    /// @name Debug メニュー - レンダリングオーバーレイ

    /// @note int で保存し renderer::ViewMode へキャストする (enum を TOML に直接書くと変換が複雑)。
    bool showColliders        = false; ///< レンダリングの診断表示にコライダーを重ねるか。
    bool showTerrainCollision = false; ///< レンダリングの診断表示に地形の当たり判定を重ねるか。
    bool showDecalBounds      = false; ///< レンダリングの診断表示にデカールの境界を重ねるか。
    bool showNavMesh          = true; ///< レンダリングの診断表示にナビゲーションメッシュを重ねるか。
    bool showNavSensors       = false; ///< レンダリングの診断表示にナビゲーションセンサーを重ねるか。
    /// @note 0=Solid, 1=Transparent, 2=Areas, 3=Portals, 4=Voxels (renderer::NavMeshDrawMode)
    int   navMeshDrawMode     = 0; ///< ナビゲーションメッシュの診断表示を選ぶ renderer::NavMeshDrawMode の保存値。
    float navMeshDrawDistance = 120.0f; ///< カメラからナビゲーションメッシュの診断表示を行う範囲 [m]。
    /// @note UI 要素の矩形とピボットを Canvas 上へ重ねる。
    /// @note 当たり判定に使っている矩形そのものを出す。「見えているのに押せない」
    /// @note 「思った場所に出ない」の切り分けが、これが無いと勘になる。
    bool showUIRects          = false; ///< UI の当たり判定に使う矩形とピボットを Canvas に重ねるか。
    int  viewMode             = 0; ///< 0=Lit, 1=Unlit, 2=WireframeLit, 3=WireframeUnlit



    /// @name TerrainTool ブラシ設定

    /// @note int で保存し TerrainTool::FalloffType / SculptMode へキャストする。
    float    terrainBrushRadius   = 5.0f; ///< Terrain Tool のブラシ半径 [m]。次回のツール起動時に復元する。
    float    terrainBrushStrength = 0.05f; ///< Terrain Tool のブラシ強度の保存値。モードごとに適用する量の調整に使う。
    int      terrainBrushFalloff  = 1;       ///< FalloffType::Smooth
    int      terrainSculptMode    = 0;       ///< SculptMode::Raise
    uint32_t terrainPaintLayer    = 0; ///< Terrain Tool で選んだ地形マテリアルレイヤーの添字。


    /// @name シーン (EditorLocalState.toml)

    /// @note 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になるため、
    /// @note Load/Save 時に projectRoot との相対パスへ変換している。
    std::string lastScenePath; ///< 次回開く最後のシーン。保存ファイルではプロジェクト相対、読み込み後は絶対パス。

    /// @note 最近開いた/保存したシーン (新しい順, 最大 kMaxRecentScenes 件)。
    /// @note lastScenePath と同様に projectRoot 相対で永続化し、ロード時に絶対へ戻す。
    static constexpr int         kMaxRecentScenes = 10;
    std::vector<std::string>     recentScenes; ///< 最近開いたシーンを新しい順に保持する。保存ファイルではプロジェクト相対。


    /// @name オートセーブ

    /// @note 編集中シーンを一定間隔で `Library/AutoSave/` へ退避し、クラッシュ時に復旧できるようにする。
    bool  autoSaveEnabled     = true; ///< シーン変更のクラッシュ復旧用に Library/AutoSave へ定期的に退避するか。
    int   autoSaveIntervalSec = 300; ///< 既定 5 分
    /// @note Fluid Editor で編集中の .fluid を、手を止めてしばらくしたら保存するか。
    /// @note 既定 off。.fluid は焼きと AI (fluid.set) も読み書きする共有ファイルで、
    /// @note 黙って書くと «誰が書いたか» が追えなくなるため、使う人が選んで点ける。
    bool  fluidEditorAutoSave = false; ///< Fluid Editor で変更が止まった後、開いている .fluid を自動保存するか。
    /// @note Asset Browser で .fluid を選んだら Fluid Editor の編集対象を切り替えるか。
    /// @note 切り替えるのは «未保存の変更が無いとき» だけ (Animation Graph が追従で変更を飛ばした前例がある)。
    bool  fluidEditorFollowSelection = true; ///< 未保存の変更がないとき、Asset Browser の .fluid 選択へ編集対象を追従させるか。
    /// @note Fluid Editor で最近開いた .fluid (新しい順, 最大 kMaxRecentFluids 件)。
    /// @note recentScenes と同じく projectRoot 相対で永続化する。保存先は EditorLocalState.toml の
    /// @note `[fluid_editor_state] recent` — 共有側 `[fluid_editor]` と同名だと Overlay がセクション
    /// @note ごと差し替え follow_selection を隠すため、節を分けてある。
    static constexpr int     kMaxRecentFluids = 10;
    std::vector<std::string> recentFluids; ///< Fluid Editor で最近開いた .fluid の一覧。保存ファイルではプロジェクト相対。

    /// @note 個人の表示設定だけを保持し、再生中状態・ジョブ・GPU資源は保存しない。
    struct FluidEditorViewSettings {
        bool loop = true; ///< Fluid Editor のプレビューを最後まで再生したとき先頭へ戻すか。
        bool checkerBackground = true; ///< Fluid Editor のプレビューに透明背景の市松模様を表示するか。
        bool showOverlays = true; ///< Fluid Editor のプレビューに補助表示を重ねるか。
        bool compareBaked = false; ///< Fluid Editor のプレビューにベイク済みの結果を比較表示するか。
        bool showVolumeCenterPlane = true; ///< ボリュームプレビューで中心を通る断面を表示するか。
        bool viewModeChosen = false; ///< Fluid Editor でプレビュー表示方式を明示的に選択済みか。
        bool orbitInitialized = false; ///< Fluid Editor の 3D プレビューに復元できるオービット姿勢があるか。
        int quality = 0; ///< Fluid Editor の流体プレビュー品質を選ぶ保存値。
        int viewMode = 0; ///< Fluid Editor の流体プレビューの表示方式を選ぶ保存値。
        int centerPlane = 0; ///< Fluid Editor で表示するボリューム中心断面の向きを選ぶ保存値。
        int gizmo = 0; ///< Fluid Editor で編集に使うギズモを選ぶ保存値。
        float zoom = 1.0f; ///< Fluid Editor のプレビュー画像の拡大倍率。
        float panX = 0.0f, panY = 0.0f; ///< Fluid Editor の 2D プレビューの水平・垂直オフセット。
        float yaw = 0.0f, pitch = 0.0f; ///< Fluid Editor の 3D オービットカメラの方位角と仰角。
        float outlinerWidth = 230.0f; ///< Fluid Editor の Outliner を開く際に復元する幅 [px]。
        float propertiesWidth = 340.0f; ///< Fluid Editor の設定欄を開く際に復元する幅 [px]。
        float timelineHeight = 200.0f; ///< Fluid Editor のタイムラインを開く際に復元する高さ [px]。
    };
    FluidEditorViewSettings fluidEditorView; ///< Fluid Editor の個人用表示設定。実行中のベイク、再生状態、GPU 資源は含めない。
    bool sfxAutoPreview = true; ///< SFX Editor で設定を編集したあと効果音の試聴を自動で鳴らすか。


    /// @name Camera Bookmarks (最大 9 件 / EditorLocalState.toml)

    struct CameraBookmark {
        float px = 0.0f, py = 0.0f, pz = 0.0f; ///< Scene View カメラのブックマーク時のワールド位置 X、Y、Z [m]。
        float rx = 0.0f, ry = 0.0f, rz = 0.0f, rw = 1.0f; ///< Scene View カメラのブックマーク時のワールド回転のクォータニオン X、Y、Z、W。
        bool  valid = false; ///< このカメラブックマークに復元できる位置と回転を保存したか。
    };
    std::array<CameraBookmark, 9> cameraBookmarks; ///< Scene View の数字キー 1～9 に対応するカメラブックマーク。


    /// @name Import デフォルト設定

    FbxImportOptions         defaultImportOptions; ///< FBX インポートへ初期値として渡す設定。EditorSettings に保存する。


    /// @name Build Settings

    /// @note 配布ビルドの構成 (シーン一覧・出力先・製品名) は専用ファイルを増やすほどの
    /// @note 独立性がないため、エディターが覚えている他の状態と同じ 1 ファイルに集約する。
    BuildSettings            build; ///< ゲームのシーン一覧、製品名、出力先を含むチーム共有の配布ビルド設定。


    /// @name ホットキーオーバーライド

    struct HotkeyOverride {
        std::string name; ///< 既定の割り当てを置き換えるホットキーの登録名。
        int         key   = 0;   ///< ImGuiKey 値
        bool        ctrl  = false; ///< 上書きしたショートカットで Ctrl キーを要求するか。
        bool        shift = false; ///< 上書きしたショートカットで Shift キーを要求するか。
        bool        alt   = false; ///< 上書きしたショートカットで Alt キーを要求するか。
    };
    std::vector<HotkeyOverride> hotkeyOverrides; ///< 既定値から変更した Editor ホットキーの一覧。


    /// @name UI

    /// @note エディター UI の表示言語 ("en" / "ja")。loc::Id / loc::FromId と対。
    /// @note 表示言語は個人設定であり、ランタイムにも配られる ProjectSettings には持たせない。
    /// @note 切り替え UI は Project Settings パネルの Editor セクションに置く。
    std::string              language = "ja"; ///< Editor の UI だけに使う個人設定の言語コード。
    float                    editorUiScale = 1.0f; ///< UI 全体スケール (フォント+余白)
    /// @note パネルを OS ウィンドウとして DockSpace 外へ分離できるマルチビューポート。
    /// @note 既定 OFF。単一ウィンドウ前提の挙動 (OLE D&D 等) を壊さないよう opt-in で有効化する。
    bool                     multiViewportEnabled = false; ///< Editor パネルを DockSpace の外へ OS ウィンドウとして分離できるようにするか。


    /// @name Asset Browser (EditorLocalState.toml)

    /// @note Asset Browser は Unity の Project ウィンドウと同じく複数開けるので、
    /// @note 1 パネルぶんの状態はここにまとめて配列で持つ (添字 = パネルのインスタンス番号)。
    /// @note お気に入り・フォルダ色・最近使った色は「プロジェクトの見え方」であって個々の
    /// @note ウィンドウの状態ではないため、この配列には含めない (別々だと片方の変更が他方に出ない)。
    struct AssetBrowserPanelState {
        float        iconSize         = 84.0f; ///< この Asset Browser パネルで使うサムネイルの基準サイズ [px]。
        float        treeWidth        = 180.0f; ///< この Asset Browser パネルのフォルダーツリーの幅 [px]。
        int          viewMode         = 0; ///< 0=Grid, 1=List
        int          sortMode         = 0; ///< 0=NameAsc, 1=NameDesc, 2=Type, 3=Modified
        /// @note 有効なタイプフィルタのビット集合 (bit N = TypeFilter N)。0 = 絞り込みなし。
        unsigned int typeFilterMask   = 0; ///< このパネルで表示するアセット種別のビット集合。0 なら絞り込まない。
        bool         searchAllFolders = false; ///< Asset Browser の検索範囲を現在のフォルダーから全対象フォルダーへ広げるか。
        /// @note 左の階層ツリーにファイルも並べるか。
        bool         treeShowFiles    = false; ///< この Asset Browser のフォルダーツリーにファイルを表示するか。
        /// @note 前回いたフォルダ。lastScenePath と同じく projectRoot 相対で持つ。
        std::string  currentFolder; ///< この Asset Browser で最後に表示したフォルダーのプロジェクト相対パス。
    };
    std::vector<AssetBrowserPanelState> assetBrowserPanels; ///< Asset Browser のインスタンス番号を添字とする個人用パネル設定。

    std::vector<std::string> assetBrowserBookmarks; ///< 全 Asset Browser が共有する、お気に入りフォルダーのパス。
    /// @note フォルダの色分け (Unreal の Set Color 相当)。projectRoot 相対パス → IM_COL32 値。
    std::vector<std::pair<std::string, unsigned int>> assetBrowserFolderColors; ///< プロジェクト相対フォルダーパスと IM_COL32 の色の対応を保存する。
    /// @note Set Color で最近使った色 (新しい順)。プロジェクトをまたいでも使い回せるよう保存する。
    std::vector<unsigned int> assetBrowserRecentFolderColors; ///< 最近フォルダーに設定した表示色。新しい色が先頭。

    /// @note 添字 index のパネル状態。足りなければ既定値のまま伸ばして返す。
    AssetBrowserPanelState& AssetBrowserPanelAt(std::size_t index)
    {
        if (assetBrowserPanels.size() <= index) {
            assetBrowserPanels.resize(index + 1);
        }
        return assetBrowserPanels[index];
    }
    [[nodiscard]] AssetBrowserPanelState AssetBrowserPanelAt(std::size_t index) const
    {
        return index < assetBrowserPanels.size() ? assetBrowserPanels[index]
                                                 : AssetBrowserPanelState{};
    }


    /// @name Console (EditorLocalState.toml)

    /// @note ログレベルの絞り込みは「今追っている問題」に紐づく。毎起動で全部 ON に戻ると、
    /// @note 追跡中のエラーがまた INFO の洪水に埋もれるところからやり直しになる。
    bool consoleShowDebug   = false; ///< Console パネルに Debug レベルを表示するか。
    bool consoleShowInfo    = true; ///< Console パネルに Info レベルを表示するか。
    bool consoleShowWarn    = true; ///< Console パネルに Warning レベルを表示するか。
    bool consoleShowError   = true; ///< Console パネルに Error レベルを表示するか。
    bool consoleAutoScroll  = true; ///< ログ追加時に Console パネルを末尾までスクロールするか。
    bool consoleCollapse    = false; ///< Console パネルで同一のログをまとめて表示するか。
    bool consoleClearOnPlay = false; ///< Play を開始するとき Console のログ履歴を消すか。
    bool consoleShowDetail  = true; ///< Console で選択したログの詳細欄を表示するか。
    /// @note 詳細ペインが占める高さの比率。スプリッタのドラッグで動く。
    float consoleDetailRatio = 0.30f; ///< Console 全体の高さに対するログ詳細欄の比率 [0,1]。


    /// @name パネル表示状態 (EditorLocalState.toml)

    /// @note ウィンドウ名 → 開いているか。View > Panels に出るパネルだけを対象にする。
    /// @note ImGui の .ini はウィンドウの位置・サイズ・ドック先しか覚えず「閉じた」状態は
    /// @note 持たないため、無いと閉じたパネルが毎起動で開き直る。
    std::vector<std::pair<std::string, bool>> panelVisibility; ///< パネル名ごとの開閉状態。保存対象は View > Panels に表示するパネル。
    /// @note 収集設定だけを個人状態へ保存する。snapshot と Freeze は保存しない。
    bool performanceProfilerRecording = true; ///< Performance Profiler がフレームの性能記録を収集するか。
    bool scriptProfilerRecording = false; ///< Script Profiler が C# 呼び出しの計測値を収集するか。
    bool memoryProfilerRecording = true; ///< Memory Profiler が割当の計測値を収集するか。
    int scriptProfilerGroup = 0; ///< Script Profiler の行をまとめる分類の保存値。
    int scriptProfilerMode = 0; ///< Script Profiler の表示モードの保存値。
    int scriptProfilerRange = 1; ///< Script Profiler で集計する範囲の保存値。
    std::string scriptProfilerNameFilter; ///< Script Profiler の名前検索欄に復元する文字列。
    std::string scriptProfilerTypeFilter; ///< Script Profiler で C# 型を絞り込む検索文字列。
    std::string scriptProfilerInstanceFilter; ///< Script Profiler でスクリプト個体を絞り込む検索文字列。
    std::string scriptProfilerCallbackFilter; ///< Script Profiler でコールバックを絞り込む検索文字列。
    std::string scriptProfilerSceneFilter; ///< Script Profiler でシーンを絞り込む検索文字列。


    /// @name Animation Preview (EditorLocalState.toml)

    /// @note トグルは「今どのクリップの何を疑っているか」で選ぶもので、毎起動で既定へ戻ると
    /// @note 選び直しから作業が始まる。カメラ角も含むのは同じ向きで見比べたいことが常なため。
    bool  animPreviewShowMesh       = true; ///< Animation Preview でモデルのメッシュを表示するか。
    bool  animPreviewShowBones      = false; ///< Animation Preview でボーンを表示するか。
    bool  animPreviewShowBoneNames  = false; ///< Animation Preview でボーン名を表示するか。
    bool  animPreviewShowTrail      = false; ///< Animation Preview でボーンの移動軌跡を表示するか。
    bool  animPreviewShowGhost      = false; ///< Animation Preview で時刻をずらした姿勢を重ねるか。
    bool  animPreviewShowInfo       = false; ///< Animation Preview の診断情報を表示するか。
    bool  animPreviewShowCurves     = false; ///< Animation Preview にアニメーションカーブを表示するか。
    bool  animPreviewShowRootMotion = false; ///< Animation Preview にルートモーションの表示を出すか。
    int   animPreviewLabelMode      = 1;    ///< 0=選択/ホバー 1=アニメ有 2=全部
    float animPreviewGhostOffset    = 0.0f; ///< 0 = タイムライン長依存の自動
    bool  animPreviewLoop           = true; ///< Animation Preview のクリップを繰り返し再生するか。
    float animPreviewSpeed          = 1.0f; ///< Animation Preview の再生速度倍率。
    float animPreviewCameraYaw      = 2.55f; ///< Animation Preview カメラの開始時の方位角 [rad]。
    float animPreviewCameraPitch    = 0.30f; ///< Animation Preview カメラの開始時の仰角 [rad]。
    bool  animPreviewShowGrid       = true; ///< Animation Preview のグリッドを表示するか。
    bool  animPreviewShowGroundRing = true; ///< Animation Preview でモデルの足元の基準円を表示するか。
    bool  animPreviewWireframe      = false; ///< Animation Preview のメッシュのワイヤーフレームを表示するか。
    bool  animPreviewShowAxisGizmo  = true; ///< Animation Preview に座標軸ギズモを表示するか。
    int   animPreviewBackground     = 0; ///< Animation Preview の背景を選ぶ保存値。
    float animPreviewFov            = 40.0f; ///< Animation Preview カメラの画角 [度]。
    float animPreviewLightYaw       = 0.0f; ///< Animation Preview の光源の方位角。


    /// @name Inspector セクション折り畳み状態 (EditorLocalState.toml)

    /// @note ImGui の CollapsingHeader が使う ImGuiID (uint32) と open フラグのペアを保存する。
    /// @note ImGui の .ini はウィンドウ位置・サイズしか保存しないため、
    /// @note ここで StateStorage を丸ごとスナップショットして永続化する。
    std::vector<std::pair<uint32_t, bool>> inspectorSectionState; ///< EditorSettings と ImGui の間を中継するセクション ID ごとの開閉状態。

    /// @note projectRoot を渡すと lastScenePath を相対パスで保存し、ロード時に絶対パスに戻す。
    /// @note 絶対パスのまま保存するとプロジェクトフォルダを移動した後に無効になるため。
    bool Load(const std::string& path, const std::string& projectRoot = "");
    bool Save(const std::string& path, const std::string& projectRoot = "") const;
    /// @note 共有設定と個人状態を結合した編集用の値。ディスクを変更しない。
    [[nodiscard]] std::string ToToml(const std::string& projectRoot = "") const;
    [[nodiscard]] bool LoadFromToml(const std::string& text, const std::string& projectRoot = "");
    /// @note 通常保存とMCPが同じキー定義を使う。sharedとlocalは実際の保存先ごとの本文。
    void SerializeToml(std::string& shared, std::string& local, const std::string& projectRoot = "") const;

};

} // namespace fbzz::editor
