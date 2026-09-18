---
name: playtest
description: FBZZ Engine / GreenWare の挙動や見た目の変化を Playtest シナリオ (*.playtest.json) で機械判定する手順。ゲームの挙動を変えた・描画を変えた・「動くか確かめて」「回帰してないか」「基準画像を更新して」と言われたとき、入力の記録/再生、絵の比較、--batch 実行に使う。
---

# Playtest シナリオ

設計: `Docs/design/ai-verification-loop.md`。手順の語彙は Command Bus そのもの (MCP で読める値は全部表明できる)。

## 置き場所

| 物 | 場所 |
|---|---|
| シナリオ | `<Project>/Tests/Playtests/<Name>.playtest.json` |
| 入力の記録 | `<Project>/Tests/Playtests/Recordings/*.inputrec.json` |
| 基準画像 | `<Project>/Tests/Golden/<baseline>.png` |
| レポート・実画像・差分画像 | `<Project>/Library/Playtests/<Name>/` (追跡しない) |

見本: `GreenWare/Tests/Playtests/TitleSmoke.playtest.json`

## 形式

```json
{ "name": "X", "scene": "Assets/Scenes/Stage_01.scene", "lockstep": 0.016666667,
  "steps": [ { "do": "play" }, { "do": "frames", "count": 60 }, { "do": "stop" } ] }
```

| do | 引数 | 意味 |
|---|---|---|
| `play` / `stop` / `pause` / `resume` | — | play.control。stop は復元に 2 フレーム置く |
| `frames` | `count` | 進める |
| `input` | `inject` (input.inject の中身) | 注入。押したら離す手順も書く |
| `bus` | `request`, `kind?`, `expectError?` | 任意の要求 |
| `assert` | `query`, `path`, `op`, `value` | 今すぐ成立しなければ失敗 |
| `waitUntil` | 上 + `timeoutFrames` (既定 600) | 成立まで毎フレーム評価 |
| `replay` | `file` (シナリオからの相対) | 記録した入力を同じフレーム番号で注入 |
| `capture` | `name`, `view?` | 撮るだけ |
| `compareImage` | `baseline`, `view?`, `maxMeanDiff?` (0.01), `maxBadPixelRatio?` (0.005), `pixelThreshold?` (0.1) | 基準画像と比べる |
| `log` | `message` | 記録だけ |

`op`: `exists` `missing` `==` `!=` `<` `<=` `>` `>=` `contains` `length==` `length>=` `length<=`。`path` は `nodes.0.name` のようなドット区切り。

## 回し方

- エディター起動中: MCP `scenario_run {path}` → `scenario_status` を state が running でなくなるまで。失敗なら `failure` と `steps` を読む
- エディター無し: `FBZZEditor.exe --project <GreenWare> --batch <scenario> --hidden --report <json>`。終了コード 0/1/2
- GPU の無い環境: `--warp --skip-images` (WARP の絵は GPU と一致しない)

## 絵の比較

1. 初回は基準画像が無いので失敗する。`scenario_run {path, updateBaselines:true}` で作る
2. **作った PNG を自分で読んで、正しい絵か確かめてからユーザーに報告する** (壊れた絵を基準にしない)
3. 閾値の下見は MCP `visual_compare {baseline}`。差分画像 (違う画素が赤) が返る
4. 基準画像の更新は «意図した見た目の変更» のときだけ。更新理由を報告に書く

## 入力の記録

Play 中に MCP `input_record {action:"start"}` → 人が操作 → `{action:"stop", path}`。シナリオの `replay` で使う。乱数と非同期ロードがあるので結果の一致は保証されない。再生後の状態は `assert` で見る。

## 決まり

- 固定の待ち時間 (`frames` の大きな値) より `waitUntil` を使う
- 1 シナリオ 1 観点。失敗したら後続の手順は実行されない
- 追加したシナリオは `playtest_list` / `scenario_list` に出ることを確かめる
