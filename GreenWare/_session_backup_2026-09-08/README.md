# 2026-09-08 セッションの退避物

作業前のファイルをここへ寄せてある。**差し戻したいときはここから戻す。**
`Assets/` の外に置いてあるのは、`.hpp.bak_*` のような名前でも
ScriptCodeGen やアセットインポータの目に触れさせないため。

| フォルダ | 中身 |
|---|---|
| `Scripts/` | スクリプト 10 本の変更前（当たり判定の遅延構築・登攀・脚破壊→転倒・SE 配線） |
| `Scenes/` | `Stage_01.scene` の 2 世代（切り分け用に 2 本無効化した版 / マスク GUID 修正前） |
| `Animation/` | マスク 3 種の `skeleton_source` 修正前、`Boss.animcontroller` の Hatch レイヤー入り版 |
| 直下の `*.md` | **中身は編集«後»。取り違えて上書きしてしまった** ─ ドキュメントは git 管理下なので `git diff` / `git checkout` で戻せる。こちらは見ないこと |

当時別置きしていた音の旧版22本は2026-09-14に整理済み。旧音源はgit履歴を参照。

## 戻し方

このリポジトリは git 管理下なので、**まずは git を見るのが早い**。

    git diff --stat                      今日の変更の全体像
    git diff GreenWare/Assets/Docs/      ドキュメントの差分だけ
    git checkout -- <path>               1 ファイル戻す

ここのフォルダが要るのは、**git に入っていないもの**（`Library/Baked/` の生成物、
`.playmode_snapshot.scene`）と、**1 コミットの中で 2 世代作った Stage_01.scene** だけ。
