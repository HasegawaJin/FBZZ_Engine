# FBZZ Hub — 設計ドキュメント

Unity Hub に相当する、プロジェクト管理・エンジン起動のランチャーアプリ。  
`Projects/GameHub/` に実装する独立した Win32 実行ファイル。

---

## 概要

| 項目 | 内容 |
|------|------|
| 実行ファイル | `FBZZHub.exe` |
| CMake ターゲット | `fbzz_hub` |
| ソース | `Projects/GameHub/` |
| 依存ライブラリ | Win32 API, ImGui, toml++ |
| エンジン依存 | **なし** — Engine / Physics / Math ライブラリに依存しない独立バイナリ |
| 設定ファイル | `%APPDATA%\FBZZHub\hub_config.toml` |
| ウィンドウサイズ | 960 × 640 (最小サイズ、リサイズ可) |

---

## 配置方針

Hub が作成するゲームプロジェクトは **Standalone Project** とする。  
New Project で作成されたフォルダーは、C++ スクリプト、アセット、ProjectSettings をプロジェクトごとに所有し、FBZZ_Engine リポジトリ内の `Projects/Sandbox/` に混ぜない。

Hub の実装自体は `Projects/GameHub/` に置くが、実行ファイルは Engine / Physics / Math から独立したランチャーツールとして扱う。

| 項目 | 方針 |
|------|------|
| Hub ソース配置 | `Projects/GameHub/` |
| CMake ターゲット | `fbzz_hub` |
| ビルド成果物 | `FBZZHub.exe` |
| New Project の出力先 | ユーザーが選択した任意のフォルダー |
| ゲームプロジェクト構成 | Standalone。`Include/`, `Src/`, `Assets/`, `ProjectSettings/`, `Lib/`, `Binaries/`, `Build/` をプロジェクト内に持つ |
| Hub のリンク禁止 | `fbzz_engine`, `fbzz_physics`, `fbzz_math` |
| 共有してよいもの | Win32 API, ImGui, toml++, 標準ライブラリ |
| 連携方法 | `.fbzz_proj`, `Include/<ProjectName>/ProjectAPI.hpp`, `ProjectSettings/ProjectSettings.toml`, `FBZZEditor.exe --project "<path>"` |

### Project Layout: Monorepo vs Standalone

| 方式 | 採用 | 理由 |
|------|------|------|
| Standalone Project | 採用 | プロジェクトごとに C++ スクリプト、アセット、設定を閉じ込められる。Hub のプロジェクト一覧にも任意パスを登録しやすい |
| Engine リポジトリ内 Monorepo Project | 不採用 | ユーザーゲームと Engine 開発用 Sandbox が混ざり、ポートフォリオ用 Engine 本体の責務がぼやける |
| Hub / Engine と同一バイナリに組み込み | 不採用 | ランチャー、Editor、Runtime の責務が混ざり、依存方向が崩れやすい |

---

## 画面構成

```
┌──────────────────────────────────────────────────────────────┐
│  FBZZ Hub  v0.1.0                          [_][□][×]         │
├────────────┬─────────────────────────────────────────────────┤
│            │  [New Project]  [Add Existing]        [🔍 検索] │
│ ● Projects ├─────────────────────────────────────────────────┤
│            │  ┌──────────────────────────────────────────┐   │
│   Learn    │  │ MyGame                          [Open ▼] │   │
│            │  │ C:/Projects/MyGame                       │   │
│   Settings │  │ 最終更新: 2026-05-25   v0.1.0            │   │
│            │  └──────────────────────────────────────────┘   │
│            │  ┌──────────────────────────────────────────┐   │
│            │  │ Shooter                         [Open ▼] │   │
│            │  │ ...                                      │   │
│            │  └──────────────────────────────────────────┘   │
└────────────┴─────────────────────────────────────────────────┘
```

### レイアウト定数

| 要素 | サイズ |
|------|--------|
| サイドバー幅 | 160 px (固定) |
| プロジェクトカード高さ | 72 px |
| カード間マージン | 6 px |
| ツールバー高さ | 40 px |

### サイドバー項目

| 項目 | 役割 |
|------|------|
| Projects | プロジェクト一覧・新規作成 (デフォルト表示) |
| Learn | ドキュメント / GitHub リンク集 |
| Settings | Hub の設定 (Editor パス・テーマ) |

---

## Projects パネル

### プロジェクトカード

各カードに表示する情報:

| フィールド | 取得元 |
|-----------|--------|
| プロジェクト名 | `.fbzz_proj` の `project.name` |
| フルパス | `hub_config.toml` の `projects[].path` |
| 最終更新日時 | `hub_config.toml` の `projects[].last_opened` (Open するたびに更新) |
| エンジンバージョン | `.fbzz_proj` の `project.engine_version` |
| 存在確認 | 起動時に `std::filesystem::exists(path)` でチェック。存在しない場合はカードをグレーアウト |

### 操作一覧

