/// @file    EditorContext.hpp
/// @brief   パネル間で共有するエディター状態。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/GraphLayout.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/EditorSceneState.hpp>
#include <Editor/Util/HotkeyScope.hpp>
#include <Editor/Util/ScriptRequirementReview.hpp>
#include <Engine/Audio/SynthSpec.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Renderer/Camera.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Quaternion.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer { class Camera; }
namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; class ResourceManager; }
namespace fbzz::core     { class MemorySystem; }
namespace fbzz::scene    { struct AnimatorComponent; class ProjectRuntime; }
namespace fbzz::editor::playtest { class PlaytestRunner; class InputRecorder; }
namespace fbzz::editor   { class UndoStack; class PlayModeController; class TerrainTool; class HotkeyManager; class BuildConsole; class ConsoleSink; class OperatorRegistry; class MemoryLeakDiff; class FluidBakeService; }

namespace fbzz::editor {

struct InspectorComponentDrawCollector;

struct EditorContext {
    /// @note  Animation Graph 上で選択中の編集対象をパネル間で共有する。
    /// @note ノードキャンバスと詳細編集を別パネルへ分離するため、ポインタではなく EntityID と index を保持する。
    struct AnimationGraphSelection {
        enum class Type {
            None,
            State,
            Transition,
            AnyState,
            AnyStateTransition,
            BlendTreeMotion
        };

        Type            type = Type::None;
        scene::EntityID entityId = scene::EntityID::INVALID;
        std::string     assetPath;
        /// @note  どのレイヤーのグラフに対する選択か。空 = Base Layer。
        /// @note stateIndex はグラフごとの添字なので、レイヤー名が無いと Inspector が別レイヤーのステートを編集してしまう。
        std::string     layerName;
        int             stateIndex = -1;
        int             transitionIndex = -1;
        /// @note  BlendTree 内で選択中の Motion。stateIndex が親 State、motionIndex が Motion の添字。
        /// @note Motion は State とは別の入れ子データなので、State と同じ index だけでは編集対象を特定できない。
        int             motionIndex = -1;
        /// @note  グラフ上で掴んでいるステートの数 (矩形選択・Ctrl クリック)。
        /// @note Inspector が編集するのは常にプライマリ 1 件だが Delete は選択全部に効くため、件数を UI 上で明示する。
        int             selectedCount = 0;

        void Clear()
        {
            type = Type::None;
            entityId = scene::EntityID::INVALID;
            assetPath.clear();
            layerName.clear();
            stateIndex = -1;
            transitionIndex = -1;
            motionIndex = -1;
            selectedCount = 0;
        }
    };

    /// @note  エンジンオブジェクト (非所有)
    /// @note  今«見えている»シーン。Play 中に LoadScene が起きると、SceneManager が所有する
    /// @note  遷移後シーンを指す。パネル・ギズモ・AI クエリはこちらを見る。
    scene::Scene*     activeScene  = nullptr;
    /// @note  編集対象のシーン (ドキュメント本体)。currentScenePath が指すファイルの中身であり、Play 中もシーン遷移の影響を受けない。
    /// @note activeScene で保存・読込すると、Play 中の遷移後に Ctrl+S を押した際「遷移後シーンの中身」を「遷移前シーンのファイル」へ書き込み、開いていたシーンが潰れる。
    scene::Scene*     editScene    = nullptr;
    /// @note  Play 中に遷移した実行シーンの登録名 (遷移していなければ空)。表示専用。
    std::string       playSceneName;
    /// @note  編集中Sceneを駆動するProjectRuntime (EditorApp所有)。Physics World等の実行時系へアクセスする。
    /// @note AI (EditorBusDispatcher) の raycast/overlap 等はこの runtime の Physics World へ問い合わせる。
    scene::ProjectRuntime* runtime = nullptr;
    /// @note  AI の viewport.capture / viewport.semantic が RT を読み出す間、表示状態に関わらず
    /// @note  Scene View / Game View を描き続けるための猶予フレーム。
    /// @note  EditorApp は画面に出ているビューポートだけを描くので、隠れている側をキャプチャすると
    /// @note  描画が止まった RT の古い内容を読んでしまう。
    std::uint64_t     aiViewportRenderUntilFrame = 0;
    renderer::Camera* editorCamera = nullptr;
    /// @note  DebugCamera が持つオービット中心と注視距離のミラー (EditorApp が毎フレーム更新)。
    /// @note ナビゲーションギズモは「ピボットを動かさず視点だけ回す」ため、カメラ位置から復元できない DebugCamera 内部の中心・距離を必要とする。
    math::Vector3     editorCameraPivot         = {};
    float             editorCameraFocusDistance = 10.0f;
    /// @note  ピボットからカメラを引いている実距離。正投影では焦点距離と一致しない
    /// @note  (DebugCamera::ViewDistance)。視点だけ回す計算はこちらを使うこと。
    float             editorCameraViewDistance  = 10.0f;
    renderer::IRenderer* renderer = nullptr;
    renderer::IImGuiRenderer* imguiRenderer = nullptr;
    renderer::ResourceManager* resources = nullptr;
    /// @note  .fluid の焼き・プレビューの窓口 (EditorApp が所有)。
    FluidBakeService* fluidBake = nullptr;
    core::MemorySystem* memorySystem = nullptr;
    /// @note  Renderer リソースのリーク差分。EditorApp が所有し、Analysis パネルが読む。
    MemoryLeakDiff*     memoryLeakDiff = nullptr;
    std::string       projectRoot;
    std::string       projectBuildRoot;
    std::string       engineRoot;          ///< @note GameHubが選択したimmutable SDK root (cmake configureへ渡す)
    std::string       standaloneTargetName = "SandboxStandalone";
    std::string       projectTargetName;   ///< @note .fbzz_proj の target_name (GameHub プロジェクトの識別に使う)

