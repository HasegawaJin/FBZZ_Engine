/// @file    EditorApp.hpp
/// @brief   エディター全体のライフサイクルを管理する。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/Compiler.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/StatusBar.hpp>
#include <Editor/ScriptDllLoader.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Util/UndoStack.hpp>
#include <Editor/Util/HotkeyManager.hpp>
#include <Editor/Util/ConsoleSink.hpp>
#include <Editor/Util/BuildConsole.hpp>
#include <Editor/Util/EditorSettings.hpp>
#include <Editor/Util/MemoryLeakDiff.hpp>
#include <Editor/Util/SceneDirtyTracker.hpp>
#include <Editor/PlayModeController.hpp>
#include <Editor/Playtest/InputRecorder.hpp>
#include <Editor/Playtest/PlaytestRunner.hpp>
#include <Engine/Core/IModule.hpp>
#include <Engine/Renderer/DebugCamera.hpp>
#include <Engine/Renderer/ResourceHandle.hpp>
#include <Engine/Scene/Components/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/ProjectRuntime.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/Vector3.hpp>
#include <Physics/Layer.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct HWND__;
namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; class ResourceManager; }
namespace fbzz::scene { struct RenderFrameGeometryCache; }
namespace fbzz::core     { class Window; }
namespace fbzz::editor::ai { class EditorBusDispatcher; class NamedPipeServer; }

namespace fbzz::editor {

class ViewportPanel;
class BuildOutputPanel;
class ProjectSettingsPanel;
class BuildSettingsPanel;
class AssetBrowserPanel;
class ConsolePanel;
class AnalysisPanel;
class RenderPassViewerPanel;
class MapEditorPanel;
class IblBakePanel;
class VolumeFlipbookBakePanel;
class FluidEditorPanel;
class AssetMaintenancePanel;
class NavigationPanel;
class AiSettingsPanel;
/// @note TerrainTool.hpp は imgui に依存するため前方宣言のみに留め、unique_ptr で所有して隠蔽する (EditorApp.hpp を imgui 非依存に保つ)。
class TerrainTool;

class EditorApp final : public core::IModule {
public:
    EditorApp();
    ~EditorApp(); ///< @note TerrainTool の完全型が見えるところ (EditorApp.cpp) で定義する

    bool Init(renderer::IRenderer& renderer, renderer::IImGuiRenderer& imguiRenderer, renderer::ResourceManager& resources, core::Window& window);
    void Shutdown();
    bool OpenProject(const std::string& projectRoot, const std::string& projectSettingsPath, const std::string& scenePath);

    /// @brief `--batch` 起動の設定。シナリオを回し終えたら Application を終了させる。
    struct BatchOptions {
        std::filesystem::path   scenarioPath;
        std::filesystem::path   reportPath;
        playtest::PlaytestOptions playtest;
        [[nodiscard]] bool IsEnabled() const { return !scenarioPath.empty(); }
    };
    /// @pre OpenProject の後、Application::Run の前に呼ぶ。
    void ConfigureBatch(BatchOptions options) { m_batch = std::move(options); }
    /// @return 0 = 全合格 / 1 = 不合格 / 2 = シナリオを読めない・開始できない。バッチでなければ 0。
    [[nodiscard]] int BatchExitCode() const { return m_batchExitCode; }

    /// @brief IModule — app::Run() から呼ばれるライフサイクル
    [[nodiscard]] bool OnInit()               override;
    void               OnInputPolled()        override;
    void               OnUpdate(float dt)     override;
    void               OnLateUpdate(float dt) override;
    void               OnRender()             override;
    void               OnShutdown()           override;

    void BeginFrame();
    void RenderPanels(EditorContext& ctx);
    void EndFrame(renderer::IImGuiRenderer& imguiRenderer);

