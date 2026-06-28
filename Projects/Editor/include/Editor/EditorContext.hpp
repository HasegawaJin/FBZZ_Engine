// FBZZ Engine
// EditorContext.hpp | fbzz::editor
// パネル間で共有するエディター状態
#pragma once
#include <Editor/GraphLayout.hpp>
#include <Editor/Import/FbxImportTool.hpp>
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
namespace fbzz::scene    { struct AnimatorComponent; }
namespace fbzz::editor   { class UndoStack; class PlayModeController; class TerrainTool; class WaterTool; class DetailTool; class FoliageTool; class HotkeyManager; }

namespace fbzz::editor {

struct EditorContext {
    // Animation Graph 上で選択中の編集対象をパネル間で共有する。
    // WHY: ノードキャンバスと詳細編集を別パネルへ分離するため、ポインタではなく EntityID と index を保持する。
    struct AnimationGraphSelection {
        enum class Type {
            None,
            State,
            Transition,
            AnyState,
            AnyStateTransition
        };

        Type            type = Type::None;
        scene::EntityID entityId = scene::EntityID::INVALID;
        std::string     assetPath;
        int             stateIndex = -1;
        int             transitionIndex = -1;

        void Clear()
        {
            type = Type::None;
            entityId = scene::EntityID::INVALID;
            assetPath.clear();
            stateIndex = -1;
            transitionIndex = -1;
        }
    };

    // エンジンオブジェクト (非所有)
    scene::Scene*     activeScene  = nullptr;
    renderer::Camera* editorCamera = nullptr;
    renderer::IRenderer* renderer = nullptr;
    renderer::IImGuiRenderer* imguiRenderer = nullptr;
    renderer::ResourceManager* resources = nullptr;
    core::MemorySystem* memorySystem = nullptr;
    std::string       projectRoot;
    std::string       projectBuildRoot;
    std::string       engineRoot;          // .fbzz_proj の [engine] root (cmake configure で FBZZ_ENGINE_ROOT に使う)
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
    // Animation Graph Editor のノード配置。Editor 専用のため SceneSerializer には渡さない。
    // WHY: AnimatorComponent はランタイム構造体なので、キャンバス座標は instanceId keyed の別データとして保持する。
    std::unordered_map<std::string, GraphLayout> graphLayouts;
    AnimationGraphSelection animationGraphSelection;
    // Animation Graph と Inspector が共有する Controller アセット編集モデル。
    std::shared_ptr<scene::AnimatorComponent> animationControllerEditor;
    std::string animationControllerEditorPath;
    bool animationControllerDirty = false;

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
    bool  viewportFocused  = false;
    bool  sceneViewportHovered = false; // シーンビューにマウスが乗っているか (ホイール制御に使う)
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
    bool  mapHierarchyFilter = true; // Map Mode 中に TerrainGrid/Terrain/Water/Detail/Foliage だけ表示
    bool  mapInspectorFilter = true; // Map Mode 中に Map 関連 Component だけ表示
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
    GizmoMode  gizmoMode  = GizmoMode::Translate;
    GizmoSpace gizmoSpace = GizmoSpace::World;

    // グリッド・スナップ
    bool  showGrid     = true;
    float gridSize     = 1.0f;
    bool  snapEnabled  = false;
    float snapPos      = 1.0f;    // 位置スナップ (m)
    float snapRot      = 15.0f;   // 回転スナップ (度)
    float snapScale    = 0.25f;   // スケールスナップ

    // プロジェクト設定
    fbzz::ProjectSettings projectSettings;

    // 表示オプション (エディター固有)
    bool showLightRange  = true;
    bool showSkeleton    = false;
    bool showStats       = true;  // Game Viewport に Stats オーバーレイを表示する
    bool showTerrainTool = false; // Terrain Tool ウィンドウを表示する
    bool showWaterTool   = false; // Water Tool ウィンドウを表示する
    bool showDetailTool  = false; // Detail Tool ウィンドウを表示する
    bool showFoliageTool = false; // Foliage Tool ウィンドウを表示する
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

    // パネル間リクエスト (one-shot フラグ: 発行側が true にセット → 受信側が処理後 false にリセット)
    bool requestOpenProjectSettings  = false;
    bool requestOpenBuildSettings    = false;
    bool requestOpenAnalysis         = false;
    bool requestOpenAnimationGraph   = false; // .animcontroller ダブルクリック → AnimationGraphPanel を開く
    bool requestScriptReload         = false;  // StatusBar の ↻ ボタン → TickScriptCompile が処理

    // F キーフォーカス: ViewportPanel がセット → main.cpp が DebugCamera に適用してクリア
    bool            requestFocusOnSelected = false;
    math::Vector3   focusTargetPosition    = {};

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

    // Util (非所有)
    HotkeyManager*      hotkeyManager = nullptr;
    UndoStack*          undoStack   = nullptr;
    PlayModeController* playMode    = nullptr;
    TerrainTool*        terrainTool = nullptr; // EditorApp が所有、ViewportPanel が使用
    WaterTool*          waterTool   = nullptr; // EditorApp が所有、ViewportPanel が使用
    DetailTool*         detailTool  = nullptr; // EditorApp が所有、ViewportPanel が使用
    FoliageTool*        foliageTool = nullptr; // EditorApp が所有、ViewportPanel が使用
    // Inspector セクション折り畳み状態 (EditorSettings ↔ ImGui StateStorage の中継)
    std::vector<std::pair<uint32_t, bool>> inspectorSectionState;

    // デフォルトインポート設定 (EditorSettings に永続化)
    FbxImportOptions                        defaultImportOptions;

    // Inspector → AssetBrowser: Reimport モーダルを開くリクエスト（empty = なし）
    std::string                             requestOpenImportModal;

    std::function<void()>                   markSceneDirty;
    std::function<void(const std::string&)> requestOpenScene;
    bool                                    requestAssetBrowserRefresh = false;
    float                                   assetBrowserIconSize       = 84.0f;
    std::vector<std::string>                assetBrowserBookmarks;

    // カメラブックマーク (最大 9 件、Shift+1~9 で保存・1~9 で呼び出し)
    struct CameraBookmark {
        math::Vector3    position;
        math::Quaternion rotation;
        bool             valid = false;
    };
    std::array<CameraBookmark, 9>           cameraBookmarks;
};

} // namespace fbzz::editor
