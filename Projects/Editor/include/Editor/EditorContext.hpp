/// @file    EditorContext.hpp
/// @brief   パネル間で共有するエディター状態。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Engine/Scene/Components/Animation/AnimatorComponent.hpp>
#include <Engine/Scene/Components/Camera/CameraComponent.hpp>
#include <Engine/Scene/Components/Physics/VolumeComponent.hpp>
#include <Engine/Scene/Components/Water/WaterComponent.hpp>
#include <Engine/Scene/Fields/FlowField.hpp>
#include <Editor/Play/StandalonePlaySession.hpp>
#include <Editor/GraphEditor/GraphLayout.hpp>
#include <Editor/Import/Fbx/FbxImportTool.hpp>
#include <Editor/Scene/Editing/EditorSceneState.hpp>
#include <Editor/Op/HotkeyScope.hpp>
#include <Editor/Diagnostics/Rendering/RenderCaptureFrame.hpp>
#include <Editor/Script/Authoring/ScriptRequirementReview.hpp>
#include <Editor/Diagnostics/Rendering/SpatialDebugVisualization.hpp>
#include <Engine/Audio/Synthesis/SynthSpec.hpp>
#include <Engine/Project/ProjectSettings.hpp>
#include <Graphics/Renderer/View/Camera.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/LinearAlgebra/Quaternion.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
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
namespace fbzz::editor   { class UndoStack; class PlayModeController; class TerrainTool; class HotkeyManager; class BuildConsole; class ConsoleSink; class OperatorRegistry; class MemoryLeakDiff; class FluidBakeService; class ManagedScriptEditor; class PackageBuildService; }

namespace fbzz::editor {

struct InspectorComponentDrawCollector;
struct FiberPaintTransaction;

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

        Type            type = Type::None; ///< 選択しているグラフ要素の種別。選択がなければ None。
        scene::EntityID entityId = scene::EntityID::INVALID; ///< シーン上で編集中の Animator を持つ EntityID。アセット編集時は INVALID。
        std::string     assetPath; ///< アセットとして編集中の Controller のパス。シーン上での編集時は空。
        /// @note  どのレイヤーのグラフに対する選択か。空 = Base Layer。
        /// @note stateIndex はグラフごとの添字なので、レイヤー名が無いと Inspector が別レイヤーのステートを編集してしまう。
        std::string     layerName; ///< stateIndex などが参照するアニメーションレイヤーの名前。空なら Base Layer。
        int             stateIndex = -1; ///< 選択中のレイヤー内のステート添字。使わない選択種別では -1。
        int             transitionIndex = -1; ///< 選択したステートまたは Any State の遷移配列の添字。未選択なら -1。
        /// @note  BlendTree 内で選択中の Motion。stateIndex が親 State、motionIndex が Motion の添字。
        /// @note Motion は State とは別の入れ子データなので、State と同じ index だけでは編集対象を特定できない。
        int             motionIndex = -1; ///< stateIndex が指すステートの BlendTree 内の Motion 添字。未選択なら -1。
        /// @note  グラフ上で掴んでいるステートの数 (矩形選択・Ctrl クリック)。
        /// @note Inspector が編集するのは常にプライマリ 1 件だが Delete は選択全部に効くため、件数を UI 上で明示する。
        int             selectedCount = 0; ///< グラフでまとめて選択しているステート数。Inspector はプライマリだけを表示する。

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
    scene::Scene*     activeScene  = nullptr; ///< パネルとギズモが参照する表示中のシーン。Play の遷移時は SceneManager が所有するシーンを借りる。
    /// @note  編集対象のシーン (ドキュメント本体)。currentScenePath が指すファイルの中身であり、Play 中もシーン遷移の影響を受けない。
    /// @note activeScene で保存・読込すると、Play 中の遷移後に Ctrl+S を押した際「遷移後シーンの中身」を「遷移前シーンのファイル」へ書き込み、開いていたシーンが潰れる。
    scene::Scene*     editScene    = nullptr; ///< currentScenePath の保存・読込対象となる借用シーン。Play 中の遷移から独立する。
    /// @note  Play 中に遷移した実行シーンの登録名 (遷移していなければ空)。表示専用。
    std::string       playSceneName; ///< Play 中に遷移したランタイムシーンの登録名。未遷移なら空。
    /// @note  編集中Sceneを駆動するProjectRuntime (EditorApp所有)。Physics World等の実行時系へアクセスする。
    /// @note AI (EditorBusDispatcher) の raycast/overlap 等はこの runtime の Physics World へ問い合わせる。
    scene::ProjectRuntime* runtime = nullptr; ///< EditorApp が所有する ProjectRuntime。シーンの更新や Physics World へのアクセスに使う。
    /// @note  AI の viewport.capture / viewport.semantic が RT を読み出す間、表示状態に関わらず
    /// @note  Scene View / Game View を描き続けるための猶予フレーム。
    /// @note  EditorApp は画面に出ているビューポートだけを描くので、隠れている側をキャプチャすると
    /// @note  描画が止まった RT の古い内容を読んでしまう。
    std::uint64_t     aiViewportRenderUntilFrame = 0; ///< AI の要求に応じて非表示のビューも描画し続けるフレーム番号の上限。
    std::uint64_t     captureRevision = 0; ///< シーン変更後に以前のビュー画像を破棄するためのリビジョン。
    std::array<std::uint64_t, 2> semanticCaptureUntilFrame{}; ///< 意味情報を取り込むフレーム番号の上限。添字 0 は Scene View、1 は Game View。
    std::array<RenderCaptureFrame, 2> viewportCaptureFrames; ///< 最後に公開されたキャプチャ情報。添字 0 は Scene View、1 は Game View。
    renderer::Camera* editorCamera = nullptr; ///< EditorApp の DebugCamera が持つ Scene View 用カメラを借りる。
    /// @note  DebugCamera が持つオービット中心と注視距離のミラー (EditorApp が毎フレーム更新)。
    /// @note ナビゲーションギズモは「ピボットを動かさず視点だけ回す」ため、カメラ位置から復元できない DebugCamera 内部の中心・距離を必要とする。
    math::Vector3     editorCameraPivot         = {}; ///< Scene View のカメラを周回させる中心点。ワールド座標。
    float             editorCameraFocusDistance = 10.0f; ///< DebugCamera が保持する注視距離 [m]。EditorApp が毎フレーム更新する。
    /// @note  ピボットからカメラを引いている実距離。正投影では焦点距離と一致しない
    /// @note  (DebugCamera::ViewDistance)。視点だけ回す計算はこちらを使うこと。
    float             editorCameraViewDistance  = 10.0f; ///< 視点を回す際に使うピボットからカメラまでの実距離 [m]。
    renderer::IRenderer* renderer = nullptr; ///< EditorApp が借りているフレーム描画インターフェース。
    renderer::IImGuiRenderer* imguiRenderer = nullptr; ///< EditorApp が借りている ImGui 描画インターフェース。
    renderer::ResourceManager* resources = nullptr; ///< EditorApp が借りている描画とアセットのリソース管理器。
    /// @note  .fluid の焼き・プレビューの窓口 (EditorApp が所有)。
    FluidBakeService* fluidBake = nullptr; ///< EditorApp が所有する流体ベイクとプレビューのサービス。
    /// @note モデル変換ワーカーの完了前にプロジェクト索引やEditorを破棄しないための寿命境界。
    std::function<bool()> modelImportBusy; ///< モデル変換ワーカーが動作中か、必要に応じて調べるコールバック。
    std::function<void()> waitForModelImports; ///< プロジェクトの破棄前にモデル変換ワーカーの完了を待つコールバック。
    core::MemorySystem* memorySystem = nullptr; ///< メモリ診断パネルが読む、エンジン所有の MemorySystem。
    /// @note  Renderer リソースのリーク差分。EditorApp が所有し、Analysis パネルが読む。
    MemoryLeakDiff*     memoryLeakDiff = nullptr; ///< Analysis パネルに描画リソースのリーク差分を渡す EditorApp 所有のサービス。
    std::string       projectRoot; ///< 現在開いているゲームプロジェクトの絶対パス。アセットと設定の基準になる。
    std::string       projectBuildRoot; ///< .fbzz_proj から解決した CMake ビルドツリー。空なら起動した Editor の配置から検索する。
    std::string       engineRoot; ///< 起動した Editor に対応する SDK またはソースリポジトリ。子の CMake ビルドへ FBZZ_SDK_ROOT として渡す。
    std::string       standaloneTargetName = "SandboxStandalone"; ///< 配布用実行ファイルを生成する CMake ターゲット名。
    std::string       projectTargetName; ///< .fbzz_proj の target_name。GameHub がプロジェクトを識別するときに使う。

