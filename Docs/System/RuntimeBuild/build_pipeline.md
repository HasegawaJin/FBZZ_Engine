# FBZZ Engine — Build Settings パネル & ビルドパイプライン 設計書

## 1. 概要

Unity の「File → Build Settings」に相当する UI をエディタに追加し、  
ボタン一発でゲームを配布可能な形にパッケージングできるようにする。

「ビルド」の実体は **コンパイル済みバイナリ + アセットのパッケージング**。  
C++ の再コンパイルは行わない（MSVC 環境変数がエディタプロセスに存在しないため）。

---

## 2. UI 設計

### 2-1. メニュー統合

```
File
  ├─ New Scene
  ├─ Open Scene
  ├─ Save Scene
  ├─ Save Scene As...
  ├─ ─────────────
  ├─ Build Settings...    ← 新規 (Ctrl+Shift+B)
  └─ Exit
```

### 2-2. Build Settings パネルレイアウト

```
┌─ Build Settings ──────────────────────────────────────────────────┐
│                                                                    │
│  Scenes in Build                                                   │
│  ┌────────────────────────────────────────────────────────────┐   │
│  │  #  Path                              Enabled              │   │
│  │  0  Scenes/MainMenu.fbzz              ☑   (← スタートシーン) │ │
│  │  1  Scenes/GameLevel01.fbzz           ☑                   │   │
│  │  2  Scenes/TestLevel.fbzz             ☐                   │   │
│  └────────────────────────────────────────────────────────────┘   │
│  [+ Add Open Scene]  [− Remove Selected]                           │
│  ※ インデックス 0 のシーンがスタートシーン                          │
│                                                                    │
│  ─────────────────────────────────────────────────────────────    │
│                                                                    │
│  Platform          Windows x64  (固定)                             │
│                                                                    │
│  Output Directory  Builds/MyGame           [Browse...]             │
│  (プロジェクトルートからの相対パス、または絶対パス)                  │
│                                                                    │
│  Product Name      My Awesome Game                                 │
│  Version           1.0.0                                           │
│                                                                    │
│  Options                                                           │
│  ☑  Development Build  (ログ・デバッグ情報を有効化)               │
│                                                                    │
│  ────────────────────────────────────────────────────────────     │
│                                                                    │
│  Progress: [████████████░░░░░░░░]  Copying assets... (42/128)      │
│                                                                    │
│  ⚠ Play 中はビルドできません          [Build]   [Build and Run]   │
└────────────────────────────────────────────────────────────────────┘
```

---

## 3. データ設計

### 3-1. BuildSettings 構造体

プロジェクトルートの `BuildSettings.toml` にシリアライズする。

```cpp
// Projects/Editor/include/Editor/BuildSettings.hpp
#pragma once
#include <string>
#include <vector>

namespace fbzz::editor {

struct SceneEntry {
    std::string path;       // プロジェクトルートからの相対パス
    bool        enabled = true;
};

struct BuildSettings {
    std::vector<SceneEntry> scenes;
    std::string outputDirectory;  // プロジェクトルート相対パス。絶対パスも許容。
    std::string productName  = "MyGame";
    std::string version      = "1.0.0";
    bool        developmentBuild = true;

    bool Save(const std::string& projectRoot) const;
    bool Load(const std::string& projectRoot);

    // シーンのうち enabled=true のものを順番に返す
    std::vector<std::string> EnabledScenes() const;
};

} // namespace fbzz::editor
```

**`BuildSettings.toml` のフォーマット:**

```toml
[build]
product_name = "MyGame"
version      = "1.0.0"
output_dir   = "Builds/MyGame"    # プロジェクトルートからの相対パス推奨
development  = true

[[scenes]]
path    = "Scenes/MainMenu.fbzz"  # index 0 = スタートシーン
enabled = true

[[scenes]]
path    = "Scenes/GameLevel01.fbzz"
enabled = true

[[scenes]]
path    = "Scenes/TestLevel.fbzz"
enabled = false
```

### 3-2. outputDirectory の解決

絶対パスと相対パスの両方を受け付けるが、**保存時は相対パスを優先する**。  
プロジェクトを別 PC に持っていっても動くように。

```cpp
std::filesystem::path BuildSettings::ResolveOutputPath(const std::string& projectRoot) const
{
    std::filesystem::path out(outputDirectory);
    if (out.is_absolute()) return out;
    return std::filesystem::path(projectRoot) / out;
}
```

### 3-3. Development Build フラグの扱い

コンパイルは走らないため、コンパイル時マクロとしては機能しない。  
代わりに `game.manifest.toml` に書き出し、`StandaloneApp` がランタイムで読んでログレベルを制御する。

