// FBZZ Engine
// EditorApp.hpp | fbzz::editor
// エディター全体のライフサイクルを管理する
#pragma once
#include <Editor/Compiler.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/ScriptDllLoader.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Editor/PlayModeController.hpp>
// TerrainTool は src/ 内の内部ヘッダーなので前方宣言で対応する
// WHY: TerrainTool.hpp は imgui.h に依存しており、EditorApp.hpp に直接インクルードすると
//      Engine 層のヘッダーが imgui に依存してしまう。std::unique_ptr で所有して隠蔽する。
#include <Engine/Core/IModule.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <Physics/Layer.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; class ResourceManager; }
namespace fbzz::core     { class Window; }
namespace fbzz::editor::ai { class EditorBusDispatcher; class NamedPipeServer; }

namespace fbzz::editor {

class ViewportPanel;
class VFXEditorPanel;
class BuildOutputPanel;
class ProjectSettingsPanel;
class BuildSettingsPanel;
class AssetBrowserPanel;
class ConsolePanel;
class AnalysisPanel;
class MapEditorPanel;
class IblBakePanel;
class AiSettingsPanel;
class TerrainTool;
class WaterTool;
class DetailTool;
class FoliageTool;

class EditorApp final : public core::IModule {
public:
    EditorApp();
    ~EditorApp(); // TerrainTool の完全型が見えるところ (EditorApp.cpp) で定義する

    bool Init(renderer::IRenderer& renderer, renderer::IImGuiRenderer& imguiRenderer, renderer::ResourceManager& resources, core::Window& window);
    void Shutdown();
    bool OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath);

    // IModule — app::Run() から呼ばれるライフサイクル
    [[nodiscard]] bool OnInit()               override;
    void               OnUpdate(float dt)     override;
    void               OnLateUpdate(float dt) override;
    void               OnRender()             override;
    void               OnShutdown()           override;

    void BeginFrame();
    void RenderPanels(EditorContext& ctx);
    void EndFrame(renderer::IImGuiRenderer& imguiRenderer);

    EditorContext& GetContext() { return m_ctx; }
    // Editor Playの更新・描画・ScriptRuntimeが参照する唯一のSceneManagerを返す。
    // WHY: Application側のSceneManagerと混在すると、LoadScene要求を受けたManagerが
    //      Updateされず、Game Viewportだけシーン遷移しないため。
    scene::SceneManager& GetSceneManager() { return m_runtime.GetSceneManager(); }
    scene::ProjectRuntime& GetProjectRuntime() { return m_runtime; }

    // Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    renderer::ResourceHandle<renderer::RenderTargetTag> GetViewportRT() const { return m_sceneViewportRT; }
    renderer::ResourceHandle<renderer::RenderTargetTag> GetGameViewportRT() const { return m_gameViewportRT; }
    // WHY: UI Viewport は Game View の完成済み RT を共有し、編集ガイドだけを ImGui で重ねる。
    //      専用 RT を返す API にすると、Clear 済みの空 RT を表示する経路が再発しやすい。
    renderer::ResourceHandle<renderer::RenderTargetTag> GetUIViewportRT() const { return m_gameViewportRT; }

private:
    // ── IModule ループが所有するステート ────────────────────────────────────
    struct FocusAnim {
        bool          active   = false;
        math::Vector3 startPos = {};
        math::Vector3 endPos   = {};
        math::Vector3 target   = {};
        float         t        = 0.0f;
    };

    void WarmupRenderResources();
    void UpdateFocusAnim(float dt);
    void RenderSceneView(const renderer::Camera& gameCamera, fbzz::LayerMask gameCullingMask);
    void RenderGameView(const renderer::Camera& gameCamera, fbzz::LayerMask gameCullingMask);
    void RenderVFXPreview();

    std::unique_ptr<scene::Scene>  m_scene;
    std::unique_ptr<scene::Scene>  m_vfxPreviewScene;
    scene::SceneManager            m_vfxPreviewSceneManager;
    physics::World                 m_vfxPreviewPhysicsWorld;
    scene::ProjectRuntime          m_runtime;
    renderer::DebugCamera          m_debugCamera;
    renderer::DebugCamera          m_vfxPreviewCamera;
    scene::UISystemContext         m_sceneUICtx;
    FocusAnim                      m_focusAnim;
    float                          m_simulationDt = 0.0f;

