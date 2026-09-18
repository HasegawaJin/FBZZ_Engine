# ベンチマーク

最適化の前後を `Tools/BenchCompare.ps1` で測った結果の一覧 (新しい順)。方法は [Docs/design/benchmark-report.md](../design/benchmark-report.md)。
このファイルは `Projects/DevTools/BenchReport` が作り直す。手で編集しない。

| 日付 | 題名 | 基準 → 候補 | 物理の場面 | Math の演算 | 改善 / 悪化 | CPU |
|---|---|---|---:|---:|---:|---|
| 2026-09-18 | [Math の小関数のインライン化](2026-09-18-math-inline/report.md) · [HTML](2026-09-18-math-inline/index.html) | `6403b867` → `e5cc1da3` | −44.5% | −42.0% | 9 / 0 | 13th Gen Intel(R) Core(TM) i7-13650HX |

`+` は未コミットの変更を含むビルド。変化は各項目の比の幾何平均。