    EditorContext& GetContext() { return m_ctx; }
    /// @brief Editor Playの更新・描画・ScriptRuntimeが参照する唯一のSceneManagerを返す。
    /// @note Application 側の SceneManager と混在すると、LoadScene 要求を受けた Manager が Update されず Game Viewport だけシーン遷移しない。
    scene::SceneManager& GetSceneManager() { return m_runtime.GetSceneManager(); }
    scene::ProjectRuntime& GetProjectRuntime() { return m_runtime; }

    /// @brief Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    renderer::ResourceHandle<renderer::RenderTargetTag> GetViewportRT() const { return m_sceneViewportRT; }
    renderer::ResourceHandle<renderer::RenderTargetTag> GetGameViewportRT() const { return m_gameViewportRT; }
    /// @note UI Viewport は Game View の完成済み RT を共有し、編集ガイドだけを ImGui で重ねる。専用 RT にすると Clear 済み空 RT を表示する経路が再発しやすい。
    renderer::ResourceHandle<renderer::RenderTargetTag> GetUIViewportRT() const { return m_gameViewportRT; }

private:
    /// @name IModule ループが所有するステート
    /// @{
    struct FocusAnim {
        bool          active   = false;
        math::Vector3 startPos = {};
        math::Vector3 endPos   = {};
        math::Vector3 target   = {};
        float         t        = 0.0f;
    };

    void WarmupRenderResources();
    void UpdateFocusAnim(float dt);
    void RenderSceneView(const renderer::Camera& gameCamera, fbzz::LayerMask gameCullingMask,
                         scene::RenderFrameGeometryCache* frameGeometry);
    void RenderGameView(const renderer::Camera& gameCamera, fbzz::LayerMask gameCullingMask,
                        scene::RenderFrameGeometryCache* frameGeometry);

    std::unique_ptr<scene::Scene>  m_scene;
    scene::ProjectRuntime          m_runtime;
    renderer::DebugCamera          m_debugCamera;
    scene::UISystemContext         m_sceneUICtx;
    FocusAnim                      m_focusAnim;
    float                          m_simulationDt = 0.0f;

    void BuildMenuBar(EditorContext& ctx);
    void InstallNativeMenuBar();
    void BuildPlayToolbar(EditorContext& ctx);
    /// @brief Help > About。メニューの中で Begin すると閉じた瞬間に消えるので、毎フレーム別に描く。
    void DrawAboutDialog();
    bool m_aboutRequested = false;
    /// @brief ビルド失敗時に、ツールバー下へ消えない通知バーを描画する (Show / Dismiss)。
    void DrawBuildNotificationBar(EditorContext& ctx);
    /// @brief GUID 重複を抱えているとき、同じ場所へ通知バーを描画する。
    /// @note 重複はスキャン中の Console エラーでしか報告されず、起動ログに流れて誰も気づかないまま参照が吸われ続けていたため画面にも出す。
    void DrawGuidConflictBar(EditorContext& ctx);
    void ProcessMapEditingModeTransition(uint32_t dockId);
    void ProcessPlayViewportLayoutTransition(uint32_t dockId);
    void EnterMapEditingMode(uint32_t dockId);
    void ExitMapEditingMode(uint32_t dockId);
    void BuildMapEditingLayout(uint32_t dockId);
    void EnterPlayViewportLayout(uint32_t dockId);
    void ExitPlayViewportLayout(uint32_t dockId);
    void BuildPlayViewportLayout(uint32_t dockId);
    /// @brief Play 中の OS カーソル (拘束範囲・方針の適用と Escape の解放)。
    void UpdatePlayCursorControls();
    /// @brief Play 開始/停止/トグル。ツールバーのボタンと Ctrl+P ホットキーの共通経路。
    void StartPlayMode();
    void StopPlayMode();
    void TogglePlayMode();
    void RegisterDefaultHotkeys();
    /// @}

