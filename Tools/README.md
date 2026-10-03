# Tools

開発の入口になる薄いスクリプトだけを置く。VS Code タスク・CI・スキル・AGENTS.md から呼ばれるもの。

- 独立したプログラム (テストや README を持つもの) は `Projects/DevTools/` へ
- ゲーム固有のアセット制作は `<Project>/Tools/` (例: `GreenWare/Tools/`) へ
- 一回きりのスクリプトは置かない。`Scratch/` (Git に入らない) を使う
- 追加はユーザーの承認を得てから。この索引に載っていないファイルは AgentLint の `tool-unlisted` が ERROR にする

## 索引

| 名前 | 役割 | 呼び出し元 |
|------|------|-----------|
| `AgentBuild.ps1` | AI の検証ループ (compile / build / test) と構成済み SDK consumer の build の入口 | AGENTS.md、`.claude/skills/`、`.claude/settings.json` |
| `VsEnvironment.ps1` | Visual Studio の開発環境を解決する唯一の場所 | `AgentBuild.ps1`、`VcBuild.ps1` |
| `VcBuild.ps1` | 人が使う VS Code タスクのビルド入口。全体ビルド後に同構成の SDK を公開 | `.vscode/tasks.json` |
| `BenchCompare.ps1` | 基準と候補の TestBench をビルドして交互に計測し、比較レポートを作る | `Docs/design/benchmark-report.md` |
| `Coverage/` | カバレッジの計測と要約 (C0: OpenCppCoverage / C1・C2: clang-cl + llvm-cov) | `.vscode/tasks.json`、`.github/workflows/tests.yml`、`Docs/conventions/test.md` |
