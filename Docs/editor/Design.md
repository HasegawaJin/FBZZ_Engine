# Editor System 設計書

`fbzz::editor` — ImGui ベースのランタイムエディター。  
**設計方針: AI-native。TOML + AI でシーンを記述し、Viewport で確認、Inspector で微調整する。**

---

## 目的と方針

| 目的 | 詳細 |
|------|------|
| マップ編集 | Gizmo + MousePicking で 3D 空間に直接オブジェクトを配置できる |
| AI との協業 | TOML シーンを AI が生成 → Hot Reload で即反映 → Viewport で確認 |
| Play モード | エディター内でゲームループを実行・停止できる |
| DX12 移行耐性 | ImGui バックエンド切り替えを `IRenderer` に閉じ込め、editor コードは DX 世代に依存しない |

### 削ぎ落とした機能（理由）

| 削除 | 理由 |
|------|------|
| AssetRegistry / Thumbnail | OS のファイルエクスプローラーで代替 |
| DragDrop (Asset → Inspector) | TOML でパスを書けば AI が管理できる |
| CommandPalette | 不要 |
| Prefab (複雑版) | TOML ファイルそのもので代替 |
| LayoutManager | 不要 |

---

## モジュール配置

```
FBZZ_Engine/
├── engine/
│   ├── include/engine/
│   │   ├── Core/
│   │   │   └── ILogSink.hpp          (NEW)
│   │   └── Util/
│   │       ├── FileSystem.hpp        (NEW)
│   │       └── StringUtils.hpp       (NEW)
│   └── src/Util/
│       ├── FileSystem.cpp            (NEW)
│       └── StringUtils.cpp           (NEW)
├── editor/
│   ├── CMakeLists.txt
│   ├── include/editor/
│   │   ├── EditorApp.hpp
│   │   ├── EditorContext.hpp
│   │   ├── PlayModeController.hpp    (NEW)
│   │   ├── Panels/
│   │   │   ├── IPanel.hpp
│   │   │   ├── SceneHierarchyPanel.hpp
│   │   │   ├── InspectorPanel.hpp
│   │   │   ├── ViewportPanel.hpp
│   │   │   ├── LightPanel.hpp
│   │   │   ├── ConsolePanel.hpp      (NEW)
│   │   │   ├── AssetBrowserPanel.hpp (NEW)
│   │   │   └── StatusBar.hpp         (NEW)
│   │   └── Util/
│   │       ├── UndoStack.hpp         (NEW)
│   │       ├── SceneSerializer.hpp   (NEW)
│   │       ├── EditorSettings.hpp    (NEW)
│   │       ├── FileDialog.hpp        (NEW)
│   │       ├── HotkeyManager.hpp     (NEW)
│   │       ├── ImGuiWidgets.hpp      (NEW)
│   │       ├── MathConvert.hpp       (NEW)
│   │       ├── ConsoleSink.hpp       (NEW)
│   │       └── ModalDialog.hpp       (NEW)
│   └── src/
│       ├── EditorApp.cpp
│       ├── PlayModeController.cpp    (NEW)
│       ├── Panels/
│       │   ├── SceneHierarchyPanel.cpp
│       │   ├── InspectorPanel.cpp
│       │   ├── ViewportPanel.cpp
│       │   ├── LightPanel.cpp
│       │   ├── ConsolePanel.cpp      (NEW)
│       │   ├── AssetBrowserPanel.cpp (NEW)
│       │   └── StatusBar.cpp         (NEW)
│       └── Util/
│           ├── UndoStack.cpp
│           ├── SceneSerializer.cpp
│           ├── EditorSettings.cpp
│           ├── FileDialog.cpp
│           ├── HotkeyManager.cpp
│           ├── ConsoleSink.cpp
│           └── ModalDialog.cpp
└── third_party/
    ├── ImGui/       (docking ブランチ)
    └── TomlPlusPlus/
```

依存方向 (逆転禁止):

```
editor → engine → physics → math
```

---

## ImGui 統合戦略 — IRenderer 経由

ImGui バックエンドを `IRenderer` の仮想メソッドとして隠蔽する。  
`editor` コードは DX11/DX12 の型を直接触らない。