```toml
# game.manifest.toml (ビルド時に自動生成)
product_name     = "MyGame"
version          = "1.0.0"
engine_version   = "0.1.0"
build_date       = "2026-05-31"
development      = true          # true のとき Logger の出力を VERBOSE に設定
```

```cpp
// StandaloneApp::Init() 内
if (manifest.development) {
    core::Logger::SetLevel(core::LogLevel::Verbose);
} else {
    core::Logger::SetLevel(core::LogLevel::Warning);
}
```

---

## 4. クラス設計

### 4-1. BuildSettingsPanel

`IPanel` を継承し、Build Settings ウィンドウを担当する。

```
Projects/Editor/include/Editor/Panels/BuildSettingsPanel.hpp
Projects/Editor/src/Panels/BuildSettingsPanel.cpp
```

```cpp
// BuildSettingsPanel.hpp
#pragma once
#include <Editor/Panels/IPanel.hpp>
#include <Editor/BuildSettings.hpp>
#include <Editor/BuildPipeline.hpp>

namespace fbzz::editor {

class BuildSettingsPanel final : public IPanel {
public:
    const char* GetWindowName()  const override { return "Build Settings"; }
    bool        ShowInViewMenu() const override { return false; }
    bool        CanClose()       const override { return true; }

    void OnInit(EditorContext& ctx) override;
    void OnRenderContent(EditorContext& ctx) override;

private:
    void DrawScenesInBuild(EditorContext& ctx);
    void DrawOutputSettings();
    void DrawProgressAndActions(EditorContext& ctx);

    BuildSettings m_settings;
    BuildPipeline m_pipeline;
    int           m_selectedSceneIdx = -1;
    bool          m_settingsLoaded   = false;
};

} // namespace fbzz::editor
```

**EditorApp への統合:**

`EditorApp.hpp` には `FindPanel<T>()` が存在しない。  
`ProjectSettingsPanel` と同様に、`m_buildSettingsPanel` として生ポインタを持つ。

```cpp
// EditorApp.hpp private メンバに追加
BuildSettingsPanel* m_buildSettingsPanel = nullptr;

// EditorApp.cpp Init() 内
auto panel = std::make_unique<BuildSettingsPanel>();
m_buildSettingsPanel = panel.get();
m_panels.push_back(std::move(panel));

// RenderPanels() 内
if (ctx.requestOpenBuildSettings) {
    ctx.requestOpenBuildSettings = false;
    if (m_buildSettingsPanel) {
        m_buildSettingsPanel->visible = true;
        ImGui::SetNextWindowFocus();
    }
}
```

### 4-2. BuildPipeline

パッケージング処理を担うステートマシン。  
シングルスレッド制約（AGENTS.md）に準拠し `std::thread` は使わない。

**フリーズを防ぐ設計:** `CopyAssets` ステップは `std::filesystem::copy_directory` を  
1 回の `Tick()` で呼ばない。代わりにファイルイテレータを `BuildPipeline` のメンバとして保持し、  
`Tick()` のたびに 1 ファイルずつコピーすることで ImGui をブロックしない。

```
Projects/Editor/include/Editor/BuildPipeline.hpp
Projects/Editor/src/BuildPipeline.cpp
```

```cpp
// BuildPipeline.hpp
#pragma once
#include <Editor/BuildSettings.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace fbzz::editor {

class BuildPipeline {
public:
    enum class State { Idle, Running, Done, Failed };

    void Start(const BuildSettings& settings, const std::string& projectRoot);

    // 1 フレームに 1 ファイル (CopyAssets ステップ) または 1 ステップ進める。
    // BuildSettingsPanel::OnRenderContent() から毎フレーム呼ぶ。
    void Tick();

    State       GetState()       const { return m_state; }
    float       GetProgress()    const { return m_progress; }
    const char* GetStatus()      const { return m_status.c_str(); }
    const char* GetError()       const { return m_error.c_str(); }
    bool        WantsRunAfter()  const { return m_runAfterBuild; }
    std::string GetOutputExePath() const;

private:
    enum class Step {
        PrepareTempDir,   // 一時ディレクトリを用意する
        CopyExecutable,
        CopyDlls,
        EnumerateAssets,  // コピー対象ファイル一覧を収集する
        CopyAssets,       // ファイルを 1 つずつコピー (複数フレーム)
        CopyProjectFiles,
        WriteManifest,
        CommitOutput,     // tmp → 出力先に atomic rename
        Done,
    };

    bool ExecuteStep();    // 現在のステップを 1 フレーム分進める。完了で true。

    // CopyAssets 専用ヘルパー
    void BeginEnumerateAssets();
    bool TickCopyOneFile();  // 1 ファイルコピー。ファイルがなくなれば false。

    State       m_state    = State::Idle;
    Step        m_step     = Step::PrepareTempDir;
    float       m_progress = 0.0f;
    std::string m_status;
    std::string m_error;
    bool        m_runAfterBuild = false;

    std::string m_projectRoot;
    BuildSettings m_settings;

    std::filesystem::path m_outputDir;  // 解決済みの出力先
    std::filesystem::path m_tmpDir;     // 作業用一時ディレクトリ (outputDir + "_tmp")
    std::filesystem::path m_exeSrcPath; // GetModuleFileNameW() で取得した自身のパス

    // CopyAssets ステップ用
    std::vector<std::filesystem::path> m_assetFiles;  // コピー対象ファイル一覧
    size_t                             m_assetIdx = 0;
};

} // namespace fbzz::editor
```

