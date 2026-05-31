# FBZZ Engine — Standalone ビルドシステム 設計書

## 1. 前提と方針

### 現状の起動フロー

```
FBZZHub.exe
  └─ ShellExecute → FBZZEditor.exe --project <path>
                        └─ ResolveProject()
                        └─ EditorApp::Init()   ← ImGui エディタ UI
                        └─ メインループ
```

`main.cpp` は `--project` 引数でプロジェクトパスを受け取り、  
`EditorApp` を生成してエディタ付きで起動する単一バイナリ構成になっている。

### 方針

**新しい実行ファイルは作らない。**  
`FBZZEditor.exe` に `--standalone` フラグを追加し、同じバイナリを  
エディタモード / スタンドアロンモードの両方で使う。

```
FBZZEditor.exe --project <path>              → エディタ起動 (現状)
FBZZEditor.exe --project <path> --standalone → エディタ UI なし・ゲームのみ起動
```

エディタの「▶ Standalone」ボタンから `CreateProcess` で後者を起動する。  
配布時は `FBZZEditor.exe` をリネームした `FBZZGame.exe` とアセットをまとめるだけでよい。

---

## 2. アーキテクチャ

### 2-1. 起動フロー (変更後)

```
FBZZEditor.exe --project <path> [--standalone]
    │
    ├─ ParseArgs()            ← LaunchArgs を返す
    ├─ ResolveProject()       ← 変更なし (.fbzz_proj を読む)
    │
    ├─ [standalone フラグなし]
    │     └─ RunEditorLoop()  ← 既存ロジックを関数化して分離
    │
    └─ [--standalone あり]
          └─ RunStandaloneLoop()  ← 新規
```

### 2-2. ループの比較

| 項目 | Editor モード | Standalone モード |
|------|:---:|:---:|
| `EditorApp` 生成 | ✅ | ❌ |
| ImGui 描画 | ✅ | ❌ |
| Debug Camera | ✅ | ❌ |
| `PlayModeController` | ✅ | ❌ |
| ゲームカメラ | Game Viewport 内に描画 | バックバッファへ直接描画 |
| 物理・アニメーション更新 | Play 中のみ | 常時 |
| ウィンドウサイズ | エディタ枠 | `ProjectSettings.window` から初期化 |

---

## 3. 実装設計

### 3-1. 引数解析の拡張

`main.cpp` の `FindProjectPathFromArgs()` を廃止し `ParseArgs()` に置き換える。

```cpp
struct LaunchArgs {
    std::filesystem::path projectPath;
    bool standalone = false;
};

LaunchArgs ParseArgs()
{
    LaunchArgs args;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return args;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--project" && i + 1 < argc)
            args.projectPath = argv[++i];
        else if (arg == L"--standalone")
            args.standalone = true;
    }
    LocalFree(argv);

    // 引数なし起動 = 配布版。exe 隣の .fbzz_proj を自動検出して Standalone 扱いにする。
    if (args.projectPath.empty()) {
        args.projectPath = GetExecutableDirectory();
        args.standalone  = true;
    }

    return args;
}
```

### 3-2. Application 初期化とウィンドウサイズの順序

**問題:** `Application::Init()` がウィンドウを生成した後に `Resize()` を呼ぶと、  
ウィンドウが一瞬デフォルトサイズで表示されてからリサイズされる。

**解決:** Standalone モードでは ProjectSettings を **`Application::Init()` より前に** 読み込み、  
初期ウィンドウサイズとして渡す。

```cpp
// §3-3 の Run() 内
if (args.standalone) {
    // Init より前に ProjectSettings を読んでウィンドウ設定を取得する
    ProjectSettings settings;
    if (!LoadProjectSettings(project.settingsFile, settings)) { ... }

    core::WindowDesc desc;
    desc.title      = settings.window.title;
    desc.width      = settings.window.width;
    desc.height     = settings.window.height;
    desc.fullscreen = settings.window.fullscreen;

    if (!app.Init(desc)) return 1;
    RunStandaloneLoop(renderer, resources, project, settings);
} else {
    if (!app.Init()) return 1;   // エディタはデフォルトサイズで起動
    RunEditorLoop(renderer, resources, project);
}
```

`Application::Init()` にオプションの `WindowDesc` を受け取るオーバーロードを追加する。

### 3-3. StandaloneApp クラス

`EditorLauncher` 内に Standalone ループを担うクラスを追加する。  
`fbzz_editor` には一切依存しない。

**新規ファイル:**

```
Projects/EditorLauncher/src/StandaloneApp.hpp
Projects/EditorLauncher/src/StandaloneApp.cpp
```

