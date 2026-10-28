<!-- @file    report.md -->
<!-- @brief   GPU計測ビューの同期を順序付き記録へ置き換えた際の正しさと性能比較。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# GPU計測ビューの描画スレッド同期削減

実装は2026-10-22、最終計測は2026-10-23。RenderPipelineとスキニング準備で行っていたGPU計測開始の同期を、描画命令と同じ列への記録へ変更した。2ビューの実GPU確認では、計測開始の同期が1フレーム2回から0回、CPU命令packetが3個から1個になった。native描画命令数と画像は同じだった。

実GPUの2ビューではCPU描画完了までの時間が初回25.1%、長時間確認18.9%短縮した。8ビューは初回34.5%短縮。単一ビューは初回と長時間確認で増減の方向が異なり、安定した短縮とは判断しない。GPUを使わない軽いfake backendの2ビューでは、メインスレッドの記録は短くなる一方、CPU描画完了までの時間は長時間確認で5.1µs増えた。一律の速度向上やゲーム全体のFPS向上を示す結果ではない。

## 変更内容

- `GpuProfRecordView` / `GpuProfEndRecordedView`を追加し、RenderPipelineとEngine側のSkinningComputePassを接続した。
- 受付とnative開始結果を別の契約にし、metadata・パス名を所有済み入力として渡す。native開始結果はworkerだけが持つ。
- 開始失敗時はその区間の計測と終了だけを抑止し、描画を実行する。失敗した入れ子を抜けると親の計測へ復帰する。
- 必要な同期によるpacket分割をまたいで計測状態を保持する。終了だけが取消された場合は次の物理フレーム境界またはShutdownで閉じる。
- 従来の同期開始API、TryDispatchの実記録receipt、資源生成・破棄・読み戻し、GPU fenceは従来の境界を使う。

