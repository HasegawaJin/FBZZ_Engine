// FBZZ Engine
// EditorContext.hpp | fbzz::editor
// パネル間で共有するエディター状態
#pragma once
#include <Editor/GraphLayout.hpp>
#include <Editor/Import/FbxImportTool.hpp>
#include <Editor/Util/EditorSceneState.hpp>
#include <Engine/Audio/SynthSpec.hpp>
#include <Engine/ProjectSettings.hpp>
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
namespace fbzz::editor   { class UndoStack; class PlayModeController; class TerrainTool; class WaterTool; class HotkeyManager; class BuildConsole; class ConsoleSink; class OperatorRegistry; }

namespace fbzz::editor {

struct InspectorComponentDrawCollector;

struct EditorContext {
    // Animation Graph 上で選択中の編集対象をパネル間で共有する。
    // WHY: ノードキャンバスと詳細編集を別パネルへ分離するため、ポインタではなく EntityID と index を保持する。
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
        // どのレイヤーのグラフに対する選択か。空 = Base Layer。
        // WHY: stateIndex はグラフごとの添字なので、レイヤー名が無いと
        //      Inspector が別レイヤーのステートを編集してしまう。
        std::string     layerName;
        int             stateIndex = -1;
        int             transitionIndex = -1;
        // BlendTree 内で選択中の Motion。stateIndex が親 State、motionIndex が Motion の添字。
        // WHY: Motion は State の配列とは別の入れ子データなので、State と同じ index だけでは
        //      Inspector がどの Motion を編集すべきか特定できない。
        int             motionIndex = -1;
        // グラフ上で掴んでいるステートの数 (矩形選択・Ctrl クリック)。
        // WHY 数だけ持つか: Inspector が編集するのは常にプライマリ 1 件だが、
        //     Delete は選択全部に効く。件数を見せないと「1 個選んでいるつもりで
        //     3 個消える」ように見えるため、食い違いを UI 上で明示する。
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

    // エンジンオブジェクト (非所有)
    scene::Scene*     activeScene  = nullptr;
    // 編集中Sceneを駆動するProjectRuntime (EditorApp所有)。Physics World等の実行時系へアクセスする。
    // WHY: AI(EditorBusDispatcher)のraycast/overlap等はこのruntimeのPhysics Worldへ問い合わせる。
    scene::ProjectRuntime* runtime = nullptr;
    // VFXEditor 専用の一時 World。編集 Scene の階層・Undo・保存へ Preview Object を混入させない。
    scene::Scene*     vfxPreviewScene = nullptr;
    // AI の決定論プレビュー要求は VFXEditor が閉じていても専用 World を数フレームだけ駆動する。
    // WHY: query 応答と GPU 描画は同一フレームで完結しないため、capture までの猶予を frame 値で共有する。
    std::uint64_t     vfxAiPreviewUntilFrame = 0;
    // AI の viewport.capture / viewport.semantic が RT を読み出す間、表示状態に関わらず
    // Scene View / Game View を描き続けるための猶予フレーム。
    // WHY: EditorApp は「実際に画面へ出ているビューポートだけ」を描くようになったため、
    //      Play 中 (Scene View が隠れる) や Map 編集中 (Game View が隠れる) に AI が
    //      キャプチャすると、描画が止まった RT の古い内容を読んでしまう。
    //      vfxAiPreviewUntilFrame と同じく frame 値で猶予を共有する。
    std::uint64_t     aiViewportRenderUntilFrame = 0;
    renderer::Camera* editorCamera = nullptr;
    // DebugCamera が持つオービット中心と注視距離のミラー (EditorApp が毎フレーム更新)。
    // WHY: ナビゲーションギズモは「ピボットを動かさずに視点だけ回す」ため、
    //      カメラ位置からは復元できない DebugCamera 内部の中心・距離を必要とする。
    math::Vector3     editorCameraPivot         = {};
    float             editorCameraFocusDistance = 10.0f;
    renderer::IRenderer* renderer = nullptr;
    renderer::IImGuiRenderer* imguiRenderer = nullptr;
    renderer::ResourceManager* resources = nullptr;
    core::MemorySystem* memorySystem = nullptr;
    std::string       projectRoot;
    std::string       projectBuildRoot;
    std::string       engineRoot;          // GameHubが選択したimmutable SDK root (cmake configureへ渡す)
    std::string       standaloneTargetName = "SandboxStandalone";
    std::string       projectTargetName;   // .fbzz_proj の target_name (GameHub プロジェクトの識別に使う)

