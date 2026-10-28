<!-- @file    Report.md -->
<!-- @brief   残るPresent内スパイクの実測、描画状態削減案の棄却と復元確認。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# OS・ドライバー側に残るスパイクの調査

**採用できる性能改善は確認できず、本番の最適化は追加しなかった。** 試作した入力アセンブラーの状態再利用は、全件可視の主比較P99が1.0227 → 1.0585 ms（+3.5%）、再測定では1.1449 → 1.0391 ms（-9.2%）と変動した。同一版同士でも+7.2%動き、描画投入の中央値にも明確な短縮がない。試作のprivate実装2ファイルとLTO入力mapを開始時のbyteへ戻し、再ビルド・回帰テストまで完了した。前回までの最適化は保持している。

今回確認できたのは、**複数の長いフレームがDXGI Present内部にあること**と、別のフレームでBeginFrame内の長いblocked区間があったこと。ベンダードライバーの内部原因や、そのblocked区間を特定のFence待ちと断定するには証拠が足りない。

## 新しいトレースで確認したこと

前回の共通Forwardバインド再利用を含む版を今回のbaselineとして保存し、同じnative計測exeでCPUトレースを再取得した。管理者の名前付きWPRセッションを開始・終了し、全3プロセスの終了コード0を確認した。4,628,388イベント、イベント欠落0、バッファ欠落0、QPC 10,000,000 Hz。対象PID/TIDの600計測フレーム×3実行へ絶対QPCで照合した。WPR中の標本は正式な性能比較へ混ぜない。[原本の場所・SHA256・取得状態](TraceEvidence.json)、[実行物](TraceRuntimeManifest.json)。

| 実行・serial | フレーム | BeginFrame側 | RenderSystem | EndFrame側 | スケジューラ上の内訳 |
|---|---:|---:|---:|---:|---|
| r0・895 | 2.6812 ms | 0.0170 ms | 0.6865 ms | 1.9770 ms | scheduled 1.6513 / blocked 0.9588 / ready 0.0711 ms |
| r0・896 | 5.3132 ms | 3.8255 ms | 1.0404 ms | 0.4468 ms | scheduled 1.5261 / blocked 3.7697 / ready 0.0174 ms |
| r2・849 | 1.6167 ms | 0.0152 ms | 0.7243 ms | 0.8767 ms | scheduled 1.2768 / blocked 0.3220 / ready 0.0179 ms |

serial896の長い単一区間3.7562msはBeginFrame側に位置する。switch-outのスタックは取得できていない。wait reason=6や隣接フレームの状態だけでFence待ち・GPU過負荷と特定しない。フレームスロットは2、swap chainは3 bufferであり、隣接serialの遅延だけで待っていた提出番号の因果関係も証明できない。[位相と待機](PhaseEvidence.json)、[scheduler原データ集計](ScheduleSummary.json.gz)、[空のswitch stackも含む記録](BlockingStacks.json.gz)。

## Presentまで絞った根拠

Release LTOの保存DLLのPE export・unwind開始位置とサンプルを照合し、さらにそのDLLだけのEndFrame関数を逆アセンブルした。preferred image baseは0x180000000、EndFrameは0x180121D30〜0x1801221FD。スタックの戻り番地0x180122136は `call [rax+40h]` の直後に一致し、ソースとCOM vtable上の `IDXGISwapChain::Present` に対応する。0x180121FDAは `ExecuteCommandLists`、0x18012200Bは `Signal` の直後に対応する。ASLR後の実アドレスをそのまま比較せず、各PIDのimage baseからRVAへ戻した。[戻り番地一覧](EndFrameCallSites.json)、[対象関数だけの逆アセンブル](GraphicsDisassembly.txt.gz)、[対応ソース](../../../Projects/Graphics/src/Renderer/Platform/DX12/DX12Context.cpp)。

計測フレーム内のEndFrame戻り番地はPresent側234件、ExecuteCommandLists側59件、Signal側5件。これはサンプリング件数であり、呼び出し回数や厳密な時間比ではない。r0 serial895、r2 serial849を含む複数の遅いフレームでPresent側の戻り番地とNVIDIAドライバー内のleafが一致した。未公開のドライバー内部ルーチンはmodule+RVAのままで、名前を推測していない。

既存CPU scopeを `DX12Frame::` だけへ絞る計測オプションをharnessへ追加し、600標本を前後それぞれ完全記録した。baselineのserial727はEndFrame側0.5297msのうち `DX12Frame::End` が0.5266ms、`DX12Frame::Present` が0.4862ms。一方この別実行のFence scope最大値は0.0004msで、ETW実行の3.7562msをそのままFence待ちと説明する証拠にはならない。異なる実行のスパイクを同じ事象として扱わない。[フレーム単位のscope](BoundaryEvidence.json)、[集計](Diagnostic-profile-Summary.json)。