    /// @note  ScriptCodeGen / ScriptDllLoader が使うソースパス (InitScriptDll で設定)
    std::string       scriptsSourceDir;     ///< @note Assets/Scripts/ の絶対パス
    std::string       scriptsDllCppPath;    ///< @note SandboxScriptsDll.cpp の絶対パス (DLL 登録)
    std::string       scriptsStaticCppPath; ///< @note SandboxScripts.cpp の絶対パス (EXE 静的登録)
    std::string       scriptsDllPath;       ///< @note コンパイル済み DLL の絶対パス (BuildPipeline が配布物へコピー)
    std::string       hlslSourceDir;       ///< @note Assets/shaders/ の絶対パス
    std::string       currentScenePath;
    std::string       selectedAssetPath; ///< @note アセットブラウザーで選択中のファイル絶対パス (空 = なし)
    bool              sceneDirty = false;

    /// @note  OSエクスプローラーからの外部ドラッグ&ドロップ受け渡しチャンネル (プラットフォーム層 → AssetBrowser)。
    /// @note AssetBrowserImport がドロップ確定を遅延処理するため、ドラッグ中のライブ位置と確定ファイル群をフレーム跨ぎで共有する。空のままなら AssetBrowser は何もしない。
    bool                     externalDragActive = false; ///< @note ドラッグ中ハイライト用
    float                    externalDragX = 0.0f;       ///< @note ドラッグ中カーソル位置 (screen space)
    float                    externalDragY = 0.0f;
    std::vector<std::string> droppedExternalFiles;       ///< @note 今フレーム確定したドロップファイルの絶対パス群
    float                    droppedExternalFilesX = 0.0f; ///< @note ドロップ確定位置 (screen space)
    float                    droppedExternalFilesY = 0.0f;
    /// @note  Animation Graph Editor のノード配置。Editor 専用のため SceneSerializer には渡さない。
    /// @note AnimatorComponent はランタイム構造体なので、キャンバス座標は GO の instanceId または Controller アセットパスをキーにした別データとして保持する。
    std::unordered_map<std::string, GraphLayout> graphLayouts;
    AnimationGraphSelection animationGraphSelection;
    /// @note  Inspector の Name 欄でステートを改名したことを Animation Graph パネルへ伝える 1 ショット。
    /// @note  パネルの選択は名前が権威なので、伝えないと改名した瞬間に選択が外れる。
    std::string animationGraphRenamedFrom;
    std::string animationGraphRenamedTo;
    /// @note  Inspector の Layers 欄でレイヤー名を改名したことを Animation Graph パネルへ伝える 1 ショット。
    /// @note Graph はレイヤー名を識別子として保持するため、データ側だけ改名すると旧名が見つからず Base Layer へ戻り Mask 欄も消えたように見える。
    std::string animationGraphLayerRenamedFrom;
    std::string animationGraphLayerRenamedTo;
    /// @note  Inspector で追加・改名したレイヤーを Graph 側で表示する要求。空なら通常の選択を維持する。
    std::string animationGraphLayerFocus;
    /// @note  Inspector で削除したレイヤー。Graph 側の古いステート選択を Base Layer に誤適用させない。
    std::string animationGraphLayerRemoved;
    /// @note  Animation Graph と Inspector が共有する Controller アセット編集モデル。
    std::shared_ptr<scene::AnimatorComponent> animationControllerEditor;
    std::string animationControllerEditorPath;
    bool animationControllerDirty = false;

    /// @note  選択状態 (Multi-select 対応)。直接書き換えず Editor/Util/Selection.hpp の SelectEntity / SelectEntities 等を使う。
    /// @note 選択にはアセット選択との排他と Hierarchy への反映が付いて回るため、直接代入すると後始末をした面としない面が混在する。
    std::vector<scene::EntityID> selectedEntities;

    /// @note  Hierarchy 以外の面 (Scene View / 検索 / Map / AI) が選んだ対象を、ツリー上で
    /// @note  見えるようにする要求。SceneHierarchyPanel が畳まれた祖先を開いてスクロールする。
    /// @note  親が畳まれていると Viewport でクリックした子は Hierarchy に 1 行も現れない。
    scene::EntityID hierarchyRevealTarget = scene::EntityID::INVALID;
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