    void BuildMenuBar(EditorContext& ctx);
    void InstallNativeMenuBar();
    void BuildPlayToolbar(EditorContext& ctx);
    // ビルド失敗時に、ツールバー下へ消えない通知バーを描画する (Show / Dismiss)。
    void DrawBuildNotificationBar(EditorContext& ctx);
    void ProcessMapEditingModeTransition(uint32_t dockId);
    void ProcessPlayViewportLayoutTransition(uint32_t dockId);
    void EnterMapEditingMode(uint32_t dockId);
    void ExitMapEditingMode(uint32_t dockId);
    void BuildMapEditingLayout(uint32_t dockId);
    void EnterPlayViewportLayout(uint32_t dockId);
    void ExitPlayViewportLayout(uint32_t dockId);
    void BuildPlayViewportLayout(uint32_t dockId);
    void UpdatePlayFocusModeControls();
    // Play 開始/停止/トグル。ツールバーのボタンと Ctrl+P ホットキーの共通経路。
    void StartPlayMode();
    void StopPlayMode();
    void TogglePlayMode();
    void RegisterDefaultHotkeys();
    void ResizeViewportRTsIfNeeded();
    void CheckHotReload();
    void CacheSceneWriteTime();

    // スクリプト DLL ホットリロード
    void InitScriptDll();
    void CheckScriptDirtyAndRebuild();
    void TickScriptCompile();

    // HLSL ホットリロード
    void CheckHlslDirty();
    void TickHlslCompile();

    // ホットリロード共通
    void SetHotReloadState(EditorContext::HotReloadState state, const std::string& msg = "");
    void CaptureCleanScene();
    void RebuildEditorUIFromScene();
    void RefreshSceneDirtyState(bool force);
    void UpdateWindowTitle();
    void MarkSceneDirty();
    void ConfirmDiscardUnsaved(const std::string& actionName, std::function<void()> action);
    void NewScene();
    void RequestNewScene();
    void RequestOpenSceneFromDialog();
    void RequestOpenScenePath(const std::string& path);
    void RequestExit();
    bool OpenSceneFromDialog();
    bool OpenScenePath(const std::string& path);
    bool SaveScene();
    bool SaveSceneAsDialog();
    bool SaveScenePath(const std::string& requestedPath); // 保存の実体 (ダイアログ / AI 共通)
    void RemoveEditorHiding();   // Play/Save 前に editor-only 非表示を一時解除
    void RestoreEditorHiding();  // Play 復元/Save 後に editor-only 非表示を再適用

    // 最近開いた/保存したシーンを先頭へ積む (EditorSettings::recentScenes を更新)。
    void AddRecentScene(const std::string& path);

    // オートセーブ / セッションロック / クラッシュ復旧 (Library/AutoSave 配下で完結)。
    [[nodiscard]] std::string AutoSaveDir() const;
    [[nodiscard]] std::string AutoSavePath() const;
    [[nodiscard]] std::string SessionLockPath() const;
    void WriteSessionLock();       // 起動時: 生存セッションの印を書く
    void ClearSessionLock();       // 正常終了時: 印とオートセーブ中間物を掃除する
    void TickAutoSave(float dt);   // 一定間隔で編集シーンを中間ファイルへ退避する
    void ProcessCrashRecovery();   // 前回異常終了時にオートセーブ復元を促す

    // コマンドパレット (Ctrl+K): アクション/アセット/GameObject を横断検索して実行する。
    void RefreshPaletteAssetIndex();
    void DrawCommandPalette(EditorContext& ctx);

    // Prefab 編集モード: 編集中シーンを丸ごと退避して .prefab だけを開く。
    // WHY: Scene オブジェクトを差し替えると ProjectRuntime / SceneManager / レンダーパスが
    //      参照している m_scene ポインタを全て張り替える必要があり、経路が増えるほど
    //      「片方だけ古いシーンを見ている」バグが出やすい。m_scene の中身だけを
    //      入れ替えれば、上位の結線は一切触らずに済む。
    void ProcessPrefabEditRequests();
    // ディスク上で書き換わった .prefab をシーン内のインスタンスへ反映する。
    void ProcessPrefabDiskReloads();
    void EnterPrefabEditMode(const std::string& assetRelPath);
    bool SavePrefabEdit();
    void ExitPrefabEditMode(bool save);
    void DrawPrefabEditBar(EditorContext& ctx);

