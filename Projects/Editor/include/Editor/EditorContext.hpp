// FBZZ Engine
// EditorContext.hpp | fbzz::editor
// パネル間で共有するエディター状態
#pragma once
#include <Editor/GraphLayout.hpp>
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fbzz::renderer { class Camera; }
namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; class ResourceManager; }
namespace fbzz::core     { class MemorySystem; }
namespace fbzz::scene    { struct AnimatorComponent; }
namespace fbzz::editor   { class UndoStack; class PlayModeController; class TerrainTool; class WaterTool; }

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

    // スクリプト DLL / HLSL ホットリロード状態 (StatusBar が表示する)
    enum class HotReloadState { Idle, Compiling, Reloading, Done, Failed };
    HotReloadState hotReloadState   = HotReloadState::Idle;
    std::string    hotReloadMessage;   // StatusBar に表示するテキスト
    float          hotReloadDoneTimer = 0.0f; // Done / Failed 表示を消すカウントダウン (秒)
    bool           scriptReloadBusy = false;  // Script DLL のビルド待ち / ロード中は Play 開始を止める

    // パネル間リクエスト (one-shot フラグ: 発行側が true にセット → 受信側が処理後 false にリセット)
    bool requestOpenProjectSettings  = false;
    bool requestOpenBuildSettings    = false;
    bool requestOpenAnalysis         = false;
    bool requestOpenAnimationGraph   = false; // .fbzzanimcontroller ダブルクリック → AnimationGraphPanel を開く
    bool requestScriptReload         = false;  // StatusBar の ↻ ボタン → TickScriptCompile が処理

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
    float                                   assetBrowserIconSize       = 84.0f;
};

} // namespace fbzz::editor
