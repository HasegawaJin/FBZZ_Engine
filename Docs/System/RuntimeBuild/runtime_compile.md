# 実行中コンパイル 設計書

エディタの [Build] ボタンから `cmake --build` を呼び出し、  
`SandboxStandalone.exe`（EditorModule なし）をパッケージング時にその場でビルドする。

---

## 1. 全体像

```
[Build ボタン]
    │
    ▼
BuildPipeline::Start()
    │
    ├─ Step: Compile        ← cmake --build でその場でコンパイル (新規)
    │     └─ Compiler::Tick() で 1 フレームずつ stdout を読む
    │
    ├─ Step: CopyExecutable ← ビルド成果物 SandboxStandalone.exe をコピー (変更)
    ├─ Step: CopyDlls
    ├─ Step: CopyAssets     (既存・変更なし)
    ├─ Step: CopyProjectFiles
    ├─ Step: WriteManifest
    └─ Step: CommitOutput
```

---

## 2. CMake 側の変更

### 2-1. `SandboxStandalone` ターゲット追加

```cmake
# Projects/Sandbox/CMakeLists.txt

# 既存: エディタ付き開発用
add_executable(Sandbox
    src/main_editor.cpp
    src/StandaloneModule.cpp
    src/EditorModule.cpp
    # ...
)
target_link_libraries(Sandbox PRIVATE Engine Editor)
target_compile_definitions(Sandbox PRIVATE FBZZ_EDITOR)

# 新規: パッケージ用 (EditorModule なし)
add_executable(SandboxStandalone
    src/main_standalone.cpp
    src/StandaloneModule.cpp
    # EditorModule.cpp は含まない
)
target_link_libraries(SandboxStandalone PRIVATE Engine)
# FBZZ_EDITOR マクロなし → Editor ヘッダが一切コンパイルされない
```

### 2-2. ビルドディレクトリパスの生成

コンパイル時に `build.config` を生成し、エディタが cmake のキャッシュ場所を知れるようにする。

```cmake
# CMakeLists.txt (ルート)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/build.config.in"
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/build.config"
    @ONLY
)
```

```ini
# cmake/build.config.in → build.config (自動生成)
cmake_exe=@CMAKE_COMMAND@
build_dir=@CMAKE_BINARY_DIR@
exe_debug=@CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG@/SandboxStandalone.exe
exe_release=@CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE@/SandboxStandalone.exe
```

実行時に `build.config` (exe 隣) を読むだけで cmake.exe・build_dir・成果物パスが分かる。

> **WHY — `exe_debug` / `exe_release` を明示する**:  
> Visual Studio はマルチコンフィグジェネレーターなので `build_dir` は Debug/Release で共通。  
> 成果物パスは `build_dir/Debug/SandboxStandalone.exe` と `build_dir/Release/SandboxStandalone.exe` に分かれる。  
> `CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG/RELEASE` を展開して記録しておけば、  
> `GetOutputExePath()` がビルド設定に関係なく正確なパスを返せる。

---

## 3. `ToolchainLocator` クラス

cmake.exe とビルドディレクトリを特定する。

**ファイル**: `Editor/include/Editor/ToolchainLocator.hpp`

```cpp
namespace fbzz::editor {

class ToolchainLocator {
public:
    struct Result {
        std::filesystem::path cmakeExe;      // cmake.exe のフルパス
        std::filesystem::path buildDir;      // cmake キャッシュのあるディレクトリ
        std::filesystem::path exeDebug;      // build.config の exe_debug
        std::filesystem::path exeRelease;    // build.config の exe_release
        bool                  found = false;
        std::string           error;
    };

    /// exe 隣の build.config を読んで cmake.exe と build_dir を特定する。
    /// build.config がない場合は PATH と VS インストールからフォールバック探索する。
    [[nodiscard]] static Result Locate();

private:
    static Result LocateFromBuildConfig(const std::filesystem::path& exeDir);
    static Result LocateFromPath();
    static Result LocateFromVSInstall();
};

} // namespace fbzz::editor
```

### 探索優先順位