    /// @note  C# スクリプトの作成・一覧に使うソースパス (EditorApp が設定)
    std::string       scriptsSourceDir; ///< スクリプト作成と列挙の基準になる Assets/Scripts の絶対パス。
    std::string       hlslSourceDir; ///< シェーダー作成と列挙の基準になる Assets/shaders の絶対パス。
    std::string       currentScenePath; ///< 現在の編集シーンの保存先。未保存の新規シーンなら空。
    std::string       selectedAssetPath; ///< Asset Browser で選択中のファイルの絶対パス。選択がなければ空。
    bool              sceneDirty = false; ///< 編集シーンに保存されていない変更があるか。

    /// @note  OSエクスプローラーからの外部ドラッグ&ドロップ受け渡しチャンネル (プラットフォーム層 → AssetBrowser)。
    /// @note AssetBrowserImport がドロップ確定を遅延処理するため、ドラッグ中のライブ位置と確定ファイル群をフレーム跨ぎで共有する。空のままなら AssetBrowser は何もしない。
    bool                     externalDragActive = false; ///< OS からファイルをドラッグ中で、ドロップ領域を強調表示するか。
    float                    externalDragX = 0.0f; ///< OS からドラッグ中のカーソルの X 座標。画面座標 [px]。
    float                    externalDragY = 0.0f; ///< OS からドラッグ中のカーソルの Y 座標。画面座標 [px]。
    std::vector<std::string> droppedExternalFiles; ///< ドロップで確定したファイルの絶対パス。Asset Browser が処理するまで保持する。
    float                    droppedExternalFilesX = 0.0f; ///< OS のファイルドロップを確定した位置の X 座標。画面座標 [px]。
    float                    droppedExternalFilesY = 0.0f; ///< OS のファイルドロップを確定した位置の Y 座標。画面座標 [px]。
    /// @note  Animation Graph Editor のノード配置。Editor 専用のため SceneSerializer には渡さない。
    /// @note AnimatorComponent はランタイム構造体なので、キャンバス座標は GO の instanceId または Controller アセットパスをキーにした別データとして保持する。
    std::unordered_map<std::string, GraphLayout> graphLayouts; ///< Animation Graph のエディター用配置。キーは Animator の instanceId または Controller アセットパス。
    AnimationGraphSelection animationGraphSelection; ///< Animation Graph、Inspector、プレビューが共有するグラフの選択状態。
    /// @note  Inspector の Name 欄でステートを改名したことを Animation Graph パネルへ伝える 1 ショット。
    /// @note  パネルの選択は名前が権威なので、伝えないと改名した瞬間に選択が外れる。
    std::string animationGraphRenamedFrom; ///< Inspector が改名したステートの旧名。Graph が選択を更新したら消費する。
    std::string animationGraphRenamedTo; ///< Inspector が改名したステートの新名。
    /// @note  Inspector の Layers 欄でレイヤー名を改名したことを Animation Graph パネルへ伝える 1 ショット。
    /// @note Graph はレイヤー名を識別子として保持するため、データ側だけ改名すると旧名が見つからず Base Layer へ戻り Mask 欄も消えたように見える。
    std::string animationGraphLayerRenamedFrom; ///< Inspector が改名したアニメーションレイヤーの旧名。Graph が選択の保持に使う。
    std::string animationGraphLayerRenamedTo; ///< Inspector が改名したアニメーションレイヤーの新名。
    /// @note  Inspector で追加・改名したレイヤーを Graph 側で表示する要求。空なら通常の選択を維持する。
    std::string animationGraphLayerFocus; ///< Inspector から Graph へ表示を依頼したレイヤー名。空なら現在の表示を維持する。
    /// @note  Inspector で削除したレイヤー。Graph 側の古いステート選択を Base Layer に誤適用させない。
    std::string animationGraphLayerRemoved; ///< Inspector が消したレイヤー名。Graph が失効したステート選択を取り除く。
    /// @note  Animation Graph と Inspector が共有する Controller アセット編集モデル。
    std::shared_ptr<scene::AnimatorComponent> animationControllerEditor; ///< Animation Graph と Inspector が共有所有する Controller アセットの編集中のモデル。
    std::string animationControllerEditorPath; ///< 編集中の Controller アセットを識別するパス。開いていなければ空。
    bool animationControllerDirty = false; ///< 編集中の Controller に、ディスクへ保存していない変更があるか。

    /// @note  選択状態 (Multi-select 対応)。直接書き換えず Editor/Scene/Editing/Selection.hpp の SelectEntity / SelectEntities 等を使う。
    /// @note 選択にはアセット選択との排他と Hierarchy への反映が付いて回るため、直接代入すると後始末をした面としない面が混在する。
    std::vector<scene::EntityID> selectedEntities; ///< 現在選択中の EntityID。先頭がプライマリ選択。変更は Selection.hpp の操作で行う。

