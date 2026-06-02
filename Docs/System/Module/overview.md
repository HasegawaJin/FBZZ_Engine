# IModule システム 設計書

`IModule` (`Engine/Core/IModule.hpp`) はアプリケーションのゲームループを抽象化するインターフェース。  
`Application::Run(IModule&)` がループのボイラープレートを担い、ゲーム固有ロジックは `IModule` の実装クラスに委ねる。

---

## 動機

### 変更前の問題

`RunStandaloneLoop()` と `RunEditorLoop()` はそれぞれ 100〜250 行の巨大関数で、  
どちらも同一のボイラープレートを持っていた。

```
Time::Tick → Memory::BeginFrame → Profiler::BeginFrame
→ Input::Update → PollEvents → ShouldClose チェック
→ [ゲーム固有処理]
→ Profiler::EndFrame → Memory::EndFrame
```

これを `Application::Run(IModule&)` に集約し、`main.cpp` を 100 行以下に抑える。

---

## インターフェース定義

**ファイル**: `Engine/include/Engine/Core/IModule.hpp`

```cpp
namespace fbzz::core {

class IModule {
public:
    virtual ~IModule() = default;

    /// Application::Init() 完了後、ループ開始前に 1 回呼ばれる。
    /// シーンロード・リソース確保などの初期化を行う。
    /// @return false を返すと即座に終了する (OnShutdown は必ず呼ばれる)。
    [[nodiscard]] virtual bool OnInit() = 0;

    /// 毎フレーム呼ばれるメイン更新フェーズ。
    /// スクリプト・物理など「変換前」の処理を書く。
    virtual void OnUpdate(float dt) = 0;

    /// OnUpdate の後に呼ばれる遅延更新フェーズ。
    /// アニメーション・IK など「変換後」の後処理を書く。
    virtual void OnLateUpdate(float dt) = 0;

    /// 毎フレーム呼ばれる描画フェーズ。
    /// renderer.BeginFrame() 〜 renderer.EndFrame() をここで担う。
    virtual void OnRender() = 0;

    /// ループ終了後・Application::Shutdown() 前に 1 回呼ばれる。
    /// リソース解放などのクリーンアップを行う。
    virtual void OnShutdown() = 0;
};

} // namespace fbzz::core
```

---

## ライフサイクル

```
Application::Init()
    │
    ▼
IModule::OnInit()  ─── false → IModule::OnShutdown() → 終了
    │ true
    ▼
┌─ ループ ──────────────────────────────────────────────────────────┐
│  core::Time::Tick()                                               │
│  MemorySystem::BeginFrame()                                       │
│  Profiler::BeginFrame()                                           │
│  Input::Update()                                                  │
│  Window::PollEvents()                                             │
│  Window::ShouldClose() ─── true → Quit → break                   │
│  IModule::OnUpdate(dt)                                            │
│  IModule::OnLateUpdate(dt)                                        │
│  IModule::OnRender()                                              │
│  Profiler::EndFrame()                                             │
│  MemorySystem::EndFrame()                                         │
└───────────────────────────────────────────────────────────────────┘
    │
    ▼
IModule::OnShutdown()
    │
    ▼
Application::Shutdown()
```

> **WHY — ウォームアップ `Time::Tick()`**:  
> `Application::Run()` の先頭で `Time::Tick()` を 1 回呼び、  
> 初期化処理の時間が最初のフレームの `DeltaTime` に混入しないようにする。

---

## `Application::Run()` の変更

```cpp
// 変更前
void Run();

// 変更後
void Run(IModule& module);
```

### 実装 (`Application.cpp`)