    /// @note  Inspector が表示する対象を、各パネルのローカル判定ではなくここで解決する。
    /// @note Hierarchy / Asset Browser / Animation Graph がそれぞれ別の優先順位を持つと、Graph 選択後に古い Hierarchy 選択へ戻るなど表示対象と編集対象が食い違う。
    struct InspectorSelection {
        enum class Type {
            None,
            Entity,
            MultiEntity,
            Asset,
            AnimationGraphEntity,
            AnimationGraphAsset
        };

        Type              type = Type::None;
        scene::GameObject* gameObject = nullptr; ///< @note 非所有。activeScene が所有する。
        std::string        assetPath;
    };

    InspectorSelection ResolveInspectorSelection() const
    {
        const bool graphSelected =
            animationGraphSelection.type != AnimationGraphSelection::Type::None;
        if (graphSelected) {
            /// @note Graph の Scene 選択は Hierarchy の現在選択より優先する。
            if (activeScene && animationGraphSelection.entityId.IsValid()) {
                if (auto* gameObject = activeScene->GetGameObject(animationGraphSelection.entityId)) {
                    InspectorSelection result;
                    result.type = InspectorSelection::Type::AnimationGraphEntity;
                    result.gameObject = gameObject;
                    return result;
                }
            }

            /// @note Asset Graph は Hierarchy に Entity が残っていても、開いている Controller を優先する。
            if (animationControllerEditor && !animationControllerEditorPath.empty() &&
                animationGraphSelection.assetPath == animationControllerEditorPath) {
                InspectorSelection result;
                result.type = InspectorSelection::Type::AnimationGraphAsset;
                result.assetPath = animationControllerEditorPath;
                return result;
            }
        }

        /// @note Graph の選択が無い、または無効になった場合だけ通常の選択へフォールバックする。
        if (!selectedAssetPath.empty()) {
            InspectorSelection result;
            result.type = InspectorSelection::Type::Asset;
            result.assetPath = selectedAssetPath;
            return result;
        }
        if (selectedEntities.size() > 1) {
            InspectorSelection result;
            result.type = InspectorSelection::Type::MultiEntity;
            return result;
        }
        if (auto* gameObject = GetSelectedGO()) {
            InspectorSelection result;
            result.type = InspectorSelection::Type::Entity;
            result.gameObject = gameObject;
            return result;
        }
        return {};
    }

    /// @note  カメラ操作設定 (EditorSettings からロードされ、main.cpp が DebugCamera へ適用する)。
    /// @note DebugCamera は Engine 層に属しパネルから直接参照できないため、EditorContext を仲介する。
    float cameraSpeed         = 5.0f;
    float cameraSensitivity   = 0.15f;

    /// @note  ビューポート
    bool  viewportFocused  = false;
    bool  sceneViewportHovered = false; ///< @note シーンビューにマウスが乗っているか (ホイール制御に使う)
    /// @note  今フォーカスされているパネルの «面»。キーの意味はここで決まる。書くのは IPanel::OnRender だけ (GetHotkeyScope() で名乗る)。
    /// @note 毎フレーム None へ落としてから描画で立て直すため、参照側は 1 フレーム遅れで見る。
    HotkeyScope focusedPanelScope = HotkeyScope::None;