    // ScriptCodeGen / ScriptDllLoader が使うソースパス (InitScriptDll で設定)
    std::string       scriptsSourceDir;     // Assets/Scripts/ の絶対パス
    std::string       scriptsDllCppPath;    // SandboxScriptsDll.cpp の絶対パス (DLL 登録)
    std::string       scriptsStaticCppPath; // SandboxScripts.cpp の絶対パス (EXE 静的登録)
    std::string       scriptsDllPath;       // コンパイル済み DLL の絶対パス (BuildPipeline が配布物へコピー)
    std::string       hlslSourceDir;       // Assets/shaders/ の絶対パス
    std::string       currentScenePath;
    std::string       selectedAssetPath; // アセットブラウザーで選択中のファイル絶対パス (空 = なし)
    bool              sceneDirty = false;

    // OSエクスプローラーからの外部ドラッグ&ドロップ受け渡しチャンネル (プラットフォーム層 → AssetBrowser)。
    // WHY: AssetBrowserImport がドロップ確定を遅延処理するため、ドラッグ中のライブ位置と
    //      ドロップ確定ファイル群をフレーム跨ぎで共有する。producer が空のままなら AssetBrowser 側は何もしない。
    bool                     externalDragActive = false; // ドラッグ中ハイライト用
    float                    externalDragX = 0.0f;       // ドラッグ中カーソル位置 (screen space)
    float                    externalDragY = 0.0f;
    std::vector<std::string> droppedExternalFiles;       // 今フレーム確定したドロップファイルの絶対パス群
    float                    droppedExternalFilesX = 0.0f; // ドロップ確定位置 (screen space)
    float                    droppedExternalFilesY = 0.0f;
    // Animation Graph Editor のノード配置。Editor 専用のため SceneSerializer には渡さない。
    // WHY: AnimatorComponent はランタイム構造体なので、キャンバス座標は Scene 内 GO の
    //      instanceId または Controller アセットパスをキーにした別データとして保持する。
    std::unordered_map<std::string, GraphLayout> graphLayouts;
    AnimationGraphSelection animationGraphSelection;
    // Inspector の Name 欄でステートを改名したことを Animation Graph パネルへ伝える 1 ショット。
    // WHY: パネルの選択は「名前が権威」(ResolveSelectionIndices) なので、
    //      Inspector だけで改名すると次フレームに旧名が見つからず、
    //      名前を変えた瞬間にグラフ上の選択が外れてしまう。
    //      パネルは描画の先頭でこれを消費し、覚えている名前を差し替える。
    std::string animationGraphRenamedFrom;
    std::string animationGraphRenamedTo;
    // Inspector の Layers 欄でレイヤー名を改名したことを Animation Graph パネルへ伝える 1 ショット。
    // WHY: Graph はレイヤー名を編集対象の識別子として保持しているため、データ側だけを
    //      改名すると次フレームに旧名が見つからず Base Layer へ戻り、Mask 欄も消えたように見える。
    std::string animationGraphLayerRenamedFrom;
    std::string animationGraphLayerRenamedTo;
    // Inspector で追加・改名したレイヤーを Graph 側で表示する要求。空なら通常の選択を維持する。
    std::string animationGraphLayerFocus;
    // Inspector で削除したレイヤー。Graph 側の古いステート選択を Base Layer に誤適用させない。
    std::string animationGraphLayerRemoved;
    // Animation Graph と Inspector が共有する Controller アセット編集モデル。
    std::shared_ptr<scene::AnimatorComponent> animationControllerEditor;
    std::string animationControllerEditorPath;
    bool animationControllerDirty = false;