```cpp
class IRenderer {
public:
    virtual void ImGuiInit(void* hwnd)       = 0;
    virtual void ImGuiShutdown()             = 0;
    virtual void ImGuiNewFrame()             = 0;
    virtual void ImGuiRenderDrawData()       = 0;
    virtual ImTextureID GetImTextureID(std::shared_ptr<IRenderTarget> rt, int slot = 0) = 0;
};
```

---

## ImGui ドックスペース構成

```
┌─────────────────────────────────────────────────────┐
│  File  Edit  View  | ▶ Play  ⏸ Pause  ⏹ Stop |    │  ← MenuBar + PlayMode
├───────────┬──────────────────────────┬──────────────┤
│ Scene     │                          │  Inspector   │
│ Hierarchy │    Viewport              │              │
│ (Tree)    │    (IRenderTarget)       │  Transform   │
│           │    Gizmo オーバーレイ     │  Light       │
│           │    Grid                  │  Emitter     │
│           │    Stats オーバーレイ     │              │
├───────────┴──────────────────────────┴──────────────┤
│  Console (フィルタ / 検索 / クリア)  │ Asset Browser │
├──────────────────────────────────────────────────────┤
│  StatusBar: FPS | シーン名 | Entity数 | Undo 履歴    │
└──────────────────────────────────────────────────────┘
```

---

## Engine 側の追加

### ILogSink

```cpp
// engine/include/engine/Core/ILogSink.hpp
namespace fbzz::core {

struct LogEntry {
    LogLevel    level;
    std::string message;
};

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual void OnLog(const LogEntry& entry) = 0;
};

} // namespace fbzz::core
```

`Logger` に `AddSink(ILogSink*)` / `RemoveSink(ILogSink*)` を追加し、  
`Log()` 内で登録済みシンクを全呼び出しする。

### FileSystem (engine/Util)

```cpp
namespace fbzz::util {
class FileSystem {
public:
    static bool        Exists(const std::string& path);
    static bool        IsDirectory(const std::string& path);
    static std::string GetExtension(const std::string& path);   // ".fbzz"
    static std::string GetFilename(const std::string& path);    // "scene.fbzz"
    static std::string GetDirectory(const std::string& path);   // "Assets/Scenes/"
    static std::vector<std::string> ListFiles(const std::string& dir, const std::string& ext = "");
    static bool EnsureDirectory(const std::string& path);
    static bool ReadText(const std::string& path, std::string& out);
    static bool WriteText(const std::string& path, const std::string& text);
};
} // namespace fbzz::util
```

### StringUtils (engine/Util)

```cpp
namespace fbzz::util {
class StringUtils {
public:
    static bool        Contains(const std::string& s, const std::string& sub);
    static bool        ContainsCI(const std::string& s, const std::string& sub);  // 大文字小文字無視
    static std::string ToLower(const std::string& s);
    static std::string ToUpper(const std::string& s);
    static bool        StartsWith(const std::string& s, const std::string& prefix);
    static bool        EndsWith(const std::string& s, const std::string& suffix);
    static std::vector<std::string> Split(const std::string& s, char delim);
    static std::string Trim(const std::string& s);
    static std::wstring ToWide(const std::string& s);    // Win32 API 用
    static std::string  ToNarrow(const std::wstring& s);
};
} // namespace fbzz::util
```

---

## EditorContext

エディター全体の共有状態。パネル間の疎結合を保つ仲介役。

```cpp
struct EditorContext {
    // エンジンオブジェクト（非所有）
    scene::Scene*          activeScene   = nullptr;
    renderer::LightSystem* lightSystem   = nullptr;
    renderer::Camera*      editorCamera  = nullptr;

    // 選択状態
    std::vector<scene::EntityID> selectedEntities;   // Multi-select 対応
    scene::EntityID PrimarySelected() const;         // 先頭 or INVALID

    // ビューポート
    bool  viewportFocused = false;
    float viewportWidth   = 1280.0f;
    float viewportHeight  = 720.0f;

    // ギズモ
    enum class GizmoMode  { Translate, Rotate, Scale };
    enum class GizmoSpace { World, Local };
    GizmoMode  gizmoMode  = GizmoMode::Translate;
    GizmoSpace gizmoSpace = GizmoSpace::World;

    // グリッド
    bool  showGrid     = true;
    float gridSize     = 1.0f;
    bool  snapEnabled  = false;
    float snapDistance = 1.0f;

    // レンダリング設定 (RenderSystem に渡す)
    renderer::RenderSettings renderSettings;

    // 表示オプション (エディター固有)
    bool showLightRange = true;
    bool showColliders  = false;
    bool showSceneStats = true;

    // Util (非所有)
    UndoStack*          undoStack = nullptr;
    PlayModeController* playMode  = nullptr;
};
```