```cpp
// StandaloneApp.hpp
#pragma once
#include <Engine/ProjectSettings.hpp>
#include <Engine/Scene/Scene.hpp>
#include <Engine/Renderer/ResourceManager.hpp>
#include <Physics/World.hpp>
#include <filesystem>

namespace fbzz::editor_launcher {

class StandaloneApp {
public:
    bool Init(renderer::IRenderer& renderer,
              renderer::ResourceManager& resources,
              const std::filesystem::path& assetRoot,
              const std::filesystem::path& sceneFile,
              const ProjectSettings& settings);

    void RunLoop(renderer::IRenderer& renderer,
                 renderer::ResourceManager& resources);

    void Shutdown();

private:
    // シーン内の isMain な CameraComponent を探してゲームカメラを返す。
    // 見つからなければデフォルト値のカメラを返す。
    renderer::Camera ResolveGameCamera(float aspectRatio) const;

    std::unique_ptr<scene::Scene>    m_scene;
    std::unique_ptr<physics::World>  m_physicsWorld;
    ProjectSettings                  m_settings;
    float                            m_physicsAccumulator = 0.0f;
};

} // namespace fbzz::editor_launcher
```

**ゲームループの骨格:**

```cpp
void StandaloneApp::RunLoop(renderer::IRenderer& renderer,
                            renderer::ResourceManager& resources)
{
    auto& app = core::Application::Get();
    core::Time::Tick(); // warmup フレームの時間を除去

    while (app.IsRunning()) {
        core::Time::Tick();
        input::Input::Update();
        app.GetWindow().PollEvents();
        if (app.GetWindow().ShouldClose()) { app.Quit(); break; }

        const float dt = core::Time::DeltaTime();

        // 物理 (固定ステップ)
        const int   physicsHz = m_settings.physics.hz < 1 ? 60 : m_settings.physics.hz;
        const float fixedDt   = 1.0f / static_cast<float>(physicsHz);
        m_physicsAccumulator += dt;
        const float maxAccum  = fixedDt * 8.0f;
        if (m_physicsAccumulator > maxAccum) m_physicsAccumulator = maxAccum;
        while (m_physicsAccumulator >= fixedDt) {
            scene::PhysicsSystem(*m_scene, *m_physicsWorld, fixedDt);
            m_physicsAccumulator -= fixedDt;
        }

        scene::TransformSystem(*m_scene);
        scene::AnimatorSystem(*m_scene, resources, dt);

        // バックバッファへ直接描画
        renderer.BeginFrame();
        renderer.SetRenderTarget({}, resources);
        renderer.Clear({ 0.02f, 0.02f, 0.05f, 1.0f });

        const auto [w, h] = app.GetWindow().GetSize();
        const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : 1.0f;
        const renderer::Camera gameCamera = ResolveGameCamera(aspect);

        scene::RenderSystem(*m_scene, renderer, resources, gameCamera, {}, &m_settings.render);
        scene::UISystem(*m_scene, renderer, resources,
                        static_cast<float>(w), static_cast<float>(h),
                        {}, true, gameCamera.GetViewProjection());

        renderer.EndFrame();
    }
}

renderer::Camera StandaloneApp::ResolveGameCamera(float aspectRatio) const
{
    // WHY: Standalone モードでは EditorCamera が存在しないため、
    //      シーン内の isMain フラグを持つ CameraComponent を唯一のゲームカメラとして使う。
    for (auto& go : m_scene->GameObjects()) {
        auto* cam = go.GetComponent<scene::CameraComponent>();
        if (!go.activeSelf() || !cam || !cam->enabled || !cam->isMain) continue;

        renderer::Camera result;
        result.m_position = go.transform.position;
        result.m_rotation = go.transform.rotation;
        result.m_fovY     = cam->fovY;
        result.m_near     = cam->nearZ;
        result.m_far      = cam->farZ;
        result.m_aspect   = aspectRatio;
        return result;
    }

    // カメラが見つからない場合のフォールバック
    renderer::Camera fallback;
    fallback.m_aspect = aspectRatio;
    return fallback;
}
```

### 3-4. main.cpp の分岐

既存のエディタループを `RunEditorLoop()` として関数化し、main を整理する。

```cpp
int Run()
{
    const LaunchArgs args = ParseArgs();

    LaunchProject project;
    std::wstring errorMsg;
    if (!ResolveProject(project, args.projectPath, errorMsg)) {
        MessageBoxW(nullptr, errorMsg.c_str(), L"FBZZ", MB_OK | MB_ICONERROR);
        return 1;
    }

    SetCurrentDirectoryW(GetExecutableDirectory().wstring().c_str());

    auto& app = core::Application::Get();

    if (args.standalone) {
        ProjectSettings settings;
        if (!LoadProjectSettings(project.settingsFile, settings)) {
            MessageBoxW(nullptr, L"ProjectSettings を読み込めませんでした。", L"FBZZ", MB_OK | MB_ICONERROR);
            return 1;
        }

        // ProjectSettings のウィンドウ設定でウィンドウを生成する
        core::WindowDesc desc;
        desc.title      = settings.window.title;
        desc.width      = settings.window.width;
        desc.height     = settings.window.height;
        desc.fullscreen = settings.window.fullscreen;
        if (!app.Init(desc)) return 1;

        auto& renderer = app.GetRenderer();
        renderer::ResourceManager resources(renderer);
        asset::AssetManager::Init(resources, PathToUtf8(project.root / L"Assets") + "/");

        StandaloneApp standaloneApp;
        if (!standaloneApp.Init(renderer, resources,
                                project.root / L"Assets",
                                project.sceneFile,
                                settings)) {
            app.Shutdown();
            return 1;
        }
        standaloneApp.RunLoop(renderer, resources);
        standaloneApp.Shutdown();
    } else {
        if (!app.Init()) return 1;
        auto& renderer = app.GetRenderer();
        renderer::ResourceManager resources(renderer);
        RunEditorLoop(renderer, resources, project); // 既存ロジックをここに切り出す
    }

    asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}
```

