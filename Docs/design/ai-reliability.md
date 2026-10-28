# AI連携の状態・変更・検証・協調実行

既存のEditor Command Bus / EditorMcpを使い、状態の取得、変更の事前確認、作者を確認する取り消し、実行後の検証、複数AIの協調所有を追加する。接続先、Named Pipe制限、`read / dry-run / write`の権限契約を維持する。既存ツールは引き続き使える。サーバーの再起動・SDK公開・Editor更新は制作担当と調整する。

## 現行機能と補強

| 項目 | 従来 | 追加 |
|---|---|---|
| 現在状態 | `editor.state`、`scene.snapshot`、選択、個別のcomponent / script Query | `editor.observation`で同一メインスレッド呼び出しの状態・Scene・選択・検証・build・ログを返す。選択または明示IDのcomponent / script inspectionも含む |
| 変更前検証 | dry-runとUndoグループ`editor.transaction` | `editor.checked`によるScene・履歴precondition、変更前/予定値のfield差分、同じ呼び出しでの読み返し |
| 失敗時取り消し | 通常のUndo | 同一呼び出しのreadback失敗をUndo。後からはowner / receipt / Scene / 履歴が一致する場合だけ`editor.rollback` |
| 実行後検証 | `run_transaction_and_observe`のQuery集合 | 保存・再読込・今回のログ・参照検証・画像取得を個別にsuccess / failure / unverifiedで返す |
| 複数AI | 説明文と制作担当の調整 | 明示opt-inのtarget lease / capability / operation pin。buildとEditor操作は共通execution資源を使う |
| CPU性能証拠 | 従来の`profiler.snapshot`のsample/描画統計 | 従来fieldを保持し、記録可用性・完全性・収集filter・種別別欠落数・表示上限・警告を追加。未取得と不完全な記録を完全な計測として扱わない |

旧`editor.transaction`はlegacy CommandのUndoグループであり、一般的なatomic保証は持たない。MCPの説明とnative応答はその点を明示する。builder実行で`UndoStack::Execute`がfalseを返した場合は成功応答を返さない。

## 同一呼び出しの状態取得

MCP `editor_observe_structured`はnative `editor.observation`を呼ぶ。結果は`schemaVersion:1`、`consistency:"main-thread"`と以下を含む。

- `state / selection / snapshot / validation / build / logs / undoHistory`。取得できない項目は省略し、`checks.<項目>.status:"unverified"`と理由を必ず返す。
- `components`は選択ID、または`ids`で指定した最大128 IDの既存`node.components`。各Scriptの既存`script.inspect`による設定・観測・参照診断も含む。選択が128件を超える場合も状態/選択は返し、inspectionは128件と`componentIdsRequested`、未検証のcheckを返す。失敗したinspectionは値を捏造せず未検証とする。
- `guard`はproject / scene / Undo revision / cursor / native Scene snapshot。生成物と非公開Componentはsnapshotの対象外。Edit modeと記録可能なUndoStackが必要。Play復元待ち/managed build中は変更用guardを発行しない。

buildの`managedRequestId / packageRequestId`、ログの`afterSequence`を指定できる。buildは要求IDの`terminal`と状態を確認し、過去のlatestを今回の成功と扱わない。ファイルシステムや非同期build workerはメインスレッド外なので、同一時点のatomic snapshotと称しない。

`scene.validate`が検査するnative参照範囲は既存規則のまま。選択外Scriptや全ての任意GUID参照を検査したという意味ではない。必要なIDを明示し、専用inspectionの診断も見る。

追加APIの無い稼働中Editorでは従来Queryへ戻し、`consistency:"multi-call-unverified"`、guard無しを返す。既存接続を維持できるが、新しいchecked変更は拒否する。

## plan・変更・作者の取り消し

`editor_plan_checked`は必ずnative dry-runを実行し、変更前と予定値のfield差分、60秒間有効なsingle-use plan、必要targetを返す。Commandとguardは入力とは独立したJSONコピーで保持する。最大32 planを保持する。

`editor_apply_checked`は自分のenforced leaseをoperation pinへ原子的に固定し、共通executionを取得する。nativeは変更直前にguardを再比較する。この比較とCommand実行は同じdispatcher呼び出しなので、間に別のAI/UI要求が入らない。履歴を通さない直接のnative field変更もScene snapshot比較で検出する。planはdispatch前に消費し、timeoutや切断時に自動再送しない。