    // 選択状態 (Multi-select 対応)
    // 直接書き換えず Editor/Util/Selection.hpp の SelectEntity / SelectEntities 等を使う。
    // WHY: 選択にはアセット選択との排他と Hierarchy への反映が付いて回る。
    //      ここへ直接代入すると、その後始末をした面としない面が混在する。
    std::vector<scene::EntityID> selectedEntities;

    // Hierarchy 以外の面 (Scene View / 検索 / Map / AI) が選んだ対象を、
    // ツリー上で見えるようにする要求。SceneHierarchyPanel が消費して
    // 畳まれた祖先を開き、その行までスクロールする。
    // WHY: 親が畳まれていると、Viewport でクリックした子は Hierarchy に 1 行も現れない。
    //      「何を選んだのか」を確かめる場所が無くなるため、発生源に関わらず見せに行く。
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

    // Inspector が表示する対象を、各パネルのローカル判定ではなくここで解決する。
    // WHY: Hierarchy / Asset Browser / Animation Graph がそれぞれ別の優先順位を持つと、
    //      Graph を選択しても古い Hierarchy 選択へ戻るなど、表示対象と編集対象が食い違う。
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
        scene::GameObject* gameObject = nullptr; // 非所有。activeScene が所有する。
        std::string        assetPath;
    };

