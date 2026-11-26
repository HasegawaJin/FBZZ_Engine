/// @file    EditorApp.hpp
/// @brief   エディター全体のライフサイクルを管理する。
/// @author  Hasegawa Jin
/// @date    2026-05-21
#pragma once
#include <Editor/BuildPipeline/Compiler.hpp>
#include <Editor/EditorContext.hpp>
#include <Editor/Panels/IPanel.hpp>
#include <Editor/Panels/Workspace/StatusBar.hpp>
#include <Editor/Op/EditorOperator.hpp>
#include <Editor/Scene/Editing/UndoStack.hpp>
#include <Editor/Op/HotkeyManager.hpp>
#include <Editor/Diagnostics/Logging/ConsoleSink.hpp>
#include <Editor/Diagnostics/Logging/BuildConsole.hpp>
#include <Editor/Project/EditorSettings.hpp>
#include <Editor/BuildPipeline/PackageBuildService.hpp>
#include <Editor/Diagnostics/Performance/MemoryLeakDiff.hpp>
#include <Editor/Scene/Persistence/SceneDirtyTracker.hpp>
#include <Editor/Scene/Persistence/SceneIO.hpp>
#include <Editor/Play/PlayModeController.hpp>
#include <Editor/Play/StandalonePlaySession.hpp>
#include <Editor/Playtest/InputRecorder.hpp>
#include <Editor/Playtest/PlaytestRunner.hpp>
#include <Engine/Core/Platform/IModule.hpp>
#include <Engine/Rendering/DebugCamera.hpp>
#include <Graphics/Renderer/Resources/ResourceHandle.hpp>
#include <Engine/Scene/Components/Navigation/NavMeshSurfaceComponent.hpp>
#include <Engine/Scene/Runtime/ProjectRuntime.hpp>
#include <Engine/Rendering/UI/UISystem.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Math/LinearAlgebra/Vector3.hpp>
#include <Physics/Collision/Filtering/Layer.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct HWND__;
struct ImPlotContext;
namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; class ResourceManager; class SurfacePatchSelectionCacheOwner; }
namespace fbzz::scene { struct RenderFrameGeometryCache; }
namespace fbzz::core     { class Window; }
namespace fbzz::editor::ai { class EditorBusDispatcher; class NamedPipeServer; }

namespace fbzz::editor {

class ViewportPanel;
class BuildOutputPanel;
class HotkeyEditorPanel;
class ProjectSettingsPanel;
class BuildSettingsPanel;
class AssetBrowserPanel;
class ConsolePanel;
class PerformanceProfilerPanel;
class RenderPassViewerPanel;
class MapEditorPanel;
class IblBakePanel;
class VolumeFlipbookBakePanel;
class FluidEditorPanel;
class AssetMaintenancePanel;
class NavigationPanel;
class AiSettingsPanel;
class ManagedScriptEditor;
/// @note TerrainTool.hpp は imgui に依存するため前方宣言のみに留め、unique_ptr で所有して隠蔽する (EditorApp.hpp を imgui 非依存に保つ)。
class TerrainTool;

class EditorApp final : public core::IModule {
public:
    EditorApp();
    ~EditorApp(); ///< TerrainTool の完全型が見えるところ (EditorApp.cpp) で定義する

    /// @param[in] enableViewports false の場合は Scene / Game の描画と撮影を省く。
    /// @return 初期化済みなら false。
    bool Init(renderer::IRenderer& renderer, renderer::IImGuiRenderer& imguiRenderer,
              renderer::ResourceManager& resources, core::Window& window, bool enableViewports = true);
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
    scene::SceneManager& GetSceneManager() { return ActiveRuntime().GetSceneManager(); }
    scene::ProjectRuntime& GetProjectRuntime() { return ActiveRuntime(); }

