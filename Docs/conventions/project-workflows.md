<!-- @file    project-workflows.md -->
<!-- @brief   エンジンの領域ごとの制約と、変更を検証する手順。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-24 -->
# 領域ごとの開発規約

実装例と可読性の基準は [AGENTS.md](../../AGENTS.md) にある。

<a id="cpp"></a>

## 絶対制約

| 扱うもの | 守ること |
| --- | --- |
| 依存方向 | `Editor / GameHub / Sandbox → Engine → Physics / Fluid → Math`。`Physics` と `Fluid` は互いに依存しない。`Fluid` は数式だけを扱い、アセット形式・RHI・Scene を参照しない。[流体の設計](../design/fluid-library.md) |
| C++ の境界 | 名前空間は `fbzz::`。ヘッダーは `.hpp` と `#pragma once`。循環は前方宣言で切る。エンジンヘッダーで `using namespace` を使わない。`<Windows.h>` は `WIN32_LEAN_AND_MEAN` を先に定義して `.cpp` に閉じる。`NOMINMAX` は CMake が定義する。 |
| 所有権 | `new` / `delete` は使わず `make_unique` を基本にする。GPU リソースは単独所有。複数システムが寿命を共有するときだけ `make_shared` を使う。生ポインターは非所有。カスタムアロケーターは高頻度・大量確保に限る。 |
| 失敗と型変換 | `throw` / `std::exception` を使わず、回復可能なら `bool` と Logger、回復不能なら `assert()`。HRESULT は `FBZZ_HR_CHECK`、数学契約は `FBZZ_MATH_CONTRACT`。`dynamic_cast` は `GetComponent<T>()` に置き換える。`reinterpret_cast` は定数バッファ転送に限る。 |
| 描画・言語機能 | 上位層は `IRenderer&` を使い、DX ヘッダーと具体型は各バックエンドの PRIVATE に閉じる。C++20 Modules・`std::ranges`・標準コルーチンは使わない。コルーチンには `Engine/Scene/Scripting/Coroutine.hpp` を使う。 |
| 外部依存 | GLM / GLFW / Bullet / PhysX / Box2D は導入しない。FetchContent は使わず `ThirdParty/` に配し、`LICENSE`・`VERSION`・`THIRD-PARTY-NOTICES.md` を揃える。 |
| モジュール | `Engine/src/` にディレクトリを増やしたら `Projects/Engine/CMakeLists.txt` に登録する。自己登録の静的初期化子を残すため OBJECT ライブラリを維持する。標準ヘッダーは `fbzz_use_std_pch` を使い、重い実装は公開ヘッダーに含めない。[ビルド性能](build-performance.md) |

設計判断は [Docs/design/](../design/) を確認する。根拠となる設計が見つからず判断に迷う場合は、実装前に選択肢を提示する。旧 SDK 0.9.1 の GreenWare は別リポジトリで管理し、新 SDK のビルド・回帰テストに含めない。

<a id="scene"></a>

## シーン・システム

- 新システムは `ISystem` で Phase / RunMode / ComponentAccess を宣言し、`SystemScheduler` に登録する。実行順は PreScript → Script → PrePhysics → Physics（固定）→ PostPhysics → Navigation → LateScript → Cleanup → LateUpdate。描画と UI は描画パスで駆動する。
- `Transform.LocalPosition` は親基準、`WorldPosition` はワールド基準。両 setter は local を正本に world を即時同期する。Engine 内で直接フィールドを変更した場合は `FlushWorldTransforms` を呼ぶ。[Transform の契約](../design/script-transform-contract.md)
- Editor 操作は `Editor/src/Op/` の登録簿に一度登録する。メニュー・ホットキー・パレット・AI バスは登録簿から公開する。

<a id="scripts"></a>

## C# スクリプト