    // ショートカット一覧オーバーレイ (F1) と選択ヒストリ (Alt+←/→)。
    void DrawShortcutsOverlay(EditorContext& ctx);
    void RecordSelectionHistory();
    void NavigateSelectionHistory(int dir);

    // AI Command Bus の所有権と寿命は EditorApp に集約し、Panel には制御コールバックだけを公開する。
    bool StartAiCommandBus();
    void StopAiCommandBus();
    // AI 要求1行を、このプロセスで処理するか独立VFXEditorプロセスへ転送するかを決めて実行する。
    // WHY: VFX Preview World は FBZZVFXEditor.exe が所有するため、
    //      preview 系をここで処理すると必ず「World がありません」になる。
    std::string HandleAiRequest(const std::string& request);

    EditorContext                        m_ctx;
    std::vector<std::unique_ptr<IPanel>> m_panels;
    std::unique_ptr<StatusBar>           m_statusBar;

    UndoStack          m_undoStack;
    HotkeyManager      m_hotkeys;
    ConsoleSink        m_consoleSink;
    EditorSettings     m_settings;
    SceneDirtyTracker  m_dirtyTracker;
    std::string        m_projectRoot;
    std::string        m_projectSettingsPath;
    float              m_dirtyPollTimer = 0.0f;
    bool               m_titleInitialized = false;
    bool               m_lastTitleDirty = false;
    std::string        m_lastTitleScenePath;
    PlayModeController              m_playMode;
    // Play→Stop 時の再ベイクを避けるため、Play 開始前にベイク済み NavMesh をキャッシュする。
    // key = GameObject::instanceId
    std::unordered_map<std::string, scene::NavMesh> m_navMeshPlayCache;
    std::unique_ptr<TerrainTool>    m_terrainTool; // pimpl: EditorApp.hpp が imgui に依存しないよう unique_ptr で隠蔽
    std::unique_ptr<WaterTool>      m_waterTool;   // WaterTool も同じ pimpl パターンで隠蔽する
    std::unique_ptr<DetailTool>     m_detailTool;  // DetailTool も同じ pimpl パターン
    std::unique_ptr<FoliageTool>    m_foliageTool; // FoliageTool も同じ pimpl パターン
    std::string                     m_normalLayoutIni;
    const char*                     m_normalIniFilename = nullptr;
    std::string                     m_imguiIniPath;   // io.IniFilename が指すパス (文字列寿命を保持)
    std::vector<bool>               m_normalPanelVisibility;
    std::string                     m_playLayoutIni;
    const char*                     m_playIniFilename = nullptr;
    std::vector<bool>               m_playPanelVisibility;
    bool                            m_playViewportLayoutActive = false;
    bool                            m_playFocusedCursorHidden = false;
    bool                            m_terrainToolWasActive = false;
    int                             m_terrainToolModeBeforeMap = 0;
    bool                            m_waterToolWasActive = false;
    bool                            m_detailToolWasActive = false;
    bool                            m_foliageToolWasActive = false;

    // ビルドコンソール: Script / HLSL コンパイルの出力・診断・履歴を集約する。
    // WHY: m_ctx.buildConsole がこれを指し、Build Output パネル・StatusBar・通知バーが共有する。
    BuildConsole             m_buildConsole;
    BuildOutputPanel*        m_buildOutputPanel = nullptr;

    std::unique_ptr<ai::EditorBusDispatcher> m_aiDispatcher;
    std::unique_ptr<ai::NamedPipeServer>      m_aiPipeServer;

    // スクリプト DLL ホットリロード
    ScriptDllLoader          m_scriptDll;
    std::filesystem::path    m_scriptDllPath;      // SandboxScripts.dll のビルド出力パス
    std::filesystem::path    m_scriptsSourceDir;   // Scripts/ ソースディレクトリ (変更検知用)
    FILETIME                 m_lastScriptWriteTime = {};  // Scripts/ ツリー内で最も新しい更新時刻
    Compiler                 m_scriptCompiler;
    bool                     m_scriptCompilePending  = false; // 変更検知からビルド開始待ち
    bool                     m_scriptInitialBuild    = false; // true のとき初回ビルド (skipDeps=false)
    float                    m_scriptDebounceTimer   = 0.0f;  // デバウンス用タイマー (秒)
    float                    m_scriptDirtyPollTimer  = 0.0f;  // Scripts/ ツリー監視を毎フレーム走らせないための間隔管理