| 操作 | トリガー | 動作 |
|------|---------|------|
| New Project | ツールバーボタン | `NewProjectDialog` を開く |
| Add Existing | ツールバーボタン | `SHBrowseForFolder` でフォルダー選択 → `.fbzz_proj` を探してリストに追加 |
| Open | カードをダブルクリック / `[Open]` ボタン | `ProcessLauncher::OpenInEditor` |
| コンテキストメニュー | カードを右クリック | Open / Reveal in Explorer / Remove from List |
| Remove from List | コンテキストメニュー | リストから除外 (フォルダー自体は削除しない) |
| Reveal in Explorer | コンテキストメニュー | `ShellExecute("explore", path)` |
| 検索 | 検索ボックス | プロジェクト名・パスをリアルタイムフィルタリング |

### 新規プロジェクト作成フロー

```
[New Project] クリック
  → NewProjectDialog を開く
      1. テンプレートをグリッドで選択
      2. プロジェクト名を入力 (英数字・アンダースコアのみ)
      3. 保存先フォルダーを選択 (SHBrowseForFolder)
      4. [Create] ボタン
         → TemplateManager::Instantiate(templateId, destPath, projectName)
            - Templates/<id>/ を destPath/projectName/ にコピー
            - .fbzz_proj を生成
            - CMakeLists.txt, CMakePresets.json, Src/GameMain.cpp を生成
            - Include/<ProjectName>/ProjectAPI.hpp を生成
            - ProjectSettings/ProjectSettings.toml を生成
            - Assets/, Src/Scripts/, Lib/, Binaries/, Build/ をプロジェクト内に配置
         → ProjectManager::Add(newProjectPath)
         → ProcessLauncher::OpenInEditor(newProjectPath)
         → ダイアログを閉じる
```

### エラーケース

| 状況 | UI の挙動 |
|------|----------|
| `path` が存在しない | カードをグレーアウト、「パスが見つかりません」ツールチップ |
| `.fbzz_proj` が読めない / 壊れている | 名前欄に `"(不明)"` と表示、バージョン欄を `-` に |
| Editor が見つからない | 「FBZZEditor.exe が見つかりません。Settings で設定してください」モーダル |
| プロジェクト名が空 / 重複フォルダー | [Create] ボタンを無効化し理由をインライン表示 |

---

## Editor 起動仕様

`ProcessLauncher` は `CreateProcess` でエディターを起動する。

### コマンドライン

```
FBZZEditor.exe --project "<project_root_path>"
```

例:
```
FBZZEditor.exe --project "C:/Projects/MyGame"
```

### Editor の検索順序

1. `hub_config.toml` の `hub.editor_exe` に絶対パスが設定されていればそれを使う
2. 未設定の場合、`FBZZHub.exe` と同一ディレクトリの `FBZZEditor.exe` を探す
3. 見つからなければ Settings へ誘導するモーダルを表示

### プロセス管理

- `CreateProcess` の `bInheritHandles = FALSE`、デタッチして起動 (`CREATE_NEW_PROCESS_GROUP`)
- Hub は Editor の終了を待たない (Hub を閉じても Editor は動き続ける)
- 同一プロジェクトを二重起動しない。検出は Hub 側のベストエフォートとし、起動済みプロセスの HWND を `FindWindowEx` で確認できれば `SetForegroundWindow` する

### Editor との依存境界

- Hub から Editor への連携は `FBZZEditor.exe --project "<project_root_path>"` のプロセス起動のみ
- Hub は Editor の内部ヘッダー、`EditorApp`、`EditorContext`、Panel、Engine API を include しない
- Hub は Editor の終了コードを待たず、Editor の実行状態をランタイム制御しない

---

## プロジェクトファイル形式 (`.fbzz_proj`)

プロジェクトルートに配置する TOML ファイル。Hub / Editor 間の最小共有契約として扱い、toml++ で読み書きする。

```toml
[project]
name              = "MyGame"
project_id        = "my_game"
cpp_namespace     = "my_game"
target_name       = "MyGame"
engine_version    = "0.1.0"
created_at        = "2026-05-25T14:00:00Z"
default_scene     = "Assets/Scenes/Main.fbzz"
asset_root        = "Assets/"
settings_path     = "ProjectSettings/ProjectSettings.toml"
api_root          = "Include/"
public_api_header = "Include/MyGame/ProjectAPI.hpp"
script_root       = "Src/Scripts/"
library_root      = "Lib/"
binary_root       = "Binaries/"
build_root        = "Build/"

[engine]
root = "" # 空文字の場合は Hub / Editor が保持する engine_root を使う
```

