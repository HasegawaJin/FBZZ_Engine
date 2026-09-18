---
name: verify-cpp
description: FBZZ Engine の C++ を変更した後、自分でコンパイル・テストして結果を確かめる手順。Projects/ や GreenWare/Assets/Scripts の .cpp/.hpp を編集したとき、報告の前に必ず使う。「ビルドして」「コンパイル通る？」「テスト回して」にも使う。
---

# C++ の検証ループ

入口は `Tools/AgentBuild.ps1` だけ。`cmake` / `msbuild` / `cl` を直接叩かない (VS 環境を作れるのは `Tools/VsEnvironment.ps1` だけ)。

## 1. 変えたファイルをコンパイルする (毎回)

```
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 check <変えた .cpp / .hpp ...>
```

- `.hpp` を渡すと、それを include する `.cpp` を最大 3 本選んで代わりにコンパイルする
- リンクしないので、エディターが起動中でも使える
- 新規ファイルは GLOB に載っていないので自動で再 configure する (初回は数分)
- 出力は `ERROR path:line:col CODE message` と最後の `RESULT ok|failed`。全文は `RESULT` 行の log=
- `NOT-COMPILED` が出たら «何もコンパイルされずに成功した» ではなく失敗扱い。パスと CMake の登録を確かめる
- 時間がかかるので Bash の `run_in_background` で回し、終わってから `RESULT` 行を読む

## 2. テストを回す (契約を変えたとき)

```
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 build FBZZTestsEditorAuto
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 test -Filter <正規表現>
```

- `build` はリンクする。`LNK1168` が出たら起動中の FBZZEditor / テスト exe が出力を掴んでいる。ユーザーに閉じてもらうよう依頼する (勝手に taskkill しない)
- 新しいテスト `.cpp` は `Projects/Tests/CMakeLists.txt` の `SOURCES` へ手で 1 行足す (lint が `test-not-registered` で知らせる)

## 3. ビルドが重なったとき

ツリーは人のタスク (`GameHub + SDK` / `CMake: Build *` / `Tests: *`)・別エージェントと共有している。MSBuild は同じツリーを 2 本で触ると中間ファイルを奪い合い、コードのエラーと同じ形で落ちる。

| 出力 | 意味 | すること |
|---|---|---|
| `RESULT busy` + `BUSY pid=...` 行 | 始める前に別のビルド (`cl.exe` / `link.exe` / `MSBuild.exe` / `cmake.exe` / `ctest.exe`) を見つけて断った | 待つ。Monitor で上記プロセスが消えるまで待ってから同じコマンドを 1 回出す。長引くならユーザーに何が走っているか確かめる |
| `HINT CONTENDED` | 途中で重なった (C1041 / `C1083 Permission denied` / LNK1104 / MSB3491 / MSB3026 / "being used by another process") | コードを直さない。上と同じく待ってから 1 回だけ再実行する。再実行で消えれば重なりだった |
| `HINT LNK1168` | 起動中の exe が DLL を掴んでいる (ビルドの重なりではない) | `check` に切り替えるか、ユーザーに終了を依頼する |

やってはいけないこと:

- `taskkill` で他のビルドを止める (人の SDK 公開を壊す)
- `build/` や `.obj` / `.pdb` / `.tlog` を消す
- `-Preset debug` 等の別ツリーへ逃げる (Engine / Editor の全体コンパイルになり、メモリも奪い合う)
- 再試行のループを回す。自分の `check` を複数本 (サブエージェント・`run_in_background`) 並べる

## 4. 規約

- 編集ごとに PostToolUse フックの `Projects/DevTools/AgentLint/lint.mjs` が走る。`ERROR` は直してから進む。まとめて見るなら `node Projects/DevTools/AgentLint/lint.mjs --changed`
- シェーダーを直したら `Assets/Shaders` と `GreenWare/Assets/Shaders` の両方を同じ内容にする (lint の `shader-copies`)

## 5. 報告

- `RESULT` 行をそのまま添える。通していないものは «未検証» と書く
- 実際の挙動の確認が要るなら `playtest` スキルでシナリオを回す