    InspectorSelection ResolveInspectorSelection() const
    {
        const bool graphSelected =
            animationGraphSelection.type != AnimationGraphSelection::Type::None;
        if (graphSelected) {
            // Graph の Scene 選択は Hierarchy の現在選択より優先する。
            if (activeScene && animationGraphSelection.entityId.IsValid()) {
                if (auto* gameObject = activeScene->GetGameObject(animationGraphSelection.entityId)) {
                    InspectorSelection result;
                    result.type = InspectorSelection::Type::AnimationGraphEntity;
                    result.gameObject = gameObject;
                    return result;
                }
            }

            // Asset Graph は Hierarchy に Entity が残っていても、開いている Controller を優先する。
            if (animationControllerEditor && !animationControllerEditorPath.empty() &&
                animationGraphSelection.assetPath == animationControllerEditorPath) {
                InspectorSelection result;
                result.type = InspectorSelection::Type::AnimationGraphAsset;
                result.assetPath = animationControllerEditorPath;
                return result;
            }
        }

        // Graph の選択が無い、または無効になった場合だけ通常の選択へフォールバックする。
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

    // カメラ操作設定 (EditorSettings からロードされ、main.cpp が DebugCamera へ適用する)
    // WHY: DebugCamera は Engine 層に属しパネルから直接参照できない。
    //      EditorContext を仲介とすることで、将来的にカメラ設定 UI を
    //      任意のパネルから編集できるようにしている。
    float cameraSpeed         = 5.0f;
    float cameraSensitivity   = 0.15f;

    // ビューポート
    bool  viewportFocused  = false;
    bool  sceneViewportHovered = false; // シーンビューにマウスが乗っているか (ホイール制御に使う)
    // HotkeyManager が「今どの文脈のキーを受け付けるか」を決めるためのフォーカス状態。
    // WHY: 同じ Delete でも Scene View と Hierarchy では対象が違い、F2 は Hierarchy
    //      だけで意味を持つ。各パネルが自分の描画中に立てて、次フレームの
    //      HotkeyManager::ProcessInput が参照する (1 フレーム遅れるが実用上問題ない)。
    bool  hierarchyFocused    = false;
    bool  assetBrowserFocused = false;
    // F2 リネーム要求 (HotkeyManager → Hierarchy)。
    // WHY: リネームは編集バッファとフォーカス制御がパネル内部にあり、外へ出すと
    //      パネルの内部状態を公開することになる。キーの割り当てだけを
    //      HotkeyManager に集約し、「何をするか」はパネルに残すための 1 ショット。
    bool  requestRenameSelected = false;
    float viewportWidth   = 1280.0f;
    float viewportHeight  = 720.0f;
    bool  gameViewportFocused = false;
    float gameViewportOriginX = 0.0f;
    float gameViewportOriginY = 0.0f;
    float gameViewportWidth   = 1280.0f;
    float gameViewportHeight  = 720.0f;
    bool  requestGameViewportFocus = false; // Play 開始時に Game ビューへフォーカスを移す one-shot フラグ。ViewportPanel が消費する
    enum class PlayFocusMode {
        Focused,
        Maximized,
        Unfocused
    };
    PlayFocusMode playFocusMode = PlayFocusMode::Maximized; // Unity の Play Focused / Maximized / Unfocused 相当
    bool  mapEditingMode = false; // Scene Viewport 中心の Map 専用 Workspace が有効か
    bool  requestMapEditingModeToggle = false; // Toolbar/Menu からの Workspace 切替要求
    // Map Editing Mode の選択中ツール。
    // WHY: MapEditorPanel (設定 UI) と Scene Viewport のオーバーレイツールバー / 数字キーが
    //      同じ選択状態を共有するため、パネルのローカル変数ではなく Context に置く。
    enum class MapTool {
        TerrainSculpt,
        TerrainPaint,
        Water,
        Grid
    };
    MapTool mapActiveTool = MapTool::TerrainSculpt;
    bool  mapHierarchyFilter = true; // Map Mode 中に TerrainGrid/Terrain/Water だけ表示
    bool  mapInspectorFilter = true; // Map Mode 中に Map 関連 Component だけ表示
    // システムが実行時に生成した GameObject (GameObject::runtimeGenerated) を Hierarchy へ出すか。
    // WHY: VFX Graph は 1 エフェクトにつきノード数ぶんの GameObject を作る。爆発を 5 箇所へ
    //      置けば数十行が Hierarchy を埋め、自分で作ったオブジェクトが探せなくなる。
    //      既定は非表示にして行数を親 1 行へ畳み、デバッグしたいときだけ開く。
    bool  showGeneratedObjects = false;
    bool  uiViewportFocused = false;
    float uiViewportOriginX = 0.0f;
    float uiViewportOriginY = 0.0f;
    float uiViewportWidth = 1280.0f;
    float uiViewportHeight = 720.0f;
    // UIViewport が編集対象にする ScreenSpace Canvas。
    // WHY: 複数 Canvas があると「最初の Canvas」を暗黙選択するだけでは編集対象が不安定になる。
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

    // ギズモ
    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };
    // ギズモを置く基準点 (Unity の Pivot / Center トグル相当)。
    // WHY: Pivot はプライマリ選択の原点にギズモを置く。複数選択して回すと
    //      「たまたま最初に選んだオブジェクト」を軸に全体が振り回されるため、
    //      選択全体のバウンズ中心を軸にする Center が要る。単一選択でも、
    //      原点がメッシュの外にあるモデル (足元原点のキャラ等) では Center が効く。
    enum class GizmoPivot { Pivot, Center };
    GizmoMode  gizmoMode  = GizmoMode::Translate;
    GizmoSpace gizmoSpace = GizmoSpace::World;
    GizmoPivot gizmoPivot = GizmoPivot::Pivot;

    // グリッド・スナップ
    bool  showGrid     = true;
    float gridSize     = 1.0f;
    bool  snapEnabled  = false;
    float snapPos      = 1.0f;    // 位置スナップ (m)
    float snapRot      = 15.0f;   // 回転スナップ (度)
    float snapScale    = 0.25f;   // スケールスナップ

    // 頂点スナップ / 面スナップ (ViewportPanel が押下状態を反映する)。
    // WHY: 地形へ建物を隙間なく置く作業が、これが無いと目視 + 数値打ちになる。
    //      V 押下中は選択メッシュの頂点を掴んで他メッシュの頂点へ吸着し、
    //      Ctrl+Shift ドラッグ中はカーソル下のサーフェスへ接地させる (Unity 互換)。
    bool  vertexSnapActive  = false;
    bool  surfaceSnapActive = false;
    // 面スナップ時に、接地面の法線へオブジェクトの上方向を合わせるか。
    bool  surfaceSnapAlignToNormal = false;

    // プロジェクト設定
    fbzz::ProjectSettings projectSettings;