    /// @note  Hierarchy 以外の面 (Scene View / 検索 / Map / AI) が選んだ対象を、ツリー上で
    /// @note  見えるようにする要求。SceneHierarchyPanel が畳まれた祖先を開いてスクロールする。
    /// @note  親が畳まれていると Viewport でクリックした子は Hierarchy に 1 行も現れない。
    scene::EntityID hierarchyRevealTarget = scene::EntityID::INVALID; ///< Hierarchy で祖先を開きスクロールして見せる EntityID。要求がなければ INVALID。
    scene::EntityID PrimarySelected() const
    {
        return selectedEntities.empty() ? scene::EntityID{} : selectedEntities.front();
    }

    bool HasActiveScene() const { return activeScene != nullptr; }

    scene::GameObject* GetSelectedGO() const
    {
        auto sel = PrimarySelected();
        if (!sel.IsValid() || !activeScene) {
            return nullptr;
        }
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

        Type              type = Type::None; ///< Inspector の表示対象の種別。選択がなければ None。
        scene::GameObject* gameObject = nullptr; ///< 表示対象の GameObject を activeScene から借りる。アセットや複数選択の場合は null。
        std::string        assetPath; ///< アセットを表示するときの対象パス。
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

    /// @note  カメラ操作設定 (EditorSettings からロードされ、main.cpp が DebugCamera へ適用する)。
    /// @note DebugCamera は Engine 層に属しパネルから直接参照できないため、EditorContext を仲介する。
    float cameraSpeed         = 5.0f; ///< Scene View のデバッグカメラの移動速度設定。
    float cameraSensitivity   = 0.15f; ///< Scene View のカメラをマウスで回す感度。

    /// @note  ビューポート
    bool  viewportFocused  = false; ///< Scene View がキーボード操作を受けるフォーカスを持つか。
    bool  sceneViewportHovered = false; ///< Scene View にマウスが乗っているか。ホイール操作の判定に使う。
    /// @note 空は直接選択、GUIDは所有者の点編集。点の順序や属性は利用側Componentが持つ。
    std::string controlPointOwnerGuid; ///< 制御点を編集中の所有オブジェクトの GUID。空なら通常の直接選択。
    /// @note camera入力は直前の描画frameの占有だけを読む。非表示panelの古い占有を残さない。
    int controlPointMouseClaimFrame = -1000000; ///< 制御点操作がマウスを占有した ImGui フレーム番号。
    /// @note  今フォーカスされているパネルの «面»。キーの意味はここで決まる。書くのは IPanel::OnRender だけ (GetHotkeyScope() で名乗る)。
    /// @note 毎フレーム None へ落としてから描画で立て直すため、参照側は 1 フレーム遅れで見る。
    HotkeyScope focusedPanelScope = HotkeyScope::None; ///< ホットキーを解釈するパネルの操作範囲。フォーカスがなければ None。

    /// @note  その面に今フォーカスがあるか。パネル内のキー処理もここを通す。
    [[nodiscard]] bool PanelScopeFocused(HotkeyScope scope) const
    {
        return HasScope(focusedPanelScope, scope);
    }
    /// @note  F2 リネーム要求 (HotkeyManager → Hierarchy)。
    /// @note リネームの編集バッファとフォーカス制御はパネル内部にあるため、キーの割り当てだけ HotkeyManager に集約し「何をするか」はパネルに残す 1 ショット。
    bool  requestRenameSelected = false; ///< 選択中の名前を変更する一回限りの要求。Hierarchy が消費する。
    float viewportWidth   = 1280.0f; ///< Scene View の描画領域の幅 [px]。
    float viewportHeight  = 720.0f; ///< Scene View の描画領域の高さ [px]。
    bool  gameViewportFocused = false; ///< Game View が現在のフォーカスを持つか。
    /// @note 明示的な一時最大化。物理入力の所有権と自動Playの表示設定から独立し、保存しない。
    enum class GameViewportLayout { AUTOMATIC, MAXIMIZED, NORMAL };
    GameViewportLayout gameViewportLayout = GameViewportLayout::AUTOMATIC; ///< Game View の自動配置または手動の最大化状態を選ぶ一時設定。
    bool  gameViewportMaximized = false; ///< 現在の Game View レイアウトが最大化状態か。
    float gameViewportOriginX = 0.0f; ///< Game View の表示矩形左端の画面 X 座標 [px]。
    float gameViewportOriginY = 0.0f; ///< Game View の表示矩形上端の画面 Y 座標 [px]。
    float gameViewportWidth   = 1280.0f; ///< Game View の表示矩形の幅 [px]。
    float gameViewportHeight  = 720.0f; ///< Game View の表示矩形の高さ [px]。
    /// @note  Game View がこのフレームに実際に描かれ、上の矩形が «今の画面» を指しているか。
    /// @note 寸法と原点はパネルが描いたときにしか書かれず閉じている間は前回値が残るため、カーソル拘束のような «外れると操作不能» な用途は古い矩形を掴んではいけない。
    bool  gameViewportRectValid = false; ///< このフレームの Game View の表示矩形を、入力やカーソルの拘束で使えるか。
    bool  requestGameViewportFocus = false; ///< Play 開始時の一回限りのフォーカス要求。ViewportPanel が消費する。
    /// @note  Escape で解放したカーソルをゲームへ返す one-shot 要求。Game View をクリックしたときと
    /// @note  オーバーレイの「Click to capture」から立ち、EditorApp が消費する。
    bool  requestGameCursorCapture = false; ///< Game View がゲームへカーソルを返す一回限りの要求。EditorApp が消費する。
    /// @note  Play 開始時の «ウィンドウの並べ方»。カーソルの扱いはここでは決めない
    /// @note  (スクリプトの cursor.Push と playCursorOverride が決める)。
    enum class PlayFocusMode {
        Focused,
        Maximized,
        Unfocused
    };
    PlayExecutionMode playExecutionMode = PlayExecutionMode::IN_EDITOR; ///< Play 操作が Editor 内実行と別プロセス実行のどちらを選ぶか。
    StandalonePlaySession* standalonePlay = nullptr; ///< EditorApp が所有する別プロセスの Play セッション。
    PlayFocusMode playFocusMode = PlayFocusMode::Maximized; ///< Play 開始時に Game View をフォーカスまたは最大化するか。
    /// @note  Play 中のカーソル要求を、スクリプトを書き換えずに一時的に外す口。
    /// @note Confined + 非表示のゲームを Editor で触るとき、確認のたびにスクリプトへ手を入れて再ビルドさせたくない。
    enum class PlayCursorOverride {
        Game,      /// @note ゲーム (cursor.Push / SetLockMode) の要求どおりにする
        Free       /// @note 常に None + 表示 (デバッグ用)
    };
    PlayCursorOverride playCursorOverride = PlayCursorOverride::Game; ///< Play 中にゲームのカーソル制御を使うか、Editor で一時的に解放するか。
    bool  mapEditingMode = false; ///< Scene View を中心とするマップ制作向けワークスペースを有効にしているか。
    bool  requestMapEditingModeToggle = false; ///< ツールバーやメニューからのワークスペース切り替え要求。
    /// @note  Map Editing Mode の選択中ツール。
    /// @note MapEditorPanel と Scene Viewport のオーバーレイツールバー / 数字キーが同じ選択状態を共有するため、パネルのローカル変数ではなく Context に置く。
    enum class MapTool {
        TerrainSculpt,
        TerrainPaint,
        Grid,
        TerrainHole, /// @note 保存済み int 値を変えないため末尾に置く
    };
    MapTool mapActiveTool = MapTool::TerrainSculpt; ///< マップ制作で現在選択しているツール。
    bool  mapHierarchyFilter = true; ///< Map Mode 中の Hierarchy を地形・グリッド・水のオブジェクトだけに絞るか。
    bool  mapInspectorFilter = true; ///< Map Mode 中の Inspector をマップ制作用コンポーネントに絞るか。
    /// @note  システムが実行時に生成した GameObject (GameObject::runtimeGenerated) を Hierarchy へ出すか。
    /// @note VFX Graph は 1 エフェクトにつきノード数ぶんの GameObject を作るため、既定は非表示にして親 1 行へ畳み、デバッグ時だけ開く。
    bool  showGeneratedObjects = false; ///< VFX 等が実行時に生成した GameObject を Hierarchy に表示するか。
    bool  uiViewportFocused = false; ///< UI View が現在のフォーカスを持つか。
    float uiViewportOriginX = 0.0f; ///< UI View の表示矩形左端の画面 X 座標 [px]。
    float uiViewportOriginY = 0.0f; ///< UI View の表示矩形上端の画面 Y 座標 [px]。
    float uiViewportWidth = 1280.0f; ///< UI View の表示矩形の幅 [px]。
    float uiViewportHeight = 720.0f; ///< UI View の表示矩形の高さ [px]。
    /// @note  UIViewport が編集対象にする ScreenSpace Canvas。
    /// @note 複数 Canvas があると「最初の Canvas」を暗黙選択するだけでは編集対象が不安定になる。
    scene::EntityID activeUICanvas = scene::EntityID::INVALID; ///< UI View で編集する Screen Space Canvas の EntityID。未選択なら INVALID。

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
    GameViewportAspect gameViewportAspect = GameViewportAspect::Free; ///< Game View の表示領域やレンダーサイズを決めるプリセット。

    /// @note  ギズモ
    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };
    /// @note  ギズモを置く基準点 (Unity の Pivot / Center トグル相当)。
    /// @note  Pivot はプライマリ選択の原点。複数選択で回すと「たまたま最初に選んだもの」が軸になる。
    /// @note  Center は選択全体のバウンズ中心を軸にする (足元原点のキャラなど単一選択でも効く)。
    enum class GizmoPivot { Pivot, Center };
    GizmoMode  gizmoMode  = GizmoMode::Translate; ///< Scene View ギズモで現在編集する変換の種類。
    GizmoSpace gizmoSpace = GizmoSpace::World; ///< Scene View ギズモで使うワールド／ローカル座標系。
    GizmoPivot gizmoPivot = GizmoPivot::Pivot; ///< ギズモの位置をプライマリの原点と選択全体の中心のどちらに置くか。