---

## PlayModeController

```cpp
enum class PlayState { Editor, Playing, Paused };

class PlayModeController {
public:
    void Play(scene::Scene& scene);   // シーンをスナップショット保存 → ゲームループ開始
    void Pause();
    void Stop(scene::Scene& scene);   // スナップショットから復元

    PlayState GetState() const { return m_state; }
    bool IsPlaying() const     { return m_state == PlayState::Playing; }

private:
    PlayState   m_state    = PlayState::Editor;
    std::string m_snapshot; // TOML 文字列でシーンを保存
};
```

---

## Util 設計

### UndoStack

```cpp
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void Execute() = 0;
    virtual void Undo()    = 0;
    virtual std::string GetDescription() const = 0;
};

class UndoStack {
public:
    static constexpr int MAX_HISTORY = 64;
    void Push(std::unique_ptr<ICommand> cmd);  // Execute() を呼んでからプッシュ
    void Undo();
    void Redo();
    bool CanUndo() const;
    bool CanRedo() const;
    void Clear();
    std::string GetUndoDescription() const;
    std::string GetRedoDescription() const;
private:
    std::vector<std::unique_ptr<ICommand>> m_history;
    int m_cursor = -1;
};
```

### SceneSerializer

```cpp
class SceneSerializer {
public:
    // シーンを TOML ファイルに保存
    static bool Save(const scene::Scene& scene, const std::string& path);
    // TOML ファイルからシーンを復元
    static bool Load(scene::Scene& scene, const std::string& path);
    // メモリ上の TOML 文字列に変換（PlayMode スナップショット用）
    static std::string Serialize(const scene::Scene& scene);
    static bool Deserialize(scene::Scene& scene, const std::string& toml);
};
```

### EditorSettings

```cpp
struct EditorSettings {
    float cameraSpeed       = 5.0f;
    float cameraSensitivity = 0.3f;
    bool  showGrid          = true;
    bool  snapEnabled       = false;
    float snapDistance      = 1.0f;
    std::string lastScenePath;

    bool Load(const std::string& path);   // toml++ で読み込み
    bool Save(const std::string& path) const;
};
```

### FileDialog

```cpp
struct FileFilter { std::string name; std::string spec; };  // {"FBZZ Scene", "*.fbzz"}

class FileDialog {
public:
    static bool OpenFile(void* hwnd, const std::vector<FileFilter>& filters, std::string& outPath);
    static bool SaveFile(void* hwnd, const std::vector<FileFilter>& filters, std::string& outPath);
};
```

### HotkeyManager

```cpp
struct Hotkey {
    std::string            name;
    int                    imguiKey;
    bool ctrl = false, shift = false, alt = false;
    std::function<void()>  callback;
};

class HotkeyManager {
public:
    void Register(Hotkey hotkey);
    void ProcessInput();   // 毎フレーム EditorApp から呼ぶ
    void Clear();
private:
    std::vector<Hotkey> m_hotkeys;
};
```

### ImGuiWidgets

```cpp
namespace fbzz::editor::widgets {

// Vector3 の DragFloat3（ラベル幅を統一）
bool DragVec3(const char* label, math::Vector3& v, float speed = 0.1f);

// Vector3 を色として扱う ColorEdit3
bool ColorEdit3(const char* label, math::Vector3& color);

// セクションヘッダー（太字 + 区切り線）
void SectionHeader(const char* label);

// 読み取り専用テキスト（色付き）
void ColoredText(const char* text, ImVec4 color);

} // namespace fbzz::editor::widgets
```

### MathConvert

