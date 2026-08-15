# Core 公開 API 整理

## 目的

`Projects/Engine/include/Engine/Core/` にあるヘッダを、共有 SDK の利用者が直接利用する契約と、Engine の起動・診断実装に限定する内部 API に分類する。

GoogleTest への移行では、この分類をテスト単位にも反映する。ウィンドウや Application をヘッドレスの単体テストへ持ち込まず、状態を注入できる小さな契約を優先して検証する。

## 現状の公開境界

- `Projects/Engine/CMakeLists.txt` は `include/*.hpp` を再帰的に `fbzz_engine` へ追加し、`include` 全体を `PUBLIC` にしている。
- `WINDOWS_EXPORT_ALL_SYMBOLS` が有効なため、ヘッダ上で明示的に公開対象を絞る契約はまだ存在しない。
- Core のヘッダは `fbzz::core` と `fbzz` が混在している。
- `Application`, `Logger`, `Time`, `TaskSystem`, `Cursor` は静的状態またはシングルトンを持つため、テスト間の状態リセット契約が必要になる。

## 公開 API の分類

### 1. 利用者が直接使う安定契約

| 分類 | ヘッダ | 公開する責務 | GoogleTest の単位 |
|---|---|---|---|
| ライフサイクル | `IModule.hpp` | Application のメインループへ接続するモジュール契約 | `Core.Module` |
| 時間 | `Time.hpp` | フレーム時間、累積時間、時間倍率、FPS 設定 | `Core.Time` |
| ログ | `Logger.hpp`, `ILogSink.hpp` | レベル付きログと非所有 Sink の登録 | `Core.Logger` |
| イベント | `Signal.hpp` | 型安全な購読、解除、通知 | `Core.Signal` |
| 非同期処理 | `Concurrency/TaskSystem.hpp` | タスク投入と `std::future` による結果取得 | `Core.TaskSystem` |
| メモリ契約 | `Memory/Allocator.hpp` | Allocator、`MemoryStats`、構築・破棄補助 | `Core.Memory.Allocator` |
| メモリ実装 | `Memory/LinearAllocator.hpp`, `FrameAllocator.hpp`, `PoolAllocator.hpp`, `StackAllocator.hpp` | 用途別アロケータのライフサイクルと確保契約 | `Core.Memory.*Allocator` |
| メモリ診断 | `Memory/MemoryTracker.hpp`, `Memory/MemoryDebug.hpp`, `AllocationInfo.hpp` | 使用量、リーク、共有リソースの観測 | `Core.Memory.Diagnostics` |
| システム実行 | `Scheduler/ISystem.hpp`, `SystemScheduler.hpp`, `Phase.hpp`, `SystemContext.hpp`, `ComponentAccess.hpp`, `OrderingHints.hpp` | Phase、依存関係、Component access を宣言して System を実行 | `Core.Scheduler` |

### 2. ホスト・プラットフォーム境界

| ヘッダ | 扱い | 理由 |
|---|---|---|
| `Window.hpp` | 公開するが単体テスト対象外 | Win32、`HWND`、OLE、メッセージポンプを含むため、ウィンドウを使う統合テストで検証する。 |
| `Cursor.hpp` | 公開するが単体テスト対象外 | OS カーソル状態を変更するため、状態管理と OS 適用を分離できるまで統合テストに置く。 |
| `Application.hpp` | 公開するが単体テスト対象外 | Renderer、SceneManager、Audio、Window、MemorySystem の所有者であり、起動統合の責務を持つ。 |

### 3. Engine 内部へ移す候補

| ヘッダ | 現状の問題 | 整理方針 |
|---|---|---|
| `HResult.hpp` | `Logger.hpp` と `__debugbreak()` に直接依存するマクロで、DX/Win32 実装詳細が漏れる。 | `Engine/Renderer` または `Engine/Core/Platform` 側の内部ヘッダへ移し、SDK の一般利用者へは公開しない。 |
| `EngineRebuildBootstrap.hpp` | 開発環境の CMake パスと再起動を扱うランチャー専用 API。 | `EditorLauncher` / Standalone の起動補助へ移し、ランタイム Core 契約から外す。 |
| `Memory/MemoryDebug.hpp` | `shared_ptr` の弱参照台帳と固定容量の診断実装を公開している。 | 診断ビルド用の任意サービスとして `MemorySystem` から分離し、通常の SDK 契約からは一段下げる。 |

## 先に固定する設計判断

### 名前空間

Core の新しい公開 API は `fbzz::core` に統一する。既存の `fbzz::Time`、`fbzz::Signal`、`fbzz::TaskSystem`、Scheduler の `fbzz::*` は、既存利用箇所を確認しながら段階的に互換 alias または移行用 include を用意する。

`fbzz::physics`、`fbzz::renderer`、`fbzz::scene` のように、上位サブシステムの名前空間へ Core 型を置かない。

### テスト可能性

- `Time` は実時間 `sleep` に依存する検証を最小限にし、時刻源を注入できる設計へ移行する候補とする。
- `Logger` は Sink の非所有契約を維持し、各テストで `RemoveSink` と最小レベル復元を必ず行う。
- `TaskSystem` は `Init` / `Shutdown` を fixture の SetUp / TearDown に閉じ込め、タスク完了前に Shutdown しない。
- Allocator は OS、Renderer、Window に依存しないため、最初の GoogleTest 移行対象にする。
- `Signal` はヘッダオンリーのため、購読解除、通知中の変更、空通知を独立テストする。
- `Application`、`Window`、`Cursor`、`EngineRebuildBootstrap` は Core の単体テストから除外し、後段の Windows 統合テストで扱う。

### 公開データと実装状態

次の型は現在フィールドを直接公開しているため、当面は値型の設定 DTO として扱う。ただし、将来 ABI を安定させる場合は変更検証が必要になる。

- `MemoryStats`
- `AllocationInfo`
- `ComponentAccess`
- `OrderingHints`
- `PhaseConfig`
- `SystemContext`

一方、`Time` の static フィールドと各サービスの static 状態は、外部から直接書き換え可能なため、将来は getter / setter または状態所有オブジェクトへ寄せる。

## GoogleTest 移行順序

1. `Core.Memory`、`Core.Signal`、`Core.Logger` を単一の GoogleTest ターゲットへ移す。
2. `Core.Time`、`Core.TaskSystem` を fixture 化して移す。
3. `Core.Scheduler` の API を検証するテストを新設する。型取得の `dynamic_cast` を廃止し、`SystemScheduler::FindSystem(std::string_view)` として名前検索へ整理する。
4. 既存の `Projects/Tests` の手書き実行ファイルと `TestHelper.hpp` を削除する。
5. `Application` / `Window` / `Cursor` の統合テストは、GoogleTest の別ターゲットとして必要になった時点で追加する。

## 現時点での結論

Core の最初の移行範囲は `Memory`、`Signal`、`Logger`、`Time`、`Concurrency/TaskSystem` とする。`Application`、`Window`、`Cursor`、`EngineRebuildBootstrap` は公開範囲またはテスト種別の整理が必要なため、既存テスト全削除と同じ変更へ混ぜない。