詳細は[Render Thread設計](../../design/render-thread.md#gpu計測ビューの順序付き記録)。[Godotの描画スレッドへのcallable投入](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class-renderingserver-method-call-on-render-thread)を参考に、描画側で完結できる処理を順序付きで渡す方式を採用した。参照URLは対応する実装と公開契約にも記載した。

## 比較条件

| 項目 | 条件 |
|---|---|
| CPU / GPU | Intel Core i7-14700F / GeForce RTX 4070 |
| ドライバー | 32.0.16.1088 |
| ビルド | 同じReleaseツリー、MSVC 14.51、前後ともLTO有効 |
| 実行 | 同一ベンチexe、前後のDLLを別フォルダーへ固定、affinity `0xF0`、High優先度 |
| 順序 | A→B→B→A→A→B。各条件・各方式で別プロセス3回 |
| 初回 | 各プロセス120フレームwarmup後、CPU 3,000 / GPU 600標本 |
| 長時間確認 | CPU 2ビュー20,000 / GPU 1・2ビュー5,000標本、各方式3プロセス |
| 描画 | hidden DX12 window、VSync無効、32×32 offscreen、1ビュー16パスのClear |
| 正しさ | 全画素の既知色（絶対誤差0.01未満）、完了済み計測のパス数・名前・view ID、fakeの実行件数 |
| 診断 | 時間計測ではCPU trace無効。同期・packet・命令数は別実行で4フレーム取得 |

変更前は作業開始時に保存した実DLLを使い、ベンチから既存の同期APIだけを呼んだ。変更後は最終LTOビルドのDLLで新しいAPIを呼んだ。実際にロードしたGraphics DLLの絶対パスを各結果へ保存し、想定のパスと照合した。両方のexeのSHA-256は同じ。全native入力1,358件をLTO入力台帳と照合し、不一致は0件だった。

通常ビルドでGraphicsとEngineの実exportを取得してからLTOへ戻し、既存symbolのordinalを維持した。比較は全ビルド完了後に実施した。Editor/Standaloneも再ビルド済み。保存フォルダーのEngineなどの同梱DLLも環境記録に含むが、この性能ベンチはGraphics/Core/Mathに依存し、Engine DLLはロードしない。

集計値は「各プロセスのフレーム時間中央値」の中央値。`recordMs`はBeginCpuFrameからEndFrame直前まで、`completeMs`はそこからEndFrameとWaitForCpuFrameまで含むwall時間。後者にもGPU全体の完了を要求する読み戻しは含めない。全画素の読み戻しは標本取得後に行う。GPU timestampの合計やMainとRenderの時間の和をフレーム時間として扱わない。

## 初回比較

単位ms。負の差分は短縮。GPU表は実DX12 backendのCPU描画完了までの時間であり、GPU演算時間やゲーム全体のフレーム時間ではない。

| Backend / ビュー数 | 記録・変更前 | 記録・変更後 | CPU描画完了・変更前 | CPU描画完了・変更後 | 完了までの差分 |
|---|---:|---:|---:|---:|---:|
| CPU fake / 0 | 0.0002 | 0.0002 | 0.0014 | 0.0016 | +14.3% / +0.2µs |
| CPU fake / 1 | 0.0048 | 0.0028 | 0.0065 | 0.0049 | −24.6% |
| CPU fake / 2 | 0.0092 | 0.0057 | 0.0112 | 0.0143 | +27.7% / +3.1µs |
| CPU fake / 8 | 0.0377 | 0.0205 | 0.0398 | 0.0353 | −11.3% |
| DX12 / 0 | 0.0004 | 0.0004 | 0.12305 | 0.12035 | −2.2% |
| DX12 / 1 | 0.0278 | 0.00275 | 0.1486 | 0.1793 | +20.7% |
| DX12 / 2 | 0.0596 | 0.0047 | 0.19865 | 0.14875 | −25.1% |
| DX12 / 8 | 0.2303 | 0.0159 | 0.37570 | 0.24625 | −34.5% |

全標本、各プロセスの中央値・P95・P99は[summary.json](summary.json)。0ビューは対象の同期が元からない対照条件であり、この差を同期削減の効果とは解釈しない。

## 悪化条件の長時間確認

初回で悪化したCPU fake 2ビューとDX12 1ビューを長く測定し、DX12 2ビューも対照として再確認した。初回結果を差し替えず、両方を残す。

| 条件 | 記録・変更前→変更後 | CPU描画完了・変更前→変更後 | 完了までの差分 |
|---|---:|---:|---:|
| CPU fake / 2 | 0.0094 → 0.0058 | 0.0112 → 0.0163 | +45.5% / +5.1µs |
| DX12 / 1 | 0.0273 → 0.0026 | 0.1585 → 0.1336 | −15.7% |
| DX12 / 2 | 0.0531 → 0.0042 | 0.1881 → 0.1525 | −18.9% |

詳細は[confirmation.json](confirmation.json)。CPU fakeの2ビューの悪化は再現した。命令を一括提出するためproducerとworkerの重なり方も変わることが原因の候補だが、今回の計測では原因までは切り分けていない。DX12の単一ビューは初回+20.7%から長時間確認−15.7%へ変わり、一定の改善率を主張できない。2ビューは両実行で短縮した。大きな実シーン、GPU負荷が高い場面、ゲーム更新との重なりを含むFPSは未計測。

## 同期と実行量

2ビュー、4フレームの別trace。CPU・DX12ともcapture incompleteとdropは0。

| 指標 | 変更前 | 変更後 |
|---|---:|---:|
| 計測開始の同期API | 8回 | 0回 |
| packet | 12個 | 4個 |
| CPU frame wait API | 4回 | 4回 |
| 命令数・CPU fake | 408個 | 408個 |
| 命令数・DX12 | 416個 | 416個 |

命令の省略やframe完了境界の削除による短縮ではない。DX12の全実行で画素checksumは2560、全4,096値が既知のRGBAに絶対誤差0.01未満で一致し、viewごとに16パスが完了snapshotへ記録された。

## 正しさとビルド

| 検証 | 結果 |
|---|---|
| 変更C++のAgentBuild check | 合格 |
| Release LTO build | Graphics / Engine / Editor / Standalone / 対象テスト・ベンチ 合格 |
| 描画系CPU CTest | 303 / 303合格 |
| 実GPU描画・計測・ImGui | 6 / 6合格 |
| Skinning・変形・寿命・再入 | 58 / 58合格 |
| 追加CPU回帰 | metadata・パス名の所有、開始拒否、入れ子、同期分割、取消し、旧API、処理中の次ビュー記録、RenderPipeline接続を含む9件 |
| 故障注入 | 失敗区間のBeginPass抑止を外して2件失敗。元のbyte列へ復元し、9件が合格 |
| AgentLint | 対象9ファイル、error 0 / warning 0 |
| LTO source ledger | 1,358入力一致、未ビルド変更0 |

ビルドには既存の共有中間ディレクトリー`MSB8028`、LTO/PCHの`C4728`警告が残る。途中のVS環境初期化失敗は一度再実行して解消した。GPUテストの通常stderrログをPowerShellの外側が例外として扱ったため集約処理を再開したが、当該GPUテスト6件自体は保存済みJSONで全件合格していた。合否とログ出力を区別して確認した。

## 成果物と再現

- [測定環境・実行物のhash](environment.json)、[長時間確認の環境](environment-confirmation.json)
- [検証の集約](validation.json)、[ソースのhash](sources.json)
- 同じフォルダーの`cpu-*.json` / `gpu-*.json`は初回の生標本、`confirm-*.json`は長時間確認、`trace-*.csv`は別取得した診断。
- 実装・通常export・LTO復帰・故障注入・実行物保存の作業記録は`Scratch/RenderSynchronization20261022/`。恒久的な開発ツールとしては追加していない。

ベンチは`Projects/Tests/Bench/RenderSynchronizationMeasure.cpp`、対象は`FBZZRenderSynchronizationBench`。引数は`<legacy|queued> <cpu|gpu> <ビュー数> <標本数> <出力JSON>`。`legacy`は同期API、`queued`は順序付き記録APIを使う。同じ入力・同じ完了境界・別プロセスを使い、旧版DLLへ`queued`を要求しないこと。過去の実行物を保存せず現在のDLLだけでlegacyを走らせた値は、この報告の変更前実DLLの値とは分けて扱う。