    /// @brief Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    renderer::ResourceHandle<renderer::RenderTargetTag> GetViewportRT() const { return m_sceneViewportRT; }
    renderer::ResourceHandle<renderer::RenderTargetTag> GetGameViewportRT() const { return m_gameViewportRT; }
    /// @note UI Viewport は Game View の完成済み RT を共有し、編集ガイドだけを ImGui で重ねる。専用 RT にすると Clear 済み空 RT を表示する経路が再発しやすい。
    renderer::ResourceHandle<renderer::RenderTargetTag> GetUIViewportRT() const { return m_gameViewportRT; }

private:
    scene::ProjectRuntime& ActiveRuntime() { return m_playMode.PlayRuntime() ? *m_playMode.PlayRuntime() : m_runtime; }
    /// @name IModule ループが所有するステート

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
                        const std::string& cameraId,
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
    void StartPlayMode(bool shadowBakePrepared = false, bool allowPhysicalInput = true);
    void TickPendingShadowBakePlay();
    void StartStandalonePlayMode(bool allowPhysicalInput);
    void LaunchStandalonePlay(bool allowPhysicalInput);
    void TickStandalonePlay(float dt);
    void OpenStandalonePlayLog();
    void StopPlayMode();
    void TogglePlayMode(bool allowPhysicalInput = true);
    void RegisterDefaultHotkeys();


    /// @name Operator モデル (Docs/design/editor-operator-model.md)

    /// @brief メニュー・ホットキー・コマンドパレットは、いずれもここへ登録された操作の
    /// @brief 投影として描かれる。実行可能条件 (poll) を 1 箇所に持つことで、
    /// @brief 「メニューではグレーアウトなのにホットキーからは通る」を構造的に防ぐ。
    void RegisterBuiltinOperators();
    void RegisterWorkspaceOperators();
    /// @brief 設定ロードと直接APIのライブ反映を共有する。
    void ApplyEditorSettingsToContext(bool loadPanels);
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
    void CheckSceneDirtyForStandalone();
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
    bool SaveScene(bool background = false, std::function<void()> afterSave = {});
    bool SaveSceneAsDialog(bool background, std::function<void()> afterSave = {});
    bool SaveScenePath(const std::string& requestedPath, bool background = false, std::function<void()> afterSave = {});
    bool StartSceneSave(const std::string& path, bool backup, bool background, std::function<void()> afterSave = {});
    void PollSceneSave();
    bool FinishSceneSave(const std::string& path, bool backup, uint64_t editRevision,
                         std::size_t undoRevision, const SceneSaveResult& result);
    bool RejectWhileSceneSaving();
    void RemoveEditorHiding();   ///< Play/Save 前に editor-only 非表示を一時解除
    void RestoreEditorHiding();  ///< Play 復元/Save 後に editor-only 非表示を再適用
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
    void WriteSessionLock();       ///< 起動時: 生存セッションの印を書く
    void ClearSessionLock();       ///< 正常終了時: 印とオートセーブ中間物を掃除する
    /// @brief 未保存の変更がある間だけ時間を数え、間隔ごとに編集シーンを中間ファイルへ退避する。
    /// @note 直前 kAutoSaveWarningSec 秒は通知を出す。期限に操作中なら手を離すまで待つ (Unreal と同じ)。
    void TickAutoSave(float dt);
    /// @brief 今すぐ中間ファイルへ書き、タイマーを戻す。本体のシーンファイルと dirty 状態は変えない。
    bool AutoSaveNow();
    /// @brief オートセーブ直前のカウントダウン通知 (今すぐ保存 / 延期)。
    void DrawAutoSaveNotice();
    /// @brief 前回の印とオートセーブが残っていれば、復旧の提案を予約する。OpenProject がシーンを開いた後に呼ぶ。
    void DetectCrashRecovery();
    void ProcessCrashRecovery();   ///< 前回異常終了時にオートセーブ復元を促す

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


    /// @name 開いているシーンとディスクの食い違いを扱う

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

    EditorContext                        m_ctx; ///< パネルと操作が参照する現在の Editor 状態。
    std::unique_ptr<ImPlotContext, void (*)(ImPlotContext*)> m_implotContext; ///< 起動時に所有するプロット用 UI コンテキスト。初期化の途中で終了しても解放する。
    std::vector<std::unique_ptr<IPanel>> m_panels; ///< 毎フレーム更新・描画する Editor パネルの所有コンテナ。
    std::unique_ptr<StatusBar>           m_statusBar; ///< Editor ウィンドウ下部の操作と状態表示を担当する、所有するバー。

