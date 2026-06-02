# 設計上の未決事項 — 決定記録

実装開始前に確定した設計上の判断を記録する。  
変更する場合はここを更新し、理由を残すこと。

---

## ① `EditorLauncher::StandaloneApp` の扱い

### 決定: `IModule` を実装する形に変換する

`StandaloneApp` クラスを `IModule` を実装した `StandaloneModule` に作り直す。  
EditorLauncher 内に配置し、テンプレートの `StandaloneModule` とは別物として共存させる。

```
EditorLauncher/src/StandaloneModule.hpp/.cpp
    ← 旧 StandaloneApp を IModule 実装に変換
    ← 「任意のプロジェクトを汎用的に実行する」EditorLauncher 固有の責務

Templates/standard/Src/StandaloneModule.hpp/.cpp
    ← ユーザーのゲーム固有の実装
    ← テンプレートから生成されたプロジェクトがそれぞれ持つ
```

**WHY**:  
EditorLauncher は `--project <path>` で任意のゲームプロジェクトを開く汎用ツール。  
テンプレートの `StandaloneModule` はユーザープロジェクト固有のコードなので  
EditorLauncher からは参照できない。両者は役割が異なり、別々の IModule 実装として共存する。

---

## ② ターゲット名サフィックス

### 決定: `Standalone` に統一

| ターゲット | 名前 |
|-----------|------|
| Sandbox パッケージ用 | `SandboxStandalone` |
| ユーザープロジェクト パッケージ用 | `{{TARGET_NAME}}Standalone` |

**WHY**:  
`Game` サフィックスは「どちらもゲームなので区別にならない」という問題がある。  
`Standalone` はエディタ依存がないことを明示しており、意図が明確。

**影響範囲**:  
- `runtime_compile.md` の `SandboxGame` → `SandboxStandalone` に修正済み
- `gamehub_integration.md` は最初から `{{TARGET_NAME}}Standalone` を使用
- `build.config.in` の `exe_*` パスも `SandboxStandalone.exe` で統一

---

## ③ `FBZZ_REGISTER_SCRIPT` と `GameMain.cpp`

### 決定: `FBZZ_REGISTER_SCRIPT` に完全移行し `GameMain.cpp` を廃止

| 役割 | 変更前 | 変更後 |
|------|--------|--------|
| スクリプト登録 | `GameMain.cpp` の `RegisterScripts()` | 各スクリプト `.cpp` の `FBZZ_REGISTER_SCRIPT` |
| ゲーム固有の初期化 | `GameMain.cpp` に追記 | `StandaloneModule::OnInit()` に書く |
| `RegisterScripts()` 呼び出し | `main.cpp` から呼ぶ | 不要 (自動登録) |

**各スクリプト `.cpp` の末尾**:

```cpp
// PlayerController.cpp
#include "PlayerController.hpp"

// ... メソッド定義 ...

// WHY: .cpp に置くことでリンカが TU を必ず取り込み、
//      MSVC /OPT:REF による dead-code elimination を回避する。
FBZZ_REGISTER_SCRIPT(MyGame::PlayerController)
```

**`StandaloneModule::OnInit()` (ゲーム固有の初期化)**:

```cpp
bool StandaloneModule::OnInit()
{
    // スクリプト登録は FBZZ_REGISTER_SCRIPT が自動で行うため不要。
    // ここにはシーンロード・物理設定など初期化処理を書く。
    if (!editor::SceneSerializer::Load(m_scene, sceneFile)) return false;
    ApplyPhysicsSettings(m_physicsWorld, m_settings);
    return true;
}
```

**WHY**:  
- `GameMain.cpp` は「スクリプト登録の一覧を手動管理する」ファイルだった。  
  `FBZZ_REGISTER_SCRIPT` で自動化できるため存在意義がなくなる。
- ゲーム固有の初期化は `StandaloneModule::OnInit()` に書けばよく、  
  別ファイルを挟む必要がない。
- `GameMain.cpp` を廃止することで、ユーザーが「どこに何を書くか」が明確になる。

**廃止に伴うテンプレートの変更**:

```
削除: Templates/standard/Src/GameMain.cpp
削除: main_editor.cpp / main_standalone.cpp からの RegisterScripts() 呼び出し
変更: CMakeLists.txt から GameMain.cpp のソースエントリを削除
```

---

## 実装順序 (確定)

| Phase | 対象 | 依存 |
|-------|------|------|
| 1 | `IModule` インターフェース + `Application::Run(IModule&)` | なし |
| 2 | Sandbox のリファクタリング (`StandaloneModule` / `EditorModule`) | Phase 1 |
| 3 | EditorLauncher の `StandaloneApp` → `StandaloneModule` 変換 | Phase 1 |
| 4 | テンプレートの分割 (`main_editor` / `main_standalone` / `StandaloneModule`) | Phase 1 |
| 5 | ビルドパイプライン (`Compiler` / `ToolchainLocator` / `BuildPipeline`) | Phase 1〜4 |
