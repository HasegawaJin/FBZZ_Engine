<!-- @file    Report.md -->
<!-- @brief   部分移動の描画遅延を再調査し、材質需要の重複走査を削減する。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# 1%変更時の描画費用の調査と改善

## 確認したこと

前回の最終版（静的geometry再利用あり）を今回のbeforeとした。前回報告の「導入前比+8.2%」を同じ旧版DLLと再比較すると、7.4581 → 7.4433 ms（-0.2%）で再現しなかった。[追試集計](Reproduce-gpu-Summary.json)と[前回報告](../2026-10-23-static-mesh-reuse/Report.md)を分けて保持する。したがって、+8.2%全体の原因を今回の変更だけで説明したとは扱わない。

実際に削減できたのは、同じ公開snapshotの材質同期が不要かを、複数のconsumer境界で繰り返す走査だった。画面外9,900体について`CompleteBindings`が`NeedsMaterialBindings`を再実行し、各回にカメラ・距離・視錐台などを再評価していた。RenderSystemにはReflectionProbe準備前、pipeline準備後、graph実行直前の境界がある。

入力が同じときは確認済みの「需要なし」を再利用する。判定入力68個の32bit値を完全比較し、hash衝突・構造体padding・視錐台ポインターの一致に依存しない。script/probe等の利用条件と資源owner/reset/frame・asset世代・Scene世代は毎境界で確認する。変更・snapshot差し替えでは再判定または従来の全補完へ進む。保持は一描画scope内だけで、別view・次frameへ持ち越さない。

最初の全件検証とgeometryの現在値照合は残る。全走査をなくすO(K)方式ではなく、同じフレーム内の重複判定を減らす修正である。

## 同条件の実描画比較

既存Release LTO、i7-14700F / RTX 4070。DX12 native renderer、1920×1080、Forward / Unlit、instancing・shadow・VSync無効。前後同じexe・同じshader・同じ計測ソースで、Engine DLLのみ今回のprivate実装を変更した。読み込んだDLLの絶対パスを各実行で確認した。[環境・実行物](Final-gpu-Environment.json)、[ソースとABIの監査](EvidenceAudit.json)。

前回と同じaffinity=240（4論理CPU）、High priority、HighQoS。A/B/B/A/A/Bの順で各3プロセス。1,000体は600 warmup、他は120 warmup、その後600標本/プロセス。CPU Profiler停止で、変更適用・WaitForCpuFrame・RenderSystem・EndFrame/Presentを含む経過時間を計測する。画像読戻しとログ出力は計測外。

P50は各プロセスP50の中央値、P95/P99は同条件3プロセスをまとめた標本の値。単位ms、負の変化は時間短縮。「前 / 後」を示す。

| 条件 | 前P50 | 後P50 | 変化 | P95 前 / 後 | P99 前 / 後 |
|---|---:|---:|---:|---:|---:|
| 1,000体・全件可視 | 1.0948 | 1.1006 | +0.5% | 1.2301 / 1.3591 | 1.3673 / 1.7879 |
| 10,000体・100体可視・無変更 | 8.2003 | 7.4015 | -9.7% | 12.4723 / 10.2808 | 15.0560 / 14.3558 |
| 10,000体・1%変更 | 8.2360 | 6.9130 | -16.1% | 11.4255 / 8.4283 | 15.0002 / 10.2081 |
| 10,000体・全件変更 | 9.3013 | 7.2303 | -22.3% | 11.8854 / 11.3335 | 15.3594 / 13.8412 |
| 10,000体・反対向き2ビュー | 21.9291 | 19.2559 | -12.2% | 32.7233 / 33.2594 | 39.5632 / 36.3497 |

1%変更ではP50・P95・P99が短縮した。一方、全件可視1,000体はP50が+0.5%、P99も1.3673 → 1.7879 msと後退し、2ビューのP95もわずかに後退した。全条件・全分位点の改善は示していない。

全試行で、全計測フレームの描画・cull・三角形件数と、1ビュー目の最終画像hashが一致した。単一ビューの1万体は100体可視。変更は先頭100体または全件のZを±0.125動かし、可視件数を一定にした。2ビュー目は320×180で反対側9,900体を描き、非背景画素と件数を確認した。2ビュー目の画像hashは計測していない。

[各試行・GPU passの集計](Final-gpu-Summary.json)に全分布を保存した。GPU pass時間は完了したsource/view/resource付きの標本であり、GPU全体時間やEditorの描画thread全体、StandaloneゲームFPSとは区別する。