---

## 5. ビルドパイプライン処理フロー

### アトミック設計

出力先を直接クリアせず、**一時ディレクトリ (`outputDir + "_tmp"`)** に全ファイルを出力し、  
完了後に rename する。コピー中にエラーが起きても旧ビルドは保持される。

```
[Step 1] PrepareTempDir
  一時ディレクトリを準備する
  └─ m_tmpDir = m_outputDir.string() + "_tmp"
  └─ 既存の _tmp を削除してから create_directories

[Step 2] CopyExecutable
  自身の exe を <ProductName>.exe として _tmp にコピー
  └─ GetModuleFileNameW() で m_exeSrcPath を取得
  └─ copy_file(exeSrc, tmpDir / (productName + ".exe"))

[Step 3] CopyDlls
  必要な DLL を _tmp にコピー
  └─ exeDir / "assimp-vc145-mt.dll" → tmpDir

[Step 4] EnumerateAssets
  コピー対象ファイルを m_assetFiles に列挙する (1 フレームで完了)
  └─ exeDir / "assets" 以下の全ファイルを recursive_directory_iterator で収集

[Step 5] CopyAssets  ← 複数フレームにまたがる
  m_assetFiles を 1 フレームに 1 ファイルずつコピー
  └─ 宛先パス: tmpDir / "assets" / ソースからの相対パス
  └─ m_progress をファイル数で細分化

[Step 6] CopyProjectFiles
  プロジェクト定義をコピー
  └─ projectRoot / ".fbzz_proj"          → tmpDir
  └─ projectRoot / "ProjectSettings.toml" → tmpDir
  └─ enabled=true のシーンを projectRoot / scenes → tmpDir / assets / scenes

[Step 7] WriteManifest
  game.manifest.toml を _tmp に生成
  └─ product_name, version, engine_version, build_date, development フラグ

[Step 8] CommitOutput
  _tmp → outputDir に rename (アトミック)
  └─ outputDir が存在すれば先に削除
  └─ std::filesystem::rename(tmpDir, outputDir)

[Step Done] 完了
  State = Done
  WantsRunAfter() == true の場合、呼び出し元が CreateProcess で起動する
```

---

## 6. ステートマシンの Tick() 実装

```cpp
void BuildPipeline::Tick()
{
    if (m_state != State::Running) return;

    // CopyAssets だけは 1 Tick = 1 ファイル
    if (m_step == Step::CopyAssets) {
        if (!TickCopyOneFile()) {
            // 全ファイルコピー完了 → 次ステップへ
            m_step = Step::CopyProjectFiles;
        }
        return;
    }

    // それ以外は 1 Tick = 1 ステップ
    if (!ExecuteStep()) {
        m_state = State::Failed;
        return;
    }

    m_step = static_cast<Step>(static_cast<int>(m_step) + 1);

    // EnumerateAssets が終わったら CopyAssets の準備
    if (m_step == Step::CopyAssets) {
        BeginEnumerateAssets();
        m_assetIdx = 0;
    }

    if (m_step == Step::Done) {
        m_state = State::Done;
        m_progress = 1.0f;
    }
}

bool BuildPipeline::TickCopyOneFile()
{
    if (m_assetIdx >= m_assetFiles.size()) return false;

    const auto& src = m_assetFiles[m_assetIdx];
    // tmpDir / "assets" / ソースの exeDir/assets 以下の相対パス
    const std::filesystem::path exeAssetsDir = m_exeSrcPath.parent_path() / "assets";
    const std::filesystem::path rel = std::filesystem::relative(src, exeAssetsDir);
    const std::filesystem::path dst = m_tmpDir / "assets" / rel;

    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);

    if (ec) {
        m_error = "Failed to copy: " + src.string();
        m_state = State::Failed;
        return false;
    }

    ++m_assetIdx;
    m_progress = static_cast<float>(m_assetIdx) / static_cast<float>(m_assetFiles.size());
    m_status   = "Copying assets... (" + std::to_string(m_assetIdx)
                 + "/" + std::to_string(m_assetFiles.size()) + ")";
    return true;
}
```