- ゲーム作者は公開 C# SDK を使う。`Assets/Scripts/<領域>/XxxComponent.cs` に `using Fbzz;` と `Script` 派生クラスを置く。namespace は任意。設定は public、内部状態は private。ライフサイクルは `protected override` で実装する（例 `Update`）。登録マクロ・手書き GUID・登録リストは不要。[C# 導入](../csharp-start.md)・[作者向け契約](../design/csharp-authoring.md)
- API の署名・単位・座標・callback 権限は `script_api_catalog` で確認する。物理を変えるのは `FixedUpdate`、Engine API は callback のスレッドから呼ぶ。新規 csproj は SDK の `share/fbzz/managed/ManagedStarter` を使い、API と Analyzer の版を揃える。SDK の資料と starter も同期する。
- 変更後は Editor MCP の `build_run` で `target: "managed"` を指定し、診断・要求 ID の `applied` を確認する。`script_catalog` で設定と参照を接続し、保存して Playtest する。[AI 向け手順](../design/ai-script-workflow.md)
- 作者向け C++ Script・Script DLL・登録・専用テストは廃止済み。Engine 内には ManagedScriptAdapter の共通基盤と内蔵 Component の反射・保存を残し、`.generated.hpp` を作成・include しない。大量処理はバッファ再利用と一括 API を優先し、計測した箇所を C++ サービスに集約する。[C# 専用制作](../design/csharp-only-gameplay.md)
- Play は別 Scene と C# static の別世代で動かす。Edit 側の元個体と履歴は保持し、外部 I/O はプロセスで共有する。[Play の契約](../design/csharp-settings-transactions.md)

<a id="assets"></a>

## アセット

`Assets/` の実ファイルと対になる `.meta` を一緒に扱い、`guid:` 参照を保つ。シーンやプレファブはディスク上の原本へ差分を当てる。Editor 起動中に Git でツリーを巻き戻さない。シェーダー変更時はエンジンとプロジェクトの全コピーを検索し、すべてに反映する。形式の定義は `Engine/Format/` と `Engine/Asset/` にある。

<a id="tools"></a>

## ツールの置き場所

開発用プログラムは `Projects/DevTools/`、タスク・CI の入口は `Tools/`、ゲーム固有の制作パイプラインは `<Project>/Tools/`、一回きりの調査は `Scratch/`。常設ツールはユーザーの承認を得て、該当する `README.md` の索引に役割と呼び出し元を登録する。AgentLint は未登録ファイルをエラーにする。

<a id="verification"></a>

## ビルド・テスト・Git

### 変更をテストする

機能追加・不具合修正では、設計・公開 API の契約からテストを書き、変更前に意図した理由で失敗することを確かめる。実装後は同じテストを通す。既存テストの前提や期待値を直すときは、元の誤りと根拠となる契約を報告する。

テストは `TEST_F` と TestKit を使い、新規ファイルは `Projects/Tests/CMakeLists.txt` の `SOURCES` に登録する。浮動小数点の比較には `EXPECT_VEC3_NEAR` 等を使う。本番と同じ操作と出力で検証し、テスト専用の公開 getter / setter や乱数・時刻・sleep を持ち込まない。[テスト規約](test.md)

### ビルドと解析

AI は `Tools/AgentBuild.ps1`、人は VS Code / VS のタスクを使う。`cmake` / `msbuild` を直接実行しない。Visual Studio の場所を知るのは `Tools/VsEnvironment.ps1` だけにする。

| 目的 | AgentBuild の引数 |
| --- | --- |
| 変更箇所をコンパイル | `check <ファイル...>` |
| C++ の可読性を検査 | `tidy <ファイル...>` |
| リンクまで実行 | `build <target>` |
| 関連テストを実行 | `test -Filter <regex>` |

**ビルドは SDK 公開を含めて同時に 1 本。** `RESULT busy` / `HINT CONTENDED` が出たら完了を待つかユーザーに確認し、同じ構成で一度だけ再実行する。競合中にプロセスを止めたり、コード・ビルドツリー・preset を変更したりしない。

DLL のロックでフルビルドや Editor 再起動が必要ならユーザーに VS Code タスクを依頼する。VS 更新後の configure 失敗は、対象の `build/<Config>/` を削除して再 configure する。検証ログは `RESULT` 行の `log=` を使う。手動保存も `build/agent/` 配下の絶対パスへ出す。

### Editor で動作を確かめる

シナリオは `<Project>/Tests/Playtests/*.playtest.json`、基準画像は `<Project>/Tests/Golden/`、出力は `<Project>/Library/Playtests/` に置く。

対象 Editor の MCP に接続し、操作入力は `scenario_run` → `scenario_status` で検証する。Editor が開いていない場合は通常の可視起動を使う。検証用の別 Editor は立てない。MCP 不通時は接続先・AI Command Bus を確認し、解決しなければ理由を報告する。ComputerUse での代行は行わない。非表示バッチはユーザーの明示指定か既存 CI に限る。[検証とシナリオの手順](../design/ai-verification-loop.md)

### Git で変更を扱う

改名・削除前に全参照を検索する。C# 型は `.cs.meta`、Scene・Prefab・DataAsset も調べる。ファイルの巻き戻しは対象を示して判断を仰ぎ、`git restore` と `git checkout -- <path>` は使わない。

ブランチは `main → develop → feature/<name>`。コミットは `[Feature|Fix|Design|Build|Refactor|Chore|Release]` に動詞と概要を続け、本文は Markdown。`Co-Authored-By` / `Generated with` は付けない。報告前に `git status` を確認する。[Git 規約](git.md)