    /// @note  グリッド・スナップ
    bool  showGrid     = true; ///< Scene View のワールドグリッドを表示するか。
    float gridSize     = 1.0f; ///< 隣接するワールドグリッド線の基本間隔 [m]。
    bool  snapEnabled  = false; ///< Scene View の変換ギズモで刻み値へのスナップを有効にするか。
    float snapPos      = 1.0f; ///< 位置スナップの刻み幅 [m]。
    float snapRot      = 15.0f; ///< 回転スナップの刻み幅 [度]。
    float snapScale    = 0.25f; ///< スケールスナップの刻み幅。元の寸法に対する倍率。

    /// @note  頂点スナップ / 面スナップ (ViewportPanel が押下状態を反映する)。
    /// @note V 押下中は選択メッシュの頂点を他メッシュの頂点へ吸着し、Ctrl+Shift ドラッグ中はカーソル下のサーフェスへ接地させる (Unity 互換)。
    bool  vertexSnapActive  = false; ///< キーを押している間、ドラッグ中の選択メッシュを頂点へ吸着させるか。
    bool  surfaceSnapActive = false; ///< キーを押している間、ドラッグ中の選択をカーソル下の面へ接地させるか。
    /// @note  面スナップ時に、接地面の法線へオブジェクトの上方向を合わせるか。
    bool  surfaceSnapAlignToNormal = false; ///< 面スナップ時にオブジェクトの上方向を面の法線へ合わせるか。

    /// @note  プロジェクト設定
    fbzz::ProjectSettings projectSettings; ///< 現在編集中のプロジェクト設定。自動保存とランタイムの設定反映に使う。