    /// @note  その面に今フォーカスがあるか。パネル内のキー処理もここを通す。
    [[nodiscard]] bool PanelScopeFocused(HotkeyScope scope) const
    {
        return HasScope(focusedPanelScope, scope);
    }
    /// @note  F2 リネーム要求 (HotkeyManager → Hierarchy)。
    /// @note リネームの編集バッファとフォーカス制御はパネル内部にあるため、キーの割り当てだけ HotkeyManager に集約し「何をするか」はパネルに残す 1 ショット。
    bool  requestRenameSelected = false;
    float viewportWidth   = 1280.0f;
    float viewportHeight  = 720.0f;
    bool  gameViewportFocused = false;
    float gameViewportOriginX = 0.0f;
    float gameViewportOriginY = 0.0f;
    float gameViewportWidth   = 1280.0f;
    float gameViewportHeight  = 720.0f;
    /// @note  Game View がこのフレームに実際に描かれ、上の矩形が «今の画面» を指しているか。
    /// @note 寸法と原点はパネルが描いたときにしか書かれず閉じている間は前回値が残るため、カーソル拘束のような «外れると操作不能» な用途は古い矩形を掴んではいけない。
    bool  gameViewportRectValid = false;
    bool  requestGameViewportFocus = false; ///< @note Play 開始時に Game ビューへフォーカスを移す one-shot フラグ。ViewportPanel が消費する
    /// @note  Escape で解放したカーソルをゲームへ返す one-shot 要求。Game View をクリックしたときと
    /// @note  オーバーレイの「Click to capture」から立ち、EditorApp が消費する。
    bool  requestGameCursorCapture = false;
    /// @note  Play 開始時の «ウィンドウの並べ方»。カーソルの扱いはここでは決めない
    /// @note  (スクリプトの cursor.Push と playCursorOverride が決める)。
    enum class PlayFocusMode {
        Focused,
        Maximized,
        Unfocused
    };
    PlayFocusMode playFocusMode = PlayFocusMode::Maximized; ///< @note Unity の Play Focused / Maximized / Unfocused 相当
    /// @note  Play 中のカーソル要求を、スクリプトを書き換えずに一時的に外す口。
    /// @note Confined + 非表示のゲームを Editor で触るとき、確認のたびにスクリプトへ手を入れて再ビルドさせたくない。
    enum class PlayCursorOverride {
        Game,      ///< @note ゲーム (cursor.Push / SetLockMode) の要求どおりにする
        Free       ///< @note 常に None + 表示 (デバッグ用)
    };
    PlayCursorOverride playCursorOverride = PlayCursorOverride::Game;
    bool  mapEditingMode = false; ///< @note Scene Viewport 中心の Map 専用 Workspace が有効か
    bool  requestMapEditingModeToggle = false; ///< @note Toolbar/Menu からの Workspace 切替要求
    /// @note  Map Editing Mode の選択中ツール。
    /// @note MapEditorPanel と Scene Viewport のオーバーレイツールバー / 数字キーが同じ選択状態を共有するため、パネルのローカル変数ではなく Context に置く。
    enum class MapTool {
        TerrainSculpt,
        TerrainPaint,
        Grid,
        TerrainHole, ///< @note 保存済み int 値を変えないため末尾に置く
    };
    MapTool mapActiveTool = MapTool::TerrainSculpt;
    bool  mapHierarchyFilter = true; ///< @note Map Mode 中に TerrainGrid/Terrain/Water だけ表示
    bool  mapInspectorFilter = true; ///< @note Map Mode 中に Map 関連 Component だけ表示
    /// @note  システムが実行時に生成した GameObject (GameObject::runtimeGenerated) を Hierarchy へ出すか。
    /// @note VFX Graph は 1 エフェクトにつきノード数ぶんの GameObject を作るため、既定は非表示にして親 1 行へ畳み、デバッグ時だけ開く。
    bool  showGeneratedObjects = false;
    bool  uiViewportFocused = false;
    float uiViewportOriginX = 0.0f;
    float uiViewportOriginY = 0.0f;
    float uiViewportWidth = 1280.0f;
    float uiViewportHeight = 720.0f;
    /// @note  UIViewport が編集対象にする ScreenSpace Canvas。
    /// @note 複数 Canvas があると「最初の Canvas」を暗黙選択するだけでは編集対象が不安定になる。
    scene::EntityID activeUICanvas = scene::EntityID::INVALID;

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

    /// @note  ギズモ
    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };
    /// @note  ギズモを置く基準点 (Unity の Pivot / Center トグル相当)。
    /// @note  Pivot はプライマリ選択の原点。複数選択で回すと「たまたま最初に選んだもの」が軸になる。
    /// @note  Center は選択全体のバウンズ中心を軸にする (足元原点のキャラなど単一選択でも効く)。
    enum class GizmoPivot { Pivot, Center };
    GizmoMode  gizmoMode  = GizmoMode::Translate;
    GizmoSpace gizmoSpace = GizmoSpace::World;
    GizmoPivot gizmoPivot = GizmoPivot::Pivot;

    /// @note  グリッド・スナップ
    bool  showGrid     = true;
    float gridSize     = 1.0f;
    bool  snapEnabled  = false;
    float snapPos      = 1.0f;    ///< @note 位置スナップ (m)
    float snapRot      = 15.0f;   ///< @note 回転スナップ (度)
    float snapScale    = 0.25f;   ///< @note スケールスナップ

    /// @note  頂点スナップ / 面スナップ (ViewportPanel が押下状態を反映する)。
    /// @note V 押下中は選択メッシュの頂点を他メッシュの頂点へ吸着し、Ctrl+Shift ドラッグ中はカーソル下のサーフェスへ接地させる (Unity 互換)。
    bool  vertexSnapActive  = false;
    bool  surfaceSnapActive = false;
    /// @note  面スナップ時に、接地面の法線へオブジェクトの上方向を合わせるか。
    bool  surfaceSnapAlignToNormal = false;

    /// @note  プロジェクト設定
    fbzz::ProjectSettings projectSettings;

