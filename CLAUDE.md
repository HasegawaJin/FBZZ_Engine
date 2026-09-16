@AGENTS.md

## Claude Code での作業

- まず検索で場所を特定し、読むのは関連範囲だけ。独立した調査は並行呼び出しでまとめる
- 局所修正は Write ではなく Edit。**圧縮された読み取り結果を `old_string` にしない** (非圧縮で読み直す)
- 主要な入口: コンポーネントは `Engine/Scene/Components/`、システムは `Engine/Scene/Systems/`、Inspector は `Editor/src/Panels/Inspector/`、エディター操作は `Editor/src/Op/`、ゲームスクリプトは `GreenWare/Assets/Scripts/`
- ビルドは自分で叩かない。必要ならユーザーにタスク実行を依頼し、出力を受け取ってから直す