対応範囲は`node.rename / node.setActive / node.setTag / node.setLayer / transform.set / component.set`のnative公開field。複数回書くfieldは最終予定値を検証し、Undoは変更前へ戻す。読み返しはnative floatへの変換を許容する相対誤差1e-6、それ以外は型と構造を比較する。失敗は単なるaccepted応答で成功扱いにしない。

ファイル作成、node作成/削除、Prefab変更、焼き、build、managed設定batchは混在を拒否する。managed設定には既存のchecked Configure契約を使う。native checked応答は常に`atomic:false`。保証範囲は対応fieldの読み返しと条件付きmemory rollbackであり、外部I/Oのatomic保証ではない。

成功receiptはdispatcherとMCPの双方で最近8件保持し、native guardの上限は4 MiB。native receiptは1..128 byte識別子、MCPはUUIDで生成する。ownerはMCPセッションごとのUUIDで、表示labelとは独立。`editor_rollback_checked`はそのセッションのreceiptのみ受け付け、nativeがowner・Scene個体・完全guard・最上位Undo descriptionを照合する。他作者の変更、履歴遷移、直接field変更があれば`ROLLBACK_CONFLICT`として何もUndoしない。

自動rollbackは同一呼び出しのfield読み返し失敗、または未保存の検証失敗に限る。保存後の検証失敗では自動Undoしない。手動rollbackもmemory-onlyで、保存済ファイルを戻さない。適用/検証の失敗時と取り消し成功時はoperation pin終了後にtarget leaseを解放する。

## 保存・再読込・描画・エラー

`editor_verify_checked`はreceiptの変更後guardを使う。save/reloadはnative `editor.finalize`がguard確認と既存`scene.save / scene.open`を同一呼び出しで行う。reloadには`save:true`が必須で、`discardUnsaved`は使わない。保存callback成功でもSceneがdirtyなら失敗、保存失敗時は再読込を行わない。再読込後にnative snapshotが完全一致しなければ失敗を返す。正規化で変わる値も一致判定では無視しない。

保存だけのsuccessは既存host callback完了とclean状態の確認。ディスクroundtripの確認にはreloadが必要。save/reloadは非Undo操作なので`atomic:false`。再読込後のreceiptは失効する。

今回のログはplan時のcursorから読み、errorがあればfailure。ログのdrop・truncation・cursor不明はunverified。参照検証のerrorはfailure。取得不能を0件と読み替えない。

`viewport.semantic`のPNG・寸法・objectsが取得できればcaptureはsuccess。現行render Queryに提出frameの正本が無いため、freshな描画や見た目の正しさを`verified`とはしない。renderはunverifiedとして画像を証拠に返す。視覚の合否は既存Playtestのframe手順と`compareImage`、基準画像を使って確認する。既存のGPU/Editor専有担当へその実行を調整する。

## 協調所有と実行排他

`editor_resource_claim`の既定は`mode:"trial"`。trialまたはdry-run権限では状態と予定targetのみ返し、所有を取得しない。`write`かつ`mode:"enforced"`が明示opt-in。旧ツールや人の作業を突然締め出さない。

所有targetはprojectとsceneから作ったSHA-256 scope、大小文字を統一したnode UUIDの階層で表す。Scene全体のclaimはそのnode claimと競合する。tokenは`{leaseId,capability}`で、表示名だけでは更新・解放できない。capabilityのhashのみをローカルregistryへ保存し、statusには秘密値を含めない。

Registryは一時領域の`fbzz-editor-mcp-coordination/<namespace hash>/`。既定namespaceは`fbzz-ai-reliability-v1`で、参加するbuildとEditor操作がmachine-wideの`execution`を共有する。公開先はネットワークではない。registry初期化はopt-in機能を呼んだ時のみ行い、旧ツールの登録時には行わない。

状態は完全に書いたnonempty generation directoryをexclusive atomic renameで公開する。取得判定をした後に別の作者が先行した場合は、新generationから判定し直す。lease期限切れでも実行pinのPIDが生きている間は回収しない。PID再利用や生存検査の拒否は安全側で保持する。クラッシュしたPIDのpinは次のregistry操作で回収する。破損registryやatomic publication不能はfail closed。履歴generationは自動削除しない。

`Assert`は瞬間の確認だけであり、操作には`RunWithLease`を使う。`RunExclusive`はcallbackが実際に完了するまでpinを保持し、成功/失敗ともfinallyで解放する。表示job名が同じでもopaque IDは別。`editor_build_managed_exclusive`は既存managed build要求のIDがterminalになるまでbackground monitorでpinを保持する。要求IDを取得できないtransport失敗は再送せず、backend idleが確認できるまで保持し、結果はunverified。通信不能のままならpinを維持する。