    UndoStack          m_undoStack; ///< 現在の編集操作を取り消し・再適用するための履歴。
    HotkeyManager      m_hotkeys; ///< 編集中のパネルやモードに応じてショートカットを解決する。
    HotkeyEditorPanel* m_hotkeyEditorPanel = nullptr; ///< ショートカット設定の表示を制御するパネル。実体は m_panels が所有する。
    OperatorRegistry   m_operators; ///< メニュー、キー、パレット、AI バスから共通で呼ぶ編集操作の登録簿。
    ConsoleSink        m_consoleSink; ///< パネルから参照できるログを受け取る出力先。
    EditorSettings     m_settings; ///< パネル配置やカメラなど Editor 固有の永続設定。
    PackageBuildService m_packageBuild; ///< 配布ビルドの実行と結果を管理するサービス。
    SceneDirtyTracker  m_dirtyTracker; ///< シーンの保存時点と現在の編集状態を比較する。
    std::string        m_projectRoot; ///< 現在開いているプロジェクトのルートパス。
    std::string        m_projectSettingsPath; ///< 現在のプロジェクト設定の保存先。
    std::string        m_projectSettingsSavedToml; ///< 最後にディスクと一致していた ProjectSettings の TOML。
    std::string        m_projectSettingsLastToml; ///< 前回ポーリング時の TOML。値が動き続けている間は保存を待つ。
    std::string        m_projectSettingsFailedToml; ///< 書き出しに失敗した TOML。同じ内容のままなら自動では再試行しない。
    float              m_projectSettingsPollTimer = 0.0f; ///< プロジェクト設定を次に確認するまでの周期管理 [s]。
    float              m_projectSettingsIdleTime  = 0.0f; ///< プロジェクト設定の変更が止まってからの経過時間 [s]。
    uint64_t           m_sceneEditRevision = 0; ///< 非同期保存開始後に編集が加わったかを区別する番号。
    struct PendingSceneSave {
        std::future<SceneSaveResult> result; ///< シーンファイルを書き出す非同期処理の結果。
        std::string path; ///< 開始時に控えたシーンの保存先。
        std::function<void()> afterSave; ///< 保存が終わってから続ける操作。無ければ空。
        uint64_t editRevision = 0; ///< 保存した内容を採取した時点のシーン編集リビジョン。
        std::size_t undoRevision = 0; ///< 保存した内容を採取した時点の Undo 履歴のリビジョン。
        bool backup = false; ///< 中間退避用のバックアップ保存か。
    };
    PendingSceneSave   m_pendingSceneSave; ///< 進行中のシーン保存と、完了時の編集状態照合に使うスナップショット。
    bool               m_titleInitialized = false; ///< タイトルの初回表示を済ませたか。
    bool               m_lastTitleDirty = false; ///< 最後にタイトルへ反映した未保存マークの状態。
    std::string        m_lastTitleScenePath; ///< 最後にウィンドウタイトルへ反映したシーンパス。
    PlayModeController              m_playMode; ///< 編集シーンとエディター内 Play シーンの切り替えと寿命を管理する。
    StandalonePlaySession           m_standalonePlay; ///< 別プロセスで動く Standalone Play の起動・終了・ログを管理する。
    /// @brief Play→Stop 時の再ベイクを避けるため、Play 開始前にベイク済み NavMesh をキャッシュする (key = GameObject::instanceId)。
    /// @note 統計とソースハッシュも戻さないと、Stop した瞬間に Navigation パネルが「ベイク後にソースが変わった」と誤検知する。
    struct NavMeshPlayCacheEntry {
        scene::NavMesh              navMesh; ///< Play 前に編集シーンで有効だったベイク済みメッシュ。
        scene::NavMeshBakeStats     stats; ///< ベイク結果に対応し、Navigation パネルで表示する統計。
        scene::NavMeshBakeDebugGrid debug; ///< ベイク結果に対応する診断用グリッド。
        uint64_t                    sourceHash = 0; ///< 復元したメッシュが対応するベイク入力のハッシュ。
    };
    std::unordered_map<std::string, NavMeshPlayCacheEntry> m_navMeshPlayCache; ///< Play から戻す編集側 NavMesh の控え。キーは GameObject の instanceId。
    renderer::RenderSettings m_playRenderSettings; ///< Play 開始時の解決済み設定から作る実行用コピー。編集側の自動保存とは独立する。
    uint64_t m_scriptQuitSceneLifetime = 0; ///< Script.App.Quit intent を次 frame へ渡す。Scene が先に変われば要求を捨てる。
    uint64_t m_pendingShadowBakePlayScene = 0; ///< 影のベイク完了を待っている編集シーンの世代番号。待機していなければ 0。
    bool m_pendingShadowBakeAllowPhysicalInput = true; ///< 影のベイク完了待ち後に開始する Play の実機入力ポリシー。
    bool m_playAllowsPhysicalInput = true; ///< 現在の Play セッションの実機入力を許可するか。
    bool m_playInputPolicyApplied = false; ///< Play 用入力ポリシーを適用し、停止時に復元する必要があるか。
    bool m_playPreviousPhysicalInputAllowed = true; ///< Play 開始直前の実機入力ポリシー。終了時に戻す。
    MemoryLeakDiff     m_memoryLeakDiff; ///< Play 開始時に取った Renderer リソースの基準。Stop 後の復元が落ち着いてから比べる。
    std::unique_ptr<FluidBakeService> m_fluidBake; ///< パネルが閉じている間も続行する流体ベイクとプレビューの窓口。m_ctx.fluidBake が参照する。
    std::unique_ptr<TerrainTool>    m_terrainTool; ///< pimpl: EditorApp.hpp が imgui に依存しないよう unique_ptr で隠蔽
    std::string                     m_normalLayoutIni; ///< Play 用のレイアウトに切り替える前の通常レイアウト。
    const char*                     m_normalIniFilename = nullptr; ///< Play へ切り替える前の ImGui 設定ファイル名。文字列は借用する。
    std::string                     m_imguiIniPath;   ///< io.IniFilename が指すパス (文字列寿命を保持)
    std::vector<bool>               m_normalPanelVisibility; ///< Play に切り替える前の m_panels の表示状態を並び順で控える。
    std::string                     m_playLayoutIni; ///< Play 用のレイアウトを復元するために保持する ImGui 設定。
    const char*                     m_playIniFilename = nullptr; ///< Play 中のレイアウトに用いる ImGui 設定ファイル名。文字列は借用する。
    std::vector<bool>               m_playPanelVisibility; ///< Play レイアウトの m_panels の表示状態を並び順で控える。
    bool                            m_playViewportLayoutActive = false; ///< 通常の編集レイアウトから Play 向けの配置へ切り替え済みか。
    bool                            m_playCursorApplied  = false; ///< Play 開始時のカーソル初期化を済ませたか
    bool                            m_playCursorReleased = false; ///< Escape で一時解放したか (2 回目で Stop)
    uint8_t                         m_playCursorAcquireAttempts = 0; ///< Play 中のマウス取得を試行した回数。
    bool                            m_terrainToolWasActive = false; ///< Map Editor を出るときに Terrain Tool を戻すための作動状態の控え。
    int                             m_terrainToolModeBeforeMap = 0; ///< Map Editor へ移る前に Terrain Tool で選択していたモード。

