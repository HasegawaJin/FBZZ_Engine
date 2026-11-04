<!-- @file    play-modes.md -->
<!-- @brief   Editor 内 Play と別プロセス Standalone Play の操作・所有権・入力契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# Play モード

Game View 上部の Play 設定から、Editor 内で動かす方法と、独立したウィンドウで動かす **Standalone Game** を選べる。開始・停止には上部の Play / Stop ボタン、または既存の Play / Stop ショートカットを使う。

## 選び方と保存される値

| セレクターの項目 | 実行方式 | 表示方法 |
|---|---|---|
| Play Focused | Editor 内 | Game View へフォーカスを移す |
| Play Maximized | Editor 内 | Game View を最大化する既存レイアウト |
| Play Unfocused | Editor 内 | Game View へフォーカスを移さず開始する |
| Standalone Game | 別プロセス | ゲーム用の独立ウィンドウ |

内部では `PlayExecutionMode::IN_EDITOR` / `STANDALONE` と `PlayFocusMode` を分けている。`[game_view]` の `play_execution_mode` と `play_focus_mode` に別々に保存する。Standalone を選んでも直前の Focused / Maximized / Unfocused の値は保持される。Focus ボタンは Game View へフォーカスを移す操作で、実行方式は変更しない。実行中はモードのセレクターを変更できない。

Editor 内 Play は既存の実行用 Scene コピーを使い、編集 Scene と Undo を保持する。Standalone Play 中も親 Editor は編集状態を保つ。

Editor 内の Play / Pause 中は Scene View でオブジェクトを選択し、Gizmo で移動・回転・拡縮できる。ドラッグは実行中の Scene にだけ反映し、剛体の位置・回転は次の物理ステップで取り込む。Stop で実行用 Scene を破棄し、操作前の編集 Scene に戻る。Play / Pause 中の Gizmo 操作は保存・Undo の対象外とする。

## Standalone を開始する

1. Game View のセレクターで **Standalone Game** を選び、Play を押す。
2. 未保存の Scene、無題の Scene、未保存アセットがある場合は、**Save and Start Standalone Play** の確認画面が出る。共通ダイアログの **OK** が保存に同意して開始する操作、**Cancel** が編集を保持して開始を取り消す操作となる。
3. 無題 Scene は既存の Save As ダイアログで保存先を選ぶ。保存の失敗、Save As の取り消し、保存後も残る未保存アセットがあれば起動しない。Scene が外部から更新されている場合は、先に既存の再読込・保存の確認を解決する。

子プロセスへ渡すのは、現在開いている **保存済み Scene の絶対パス**。ProjectSettings の開始 Scene を書き換えず、同じファイル名の Scene が複数あっても選択したパスを登録する。Scene 相対の Terrain や付随ファイルの参照を保つため、この経路は共有一時 Scene ファイルを使わない。未保存内容を黙って保存・破棄する処理は行わない。

C# はプロジェクトの既存マネージドゲーム解決・初期化経路を使う。マネージドビルド適用中、配布ビルド中、Prefab 編集中など、既存の開始条件を満たさないときは開始できない。

## Stop、終了、再実行

Standalone 起動中の表示は **STANDALONE**、停止要求後は **STOPPING STANDALONE**。Pause / Step ボタンは既存の表記のまま無効になる。別プロセスの一時停止・コマ送りには対応していない。

Stop は、この Editor が今回起動して保持している子プロセスだけを対象にする。まずその PID のウィンドウへ `WM_CLOSE` を要求し、Editor の更新で約 2 秒の猶予を経ても終了しなければ、保持している同じプロセスハンドルを使って終了させる。停止の完了は待機で UI を止めずにポーリングする。二重起動は受け付けない。

子ウィンドウの通常終了やゲーム側の終了要求を検出すると、次の Play を開始できる。異常終了では終了コードとログの場所を Console / Toast に表示する。Editor 終了・プロジェクト切替では所有するセッションを終了させ、専用の kill-on-close Job で親の異常終了時にも所有プロセスが残らないようにする。別途起動したゲームは、この Editor の Stop 対象にはならない。

ログは `<Project>/Library/StandalonePlay/<session>.log` に残る。起動中の STANDALONE 表示をクリックすると、そのセッションのログを開く。起動失敗や終了後は Console に記録されたログパスを確認する。

## 入力とカーソル

- **Shift+F1** はゲームの物理入力を解放する。終了や一時停止の操作ではない。
- Game View または Standalone ウィンドウを明示的にクリックすると再取得する。取得に使った押下は、そのゲーム入力へ流さない。
- OS フォーカスを失った後、Editor 内の Pause 後、ウィンドウ移動後などは、フォーカス復帰・再開だけでは再取得しない。
- Standalone の **Escape** はゲームに渡す。Editor 内 Play は、既存の Escape による捕獲解除・条件に応じた Stop を維持する。
- Editor の **Cursor: Free** はカーソルの表示・移動制限だけを一時解除する。ゲーム入力の所有権とは別の設定である。