    /// @note  表示オプション (エディター固有)
    /// @note  エミッターの発生形状と初速のワイヤー表示。
    bool showVFXGizmos = false; ///< Scene View に VFX の発生形状と初速を表示するか。
    /// @note Scene View の布固定点ブラシ。シーンに保存しないエディター状態。
    bool clothPinPainting = false; ///< Scene View の布固定点ペイントを有効にする一時状態。
    bool fiberPainting = false; ///< Scene View のファイバーペイントを有効にする一時状態。
    /// @note Viewport 所有の確定待ちストローク。Operator 呼び出し中だけ非 null。
    FiberPaintTransaction* fiberPaintPending = nullptr; ///< Viewport が所有する確定待ちのストローク。Operator 呼び出し中だけ借りる。
    /// @note  FlowField の範囲・減衰・流れの向き。
    bool showFlowFields = false; ///< Scene View に FlowField の範囲・減衰・向きを表示するか。
    /// @note  格子点で評価した実効流速の矢印。範囲は選択中の FlowField、無ければカメラ前方。
    bool showFlowSamples = false; ///< Scene View に格子点で評価した実効流速の矢印を表示するか。
    /// @note  VolumeComponent のトリガー形状と効果。
    bool showPhysicsVolumes = false; ///< Scene View に物理 Volume のトリガー形状と効果を表示するか。
    /// @note  全 WaterComponent の水流・渦・浮力の届く深さ。選択中の水面ギズモと違い常時出す。
    bool showWaterFlow = false; ///< Scene View に WaterComponent の水流、渦と浮力の有効範囲を表示するか。
    /// @note  ラグドールの剛体・可動域・接触点。飽和した関節が赤く出る。
    /// @note  常時出すと骨の絵が線で埋まるので、可動域とサーボを詰めるときだけ点ける。
    bool showRagdoll   = false; ///< Scene View にラグドールの剛体、関節可動域と接触点を表示するか。
    bool showLightRange  = true; ///< Scene View に光源の有効範囲を表示するか。
    bool showSkeleton    = false; ///< Scene View にアニメーションのスケルトンを表示するか。
    /// @name Scene View の診断オーバーレイ (RenderSettings の同名フラグへ毎フレーム複写)
    /// @{
    /// @note  true なら選択中のスケルトンだけ描く。
    bool skeletonSelectedOnly = true; ///< スケルトン表示を選択中のオブジェクトに絞るか。
    /// @note  スクリプトの OnDrawGizmos / debug.Draw*。Game View と配布ビルドでは常に出ない。
    bool showScriptGizmos  = true; ///< Scene View に C# スクリプトから依頼されたギズモを表示するか。
    bool showConstraints   = false; ///< Scene View に制約の診断オーバーレイを表示するか。
    bool showRigidBodies   = false; ///< Scene View に剛体の診断オーバーレイを表示するか。
    bool showIK            = false; ///< Scene View に IK の診断オーバーレイを表示するか。
    bool showSpringBones   = false; ///< Scene View に揺れ物のボーンの診断オーバーレイを表示するか。
    bool showAttachments   = false; ///< Scene View にアタッチメントの診断オーバーレイを表示するか。
    bool showVFXPaths      = false; ///< Scene View に VFX の経路の診断オーバーレイを表示するか。
    bool showTerrainBounds = false; ///< Scene View に Terrain の境界の診断オーバーレイを表示するか。
    bool showLODBounds     = false; ///< Scene View に LOD の境界の診断オーバーレイを表示するか。
    /// @brief Scene View diagnostics are opt-in and never persisted in project render settings.
    SpatialDebugVisualization spatialDebugVisualization; ///< Scene View 専用の空間診断設定。プロジェクトの描画設定には保存しない。
    /// @}
    /// @brief Scene View のコンポーネントアイコン全体の表示。false ならアイコンのピッキングも止まる。
    bool showSceneIcons  = true; ///< Scene View のコンポーネントアイコンを表示し、クリック選択を許可するか。
    /// @brief 非表示にしたアイコン種別の保存キー。
    /// @see SceneIconTypeKey
    std::vector<std::string> hiddenSceneIcons; ///< Scene View で非表示にしたアイコン種別の永続化キー。
    /// @note  Scene View で CPU ソフトウェアオクルージョンカリングを効かせるか。
    /// @note  Scene View はデバッグカメラで描くので CameraComponent の設定が効かない。
    /// @note  既定は無効 (編集中は見えているものが見えることを優先)。効きは Stats の Occlusion 行。
    bool sceneViewOcclusionCulling = false; ///< Scene View のデバッグカメラで CPU ソフトウェアオクルージョンカリングを使うか。
    bool showStats       = true; ///< Scene View と Game View に描画統計を表示するか。
    bool showTerrainTool = false; ///< Terrain Tool のウィンドウを表示するか。
    bool hotReloadEnabled = true; ///< ディスク上の変更を実行中の Editor へ自動で取り込むか。
    /// @brief ホットリロードの完了・失敗を音で知らせるか。@see EditorSettings::hotReloadSound
    bool hotReloadSound   = true; ///< ホットリロードの完了と失敗を効果音で知らせるか。

    /// @note  スクリプト DLL / HLSL ホットリロード状態 (StatusBar が表示する)
    enum class HotReloadState { Idle, Compiling, Reloading, Done, Failed };
    HotReloadState hotReloadState   = HotReloadState::Idle; ///< StatusBar に表示するホットリロードの進行または終端状態。
    std::string    hotReloadMessage; ///< StatusBar に表示するホットリロードの現在の診断と進捗の文言。
    /// @note シェーダー再コンパイルの進捗を 0.0〜1.0 で公開する。
    float          hotReloadProgress = -1.0f; ///< シェーダー再コンパイルの進行割合 [0,1]。不明なら負。
    float          hotReloadDoneTimer = 0.0f; ///< ホットリロード結果を StatusBar から消すまでの残り時間 [s]。
    /// @brief 変更検知 (Compiling へ入った瞬間) の ImGui::GetTime() [s]。経過秒の表示に使う。
    double         hotReloadStartTime  = 0.0; ///< コンパイルを開始した ImGui の時刻 [s]。表示用の経過時間の基準。
    /// @brief Done / Failed へ入った瞬間の ImGui::GetTime() [s]。確定後は経過秒をここで止める。
    double         hotReloadFinishTime = 0.0; ///< ホットリロード結果が確定した ImGui の時刻 [s]。
    bool           managedBuildBusy = false; ///< C# ビルドまたは適用中で、シーンの変更や Play 開始を止める必要があるか。

    /// @note  ビルドコンソール: ゲーム実行ファイル / HLSL コンパイルの出力・診断・履歴の唯一の情報源。
    /// @note EditorApp が所有し、Build Output パネル・StatusBar・通知バーが読み取る。
    BuildConsole*  buildConsole = nullptr; ///< EditorApp 所有のビルド出力と診断の集約先。
    /// @note EditorApp が所有する C# ビルドサービス。C++ の BuildConsole と要求 ID を共有しない。
    ManagedScriptEditor* managedScripts = nullptr; ///< EditorApp 所有の C# ビルドと成果物公開のサービス。
    /// @note EditorApp所有。描画しないAI/batchも同じ配布パイプラインを参照する。
    PackageBuildService* packageBuild = nullptr; ///< EditorApp 所有の配布ビルドサービス。非表示の処理経路も同じ状態を使う。
    /// @note パネル外のビルド要求も配布ビルドと排他にする。
    std::function<bool()> packageOtherBuildBusy; ///< 配布ビルドと競合する別のビルドが実行中か問い合わせるコールバック。
    bool           requestOpenBuildOutput  = false; ///< Build Output パネルを開く一回限りの要求。
    bool           requestFocusBuildError  = false; ///< Build Output を開き、最初のエラーまで移動する一回限りの要求。

    /// @note  パネル間リクエスト (one-shot フラグ: 発行側が true にセット → 受信側が処理後 false にリセット)
    bool requestOpenProjectSettings  = false; ///< Project Settings パネルを開く一回限りの要求。
    bool requestOpenBuildSettings    = false; ///< Build Settings パネルを開く一回限りの要求。
    bool requestOpenAnalysis         = false; ///< Analysis パネルへ移動する一回限りの要求。
    bool requestOpenAnimationGraph   = false; ///< 選んだ Controller を Animation Graph パネルで開く一回限りの要求。
    bool requestOpenBehaviorTree     = false; ///< 選んだ .behaviortree を Behavior Tree パネルで開く一回限りの要求。
    bool requestOpenSfxEditor        = false; ///< 選んだ .synth を SFX Editor で開く一回限りの要求。
    bool requestOpenSequence         = false; ///< 選んだ .sequence を Sequence パネルで開く一回限りの要求。
    /// @note  .fluid Inspector の «Bake in 3D» → Volume Flipbook Baker を開き、この .fluid を Fluid ソースにする。
    /// @note  one-shot。消すのはパネル (要求を読んだとき)。
    std::string requestVolumeFlipbookFluid; ///< Volume Flipbook Baker に読み込ませる .fluid のパス。空なら要求なし。
    /// @note  .fluid ダブルクリック / Inspector の «Open in Fluid Editor» → Fluid Editor でこの .fluid を開く (実パス)。
    /// @note  one-shot。消すのは Fluid Editor パネル (要求を読んだとき)。
    std::string requestOpenFluidEditor; ///< Fluid Editor で開く .fluid の実パス。空なら要求なし。

