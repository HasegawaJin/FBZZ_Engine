
---

## 5. シーンを組んだ（Result.scene / StageSelect.scene）

エディタが起動していなかったので MCP は使えず、**.scene（TOML）を直接書いて**います。
形式は `Title.scene` / `Options.scene` に合わせてあります。開いて保存すれば
エディタ側の整形が入ります。

| ファイル | 中身 |
|---|---|
| `Assets/Scenes/Result.scene` | 90 オブジェクト。CLEAR と FAILED を 1 シーンに入れ、`active` で切替 |
| `Assets/Scenes/StageSelect.scene` | 125 オブジェクト。7 行ぶんの器と右パネル |

### CSS → エンジンの対応（Title.scene から実測して決めた）

```text
fontSize      = size_px * 1.2
letterSpacing = letter_spacing_px（そのまま）
position.y    = baseline_y - 1.16 * size_px      1.16 は hhea.ascent / unitsPerEm
position.x    = x                                align 0 は左端 / 2 は右端 + pivot [1,0]
画像 position = layout_shapes.json の anchor
画像 scale    = size + pad*2（PNG の実寸）
```

`Title.scene` の `Version`（87.66）と `LawText`（954.60）がこの式でぴったり一致します。
組んだシーンを PIL で描き起こして `Reference/` と重ねた差分は、
**発光（text-shadow / box-shadow）を除けばほぼ 0** でした。発光はスプライトに焼けないので、
必要ならマテリアル側で足してください（タイトルのバーと同じ扱い）。

### CSS の border は 1px の単色 UIImage にした

区切り線はテクスチャを持たない `UIImage` です（`Result.scene` の旧 Background と同じ）。
`layout_shapes.json` の `dividers` に位置と色が入っています。
床の楕円リング（`ring` / `ring2`）は線ではないので除いてあります — シェーダ側の担当です。

### 追加したスクリプト

| ファイル | 役割 |
|---|---|
| `Scripts/Game/ResultPresenterComponent.hpp` | **書き換え。**1 本の文字列ではなく、項目ごとの UIText に流し込む |
| `Scripts/UI/StageSelectComponent.hpp` | 新規。行の選択・記録表示・解放判定 |
| `Scripts/UI/StageProgressState.hpp` | 新規。ステージごとの解放状態と自己ベスト |
| `Scripts/UI/UiNavSe.hpp` | 新規。メニューの移動・決定・戻るのパス |

**ノード名で引いています。**参照フィールドを 40 個並べると、シーンを作り直すたびに
割り当ても作り直しになるため。名前さえ保てば C++ を触らずに配置を変えられます。
引いている名前 37 個は、シーンに全部存在することを機械で確認済みです。

### 手で入れてほしいもの

1. `Scripts/Game/GameResultState.patch.md` の差分（`bossHpRemain01` / `bossPhase` と、
   クリアタイムの採点基準）
2. 敗北を確定させる場所で、その 2 つに値を入れる
3. **入力アクション。**`Submit` / `Cancel` と `GetMoveAxis().y` を使っています。
   マウスで選ぶなら行に `UIButton` を足して `ui.IsHovered` を見る形へ足せます
   （タイトルと同じ作りにできます）

### 未確認

エディタで開いていないので、**エンジンが実際にこの TOML を読めるかは未検証**です。
`format_version = 2`、`instanceId` の重複なし、親子の参照切れなし、TOML として
パースできることは確認しました。開いたときに落ちるなら、まず `Result.scene` を
1 つ開いて、エディタの出す文言をもらえれば直します。
