# DevTools

エンジンが持つ開発用プログラム。単独で動き、役割と呼び出し元がはっきりしているものだけを置く。

- 一回きりのスクリプト (調査・検査・プレビュー・試行錯誤) は置かない。`Scratch/` (Git に入らない) を使う
- 追加はユーザーの承認を得てから。この索引に載っていないファイルは AgentLint の `tool-unlisted` が ERROR にする
- 置き場所の使い分けは AGENTS.md «ツールの置き場所»

## 索引

| 名前 | 役割 | 呼び出し元 |
|------|------|-----------|
| `AgentLint/` | AGENTS.md の絶対制約・記述規約・置き場所を機械で検査する (Node) | `.claude/settings.json` の PostToolUse フック、`.github/workflows/tests.yml` |
| `BenchReport/` | `--measure` の JSON から比較レポート (Markdown・SVG・HTML) と一覧を作る (TypeScript。Node 24 で直接実行) | `Tools/BenchCompare.ps1`、`Docs/design/benchmark-report.md` |
| `ApiReference/` | Doxygen の XML から AI 向けの API Markdown (`build/docs/api/`) を作る (Python) | `Docs/conventions/comments.md`、CLAUDE.md |
| `FontAtlasGen/` | TTF から BMFont 形式 (`.fnt` + PNG) のアトラスを作る (Python + Pillow) | `Assets/Fonts/` の `.fnt` の生成 |