    /// @name Operator モデル (Docs/design/editor-operator-model.md)
    /// @{
    /// @brief メニュー・ホットキー・コマンドパレットは、いずれもここへ登録された操作の
    /// @brief 投影として描かれる。実行可能条件 (poll) を 1 箇所に持つことで、
    /// @brief 「メニューではグレーアウトなのにホットキーからは通る」を構造的に防ぐ。
    void RegisterBuiltinOperators();
    /// @brief パネル表示 / UI スケール / Prefab 編集モードの操作 (src/Op/PanelOperators.cpp)。
    /// @brief m_panels を参照するため EditorApp のメンバー関数として登録する。
    void RegisterPanelOperators();
    /// @brief ウィンドウ名または View メニュー名 (大小無視の完全一致) でパネルを引く。
    [[nodiscard]] IPanel* FindPanelByName(const std::string& name) const;

    /// @brief パネル側の設定を回収して editor_settings.toml へ即時書き出す。
    /// @note ビルド構成のように落ちても残ってほしい設定があるため Shutdown とは別に呼べる。Shutdown はこれに加えカメラ等も回収する。
    void PersistEditorSettings();
    /// @brief EditorContext のライブ値を m_settings へ書き戻す。Shutdown と PersistEditorSettings が共有する。
    void CaptureEditorSettingsFromContext();

    /// @brief ProjectSettings.toml の自動保存。変化が止まって kProjectSettingsSaveDelay 秒後に書く。
    /// @note 変化の検出は ToToml() の比較。Debug メニューや AI 経由の変更も同じ経路で拾える。
    void TickProjectSettingsAutoSave(float dt);
    /// @brief 今すぐ書き、保存済みの内容と表示状態を更新する。
    bool SaveProjectSettingsNow();

    /// @brief 通常 Workspace でのパネル開閉状態を EditorSettings へ採る / 戻す。
    /// @note Map Mode と Play Maximized は visible を一時潰すため、そのまま保存すると「パネルが 1 枚しか無い」状態が焼き付く。
    void CaptureNormalPanelVisibility();
    void RestorePanelVisibility();
    /// @brief panel.focus を「既に手元にあるパネルポインタ」から呼ぶ小さな包み。panel が null なら何もしない。
    /// @note メニュー側は具体ポインタを持つが名前引きへ戻さず operator を通す (人と AI の経路を揃えるため)。
    void InvokePanelFocus(IPanel* panel);
    /// @brief 1 つの operator をメニュー項目として描く。
    /// @brief 表示名・実行可否・ショートカット表示をすべてレジストリと HotkeyManager から引く。
    /// @brief labelOverride は文脈で名前が変わる項目 (Prefab 編集中の "Save Prefab") 用。
    bool MenuItemOp(const char* operatorId, const char* labelOverride = nullptr);
    /// @brief 引数付きで呼ぶメニュー項目。ラジオ表示 (View Mode の 4 つ) と、
    /// @brief 対象を指定するトグル (パネルの表示切替) がこれを使う。
    /// @brief チェックマークは operator の checked を同じ args で評価した値になる。
    bool MenuItemOpArgs(const char* operatorId, const OpArgs& args,
                        const char* labelOverride = nullptr);
    /// @brief 描画モード切り替えを operator へ渡す (ネイティブメニューの 4 項目が使う)。
    void InvokeViewMode(const char* mode);
    /// @brief 4 面が共有する実行文脈を組み立てる (生成箇所を 1 つに保つ)。
    [[nodiscard]] OpContext MakeOpContext();
    [[nodiscard]] bool      CanInvokeOperator(std::string_view id, const OpArgs& args = {});
    OpResult                InvokeOperator(std::string_view id, const OpArgs& args = {});
    void ResizeViewportRTsIfNeeded();
    void CheckHotReload();
    void CacheSceneWriteTime();

    /// @brief スクリプト DLL ホットリロード
    void InitScriptDll();
    void CheckScriptDirtyAndRebuild();
    void TickScriptCompile();

