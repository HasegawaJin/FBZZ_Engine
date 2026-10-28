<!-- @file    Report.md -->
<!-- @brief   全件可視1,000体のP99追試、重複転送削減案の棄却と測定変動の検証。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# 全件可視1,000体のP99調査と改善案の比較

**P99の安定した改善は確認できず、試作した最適化は採用しなかった。** 主比較はP99が3.8831 → 1.6678 msと短縮したが、独立した再測定は1.4838 → 1.7886 msと逆転した。同一版同士でも1.7226 → 1.4603 msと変動したため、主比較だけを改善の根拠にしない。

試作のDX12Renderer.cpp / private header / 設計追記とLTO入力mapは今回開始時の状態へ戻した。追加したGPU回帰テストと、既定では無効のCPU実行サイクル診断を計測テストに残す。描画結果の破綻は見つかっていない。P99スパイクの最終原因の断定と、P99改善の採用には至っていない。

## 調査結果と変更

前回のP99 1.3673 → 1.7879 msは、候補版の最後の試行で遅いフレームが集中した影響が大きかった。その試行のP99は2.0967 msで、遅い6フレームはserial 829, 828, 850, 849, 851, 831。GPUのフレーム待機はP99でも約0.0003 msで、遅延は主にRenderSystemとEndFrame/Present側に現れた。[前回標本の分析](PriorTail.json)、[前回報告](../2026-10-23-partial-mesh-motion/Report.md)。

前回の変更前・変更後DLLを同じ計測exeで追試するとP99は1.2905 → 1.3545 msだった。前回の+30.8%という大きさは再現しない。全件可視では材質補完の未処理項目がなく、前回追加した「材質需要なし」の再利用処理は実行されない。この変更を前回P99後退の原因と断定できない。

600フレームの別実行のCPU内訳では、ForwardOpaqueの描画投入が約0.51 msを占めた。遅いフレームでは複数のCPU区間が同時に延びており、単一の新しい処理に限定した周期的スパイクは確認できなかった。OSの割込み・スケジューリング・ドライバー内部のどれかまでは切り分けていない。[CPU内訳](Reproduce-profile-Summary.json)、[遅いフレームのscope](Reproduce-TailScopes.json)。

試作した案は、同じ参照番号を使う連続Drawでも毎回行っていたb14の添字定数ブロックの確保・176 byte転送・root CBV設定の削減。DX12Rendererに直前の全添字とGPU VAを一つだけ保持した。毎DrawのResetと現在の資源解決は残し、pixel / vertex / UAVの全要素が一致するときだけ転送と再設定を省いた。ハンドルやhash一致で代用しない。

BeginFrame・Compute・root signature切替・共有command listの状態世代変更・再取得・shader reloadでは、既存のroot binding無効化と同時に再利用を解除した。確保失敗時も解除した。GPUの状態遷移、prepared skinningの保持、他の定数バッファ更新は毎回行い、描画順・描画件数・インスタンシング設定は維持した。保持する転送領域を上書きしない。[棄却した試作のprivate実装差分](TaskDelta.patch)。

## 同条件の実描画比較

以下は採用しなかった候補との比較で、beforeは前回の最終版。既存Release LTO、i7-14700F / RTX 4070、DX12 native renderer、1920×1080、Forward / Unlit、instancing・shadow・VSync無効。affinity=240（4論理CPU）、High priority、HighQoS。A/B/B/A/A/Bで各3プロセス、1,000体は600 warmup、他は120 warmup、その後各600標本。前後で同じexe・shader・計測ソースを使い、DLLを隔離した。

CPU Profiler停止で、変更適用・WaitForCpuFrame・RenderSystem・EndFrame/Presentを含む経過時間を測る。読戻しとログ出力は計測外。P50は3プロセスのP50の中央値、P95/P99は各1,800標本の分位点。単位ms、「前 / 後」、負の変化は時間短縮。[環境・実行物](Final-gpu-Environment.json)、[集計](Final-gpu-Summary.json)。

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 1,000体・全件可視 | 1.2102 / 1.2300 | +1.6% | 2.4823 / 1.4435 | 3.8831 / 1.6678 | -57.0% |
| 10,000体・100体可視・無変更 | 8.1680 / 7.2633 | -11.1% | 11.0911 / 11.5559 | 14.5407 / 14.2903 | -1.7% |
| 10,000体・1%変更 | 7.3868 / 8.2309 | +11.4% | 9.3377 / 11.1704 | 10.4271 / 13.7762 | +32.1% |
| 10,000体・全件変更 | 8.8511 / 8.4499 | -4.5% | 12.8519 / 11.0058 | 15.6120 / 13.7024 | -12.2% |
| 10,000体・反対向き2ビュー | 18.8948 / 18.7235 | -0.9% | 23.6220 / 23.9370 | 27.9138 / 29.3593 | +5.2% |

全試行の全計測フレームで、描画件数・cull件数・三角形件数・1ビュー目の最終画像hashが一致した。全件可視は1,000体を個別描画し続ける。2ビュー目は320×180で反対側9,900体を描き、件数と非背景画素を検証するが、2ビュー目の画像hashは計測しない。

