<!-- @file    Report.md -->
<!-- @brief   ETWで特定したForward共通資源検索の削減と同条件の実描画比較。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# 全件可視1,000体：トレース分析と共通バインドの再利用

全件可視1,000体はP50 **1.0292 → 0.7851 ms（-23.7%）**、P99 **1.4763 → 1.0526 ms（-28.7%）**。独立した再測定も下表に残す。今回のForward共通バインド再利用を採用した。

## 原因と変更

`ForwardDraw` は各Meshで影・Cookie・IBL・画面空間資源を同じ条件で解決していた。`RenderResourceRegistry::Target(std::string_view)` は `find(std::string(name))` を使うため、`PunctualShadowMap`（17文字）と `LightCookieAtlas`（16文字）は使用中のMSVC STLの短い文字列領域（charで15文字）に収まらない。計測区間のスタックでもRegistry検索とヒープ処理がForward描画の下に現れた。1,000 Drawではソース上、この2名だけで最大2,000回/フレームの一時文字列構築・ヒープ確保/解放に達する。確保回数を直接計測した値ではない。[MSVC STL実装](https://github.com/microsoft/STL/blob/main/stl/inc/xstring)、[実装差分](TaskDelta.patch)、[トレース根拠](TraceEvidence.json)。

共通資源は最初の有効なDrawで一度解決し、通常の不透明Mesh群内で再利用する。Instance / Fiberの実行後、半透明群の最初の有効Drawで再解決する。材質、物体定数、スキニング、各DrawのGPU状態遷移・実リソース解決は維持する。保持範囲はその関数呼び出し内だけで、次のパス・ビュー・フレームへ持ち越さない。個別の公開 `ForwardDraw` 呼び出しも従来どおり都度解決する。公開APIや構造体レイアウトは変えていない。

変更した本番入力はprivateの `ForwardPasses.cpp` / `RenderSceneDraw.cpp` / `RenderScenePassHelpers.hpp` の3件。前回棄却したDX12 b14転送キャッシュは追加しない。物理・抽出・材質需要再利用の既存最適化をbefore側にも含む。[入力監査](EvidenceAudit.json)、[設計](../../design/graphics-library.md)。

## トレースの取得と切り分け

以前の管理者実行では指定ETLが生成されていなかったため、この調査では名前付きWPRセッションを管理者で再取得した。`TraceEvidence.json` にETLの絶対パス・サイズ・SHA256・開始終了状態を記録した。原本はScratchに保持し、約2GBのETLをリポジトリへ複製しない。

CPUサンプリング、StackWalk、CSwitch、ReadyThread、ImageLoadを解析した。17,237,383イベント、イベント欠落0、バッファ欠落0。QPC周波数10,000,000 Hz。共通の計測exeにフレームの絶対開始終了時刻とQPC校正値を追加し、対象PID/TIDの600計測フレーム×3実行に限定して照合した。時刻出力には既存の開始終了時計値を使い、フレーム内の時計呼び出しは増やさない。

Release LTOのローカルPE exportとunwind関数開始位置が一致した関数だけ名前解決し、未解決はmodule+RVA、ICF同一番地の別名は明示した。PDBの行番号まで解決した結果ではない。Registry::Targetを含むサンプルは3実行でそれぞれ123 / 149 / 127件。サンプル数は時間比率や厳密な呼び出し回数ではない。[解決済みサンプル](ResolvedFrameSamples.json.gz)、[スケジューラ集計](ScheduleSummary.json.gz)。

遅いフレームの多くは対象スレッドにCPUが割り当てられた区間が大半を占め、ready待ちだけでは説明できない。一方、12.5599msのフレームではblockedが8.9078msあり、EndFrame配下のドライバー/カーネル側の長い区間も残る。scheduledにはカーネル処理等も含まれる。全スパイクが文字列検索に起因する、前回P99後退の唯一の原因を証明した、OS/ドライバー由来の揺れを解消した、とは結論しない。WPR中の中央値は約1.80 / 3.07 / 1.90msと観測負荷があるため、以下の性能比較に混ぜない。

## 同条件の実描画比較

既存Release LTO、i7-14700F / RTX4070、DX12 native、1920×1080、Forward / Unlit、instancing・shadow・VSync無効。affinity=240（4論理CPU）、High priority、HighQoS検証済み。A/B/B/A/A/Bで各3プロセス。全件可視は600 warmup、他は120 warmup、その後各600標本。前後で同じexe・shaderを使い、DLLだけ隔離する。CodSpeedの代わりにFBZZ既存環境を使うことはユーザー承認済み。

Profiler停止で変更適用・WaitForCpuFrame・RenderSystem・EndFrame/Presentを含む経過時間を測る。読戻しとログ出力は計測外。P50は3プロセスのP50の中央値、P95/P99は各1,800標本の分位点。単位ms、前 / 後、負の変化は時間短縮。[環境](Final-gpu-Environment.json)、[集計](Final-gpu-Summary.json)。

| 条件 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 10,000体・全件変更 | 6.4645 / 6.4822 | +0.3% | 8.2893 / 6.9525 | 9.8169 / 7.3626 | -25.0% |
| 1,000体・全件可視 | 1.0292 / 0.7851 | -23.7% | 1.3081 / 0.9164 | 1.4763 / 1.0526 | -28.7% |
| 10,000体・100体可視・無変更 | 6.4133 / 6.1099 | -4.7% | 8.1018 / 6.6244 | 9.5767 / 6.9796 | -27.1% |
| 10,000体・1%変更 | 6.2290 / 6.1956 | -0.5% | 8.0698 / 7.1624 | 8.9650 / 8.2039 | -8.5% |
| 10,000体・反対向き2ビュー | 15.6812 / 13.6836 | -12.7% | 17.0016 / 14.8412 | 18.3438 / 17.5794 | -4.2% |

全件可視のForwardOpaque CPU時間P50は **0.4769 → 0.2763 ms（-42.1%）**。フレーム全体に加えて変更箇所の描画投入費用も短縮した。全計測フレームで描画件数・cull件数・三角形件数と最終画像hashがbefore/after一致した。1,000体は個別Drawを維持する。2ビュー目は320×180で反対側9,900体を描き、件数と非背景画素を検証するが、2ビュー目の画像hashは取得していない。

## 独立した再測定と試行間の分布

主比較後に全件可視を同じ600 warmup・600標本・各3プロセスで再実行した。初回の好結果だけを採用根拠にしない。[再測定](Confirm-gpu-Summary.json)。

| 実行 | P50 前 / 後 | P50変化 | P95 前 / 後 | P99 前 / 後 | P99変化 |
|---|---:|---:|---:|---:|---:|
| 独立した再測定 | 1.0074 / 0.8224 | -18.4% | 1.1741 / 0.9401 | 1.3844 / 1.0921 | -21.1% |

| 実行 | 版 | 各試行のP50 (ms) | 各試行のP99 (ms) |
|---|---|---|---|
| First | Baseline | 0.9827, 0.9910, 1.0290 | 1.2261, 1.1695, 1.2495 |
| First | Candidate | 0.7790, 0.7792, 0.8240 | 0.9902, 0.9299, 1.0487 |
| Final | Baseline | 0.9884, 1.0292, 1.0412 | 1.2149, 1.3766, 1.6646 |
| Final | Candidate | 0.7790, 0.7851, 0.8223 | 0.9549, 1.0619, 1.0701 |
| Confirm | Baseline | 1.0610, 1.0074, 0.9843 | 1.4376, 1.3724, 1.2193 |
| Confirm | Candidate | 0.8224, 0.7933, 0.8330 | 1.0984, 1.0055, 1.1044 |

Firstは最初の比較、Finalは全5条件の正式比較、Confirmはその後の独立再測定。First以降に本番処理の意味は変えず、参照コメントを追加して再ビルドした。Final / Confirmは最終DLLに一致することを検証済み。[試行別値](VisibleTrialDistributions.json)。P99には試行・OS・ドライバーの変動が含まれ、観測した短縮率の全量を厳密な因果推定値とはしない。

## 正しさと実行物の検証

- AgentBuild経由で変更C++のコンパイル検査とReleaseビルドを完了。
- Graphics CPU **304件合格**。新規回帰テストは同じcontextとresource frameで3ビューを切り替え、静的/スキンド/半透明の各Draw、材質、影、Cookie、IBL、AO、接触影をSubmit境界で検査する。IBL無効時も検査する。
- 影texture28を意図的に消す変異で新規テストが1件失敗。ソース・LTO入力mapをbyte単位で戻し、同一テストexeで304件が再合格。[失敗](Negative.json)、[復元後](Restored.json)、[テスト差分](RetainedTestChanges.patch)。
- native Graphicsは**52件合格、既存の1件無効**。スキニング**58件合格**。[native結果](NativeGraphics.json)、[skinning結果](Skinning.json)。
- 全1,358 native入力の鮮度、6 DLLの全export名・ordinal、before DLLの保存版一致、before/after共通exe、Final/Confirmの候補DLLと最終ビルドの一致を検証。[実行物監査](FinalArtifactAudit.json)、[再測定監査](Confirm-ArtifactAudit.json)。
- 30標本/条件のProfiler付き別実行も保存。完全記録でないrunのscope値は集計から除外し、欠落scopeを0とみなさない。正式タイミングと混ぜない。[Profiler集計](Final-profile-Summary.json)。

## 保存物と再実行

`raw/*.jsonl.gz` は各実行の `REGULAR_MESH_JSON` 原文だけを抽出し、mtime=0で圧縮した。原文・圧縮後SHA256と出典を [RawManifest.json](RawManifest.json) に記録する。トレース解釈と計測・監査スクリプトも `raw/` に圧縮保存した。Scratchの作業一式は `K:\FBZZ_Engine\Scratch\VisibleMeshTrace20261023`。

再実行はScratchの `Measure.ps1 -Label <新しい名前> -AllCases` と `Analyze.py <名前> gpu`。対象だけなら `-AllCases` を省略する。固定Labelの再利用は既存の隔離DLLを使うため、新たなビルドを比較するときは新Labelを使う。ETW再取得は `LaunchCapture.ps1` の管理者実行を要するため、既存の固定出力を上書きせず別出力先・別セッション名へ変更する。