    /// @note  Inspector 等のアセット参照欄 → AssetBrowser: 参照先を一覧上で選択させる要求 (one-shot)。
    /// @note パスは絶対 / Assets 相対どちらでもよく AssetBrowser 側が解決する。空文字 = 要求なし。
    std::string requestRevealAssetPath; ///< Asset Browser で見せる項目のパス。絶対パスまたは Assets 相対で、空なら要求なし。
    /// @note  true = 一覧で選択するだけでなく、Inspector の表示対象もそのアセットへ移す (ダブルクリック)。
    bool        requestRevealAssetSelect = false; ///< 表示要求を処理した Asset Browser が選択と Inspector の対象も移すか。

    /// @name Prefab 編集モード
    /// @{
    /// @note  編集中のシーンを一時退避して .prefab だけを開き、閉じるときに戻す。
    /// @note  「インスタンスを選んで Apply」だけだと、シーンに 1 個も置いていないものを直せない。
    std::string requestOpenPrefabEdit; ///< Prefab 編集モードで開く .prefab の Assets 相対パス。空なら要求なし。
    bool     requestSavePrefabEdit  = false; ///< 現在の Prefab 編集をディスクへ保存する一回限りの要求。
    bool     requestClosePrefabEdit = false; ///< 現在の Prefab を保存せずに閉じる一回限りの要求。
    std::string prefabEditPath; ///< 編集中の Prefab の Assets 相対パス。通常のシーン編集時は空。
    bool     prefabEditDirty = false; ///< 編集中の Prefab に保存されていない変更があるか。
    [[nodiscard]] bool InPrefabEditMode() const { return !prefabEditPath.empty(); }

    /// @note  ディスク上で書き換わった .prefab の絶対パス。AssetBrowser のファイル監視が積み、
    /// @note  EditorApp が「シーン内のインスタンスへ反映」して消費する。
    /// @note  ファイルの変化そのものを拾うので、外部エディタからの変更にも追従する。
    std::vector<std::string> pendingPrefabReloads; ///< ディスク上で変わり、シーン中のインスタンスへ再適用する .prefab の絶対パス。

    /// @note  ディスク上で書き換わったアセット (.mat / .anim / .animcontroller / .mask /
    /// @note  .fzdata / .physmat / .synth / .terrain) の絶対パス。積むのは AssetBrowser のファイル監視、
    /// @note  消費するのは EditorApp。
    /// @note  積んでから処理する。監視イベントはパネル描画の途中で届くので、その場でキャッシュを
    /// @note  差し替えると同じフレームのパネル同士が別の版を見ることになる。
    std::vector<std::string> pendingAssetReloads; ///< ディスク上で変わり、EditorApp が次の更新で読み直すアセットの絶対パス。

    /// @note  ディスク上で書き換わった .scene の絶対パス。
    /// @note シーンの再読込は EntityID・選択・Undo 履歴を作り直すため、アセットとは別経路で未保存判定・Play 判定を通す。
    std::vector<std::string> pendingSceneReloads; ///< ディスク上で変わり、編集状態を確認してから読み直す .scene の絶対パス。

    /// @note  パネルが書き換えた EditorSettings を今すぐ editor_settings.toml へ書き出す要求。
    /// @note 通常の回収は Shutdown 一括だが、ビルド構成のように次の起動まで待てない設定があるため、強制終了しても直前の編集が残るようにする。
    bool requestEditorSettingsSave   = false; ///< EditorSettings を通常の終了時保存を待たずに書き出す一回限りの要求。

    /// @brief ProjectSettings.toml の自動保存の状態。書くのは EditorApp、読むのは Project Settings パネル。
    enum class SettingsSaveState { Saved, Pending, Failed };
    SettingsSaveState projectSettingsSaveState = SettingsSaveState::Saved; ///< Project Settings パネルへ通知する自動保存の状態。EditorApp が更新する。
    /// @brief 最後に書き出せたローカル時刻 ("14:03:22")。まだ書いていなければ空。
    std::string       projectSettingsSavedClock; ///< 最後に ProjectSettings を保存したローカル時刻。まだ保存していなければ空。
    /// @brief 保存先の絶対パス (表示用)。
    std::string       projectSettingsPath; ///< Project Settings パネルに表示する ProjectSettings の絶対パス。
    /// @brief 待ち時間を飛ばして今すぐ保存する要求 (失敗時の Retry)。one-shot。
    bool              requestProjectSettingsSave = false; ///< 自動保存の待機を省いて ProjectSettings を書く一回限りの要求。

    /// @brief シーンのオートセーブ (Unreal 方式: 一定間隔で Library/AutoSave へ退避)。正本は EditorSettings。
    bool  sceneAutoSaveEnabled     = true; ///< 編集シーンを Library/AutoSave に定期退避するか。
    int   sceneAutoSaveIntervalSec = 300; ///< 未保存の編集シーンを自動退避する間隔 [s]。
    /// @brief 次のオートセーブまでの残り秒。数えていない (未変更・Play 中・無効) ときは負。
    float sceneAutoSaveRemainingSec = -1.0f; ///< 次の自動退避までの残り時間 [s]。計時していなければ負。

    /// @note  F キーフォーカス: ViewportPanel がセット → main.cpp が DebugCamera に適用してクリア
    bool            requestFocusOnSelected = false; ///< Scene View の視点を現在の選択へ合わせる一回限りの要求。
    math::Vector3   focusTargetPosition    = {}; ///< フォーカス要求で使う選択対象の中心。ワールド座標。
    /// @note  選択バウンディング球の半径。0 は「バウンズ不明」で従来の固定距離フォーカスになる。
    float           focusTargetRadius      = 0.0f; ///< フォーカス対象を包む球の半径 [m]。不明なら 0。

    /// @note  カメラブックマーク呼び出し: ViewportPanel がセット → EditorApp が Teleport してクリア
    bool             requestTeleportCamera  = false; ///< Scene View カメラをブックマーク位置へ移す一回限りの要求。
    math::Vector3    teleportPosition       = {}; ///< 移動要求に含まれるカメラ位置。ワールド座標。
    math::Quaternion teleportRotation       = {}; ///< 移動要求に含まれるカメラのワールド回転。