```cpp
namespace fbzz::editor {

inline ImVec2 ToImGui(const math::Vector2& v)  { return { v.x, v.y }; }
inline ImVec4 ToImGui(const math::Vector4& v)  { return { v.x, v.y, v.z, v.w }; }
inline math::Vector2 FromImGui(const ImVec2& v) { return { v.x, v.y }; }
inline math::Vector4 FromImGui(const ImVec4& v) { return { v.x, v.y, v.z, v.w }; }

} // namespace fbzz::editor
```

### ConsoleSink

```cpp
class ConsoleSink final : public core::ILogSink {
public:
    static constexpr size_t MAX_ENTRIES = 512;
    void OnLog(const core::LogEntry& entry) override;
    const std::deque<core::LogEntry>& GetEntries() const { return m_entries; }
    void Clear() { m_entries.clear(); }
private:
    std::deque<core::LogEntry> m_entries;
};
```

### ModalDialog

```cpp
class ModalDialog {
public:
    static void OpenConfirm(const std::string& title,
                            const std::string& message,
                            std::function<void()> onConfirm);
    static void OnRender();   // 毎フレーム EditorApp から呼ぶ
private:
    struct State { std::string title, message; std::function<void()> cb; bool pending = false; };
    static State s_state;
};
```

---

## パネル設計

### SceneHierarchyPanel

- `scene::Scene` の全 Entity をツリー表示（親子関係対応、折りたたみ可）
- クリックで `ctx.selectedEntities` に書き込み
- Ctrl+クリックで Multi-select
- Entity を別 Entity へドラッグ → 親子関係を変更
- 右クリックコンテキストメニュー: 追加 / 削除 / Duplicate / 表示切り替え

### InspectorPanel

- `ctx.PrimarySelected()` の各コンポーネントを表示
- `DrawComponent<T>()` テンプレート特殊化で各型に対応
- 編集操作は `UndoStack` に `ICommand` として積む

```cpp
template<typename T>
void DrawComponent(EditorContext& ctx, T& component);

// 特殊化: TransformComponent / MeshRenderer / LightComponent /
//         ParticleEmitter / RigidbodyComponent / AudioSource
```

### ViewportPanel

- `hdrRT`（IRenderTarget）を `GetImTextureID` → `ImGui::Image` で表示
- パネルサイズ変更時にカメラのアスペクト比を更新
- **EditorCamera 操作**
  - RMB ドラッグ: オービット
  - RMB + WASD: フライカメラ
  - スクロール: ズーム
  - F キー: 選択 Entity にフォーカス
- **Gizmo**（ImGuizmo）
  - Translate / Rotate / Scale モード（Q/W/E/R キー）
  - World / Local 切り替え（X キー）
  - 操作確定時に UndoStack に積む
- **Grid**: XZ 平面グリッドを DebugDraw で描画
- **Stats オーバーレイ**: 右上に FPS・Entity 数・トライアングル数

### LightPanel

- DirectionalLight: ColorEdit3 / SliderFloat(intensity) / DragFloat3(direction)
- PointLight[i]: + SliderFloat(range) / DragFloat3(position)
- SpotLight[i]: + SliderAngle(innerAngle / outerAngle)
- `ctx.showLightRange` が ON の時、Viewport にライト範囲を球・コーンで可視化

### ConsolePanel

- `ConsoleSink` のエントリを表示
- INFO / WARN / ERROR のトグルフィルタ
- テキスト検索バー
- クリアボタン・自動スクロールチェックボックス

### AssetBrowserPanel（簡易版）

- `Assets/` フォルダをファイルリストで表示
- テキスト検索
- ダブルクリックでシーン (.fbzz) を開く

### StatusBar

- 毎フレーム Viewport 最下部に固定表示
- FPS / シーン名 / 選択 Entity 名 / Undo 履歴の最新操作

---

## EditorApp