```
1. exe 隣の build.config (configure_file で生成) ← 最優先・確実
2. PATH 上の cmake.exe + exe 隣の build/ ディレクトリ推測
3. vswhere.exe でVSインストール検索
   → %ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
   → VS付属の cmake: <VS_INSTALL>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

---

## 4. `Compiler` クラス

`cmake --build` を子プロセスとして起動し、stdout を非同期で読む。

**ファイル**: `Editor/include/Editor/Compiler.hpp`

```cpp
namespace fbzz::editor {

class Compiler {
public:
    enum class State { Idle, Building, Done, Failed, Cancelled };

    struct Config {
        std::filesystem::path cmakeExe;
        std::filesystem::path buildDir;
        std::filesystem::path exePath;         // build.config から読んだ成果物パス
        std::string           target;          // "SandboxStandalone"
        std::string           configuration;   // "Release"
    };

    Compiler() = default;
    ~Compiler();

    // WHY: Win32 ハンドルを一意に所有するためコピー禁止。
    Compiler(const Compiler&)            = delete;
    Compiler& operator=(const Compiler&) = delete;

    /// ビルドを開始する。成功で true。既に Building 中なら false。
    [[nodiscard]] bool Start(const Config& config);

    /// 毎フレーム呼ぶ。stdout を 1 チャンク読んで m_log に追記する。
    /// 子プロセスが終了したら State を Done / Failed に遷移させる。
    void Tick();

    /// 実行中のビルドをキャンセルする。ハンドルを閉じて Cancelled に遷移する。
    void Cancel();

    State              GetState()    const { return m_state; }
    const std::string& GetLog()      const { return m_log; }
    int                GetExitCode() const { return m_exitCode; }

    /// ビルド成果物 exe のパスを返す。Done 状態でのみ有効。
    /// build.config に記録された exe_debug / exe_release から取得する。
    std::filesystem::path GetOutputExePath() const { return m_config.exePath; }

private:
    /// PeekNamedPipe で利用可能なバイト数を確認してから ReadFile する (非ブロッキング)。
    void PollOutput();
    void CloseHandles();

    State       m_state    = State::Idle;
    std::string m_log;
    int         m_exitCode = 0;
    Config      m_config;

    // Win32 ハンドル
    HANDLE m_hProcess    = INVALID_HANDLE_VALUE;
    HANDLE m_hStdoutRead = INVALID_HANDLE_VALUE;
};

} // namespace fbzz::editor
```

### 子プロセス起動 (`Compiler.cpp`)

```cpp
bool Compiler::Start(const Config& config)
{
    if (m_state == State::Building) return false;

    m_config   = config;
    m_log      = {};
    m_exitCode = 0;
    m_state    = State::Building;

    // cmake --build <buildDir> --target SandboxStandalone --config Release
    std::wstring cmd =
        L"\"" + config.cmakeExe.wstring() + L"\""
        L" --build \"" + config.buildDir.wstring() + L"\""
        L" --target " + Utf8ToWide(config.target) +
        L" --config " + Utf8ToWide(config.configuration);

    // stdout / stderr を同一パイプに束ねる (エラーもログに流す)
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    CreatePipe(&m_hStdoutRead, &hWrite, &sa, 0);
    SetHandleInformation(m_hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr, cmd.data(), nullptr, nullptr,
        TRUE,                   // bInheritHandles
        CREATE_NO_WINDOW,       // コンソールウィンドウを表示しない
        nullptr, nullptr, &si, &pi);

    CloseHandle(hWrite);        // 子プロセスが所有したので親は閉じる

    if (!ok) {
        m_state = State::Failed;
        m_log   = "CreateProcess failed.";
        return false;
    }

    m_hProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
}
```

### 非ブロッキング出力読み取り (`Tick`)

```cpp
void Compiler::Tick()
{
    if (m_state != State::Building) return;

    PollOutput();

    // 子プロセスの終了を確認 (ブロックしない)
    if (WaitForSingleObject(m_hProcess, 0) == WAIT_OBJECT_0) {
        PollOutput();  // 残りの出力を回収

        DWORD exitCode = 0;
        GetExitCodeProcess(m_hProcess, &exitCode);
        m_exitCode = static_cast<int>(exitCode);
        m_state    = (exitCode == 0) ? State::Done : State::Failed;

        CloseHandle(m_hProcess);
        CloseHandle(m_hStdoutRead);
        m_hProcess    = INVALID_HANDLE_VALUE;
        m_hStdoutRead = INVALID_HANDLE_VALUE;
    }
}