PresentはVSync無効でもドライバー/OS側処理を通る。[MicrosoftのPresent契約](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)。CPUトレースだけでは、GPU hardware queueの滞留、他プロセスとの競合、カーネル割込み、ベンダー内部同期のどれが各遅延を作ったかまでは確定していない。

## 実装して比較した案

通常Meshの連続Drawで同じ頂点view・index view・topologyを毎回送る処理を、privateの直前値との比較へ変更した。GPU VAだけでなくサイズ・stride・formatを比較し、CPU更新で新しいsnapshot VAが出れば再設定する。BeginFrame、Compute、shader reload、共有command listの世代変更と再取得では既存の状態無効化と同時に破棄する。ExecuteIndirectの後もviewを無効化する。バッファ解決・状態遷移・snapshot生成・Draw件数は維持した。[棄却した実装差分](TaskDelta.patch)、[D3D12の状態継承](https://learn.microsoft.com/en-us/windows/win32/direct3d12/managing-graphics-pipeline-state-in-direct3d-12)、[間接描画後の状態](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing)。

この案はドライバーへの冗長設定を減らすための試作で、Present内部の待機原因を取り除いたと仮定していない。全件可視のForwardOpaque P50は主比較0.2919 → 0.2937ms、再測定0.2946 → 0.2937ms。入力設定の削減をCPU時間の安定した短縮として確認できず、以下の回帰もあるため採用しなかった。

## 前回と同条件の実描画比較

既存Release LTO、i7-14700F / RTX4070、DX12 native、1920×1080、Forward / Unlit、instancing・shadow・VSync無効。affinity240（4論理CPU）、High priority、HighQoS検証済み。A/B/B/A/A/Bで各3プロセス。全件可視は600 warmup、他は120 warmup、その後各600標本。前後は同じexe・shaderを使い、DLLだけ隔離した。ユーザー承認済みのFBZZ既存環境を使った。

CPU Profiler停止で、変更適用・WaitForCpuFrame・RenderSystem・EndFrame/Presentを含む経過時間を測る。読戻しとログは計測外。P50は3プロセスのP50の中央値、P95/P99は各1,800標本の分位点。単位ms、前 / 後、負の変化は時間短縮。「後」は最終採用版ではなく棄却した候補。[環境](Final-gpu-Environment.json)、[集計](Final-gpu-Summary.json)。

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 1,000体・全件可視 | 0.8325 / 0.8043 | -3.4% | 0.9599 / 0.9367 | 1.0227 / 1.0585 | +3.5% |
| 10,000体・100体可視・無変更 | 6.2466 / 6.5842 | +5.4% | 6.7730 / 8.4908 | 7.3331 / 11.2943 | +54.0% |
| 10,000体・1%変更 | 6.2390 / 6.1674 | -1.1% | 6.7962 / 7.0851 | 7.1200 / 8.8512 | +24.3% |
| 10,000体・全件変更 | 6.4565 / 6.4953 | +0.6% | 7.2317 / 7.3437 | 8.0390 / 7.7317 | -3.8% |
| 10,000体・反対向き2ビュー | 13.7100 / 13.8509 | +1.0% | 17.4476 / 14.9291 | 22.0346 / 17.4448 | -20.8% |

全計測フレームで描画件数・cull件数・三角形件数と1ビュー目の最終画像hashは一致。1,000体の個別描画は維持した。2ビュー目は320×180で反対側9,900体を描き、件数と非背景画素を検査するが、その画像hashは取得していない。

## 独立再測定と同一版同士の対照

主比較後、全件可視だけを同じ600 warmup・600標本・各3プロセスで再実行した。続いて両側にbyte同一のbaseline DLLを置き、同じ順序でA/A比較した。[再測定](Confirm-gpu-Summary.json)、[対照](Control-gpu-Summary.json)。

| 実行 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 独立した再測定 | 0.8226 / 0.8091 | -1.6% | 0.9713 / 0.9336 | 1.1449 / 1.0391 | -9.2% |
| 同一版のA/A対照 | 0.8001 / 0.8258 | +3.2% | 0.9279 / 0.9470 | 1.0474 / 1.1229 | +7.2% |

主比較と再測定でP99の向きが変わり、A/A対照でも差が生じた。再測定の-9.2%だけを修正の効果と扱わない。10,000体の+54.0%等もこの変更だけの厳密な因果推定値とはしないが、採用を支える結果ではない。

| 実行 | 版 | 各試行のP50 (ms) | 各試行のP99 (ms) |
|---|---|---|---|
| First | Baseline | 0.8123, 0.7819, 0.8162 | 1.0775, 0.9413, 1.0556 |
| First | Candidate | 0.7918, 0.8001, 0.8021 | 0.9627, 1.0199, 1.0172 |
| Final | Baseline | 0.8784, 0.8325, 0.8251 | 1.0228, 1.0212, 1.0210 |
| Final | Candidate | 0.8202, 0.8005, 0.8043 | 1.1075, 0.9857, 1.0420 |
| Confirm | Baseline | 0.8708, 0.8226, 0.8054 | 1.1681, 1.1100, 1.0602 |
| Confirm | Candidate | 0.8083, 0.8091, 0.8216 | 1.0945, 0.9780, 1.0436 |
| Control | Baseline | 0.7957, 0.8001, 0.8491 | 0.9553, 1.0559, 1.0480 |
| Control | Candidate | 0.8258, 0.8065, 0.8341 | 1.1325, 1.1061, 1.0718 |

Firstは初回比較、Finalは全5条件、Confirmは独立再測定、Controlは同一版対照。詳細scope絞り込みをharnessへ追加してからFinal以降を実行し、各比較内では同一exeを使った。標本を選別・除外せず全分布を保存した。[試行別値](VisibleTrialDistributions.json)。

## 正しさ、復元、残した変更

- 新規のnative回帰テストは、同じ頂点・index handleのCPU更新、別バッファ・stride・topology切替、Compute挿入、間接描画、4フレームのring再利用を実デバイスの読戻しで検査する。読戻しは全Drawの後に行い、過去snapshotの上書きも検出する。
- 頂点viewの変更を無視する変異で1テストが失敗し、ソース・mapをbyte単位で復元すると同一exeで合格した。[失敗結果](Negative.json.gz)、[復元後](Restored.json)、[exe一致を検証した実行物](Restored-Identity.json)。
- 候補版でGraphics CPU304件、native Graphics53件（既存1件無効）、skinning58件が合格。本番試作を撤去・再ビルドした最終状態でも同じ計415件が合格。[最終native](BaselineRestored.json)、[CPU](FinalCpu.json)、[skinning](FinalSkinning.json)。
- 今回のprivate本番実装2件とLTO入力mapは開始時のbyteへ戻した。全1,358 native入力の鮮度・6 DLLのexport名/ordinal・A/Aの全DLL byte一致・比較exeの一致を検証。計測候補の監査と、復元後の最終状態を区別する。[候補監査](FinalArtifactAudit.json)、[再測定候補監査](Confirm-ArtifactAudit.json)、[撤去監査](RetirementAudit.json)、[最終状態](FinalStateAudit.json)。
- 残すソース変更は上記回帰テストと、`FBZZ_REGULAR_MESH_PROFILE_SCOPE` による計測scopeの絞り込みだけ。未設定時は従来どおり全scopeを対象とし、正式タイミング比較ではProfilerを停止する。[残したテスト差分](RetainedTestChanges.patch)。

## 次に必要な切り分け

Present内のどのGPU/DWM/カーネル区間が待たせているかには、CPUスタックだけでなくGPU hardware queueの時系列が必要。今回は、存在しないswitch-outスタックや未公開ドライバー名を推測して原因を断定しなかった。

Presentを待たずに表示を見送る方式、フレーム先行数・表示待機の方針変更は、表示更新・入力遅延の契約を伴う。現行の同条件比較で安定した改善が確認できた変更としては提出していない。今回の結果だけを根拠にOS設定変更や表示品質低下を勧めない。

## 保存物

各実行の `REGULAR_MESH_JSON` 原文をmtime=0のgzipで保存し、原文・圧縮後のSHA256と出典を [RawManifest.json](RawManifest.json) へ記録した。ETL原本はScratchに保持し、約0.65GBの原本をリポジトリへ複製しない。抽出・解決・逆アセンブル・計測スクリプトも `raw/` に保存する。作業一式は `K:\FBZZ_Engine\Scratch\DriverSpikes20261023`。

再計測は `Measure.ps1 -Label <新しい名前> -AllCases` と `Analyze.py <名前> gpu`。A/Aは `-SameBinary`。詳細計測は `FBZZ_REGULAR_MESH_PROFILE_SCOPE=DX12Frame::` と `-Profile -ProfileSamples 600`。既存Labelは保存済みDLLを再利用するため、新ビルドには必ず新Labelを使う。
