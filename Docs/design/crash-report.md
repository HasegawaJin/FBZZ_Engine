# クラッシュレポート

落ちたときに «どこで・何が» を残し、次に起動したときに知らせる。エディターでも配布したゲームでも同じ仕組みで動く。

## 1. 残すもの

```
<根>/Saved/Crashes/<YYYY-MM-DD_HH-MM-SS>_<pid>/
  crash.dmp     ミニダンプ (Visual Studio / WinDbg で開くと落ちた行とスタックが出る)
  report.txt    例外コード・落ちた場所 (モジュール+オフセット)・版・コマンドライン・ログの末尾 200 行
  reported      次回起動で知らせ終えた印 (中身は空)
```

| 起動の仕方 | `<根>` |
|---|---|
| エディター (`FBZZEditor.exe` / `Sandbox.exe`) | プロジェクトのルート |
| 配布したゲーム | exe のあるディレクトリ (`game.log` と同じ場所) |
| 開発中の Standalone | プロジェクトのルート |

- 入口はどれも «作業ディレクトリ» (`.fbzz_proj` が exe の隣にあれば exe のディレクトリ、無ければプロジェクトのルート) を `<根>` に渡す
- `Saved/` は Git の対象にしない (`.gitignore` の `**/Saved/`)

## 2. 捕まえるもの

| 落ち方 | 受け口 |
|---|---|
| アクセス違反・ゼロ除算・スタックオーバーフローなどの SEH 例外 | `SetUnhandledExceptionFilter` |
| `abort()` (Debug の `assert` 失敗も最後はここへ来る) | `signal(SIGABRT)` |
| 純粋仮想関数の呼び出し | `_set_purecall_handler` |
| CRT 関数への不正な引数 | `_set_invalid_parameter_handler` |
| 落ちてはいないが続けられない (デバイスの消失など) | `CrashHandler::WriteReport(reason)` を呼び出し側が呼ぶ |

- デバッガーを付けているときは OS が先にデバッガーへ渡すので、この仕組みは動かない (そのまま止まって調べられる)
- CRT は `abort` と不正引数のときに独自に WER (Windows エラー報告) を呼ぶ。上の 3 つを差し替えるのはそのため

## 3. 書き方

- ダンプとレポートは**別スレッドで書く**。スタックオーバーフローで落ちたスレッドには関数を呼ぶだけのスタックが残っていないため。落ちたスレッドは書き終わるまで待ち、`TerminateProcess(例外コード)` で終わる
  - メインスレッドには `SetThreadStackGuarantee` で 64 KB を確保し、スレッドを作るぶんのスタックを残す
  - スレッドを作れない、または 30 秒で終わらないとき (ローダーロックを持ったまま落ちたとき) は、落ちたスレッドで直接書く
- 落ちた後はヒープが壊れているかもしれない。レポートを書く経路ではヒープを使わない。パスとログの末尾は `Install` の時点で固定長の領域に置き、ファイルは `CreateFileW` / `WriteFile` で直接書く
- `dbghelp.dll` は `Install` のときに System32 から読んでおく。落ちてからの `LoadLibrary` は避ける
- ダンプの種類は `MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules`。スタックと、スタックから指されている周辺のメモリまで含む。数 MB に収まる (全メモリは含まない)
- 同じプロセスで 2 つ目のスレッドが落ちても、1 つ目のレポートを書き終えるまで待たせる (ダンプは 1 つだけ)

## 4. 次の起動で知らせる

- 入口は `Install` の直後に `CrashHandler::NotifyUnreported(<根>, アプリ名, interactive)` を呼ぶ
- `reported` の印が無いレポートがあれば、件数と最新の 1 行 (例外コードと場所) をダイアログで出し、«はい» でそのフォルダを開く。答えがどちらでも印を付ける
- `--batch` で起動したときはダイアログを出さない (CI が人のクリックを待って止まるため)。Logger に WARN を出すだけにして、印は付けない

## 5. やらないこと (今は)

- クラッシュ時のダイアログやアップロード: 別プロセスのレポーター (Crashpad や UE の CrashReportClient のような形) が要る。配布先が増えてから
- シンボルサーバー: 今は配布物と同じビルドの `.pdb` を手元に残して開く