協調排他は参加者のみが対象。既存raw bus、通常のUI、手動VS、native AgentBuildを自動的にロックするものではない。native buildのCPU予約は引き続き親/制作担当とAgentBuildで調整する。managed buildからMCPがクラッシュしても、次のchecked Editor操作はnative `managedBuildBusy`確認で拒否する。強制解除、資格情報追加、OAuth、承認迂回、ネットワーク公開は行わない。

## 実用API例

Nodeは配布済みdependenciesのまま使用する。SDK更新や常駐サーバー再起動は別途調整する。

```json
{"tool":"editor_observe_structured","arguments":{"ids":["11111111-1111-4111-8111-111111111111"]}}
{"tool":"editor_plan_checked","arguments":{"label":"魚の配置補正","cmds":[{"t":"transform.set","id":"11111111-1111-4111-8111-111111111111","pos":[0,1,2]}]}}
{"tool":"editor_resource_claim","arguments":{"ids":["11111111-1111-4111-8111-111111111111"],"mode":"enforced","ttlMs":60000}}
{"tool":"editor_apply_checked","arguments":{"planId":"<plan UUID>","token":{"leaseId":"<lease UUID>","capability":"<claimが返した値>"}}}
{"tool":"editor_verify_checked","arguments":{"receipt":"<apply receipt>","token":{"leaseId":"<lease UUID>","capability":"<claimが返した値>"},"save":true,"reload":true,"render":false}}
```

再読込は現在Sceneを変える操作。稼働中のユーザーSceneへ例をそのまま送らず、対象Sceneと制作担当の所有を確認して実行する。未保存の取り消しは`editor_rollback_checked`へreceipt/tokenを渡す。長い作業は`editor_resource_renew`、正常終了時の残った所有は`editor_resource_release`で解放する。

```json
{"tool":"editor_build_managed_exclusive","arguments":{"name":"managed-build"}}
{"tool":"editor_exclusive_job_status","arguments":{"jobId":"<opaque job UUID>"}}
{"tool":"editor_resource_status","arguments":{}}
```

隔離MCPコピーでの回帰テストとnative担当へのCLI例:

```powershell
npm --prefix <isolated-EditorMcp-directory> run build
node --test "<isolated-EditorMcp-directory>/dist/tests/*.test.js"
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 check Projects/Editor/src/Ai/EditorBusDispatcher.cpp Projects/Editor/src/Ai/Bus/EditorHandlers.cpp Projects/Tests/Editor/Auto/EditorBusDispatcherTests.cpp
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 build FBZZTestsEditorAuto
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/AgentBuild.ps1 test -Filter 'EditorBusReliabilityTest|EditorBusProfilerSnapshotTest|EditorBusDispatcherTest|EditorBusFluidTest|BusContractParityTest'
```

native check/testは共有CPU枠で直列に実行し、Editor再起動やSDK公開を独自に行わない。構造化結果は個々のstatusとscopeを読む。accepted、capture取得、過去build成功、dry-run合格だけで制作完了と判断しない。

## 今回の検証記録（2026-10-11）

隔離コピーのTypeScript compileは成功。既存MCP回帰と新しいreliability/resource control検査の全110件が通過し、failure / skip は0件。新しいreliability serviceのfixtureはnative動作を模擬したMCP契約テストであり、native実機確認ではない。ResourceControlは実際の一時registryと独立processで、同時取得、disjoint変更の消失防止、live pin、crash後回収を検査した。

既存C++ fixtureへReliability 29件、Profiler互換4件を追加し、同ファイルは既存29件を含む計62件。snapshot/選択/詳細、guard、dry-run、作者と直接変更の競合、単回receipt、field失敗とUndo、fixture Sceneのsave/load、復元待ち、build busy、UUID表記、Profiler欠落/不完全なcaptureを対象とする。

Inspectorの枠解放後、共有CPU枠で変更cpp 3本のRelease checkと`FBZZTestsEditorAuto`のRelease buildが成功し、いずれもerror / warningは0件。CTestは上記62件と`BusContractParityTest`3件を合わせた65/65件が通過し、失敗0件、実行時間2.52秒。予約枠での検証終了後、MSBuild / cl / link / ctestの残存processが無いことも確認した。これはCPU fixtureによるnative検証であり、稼働中Editorの描画確認は実行していない。

海Sceneへの書き込み/再読込、稼働Editor/接続MCPの再起動、SDK更新、commit/pushは行っていない。新native APIと新MCPツールの実稼働反映は制作担当と調整した再ビルド/再起動後になる。