    /// @note  Scene View の射影切り替え: Operator / パネルがセット → EditorApp が DebugCamera へ適用してクリア。
    /// @note 射影を変えるとピボットからの距離も変わるため、Camera::m_projection だけの書き換えは pivot/focusDistance と食い違い、次のオービットで視点が飛ぶ (Teleport 要求と同じ理由)。ProjectionMode は enum class で完全な定義が要るため Camera.hpp を include する。
    bool                    requestCameraProjection = false; ///< Scene View の射影モードを切り替える一回限りの要求。
    renderer::ProjectionMode cameraProjection       = renderer::ProjectionMode::Perspective; ///< Scene View カメラへ適用する透視または正投影の指定。

    /// @note  エディター専用: ロック中の EntityID 一覧（シリアライズしない）
    std::vector<scene::EntityID> lockedEntities; ///< 編集操作をロックした、このシーンの有効な EntityID 一覧。
    bool IsLocked(scene::EntityID id) const {
        return std::find(lockedEntities.begin(), lockedEntities.end(), id) != lockedEntities.end();
    }
    void ToggleLock(scene::EntityID id) {
        auto it = std::find(lockedEntities.begin(), lockedEntities.end(), id);
        if (it != lockedEntities.end()) {
            lockedEntities.erase(it);
        } else {
            lockedEntities.push_back(id);
        }
    }

    /// @note  エディター専用: 非表示 instanceId → 非表示前の activeSelf 値 (シリアライズしない)。
    /// @note runtime の activeSelf フラグと切り離し、参照メッシュ等をエディタ上だけ隠す。Play/Save 時に一時解除してこの値を復元する。
    std::unordered_map<std::string, bool> editorHiddenGuids; ///< Editor でだけ隠している instanceId と、Play や保存で復元する元の activeSelf。

    /// @note  Inspector の Transform ヘッダーメニュー (Copy / Paste) と
    /// @note  AI の transform.copy / transform.paste が共有するクリップボード。
    /// @note  別々に持つと、同じ操作名なのに中身が違うという追いにくい食い違いになる。
    struct TransformClipboard {
        bool             has = false; ///< クリップボード内にコピー済みの Transform があるか。
        math::Vector3    position{}; ///< コピー元のローカル位置。
        math::Quaternion rotation{}; ///< コピー元のローカル回転。
        math::Vector3    scale{ 1.0f, 1.0f, 1.0f }; ///< コピー元のローカルスケール。
    };
    TransformClipboard transformClipboard; ///< Inspector と AI 操作が共有する Transform のクリップボード。

    /// @note  Util (非所有)
    HotkeyManager*      hotkeyManager = nullptr; ///< EditorApp が所有するショートカット管理器。
    UndoStack*          undoStack   = nullptr; ///< EditorApp が所有する現在の編集操作の Undo / Redo 履歴。
    /// @note  エディター操作の登録簿 (EditorApp 所有)。メニュー・ホットキー・パレットに加え、AI Command Bus の editor.op.list / editor.op.invoke がここを読む。
    /// @note AI 専用の実装経路を作らないための唯一の入口。
    /// @see Docs/design/editor-operator-model.md
    OperatorRegistry*   operators   = nullptr; ///< EditorApp が所有する操作登録簿。メニュー、キー、パレット、AI バスが同じ表を使う。
    /// @brief 必須設定の検証結果と明示的な Inspector 移動要求。
    ScriptRequirementReview scriptRequirementReview; ///< スクリプトの必須設定の検証結果と Inspector を開く要求。
    /// @note  ログ集約シンク (EditorApp 所有)。AI の console.logs クエリが履歴を読む。
    ConsoleSink*        consoleSink = nullptr; ///< EditorApp が所有するログの出力先。AI クエリとパネルが同じ履歴を読む。
    /// @note  AI 連携のライブ状態と制御口。Panel は EditorApp の所有物へ直接依存せず、この窓口だけを使う。
    bool                aiCommandBusEnabled = false; ///< 保存設定で AI Command Bus の起動を許可しているか。
    bool                aiCommandBusRunning = false; ///< EditorApp の AI Command Bus が実際に動作しているか。
    std::function<bool()> startAiCommandBus; ///< EditorApp に AI Command Bus の起動を要求し、成否を受け取る。
    std::function<void()> stopAiCommandBus; ///< 現在の AI Command Bus を EditorApp 経由で停止する。
    /// @brief Playtest の実行器と入力記録 (EditorApp 所有)。playtest.* / input.record が使う。
    playtest::PlaytestRunner* playtest      = nullptr; ///< EditorApp が所有する Playtest のシナリオ実行器。
    playtest::InputRecorder*  inputRecorder = nullptr; ///< EditorApp が所有する Playtest 用の入力記録器。
    /// @note  Inspector から専用 Sprite Editor を開く。パネル所有権を公開せず要求だけを渡す。
    std::function<void(const std::string&)> openSpriteEditor; ///< 指定したアセットを Sprite Editor で開くコールバック。
    /// @note  .behaviortree を BehaviorTreePanel へ渡す (同じくパネル実体は公開しない)。
    std::function<void(const std::string&)> openBehaviorTree; ///< 指定した .behaviortree を Behavior Tree パネルで開くコールバック。
    /// @note  .synth を SfxEditorPanel へ渡す。
    std::function<void(const std::string&)> openSfxEditor; ///< 指定した .synth を SFX Editor で開くコールバック。
    /// @note  .sequence を SequencePanel へ渡す。
    std::function<void(const std::string&)> openSequence; ///< 指定した .sequence を Sequence パネルで開くコールバック。