    /// @note m_ctx.buildConsole がこれを指し、Build Output パネル・StatusBar・通知バーが共有する。
    BuildConsole             m_buildConsole; ///< 各種コンパイルの出力、診断、結果をパネルへ配る。
    BuildOutputPanel*        m_buildOutputPanel = nullptr; ///< ビルド結果の表示を制御するパネル。実体は m_panels が所有する。

    std::unique_ptr<ai::EditorBusDispatcher> m_aiDispatcher; ///< AI バスの要求を現在のシーンと操作登録簿へ振り分ける。
    std::unique_ptr<ai::NamedPipeServer>      m_aiPipeServer; ///< AI バスとの名前付きパイプ接続を所有する。

    std::unique_ptr<ai::EditorBusDispatcher> m_playtestDispatcher; ///< Playtest の手順が叩く専用のバス。AI のパイプを閉じていても (バッチ実行でも) 回せるよう別に持つ。
    playtest::PlaytestRunner                 m_playtest; ///< 現在の Playtest シナリオと各手順の進行状態。
    playtest::InputRecorder                  m_inputRecorder; ///< Playtest に再利用する入力イベントの記録器。
    BatchOptions                             m_batch; ///< コマンドラインから指定したシナリオと出力設定。
    bool                                     m_batchStarted = false; ///< バッチシナリオの開始処理を済ませたか。
    int                                      m_batchExitCode = 0; ///< バッチ終了時にランチャーへ返す終了コード。
    /// @brief シナリオを 1 フレーム進める。OnInputPolled から呼ぶ。
    void TickPlaytest();