OS 所有権の解放・再取得の判定には物理入力専用 API を使う。`input.inject` によるキーやクリックは OS カーソルを取得・解放しない。子プロセスの終了だけで、親 Editor が入力を再取得することもない。

`InjectMousePosition` のクライアント座標と注入ボタンは、ゲームスクリプト・Action Map の入力として使う。Host が入力を所有している間、それらを契機に実マウス位置をゲーム UI へ流さない。非表示の UI 自動操作には Canvas 空間の明示的な `UIPointer`、または UI ナビゲーション入力を使う。要求値・実効値、入力の所有権、仮想入力と UI の座標系の詳細は [Cursor とゲーム入力の所有権](cursor-control.md) を参照する。

## Operator と起動引数

既存の Command Bus からは、次の kind とペイロードを使う。実行方式の変更は次回 Play に適用され、保存されている表示方法は変更しない。

```text
command {"t":"editor.op.invoke","id":"play.set_execution_mode","args":{"mode":"standalone"}}
command {"t":"play.control","action":"start"}
query   {"t":"editor.state"}
command {"t":"play.control","action":"stop"}
command {"t":"editor.op.invoke","id":"play.set_execution_mode","args":{"mode":"inEditor"}}
```

`editor.state` の `executionMode` と `standaloneActive` / `standaloneStopping` / `standaloneFailed` / `standaloneExitCode` / `standaloneLog` / `standaloneDiagnostic` を確認する。既存の `playState` は Editor 内の実行状態を表す。`standaloneActive` は子プロセスの所有中を示し、Scene 初期化完了を保証する値ではない。未保存確認が必要な開始要求もあるため、要求の応答だけで起動完了と判断しない。

Editor が内部で生成する起動引数の形は以下となる。普段の操作は UI または上記 Operator を使う。

```text
FBZZEditor.exe --standalone --editor-play --project "<absolute project>" --play-scene "<absolute scene>" --play-log "<absolute session log>"
```

`--editor-play` は Editor が所有する Play 用の起動経路で、プロジェクト・Scene・ログには絶対パスを要求する。この経路では親が保持する PID を変えないため、ランチャーの自己再起動や自動再ビルドを行わない。引数だけを手動実行しても、既に開いている Editor の所有セッションとしては登録されない。配布ゲームの起動方法とは別の契約である。

## 確認手順

リポジトリルートから、共通ビルドを直列に実行する。`check` には変更した C++ ファイルを指定する。次はプロセスサービスと起動経路を確認する例となる。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 check Projects/Editor/src/StandalonePlaySession.cpp Projects/Editor/src/EditorApp_StandalonePlay.cpp Projects/EditorLauncher/src/main.cpp Projects/Engine/src/Scene/StandaloneProjectModule.cpp
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 build FBZZTestsEditorAuto
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 test -Filter "StandalonePlaySessionTest|EditorSettingsTest"
```

サービスのテストは、起動失敗、二重起動、Stop の猶予、終了コード、再実行、所有権の分離、Windows 引数の受け渡し、設定の保存を検証する。実ウィンドウと OS の動作は、検証用プロジェクトで次を確認する。

1. ProjectSettings の開始 Scene と異なる Scene を開き、Standalone で現在の Scene が起動する。同じ名前の Scene が別の場所にあっても、選択したファイルが使われる。親 Editor の編集 Scene、選択、Undo を保持する。
2. 未保存確認の Cancel と Save As の取り消しでは子プロセスを開始せず、編集内容を保持する。OK では既存の保存経路を通り、保存失敗時は開始しない。
3. 二重起動を拒否し、Stop は所有セッションだけを終了する。別途起動したゲームは動作を続ける。停止後は再実行できる。
4. 通常のウィンドウ終了、ゲーム側の終了要求、検証用の所有プロセスによる異常終了をそれぞれ再現する。状態の更新、異常時の終了コード・ログ、再実行を確認する。
5. 所有セッションを起動した Editor の終了・プロジェクト切替で、そのセッションが残らない。別の Editor やゲームには影響しない。
6. OS フォーカス喪失や Shift+F1 で入力が解放され、フォーカス復帰だけでは取得されない。明示的なクリックで再取得し、そのクリックがゲームスクリプトやゲーム UI の操作へ流れない。
7. 注入した解放キー・クリックでは OS カーソルを取得・解放しない。クライアント座標の注入と Canvas 空間の `UIPointer` を使う UI 自動操作を分けて確認する。
8. Standalone 中の Pause / Step が無効で、Editor 内 Play に戻すと既存の操作が使える。表示方法と実行方式の保存値を、Editor 再起動後も個別に保持する。