    /// @note  SFX Editor が編集中の手続き効果音。読み書きが同じ場所を指すよう context に置く。
    /// @note  パネルが実体を抱えて Operator が要求を積む形だと、set_param の直後に inspect を
    /// @note  呼んだときパネルがまだ要求を消費しておらず 1 フレーム古い値が返る。
    std::string      sfxEditorPath; ///< SFX Editor の編集中のアセットパス。新規の下書きなら空。
    audio::SynthSpec sfxEditorSpec; ///< SFX Editor のパネルと操作が共有する、現在編集中の効果音の設定。
    std::string      sfxEditorPresetName; ///< 効果音の由来となったプリセット名。手編集の下書きなら空。
    bool             sfxEditorDirty = false; ///< 効果音の編集中の設定に保存していない変更があるか。
    /// @note  BehaviorTreePanel が開いているドキュメント (パネルが毎フレーム公開する)。
    /// @note  Operator の poll が「今この操作ができるか」を判定するのに使う。
    std::string behaviorTreeEditorPath; ///< Behavior Tree パネルが現在開いているドキュメントのパス。
    /// @note  整列要求 (ワンショット)。パネルが消費して AutoLayout を実行する。
    /// @note 整列は Undo スタックを通す必要があり、それを持つのはパネル。外から木の中身だけ書き換えると整列前へ戻せなくなる。
    bool requestBehaviorTreeAutoLayout = false; ///< Undo 履歴を作る Behavior Tree の自動整列要求。パネルが消費する。
    /// @note  .animcontroller を AnimationGraphPanel へ渡す。
    /// @note selectedAssetPath への追従だと Asset Browser のクリックだけで編集対象が入れ替わり未保存の変更が確認なしに消えるため、「開く」を明示的な操作として切り出す。
    std::function<void(const std::string&)> openAnimationGraph; ///< 指定した .animcontroller を Animation Graph パネルで開くコールバック。
    PlayModeController* playMode    = nullptr; ///< EditorApp が所有する Editor 内 Play の状態管理器。
    TerrainTool*        terrainTool = nullptr; ///< EditorApp が所有し、ViewportPanel が操作する Terrain Tool。
    /// @note  Inspector セクション折り畳み状態 (EditorSettings ↔ ImGui StateStorage の中継)
    std::vector<std::pair<uint32_t, bool>> inspectorSectionState; ///< EditorSettings と ImGui の間を中継するセクション ID ごとの開閉状態。
    /// @note  現在の Scene に紐づく Editor 専用メタデータ。Scene 本体には保存しない。
    EditorSceneState editorSceneState; ///< 現在の編集シーンに対応する Editor 専用のサイドカーデータ。
    /// @note  Inspector がカテゴリ別の既存描画関数から Component カードを収集するための一時窓口。
    /// @note  1 フレームだけ設定し、描画順を決定した後に必ず nullptr へ戻す。
    InspectorComponentDrawCollector* inspectorComponentCollector = nullptr; ///< Inspector が現在の描画でコンポーネントを集める一時参照。描画後は null。

    /// @note  デフォルトインポート設定 (EditorSettings に永続化)
    FbxImportOptions                        defaultImportOptions; ///< FBX インポートへ初期値として渡す設定。EditorSettings に保存する。

    /// @note  Inspector → AssetBrowser: Reimport モーダルを開くリクエスト（empty = なし）
    std::string                             requestOpenImportModal; ///< Asset Browser が再インポート画面を開く対象パス。空なら要求なし。

    std::function<void()>                   markSceneDirty; ///< 現在の編集対象に変更が発生したことを EditorApp に伝えるコールバック。
    std::function<void(const std::string&)> requestOpenScene; ///< 確認や未保存チェックを経由するシーンオープン要求のコールバック。
    bool                                    requestAssetBrowserRefresh = false; ///< Asset Browser に一覧の再構築を要求する一回限りのフラグ。
    bool                                    requestSaveScene = false; ///< 現在のシーンを保存する一回限りの要求。

    /// @note  AI (Command Bus の scene.open / scene.save) 専用のシーン入出力。確認モーダルを挟まない。
    /// @note  モーダルを開くとエディタが人のクリック待ちで止まり、バスの drain も止まる。
    /// @note  未保存の確認は AI 側の引数 (discardUnsaved) で成立させる。
    /// @note  save の path が空なら現在のシーンパスへ上書き保存する。
    std::function<bool(const std::string&)> openScenePathImmediate; ///< 確認ダイアログなしで指定パスを開く AI バス用のコールバック。
    std::function<bool(const std::string&)> saveScenePathImmediate; ///< 確認ダイアログなしでシーンを保存する AI バス用のコールバック。空なら現在の保存先。
    /// @note Asset Browser のアイコンサイズ / ツリー幅はここには持たない。パネルは複数開けるので「どのパネルの値か」が決まらず、1 パネルぶんの状態は EditorSettings::assetBrowserPanels[instance] が唯一の持ち場で OnLoadSettings / OnSaveSettings だけで往復する。
    float                                   editorUiScale              = 1.0f; ///< Editor 全体の UI スケール倍率。EditorSettings に保存する。
    std::vector<std::string>                assetBrowserBookmarks; ///< 全 Asset Browser が共有する、お気に入りフォルダーのパス。
    /// @note  フォルダごとの色 (Unreal の Set Color 相当)。キーは絶対パス、値は IM_COL32 の packed 値。
    /// @note Favorites と同じく EditorSettings 経由で永続化され、左ツリー・グリッド・パンくずの複数の描画箇所が同じ 1 つの表を引くため ctx に置く。
    std::unordered_map<std::string, uint32_t> assetBrowserFolderColors; ///< フォルダーの絶対パスから IM_COL32 の表示色を引く永続設定。
    /// @note  一覧を作り直すべき変更が起きた回数。
    /// @note requestAssetBrowserRefresh は Asset Browser を複数開けると最初に描いたパネルが消費して残りへ届かないため、世代を進め各パネルが自分の適用済み世代と比べる方式にした。
    uint64_t                                 assetBrowserRefreshGeneration = 0; ///< 全 Asset Browser が自分の一覧を再構築するか比較する世代番号。
    /// @note ファイル内容の変更を全パネルの stat キャッシュへ伝え、選択と表示位置は維持する。
    uint64_t                                 assetBrowserMetadataGeneration = 0; ///< 一覧の選択と位置を保ちながらファイルの stat を更新するための世代番号。
    /// @note  Set Color で最近使った色 (新しい順)。プリセットに無い色を選び直す手間を省く。
    std::vector<uint32_t>                    assetBrowserRecentFolderColors; ///< 最近フォルダーに設定した表示色。新しい色が先頭。

    /// @note  カメラブックマーク (最大 9 件、Shift+1~9 で保存・1~9 で呼び出し)
    struct CameraBookmark {
        math::Vector3    position; ///< ブックマークした Scene View カメラのワールド位置。
        math::Quaternion rotation; ///< ブックマークした Scene View カメラのワールド回転。
        bool             valid = false; ///< このカメラブックマークに位置と回転を保存済みか。
    };
    std::array<CameraBookmark, 9>           cameraBookmarks; ///< 数字キー 1～9 に対応する Scene View カメラのブックマーク。

    /// @note  Inspector → AssetBrowser: 編集中 .mat のサムネイル即時更新。
    /// @note  サムネイルはファイル更新時刻でしか再生成されないので、値が変わるたびにリビジョンを進め、
    /// @note  AssetBrowser が未保存の MaterialAsset から描き直せるようにする。
    /// @note  キーは NormalizeAssetPath 済みのプロジェクト相対パス。
    std::unordered_map<std::string, uint64_t> materialPreviewRevisions; ///< 未保存のマテリアル変更でサムネイルを更新するリビジョン。キーは正規化済みのプロジェクト相対パス。

    void BumpMaterialPreviewRevision(const std::string& relativeMaterialPath)
    {
        if (relativeMaterialPath.empty()) {
            return;
        }
        ++materialPreviewRevisions[relativeMaterialPath];
    }
    uint64_t MaterialPreviewRevision(const std::string& relativeMaterialPath) const
    {
        const auto it = materialPreviewRevisions.find(relativeMaterialPath);
        return it == materialPreviewRevisions.end() ? 0ull : it->second;
    }
    /// @}
};

} // namespace fbzz::editor