    /// @note  表示オプション (エディター固有)
    /// @note  エミッターの発生形状と初速のワイヤー表示。
    bool showVFXGizmos = false;
    /// @note Scene View の布固定点ブラシ。シーンに保存しないエディター状態。
    bool clothPinPainting = false;
    /// @note  FlowField の範囲・減衰・流れの向き。
    bool showFlowFields = false;
    /// @note  格子点で評価した実効流速の矢印。範囲は選択中の FlowField、無ければカメラ前方。
    bool showFlowSamples = false;
    /// @note  VolumeComponent のトリガー形状と効果。
    bool showPhysicsVolumes = false;
    /// @note  全 WaterComponent の水流・渦・浮力の届く深さ。選択中の水面ギズモと違い常時出す。
    bool showWaterFlow = false;
    /// @note  ラグドールの剛体・可動域・接触点。飽和した関節が赤く出る。
    /// @note  常時出すと骨の絵が線で埋まるので、可動域とサーボを詰めるときだけ点ける。
    bool showRagdoll   = false;
    bool showLightRange  = true;
    bool showSkeleton    = false;
    /// @name Scene View の診断オーバーレイ (RenderSettings の同名フラグへ毎フレーム複写)
    ///@{
    /// @note  true なら選択中のスケルトンだけ描く。
    bool skeletonSelectedOnly = true;
    /// @note  スクリプトの OnDrawGizmos / debug.Draw*。Game View と配布ビルドでは常に出ない。
    bool showScriptGizmos  = true;
    bool showConstraints   = false;
    bool showRigidBodies   = false;
    bool showIK            = false;
    bool showSpringBones   = false;
    bool showAttachments   = false;
    bool showVFXPaths      = false;
    bool showTerrainBounds = false;
    bool showLODBounds     = false;
    ///@}
    /// @brief Scene View のコンポーネントアイコン全体の表示。false ならアイコンのピッキングも止まる。
    bool showSceneIcons  = true;
    /// @brief 非表示にしたアイコン種別の保存キー。
    /// @see SceneIconTypeKey
    std::vector<std::string> hiddenSceneIcons;
    /// @note  Scene View で CPU ソフトウェアオクルージョンカリングを効かせるか。
    /// @note  Scene View はデバッグカメラで描くので CameraComponent の設定が効かない。
    /// @note  既定は無効 (編集中は見えているものが見えることを優先)。効きは Stats の Occlusion 行。
    bool sceneViewOcclusionCulling = false;
    bool showStats       = true;  ///< @note Game Viewport に Stats オーバーレイを表示する
    bool showTerrainTool = false; ///< @note Terrain Tool ウィンドウを表示する
    bool hotReloadEnabled = true;
    /// @brief ホットリロードの完了・失敗を音で知らせるか。@see EditorSettings::hotReloadSound
    bool hotReloadSound   = true;

    /// @note  スクリプト DLL / HLSL ホットリロード状態 (StatusBar が表示する)
    enum class HotReloadState { Idle, Compiling, Reloading, Done, Failed };
    HotReloadState hotReloadState   = HotReloadState::Idle;
    std::string    hotReloadMessage;   ///< @note StatusBar に表示するテキスト
    /// @note  Script DLL はデバウンス・ビルド・リロードの段階進捗を 0.0〜1.0 で公開する。
    /// @note MSBuild は安定した作業総数を返さないため、負値は実進捗を算出できない処理を表す。
    float          hotReloadProgress = -1.0f;
    float          hotReloadDoneTimer = 0.0f; ///< @note Done / Failed 表示を消すカウントダウン (秒)
    /// @brief 表示中のホットリロードがどちらの対象か。ツールバーの短い表示とトーストの主語に使う。
    enum class HotReloadTarget { Scripts, Shaders };
    HotReloadTarget hotReloadTarget = HotReloadTarget::Scripts;
    /// @brief 変更検知 (Compiling へ入った瞬間) の ImGui::GetTime() [s]。経過秒の表示に使う。
    double         hotReloadStartTime  = 0.0;
    /// @brief Done / Failed へ入った瞬間の ImGui::GetTime() [s]。確定後は経過秒をここで止める。
    double         hotReloadFinishTime = 0.0;
    /// @brief スクリプトの直近リロードの結果。ツールバーの Reload アイコンが ✓ / ✗ を出すのに使う。
    /// @note hotReloadState と違い時間で消えない。HLSL の結果でも上書きされない。
    enum class ScriptReloadResult { None, Ok, Failed };
    ScriptReloadResult scriptReloadResult = ScriptReloadResult::None;
    bool           scriptReloadBusy = false;  ///< @note Script DLL のビルド待ち / ロード中は Play 開始を止める

    /// @note  ビルドコンソール: Script / HLSL コンパイルの出力・診断・履歴の唯一の情報源。
    /// @note EditorApp が所有し、Build Output パネル・StatusBar・通知バーが読み取る。
    BuildConsole*  buildConsole = nullptr;
    bool           requestOpenBuildOutput  = false; ///< @note Build Output パネルを開く要求 (StatusBar クリック等)
    bool           requestFocusBuildError  = false; ///< @note 開いた上で最初のエラーへスクロールする要求 (通知バー)

    /// @note  パネル間リクエスト (one-shot フラグ: 発行側が true にセット → 受信側が処理後 false にリセット)
    bool requestOpenProjectSettings  = false;
    bool requestOpenBuildSettings    = false;
    bool requestOpenAnalysis         = false;
    bool requestOpenAnimationGraph   = false; ///< @note .animcontroller ダブルクリック → AnimationGraphPanel を開く
    bool requestOpenBehaviorTree     = false; ///< @note .behaviortree ダブルクリック → BehaviorTreePanel を開く
    bool requestOpenSfxEditor        = false; ///< @note .synth ダブルクリック → SfxEditorPanel を開く
    bool requestOpenSequence         = false; ///< @note .sequence ダブルクリック → SequencePanel を開く
    /// @note  .fluid Inspector の «Bake in 3D» → Volume Flipbook Baker を開き、この .fluid を Fluid ソースにする。
    /// @note  one-shot。消すのはパネル (要求を読んだとき)。
    std::string requestVolumeFlipbookFluid;
    /// @note  .fluid ダブルクリック / Inspector の «Open in Fluid Editor» → Fluid Editor でこの .fluid を開く (実パス)。
    /// @note  one-shot。消すのは Fluid Editor パネル (要求を読んだとき)。
    std::string requestOpenFluidEditor;