    /// @brief HLSL ホットリロード
    /// @brief 監視先のシェーダーツリーを決め、プロジェクトの持ち物かどうかを判定する。
    /// @brief 共有 SDK 側へ解決された場合は監視も再コンパイルも行わない。
    void InitHlslHotReload();
    void CheckHlslDirty();
    void TickHlslCompile();

    /// @brief ホットリロード共通
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
    bool SaveScenePath(const std::string& requestedPath); ///< @note 保存の実体 (ダイアログ / AI 共通)
    void RemoveEditorHiding();   ///< @note Play/Save 前に editor-only 非表示を一時解除
    void RestoreEditorHiding();  ///< @note Play 復元/Save 後に editor-only 非表示を再適用
    /// @brief Play 中のシーン読み書き要求を断る (断ったら true)。理由は Console と Toast へ。
    bool RejectSceneIOWhilePlaying(const char* action);

    /// @brief Hierarchy の非表示 / ロックを Scene サイドカー (`<scene>`.meta) と往復させる。
    /// @note 非表示とロックはセッション中ずっと書き換わる。常時同期させると Undo や Play の内部スナップショット (SceneIO::Serialize) のたびに巻き添えで書き戻るため、「開く」「保存する」の瞬間だけ橋渡しする。
    void CaptureEditorViewStateToSceneMeta();
    void ApplyEditorViewStateFromSceneMeta();

    /// @brief 最近開いた/保存したシーンを先頭へ積む (EditorSettings::recentScenes を更新)。
    void AddRecentScene(const std::string& path);

    /// @brief オートセーブ / セッションロック / クラッシュ復旧 (Library/AutoSave 配下で完結)。
    [[nodiscard]] std::string AutoSaveDir() const;
    [[nodiscard]] std::string AutoSavePath() const;
    [[nodiscard]] std::string SessionLockPath() const;
    void WriteSessionLock();       ///< @note 起動時: 生存セッションの印を書く
    void ClearSessionLock();       ///< @note 正常終了時: 印とオートセーブ中間物を掃除する
    /// @brief 未保存の変更がある間だけ時間を数え、間隔ごとに編集シーンを中間ファイルへ退避する。
    /// @note 直前 kAutoSaveWarningSec 秒は通知を出す。期限に操作中なら手を離すまで待つ (Unreal と同じ)。
    void TickAutoSave(float dt);
    /// @brief 今すぐ中間ファイルへ書き、タイマーを戻す。本体のシーンファイルと dirty 状態は変えない。
    bool AutoSaveNow();
    /// @brief オートセーブ直前のカウントダウン通知 (今すぐ保存 / 延期)。
    void DrawAutoSaveNotice();
    /// @brief 前回の印とオートセーブが残っていれば、復旧の提案を予約する。OpenProject がシーンを開いた後に呼ぶ。
    void DetectCrashRecovery();
    void ProcessCrashRecovery();   ///< @note 前回異常終了時にオートセーブ復元を促す

    /// @brief コマンドパレット (Ctrl+K): アクション/アセット/GameObject を横断検索して実行する。
    void RefreshPaletteAssetIndex();
    void DrawCommandPalette(EditorContext& ctx);

    /// @brief Prefab 編集モード: 編集中シーンを丸ごと退避して .prefab だけを開く。
    /// @note Scene オブジェクトごと差し替えると各所が持つ m_scene ポインタの張替えが増え「片方だけ古いシーンを見る」バグが出やすいため、m_scene の中身だけを入れ替える。
    void ProcessPrefabEditRequests();
    /// @brief ディスク上で書き換わった .prefab をシーン内のインスタンスへ反映する。
    void ProcessPrefabDiskReloads();
    /// @brief ディスク上で書き換わったアセットを、実行中のキャッシュへ読み直す。
    void ProcessAssetDiskReloads();
    /// @}