    std::unique_ptr<ManagedScriptEditor> m_managedScripts; ///< C# 編集、コンパイルと Editor への公開を管理する。
    /// @brief HLSL ホットリロード
    std::filesystem::path    m_hlslSourceDir;       ///< プロジェクト側の Assets/Shaders。監視不可なら空。
    std::filesystem::path    m_compileShadersScript; ///< プロジェクトまたは SDK の compile_shaders.ps1。
    Compiler                 m_hlslCompiler; ///< HLSL ビルドの子プロセスと出力を管理する。
    bool                     m_hlslCompilePending = false; ///< 変更検知からコンパイル開始までの間に保留している HLSL 更新。
    float                    m_hlslDebounceTimer  = 0.0f; ///< 連続する HLSL 変更をまとめるための待機時間 [s]。
    float                    m_hlslDirtyPollTimer = 0.0f; ///< HLSL ツリー監視を毎フレーム走らせないための間隔管理
    std::future<std::uint64_t> m_hlslSourceScan; ///< 走査は最大 1 件。worker はパスのコピーだけを読み、Editor の状態には触れない。

    std::uint64_t                            m_lastSceneWriteTime = 0; ///< シーン自身の書き込み通知を外部変更と区別する時刻の控え。
    std::uint64_t                            m_hlslSourceFingerprint = 0; ///< HLSL ソースツリーの内容フィンガープリント。前回値との差分でホットリロードを判定する。

    /// @brief ディスク変更を検知したアセットの、最後に通知が来た時刻。
    /// @note 1 回の保存で通知は複数回届き途中状態を掴むこともあるため、静かになるまで待って 1 回だけ読み直す。
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
                 m_pendingAssetReloadAt;

    std::filesystem::file_time_type m_sceneDiskStamp{}; ///< シーンを開いたか保存した時点の更新時刻。現在の時刻と違えば外部書き込みを検知する。
    bool m_sceneDiskStampValid = false; ///< シーンファイルの照合に使える更新時刻を取得済みか。
    bool m_sceneDiskChanged    = false; ///< 外部からの書き換えを検知した状態。通知バーがこれを見る。
    std::chrono::steady_clock::time_point m_sceneDiskChangedAt{}; ///< 検知した時刻。書き込み途中のファイルを開き直さないよう、少し置いてから動く。
    /// @note SaveScene は ModalDialog の onSave コールバックからも呼ばれ、そこで ModalDialog::Open* を呼ぶと実行中の std::function ごと state が壊れるため、開くのは必ずモーダル描画の外で行う。
    bool m_staleSaveConfirmPending = false; ///< 上書き確認を次フレームのモーダル外で開くための保留フラグ。

    /// @brief オートセーブ / クラッシュ復旧の状態。
    float        m_autoSaveTimer = 0.0f;          ///< 前回オートセーブからの経過秒
    bool         m_autoSaveWaitingForIdle = false; ///< 期限を過ぎたが操作中なので待っている。
    bool         m_crashRecoveryChecked = false;  ///< 復旧プロンプトは起動後1回だけ
    std::string  m_pendingRecoveryAutoSave;       ///< 前回異常終了時に見つかったオートセーブのパス

    /// @brief コマンドパレット (Ctrl+K) の状態。
    std::vector<std::string> m_paletteAssetPaths;      ///< 開いた瞬間のアセット索引スナップショット
    bool                     m_commandPaletteOpen = false;   ///< 起動要求フラグ
    char                     m_commandPaletteQuery[256] = {}; ///< 検索クエリ入力バッファ
    int                      m_commandPaletteSel = 0;        ///< 選択中の候補行
    bool                     m_commandPaletteFocus = false;  ///< 開いた次フレームで検索欄へフォーカス

    std::string              m_prefabEditStashedScene; ///< Prefab 編集モードの退避状態。編集前のシーンをテキストで丸ごと保持する。
    std::string              m_prefabEditStashedScenePath; ///< Prefab 編集から戻るときに復元する、元のシーンパス。
    bool                     m_prefabEditStashedDirty = false; ///< Prefab 編集に切り替える直前のシーンの未保存状態。
    std::vector<std::string> m_prefabEditStashedSelection;   ///< GUID (EntityID は復元で変わる)
    std::string              m_prefabEditDiskPath;           ///< 保存先の絶対パス
    /// @note 「Save Prefab」→「Back to Scene」では閉じる操作自体は保存を伴わないが、これを見ないとシーンへ戻ったときにインスタンスへ反映されない。
    bool                     m_prefabEditSaved = false; ///< 現在の Prefab 編集セッションで一度でも保存したか。

