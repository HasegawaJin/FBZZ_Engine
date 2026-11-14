# ベンチマークの計測と比較レポート

最適化の前後を同じ条件で測り、ポートフォリオと PR にそのまま載せられる形 (Markdown・SVG・HTML) で残す。

## 1. 構成

```
測る (C++)                         比べる・見せる (TypeScript)              残す (Git)
FBZZTestBench --measure  ──JSON──▶ Projects/DevTools/BenchReport  ──▶ Docs/benchmarks/<日付>-<題名>/
  ├ scene: 物理の場面 (Simulate / Present)   ├ 集計・判定                        ├ index.html  (公開用)
  └ micro: Math の小さな演算                 ├ report.md / chart.svg            ├ report.md / chart.svg (README・PR 用)
                                            └ 一覧 (Docs/benchmarks/README.md)  └ raw/*.json  (元データ)
Tools/BenchCompare.ps1 … 基準のビルド (worktree) → 交互実行 → レポート生成 を 1 コマンドにする
```

- 見ながら調べる画面は TestBench (C++ + ImGui) のまま。Tracy / Unreal Insights と同じく、リアルタイムの道具はネイティブに置く
- 計測は本体の言語、比較と公開は別の道具、という分け方は Unreal (CsvProfiler → PerfReportTool)、Unity (Performance Testing → Benchmark Reporter)、Godot (godot-benchmarks → 公開サイト) と同じ
- BenchReport は Node 24 の型除去実行で `.ts` をそのまま動かす。実行時の依存は持たない (公開リポジトリで長く動かすため)

## 2. 計測 (`FBZZTestBench --measure`)

```
FBZZTestBench.exe --measure [--steps 600] [--warmup 120] [--repeats 7] [--ops 200000]
                            [--json <path>] [--label <text>] [--commit <hash>] [--dirty]
```

| 種類 | 対象 | 単位 | 1 回の計時 |
|------|------|------|-----------|
| `scene` | TestBench の各場面の `Simulate` と `Present` | us/step | Reset → warmup → steps 回 |
| `micro` | Math の演算 (行列積・回転・視錐台判定・正規化) | ns/op | 1024 要素の入力を ops 回ぶん回す |
| `water` | Water TargetPoint workset の Transform 同期・参照解決・mesh/BVH・Zone 生成 | us/iteration | 10 / 100 / 1,000 / 10,000 点。owner あたり最大 256 点 |

- 繰り返し (`repeats`) のたびに `Reset` し、どの回も同じ軌道の同じ区間を測る。代表値は中央値
- 計時中はスレッドを 1 コアへ固定し、優先度を上げる
- `micro` の入力は固定の線形合同法で作る (乱数ライブラリも時刻も使わない)。結果は checksum に畳み、最適化で消されないよう volatile へ書く
- `water` は scene/TargetPoint/Zone の構築を計時前に終え、同期・数値解決・mesh/BVH 生成・Zone 生成を別々に測る。LIVE の静止 / 1 点移動 / 全点移動と BAKED_STATIC の静止 / 移動を含む
- `water` は GPU / UI を使わず、asset load も行わない。標準 allocator は Engine MemoryTracker の対象外なので確保回数は `null`、再生成数は builder 実行数から記録する

### 水面と DX12 の個別測定

`Tools/AgentBuild.ps1 build FBZZAlgorithmOptimizationBench -Preset release` でビルドする。

```text
FBZZAlgorithmOptimizationBench.exe <family> <scenario> <before|after> <count> <warmup> <samples> <json> <build-label>
```

| family | scenario | 回数に含める処理 |
|---|---|---|
| `water-spline` | `river` / `lake`、`*-edit`、`river-steady` | 本番の形状生成。編集は計時外で制御点を交互に動かす。定常は構築済み形状の再利用判定 |
| `water-cache` | `independent` / `zone-steady` / `zone-change` / `horizon-links` | CPU バックエンドで水面の抽出を一巡。Zone 有効化後の抽出は資源返却と再生成を含み、無効状態への復帰は計時外 |
| `water-medium` | `unique` / `duplicates` | 候補の照合とビュー外判定、または所有キー重複の一括除外。GPU の媒体アトラス生成を含まない |
| `dx12-common` | `registered` / `half-pending` | `QueueTransitionAllToCommon` の CPU 時間。資源生成、保留遷移の準備、コマンドへのバリア記録は計時外。WARP を使い GPU へ提出しない |

各 JSON の `workers[].totalUs` は操作あたりの µs。複数操作をまとめて測る水面形状では `operationsPerSample` にその回数を記録する。同じ実行ファイルから比較する Release DLL を切り替えるときは、`runtimeModules` が指定の DLL を指すことを確認し、同条件の出力 checksum と標本数も照合する。

### 植生・地形・影の個別測定

同じ `FBZZAlgorithmOptimizationBench` を使う。Kestrel Basin の描画機能を固定入力で個別に呼び出し、`static` は固定視点、`moving` は 24 フレーム周期の水平往復にする。CPU バックエンドで本番の抽出・可視判定・描画提出を測る。GPU の実行時間、シーン読込、フレーム全体の時間は計測範囲の外。