void Compiler::PollOutput()
{
    DWORD available = 0;
    while (PeekNamedPipe(m_hStdoutRead, nullptr, 0, nullptr, &available, nullptr)
           && available > 0)
    {
        std::string buf(available, '\0');
        DWORD read = 0;
        ReadFile(m_hStdoutRead, buf.data(), available, &read, nullptr);
        m_log.append(buf.data(), read);
    }
}
```

### デストラクタ・ハンドル解放

```cpp
Compiler::~Compiler()
{
    CloseHandles();
}

void Compiler::CloseHandles()
{
    if (m_hProcess    != INVALID_HANDLE_VALUE) { CloseHandle(m_hProcess);    m_hProcess    = INVALID_HANDLE_VALUE; }
    if (m_hStdoutRead != INVALID_HANDLE_VALUE) { CloseHandle(m_hStdoutRead); m_hStdoutRead = INVALID_HANDLE_VALUE; }
}
```

### キャンセル

```cpp
void Compiler::Cancel()
{
    if (m_state != State::Building) return;
    TerminateProcess(m_hProcess, 1);
    // WHY: TerminateProcess は非同期。プロセスが終了するまで待ってからハンドルを閉じる。
    //      WaitForSingleObject(INFINITE) はフレームをブロックするが、
    //      TerminateProcess 後の終了は通常数ミリ秒以内に完了する。
    WaitForSingleObject(m_hProcess, INFINITE);
    CloseHandles();
    m_state = State::Cancelled;
}
```

---

## 5. `BuildPipeline` の変更

### Step 追加

```cpp
enum class Step {
    Compile,           // ← 新規 (複数フレーム)
    CopyExecutable,    // 変更: ビルド成果物をコピー
    CopyDlls,
    EnumerateAssets,
    CopyAssets,        // 複数フレーム
    CopyProjectFiles,
    WriteManifest,
    CommitOutput,
    Done,
};
```

### `Compile` ステップ

```cpp
// BuildPipeline.hpp に追加
Compiler m_compiler;
```

```cpp
// BuildPipeline::Tick() の Compile ステップ処理
case Step::Compile: {
    if (m_compiler.GetState() == Compiler::State::Idle) {
        // 初回 Tick で Start する
        ToolchainLocator::Result toolchain = ToolchainLocator::Locate();
        if (!toolchain.found) {
            m_error = "cmake.exe が見つかりません: " + toolchain.error;
            m_state = State::Failed;
            return;
        }

        Compiler::Config cfg;
        cfg.cmakeExe      = toolchain.cmakeExe;
        cfg.buildDir      = toolchain.buildDir;
        cfg.target        = "SandboxStandalone";
        cfg.configuration = m_settings.developmentBuild ? "Debug" : "Release";
        // build.config に記録された成果物パスを設定 (マルチコンフィグ対応)
        cfg.exePath = m_settings.developmentBuild ? toolchain.exeDebug : toolchain.exeRelease;

        if (!m_compiler.Start(cfg)) {
            m_error = "コンパイルを開始できませんでした。";
            m_state = State::Failed;
            return;
        }
        m_status = "Compiling SandboxStandalone...";
        return;
    }

    m_compiler.Tick();
    m_status = "Compiling... (Build log を確認)";

    if (m_compiler.GetState() == Compiler::State::Done) {
        m_step = Step::CopyExecutable;
    } else if (m_compiler.GetState() == Compiler::State::Failed) {
        m_error = "コンパイル失敗 (exit code " + std::to_string(m_compiler.GetExitCode()) + ")";
        m_state = State::Failed;
    }
    return;
}
```

### `CopyExecutable` ステップの変更

```cpp
// 変更前: 自身 (エディタ exe) をコピー
// 変更後: ビルド成果物 SandboxStandalone.exe をコピー
case Step::CopyExecutable: {
    const std::filesystem::path src = m_compiler.GetOutputExePath();
    const std::filesystem::path dst = m_tmpDir / (m_settings.productName + ".exe");
    // ...
}
```

---

## 6. UI の変更 (`BuildSettingsPanel`)

### ビルドログ表示

```cpp
void BuildSettingsPanel::DrawProgressAndActions(EditorContext& ctx)
{
    // ... 既存の Build / Build and Run ボタン ...

    if (isBuilding) {
        ImGui::ProgressBar(m_pipeline.GetProgress(), ImVec2(-1, 0), m_pipeline.GetStatus());

        // ビルドログを折りたたみ表示
        if (ImGui::CollapsingHeader("Build Log")) {
            ImGui::InputTextMultiline(
                "##log",
                const_cast<char*>(m_pipeline.GetBuildLog().c_str()),
                m_pipeline.GetBuildLog().size(),
                ImVec2(-1, 200),
                ImGuiInputTextFlags_ReadOnly);

            // 末尾に自動スクロール
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }

        // キャンセルボタン
        if (ImGui::Button("Cancel")) {
            m_pipeline.Cancel();
        }

        m_pipeline.Tick();
    }
}
```

---

## 7. 設計上の決定と理由

| 項目 | 判断 | 理由 |
|------|------|------|
| `cmake --build` を使う | MSBuild 直呼びではなく cmake 経由 | CMake がツールチェーンを既にキャッシュしており、コマンドが環境に依存しない |
| `build.config` で cmake パスを渡す | vswhere 探索よりも優先 | configure_file で生成するため確実。vswhere はフォールバック扱い |
| `CREATE_NO_WINDOW` フラグ | コンソールウィンドウを非表示 | エディタ中にコンソールが出現しない |
| `PeekNamedPipe` で非ブロッキング読み取り | `ReadFile` の直呼びではなく | エディタのフレームをブロックしない |
| stdout と stderr を同じパイプに束ねる | 2 本のパイプではなく 1 本 | エラーもログ順序通りに流れる。読み取りロジックが単純になる |
| キャンセルは `TerminateProcess` | 子プロセスに Ctrl+C を送らない | 子の cmake がさらに子 (cl.exe) を起動するため、シグナルの伝播が不安定 |

---

## 8. ファイル一覧

| ファイル | 役割 |
|----------|------|
| `cmake/build.config.in` | configure_file テンプレート |
| `Editor/include/Editor/ToolchainLocator.hpp` | cmake.exe / build_dir 特定 |
| `Editor/src/ToolchainLocator.cpp` | build.config 読み取り + vswhere フォールバック |
| `Editor/include/Editor/Compiler.hpp` | cmake --build 子プロセス管理 |
| `Editor/src/Compiler.cpp` | CreateProcess + PeekNamedPipe 実装 |
| `Editor/include/Editor/BuildPipeline.hpp` | `Compile` ステップ + `Compiler` メンバ追加 |
| `Editor/src/BuildPipeline.cpp` | `Compile` ステップ処理 + `CopyExecutable` 変更 |
| `Editor/src/Panels/BuildSettingsPanel.cpp` | ビルドログ表示 + キャンセルボタン |
| `Projects/Sandbox/CMakeLists.txt` | `SandboxStandalone` ターゲット追加 |
| `CMakeLists.txt` | `configure_file` 追加 |

---

## 9. 実装ロードマップ

| フェーズ | タスク |
|----------|--------|
| **Phase 1** | CMake に `SandboxStandalone` ターゲット追加 + `build.config.in` 作成 |
| **Phase 2** | `ToolchainLocator` 実装 (build.config 読み取り + vswhere フォールバック) |
| **Phase 3** | `Compiler` 実装 (CreateProcess + PeekNamedPipe + Cancel) |
| **Phase 4** | `BuildPipeline` に `Compile` ステップを組み込み |
| **Phase 5** | `BuildSettingsPanel` にビルドログ表示とキャンセルボタンを追加 |