    // 表示オプション (エディター固有)
    // 力場の影響体積とエミッター発生形状のワイヤー表示。
    // シーンへ直接置いた ParticleForceField / ParticleEmitter にも効く。
    bool showVFXGizmos = false;
    // AI が vfx.preview で明示要求したときだけ立つ診断表示。
    // 既定はどちらも false で、評価画は常にクリーンなまま保つ。
    bool vfxAiPreviewOverdraw = false;
    bool vfxAiPreviewGizmos = false;
    // AI capture 用プレビューの視点。vfx.preview の camera 引数で上書きされる。
    // WHY: これが無いと AI は常に同じ 1 方向・1 距離からしか自分の作ったものを見られず、
    //      ゲーム内距離での可読性・ビルボードのシルエット・LOD の切り替わりを一度も検証できない。
    //      注視点からの球面座標で持つのは、AI が「回り込む」「離れる」を 1 パラメーターで
    //      指定できるようにするため (自由なカメラ行列を組ませると再現性が落ちる)。
    struct VFXAiPreviewCamera {
        bool  valid    = false;   // false の間は従来どおり操作用プレビューのカメラを流用する
        float targetX  = 0.0f;    // 注視点 (エフェクト原点まわりを想定)
        float targetY  = 0.5f;
        float targetZ  = 0.0f;
        float distance = 5.0f;    // 注視点からの距離 (m)。ゲーム内距離の検証はここを振る
        float yaw      = 0.0f;    // 度。0 = 正面 (-Z 側から見る) / 90 = 真横
        float pitch    = 10.0f;   // 度。正で見下ろし
        float fovY     = 60.0f;   // 度
    };
    VFXAiPreviewCamera vfxAiPreviewCamera;
    bool showLightRange  = true;
    bool showSkeleton    = false;
    // Scene View で CPU ソフトウェアオクルージョンカリングを効かせるか。
    // WHY: Scene View はデバッグカメラで描くため CameraComponent の設定が効かず、
    //      ここを持たないと「編集ビューで落とすかどうか」を選ぶ手段が一切無い。
    //      既定は無効 (編集中は見えているものが見えることを優先する)。効きは
    //      Viewport の Stats オーバーレイの Occlusion 行で確認できる。
    bool sceneViewOcclusionCulling = false;
    bool showStats       = true;  // Game Viewport に Stats オーバーレイを表示する
    bool showTerrainTool = false; // Terrain Tool ウィンドウを表示する
    bool showWaterTool   = false; // Water Tool ウィンドウを表示する
    bool hotReloadEnabled = true;

    // スクリプト DLL / HLSL ホットリロード状態 (StatusBar が表示する)
    enum class HotReloadState { Idle, Compiling, Reloading, Done, Failed };
    HotReloadState hotReloadState   = HotReloadState::Idle;
    std::string    hotReloadMessage;   // StatusBar に表示するテキスト
    // Script DLL はデバウンス・ビルド・リロードの段階進捗を 0.0〜1.0 で公開する。
    // WHY: MSBuild は安定した作業総数を返さないため、負値は実進捗を算出できない処理を表す。
    float          hotReloadProgress = -1.0f;
    float          hotReloadDoneTimer = 0.0f; // Done / Failed 表示を消すカウントダウン (秒)
    bool           scriptReloadBusy = false;  // Script DLL のビルド待ち / ロード中は Play 開始を止める

    // ビルドコンソール: Script / HLSL コンパイルの出力・診断・履歴の唯一の情報源。
    // WHY: EditorApp が所有し、Build Output パネル・StatusBar・通知バーが読み取る。
    BuildConsole*  buildConsole = nullptr;
    bool           requestOpenBuildOutput  = false; // Build Output パネルを開く要求 (StatusBar クリック等)
    bool           requestFocusBuildError  = false; // 開いた上で最初のエラーへスクロールする要求 (通知バー)

