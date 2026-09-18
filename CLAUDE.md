@AGENTS.md

- リファレンスは実装のコメントにも記載する。参考にした論文・公式仕様・公式ドキュメントの URL と題名・節名を、対応する数式・アルゴリズム・API 契約の直近に `/// @see <URL>` で残す。設計文書や作業報告だけへの記載で済ませない。

## Claude Code での作業

- まず検索で場所を特定し、読むのは関連範囲だけ。独立した調査は並行呼び出しでまとめる
- 局所修正は Write ではなく Edit。**圧縮された読み取り結果を `old_string` にしない** (非圧縮で読み直す)
- 主要な入口: コンポーネントは `Engine/Scene/Components/`、システムは `Engine/Scene/Systems/`、Inspector は `Editor/src/Panels/Inspector/`、エディター操作は `Editor/src/Op/`、ゲームスクリプトは `GreenWare/Assets/Scripts/`
- C++ を変えたら `Tools/AgentBuild.ps1 check <files>` で自分でコンパイルを通してから報告する。`cmake` / `msbuild` を直接叩かない。リンクや起動が要る確認だけユーザーに依頼する
- `AgentBuild.ps1` はサブエージェントや `run_in_background` で並べない。前の `RESULT` 行を読んでから次を出す。`RESULT busy` が出たら Monitor で `cl.exe` / `MSBuild.exe` / `cmake.exe` が消えるのを待つか、ユーザーに確かめる (`taskkill` しない)
- 編集のたびに PostToolUse フック (`Tools/AgentLint/lint.mjs --hook`) が規約違反を返す。ERROR は直してから先へ進む
- 手順の決まった作業は `.claude/skills/` (スクリプト追加・プロキシ追加・バス型追加・Playtest) を使う
- 公開 API を調べるときはヘッダーより先に `build/docs/api/<Module>/<Header>.md` (索引 `build/docs/api/index.md`) を読む。無ければ `doxygen Docs/Doxyfile` → `python Tools/ApiReference.py` で生成。コメント規約は `Docs/conventions/comments.md`