| フィールド | 型 | 説明 |
|-----------|-----|------|
| `name` | string | プロジェクト表示名 |
| `project_id` | string | ファイル名・フォルダー名・内部 ID に使う安定識別子。`lower_snake_case` |
| `cpp_namespace` | string | ゲームコードの C++ namespace。`project_id` と同じ値をデフォルトにする |
| `target_name` | string | CMake ターゲット名。`PascalCase` |
| `engine_version` | string | 作成時のエンジンバージョン (semver) |
| `created_at` | string | ISO 8601、Hub が生成時に書き込む |
| `default_scene` | string | Editor 起動時に開くシーンの `asset_root` 相対パス |
| `asset_root` | string | アセット検索ルートのプロジェクトルート相対パス |
| `settings_path` | string | `ProjectSettings` の TOML ファイル。プロジェクトルート相対パス |
| `api_root` | string | 公開 API ヘッダー配置ルート。プロジェクトルート相対パス |
| `public_api_header` | string | AI / Editor / 外部ツールが最初に参照する公開 API ヘッダー |
| `script_root` | string | C++ スクリプト配置ルート。プロジェクトルート相対パス |
| `library_root` | string | プロジェクト固有の外部ライブラリ配置ルート。プロジェクトルート相対パス |
| `binary_root` | string | ゲーム実行ファイル、DLL、PDB などの出力ルート。プロジェクトルート相対パス |
| `build_root` | string | CMake キャッシュや中間生成物の出力ルート。プロジェクトルート相対パス |
| `engine.root` | string | FBZZ_Engine ルート。空文字なら Hub / Editor 設定の engine_root を使う |

### Engine バージョン管理

`project.engine_version` は、プロジェクト作成時または最後に移行した FBZZ Engine のバージョンを表す。  
Hub / Editor は起動時に現在の `FBZZ_VERSION` と比較し、差分がある場合はプロジェクトカードまたは Open 時モーダルで通知する。

| 状況 | 判定 | 挙動 |
|------|------|------|
| `project.engine_version == FBZZ_VERSION` | 最新 | 通常表示 |
| patch 差分のみ | 軽微更新 | 通知のみ。自動移行はしない |
| minor 差分 | 要確認 | Open 前に「更新可能」モーダルを出す |
| major 差分 | 互換性注意 | Open 前に警告モーダルを出し、バックアップ推奨 |
| `engine_version` が読めない | 不明 | degraded 表示し、自動移行しない |

Hub は Engine 更新を検出しても、ユーザー確認なしにプロジェクトファイルやソースを上書きしない。

### 読み取りルール

- `project.name`, `project.project_id`, `project.cpp_namespace`, `project.target_name`, `project.engine_version`, `project.created_at`, `project.default_scene`, `project.asset_root`, `project.settings_path`, `project.api_root`, `project.public_api_header`, `project.script_root`, `project.library_root`, `project.binary_root`, `project.build_root`, `engine.root` は必須
- Hub は未知フィールドを無視する
- 必須フィールドが欠落している、または型が一致しない場合は `ProjectEntry::projFileValid = false` とする
- `.fbzz_proj` が壊れていても Hub は終了せず、プロジェクトカードを degraded 表示する
- Hub は `.fbzz` シーンファイルを解釈しない。`default_scene` はパス文字列として扱う
- Hub は `ProjectSettings/ProjectSettings.toml` の中身を解釈しない。ファイルの存在確認とテンプレート生成のみ担当する
- Hub は `public_api_header` の中身を解釈しない。ファイルの存在確認とテンプレート生成のみ担当する
- Hub は `Lib/`, `Binaries/`, `Build/` の中身を解釈しない。ディレクトリ作成と存在確認のみ担当する

### 名前の派生ルール

New Project ではユーザー入力の表示名から、ビルドや C++ で使う名前を派生して `.fbzz_proj` に保存する。

| 値 | 例 | 用途 |
|----|----|------|
| `name` | `My Game` | UI 表示名 |
| `project_id` | `my_game` | フォルダー名、内部 ID、namespace のデフォルト |
| `cpp_namespace` | `my_game` | `ProjectAPI.hpp` とゲームコードの namespace |
| `target_name` | `MyGame` | CMake ターゲット名、出力ファイル名 |

`project_id` は英小文字・数字・アンダースコアのみ、先頭は英小文字とする。`target_name` は英数字のみの PascalCase とし、空または重複する場合は [Create] を無効化する。

### 公開 API ヘッダー

Standalone Project には AI / Editor / 外部ツールが読むための公開 API ヘッダーを必ず置く。  
ゲーム固有の C++ スクリプト実装は `Src/Scripts/` に置き、外部から参照してよい型・関数・登録エントリだけを `Include/<ProjectName>/ProjectAPI.hpp` に集約する。

```cpp
// MyGame
// ProjectAPI.hpp | my_game
// ゲームプロジェクトの公開 API エントリ
#pragma once

namespace my_game {

void RegisterScripts();

} // namespace my_game
```

| 項目 | 方針 |
|------|------|
| 配置 | `Include/<ProjectName>/ProjectAPI.hpp` |
| 役割 | AI や外部ツールが最初に読むゲーム側 API の入口 |
| 記述対象 | スクリプト登録関数、公開コンポーネント型、ゲーム固有の定数、外部から触ってよい関数 |
| 非対象 | 実装詳細、ローカル補助関数、アセット読み込みの内部処理 |
| include 方向 | `Src/Scripts/*.cpp` は `ProjectAPI.hpp` を include してよい。`ProjectAPI.hpp` から個別スクリプト実装 `.cpp` は include しない |

---

## テンプレート

Hub 実行ファイルと同じディレクトリに `Templates/` を配置する。