    /// @note  Inspector 等のアセット参照欄 → AssetBrowser: 参照先を一覧上で選択させる要求 (one-shot)。
    /// @note パスは絶対 / Assets 相対どちらでもよく AssetBrowser 側が解決する。空文字 = 要求なし。
    std::string requestRevealAssetPath;
    /// @note  true = 一覧で選択するだけでなく、Inspector の表示対象もそのアセットへ移す (ダブルクリック)。
    bool        requestRevealAssetSelect = false;

    /// @name Prefab 編集モード
    /// @{
    /// @note  編集中のシーンを一時退避して .prefab だけを開き、閉じるときに戻す。
    /// @note  「インスタンスを選んで Apply」だけだと、シーンに 1 個も置いていないものを直せない。
    std::string requestOpenPrefabEdit;        ///< @note one-shot: 開きたい .prefab (Assets 相対)
    bool     requestSavePrefabEdit  = false;
    bool     requestClosePrefabEdit = false;  ///< @note 保存せず戻る
    std::string prefabEditPath;                  ///< @note 空でなければ編集モード中 (Assets 相対)
    bool     prefabEditDirty = false;
    [[nodiscard]] bool InPrefabEditMode() const { return !prefabEditPath.empty(); }

    /// @note  ディスク上で書き換わった .prefab の絶対パス。AssetBrowser のファイル監視が積み、
    /// @note  EditorApp が「シーン内のインスタンスへ反映」して消費する。
    /// @note  ファイルの変化そのものを拾うので、外部エディタからの変更にも追従する。
    std::vector<std::string> pendingPrefabReloads;

    /// @note  ディスク上で書き換わったアセット (.mat / .anim / .animcontroller / .mask /
    /// @note  .fzdata / .physmat / .synth / .terrain) の絶対パス。積むのは AssetBrowser のファイル監視、
    /// @note  消費するのは EditorApp。
    /// @note  積んでから処理する。監視イベントはパネル描画の途中で届くので、その場でキャッシュを
    /// @note  差し替えると同じフレームのパネル同士が別の版を見ることになる。
    std::vector<std::string> pendingAssetReloads;

    /// @note  ディスク上で書き換わった .scene の絶対パス。
    /// @note シーンの再読込は EntityID・選択・Undo 履歴を作り直すため、アセットとは別経路で未保存判定・Play 判定を通す。
    std::vector<std::string> pendingSceneReloads;
    bool requestScriptReload         = false;  ///< @note StatusBar の ↻ ボタン → TickScriptCompile が処理

    /// @note  パネルが書き換えた EditorSettings を今すぐ editor_settings.toml へ書き出す要求。
    /// @note 通常の回収は Shutdown 一括だが、ビルド構成のように次の起動まで待てない設定があるため、強制終了しても直前の編集が残るようにする。
    bool requestEditorSettingsSave   = false;

    /// @brief ProjectSettings.toml の自動保存の状態。書くのは EditorApp、読むのは Project Settings パネル。
    enum class SettingsSaveState { Saved, Pending, Failed };
    SettingsSaveState projectSettingsSaveState = SettingsSaveState::Saved;
    /// @brief 最後に書き出せたローカル時刻 ("14:03:22")。まだ書いていなければ空。
    std::string       projectSettingsSavedClock;
    /// @brief 保存先の絶対パス (表示用)。
    std::string       projectSettingsPath;
    /// @brief 待ち時間を飛ばして今すぐ保存する要求 (失敗時の Retry)。one-shot。
    bool              requestProjectSettingsSave = false;

    /// @brief シーンのオートセーブ (Unreal 方式: 一定間隔で Library/AutoSave へ退避)。正本は EditorSettings。
    bool  sceneAutoSaveEnabled     = true;
    int   sceneAutoSaveIntervalSec = 300;
    /// @brief 次のオートセーブまでの残り秒。数えていない (未変更・Play 中・無効) ときは負。
    float sceneAutoSaveRemainingSec = -1.0f;

    /// @note  F キーフォーカス: ViewportPanel がセット → main.cpp が DebugCamera に適用してクリア
    bool            requestFocusOnSelected = false;
    math::Vector3   focusTargetPosition    = {};
    /// @note  選択バウンディング球の半径。0 は「バウンズ不明」で従来の固定距離フォーカスになる。
    float           focusTargetRadius      = 0.0f;

    /// @note  カメラブックマーク呼び出し: ViewportPanel がセット → EditorApp が Teleport してクリア
    bool             requestTeleportCamera  = false;
    math::Vector3    teleportPosition       = {};
    math::Quaternion teleportRotation       = {};