    // HLSL ホットリロード
    std::filesystem::path    m_hlslSourceDir;      // Assets/shaders/ ディレクトリ
    std::filesystem::path    m_compileShadersScript; // compile_shaders.bat パス
    FILETIME                 m_lastHlslWriteTime = {}; // HLSL ツリー内で最も新しい更新時刻
    Compiler                 m_hlslCompiler;
    bool                     m_hlslCompilePending = false;
    float                    m_hlslDebounceTimer  = 0.0f;
    float                    m_hlslDirtyPollTimer = 0.0f; // HLSL ツリー監視を毎フレーム走らせないための間隔管理

    FILETIME                                 m_lastSceneWriteTime = {};
    // HLSL ソースツリーの内容フィンガープリント。前回値との差分でホットリロードを判定する。
    std::uint64_t                            m_hlslSourceFingerprint = 0;

    // オートセーブ / クラッシュ復旧の状態。
    float        m_autoSaveTimer = 0.0f;          // 前回オートセーブからの経過秒
    bool         m_crashRecoveryChecked = false;  // 復旧プロンプトは起動後1回だけ
    std::string  m_pendingRecoveryAutoSave;       // 前回異常終了時に見つかったオートセーブのパス

    // コマンドパレット (Ctrl+K) の状態。
    std::vector<std::string> m_paletteAssetPaths;      // 開いた瞬間のアセット索引スナップショット
    bool                     m_commandPaletteOpen = false;   // 起動要求フラグ
    char                     m_commandPaletteQuery[256] = {}; // 検索クエリ入力バッファ
    int                      m_commandPaletteSel = 0;        // 選択中の候補行
    bool                     m_commandPaletteFocus = false;  // 開いた次フレームで検索欄へフォーカス

    // Prefab 編集モードの退避状態。編集前のシーンをテキストで丸ごと保持する。
    std::string              m_prefabEditStashedScene;
    std::string              m_prefabEditStashedScenePath;
    bool                     m_prefabEditStashedDirty = false;
    std::vector<std::string> m_prefabEditStashedSelection;   // GUID (EntityID は復元で変わる)
    std::string              m_prefabEditDiskPath;           // 保存先の絶対パス
    // この編集セッション中に 1 度でも保存したか。
    // WHY: 「Save Prefab」→「Back to Scene」の順に押した場合、閉じる操作自体は
    //      保存を伴わないが、アセットは既に変わっている。ここを見ないと
    //      シーンへ戻ったときにインスタンスへ反映されない。
    bool                     m_prefabEditSaved = false;

    // ショートカット一覧オーバーレイ (F1) と選択ヒストリ (Alt+←/→)。
    bool                         m_showShortcutsOverlay = false;
    std::vector<scene::EntityID> m_selectionHistory;          // 選択の往復履歴
    int                          m_selectionHistoryIndex = -1;
    bool                         m_selectionNavigating = false; // 履歴移動由来の選択は再記録しない
    scene::EntityID              m_lastRecordedSelection;       // 直近で履歴に積んだ選択 (既定=INVALID)

    HWND                                     m_hwnd          = nullptr;
    core::Window*                            m_window        = nullptr;
    renderer::IRenderer*                     m_renderer      = nullptr;
    renderer::IImGuiRenderer*                m_imguiRenderer = nullptr;
    renderer::ResourceManager*               m_resources     = nullptr;
    ViewportPanel*                           m_sceneViewportPanel     = nullptr;
    ViewportPanel*                           m_gameViewportPanel      = nullptr;
    ViewportPanel*                           m_uiViewportPanel        = nullptr;
    VFXEditorPanel*                          m_vfxEditorPanel         = nullptr;
    ProjectSettingsPanel*                    m_projectSettingsPanel   = nullptr;
    BuildSettingsPanel*                      m_buildSettingsPanel     = nullptr;
    AssetBrowserPanel*                       m_assetBrowserPanel      = nullptr;
    // StatusBar から Asset Browser と同じ操作感で開閉するため、専用ポインタで保持する。
    ConsolePanel*                            m_consolePanel           = nullptr;
    AnalysisPanel*                           m_analysisPanel          = nullptr;
    MapEditorPanel*                          m_mapEditorPanel         = nullptr;
    IblBakePanel*                            m_iblBakePanel           = nullptr;
    AiSettingsPanel*                         m_aiSettingsPanel        = nullptr;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_vfxPreviewRT;
};

} // namespace fbzz::editor
