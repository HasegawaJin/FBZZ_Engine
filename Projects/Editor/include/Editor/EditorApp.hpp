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
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <Windows.h>

namespace fbzz::renderer { class IImGuiRenderer; class IRenderer; class ResourceManager; }
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
class MapEditorPanel;
class IblBakePanel;
class VolumeFlipbookBakePanel;
class FluidEditorPanel;
class AssetMaintenancePanel;
class NavigationPanel;
class AiSettingsPanel;
class TerrainTool;

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

    std::unique_ptr<scene::Scene>  m_scene;
    scene::ProjectRuntime          m_runtime;
    renderer::DebugCamera          m_debugCamera;
    scene::UISystemContext         m_sceneUICtx;
    FocusAnim                      m_focusAnim;
    float                          m_simulationDt = 0.0f;

    void BuildMenuBar(EditorContext& ctx);
    void InstallNativeMenuBar();
    void BuildPlayToolbar(EditorContext& ctx);
    // Help > About。メニューの中で Begin すると閉じた瞬間に消えるので、毎フレーム別に描く。
    void DrawAboutDialog();
    bool m_aboutRequested = false;
    // ビルド失敗時に、ツールバー下へ消えない通知バーを描画する (Show / Dismiss)。
    void DrawBuildNotificationBar(EditorContext& ctx);
    // GUID 重複を抱えているとき、同じ場所へ通知バーを描画する。
    // WHY 画面に出すか: 重複はスキャン中の Console エラーでしか報告されず、
    //     起動ログに流れて誰も気づかないまま参照が別のアセットへ吸われ続けていた。
    void DrawGuidConflictBar(EditorContext& ctx);
    void ProcessMapEditingModeTransition(uint32_t dockId);
    void ProcessPlayViewportLayoutTransition(uint32_t dockId);
    void EnterMapEditingMode(uint32_t dockId);
    void ExitMapEditingMode(uint32_t dockId);
    void BuildMapEditingLayout(uint32_t dockId);
    void EnterPlayViewportLayout(uint32_t dockId);
    void ExitPlayViewportLayout(uint32_t dockId);
    void BuildPlayViewportLayout(uint32_t dockId);
    // Play 中の OS カーソル (拘束範囲・方針の適用と Escape の解放)。
    void UpdatePlayCursorControls();
    // Play 開始/停止/トグル。ツールバーのボタンと Ctrl+P ホットキーの共通経路。
    void StartPlayMode();
    void StopPlayMode();
    void TogglePlayMode();
    void RegisterDefaultHotkeys();

    // ── Operator モデル (Docs/design/editor-operator-model.md) ───────────────
    // メニュー・ホットキー・コマンドパレットは、いずれもここへ登録された操作の
    // 投影として描かれる。実行可能条件 (poll) を 1 箇所に持つことで、
    // 「メニューではグレーアウトなのにホットキーからは通る」を構造的に防ぐ。
    void RegisterBuiltinOperators();
    // パネル表示 / UI スケール / Prefab 編集モードの操作 (src/Op/PanelOperators.cpp)。
    // m_panels を参照するため EditorApp のメンバー関数として登録する。
    void RegisterPanelOperators();
    // ウィンドウ名または View メニュー名 (大小無視の完全一致) でパネルを引く。
    [[nodiscard]] IPanel* FindPanelByName(const std::string& name) const;

    // パネル側の設定を回収して editor_settings.toml へ即時書き出す。
    // WHY Shutdown と別にあるか: ビルド構成のように、エディターが落ちても
    //     残っていてほしい設定がある。Shutdown 側はここに加えてカメラ等も回収する。
    void PersistEditorSettings();

    // 通常 Workspace でのパネル開閉状態を EditorSettings へ採る / 戻す。
    // WHY 専用経路か: Map Mode と Play Maximized は visible を一時的に潰すため、
    //     そのまま保存すると「次に開いたらパネルが 1 枚しか無い」状態が焼き付く。
    void CaptureNormalPanelVisibility();
    void RestorePanelVisibility();
    // panel.focus を「既に手元にあるパネルポインタ」から呼ぶ小さな包み。
    // WHY: メニュー側は m_iblBakePanel のような具体ポインタを持っているので、
    //      名前引きへ戻さずに済ませたい。しかし実体は operator を通す
    //      (人が押した経路と AI の経路で同じ処理になる)。panel が null なら何もしない。
    void InvokePanelFocus(IPanel* panel);
    // 1 つの operator をメニュー項目として描く。
    // 表示名・実行可否・ショートカット表示をすべてレジストリと HotkeyManager から引く。
    // labelOverride は文脈で名前が変わる項目 (Prefab 編集中の "Save Prefab") 用。
    bool MenuItemOp(const char* operatorId, const char* labelOverride = nullptr);
    // 引数付きで呼ぶメニュー項目。ラジオ表示 (View Mode の 4 つ) と、
    // 対象を指定するトグル (パネルの表示切替) がこれを使う。
    // チェックマークは operator の checked を同じ args で評価した値になる。
    bool MenuItemOpArgs(const char* operatorId, const OpArgs& args,
                        const char* labelOverride = nullptr);
    // 描画モード切り替えを operator へ渡す (ネイティブメニューの 4 項目が使う)。
    void InvokeViewMode(const char* mode);
    // 4 面が共有する実行文脈を組み立てる (生成箇所を 1 つに保つ)。
    [[nodiscard]] OpContext MakeOpContext();
    [[nodiscard]] bool      CanInvokeOperator(std::string_view id, const OpArgs& args = {});
    OpResult                InvokeOperator(std::string_view id, const OpArgs& args = {});
    void ResizeViewportRTsIfNeeded();
    void CheckHotReload();
    void CacheSceneWriteTime();

    // スクリプト DLL ホットリロード
    void InitScriptDll();
    void CheckScriptDirtyAndRebuild();
    void TickScriptCompile();

    // HLSL ホットリロード
    /// 監視先のシェーダーツリーを決め、プロジェクトの持ち物かどうかを判定する。
    /// 共有 SDK 側へ解決された場合は監視も再コンパイルも行わない。
    void InitHlslHotReload();
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
    // Play 中のシーン読み書き要求を断る (断ったら true)。理由は Console と Toast へ。
    bool RejectSceneIOWhilePlaying(const char* action);

    // Hierarchy の非表示 / ロックを Scene サイドカー (<scene>.meta) と往復させる。
    // WHY ctx が正本のままか: 非表示とロックはセッション中ずっと書き換わる。
    //     EditorSceneState を常時同期させると、Undo や Play の内部スナップショット
    //     (SceneIO::Serialize) が走るたびに巻き添えで書き戻ることになる。
    //     ユーザーが「開く」「保存する」と言った瞬間だけ、明示的に橋渡しする。
    void CaptureEditorViewStateToSceneMeta();
    void ApplyEditorViewStateFromSceneMeta();

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
    // ディスク上で書き換わったアセットを、実行中のキャッシュへ読み直す。
    void ProcessAssetDiskReloads();

    // ── 開いているシーンとディスクの食い違いを扱う ───────────────────────────
    // WHY: AI・外部エディタ・git がシーンファイルを直接書く前提に立つと、
    //      「読み込んだ後にファイルが変わったか」を知らないまま保存する経路が
    //      そのまま他人の編集を踏み潰す事故になる。読み込み・保存の両方で
    //      同じタイムスタンプを見て判断する。
    void ProcessSceneDiskReload();
    void DrawSceneReloadBar(EditorContext& ctx);
    // 現在のシーンパスの更新時刻を控える。開いた直後と保存直後に呼ぶ。
    void CaptureSceneDiskStamp();
    // 控えた時刻とディスクの現在値が食い違うか (= 自分以外が書いたか)。
    [[nodiscard]] bool IsSceneStaleOnDisk() const;
    // ディスクから読み直す。未保存の変更は失われる。
    bool ReloadSceneFromDisk();
    // 読み直したシーンファイルの中で、解決できない guid 参照を Console へ報告する。
    void ReportUnresolvedSceneRefs(const std::string& path) const;
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
    // AI 要求 1 行を処理する。
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
    float              m_dirtyPollTimer = 0.0f;
    bool               m_titleInitialized = false;
    bool               m_lastTitleDirty = false;
    std::string        m_lastTitleScenePath;
    PlayModeController              m_playMode;
    // Play→Stop 時の再ベイクを避けるため、Play 開始前にベイク済み NavMesh をキャッシュする。
    // key = GameObject::instanceId
    // WHY 診断データまで持つか: 統計とソースハッシュを戻さないと、Stop した瞬間に
    //     Navigation パネルが「ベイク後にソースが変わった」と言い出す (何も変えていないのに)。
    struct NavMeshPlayCacheEntry {
        scene::NavMesh              navMesh;
        scene::NavMeshBakeStats     stats;
        scene::NavMeshBakeDebugGrid debug;
        uint64_t                    sourceHash = 0;
    };
    std::unordered_map<std::string, NavMeshPlayCacheEntry> m_navMeshPlayCache;
    // Play 開始時の描画設定。Play 中のスクリプトが graphics プロキシで画質や明るさを
    // 変えても、Stop でここへ戻して編集側とプロジェクト設定を汚さない。
    renderer::RenderSettings m_renderSettingsPlaySnapshot;
    // Play 開始時に取った Renderer リソースの基準。Stop 後の復元が落ち着いてから比べる。
    MemoryLeakDiff     m_memoryLeakDiff;
    // .fluid の焼き・プレビューのジョブ窓口 (m_ctx.fluidBake がこれを指す)。
    // パネルではなくここが持つので、パネルを閉じても AI から頼んでも焼きが進む。
    std::unique_ptr<FluidBakeService> m_fluidBake;
    std::unique_ptr<TerrainTool>    m_terrainTool; // pimpl: EditorApp.hpp が imgui に依存しないよう unique_ptr で隠蔽
    std::string                     m_normalLayoutIni;
    const char*                     m_normalIniFilename = nullptr;
    std::string                     m_imguiIniPath;   // io.IniFilename が指すパス (文字列寿命を保持)
    std::vector<bool>               m_normalPanelVisibility;
    std::string                     m_playLayoutIni;
    const char*                     m_playIniFilename = nullptr;
    std::vector<bool>               m_playPanelVisibility;
    bool                            m_playViewportLayoutActive = false;
    bool                            m_playCursorApplied  = false; // Play 開始時のカーソル初期化を済ませたか
    bool                            m_playCursorReleased = false; // Escape で一時解放したか (2 回目で Stop)
    bool                            m_terrainToolWasActive = false;
    int                             m_terrainToolModeBeforeMap = 0;

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
    // SDK 解決失敗による CMakeCache 再構成をこのセッションで実施済みか。
    // WHY: SDK が本当に存在しない場合に configure 失敗 → 再ビルド → 同じ失敗を繰り返さないよう 1 回に制限する。
    bool                     m_scriptSdkRecoveryDone = false;

    // HLSL ホットリロード
    std::filesystem::path    m_hlslSourceDir;      // Assets/shaders/ ディレクトリ
    std::filesystem::path    m_compileShadersScript; // compile_shaders.bat パス
    /// m_hlslSourceDir が「開いているプロジェクトの持ち物」か。
    /// 共有 SDK の shader へ解決されたときは false になり、監視も再コンパイルも行わない
    /// (実行中の Editor が SDK を書き換えると、その SDK を使う他プロジェクトまで巻き込む)。
    bool                     m_hlslProjectOwned = false;
    FILETIME                 m_lastHlslWriteTime = {}; // HLSL ツリー内で最も新しい更新時刻
    Compiler                 m_hlslCompiler;
    bool                     m_hlslCompilePending = false;
    float                    m_hlslDebounceTimer  = 0.0f;
    float                    m_hlslDirtyPollTimer = 0.0f; // HLSL ツリー監視を毎フレーム走らせないための間隔管理

    FILETIME                                 m_lastSceneWriteTime = {};
    // HLSL ソースツリーの内容フィンガープリント。前回値との差分でホットリロードを判定する。
    std::uint64_t                            m_hlslSourceFingerprint = 0;

    // ディスク変更を検知したアセットの、最後に通知が来た時刻。
    // WHY 即座に読み直さないか: 1 回の保存で通知は複数回届き、しかも途中の状態を
    //      掴むことがある。静かになるまで待ってから 1 回だけ読み直す。
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
                 m_pendingAssetReloadAt;

    // 現在のシーンを読み込んだ / 保存した時点のファイル更新時刻。
    // 自分の書き込みで上がった時刻もここへ入れるので、これと食い違う = 他人が書いた。
    std::filesystem::file_time_type m_sceneDiskStamp{};
    bool m_sceneDiskStampValid = false;
    // 外部からの書き換えを検知した状態。通知バーがこれを見る。
    bool m_sceneDiskChanged    = false;
    // 検知した時刻。書き込み途中のファイルを開き直さないよう、少し置いてから動く。
    std::chrono::steady_clock::time_point m_sceneDiskChangedAt{};
    // 上書き確認モーダルの要求。SaveScene が立て、次フレームの先頭で開く。
    // WHY 直接開かないか: SaveScene は ModalDialog の onSave コールバックからも呼ばれる。
    //      そこで ModalDialog::Open* を呼ぶと、実行中の std::function ごと state が
    //      作り直されて未定義動作になる。開くのは必ずモーダル描画の外から行う。
    bool m_staleSaveConfirmPending = false;

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
    ProjectSettingsPanel*                    m_projectSettingsPanel   = nullptr;
    BuildSettingsPanel*                      m_buildSettingsPanel     = nullptr;
    // 1 枚目の Asset Browser (ファイル監視とインポートを担当する枚)。
    AssetBrowserPanel*                       m_assetBrowserPanel      = nullptr;
    // 開いている Asset Browser 全部。ルートの差し替えのように全枚に配る操作で使う。
    std::vector<AssetBrowserPanel*>          m_assetBrowserPanels;
    // StatusBar から Asset Browser と同じ操作感で開閉するため、専用ポインタで保持する。
    ConsolePanel*                            m_consolePanel           = nullptr;
    AnalysisPanel*                           m_analysisPanel          = nullptr;
    MapEditorPanel*                          m_mapEditorPanel         = nullptr;
    IblBakePanel*                            m_iblBakePanel           = nullptr;
    VolumeFlipbookBakePanel*                 m_volumeFlipbookBakePanel = nullptr;
    FluidEditorPanel*                        m_fluidEditorPanel       = nullptr;
    AssetMaintenancePanel*                   m_assetMaintenancePanel  = nullptr;
    // 通知バーを閉じたときの件数。これより増えたらもう一度出す。
    size_t m_dismissedGuidConflicts = 0;
    bool   m_guidConflictBarDismissed = false;
    NavigationPanel*                         m_navigationPanel        = nullptr;
    AiSettingsPanel*                         m_aiSettingsPanel        = nullptr;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_sceneViewportRT;
    renderer::ResourceHandle<renderer::RenderTargetTag> m_gameViewportRT;
};

} // namespace fbzz::editor
