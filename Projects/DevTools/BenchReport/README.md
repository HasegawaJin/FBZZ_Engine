# BenchReport

`FBZZTestBench --measure` が出力する `fbzz-bench/1` と `fbzz-bench/2` の JSON から、比較レポートと一覧を作る。
`/2` は実行内 p95 と optional な確保数・再生成数を持つ。値が `null` の計測項目は利用できないことを示す。
ふだんは `Tools/BenchCompare.ps1` から呼ばれる。設計は [Docs/design/benchmark-report.md](../../../Docs/design/benchmark-report.md)。

```
node Projects/DevTools/BenchReport/src/cli.ts report <json|dir...> --out <dir> [--title <題名>] [--baseline <label>] [--candidate <label>]
node Projects/DevTools/BenchReport/src/cli.ts index Docs/benchmarks
node --test "Projects/DevTools/BenchReport/tests/*.test.ts"
```

| 出力 | 用途 |
|------|------|
| `index.html` | 1 ファイル完結の公開用ページ (ライト / ダーク両対応) |
| `report.md` | README・PR・コミット本文に貼る表 |
| `chart.svg` | 変化率の発散型横棒グラフ。README に `<img>` で貼っても OS のダークモードに追従する |
| `summary.json` | 一覧 (`index`) が読む要約 |
| `raw/*.json` | 元データ |

- Node 24 以上で `.ts` を直接実行する (型の除去だけで動く構文に限る。`tsconfig.json` の `erasableSyntaxOnly`)
- 実行時の依存は無い。型検査だけは `npm install` のあと `npm run typecheck`

| ファイル | 役割 |
|---------|------|
| `src/schema.ts` | JSON の型と検証 |
| `src/compare.ts` | 集計・変化率・判定 (しきい値はここが正本) |
| `src/markdown.ts` / `src/svg.ts` / `src/html.ts` | 各形式の描画 |
| `src/listing.ts` | `Docs/benchmarks/README.md` の一覧 |
| `src/cli.ts` | コマンドライン |