    // パネル間リクエスト (one-shot フラグ: 発行側が true にセット → 受信側が処理後 false にリセット)
    bool requestOpenProjectSettings  = false;
    bool requestOpenBuildSettings    = false;
    bool requestOpenAnalysis         = false;
    bool requestOpenAnimationGraph   = false; // .animcontroller ダブルクリック → AnimationGraphPanel を開く
    bool requestOpenVFXEditor        = false; // .vfx ダブルクリック → VFXEditorPanel を開く
    bool requestOpenBehaviorTree     = false; // .behaviortree ダブルクリック → BehaviorTreePanel を開く
    bool requestOpenSfxEditor        = false; // .synth ダブルクリック → SfxEditorPanel を開く

    // Inspector 等のアセット参照欄 → AssetBrowser: 参照先を一覧上で選択させる要求 (one-shot)。
    // WHY: Unity の Object Field と同じく、参照を辿る動線が無いと「この .mat はどのファイルか」を
    //      確かめるのにフォルダを手で探し回ることになる。パスは絶対 / Assets 相対のどちらでもよく、
    //      AssetBrowser 側が実体へ解決する。空文字 = 要求なし。
    std::string requestRevealAssetPath;
    // true = 一覧で選択するだけでなく、Inspector の表示対象もそのアセットへ移す (ダブルクリック)。
    bool        requestRevealAssetSelect = false;

    // ── Prefab 編集モード ────────────────────────────────────────────────────
    // WHY: プレファブを直す唯一の手段が「インスタンスを選んで Apply」だと、
    //      シーンに 1 個も置いていないプレファブは編集できず、シーン側の
    //      ライティングや隣接オブジェクトに紛れて中身を確認しづらい。
    //      編集中のシーンを一時退避して .prefab だけを開き、閉じるときに戻す。
    std::string requestOpenPrefabEdit;        // one-shot: 開きたい .prefab (Assets 相対)
    bool     requestSavePrefabEdit  = false;
    bool     requestClosePrefabEdit = false;  // 保存せず戻る
    std::string prefabEditPath;                  // 空でなければ編集モード中 (Assets 相対)
    bool     prefabEditDirty = false;
    [[nodiscard]] bool InPrefabEditMode() const { return !prefabEditPath.empty(); }

    // ディスク上で書き換わった .prefab の絶対パス。AssetBrowser のファイル監視が積み、
    // EditorApp が「シーン内のインスタンスへ反映」して消費する。
    // WHY: プレファブを直しても既に置いてある実体が古いままだと、
    //      アセットとシーンの内容が静かに食い違う。外部エディタや別セッションからの
    //      変更も含めて追従させるには、ファイルの変化そのものを拾う必要がある。
    std::vector<std::string> pendingPrefabReloads;

    // ディスク上で書き換わったアセット (.mat / .anim / .animcontroller / .mask /
    // .fzdata / .physmat / .synth / .terrain) の絶対パス。積むのは AssetBrowser のファイル監視、
    // 消費するのは EditorApp。
    // WHY 積んでから処理するか: 監視イベントはパネル描画の途中で届く。その場で
    //      AssetManager のキャッシュを差し替えると、同じフレームで既にアセットを
    //      読み終えたパネルと、これから読むパネルが別の版を見ることになる。
    std::vector<std::string> pendingAssetReloads;

    // ディスク上で書き換わった .scene の絶対パス。
    // WHY アセットと分けるか: シーンの再読込は EntityID も選択も Undo 履歴も作り直す。
    //      「中身を差し替えるだけ」のアセットとは失敗したときの被害が桁違いなので、
    //      未保存判定・Play 判定を通す別経路に置く。
    std::vector<std::string> pendingSceneReloads;
    // 独立VFXEditorの File > Open からホスト側のネイティブダイアログを要求する。
    std::function<void()> requestOpenVFXAssetDialog;
    bool requestScriptReload         = false;  // StatusBar の ↻ ボタン → TickScriptCompile が処理

    // パネルが書き換えた EditorSettings を今すぐ editor_settings.toml へ書き出す要求。
    // WHY: 通常の回収は Shutdown 一括だが、ビルド構成のように「次の起動まで待てない」
    //      設定がある。エディターを強制終了しても直前の編集が残るようにする。
    bool requestEditorSettingsSave   = false;