    /// @name 開いているシーンとディスクの食い違いを扱う
    /// @{
    /// @note AI・外部エディタ・git が直接書く前提のため、読み込み・保存の両方で同じタイムスタンプを見て他人の変更を検出する。
    void ProcessSceneDiskReload();
    void DrawSceneReloadBar(EditorContext& ctx);
    /// @brief 現在のシーンパスの更新時刻を控える。開いた直後と保存直後に呼ぶ。
    void CaptureSceneDiskStamp();
    /// @brief 控えた時刻とディスクの現在値が食い違うか (= 自分以外が書いたか)。
    [[nodiscard]] bool IsSceneStaleOnDisk() const;
    /// @brief ディスクから読み直す。未保存の変更は失われる。
    bool ReloadSceneFromDisk();
    /// @brief 読み直したシーンファイルの中で、解決できない guid 参照を Console へ報告する。
    void ReportUnresolvedSceneRefs(const std::string& path) const;
    void EnterPrefabEditMode(const std::string& assetRelPath);
    bool SavePrefabEdit();
    void ExitPrefabEditMode(bool save);
    void DrawPrefabEditBar(EditorContext& ctx);

    /// @brief ショートカット一覧オーバーレイ (F1) と選択ヒストリ (Alt+←/→)。
    void DrawShortcutsOverlay(EditorContext& ctx);
    void RecordSelectionHistory();
    void NavigateSelectionHistory(int dir);

    /// @brief AI Command Bus の所有権と寿命は EditorApp に集約し、Panel には制御コールバックだけを公開する。
    bool StartAiCommandBus();
    void StopAiCommandBus();
    /// @brief AI 要求 1 行を処理する。
    std::string HandleAiRequest(const std::string& request);

    EditorContext                        m_ctx;
    std::vector<std::unique_ptr<IPanel>> m_panels;
    std::unique_ptr<StatusBar>           m_statusBar;

    UndoStack          m_undoStack;
    HotkeyManager      m_hotkeys;
    OperatorRegistry   m_operators;
    ConsoleSink        m_consoleSink;
    EditorSettings     m_settings;
    SceneDirtyTracker  m_dirtyTracker;
    std::string        m_projectRoot;
    std::string        m_projectSettingsPath;
    /// @brief 最後にディスクと一致していた ProjectSettings の TOML。
    std::string        m_projectSettingsSavedToml;
    /// @brief 前回ポーリング時の TOML。値が動き続けている間は保存を待つ。
    std::string        m_projectSettingsLastToml;
    /// @brief 書き出しに失敗した TOML。同じ内容のままなら自動では再試行しない。
    std::string        m_projectSettingsFailedToml;
    float              m_projectSettingsPollTimer = 0.0f;
    float              m_projectSettingsIdleTime  = 0.0f;
    float              m_dirtyPollTimer = 0.0f;
    bool               m_titleInitialized = false;
    bool               m_lastTitleDirty = false;
    std::string        m_lastTitleScenePath;
    PlayModeController              m_playMode;
    /// @brief Play→Stop 時の再ベイクを避けるため、Play 開始前にベイク済み NavMesh をキャッシュする (key = GameObject::instanceId)。
    /// @note 統計とソースハッシュも戻さないと、Stop した瞬間に Navigation パネルが「ベイク後にソースが変わった」と誤検知する。
    struct NavMeshPlayCacheEntry {
        scene::NavMesh              navMesh;
        scene::NavMeshBakeStats     stats;
        scene::NavMeshBakeDebugGrid debug;
        uint64_t                    sourceHash = 0;
    };
    std::unordered_map<std::string, NavMeshPlayCacheEntry> m_navMeshPlayCache;
    /// @brief Play 開始時の解決済み設定から作る実行用コピー。編集側の自動保存とは独立する。
    renderer::RenderSettings m_playRenderSettings;
    /// @brief Play 開始時に取った Renderer リソースの基準。Stop 後の復元が落ち着いてから比べる。
    MemoryLeakDiff     m_memoryLeakDiff;
    /// @brief .fluid の焼き・プレビューのジョブ窓口 (m_ctx.fluidBake がこれを指す)。
    /// @brief パネルではなくここが持つので、パネルを閉じても AI から頼んでも焼きが進む。
    std::unique_ptr<FluidBakeService> m_fluidBake;
    std::unique_ptr<TerrainTool>    m_terrainTool; ///< @note pimpl: EditorApp.hpp が imgui に依存しないよう unique_ptr で隠蔽
    std::string                     m_normalLayoutIni;
    const char*                     m_normalIniFilename = nullptr;
    std::string                     m_imguiIniPath;   ///< @note io.IniFilename が指すパス (文字列寿命を保持)
    std::vector<bool>               m_normalPanelVisibility;
    std::string                     m_playLayoutIni;
    const char*                     m_playIniFilename = nullptr;
    std::vector<bool>               m_playPanelVisibility;
    bool                            m_playViewportLayoutActive = false;
    bool                            m_playCursorApplied  = false; ///< @note Play 開始時のカーソル初期化を済ませたか
    bool                            m_playCursorReleased = false; ///< @note Escape で一時解放したか (2 回目で Stop)
    bool                            m_terrainToolWasActive = false;
    int                             m_terrainToolModeBeforeMap = 0;

