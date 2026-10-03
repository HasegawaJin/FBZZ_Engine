# 開発者モード

エンジンを作る人だけが使う機能 (わざと落とす・内部状態の表示など) を、ゲームを作る人と遊ぶ人から隠す。エディターと配布したゲームの両方で同じ判定を使う。

## 1. 判定の正本

`fbzz::core::DeveloperMode` (Engine) が 1 つだけ持つ。

```
IsEnabled() = 起動で強制 || 設定
  起動で強制: 起動引数 --developer、または環境変数 FBZZ_DEVELOPER_MODE=1
  設定:       エディターの設定 (editor_config の [misc] developer_mode)。配布したゲームには無い
```

- 起動で強制したときは設定を書き換えない。一時的にオンにしたセッションが、次の起動まで持ち越されないようにするため
- 起動で強制しているあいだは、メニューからオフにできない (`FORCED_BY_LAUNCH` で拒否する)
- 入口 (EditorLauncher・Sandbox・各ゲームの AppMain) は、`CrashHandler::Install` の直後に `DeveloperMode::InitFromCommandLine()` を呼ぶ
- ビルド構成 (Debug / Development / Release) では決めない。構成が同じでも、ゲームを作る人とエンジンを作る人がいるため

## 2. エディター

| 操作 | id | 条件 |
|---|---|---|
| 開発者モードの切り替え | `developer.toggle_mode` | いつでも (起動で強制中はオフにできない) |
| わざと落とす | `developer.crash` (引数 `kind`) | 開発者モード |

- どちらも Debug メニューの末尾に並ぶ。«Developer» の子メニューは開発者モードのときだけ出る
- Op の登録簿に載せるので、コマンドパレットと AI バスからも同じ条件 (poll) で呼べる。開発者モードがオフなら AI からも呼べない

## RT 実験機能

- Hybrid / Path Tracing とレイ診断表示は DeveloperMode 限定。通常の Editor とゲーム制作を優先し、目標性能・画質へ到達する追加工数を保留する。
- Engine が毎ビューの実行許可を Graphics へ渡す。通常起動では RT 用 LOD / 材質収集、隠れた LOD の追加スキニング、BLAS / TLAS、RT 専用 shader / 履歴 / 出力資源の準備を行わない。
- DeveloperMode をオフへ切り替えると登録済み全ビューの RT 専用資源を退役する。Raster の資源は継続して使う。
- 保存した要求モード・プロファイル・効果は保持し、実効モードだけ Raster とする。診断は `DEVELOPER_MODE_REQUIRED`。設定・アセットの RT 編集と AI のレイ表示操作も同じ判定を使う。
- VS Code の `FBZZEditor (Development + DeveloperMode)` / `FBZZEditor (Debug + DeveloperMode)` は `--developer` を渡す。通常の起動構成は変更しない。
- デモ、実画像、計測条件、再開条件は [RT 実験の保存と再開](rt-experiment.md) を参照。

## 3. わざと落とす (`CrashHandler::Trigger`)

クラッシュレポート (`crash-report.md`) の受け口を 1 つずつ実際に通すために使う。

| `kind` | 起こすこと | 通る受け口 |
|---|---|---|
| `access_violation` | null への書き込み | `SetUnhandledExceptionFilter` |
| `stack_overflow` | 終わらない再帰 | 同上 (書き出しスレッドを使う経路) |
| `abort` | `std::abort()` | `SIGABRT` |
| `pure_call` | 構築中の基底から純粋仮想関数を呼ぶ | `_set_purecall_handler` |
| `invalid_parameter` | 書き込み先が短すぎる `strcpy_s` | `_set_invalid_parameter_handler` |
| `report` | 落とさずに `WriteReport` だけを呼ぶ | 手動の報告 |

- 開発者モードでなければ何もせず false を返す
- デバッガーを付けているときは、OS がデバッガーへ渡すので受け口を通らない。確かめるときはデバッガー無しで起動する

## 4. やらないこと (今は)

- ゲームの中で開発者モードを使う面 (ホットキーやコンソール): 載せる機能が 2 つ以上になってから
- 機能ごとの細かいオン/オフ: 必要になるまで 1 つのスイッチにしておく