---

## 7. BuildSettingsPanel の Play 中ビルド禁止

Play 中はビルドボタンを無効化する。

```cpp
void BuildSettingsPanel::DrawProgressAndActions(EditorContext& ctx)
{
    const bool isPlaying = ctx.playMode && ctx.playMode->IsPlaying();
    const bool isBuilding = m_pipeline.GetState() == BuildPipeline::State::Running;

    if (isPlaying) {
        ImGui::TextDisabled("Play 中はビルドできません");
    }

    ImGui::BeginDisabled(isPlaying || isBuilding);

    if (ImGui::Button("Build")) {
        m_settings.Save(ctx.projectRoot);
        m_pipeline.Start(m_settings, ctx.projectRoot);
    }
    ImGui::SameLine();
    if (ImGui::Button("Build and Run")) {
        m_settings.Save(ctx.projectRoot);
        m_pipeline.Start(m_settings, ctx.projectRoot);
        // WantsRunAfter フラグは Pipeline 内部で保持、Done 時に BuildSettingsPanel が起動
    }

    ImGui::EndDisabled();

    // 進捗バー
    if (isBuilding) {
        ImGui::ProgressBar(m_pipeline.GetProgress(), ImVec2(-1, 0), m_pipeline.GetStatus());
        m_pipeline.Tick();
    }

    // ビルド完了後の "Build and Run" 処理
    if (m_pipeline.GetState() == BuildPipeline::State::Done && m_pipeline.WantsRunAfter()) {
        StandaloneLauncher::LaunchExe(m_pipeline.GetOutputExePath(), "");
        // State をリセット (重複起動を防ぐ)
        m_pipeline.Reset();
    }

    if (m_pipeline.GetState() == BuildPipeline::State::Failed) {
        ImGui::TextColored({ 1, 0.3f, 0.3f, 1 }, "Error: %s", m_pipeline.GetError());
    }
}
```

---

## 8. EditorContext への統合

```cpp
// EditorContext.hpp に追加
bool requestOpenBuildSettings = false;
```

---

## 9. 出力フォルダ構成

```
<outputDir>/
├── MyGame.exe               ← FBZZEditor.exe のコピー
├── assimp-vc145-mt.dll
├── .fbzz_proj
├── ProjectSettings.toml
├── game.manifest.toml       ← development フラグ・バージョン情報
└── assets/
    ├── shaders/             ← コンパイル済み .cso
    ├── textures/
    ├── models/
    ├── sounds/
    └── scenes/              ← enabled=true のシーンのみ
```

---

## 10. 実装ファイル一覧

| ファイル | 役割 |
|----------|------|
| `Editor/include/Editor/BuildSettings.hpp` | BuildSettings 構造体 + TOML シリアライズ宣言 |
| `Editor/src/BuildSettings.cpp` | toml++ を使った Save / Load 実装 |
| `Editor/include/Editor/BuildPipeline.hpp` | ステートマシン宣言 |
| `Editor/src/BuildPipeline.cpp` | 各ステップの filesystem 操作・ファイルイテレータ実装 |
| `Editor/include/Editor/Panels/BuildSettingsPanel.hpp` | IPanel サブクラス宣言 |
| `Editor/src/Panels/BuildSettingsPanel.cpp` | ImGui UI 実装 |
| `Editor/include/Editor/EditorApp.hpp` | `m_buildSettingsPanel` 生ポインタ追加 |
| `Editor/src/EditorApp.cpp` | パネル登録 + one-shot フラグ処理 |
| `Editor/src/EditorApp_MenuBar.cpp` | "Build Settings..." メニュー項目追加 |
| `Editor/include/Editor/EditorContext.hpp` | `requestOpenBuildSettings` フラグ追加 |

---

## 11. 実装ロードマップ

| フェーズ | タスク |
|----------|--------|
| **Phase 1** | `BuildSettings` 構造体 + TOML シリアライズ |
| **Phase 1** | `BuildPipeline` ステートマシン (ファイルイテレータ + アトミック rename) |
| **Phase 2** | `BuildSettingsPanel` ImGui UI |
| **Phase 2** | `EditorApp` への統合 (生ポインタ + one-shot フラグ) |
| **Phase 2** | MenuBar に "Build Settings..." 追加 |
| **Phase 3** | `game.manifest.toml` 生成 + `StandaloneApp` でのランタイム読み取り |
| **Phase 3** | "Build and Run" (`StandaloneLauncher::LaunchExe`) |