| family | count | 計時と照合の対象 |
|---|---|---|
| `vegetation` | 1 セット内の個体数 | 樹木などの色パスの CPU 選別・LOD と提出。候補数・描画数・三角形数を照合 |
| `grass` | 1,024 本入りのページ数 | 常駐した草の色パスの可視判定・LOD と提出。描画数・三角形数を照合 |
| `terrain-extract` | 地形グリッドのタイル数 | パッチの抽出。 `edit-static` / `edit-moving` は全タイルの高さ変更後の抽出。出力の高さとインデックス数を照合 |
| `terrain` | 地形パッチ数 | 色パスの可視判定・LOD と提出。候補数・描画数・三角形数を照合 |
| `terrain-occlusion` | 地形パッチ数 | 透視投影で生成する CPU 遮蔽深度。全セルの深度を照合 |
| `terrain-shadow` | 地形パッチ数 | 4 カスケードの地形の影。描画数・三角形数を照合 |
| `shadow` | 配置セット数 | 4 カスケードの配置物の影。通常は 1 セット 64 個体。 `replace-static` / `replace-moving` は 1 セット 8 個体で、同じ資源フレーム内の最初のスナップショットを描いてから差し替えを計時。描画数・三角形数を照合 |
| `grass-ray` | 草のオーナー数 | `resident` は実験用レイトレーシング診断で 1 オーナー当たり 4 面を照合。診断されたオーナーと順序を確認 |

### JSON (`schema: "fbzz-bench/2"`)

```json
{
  "schema": "fbzz-bench/2",
  "label": "candidate",
  "startedAt": "2026-09-18T15:20:00+09:00",
  "environment": { "commit": "28bd01a2", "dirty": false, "cpu": "…", "logicalCores": 16,
                   "compiler": "MSVC 19.51.36231", "config": "Development" },
  "settings": { "steps": 600, "warmup": 120, "repeats": 7, "ops": 200000, "fixedStep": 0.0166667 },
  "waterCoverage": { "gpu": "not measured", "ui": "not measured", "standardAllocatorCounts": "unavailable" },
  "results": [
    { "kind": "water", "name": "TargetPoint / 1000 / live-move-one", "phase": "resolve-signature", "unit": "us/iteration",
      "samples": [7.1, 7.3], "median": 7.2, "p95": 7.29, "min": 7.1, "max": 7.3,
      "allocationsPerIteration": null, "regenerationSamples": [0, 0], "regenerationsPerIteration": 0,
      "note": "CPU-only; GPU/UI not measured; STL allocation counts unavailable." }
  ]
}
```

- 1 回の実行 = 1 ファイル。`commit` / `dirty` は exe が知らないので呼び出し側 (`BenchCompare.ps1`) が渡す
- `p95` は 1 実行内の sample を線形補間する。BenchReport は `fbzz-bench/1` と `/2` を読み、比較の代表値は中央値を保つ
- 形を変えるときは `schema` の番号を上げ、BenchReport が古い番号を読めるようにする

## 3. 比較 (`BenchReport`)

- 同じラベルの実行が複数あるとき、各実行の中央値の中央値を代表値にする
- 変化率 = (候補 − 基準) / 基準

| 判定 | 条件 | 表示 |
|------|------|------|
| 改善 | 変化率 ≤ −3% | 緑 |
| 変化なし | −3% < 変化率 < +3% | 灰 |
| 悪化 (注意) | +3% ≤ 変化率 < +5% | 橙 |
| 悪化 | 変化率 ≥ +5% | 赤 |

- しきい値 3% は、実測した実行ごとのばらつき (約 2%) を上回るように決めた
- 基準・候補とも 0.05 未満の項目 (その場面では何もしていない) は表から外す
- ばらつき = 各実行の中央値の (最大 − 最小) / 代表値。表に出し、変化率がばらつきより小さいものは «ノイズ内» と注記する

## 4. 比較の手順 (`Tools/BenchCompare.ps1`)

```
Tools/BenchCompare.ps1 -Baseline <ref> [-Candidate <ref>] [-Rounds 3] [-Title <題名>] [-Publish <名前>]
```

1. 基準 (と `-Candidate` を指定したときは候補) のコミットを `build/bench/src/<hash>/` に worktree で取り出し、その中の `AgentBuild.ps1` で `FBZZTestBench` をビルドする。出力を `build/bench/bin/<hash>/` にキャッシュして worktree を消す。2 回目以降はビルドしない
2. `-Candidate` を省略したときは作業ツリーをビルドし、`build/bench/bin/working/` に置く (毎回ビルドし直す)
3. 基準 → 候補 → 基準 → … と `Rounds` 回ずつ交互に実行し、`build/bench/runs/<時刻>/` に JSON を書く。マシン状態の変化による偏りを打ち消すため
4. BenchReport でレポートを作る。既定の出力先は `build/bench/reports/<時刻>/`。`-Publish <名前>` を付けると `Docs/benchmarks/<日付>-<名前>/` に書き、一覧 `Docs/benchmarks/README.md` を作り直す

- 基準にできるのは `--measure` の JSON 出力を持つコミット以降 (このブランチの計測モード追加以降)
- worktree のビルドは configure から行うため、初回は時間がかかる (キャッシュ後は実行だけ)

## 5. 保存の規約

- `Docs/benchmarks/` には採用する結果だけをコミットする。途中の実行は `build/bench/` に置き、Git に入れない
- 各レポートは `index.html` (1 ファイルで完結)・`report.md`・`chart.svg`・`raw/*.json` を持つ。README や PR には `report.md` の表と `chart.svg` を貼る
- 同じ CPU・同じ構成で測ったものだけを 1 枚の表に並べる。測定条件はレポートの冒頭に必ず出す

## 6. やらないこと (今は)

- CI での自動計測: 共有ランナーは時間のばらつきが大きく、しきい値判定が安定しない
- GitHub Pages でのサイト公開: 結果が数件溜まってから
- Google Benchmark の導入: `micro` で足りなくなってから (入れるなら `ThirdParty/` へベンダー)