    /// @brief ビルドコンソール: Script / HLSL コンパイルの出力・診断・履歴を集約する。
    /// @note m_ctx.buildConsole がこれを指し、Build Output パネル・StatusBar・通知バーが共有する。
    BuildConsole             m_buildConsole;
    BuildOutputPanel*        m_buildOutputPanel = nullptr;

    std::unique_ptr<ai::EditorBusDispatcher> m_aiDispatcher;
    std::unique_ptr<ai::NamedPipeServer>      m_aiPipeServer;

    /// @brief Playtest の手順が叩く専用のバス。AI のパイプを閉じていても (バッチ実行でも) 回せるよう別に持つ。
    std::unique_ptr<ai::EditorBusDispatcher> m_playtestDispatcher;
    playtest::PlaytestRunner                 m_playtest;
    playtest::InputRecorder                  m_inputRecorder;
    BatchOptions                             m_batch;
    bool                                     m_batchStarted = false;
    int                                      m_batchExitCode = 0;
    /// @brief シナリオを 1 フレーム進める。OnInputPolled から呼ぶ。
    void TickPlaytest();

    /// @brief スクリプト DLL ホットリロード
    ScriptDllLoader          m_scriptDll;
    std::filesystem::path    m_scriptDllPath;      ///< @note SandboxScripts.dll のビルド出力パス
    std::filesystem::path    m_scriptsSourceDir;   ///< @note Scripts/ ソースディレクトリ (変更検知用)
    FILETIME                 m_lastScriptWriteTime = {};  ///< @note Scripts/ ツリー内で最も新しい更新時刻
    Compiler                 m_scriptCompiler;
    bool                     m_scriptCompilePending  = false; ///< @note 変更検知からビルド開始待ち
    bool                     m_scriptInitialBuild    = false; ///< @note true のとき初回ビルド (skipDeps=false)
    float                    m_scriptDebounceTimer   = 0.0f;  ///< @note デバウンス用タイマー (秒)
    float                    m_scriptDirtyPollTimer  = 0.0f;  ///< @note Scripts/ ツリー監視を毎フレーム走らせないための間隔管理
    /// @brief ビルド成功後、Reloading 表示を 1 フレーム描いたか。
    /// @note Reload() は同期で数百 ms 止まるため、先に描かないと直前の «Compiling 89%» のまま固まって見える。
    bool                     m_scriptReloadShown     = false;
    /// @brief SDK 解決失敗による CMakeCache 再構成をこのセッションで実施済みか。
    /// @note SDK が本当に無い場合の configure 失敗 → 再ビルドの無限ループを避けるため 1 回に制限する。
    bool                     m_scriptSdkRecoveryDone = false;