```cpp
class EditorApp {
public:
    bool Init(renderer::IRenderer& renderer, core::Window& window);
    void Shutdown();

    void BeginFrame();                       // ImGui::NewFrame() + DockSpace
    void RenderPanels(EditorContext& ctx);   // 全パネル OnRender
    void EndFrame(renderer::IRenderer& renderer); // ImGui::Render + RenderDrawData

    EditorContext& GetContext() { return m_ctx; }

    // Viewport に紐づいたオフスクリーン RT (main.cpp はここに描く)
    std::shared_ptr<renderer::IRenderTarget> GetViewportRT() const;

private:
    void BuildMenuBar(EditorContext& ctx);
    void RegisterDefaultHotkeys();
    void ResizeViewportRTIfNeeded();

    EditorContext                        m_ctx;
    std::vector<std::unique_ptr<IPanel>> m_panels;
    HotkeyManager                        m_hotkeys;
    UndoStack                            m_undoStack;
    PlayModeController                   m_playMode;
    ConsoleSink                          m_consoleSink;
    EditorSettings                       m_settings;

    void*                                    m_hwnd          = nullptr;
    renderer::IRenderer*                     m_renderer      = nullptr;
    std::shared_ptr<renderer::IRenderTarget> m_viewportRT;
};
```

---

## レンダーループへの組み込み

```cpp
editor::EditorApp editor;
editor.Init(renderer, hwnd);

auto& ctx = editor.GetContext();
ctx.activeScene  = sm.GetActive();
ctx.lightSystem  = &lights;
ctx.editorCamera = &debugCamera.camera;

while (app.IsRunning())
{
    core::Time::Tick();
    input::Input::Update();
    app.GetWindow().PollEvents();

    if (!editor.GetContext().playMode->IsPlaying())
        sm.Update(dt, physWorld);   // Editor モード時のみ物理更新

    renderer.BeginFrame();
    renderer.Clear({ 0.005f, 0.005f, 0.02f, 1.0f });
    scene::RenderSystem(*sm.GetActive(), renderer, debugCamera.camera, lights);

    editor.BeginFrame();
    editor.RenderPanels(ctx);
    editor.EndFrame(renderer);

    renderer.EndFrame();
}

editor.Shutdown();
```

---

## 実装順序

| # | タスク | 主要ファイル |
|---|--------|-------------|
| 1 | Engine Util 追加 | ILogSink / FileSystem / StringUtils |
| 2 | ImGui → IRenderer 統合 | DX11Renderer + IRenderer |
| 3 | EditorApp + DockSpace | EditorApp.cpp |
| 4 | MathConvert / ImGuiWidgets | Util/ |
| 5 | SceneHierarchyPanel | SceneHierarchyPanel.cpp |
| 6 | InspectorPanel (Transform) | InspectorPanel.cpp + UndoStack |
| 7 | ViewportPanel + EditorCamera | ViewportPanel.cpp |
| 8 | Gizmo (ImGuizmo) + MousePicking | ViewportPanel.cpp |
| 9 | LightPanel + ライト範囲可視化 | LightPanel.cpp |
| 10 | ConsoleSink + ConsolePanel | Util/ConsoleSink + ConsolePanel.cpp |
| 11 | SceneSerializer (TOML) + FileDialog | Util/ |
| 12 | PlayModeController | PlayModeController.cpp |
| 13 | AssetBrowserPanel + StatusBar | 残りパネル |
| 14 | HotkeyManager + メニューバー | EditorApp.cpp |
| 15 | EditorSettings 永続化 | Util/EditorSettings.cpp |

---

## 変更ファイル一覧

| 種別 | ファイル |
|------|---------|
| 新規 | `engine/include/engine/Core/ILogSink.hpp` |
| 修正 | `engine/include/engine/Core/Logger.hpp` (AddSink/RemoveSink) |
| 修正 | `engine/src/Core/Logger.cpp` |
| 新規 | `engine/include/engine/Util/FileSystem.hpp` |
| 新規 | `engine/src/Util/FileSystem.cpp` |
| 新規 | `engine/include/engine/Util/StringUtils.hpp` |
| 新規 | `engine/src/Util/StringUtils.cpp` |
| 修正 | `engine/include/engine/Renderer/IRenderer.hpp` (ImGui* 仮想メソッド) |
| 修正 | `engine/src/Renderer/Platform/DX11/DX11Renderer.hpp/.cpp` |
| 新規 | `editor/` 全体（CMakeLists 含む） |
| 修正 | `sandbox/src/main.cpp` (EditorApp 統合) |
