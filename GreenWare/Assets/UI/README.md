# GreenWare — UI素材（文字を除く）

文字と数値はエンジン側で描く前提の受け渡しです。ここに入っているのは**図形素材と、文字を置くための情報**です。

```
Assets/
  Title/      8 枚   タイトル画面の図形（＋@2x）
  Options/    16 枚   OPTIONS画面の図形（＋@2x）
  Font/                書体データとライセンス
  Reference/           完成形の全体画像（6画面 ＋@2x）
  layout_shapes.json   図形の配置座標
  text_layout.json     全画面の文字（位置・書体・サイズ・字送り・色）
```

`Reference/` が突き合わせ用の完成形です。`Title.png` と、OPTIONSの5状態。
実装した画面をこれと重ねれば、ズレがすぐ分かります。

---

## 1. フォント形式について

**FBZZ Engine がどの形式のフォントを読むのか、こちらからは確認できませんでした。**
接続されているフォルダは `GreenWare_SoundAsset` だけで、エンジン側のソースが見えません。
エディタのMCP経由でアセット型を調べにいきましたが、エディタが起動していないため
`FBZZEditorCommandBus` に接続できませんでした。

なので今回は、**どの形式にも変換できる元データ（TTF / OTF）**を入れてあります。
形式を教えてもらえれば、その形に変換して出し直します。

| 想定される形式 | こちらで用意できるもの |
|---|---|
| 実行時ラスタライズ（FreeType / stb_truetype） | このTTF/OTFをそのまま |
| ビットマップフォントアトラス | BMFont形式（`.fnt` ＋ ページPNG） |
| SDF / MSDF アトラス | 距離場のアトラスPNG ＋ メトリクスJSON |

## 2. 使っている書体

| 書体 | ウェイト | 使用箇所数 |
|---|---|---|
| DejaVu Sans | 400 | 82 |
| Noto Sans CJK JP | 400 | 70 |
| DejaVu Sans Mono | 400 | 54 |
| DejaVu Sans Mono | 700 | 15 |
| DejaVu Sans | 200 | 5 |
| DejaVu Sans | 700 | 1 |

`Font/` の中身です。

| ファイル | 用途 |
|---|---|
| `DejaVuSans.ttf` | 英数の本文 |
| `DejaVuSans-Bold.ttf` | ロゴの `Green`、選択中のメニュー項目 |
| `DejaVuSans-ExtraLight.ttf` | ロゴの `Ware`、`OPTIONS` の見出し |
| `DejaVuSansMono.ttf` | 数値、`CONTROLS` の右列、版数表示 |
| `DejaVuSansMono-Bold.ttf` | `CONTROLS` の見出し |
| `NotoSansCJKjp-Regular-subset.otf` | 日本語（**22.8 KB**） |

日本語は**実際に使っている113文字だけに絞ってあります**（16.5 MB → 22.8 KB）。
文言を足したときは `used_chars.txt` に文字を追加して、以下で作り直せます。

```bash
python3 -m fontTools.subset NotoSansCJKjp-Regular.otf \
  --text-file=used_chars.txt --output-file=NotoSansCJKjp-Regular-subset.otf \
  --layout-features= --no-hinting --desubroutinize
```

元の全部入りが必要なら言ってください。別途送ります（OFL、再配布可）。
ライセンスは `Font/license/` に入れてあります。DejaVu も Noto も再配布できるものです。

## 3. text_layout.json の読み方

```json
{ "id":"Label_master", "text":"マスター", "font":"Noto Sans CJK JP", "weight":"400",
   "size_px":19, "letter_spacing_px":1.9, "color":"rgb(169, 166, 160)",
   "align":"left", "x":452, "baseline_y":431, "ink":[452,413,84,19] }
```

- **位置はベースライン基準**です。`x` は基準辺（`align:"right"` なら右端）、`baseline_y` がベースラインのY
- `letter_spacing_px` は1文字ごとの追加送りです。**最後の文字のあとにも入ります**（CSSの挙動）。
  右寄せのとき、この分だけ右に余白ができます。合わせたい場合は最後の1文字ぶんを引いてください