```
FBZZHub.exe
Templates/
├── empty/
│   ├── template.toml
│   ├── .fbzz_proj.tmpl       ← name / engine_version をプレースホルダーで記述
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── Include/{{PROJECT_NAME}}/ProjectAPI.hpp
│   ├── ProjectSettings/ProjectSettings.toml
│   ├── Assets/Scenes/Main.fbzz
│   ├── Src/GameMain.cpp
│   ├── Lib/.gitkeep
│   ├── Binaries/.gitignore
│   ├── Build/.gitignore
│   └── Src/Scripts/.gitkeep
├── 3d_basic/
│   ├── template.toml
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── Include/{{PROJECT_NAME}}/ProjectAPI.hpp
│   ├── ProjectSettings/ProjectSettings.toml
│   ├── Assets/Scenes/Main.fbzz
│   ├── Src/GameMain.cpp
│   ├── Lib/.gitkeep
│   ├── Binaries/.gitignore
│   ├── Build/.gitignore
│   └── Src/Scripts/.gitkeep
└── physics_sandbox/
    ├── template.toml
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── Include/{{PROJECT_NAME}}/ProjectAPI.hpp
    ├── ProjectSettings/ProjectSettings.toml
    ├── Assets/Scenes/Main.fbzz
    ├── Src/GameMain.cpp
    ├── Lib/.gitkeep
    ├── Binaries/.gitignore
    ├── Build/.gitignore
    └── Src/Scripts/.gitkeep
```

### template.toml

```toml
[template]
id                 = "3d_basic"
display_name       = "3D Basic"
description        = "デフォルトカメラ・Directional ライトを配置したシーン"
thumbnail          = "thumbnail.png"
engine_version_min = "0.1.0"
```

### 組み込みテンプレート一覧

| id | 内容 |
|----|------|
| `empty` | `.fbzz_proj` + 空シーンのみ |
| `3d_basic` | Camera + DirectionalLight 配置済み |
| `physics_sandbox` | 3d_basic + 床 Plane + RigidBody キューブ数個 |

### `.fbzz_proj.tmpl` のプレースホルダー

`TemplateManager::Instantiate` がコピー時に文字列置換する。

| プレースホルダー | 置換値 |
|----------------|--------|
| `{{PROJECT_NAME}}` | ユーザーが入力したプロジェクト名 |
| `{{PROJECT_ID}}` | `lower_snake_case` の安定識別子 |
| `{{CPP_NAMESPACE}}` | C++ namespace |
| `{{TARGET_NAME}}` | CMake ターゲット名 |
| `{{ENGINE_VERSION}}` | `FBZZ_VERSION` マクロの値 |
| `{{CREATED_AT}}` | 生成時刻 (ISO 8601) |
| `{{SETTINGS_PATH}}` | `ProjectSettings/ProjectSettings.toml` |
| `{{API_ROOT}}` | `Include/` |
| `{{PUBLIC_API_HEADER}}` | `Include/<ProjectName>/ProjectAPI.hpp` |
| `{{SCRIPT_ROOT}}` | `Src/Scripts/` |
| `{{LIBRARY_ROOT}}` | `Lib/` |
| `{{BINARY_ROOT}}` | `Binaries/` |
| `{{BUILD_ROOT}}` | `Build/` |
| `{{ENGINE_ROOT}}` | Hub / Editor 設定から解決した FBZZ_Engine ルート。共有したくない場合は空文字 |

### プロジェクト生成責務

- Hub はテンプレートフォルダーのコピー、`.fbzz_proj` 生成、`CMakeLists.txt`、`CMakePresets.json`、`Src/GameMain.cpp`、`Include/<ProjectName>/ProjectAPI.hpp`、`ProjectSettings/ProjectSettings.toml`、標準ディレクトリの配置までを担当する
- テンプレート内の `.fbzz` は静的ファイルとして扱い、Scene 内容の意味解釈や修復はしない
- Scene 内容の検証、読み込み、修復は Editor / Engine 側の責務とする
- ProjectSettings の読み込み、保存、既定値補完は Editor / Engine 側の責務とする
- 公開 API ヘッダーの意味解釈、スクリプト登録、ビルド連携は Editor / Runtime 側の責務とする
- `Binaries/` と `Build/` は生成物置き場とし、Hub は中身を削除・上書きしない
- Hub は `SceneSerializer`, `AssetManager`, `Scene`, `GameObject` を使用しない

---

## ゲームプロジェクトのビルド仕様

Standalone Project は New Project 直後に CMake Configure 可能な最小構成を持つ。

### Engine ルート解決

Hub / Editor は次の順序で FBZZ_Engine ルートを解決し、ゲームプロジェクトの `CMakePresets.json` に `FBZZ_ENGINE_ROOT` cache 変数として書き込む。

1. `.fbzz_proj` の `[engine].root` が空でなければそれを使う
2. 環境変数 `FBZZ_ENGINE_ROOT` が設定されていればそれを使う
3. Hub / Editor 設定の `engine_root` を使う
4. Hub 実行ファイルから上位の FBZZ_Engine ルートを探索する
5. 解決できなければ Configure エラーにする