## 再測定と同一版同士の対照

主比較の後に、全件可視だけを同じ600 warmup・600標本・3プロセスずつで再測定した。さらに両側にbyte同一の変更前DLLを置いたA/A対照を行った。主比較を取り替えず、全結果を残す。

| 実行 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 独立した再測定 | 1.1275 / 1.1231 | -0.4% | 1.2910 / 1.5259 | 1.4838 / 1.7886 | +20.5% |
| 同一版同士のA/A対照 | 1.2373 / 1.1264 | -9.0% | 1.4905 / 1.3428 | 1.7226 / 1.4603 | -15.2% |

A/A対照の差はコード変更による効果ではない。P99は試行間・時間帯の影響を含み、主比較で得た短縮率の全量を今回の修正の効果とは扱わない。前回の大きなP99後退が恒常的なエンジンの回帰だったと証明した結果でもない。

全件可視の試行別分布を示す。P99は各600標本、範囲を省略しない。[全試行の値](VisibleTrialDistributions.json)。

| 実行 | 版 | 各試行のP50 (ms) | 各試行のP99 (ms) |
|---|---|---|---|
| Reproduce | Baseline | 1.1014, 1.0403, 1.0298 | 1.3088, 1.1994, 1.2258 |
| Reproduce | Candidate | 1.0239, 1.0644, 1.1511 | 1.2696, 1.2414, 1.4574 |
| Experiment | Baseline | 1.4150, 1.1332, 1.1575 | 3.4796, 1.6704, 1.4651 |
| Experiment | Candidate | 1.2773, 1.1748, 1.0531 | 1.5986, 1.4605, 1.3778 |
| Final | Baseline | 1.2072, 1.2102, 1.8442 | 2.0582, 1.7756, 4.6044 |
| Final | Candidate | 1.1133, 1.2300, 1.2954 | 1.4106, 1.5637, 1.7427 |
| Confirm | Baseline | 1.0613, 1.1378, 1.1275 | 1.4417, 1.5873, 1.3720 |
| Confirm | Candidate | 1.1231, 1.2354, 1.1013 | 1.4586, 1.9185, 1.3783 |
| Control | Baseline | 1.1087, 1.2373, 1.2510 | 1.3102, 1.9133, 1.5748 |
| Control | Candidate | 1.1264, 1.2510, 1.0522 | 1.3980, 1.5160, 1.2787 |

## CPU内訳

最初の候補確認は前後各600フレームのCPU Profiler別実行。主表の性能値へ混ぜない。

| 区間 | 前P50 | 後P50 | 前P99 | 後P99 |
|---|---:|---:|---:|---:|
| pass:ForwardOpaque | 0.5894 | 0.5588 | 0.6813 | 0.6590 |
| RenderPipeline::Execute | 0.6550 | 0.6187 | 0.7781 | 0.7541 |
| RenderSystem::ExtractRenderScene | 0.3270 | 0.2902 | 0.5328 | 0.5207 |
| renderMs | 1.0971 | 1.0172 | 1.4725 | 1.4290 |
| endMs | 0.1830 | 0.1615 | 0.3751 | 0.4247 |
| frameMs | 1.2995 | 1.1988 | 1.7649 | 1.8520 |

最終5条件の内訳は各30標本の別実行として[別集計](Final-profile-Summary.json)に保存した。不完全なProfiler captureを0で補完しない。GPU passの時間は完了したsource/view付き標本であり、GPU全体時間・Editor全体のFPSとは区別する。upload量・確保回数・VRAM量は実測していない。

## 正しさ・実行物

- 実DX12の描画・バッファ検証: 25件合格、2件disabled、失敗0。固定seed 1023で並べ替え。[結果](Restored.json)。
- Graphics CPU回帰: 303件合格、0件disabled、失敗0。固定seed 1023で並べ替え。[結果](GraphicsCpu.json)。
- スキニング回帰: 58件合格、0件disabled、失敗0。固定seed 1023で並べ替え。[結果](Skinning.json)。

追加テストは実Drawを通じてb14全配列の指紋をGPUから読み戻し、同一値の連続描画、texture / PS buffer / auxiliary buffer / VS structured / VS raw / instanceの変更、未設定化、Compute挿入、3フレームのarenaリングを越える4フレームを検証する。全Drawが完了してから各出力を読み、先行Drawの転送領域が後から上書きされないことも確認する。テスト専用の公開APIは追加していない。

pixel全要素の比較を意図的に外すと追加テストは失敗し、ソースとLTO入力mapをbyte単位で復元すると合格した。同じテストexeを使い、リンク直後のGraphics DLLを隔離フォルダーへ配置した。[負例](Negative.json)、[復元後](Restored.json)、[負例の実行物](Negative-Identity.json)、[復元後の実行物](Restored-Identity.json)。

