# RobotMinimal HUD

細い枠と両端の装甲ブロックだけでロボットらしさを出したPNG素材。画像生成は組み込み image_gen を使用。プロンプトは Prompts.md。
Stage_01 / Stage_02 の体力・スタミナ（息）・ボス体力へ組み込み済み。既存スクリプトが白い中身の色と残量を更新する。ゲーム内表示の確認は未実施。
Stage_03 はプレイヤー未配置のため、既存の体力・ボスHUDの素材のみ適用。スタミナやプレイヤーを新規生成してはいない。

## パーツ

| PNG | 用途 |
|---|---|
| robot_player_frame.png | 体力・スタミナ共通の固定枠＋暗い下地 |
| robot_boss_frame.png | ボス用の固定枠＋暗い下地 |
| robot_fill_white.png | 色と残量をエンジンで制御する共通の中身。Drainにも使う |
| robot_stamina_green.png | 緑を画像に持つスタミナ用の中身。UIImage.colorは白で使う |

PNGは透過RGBA。付属.metaにSprite矩形を登録済み。PNG全面には余白があるため、UIImageの画像欄では **同名のSpriteサブアセット** を選ぶ。
中身は生成画像の矩形内部を切り出し、縁のばらつきと余白が残量制御に入らないようにしている。

## FBZZEngineの設定

Canvasの下に固定枠と中身を別々のUIImageとして置く。中身のsortOrderを枠より大きくする。
materialPathは空、imageTypeはSimple、fillMethodはEdge (0)、fillOriginはLeft (0)。
固定枠のfillAmountは1、colorは白。中身だけfillAmountを0～1で変更する。

Sprite参照例:
`Assets/Sprites/HUD/RobotMinimal/robot_fill_white.png::sprite::robot_fill_white`

既存スクリプトと組む場合、色を毎フレーム更新するため白い中身を使う。
InspectorのFullなどを変更するとPNGを作り直さず色を変更できる。

| 用途 | 中身のGameObject名 | 既存の制御スクリプト | 推奨色RGBA |
|---|---|---|---|
| 体力 | HUD_HealthFill | PlayerHealthBarComponent | Full: (0.87, 0.38, 0.35, 1) |
| スタミナ（息） | HUD_BreathFill | PlayerBreathBarComponent | Full: (0.45, 0.74, 0.29, 1) |
| ボス体力 | HUD_BossFill | BossHealthBarComponent | Full: (0.84, 0.49, 0.31, 1) |
| ボス遅延帯 | HUD_BossDrain | BossHealthBarComponent | Drain Colorで調整 |

スタミナはLowも (0.31, 0.52, 0.20, 1) にすれば通常の残量変化を緑系で統一できる。
Exhaustedは既存の警告色を使うか、常に緑にしたい場合は同じ緑に設定する。
UIImage.colorだけを変更しても既存スクリプトが上書きするため、スクリプト側のColor欄を設定する。

枠＋下地のPNGはHUD_HealthBackground / HUD_BreathBackground / HUD_BossBackgroundへ割り当てる。
BreathとBossのBackground Colorは白 (1,1,1,1) に設定する。暗い既定値のままだと枠が黒くなる。
体力側は背景色を上書きしないため、UIImage.colorを白に設定する。
Boss Backgroundとして使えば、ボス不在時の非表示も既存スクリプトに追従する。

## 配置の目安

枠の左上を原点とする。まずSimpleで配置し、Canvas Editorで表示を確認して微調整する。

| バー | 枠のサイズ | 中身の位置 | 中身のサイズ |
|---|---|---|---|
| 体力 | 578 × 54.4 | (53.176, 13.6) | 471.648 × 27.2 |
| スタミナ | 578 × 40.8 | (53.176, 10.2) | 471.648 × 20.4 |
| ボス | 1088 × 74.8 | (81.6, 26.18) | 924.8 × 22.44 |

枠はBackgroundとして下層、Drainをその上、Fillを最上層へ置く。
枠のサイズを変える場合、中身の位置とサイズも合わせて調整する。中身のscaleで残量を表さずfillAmountを使う。

## 自作スクリプトからの制御

UIImageを持つオブジェクト自身のScriptなら:

```cpp
ui.SetImageColor({ 0.45f, 0.74f, 0.29f, 1.0f });
ui.SetImageFillAmount(0.65f);
```

別オブジェクトは `ui.SetImageFillAmount(fillObject, ratio)` と
`ui.SetImageColor(fillObject, color)` で制御できる。
ratioは最大値が正のときの現在値÷最大値を0～1に収める。最大値が0以下なら0。
同じFillを既存バー制御スクリプトと別スクリプトの両方から更新しない。

## 確認状況

PNGのRGBA形式・外側の透過・切り出し矩形の範囲を確認。
UIImage / ScriptUIProxy / 既存3種のバー制御と.meta読み込み実装に照合済み。
シーンのGUID＋Sprite ID参照、FillとDrainの同一矩形、描画順、スクリプトの対象参照を静的確認。
Stage_01 / Stage_02 はHealthを赤、Breathを緑、Bossを橙赤に設定。Breathの息切れ警告とBossの転倒・Drain演出は維持。
PNGを含む描画、Play中の増減は未確認。エディターでシーンを再読み込みして確認する。