```cpp
void Application::Run(IModule& module)
{
    // 初期化時間が DeltaTime に混入しないようウォームアップ
    core::Time::Tick();

    if (!module.OnInit()) {
        module.OnShutdown();
        return;
    }

    while (m_isRunning) {
        core::Time::Tick();
        m_memorySystem.BeginFrame();
        profiler::Profiler::BeginFrame();

        input::Input::Update();
        m_window->PollEvents();
        if (m_window->ShouldClose()) {
            // WHY: ループ本体末尾の EndFrame はここに到達しないため、
            //      break する前に明示的に呼ぶ。呼ばないとプロファイラと
            //      フレームアロケータの Begin/End が非対称になる。
            profiler::Profiler::EndFrame();
            m_memorySystem.EndFrame();
            Quit();
            break;
        }

        const float dt = core::Time::DeltaTime();
        module.OnUpdate(dt);
        module.OnLateUpdate(dt);
        module.OnRender();

        profiler::Profiler::EndFrame();
        m_memorySystem.EndFrame();
    }

    module.OnShutdown();
}
```

---

## 具体実装: Sandbox

### ファイル構成

```
Projects/Sandbox/src/
  main.cpp             ─ 引数解析・起動分岐のみ (~100 行)
  StandaloneModule.hpp ─ 新規 (RunStandaloneLoop の移植)
  StandaloneModule.cpp ─ 新規
  EditorModule.hpp     ─ 新規 (RunEditorLoop の移植)
  EditorModule.cpp     ─ 新規
```

---

### `StandaloneModule`

```cpp
// StandaloneModule.hpp
class StandaloneModule : public core::IModule {
public:
    StandaloneModule(const LaunchProject& project, const ProjectSettings& settings);

    bool OnInit()             override;
    void OnUpdate(float dt)   override;
    void OnLateUpdate(float dt) override;
    void OnRender()           override;
    void OnShutdown()         override;

private:
    const LaunchProject&          m_project;
    const ProjectSettings&        m_settings;
    std::unique_ptr<scene::Scene> m_scene;
    physics::World                m_physicsWorld;
    float                         m_physicsAccumulator = 0.0f;
};
```

| フェーズ | 処理 |
|---------|------|
| `OnInit` | `SceneSerializer::Load` / `ApplyPhysicsSettings` / `ApplyUISettings` |
| `OnUpdate` | `ScriptSystem` / `TransformSystem` / 固定タイムステップ物理 / `TransformSystem` |
| `OnLateUpdate` | `LateScriptSystem` / `AnimatorSystem` / `IKSystem` |
| `OnRender` | `BeginFrame` / `RenderSystem` / `EndFrame` |

---

### `EditorModule`

```cpp
// EditorModule.hpp
class EditorModule : public core::IModule {
public:
    explicit EditorModule(const LaunchProject& project);

    bool OnInit()             override;
    void OnUpdate(float dt)   override;
    void OnLateUpdate(float dt) override;
    void OnRender()           override;
    void OnShutdown()         override;

private:
    const LaunchProject&          m_project;
    editor::EditorApp             m_editorApp;
    std::unique_ptr<scene::Scene> m_scene;
    physics::World                m_physicsWorld;
    renderer::DebugCamera         m_debugCamera;
    float                         m_physicsAccumulator = 0.0f;
    // FocusAnim など editor 固有の状態
    struct FocusAnim { /* ... */ } m_focusAnim;
};
```

| フェーズ | 処理 |
|---------|------|
| `OnInit` | `EditorApp::Init` / `OpenProject` / `ApplyPhysicsSettings` |
| `OnUpdate` | `EditorApp::BeginFrame` / PlayMode 判定 / `ScriptSystem` / 物理 / `DebugCamera::Update` |
| `OnLateUpdate` | `AnimatorSystem` / `IKSystem` |
| `OnRender` | Scene ビューポート描画 / Game ビューポート描画 / `EditorApp::RenderPanels` / `EndFrame` |
| `OnShutdown` | `EditorApp::Shutdown` |

---

### 変更後の `main.cpp`