    // F キーフォーカス: ViewportPanel がセット → main.cpp が DebugCamera に適用してクリア
    bool            requestFocusOnSelected = false;
    math::Vector3   focusTargetPosition    = {};
    // 選択バウンディング球の半径。0 は「バウンズ不明」で従来の固定距離フォーカスになる。
    float           focusTargetRadius      = 0.0f;

    // カメラブックマーク呼び出し: ViewportPanel がセット → EditorApp が Teleport してクリア
    bool             requestTeleportCamera  = false;
    math::Vector3    teleportPosition       = {};
    math::Quaternion teleportRotation       = {};

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

    // エディター専用: 非表示 instanceId → 非表示前の activeSelf 値（シリアライズしない）
    // WHY: runtime の activeSelf フラグと切り離し、参照メッシュ等をエディタ上だけ隠せるようにする。
    //      value は非表示前の activeSelf: Play/Save 時に一時解除してこの値を復元する。
    std::unordered_map<std::string, bool> editorHiddenGuids;

    // Inspector の Transform ヘッダーメニュー (Copy / Paste) と
    // AI の transform.copy / transform.paste が共有するクリップボード。
    // WHY: 以前は InspectorCore.cpp の関数内 static だったため、AI 側から
    //      「人がコピーした Transform を貼る」ことも、その逆もできなかった。
    //      別々に持つと、同じ操作名なのに中身が違うという最も追いにくい食い違いになる。
    struct TransformClipboard {
        bool             has = false;
        math::Vector3    position{};
        math::Quaternion rotation{};
        math::Vector3    scale{ 1.0f, 1.0f, 1.0f };
    };
    TransformClipboard transformClipboard;

    // Util (非所有)
    HotkeyManager*      hotkeyManager = nullptr;
    UndoStack*          undoStack   = nullptr;
    // エディター操作の登録簿 (EditorApp 所有)。メニュー・ホットキー・パレットに加え、
    // AI Command Bus の editor.op.list / editor.op.invoke がここを読む。
    // WHY: AI 専用の実装経路を作らないための唯一の入口。Docs/design/editor-operator-model.md
    OperatorRegistry*   operators   = nullptr;
    // ログ集約シンク (EditorApp 所有)。AI の console.logs クエリが履歴を読む。
    ConsoleSink*        consoleSink = nullptr;
    // AI 連携のライブ状態と制御口。Panel は EditorApp の所有物へ直接依存せず、この窓口だけを使う。
    bool                aiCommandBusEnabled = false;
    bool                aiCommandBusRunning = false;
    std::function<bool()> startAiCommandBus;
    std::function<void()> stopAiCommandBus;
    // Inspector から専用 Sprite Editor を開く。パネル所有権を公開せず要求だけを渡す。
    std::function<void(const std::string&)> openSpriteEditor;
    // .behaviortree を BehaviorTreePanel へ渡す (同じくパネル実体は公開しない)。
    std::function<void(const std::string&)> openBehaviorTree;
    // .synth を SfxEditorPanel へ渡す。
    std::function<void(const std::string&)> openSfxEditor;