Hub は New Project 作成時に `[engine].root` を空文字で生成する。プロジェクトを別 PC に移動しても、環境変数か Hub / Editor 設定で Engine ルートを差し替えられるようにする。  
ゲーム側 `CMakeLists.txt` は `.fbzz_proj` を直接 parse しない。CMake から見る入力は `CMakePresets.json` の `FBZZ_ENGINE_ROOT`、または環境変数 `FBZZ_ENGINE_ROOT` のみとする。

### 出力先

| 種類 | 出力先 |
|------|--------|
| 実行ファイル | `Binaries/<config>/<target_name>.exe` |
| DLL / PDB | `Binaries/<config>/` |
| CMake キャッシュ | `Build/VS/` |
| 中間生成物 | `Build/VS/` |

`<config>` は `Debug` / `Release` など CMake generator の構成名とする。

### CMakePresets.json テンプレート

Hub は New Project 作成時に `CMakePresets.json` を生成する。Visual Studio / VS CMake Tools はこの preset を使って Configure する。

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "fbzz-vs",
      "displayName": "FBZZ Visual Studio",
      "binaryDir": "${sourceDir}/Build/VS",
      "cacheVariables": {
        "CMAKE_CONFIGURATION_TYPES": "Debug;Release",
        "FBZZ_ENGINE_ROOT": "{{ENGINE_ROOT}}"
      }
    }
  ],
  "buildPresets": [
    {
      "name": "fbzz-debug",
      "configurePreset": "fbzz-vs",
      "configuration": "Debug"
    },
    {
      "name": "fbzz-release",
      "configurePreset": "fbzz-vs",
      "configuration": "Release"
    }
  ]
}
```

`generator` は preset に固定しない。Visual Studio 2026 / VS CMake Tools が選択するインストール済み generator を使う。

### C++ スクリプトの扱い

- v1 は DLL ホットリロードを対象外とする
- ゲームプロジェクトは `Src/GameMain.cpp` と `Src/Scripts/*.cpp` をビルドしてゲーム実行ファイルを生成する
- `Src/GameMain.cpp` は `Include/<ProjectName>/ProjectAPI.hpp` を include し、`RegisterScripts()` を呼ぶ
- Editor は `--project` でプロジェクトを開き、必要に応じて `Binaries/<config>/<target_name>.exe` を起動する
- Editor がゲーム DLL を動的ロードする方式は将来拡張とし、本設計では扱わない

### CMakeLists.txt テンプレート

ゲームプロジェクトの `CMakeLists.txt` は、FBZZ_Engine 全体のトップレベル `CMakeLists.txt` を `add_subdirectory` しない。  
Standalone 側から必要な Engine サブディレクトリだけを取り込み、`Projects/Sandbox` や `Projects/Editor` をビルド対象に含めない。

```cmake
cmake_minimum_required(VERSION 3.25)
project({{TARGET_NAME}} VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(FBZZ_ENGINE_ROOT "" CACHE PATH "Path to FBZZ_Engine root")
if(FBZZ_ENGINE_ROOT STREQUAL "" AND DEFINED ENV{FBZZ_ENGINE_ROOT})
    file(TO_CMAKE_PATH "$ENV{FBZZ_ENGINE_ROOT}" FBZZ_ENGINE_ROOT)
endif()

if(FBZZ_ENGINE_ROOT STREQUAL "")
    message(FATAL_ERROR "FBZZ_ENGINE_ROOT is not set. Set [engine].root, environment variable FBZZ_ENGINE_ROOT, or Hub/Editor engine_root.")
endif()

if(NOT EXISTS "${FBZZ_ENGINE_ROOT}/Projects/Engine/CMakeLists.txt")
    message(FATAL_ERROR "Invalid FBZZ_ENGINE_ROOT: ${FBZZ_ENGINE_ROOT}")
endif()

add_subdirectory("${FBZZ_ENGINE_ROOT}/ThirdParty"      "${CMAKE_BINARY_DIR}/FBZZ/ThirdParty")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Math"   "${CMAKE_BINARY_DIR}/FBZZ/Math")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Physics" "${CMAKE_BINARY_DIR}/FBZZ/Physics")
add_subdirectory("${FBZZ_ENGINE_ROOT}/Projects/Engine" "${CMAKE_BINARY_DIR}/FBZZ/Engine")

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
    "Src/*.cpp"
    "Src/*.hpp"
    "Include/*.hpp"
)

add_executable({{TARGET_NAME}} ${GAME_SOURCES})

target_include_directories({{TARGET_NAME}}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Include"
        "${CMAKE_CURRENT_SOURCE_DIR}/Src"
)

target_link_libraries({{TARGET_NAME}}
    PRIVATE
        fbzz_engine
        fbzz_physics
        fbzz_math
)

set_target_properties({{TARGET_NAME}} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/$<CONFIG>"
)

add_custom_command(TARGET {{TARGET_NAME}} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${FBZZ_ENGINE_ROOT}/ThirdParty/Assimp/dll/Debug/assimp-vc145-mtd.dll,${FBZZ_ENGINE_ROOT}/ThirdParty/Assimp/dll/Release/assimp-vc145-mt.dll>"
        "$<TARGET_FILE_DIR:{{TARGET_NAME}}>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FBZZ_ENGINE_ROOT}/Assets/Shaders"
        "$<TARGET_FILE_DIR:{{TARGET_NAME}}>/Assets/Shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:{{TARGET_NAME}}>/Assets"
)
```

### CMake テンプレートの責務

- `FBZZ_ENGINE_ROOT` は CMake cache 変数として定義し、環境変数でも上書きできる
- `FBZZ_ENGINE_ROOT` の通常値は Hub が生成する `CMakePresets.json` から渡す
- `ThirdParty`, `Projects/Math`, `Projects/Physics`, `Projects/Engine` だけを `add_subdirectory` する
- `Projects/Editor`, `Projects/Sandbox`, `Projects/Tests` は取り込まない
- ゲーム側 include は `Include/` と `Src/` のみ追加する
- link は `fbzz_engine`, `fbzz_physics`, `fbzz_math` のみ明示する
- 実行時に必要な Assimp DLL、Engine の shader assets、ゲームプロジェクトの `Assets/` を `Binaries/<config>/` にコピーする
- CMake Configure は Visual Studio 2026 または VS CMake Tools から `fbzz-vs` preset を選んで実行する。ビルドは `fbzz-debug` / `fbzz-release` preset を使う。ターミナルから `cmake --build` や `ninja` は実行しない

---

## Engine 更新時の対応

Standalone Project は Engine リポジトリ外に置けるため、Engine 更新時は Hub / Editor がプロジェクトとの差分を検出して安全に移行する。

### 更新検出

Hub はプロジェクト一覧の読み込み時に、各 `.fbzz_proj` の `project.engine_version` と現在の `FBZZ_VERSION` を比較する。  
差分がある場合、プロジェクトカードに更新状態を表示する。

| 状態 | UI 表示 | Open 時の挙動 |
|------|--------|---------------|
| 最新 | なし | そのまま開く |
| patch 差分 | `Update available` | そのまま開ける。通知のみ |
| minor 差分 | `Migration recommended` | 確認モーダルを表示 |
| major 差分 | `Compatibility warning` | 警告モーダルを表示し、バックアップなしの移行を禁止 |

### 移行対象

移行はユーザーのゲームコードを壊さない範囲に限定する。

| 対象 | 自動更新 | 方針 |
|------|----------|------|
| `.fbzz_proj` の schema 追加 | 可 | 既存値を保持し、不足フィールドだけ既定値で追加 |
| `ProjectSettings/ProjectSettings.toml` | 可 | 不足セクションだけ追加。既存値は上書きしない |
| `CMakePresets.json` | 可 | Hub 生成物として再生成可 |
| `CMakeLists.txt` | 確認後 | ユーザー編集の可能性が高いため、差分表示後に適用 |
| `Include/<ProjectName>/ProjectAPI.hpp` | 確認後 | 公開 API なので差分表示後に適用 |
| `Src/GameMain.cpp` | 確認後 | ユーザー編集の可能性があるため自動上書き禁止 |
| `Src/Scripts/` | 不可 | ユーザーコード。Hub は上書きしない |
| `Assets/` | 不可 | ユーザー資産。Hub は上書きしない |
| `Binaries/`, `Build/` | 不可 | 生成物。必要ならユーザーが削除・再生成する |

### バックアップ

minor / major 移行では、Hub は適用前にプロジェクトルート直下へバックアップを作る。

```
Backups/
└── 2026-05-25_143000_before_0.2.0/
    ├── .fbzz_proj
    ├── ProjectSettings/
    ├── CMakeLists.txt
    ├── CMakePresets.json
    ├── Include/
    └── Src/GameMain.cpp
```

`Assets/`, `Src/Scripts/`, `Binaries/`, `Build/`, `Lib/` はバックアップ対象外とする。ユーザーコード・アセット全体のバックアップは Git または手動コピーに任せる。

### 移行完了条件

移行が成功した場合のみ `.fbzz_proj` の `project.engine_version` を現在の `FBZZ_VERSION` に更新する。  
途中で失敗した場合は `engine_version` を更新せず、カードに `Migration failed` を表示する。

### 互換性の原則

- Hub はユーザーコードとアセットを自動上書きしない
- 不足フィールド追加は許可するが、既存値の変更はユーザー確認を必要とする
- major 更新では必ず確認モーダルを出し、バックアップ作成に失敗した場合は移行しない
- 移行処理は `bool` で成否を返し、例外は使わない

---

## Hub 設定ファイル (`hub_config.toml`)

`%APPDATA%\FBZZHub\hub_config.toml` に保存。Hub 専用の設定ファイルであり、Editor 側からは読まない。  
`%APPDATA%\FBZZHub\` が存在しない場合は Hub 起動時に作成し、設定はアプリ終了時に書き込む。

```toml
[hub]
editor_exe = ""        # 空文字 = Hub と同ディレクトリの FBZZEditor.exe を使う
engine_root = ""       # 空文字 = Hub 実行ファイルから上位の FBZZ_Engine ルートを探索
theme      = "dark"    # "dark" | "light"

[[projects]]
path        = "C:/Projects/MyGame"
last_opened = "2026-05-25T12:00:00Z"

[[projects]]
path        = "C:/Projects/Shooter"
last_opened = "2026-05-20T09:00:00Z"
```

プロジェクト一覧は `last_opened` 降順で表示する。

---

## データ構造

```cpp
// ProjectEntry.hpp
namespace fbzz::hub {

struct ProjectEntry {
    std::string name;           // .fbzz_proj から読み取り。読めない場合は "(不明)"
    std::string projectId;      // .fbzz_proj から読み取り
    std::string path;           // プロジェクトルートの絶対パス
    std::string engineVersion;  // .fbzz_proj から読み取り
    std::string lastOpened;     // ISO 8601、Open するたびに更新
    bool        engineVersionMismatch = false; // 現在の FBZZ_VERSION と異なるか
    bool        migrationRequired = false;     // minor / major 差分があるか
    bool        pathExists = false;   // std::filesystem::exists(path)
    bool        projFileValid = false; // .fbzz_proj が正常に読めたか
    bool        apiHeaderExists = false; // public_api_header が存在するか
    bool        settingsExists = false;  // settings_path が存在するか
    bool        cmakeExists = false;     // CMakeLists.txt が存在するか
    bool        layoutValid = false;     // 標準ディレクトリが揃っているか
};

} // namespace fbzz::hub
```

---

## アーキテクチャ

### ファイル構成

```
Projects/GameHub/
├── CMakeLists.txt
├── Src/
│   ├── main.cpp                    // WinMain・メッセージループ
│   ├── HubApp.hpp / .cpp           // ImGui フレームループ・パネル切り替え
│   ├── HubConfig.hpp / .cpp        // hub_config.toml の読み書き
│   ├── ProjectEntry.hpp            // ProjectEntry 構造体
│   ├── ProjectManager.hpp / .cpp   // プロジェクト一覧の CRUD・ソート
│   ├── TemplateManager.hpp / .cpp  // テンプレート列挙・フォルダーコピー
│   ├── ProcessLauncher.hpp / .cpp  // CreateProcess で Editor を起動
│   └── UI/
│       ├── Sidebar.hpp / .cpp
│       ├── ProjectsPanel.hpp / .cpp
│       ├── NewProjectDialog.hpp / .cpp
│       ├── SettingsPanel.hpp / .cpp
│       └── LearnPanel.hpp / .cpp
└── Resources/
    └── icon.ico
```

### クラス責務

| クラス | 責務 |
|--------|------|
| `HubApp` | ImGui フレームループ・アクティブパネルの切り替え |
| `HubConfig` | `hub_config.toml` のロード / セーブ |
| `ProjectManager` | `ProjectEntry` リストの管理、`.fbzz_proj` 読み取り、存在確認 |
| `TemplateManager` | `Templates/` 列挙、プロジェクトフォルダーのインスタンス化 |
| `ProcessLauncher` | Editor の検索・起動・二重起動防止 |
| `ProjectsPanel` | カード一覧・検索・コンテキストメニュー描画 |
| `NewProjectDialog` | テンプレート選択・名前入力モーダル |
| `SettingsPanel` | editor_exe パス・テーマ設定 |

### 依存関係

```
HubApp
  ├── ProjectsPanel ──→ ProjectManager ──→ HubConfig
  │        │                         └──→ (std::filesystem)
  │        └──→ NewProjectDialog ──→ TemplateManager
  │                             └──→ ProjectManager
  │                             └──→ ProcessLauncher ──→ HubConfig
  ├── SettingsPanel ──→ HubConfig
  ├── LearnPanel
  └── Sidebar
```

Engine ライブラリ (`fbzz_engine`, `fbzz_physics`, `fbzz_math`) への依存は**一切持たない**。

### 依存境界

- `Projects/GameHub/CMakeLists.txt` は `fbzz_engine`, `fbzz_physics`, `fbzz_math` をリンクしない
- Hub は `.fbzz_proj` を TOML として読むだけで、Engine 側の Scene / Asset / Serializer API を使用しない
- Hub と Editor の共有契約は `.fbzz_proj`、`Include/<ProjectName>/ProjectAPI.hpp`、`ProjectSettings/ProjectSettings.toml`、テンプレートファイル、起動コマンドラインに限定する
- 将来 Hub 側に便利機能を追加する場合も、Engine API への直接依存ではなくファイル契約かプロセス起動で連携する

---

## 実装ロードマップ

| フェーズ | 内容 |
|---------|------|
| **Phase 1** | Win32 ウィンドウ + ImGui フレームループ (`HubApp`) |
| **Phase 1** | `hub_config.toml` の読み書き (`HubConfig`) |
| **Phase 1** | プロジェクトカード一覧 + Open (`ProcessLauncher`) |
| **Phase 2** | Add Existing / Remove from List |
| **Phase 2** | New Project ダイアログ + テンプレートコピー |
| **Phase 2** | 検索フィルタリング |
| **Phase 2** | Add Existing のレイアウト検証 |
| **Phase 2** | Engine バージョン差分検出 |
| **Phase 3** | Project migration UI |
| **Phase 3** | サムネイル表示 (stb_image で `thumbnail.png` を読む、DX11 不使用) |
| **Phase 3** | Learn パネル (リンク集) |

---

## 決定済み事項

- [x] **フォーマットは TOML** — `.fbzz_proj` / `hub_config.toml` / `template.toml` すべて toml++ で統一
- [x] **Script は C++ ソース** — `Src/Scripts/` に置きゲームバイナリにコンパイル (UE スタイル)。DLL ホットリロードは対象外
- [x] **ウィンドウは通常 Win32 フレーム** — カスタムタイトルバーは実装コストに見合わない
- [x] **Editor パス解決** — Hub と同ディレクトリの `FBZZEditor.exe` をデフォルトとし、Settings でオーバーライド可
- [x] **ソート基準** — `last_opened` 降順 (最近開いたものが先頭)
- [x] **テンプレート配置** — `FBZZHub.exe` と同ディレクトリの `Templates/` サブフォルダー
- [x] **プロジェクト配置方針** — Standalone Project。Hub が作るゲームプロジェクトは Engine リポジトリ内に混ぜない
- [x] **Hub 配置方針** — `Projects/GameHub/` に置くが Engine / Physics / Math へはリンクしない
- [x] **Hub / Editor 共有契約** — `.fbzz_proj`、`Include/<ProjectName>/ProjectAPI.hpp`、`ProjectSettings/ProjectSettings.toml`、`FBZZEditor.exe --project "<path>"` を固定インターフェースとする
- [x] **Hub 設定の所有者** — `hub_config.toml` は Hub 専用。初回起動時に `%APPDATA%\FBZZHub\` を作成する
- [x] **ビルド方式 v1** — Standalone Project はゲーム実行ファイルを生成する。DLL ホットリロードと Editor への動的ロードは対象外
- [x] **ゲーム CMake テンプレート** — `ThirdParty`, `Math`, `Physics`, `Engine` のみを取り込み、`CMakePresets.json` 経由で Engine ルートを渡し、ゲーム exe を `Binaries/<config>/` に出力する
- [x] **Engine 更新対応** — `project.engine_version` と `FBZZ_VERSION` を比較し、ユーザー確認つきで schema / settings / generated files を移行する

### ゲームプロジェクトの標準フォルダー構成

```
MyGame/
├── .fbzz_proj            ← TOML プロジェクトファイル
├── CMakeLists.txt         ← ゲーム側ターゲット定義
├── Include/
│   └── MyGame/
│       └── ProjectAPI.hpp ← AI / Editor / 外部ツール向けの公開 API
├── ProjectSettings/
│   └── ProjectSettings.toml
├── Assets/
│   ├── Scenes/           ← .fbzz シーンファイル
│   ├── Meshes/
│   ├── Textures/
│   └── Materials/
├── Src/
│   ├── GameMain.cpp       ← ゲーム側エントリ / スクリプト登録
│   └── Scripts/          ← C++ スクリプト (.hpp / .cpp)
├── Lib/                  ← プロジェクト固有の外部 lib / dll / include
├── Binaries/             ← ビルド済み exe / dll / pdb
└── Build/                ← CMake キャッシュ・中間生成物
```

このフォルダーは FBZZ_Engine リポジトリの外に置ける。Hub は絶対パスを `hub_config.toml` に登録し、Editor は `--project` で渡されたルートから `.fbzz_proj`、ProjectSettings、Assets、Scripts を解決する。

### 生成物ディレクトリの扱い

| ディレクトリ | 役割 | Git 管理 |
|--------------|------|----------|
| `Lib/` | ゲームプロジェクト固有の外部ライブラリ、DLL、追加 include | 必要なものだけ管理 |
| `Binaries/` | ビルド済み `exe` / `dll` / `pdb` の出力先 | ignore。`.gitignore` のみ管理 |
| `Build/` | CMake キャッシュ、中間ファイル、オブジェクト | ignore。`.gitignore` のみ管理 |

---

## Add Existing 検証ルール

Add Existing は `.fbzz_proj` を持つフォルダーをプロジェクトとして登録する。標準構成が欠けている場合も登録は可能だが、カードに警告を表示する。

| 項目 | 判定 | UI |
|------|------|----|
| `.fbzz_proj` が存在しない | エラー | 登録しない |
| `.fbzz_proj` が壊れている | 警告 | 登録するが degraded 表示 |
| `CMakeLists.txt` がない | 警告 | 「ビルド設定なし」 |
| `public_api_header` がない | 警告 | 「公開 API なし」 |
| `settings_path` がない | 警告 | 「ProjectSettings なし」 |
| `Assets/`, `Src/`, `Include/` のいずれかがない | 警告 | 「標準フォルダー不足」 |
| `Binaries/`, `Build/`, `Lib/` がない | 情報 | Hub が必要に応じて空ディレクトリを作成できる |

---

## 未解決事項

- なし
