# GameHub プロジェクトのビルドパイプライン統合

## 1. 前提の整理

Sandbox はエンジン自体のテスト用であり、**実際のゲームは GameHub テンプレートから生成されたプロジェクトがターゲット**。  
ビルドパイプライン・IModule の設計はこちらを主軸に考える必要がある。

```
GameHub.exe
  └─ テンプレートからプロジェクト生成
        └─ MyGame/ (standard テンプレートのコピー)
              ├── CMakeLists.txt    ← fbzz_editor を無条件リンク (問題)
              ├── Src/AppMain.cpp   ← 647行 (問題)
              └── .fbzz_proj        ← build_root フィールドを持つ

EditorLauncher.exe --project MyGame/
  └─ MyGame を開いてエディタ UI で編集

[Build] ボタン
  └─ MyGame のビルド → MyGame.exe (配布用)
```

---

## 2. 現状の問題

### 2-1. テンプレートが常に Editor をリンクする

```cmake
# Templates/standard/CMakeLists.txt (現状)
target_link_libraries({{TARGET_NAME}} PRIVATE
    fbzz_engine
    fbzz_physics
    fbzz_math
    fbzz_editor    # ← 配布用 exe にも Editor が入ってしまう
)
```

### 2-2. `build.config` の場所の前提が誤っている

`runtime_compile.md` では「exe 隣の `build.config`」を読む設計になっているが、  
GameHub プロジェクトのビルドディレクトリは `.fbzz_proj` の `build_root` フィールドにある。  
EditorLauncher の exe 隣ではない。

```toml
# MyGame/.fbzz_proj
[project]
build_root = "Build/"    # ← プロジェクトルート相対のビルドディレクトリ
binary_root = "Binaries/"
```

### 2-3. `AppMain.cpp` が Sandbox と同じ問題を抱えている

テンプレートの `AppMain.cpp` は Sandbox の `main.cpp` とほぼ同じ構造 (647行)。  
IModule を適用しないと、ユーザーのゲームプロジェクトが同じ問題を引き継ぐ。

---

## 3. 設計変更

### 3-1. テンプレート CMakeLists.txt に 2 ターゲットを追加

```cmake
# Templates/standard/CMakeLists.txt (変更後)

# ─────────────────────────────────────────────
# 開発用 (エディタあり)
# ─────────────────────────────────────────────
add_executable({{TARGET_NAME}}
    Src/main_editor.cpp
    Src/StandaloneModule.cpp
    Src/EditorModule.cpp
    Src/GameMain.cpp          # RegisterScripts() を定義
)
target_link_libraries({{TARGET_NAME}} PRIVATE
    fbzz_engine fbzz_physics fbzz_math fbzz_editor
)
target_compile_definitions({{TARGET_NAME}} PRIVATE FBZZ_EDITOR)

# ─────────────────────────────────────────────
# 配布用 (エディタなし) ← 新規
# ─────────────────────────────────────────────
add_executable({{TARGET_NAME}}Game
    Src/main_standalone.cpp
    Src/StandaloneModule.cpp
    Src/GameMain.cpp
)
target_link_libraries({{TARGET_NAME}}Game PRIVATE
    fbzz_engine fbzz_physics fbzz_math
    # fbzz_editor はリンクしない
)

# ─────────────────────────────────────────────
# build.config を生成 (ToolchainLocator が読む)
# ─────────────────────────────────────────────
configure_file(
    "${FBZZ_ENGINE_ROOT}/cmake/build.config.in"
    "${CMAKE_BINARY_DIR}/build.config"
    @ONLY
)
```

`build.config.in` の中身 (既存):

```ini
cmake_exe=@CMAKE_COMMAND@
build_dir=@CMAKE_BINARY_DIR@
exe_debug=@CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG@/{{TARGET_NAME}}Game.exe
exe_release=@CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE@/{{TARGET_NAME}}Game.exe
```

> **WHY**: `{{TARGET_NAME}}Game` の exe パスをコンパイル時に確定させる。  
> マルチコンフィグジェネレーター (Visual Studio) の Debug/Release 成果物パスを  
> `CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG/RELEASE` で正確に記録する。

### 3-2. テンプレート AppMain.cpp を IModule パターンに分割

```
Templates/standard/Src/
  main_editor.cpp       ← エディタ起動 (EditorModule を使用)
  main_standalone.cpp   ← スタンドアロン起動 (StandaloneModule を使用)
  StandaloneModule.hpp/.cpp ← ゲームループ実装
  EditorModule.hpp/.cpp     ← エディタループ実装
  GameMain.cpp          ← RegisterScripts() をユーザーが書く場所 (既存)
```

**`main_standalone.cpp`** (テンプレート):

```cpp
// {{TARGET_NAME}} — Standalone エントリポイント
// EditorModule を含まないため、配布用 exe に Editor が混入しない。
#include "StandaloneModule.hpp"
#include <Util/LaunchArgs.hpp>
#include <Util/ProjectResolver.hpp>

// ユーザーが GameMain.cpp で実装する
void RegisterScripts();

int main()
{
    RegisterScripts();

    const auto args = LaunchArgs::Parse();

    ProjectResolver resolver;
    if (!resolver.Resolve(args.projectPath)) {
        MessageBoxW(nullptr, resolver.ErrorMessage().c_str(),
                    L"{{PROJECT_NAME}}", MB_OK | MB_ICONERROR);
        return 1;
    }

    auto& app = fbzz::core::Application::Get();
    fbzz::ProjectSettings settings;
    if (!settings.Load(...)) return 1;
    if (!app.Init(BuildWindowConfig(settings))) return 1;

    fbzz::renderer::ResourceManager resources(app.GetRenderer());
    fbzz::asset::AssetManager::Init(resources, assetsPath);

    StandaloneModule module(resolver.Get(), settings);
    app.Run(module);

    fbzz::asset::AssetManager::UnloadAll();
    app.Shutdown();
    return 0;
}
```