## 別実行のCPU内訳

CPU Profiler有効、前後各30標本の別実行。主表の性能値へ混ぜない。完了したcaptureだけを使い、不完全なscopeを0として補わない。1%変更時の内訳を示す。

| 区間 | 前P50 | 後P50 |
|---|---:|---:|
| RenderSceneExtractor::GeometryObjects | 3.4440 | 3.2890 |
| RenderSceneExtractor::Materials | 1.9078 | 1.6676 |
| RenderSystem::ReflectionProbeCapture | 0.3930 | 0.3875 |
| RenderSystem::ExtractRenderScene | 6.9815 | 6.5485 |
| RenderPipeline::Execute | 0.4678 | 0.4404 |
| renderMs | 10.5868 | 9.0166 |
| endMs | 0.3861 | 0.3542 |
| frameMs | 11.1503 | 9.4940 |
| RenderSystem self（子scopeを除く） | 0.7967 | 0.0202 |

Profilerの内訳には実行間の変動も含まれる。geometry計算・材質同期・GPU workを削減したという意味にはしない。変更したコードはconsumer需要の再確認だけであり、最初のReflectionProbe境界の走査は残る。[全条件の内訳](Final-profile-Summary.json)。

## 採用しなかった案

同じ最適化前DLLを保存して、各案を単独で計測した。次の2案は利益を確認できず、ソースから外した。主表へ混在させていない。

| 案 | 1%変更の実描画P50の変化 | 1万体CPU抽出P50の変化 |
|---|---:|---:|
| RenderObjectを出力配列へ直接構築 | +3.7% | +1.2% |
| 個体ごとの追跡frame書込みをScene単位へ共有 | +1.2% | -0.9% |

CPU抽出は従来の1論理CPU固定、400標本/プロセス、120 warmup、各3プロセス。最終修正はgeometry抽出後のconsumer確認を対象とするため、これらの抽出単体ベンチの値を最終改善の証拠には流用しない。採用案の事前確認も[Memo集計](Memo-gpu-Summary.json)として最終5条件とは分離した。

## 正しさ・実行物

- Releaseの関連回帰132件が合格。現在のEngine DLLと同じフォルダーへ既存テストexeを置いた固定seedの並べ替え実行は89件が合格。GPUスキニングは58件が合格。

- 追加テストは同一アドレスの視錐台の書換え、layer距離変更、資源frame進行を公開snapshotの材質有効性で検証する。既存のnative setup hook追加テストも、複数の需要なし境界を通した後で実行する。

- 視錐台・layer距離・consumer認証・資源frameの照合を意図的に外すと、対応する4テストがすべて失敗した。ソースとLTO入力mapをbyte単位で復元し、同じ4テストが合格した。[負例](MutationTests.json)、[復元後](RestoredTests.json)、[復元証拠](MutationProof.json)。

- 最初の負例確認ではテスト出力先の旧コピーDLLを読んだため、負例として採用しなかった。その後は各検証の直前にリンク済みDLLを隔離フォルダーへ複製し、同じテストexeで前後を確認した。DLL・exeのhashも[負例の実行物](MutationTests-Identity.json)と[復元後の実行物](RestoredTests-Identity.json)へ保存した。

- native入力1,358件のうち今回の差はRenderSceneExtractor.cppだけ。他1,357入力と公開header・export名/ordinalは維持した。計測harnessのソースは変更していない。[実行物監査](FinalArtifactAudit.json)、[task差分](TaskDelta.patch)。

Engine、計測exe、CPU/GPUテスト、EditorLauncher、SandboxStandaloneを既存Release LTOツリーでビルドした。既存のMSB8028等の警告があり、VS環境の再取込みが一度失敗した際は別の新しいツールプロセスで検証を完了した。

確保回数・VRAM・GPU upload量の改善は測定していない。数学cacheやScene保存形式、C# SDK、描画順、previous worldの規則は今回変更していない。単一機の経過時間であり、前回の8.2%後退の全原因が特定できたという結論ではない。

[生標本manifest](RawManifest.json)に全実験のgzipと解凍後SHA-256を列挙した。変更前・各試行のDLL、全文ログ、再実行用の一時スクリプトはScratch/MeshOnePercent20261023に保持する。契約は[Runtime作業領域](../../design/runtime-workset-optimization.md)。