    /// @note  Scene View の射影切り替え: Operator / パネルがセット → EditorApp が DebugCamera へ適用してクリア。
    /// @note 射影を変えるとピボットからの距離も変わるため、Camera::m_projection だけの書き換えは pivot/focusDistance と食い違い、次のオービットで視点が飛ぶ (Teleport 要求と同じ理由)。ProjectionMode は enum class で完全な定義が要るため Camera.hpp を include する。
    bool                    requestCameraProjection = false;
    renderer::ProjectionMode cameraProjection       = renderer::ProjectionMode::Perspective;

    /// @note  エディター専用: ロック中の EntityID 一覧（シリアライズしない）
    std::vector<scene::EntityID> lockedEntities;
    bool IsLocked(scene::EntityID id) const {
        return std::find(lockedEntities.begin(), lockedEntities.end(), id) != lockedEntities.end();
    }
    void ToggleLock(scene::EntityID id) {
        auto it = std::find(lockedEntities.begin(), lockedEntities.end(), id);
        if (it != lockedEntities.end()) lockedEntities.erase(it);
        else lockedEntities.push_back(id);
    }

    /// @note  エディター専用: 非表示 instanceId → 非表示前の activeSelf 値 (シリアライズしない)。
    /// @note runtime の activeSelf フラグと切り離し、参照メッシュ等をエディタ上だけ隠す。Play/Save 時に一時解除してこの値を復元する。
    std::unordered_map<std::string, bool> editorHiddenGuids;

    /// @note  Inspector の Transform ヘッダーメニュー (Copy / Paste) と
    /// @note  AI の transform.copy / transform.paste が共有するクリップボード。
    /// @note  別々に持つと、同じ操作名なのに中身が違うという追いにくい食い違いになる。
    struct TransformClipboard {
        bool             has = false;
        math::Vector3    position{};
        math::Quaternion rotation{};
        math::Vector3    scale{ 1.0f, 1.0f, 1.0f };
    };
    TransformClipboard transformClipboard;

    /// @note  Util (非所有)
    HotkeyManager*      hotkeyManager = nullptr;
    UndoStack*          undoStack   = nullptr;
    /// @note  エディター操作の登録簿 (EditorApp 所有)。メニュー・ホットキー・パレットに加え、AI Command Bus の editor.op.list / editor.op.invoke がここを読む。
    /// @note AI 専用の実装経路を作らないための唯一の入口。
    /// @see Docs/design/editor-operator-model.md
    OperatorRegistry*   operators   = nullptr;
    /// @brief 必須設定の検証結果と明示的な Inspector 移動要求。
    ScriptRequirementReview scriptRequirementReview;
    /// @note  ログ集約シンク (EditorApp 所有)。AI の console.logs クエリが履歴を読む。
    ConsoleSink*        consoleSink = nullptr;
    /// @note  AI 連携のライブ状態と制御口。Panel は EditorApp の所有物へ直接依存せず、この窓口だけを使う。
    bool                aiCommandBusEnabled = false;
    bool                aiCommandBusRunning = false;
    std::function<bool()> startAiCommandBus;
    std::function<void()> stopAiCommandBus;
    /// @brief Playtest の実行器と入力記録 (EditorApp 所有)。playtest.* / input.record が使う。
    playtest::PlaytestRunner* playtest      = nullptr;
    playtest::InputRecorder*  inputRecorder = nullptr;
    /// @note  Inspector から専用 Sprite Editor を開く。パネル所有権を公開せず要求だけを渡す。
    std::function<void(const std::string&)> openSpriteEditor;
    /// @note  .behaviortree を BehaviorTreePanel へ渡す (同じくパネル実体は公開しない)。
    std::function<void(const std::string&)> openBehaviorTree;
    /// @note  .synth を SfxEditorPanel へ渡す。
    std::function<void(const std::string&)> openSfxEditor;
    /// @note  .sequence を SequencePanel へ渡す。
    std::function<void(const std::string&)> openSequence;