---

## 4. `ToolchainLocator` の修正

### 現状の問題

`ToolchainLocator::Locate()` は「exe 隣の `build.config`」だけを探す。  
EditorLauncher の exe 隣にはユーザープロジェクトの `build.config` は存在しない。

### 修正: プロジェクトの `build_root` を参照する

`BuildPipeline::Start()` が `build_root` を受け取り、`ToolchainLocator` に渡す。

```cpp
// BuildPipeline.hpp (変更)
void Start(const BuildSettings& settings,
           const std::string& projectRoot,
           const std::string& buildRoot);   // ← .fbzz_proj の build_root を追加
```

```cpp
// ToolchainLocator::Result Locate() → Locate(buildRoot) に変更

class ToolchainLocator {
public:
    /// @param buildRoot .fbzz_proj の build_root をプロジェクトルートで解決した絶対パス
    [[nodiscard]] static Result Locate(const std::filesystem::path& buildRoot);

private:
    // 探索優先順位:
    // 1. buildRoot/build.config  ← ユーザープロジェクトの cmake キャッシュ
    // 2. PATH 上の cmake.exe
    // 3. vswhere.exe でフォールバック
    static Result LocateFromBuildConfig(const std::filesystem::path& buildRoot);
    static Result LocateFromPath();
    static Result LocateFromVSInstall();
};
```

```cpp
// BuildPipeline::Tick() の Compile ステップ (修正)
ToolchainLocator::Result toolchain = ToolchainLocator::Locate(m_buildRoot);
```

### `EditorContext` への追加

`build_root` は `.fbzz_proj` から読んで `EditorContext` に持たせる。

```cpp
// EditorContext.hpp に追加
std::string buildRoot;  // .fbzz_proj の build_root を解決した絶対パス
```

```cpp
// EditorApp::OpenProject() 内
ctx.buildRoot = ResolvePath(projectRoot, fbzzProj["project"]["build_root"]);
```

---

## 5. `BuildPipeline` の `target` 名の解決

Sandbox では `SandboxGame` 固定だったが、GameHub プロジェクトではターゲット名が  
`{{TARGET_NAME}}Game` (プレースホルダ展開後) になる。

`.fbzz_proj` の `target_name` フィールドから取得する:

```toml
# MyGame/.fbzz_proj
[project]
target_name = "MyGame"
```

```cpp
// BuildPipeline::Compile ステップ
cfg.target = ctx.projectSettings.project.targetName + "Game";
// → "MyGame" + "Game" = "MyGameGame" ... は変なので
```

> **注意**: ターゲット名のサフィックス問題。`{{TARGET_NAME}}Game` が `MyGame` から  
> `MyGameGame` になってしまう。以下のいずれかで対処:
> - `.fbzz_proj` に `standalone_target_name = "{{TARGET_NAME}}Standalone"` フィールドを追加
> - または `target_name` と `standalone_target_name` を別々に管理

推奨案: `.fbzz_proj` にフィールドを追加。

```toml
[project]
target_name          = "MyGame"          # 開発用 (エディタあり)
standalone_target_name = "MyGameStandalone"  # 配布用 (エディタなし)
```

テンプレートの `CMakeLists.txt` では:

```cmake
add_executable({{TARGET_NAME}}Standalone
    Src/main_standalone.cpp
    ...
)
```

---

## 6. 変更ファイル一覧

| ファイル | 変更内容 |
|----------|---------|
| `Templates/standard/CMakeLists.txt` | `{{TARGET_NAME}}Standalone` ターゲット追加 + `configure_file` |
| `Templates/standard/Src/AppMain.cpp` | `main_editor.cpp` + `main_standalone.cpp` に分割 |
| `Templates/standard/Src/StandaloneModule.hpp/.cpp` | 新規 (IModule 実装) |
| `Templates/standard/Src/EditorModule.hpp/.cpp` | 新規 (IModule 実装) |
| `Templates/standard/.fbzz_proj` | `standalone_target_name` フィールド追加 |
| `Editor/include/Editor/ToolchainLocator.hpp` | `Locate(buildRoot)` に変更 |
| `Editor/src/ToolchainLocator.cpp` | buildRoot を受け取る実装に変更 |
| `Editor/include/Editor/BuildPipeline.hpp` | `Start()` に `buildRoot` 引数追加 |
| `Editor/src/BuildPipeline.cpp` | `buildRoot` を `ToolchainLocator` に渡す |
| `Editor/include/Editor/EditorContext.hpp` | `buildRoot` / `standaloneTargetName` フィールド追加 |
| `Editor/src/EditorApp.cpp` | `OpenProject()` で `buildRoot` を解決して `ctx` に格納 |

---

## 7. Sandbox との役割分担

| | Sandbox | GameHub プロジェクト |
|--|---------|-------------------|
| 用途 | エンジン機能のテスト | ユーザーの実際のゲーム |
| IModule | `StandaloneModule` / `EditorModule` | テンプレートの同名クラス |
| ビルドターゲット | `SandboxGame` | `{{TARGET_NAME}}Standalone` |
| `build.config` の場所 | Sandbox ビルドディレクトリ | プロジェクトの `build_root` |
| パッケージの主体 | Engine 開発者のみ | GameHub ユーザー全員 |

**Sandbox の `build.config` 生成はエンジン開発者向けの動作確認用**に留め、  
本番のビルドパイプラインは GameHub プロジェクトの `build_root` を中心に設計する。