    /// @brief HLSL ホットリロード
    std::filesystem::path    m_hlslSourceDir;      ///< @note Assets/shaders/ ディレクトリ
    std::filesystem::path    m_compileShadersScript; ///< @note compile_shaders.ps1 のパス。
    /// @brief m_hlslSourceDir が「開いているプロジェクトの持ち物」か。
    /// @brief 共有 SDK の shader へ解決されたときは false になり、監視も再コンパイルも行わない
    /// @brief (実行中の Editor が SDK を書き換えると、その SDK を使う他プロジェクトまで巻き込む)。
    bool                     m_hlslProjectOwned = false;
    FILETIME                 m_lastHlslWriteTime = {}; ///< @note HLSL ツリー内で最も新しい更新時刻
    Compiler                 m_hlslCompiler;
    bool                     m_hlslCompilePending = false;
    float                    m_hlslDebounceTimer  = 0.0f;
    float                    m_hlslDirtyPollTimer = 0.0f; ///< @note HLSL ツリー監視を毎フレーム走らせないための間隔管理

    FILETIME                                 m_lastSceneWriteTime = {};
    /// @brief HLSL ソースツリーの内容フィンガープリント。前回値との差分でホットリロードを判定する。
    std::uint64_t                            m_hlslSourceFingerprint = 0;

    /// @brief ディスク変更を検知したアセットの、最後に通知が来た時刻。
    /// @note 1 回の保存で通知は複数回届き途中状態を掴むこともあるため、静かになるまで待って 1 回だけ読み直す。
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
                 m_pendingAssetReloadAt;

    /// @brief 現在のシーンを読み込んだ / 保存した時点のファイル更新時刻。
    /// @brief 自分の書き込みで上がった時刻もここへ入れるので、これと食い違う = 他人が書いた。
    std::filesystem::file_time_type m_sceneDiskStamp{};
    bool m_sceneDiskStampValid = false;
    /// @brief 外部からの書き換えを検知した状態。通知バーがこれを見る。
    bool m_sceneDiskChanged    = false;
    /// @brief 検知した時刻。書き込み途中のファイルを開き直さないよう、少し置いてから動く。
    std::chrono::steady_clock::time_point m_sceneDiskChangedAt{};
    /// @brief 上書き確認モーダルの要求。SaveScene が立て、次フレームの先頭で開く。
    /// @note SaveScene は ModalDialog の onSave コールバックからも呼ばれ、そこで ModalDialog::Open* を呼ぶと実行中の std::function ごと state が壊れるため、開くのは必ずモーダル描画の外で行う。
    bool m_staleSaveConfirmPending = false;

    /// @brief オートセーブ / クラッシュ復旧の状態。
    float        m_autoSaveTimer = 0.0f;          ///< @note 前回オートセーブからの経過秒
    /// @brief 期限を過ぎたが操作中なので待っている。
    bool         m_autoSaveWaitingForIdle = false;
    bool         m_crashRecoveryChecked = false;  ///< @note 復旧プロンプトは起動後1回だけ
    std::string  m_pendingRecoveryAutoSave;       ///< @note 前回異常終了時に見つかったオートセーブのパス

    /// @brief コマンドパレット (Ctrl+K) の状態。
    std::vector<std::string> m_paletteAssetPaths;      ///< @note 開いた瞬間のアセット索引スナップショット
    bool                     m_commandPaletteOpen = false;   ///< @note 起動要求フラグ
    char                     m_commandPaletteQuery[256] = {}; ///< @note 検索クエリ入力バッファ
    int                      m_commandPaletteSel = 0;        ///< @note 選択中の候補行
    bool                     m_commandPaletteFocus = false;  ///< @note 開いた次フレームで検索欄へフォーカス