---

## 4. エディタ側: Standalone 起動ボタン

**新規ファイル:**

```
Projects/Editor/include/Editor/Util/StandaloneLauncher.hpp
Projects/Editor/src/Util/StandaloneLauncher.cpp
```

```cpp
// StandaloneLauncher.hpp
#pragma once
#include <string>

namespace fbzz::editor {

class StandaloneLauncher {
public:
    // 現在開いているプロジェクトを Standalone モードで起動する。
    // exePath   : 自身 (FBZZEditor.exe) のフルパス。GetModuleFileNameW で取得。
    // projectPath: プロジェクトルートディレクトリの絶対パス。
    static bool Launch(const std::string& exePath,
                       const std::string& projectPath);
};

} // namespace fbzz::editor
```

```cpp
// StandaloneLauncher.cpp
bool StandaloneLauncher::Launch(const std::string& exePath,
                                const std::string& projectPath)
{
    // WHY: CreateProcess の lpCommandLine は書き込み可能バッファが必要なため
    //      wstring のコピーを渡す。
    std::wstring cmd =
        L"\"" + Utf8ToWide(exePath) + L"\""
        L" --project \"" + Utf8ToWide(projectPath) + L"\""
        L" --standalone";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    const BOOL ok = CreateProcessW(
        nullptr, cmd.data(), nullptr, nullptr,
        FALSE, 0, nullptr, nullptr, &si, &pi);

    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return ok != FALSE;
}
```

**メニュー統合 (`EditorApp_MenuBar.cpp`):**

```
Tools
  ├─ ▶ Play         (既存)
  ├─ ▶ Standalone   (新規)
  └─ ─────────────
  └─ Build Settings... → EditorContext::requestOpenBuildSettings
```

---

## 5. 配布パッケージ構成

```
MyGame_v1.0/
├── MyGame.exe              ← FBZZEditor.exe をリネーム
├── assimp-vc145-mt.dll
├── .fbzz_proj
├── ProjectSettings.toml    ← window / runtime / physics セクションを設定済み
└── assets/
    ├── shaders/            ← コンパイル済み .cso
    ├── textures/
    ├── models/
    ├── sounds/
    └── scenes/
```

`MyGame.exe` を引数なしで起動すると `ParseArgs()` が隣の `.fbzz_proj` を自動検出し、  
`--standalone` 扱いで起動する。

### VC++ ランタイム依存

```
必須: Visual C++ 2022 再頒布可能パッケージ (x64)
同梱: assimp-vc145-mt.dll
OS 標準: d3d11.dll, dxgi.dll, xaudio2_9.dll
```

---

## 6. ProjectSettings の拡張

Standalone モード用のウィンドウ設定を `ProjectSettings` に追加する。

```toml
# ProjectSettings.toml (新規セクション)
[window]
title      = "My Game"
width      = 1920
height     = 1080
fullscreen = false
```

`Application::Init(WindowDesc)` オーバーロードを追加し、  
Standalone モードでは `ProjectSettings` を先読みしてウィンドウを正しいサイズで生成する。

---

## 7. 実装ロードマップ

| フェーズ | タスク | 対象ファイル |
|----------|--------|-------------|
| **Phase 1** | `ParseArgs()` を `LaunchArgs` 構造体に変更、引数なし自動検出追加 | `main.cpp` |
| **Phase 1** | `Application::Init(WindowDesc)` オーバーロードを追加 | `Engine/Core/Application.*` |
| **Phase 1** | `ProjectSettings` に `[window]` セクションを追加 | `Engine/ProjectSettings.*` |
| **Phase 2** | `StandaloneApp` クラスを実装 | `EditorLauncher/src/StandaloneApp.*` |
| **Phase 2** | `main.cpp` に `--standalone` 分岐を追加 | `main.cpp` |
| **Phase 3** | `StandaloneLauncher` クラスを実装 | `Editor/src/Util/StandaloneLauncher.*` |
| **Phase 3** | エディタ Toolbar に「▶ Standalone」ボタンを追加 | `EditorApp_MenuBar.cpp` |
| **Phase 4** | Build Settings パネル (`build_pipeline.md` 参照) | — |