    // SFX Editor が編集中の手続き効果音。
    //
    // WHY パネルではなく context に置くか: sfx.* Operator を人 (パレット / パネル) と
    //     AI の両方が呼ぶ。パネルが実体を抱えて Operator が要求を積む形にすると、
    //     AI が set_param の直後に inspect を呼んだとき、パネルがまだ要求を
    //     消費していないので 1 フレーム古い値が返る。読み書きが同じ場所を指すよう、
    //     編集中の状態そのものをここへ置き、パネルはこれを描く面に徹する。
    std::string      sfxEditorPath;            // 空 = 未保存の下書き
    audio::SynthSpec sfxEditorSpec;
    std::string      sfxEditorPresetName;      // 由来プリセット名 (空 = 手編集)
    bool             sfxEditorDirty = false;
    // BehaviorTreePanel が開いているドキュメント (パネルが毎フレーム公開する)。
    // Operator の poll が「今この操作ができるか」を判定するのに使う。
    std::string behaviorTreeEditorPath;
    // 整列要求 (ワンショット)。パネルが消費して AutoLayout を実行する。
    // WHY 要求経由か: 整列は Undo スタックを通す必要があり、それを持つのはパネル。
    //     外から木の中身だけ書き換えると整列前へ戻せなくなる。
    bool requestBehaviorTreeAutoLayout = false;
    // .animcontroller を AnimationGraphPanel へ渡す。
    // WHY 要求経路を分けるか: 以前 Animation Graph は selectedAssetPath に追従していたため、
    //     Asset Browser で別ファイルをクリックしただけで編集対象が入れ替わり、
    //     未保存の変更が確認なしに消えていた。「開く」を明示的な操作として切り出す。
    std::function<void(const std::string&)> openAnimationGraph;
    PlayModeController* playMode    = nullptr;
    TerrainTool*        terrainTool = nullptr; // EditorApp が所有、ViewportPanel が使用
    WaterTool*          waterTool   = nullptr; // EditorApp が所有、ViewportPanel が使用
    // Inspector セクション折り畳み状態 (EditorSettings ↔ ImGui StateStorage の中継)
    std::vector<std::pair<uint32_t, bool>> inspectorSectionState;
    // 現在の Scene に紐づく Editor 専用メタデータ。Scene 本体には保存しない。
    EditorSceneState editorSceneState;
    // Inspector がカテゴリ別の既存描画関数から Component カードを収集するための一時窓口。
    // 1 フレームだけ設定し、描画順を決定した後に必ず nullptr へ戻す。
    InspectorComponentDrawCollector* inspectorComponentCollector = nullptr;

    // デフォルトインポート設定 (EditorSettings に永続化)
    FbxImportOptions                        defaultImportOptions;

    // Inspector → AssetBrowser: Reimport モーダルを開くリクエスト（empty = なし）
    std::string                             requestOpenImportModal;

    std::function<void()>                   markSceneDirty;
    std::function<void(const std::string&)> requestOpenScene;
    bool                                    requestAssetBrowserRefresh = false;
    bool                                    requestSaveScene = false; // StatusBar の●クリック等から現在シーン保存を要求

    // AI (Command Bus の scene.open / scene.save) 専用のシーン入出力。確認モーダルを挟まない。
    // WHY: requestOpenScene は未保存変更があるとモーダル確認を開く。人の操作ならそれが正しいが、
    //      AI 要求では「応答は返ったのに、その後エディタが人のクリック待ちで止まる」ことになり、
    //      次の要求も処理されない (バスはメインスレッドで drain するため)。
    //      未保存の確認は AI 側の引数 (discardUnsaved) で成立させ、ここには確認なしの実体だけを置く。
    //      save の path が空なら現在のシーンパスへ上書き保存する。
    std::function<bool(const std::string&)> openScenePathImmediate;
    std::function<bool(const std::string&)> saveScenePathImmediate;
    float                                   assetBrowserIconSize       = 84.0f;
    float                                   assetBrowserTreeWidth      = 180.0f; // 左フォルダツリーの幅 (永続化)
    float                                   editorUiScale              = 1.0f;   // UI 全体スケール (永続化)
    std::vector<std::string>                assetBrowserBookmarks;

    // カメラブックマーク (最大 9 件、Shift+1~9 で保存・1~9 で呼び出し)
    struct CameraBookmark {
        math::Vector3    position;
        math::Quaternion rotation;
        bool             valid = false;
    };
    std::array<CameraBookmark, 9>           cameraBookmarks;

    // Inspector → AssetBrowser: 編集中 .mat のサムネイル即時更新。
    // WHY: AssetBrowser のマテリアルサムネイルは .mat のファイル更新時刻でしか再生成されないため、
    //      「値をいじってもアイコンが変わらない (保存するまで古いまま)」という状態だった。
    //      Inspector 側で値が変わるたびにリビジョンを進め、AssetBrowser はそれを見て
    //      ディスクではなく AssetManager 上の (未保存の) MaterialAsset からサムネイルを描き直す。
    //      キーは NormalizeAssetPath 済みのプロジェクト相対パス。
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
};

} // namespace fbzz::editor