    /// @brief Prefab 編集モードの退避状態。編集前のシーンをテキストで丸ごと保持する。
    std::string              m_prefabEditStashedScene;
    std::string              m_prefabEditStashedScenePath;
    bool                     m_prefabEditStashedDirty = false;
    std::vector<std::string> m_prefabEditStashedSelection;   ///< @note GUID (EntityID は復元で変わる)
    std::string              m_prefabEditDiskPath;           ///< @note 保存先の絶対パス
    /// @brief この編集セッション中に 1 度でも保存したか。
    /// @note 「Save Prefab」→「Back to Scene」では閉じる操作自体は保存を伴わないが、これを見ないとシーンへ戻ったときにインスタンスへ反映されない。
    bool                     m_prefabEditSaved = false;

    /// @brief ショートカット一覧オーバーレイ (F1) と選択ヒストリ (Alt+←/→)。
    bool                         m_showShortcutsOverlay = false;
    std::vector<scene::EntityID> m_selectionHistory;          ///< @note 選択の往復履歴
    int                          m_selectionHistoryIndex = -1;
    bool                         m_selectionNavigating = false; ///< @note 履歴移動由来の選択は再記録しない
    scene::EntityID              m_lastRecordedSelection;       ///< @note 直近で履歴に積んだ選択 (既定=INVALID)

    HWND__*                                  m_hwnd          = nullptr;
    core::Window*                            m_window        = nullptr;
    renderer::IRenderer*                     m_renderer      = nullptr;
    renderer::IImGuiRenderer*                m_imguiRenderer = nullptr;
    renderer::ResourceManager*               m_resources     = nullptr;
    ViewportPanel*                           m_sceneViewportPanel     = nullptr;
    ViewportPanel*                           m_gameViewportPanel      = nullptr;
    ViewportPanel*                           m_uiViewportPanel        = nullptr;
    ProjectSettingsPanel*                    m_projectSettingsPanel   = nullptr;
    BuildSettingsPanel*                      m_buildSettingsPanel     = nullptr;
    /// @brief 1 枚目の Asset Browser (ファイル監視とインポートを担当する枚)。
    AssetBrowserPanel*                       m_assetBrowserPanel      = nullptr;
    /// @brief 開いている Asset Browser 全部。ルートの差し替えのように全枚に配る操作で使う。
    std::vector<AssetBrowserPanel*>          m_assetBrowserPanels;
    /// @brief StatusBar から Asset Browser と同じ操作感で開閉するため、専用ポインタで保持する。
    ConsolePanel*                            m_consolePanel           = nullptr;
    AnalysisPanel*                           m_analysisPanel          = nullptr;
    RenderPassViewerPanel*                   m_renderPassViewerPanel  = nullptr;
    MapEditorPanel*                          m_mapEditorPanel         = nullptr;
    IblBakePanel*                            m_iblBakePanel           = nullptr;
    VolumeFlipbookBakePanel*                 m_volumeFlipbookBakePanel = nullptr;
    FluidEditorPanel*                        m_fluidEditorPanel       = nullptr;
    AssetMaintenancePanel*                   m_assetMaintenancePanel  = nullptr;
    /// @brief 通知バーを閉じたときの件数。これより増えたらもう一度出す。
    size_t m_dismissedGuidConflicts = 0;
    bool   m_guidConflictBarDismissed = false;
    NavigationPanel*                         m_navigationPanel        = nullptr;
    AiSettingsPanel*                         m_aiSettingsPanel        = nullptr;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT;
    /// @brief このフレームで RT を作り直したか。OnRender の «出ているビューだけ描く» 判定へ渡す。
    /// @note 作り直した直後の RT は中身が未定義 (DX12 は解放待ち領域を使い回すため «少し前の絵» が残る)。描画を省いたフレームでパネルがそれを貼ると絵が重なって出る。
    bool m_sceneViewportRTRecreated = false;
    bool m_gameViewportRTRecreated  = false;
    /// @}
};

} /// @note namespace fbzz::editor