- `ink` は参考用の実測バウンディングボックスです

画面は6つ入っています。`TITLE` と、OPTIONSの5状態（`INPUT_KBM` / `INPUT_PAD` / `GAME` / `VIDEO` / `AUDIO`）。

### スタイルは11種類だけ

| 書体 | ウェイト | サイズ | 字送り | 色 | 箇所数 | 用途 |
|---|---|---|---|---|---|---|
| DejaVu Sans | 400 | 16 px | 0 px | `rgb(169, 166, 160)` | 35 | CtrlRow |
| Noto Sans CJK JP | 400 | 14.5 px | 1.45 px | `rgb(169, 166, 160)` | 35 | CtrlL |
| DejaVu Sans Mono | 400 | 14 px | 0.84 px | `rgb(222, 219, 213)` | 35 | CtrlR |
| DejaVu Sans | 400 | 16 px | 0 px | `rgb(242, 240, 236)` | 23 | Law, Row |
| Noto Sans CJK JP | 400 | 19 px | 1.9 px | `rgb(169, 166, 160)` | 17 | Label |
| DejaVu Sans | 400 | 19 px | 4.56 px | `rgb(111, 109, 104)` | 15 | Nav |
| Noto Sans CJK JP | 400 | 19 px | 1.9 px | `rgb(222, 219, 213)` | 9 | Value |
| DejaVu Sans Mono | 400 | 19 px | 1.52 px | `rgb(222, 219, 213)` | 8 | Num |
| DejaVu Sans Mono | 400 | 11.5 px | 4.6 px | `rgba(87, 85, 81, 0.55)` | 6 | Label |
| DejaVu Sans | 200 | 60 px | 14.4 px | `rgb(239, 236, 231)` | 5 | Head |
| DejaVu Sans | 400 | 19 px | 4.56 px | `rgb(255, 255, 255)` | 5 | Nav |
| Noto Sans CJK JP | 400 | 19 px | 1.9 px | `rgb(255, 255, 255)` | 5 | Label |
| DejaVu Sans Mono | 400 | 19 px | 1.52 px | `rgb(255, 255, 255)` | 5 | Num |
| DejaVu Sans Mono | 700 | 14 px | 4.48 px | `rgb(142, 139, 133)` | 5 | Ctrl |
| DejaVu Sans Mono | 700 | 14 px | 0 px | `rgb(99, 164, 255)` | 5 | Sym |
| DejaVu Sans Mono | 700 | 14 px | 0 px | `rgb(255, 106, 92)` | 5 | Sym |
| DejaVu Sans | 400 | 30 px | 7.8 px | `rgb(118, 116, 112)` | 4 | Menu |
| Noto Sans CJK JP | 400 | 13 px | 2.08 px | `rgb(108, 106, 102)` | 3 | Sub |
| DejaVu Sans | 700 | 122 px | 6.71 px | `rgb(242, 240, 236)` | 1 | Logo |
| Noto Sans CJK JP | 400 | 15 px | 3.3 px | `rgb(142, 139, 133)` | 1 | Law |

## 4. layout_shapes.json の読み方

**PNGは上下左右に32pxの余白を含みます。** `anchor` はPNGの左上を置く位置なので、そのまま使えます。

- `Row_*` / `CtrlRow_*` は**行の枠**です。区切り線はその下端 `divider_y` に `Divider.png` を置きます
- スライダーは `Track.png` を置き、`Track_fill.png` を `fill_ratio` の分だけ左から切って重ね、`Knob.png` を置きます
- 選択中の行は `Track_fill_on` / `Knob_on`、タブは `NavBar_on`、行頭に `Focus_bar.png`

## 5. 注意

**書体を差し替えると、text_layout.json の座標がずれます。** 位置はこの書体で組んだ結果なので、
別のフォントにするなら先に決めてください。決まった時点で測り直します。

DejaVu Sans は「手元にあった中で素直な書体」を選んだだけなので、
ゲームのUIとして別の顔にしたいなら、今が差し替えどきです。