試作比較時のnative入力1,358件の差はDX12Renderer.cppとそのprivate headerだけ。公開header・他のnative入力・export名/ordinalは維持した。上の比較と対照には変更前の計測harnessを共通して使った。[試作比較時のソース監査](EvidenceAudit.json)、[試作比較時のRelease LTO・実export・同一exeの監査](FinalArtifactAudit.json)。

既存Releaseツリーで変更単位のコンパイル確認に加え、Graphics、CPU/GPUテスト、計測exe、EditorLauncher、SandboxStandaloneをビルドした。既存のMSB8028等の警告がある。

[生標本manifest](RawManifest.json)に全実験のgzipと解凍後SHA-256を列挙した。変更前DLLと全文ログ・再実行用スクリプトはScratch/VisibleMeshTail20261023に保持する。

## 最終状態とCPU実行サイクル診断

試作は取り除き、native入力1,358件とLTO入力map・設計文書は今回開始時とbyte単位で一致する状態へ戻した。復元後のGraphicsを既存Release LTOで再ビルドし、追加したGPU検証を含む4件が合格した。全6モジュールの実export名・ordinalも再検証した。[最終状態監査](FinalStateAudit.json)、[復元後のGPU検証](BaselineRestored.json)。

保持する変更はGPU回帰テストと、計測テストの`FBZZ_REGULAR_MESH_THREAD_CYCLES=1`で有効になる診断。[保持するテスト差分](RetainedTestChanges.patch)。CPUサイクルの問い合わせはフレーム経過時間の計測開始前・終了後に行い、画像読戻しと出力も従来どおり計測外。通常は診断を無効にしている。

診断は前の主比較とは別の新しい共通exeで実行した。両側のDLLは今回開始時の同一版で、同じ600 warmup・600標本・3プロセスずつ。3,600フレームすべてでサイクル取得成功・正値と描画結果の一致を検証した。[診断生値の集計](ThreadCycleDiagnostics.json)、[経過時間の集計](Diagnostic-gpu-Summary.json)、[環境](Diagnostic-gpu-Environment.json)。

各試行の通常フレーム中央値と、経過時間が最も遅い6フレームの中央値を比較する。サイクルは描画を呼ぶ主スレッドのuser/kernel実行を含み、別スレッドは含まない。サイクルをmsへ換算せず、周波数・待機・別スレッドの影響と単純なCPU処理量増加を区別するための補助データとして使う。[MicrosoftのAPI契約](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)。

| 試行 | 通常 / 遅い6フレーム (ms) | 経過時間の倍率 | 通常 / 遅い6フレーム (M cycles) | 実行サイクルの倍率 |
|---|---:|---:|---:|---:|
| r0-Baseline | 1.3179 / 1.8787 | 1.43 | 2.780 / 3.961 | 1.42 |
| r1-Candidate | 1.2974 / 3.4592 | 2.67 | 2.735 / 7.289 | 2.67 |
| r2-Candidate | 1.4087 / 2.7898 | 1.98 | 2.969 / 5.858 | 1.97 |
| r3-Baseline | 1.2200 / 1.7277 | 1.42 | 2.547 / 3.643 | 1.43 |
| r4-Baseline | 1.1260 / 1.3347 | 1.19 | 2.359 / 2.793 | 1.18 |
| r5-Candidate | 1.1170 / 1.4912 | 1.33 | 2.350 / 3.074 | 1.31 |

最も大きく延びた試行では、遅い6フレームの経過時間と主スレッドに計上されたサイクルが、ともに通常の約2.67倍になった。サイクル数は命令数ではなく、これだけでエンジンの命令増加・周波数変化・割込み・ドライバー内部を区別できない。少なくともGPUフレーム待機だけをP99後退の原因とする根拠は得られなかった。

この診断だけでは待機元やOSの割込みを名指しできない。P99の改善率を採用判断に使う前に、スケジューリング・他スレッド・ドライバー内部を追跡する必要がある。今回は再現できない改善案を本体へ残さず、比較と診断の根拠を保存した。

## 続行に必要なOSトレース

WPR / xperf / WPA exporterがインストール済みで、既存のWPR記録が動いていないことを確認し、CPU traceの開始を試した。Windowsが`0xc5585011`（`Failed to enable the policy to profile system performance`）で拒否し、採取は開始できなかった。[開始結果](TraceResult.json)、[エラー全文](TraceStartFailure.log)、[最終状態](TraceFinalStatus.txt)。エンジンのテスト失敗ではない。

[Microsoftの説明](https://learn.microsoft.com/en-us/windows/apps/develop/performance/disk-memory)では、このエラーは管理者モードでない場合に起こりうる。原因確定の次の段階には、管理者権限でのCPU trace採取が必要。採取用の一時スクリプト`Scratch/VisibleMeshTail20261023/Trace.ps1`を用意した。既存の記録がない場合だけ開始し、今回開始できた記録だけを停止する。管理者PowerShellから次を実行すると、復元済みbackendの1,000体条件を1回測って`Scratch/VisibleMeshTail20261023/Trace/Cpu.etl`へ保存する。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File K:\FBZZ_Engine\Scratch\VisibleMeshTail20261023\Trace.ps1
```