    bool                         m_showShortcutsOverlay = false; ///< ショートカット一覧オーバーレイ (F1) と選択ヒストリ (Alt+←/→)。
    std::vector<scene::EntityID> m_selectionHistory;          ///< 選択の往復履歴
    int                          m_selectionHistoryIndex = -1; ///< 選択の往復履歴 m_selectionHistory の現在位置。履歴が空なら -1。
    bool                         m_selectionNavigating = false; ///< 履歴移動由来の選択は再記録しない
    scene::EntityID              m_lastRecordedSelection;       ///< 直近で履歴に積んだ選択 (既定=INVALID)

    HWND__*                                  m_hwnd          = nullptr; ///< 起動元ウィンドウの借用 Win32 ハンドル。
    core::Window*                            m_window        = nullptr; ///< ウィンドウの表示や復旧状態を参照する借用ウィンドウ。
    renderer::IRenderer*                     m_renderer      = nullptr; ///< 起動元が所有するフレーム描画インターフェース。
    renderer::IImGuiRenderer*                m_imguiRenderer = nullptr; ///< 起動元が所有する ImGui 描画インターフェース。
    renderer::ResourceManager*               m_resources     = nullptr; ///< 起動元が所有する描画・アセット用リソース管理器。
    std::shared_ptr<renderer::SurfacePatchSelectionCacheOwner> m_surfacePatchPlans; ///< Pure bounded Scene/Game Water plans; RenderSystem pins the owner across recording callbacks.
    ViewportPanel*                           m_sceneViewportPanel     = nullptr;
    ViewportPanel*                           m_gameViewportPanel      = nullptr;
    ViewportPanel*                           m_uiViewportPanel        = nullptr;
    ProjectSettingsPanel*                    m_projectSettingsPanel   = nullptr;
    BuildSettingsPanel*                      m_buildSettingsPanel     = nullptr;
    AssetBrowserPanel*                       m_assetBrowserPanel      = nullptr; ///< 1 枚目の Asset Browser (ファイル監視とインポートを担当する枚)。
    std::vector<AssetBrowserPanel*>          m_assetBrowserPanels; ///< 開いている Asset Browser 全部。ルートの差し替えのように全枚に配る操作で使う。
    ConsolePanel*                            m_consolePanel           = nullptr; ///< StatusBar から Asset Browser と同じ操作感で開閉するため、専用ポインタで保持する。
    PerformanceProfilerPanel*                m_analysisPanel          = nullptr;
    RenderPassViewerPanel*                   m_renderPassViewerPanel  = nullptr;
    MapEditorPanel*                          m_mapEditorPanel         = nullptr;
    IblBakePanel*                            m_iblBakePanel           = nullptr;
    VolumeFlipbookBakePanel*                 m_volumeFlipbookBakePanel = nullptr;
    FluidEditorPanel*                        m_fluidEditorPanel       = nullptr;
    AssetMaintenancePanel*                   m_assetMaintenancePanel  = nullptr;
    size_t m_dismissedGuidConflicts = 0; ///< 通知バーを閉じたときの件数。これより増えたらもう一度出す。
    bool   m_guidConflictBarDismissed = false;
    NavigationPanel*                         m_navigationPanel        = nullptr;
    AiSettingsPanel*                         m_aiSettingsPanel        = nullptr;
    bool m_viewportsEnabled = true; ///< Scene View と Game View の生成・描画を有効にするか。
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT; ///< Scene View の描画先をリソース管理器で解決するためのハンドル。
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT; ///< Game View の描画先をリソース管理器で解決するためのハンドル。
    /// @brief このフレームで RT を作り直したか。OnRender の «出ているビューだけ描く» 判定へ渡す。
    /// @note 作り直した直後の RT は中身が未定義 (DX12 は解放待ち領域を使い回すため «少し前の絵» が残る)。描画を省いたフレームでパネルがそれを貼ると絵が重なって出る。
    bool m_sceneViewportRTRecreated = false; ///< このフレーム中に Scene View の描画先を再作成したか。
    bool m_gameViewportRTRecreated  = false; ///< このフレーム中に Game View の描画先を再作成したか。

};

} // namespace fbzz::editor