    /// @note  SFX Editor が編集中の手続き効果音。読み書きが同じ場所を指すよう context に置く。
    /// @note  パネルが実体を抱えて Operator が要求を積む形だと、set_param の直後に inspect を
    /// @note  呼んだときパネルがまだ要求を消費しておらず 1 フレーム古い値が返る。
    std::string      sfxEditorPath;            ///< @note 空 = 未保存の下書き
    audio::SynthSpec sfxEditorSpec;
    std::string      sfxEditorPresetName;      ///< @note 由来プリセット名 (空 = 手編集)
    bool             sfxEditorDirty = false;
    /// @note  BehaviorTreePanel が開いているドキュメント (パネルが毎フレーム公開する)。
    /// @note  Operator の poll が「今この操作ができるか」を判定するのに使う。
    std::string behaviorTreeEditorPath;
    /// @note  整列要求 (ワンショット)。パネルが消費して AutoLayout を実行する。
    /// @note 整列は Undo スタックを通す必要があり、それを持つのはパネル。外から木の中身だけ書き換えると整列前へ戻せなくなる。
    bool requestBehaviorTreeAutoLayout = false;
    /// @note  .animcontroller を AnimationGraphPanel へ渡す。
    /// @note selectedAssetPath への追従だと Asset Browser のクリックだけで編集対象が入れ替わり未保存の変更が確認なしに消えるため、「開く」を明示的な操作として切り出す。
    std::function<void(const std::string&)> openAnimationGraph;
    PlayModeController* playMode    = nullptr;
    TerrainTool*        terrainTool = nullptr; ///< @note EditorApp が所有、ViewportPanel が使用
    /// @note  Inspector セクション折り畳み状態 (EditorSettings ↔ ImGui StateStorage の中継)
    std::vector<std::pair<uint32_t, bool>> inspectorSectionState;
    /// @note  現在の Scene に紐づく Editor 専用メタデータ。Scene 本体には保存しない。
    EditorSceneState editorSceneState;
    /// @note  Inspector がカテゴリ別の既存描画関数から Component カードを収集するための一時窓口。
    /// @note  1 フレームだけ設定し、描画順を決定した後に必ず nullptr へ戻す。
    InspectorComponentDrawCollector* inspectorComponentCollector = nullptr;

    /// @note  デフォルトインポート設定 (EditorSettings に永続化)
    FbxImportOptions                        defaultImportOptions;

    /// @note  Inspector → AssetBrowser: Reimport モーダルを開くリクエスト（empty = なし）
    std::string                             requestOpenImportModal;

    std::function<void()>                   markSceneDirty;
    std::function<void(const std::string&)> requestOpenScene;
    bool                                    requestAssetBrowserRefresh = false;
    bool                                    requestSaveScene = false; ///< @note StatusBar の●クリック等から現在シーン保存を要求

    /// @note  AI (Command Bus の scene.open / scene.save) 専用のシーン入出力。確認モーダルを挟まない。
    /// @note  モーダルを開くとエディタが人のクリック待ちで止まり、バスの drain も止まる。
    /// @note  未保存の確認は AI 側の引数 (discardUnsaved) で成立させる。
    /// @note  save の path が空なら現在のシーンパスへ上書き保存する。
    std::function<bool(const std::string&)> openScenePathImmediate;
    std::function<bool(const std::string&)> saveScenePathImmediate;
    /// @note Asset Browser のアイコンサイズ / ツリー幅はここには持たない。パネルは複数開けるので「どのパネルの値か」が決まらず、1 パネルぶんの状態は EditorSettings::assetBrowserPanels[instance] が唯一の持ち場で OnLoadSettings / OnSaveSettings だけで往復する。
    float                                   editorUiScale              = 1.0f;   ///< @note UI 全体スケール (永続化)
    std::vector<std::string>                assetBrowserBookmarks;
    /// @note  フォルダごとの色 (Unreal の Set Color 相当)。キーは絶対パス、値は IM_COL32 の packed 値。
    /// @note Favorites と同じく EditorSettings 経由で永続化され、左ツリー・グリッド・パンくずの複数の描画箇所が同じ 1 つの表を引くため ctx に置く。
    std::unordered_map<std::string, uint32_t> assetBrowserFolderColors;
    /// @note  一覧を作り直すべき変更が起きた回数。
    /// @note requestAssetBrowserRefresh は Asset Browser を複数開けると最初に描いたパネルが消費して残りへ届かないため、世代を進め各パネルが自分の適用済み世代と比べる方式にした。
    uint64_t                                 assetBrowserRefreshGeneration = 0;
    /// @note  Set Color で最近使った色 (新しい順)。プリセットに無い色を選び直す手間を省く。
    std::vector<uint32_t>                    assetBrowserRecentFolderColors;

    /// @note  カメラブックマーク (最大 9 件、Shift+1~9 で保存・1~9 で呼び出し)
    struct CameraBookmark {
        math::Vector3    position;
        math::Quaternion rotation;
        bool             valid = false;
    };
    std::array<CameraBookmark, 9>           cameraBookmarks;

    /// @note  Inspector → AssetBrowser: 編集中 .mat のサムネイル即時更新。
    /// @note  サムネイルはファイル更新時刻でしか再生成されないので、値が変わるたびにリビジョンを進め、
    /// @note  AssetBrowser が未保存の MaterialAsset から描き直せるようにする。
    /// @note  キーは NormalizeAssetPath 済みのプロジェクト相対パス。
    std::unordered_map<std::string, uint64_t> materialPreviewRevisions;

    void BumpMaterialPreviewRevision(const std::string& relativeMaterialPath)
    {
        if (relativeMaterialPath.empty()) return;
        ++materialPreviewRevisions[relativeMaterialPath];
    }
    uint64_t MaterialPreviewRevision(const std::string& relativeMaterialPath) const
    {
        const auto it = materialPreviewRevisions.find(relativeMaterialPath);
        return it == materialPreviewRevisions.end() ? 0ull : it->second;
    }
    /// @}
};

} /// @note namespace fbzz::editor