```cpp
int Run()
{
    const LaunchArgs args = ParseArgs();
    // ... ResolveProject, エラー処理 (現状と同じ) ...

    RegisterSandboxScripts();
    auto& app = core::Application::Get();

    if (args.standalone) {
        ProjectSettings settings;
        if (!settings.Load(PathToUtf8(project.settingsFile))) return 1;

        core::Window::Config windowConfig = BuildWindowConfig(settings);
        if (!app.Init(windowConfig)) return 1;

        renderer::ResourceManager resources(app.GetRenderer());
        asset::AssetManager::Init(resources, assetsPath);

        StandaloneModule module(project, settings);
        app.Run(module);
    } else {
        if (!app.Init()) return 1;

        renderer::ResourceManager resources(app.GetRenderer());
        asset::AssetManager::Init(resources, assetsPath);

        EditorModule module(project);
        app.Run(module);
    }

    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}
```

---

## 設計上の決定と理由

| 項目 | 判断 | 理由 |
|------|------|------|
| `OnFixedUpdate(float fixedDt)` なし | 固定タイムステップは Module 内でアキュムレーター管理 | `Application` が `ProjectSettings::physics.hz` を知るべきでない |
| `OnRender()` に `IRenderer&` を渡さない | `Application::Get().GetRenderer()` で取得 | シグネチャの簡潔さを優先。引数を増やすメリットが薄い |
| `ResourceManager` を Module に渡す | コンストラクタ引数で参照渡し | `Application` が `ResourceManager` を所有しないアーキテクチャを維持 |
| `OnInit()` は `bool` を返す | `false` で即 Shutdown | `assert` 以外のエラー方針 (回復可能エラーは `bool` 返し) に準拠 |

---

## パッケージビルドでの EditorModule 除外

### 前提

StandaloneProject をビルドして配布用 exe を生成するとき、エディタコードは不要。  
`IModule` の設計はこのシナリオに対して **`#ifdef` なしで対応できる**。

### なぜきれいに分離できるか

`StandaloneModule` は `EditorModule` を一切インクルードしない独立した実装。  
CMake ターゲットのソースリストから `EditorModule.cpp` を除外するだけで、  
エディタ依存がバイナリに混入しないことが **コンパイル時に保証される**。

```
Sandbox.exe (開発用)
  └── main_editor.cpp → StandaloneModule + EditorModule 両方リンク

SandboxGame.exe (パッケージ成果物)
  └── main_standalone.cpp → StandaloneModule だけリンク
```

### パッケージ用 `main_standalone.cpp`

常に Standalone 起動のため、条件分岐すら不要。

```cpp
#include "StandaloneModule.hpp"
// EditorModule.hpp は一切ない

int main()
{
    const auto args = LaunchArgs::Parse();
    // ... ResolveProject, エラー処理 ...

    StandaloneModule module(project, settings);
    app.Run(module);

    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}
```

### CMake ターゲット構成

```cmake
# 開発用 Sandbox (エディタあり)
target_sources(Sandbox PRIVATE
    main_editor.cpp
    StandaloneModule.cpp
    EditorModule.cpp
)
target_link_libraries(Sandbox PRIVATE Engine Editor)

# パッケージ exe (エディタなし)
target_sources(SandboxGame PRIVATE
    main_standalone.cpp
    StandaloneModule.cpp   # これだけ
)
target_link_libraries(SandboxGame PRIVATE Engine)  # Editor をリンクしない
```

### `#ifdef` 方式との比較

| | `#ifdef FBZZ_EDITOR` | IModule + ファイル分割 |
|--|--|--|
| エディタ依存の排除 | 書き忘れリスクあり | **コンパイル時に保証** |
| main.cpp の見通し | `#ifdef` が条件をまたぐ | **各ファイルが単純** |
| バイナリへの混入 | EditorApp が残る可能性 | **リンクされない** |

---

## 将来の拡張 (現時点では実装しない)

- **GameHub Template** — `AppMain.cpp` も同様に `StandaloneModule` 相当クラスを持てる
- **`OnFixedUpdate(float fixedDt)`** — `IModule::SetFixedHz(int hz)` で指定し Application が管理する案もある
- **複数 Module の重ね合わせ** — `Application::PushModule` / `PopModule` で UI レイヤーなどを合成できるが、現状の要件では不要
