# Sprite 参照の設計

Texture を切り分けた 1 コマ (Sprite) を、シーン・マテリアル・スクリプト・AI から
どう指すかの規約。**「書くのは名前、保存されるのは ID、壊れたら黙らない」** の一文が全部。

- 実装: `Engine/Asset/TextureAsset.{hpp,cpp}` (解決)、`Engine/Asset/GuidRefCodec.cpp` (保存形)、
  `Engine/Asset/TexDescSerializer.cpp` (.meta)、`Editor/Panels/SpriteEditorPanel.cpp` (編集)
- 前提: `Engine/Asset/GuidRefCodec.hpp` の「guid が権威 / `|` 以降は人向けのヒント」規約。
  Sprite はその規約をサブアセットへ広げただけで、新しい構文を 1 つも足していない

---

## 1. 参照の形

このリポジトリには既に「**左が機械の正本、右が人間向けの控え**」という規約がある
(`GuidRefCodec`)。Sprite にもそれをそのまま適用する。

```
guid:2ed3edb2…::sprite::836d2b4d-…|Assets/Sprites/HUD/keys.png::sprite::Key_W
└──── 権威 (guid + Sprite ID) ────┘ └────────── ヒント (可読性と復旧) ──────────┘
```

読むときは 4 形すべてを受ける。書く (保存する) ときは必ず 1 番目へ正規化する。
この非対称が「人と AI は名前で書ける / ディスク上は ID で壊れない」を両立させる。

| 書き手 | 形 | 用途 |
|---|---|---|
| Editor の保存 | `guid:<tex>::sprite::<id>\|<path>::sprite::<name>` | ディスク上の正本 |
| 人 / AI / スクリプト | `Assets/…/keys.png::sprite::Key_W` | **手で書く唯一の形** |
| ツール生成 | `Assets/…/keys.png::sprite::<id>` | 生成器が ID を持っているとき |
| Single Texture | `Assets/…/icon.png` | 全面 |

## 2. .meta

```toml
[[texture.sprites]]
id   = '836d2b4d-ed00-4420-ba95-fc329c7005e5'   # 権威。発行は一度きり
name = 'Key_W'                                   # テクスチャ内で一意。別名キーを兼ねる
x = 128; y = 128; width = 64; height = 64
pivot_x = 0.5; pivot_y = 0.5
```

- `id` は UUID v4。新規は `util::GenerateUUID()`。
  ID を持たない旧 `.meta` へは `sprite-<hash>` を決定論的に振る (移行専用・読み取り経路のみ)。
  ハッシュの種は **`[meta] guid`** で統一する。保存側と読み込み側で種が違うと、
  同じ Sprite に別の ID が付いて全参照が切れる。
- `name` はテクスチャ内で一意。Sprite Editor が重複を弾き、保存時にも警告する。
- `width` / `height` が 0 の Single Sprite は「画像全体」を意味する
  (元画像を差し替えても全面に追従させるため)。

## 3. 解決

`ResolveSpriteReference()` が唯一の入口。UISystem / PresentationSystem / GeometryPass /
Inspector / AI バスは全員ここを通り、自前で `.meta` を読まない。

```
"::sprite::" が無い              → NotASpriteReference (テクスチャ全面)
テクスチャ寸法が未着              → TextureSizeUnknown  (読み込み中。警告しない)
.meta が読めない                 → MetaMissing         ★壊れている
id 一致                         → ResolvedById
name 一致 (一意なので曖昧さなし)   → ResolvedByName
Single かつトークン == ファイル名  → ResolvedAsImplicitSingle
どれでもない                     → SpriteNotFound      ★壊れている
```

★ は**無言で全面に落とさない**。参照文字列ごとに 1 回だけ WARN し、Inspector は
フィールドを赤で表示し、AI バスは `component.set` をエラーで拒否する。
以前はここが黙って uv 0..1 に落ちていたため、「割り当てたのに分割前の絵が出る」が
ログにも UI にも出ないまま起きていた。

### どの欄が受けるか
切り抜きを活かせるのは、矩形を読む経路を持つ欄だけ。それ以外の欄へ入れても
`ResourceManager::LoadTexture` が `::sprite::` を落として読む (`AssetPathService::normalizeTextureKey`)
ので、**エラーにならずアトラス全面が出る**。受けられない欄は入り口で断る。

宣言は**欄の拡張子フィルター**で行う。`".sprite"` を含むフィルターだけが Sprite を受け、
ピッカーがコマを並べ、D&D を受理する (`Editor/Util/ImGuiWidgets.hpp` の `kSpriteAssetFilter`)。
フィルターは全部の欄が既に持っているので、新しいメタデータも仮想関数も足さずに済む。

| 受ける欄 | 矩形を読む場所 |
| `UIImage.texturePath` / `UIButton` の状態別スプライト | `UISystem` |
| `SpriteRenderer.spritePath` | `GeometryPassHelpers` |
| `.mat` の `albedo` (**メッシュ描画 = renderPath Auto のみ**) | `ApplyAlbedoSpriteUv` → `uvTiling` / `uvOffset` |
| `.fluid` の texture 発生源 (`source.texture`) | `LoadFluidSourceMask` が切り抜いてから 256² へ縮める |

`.mat` の albedo 以外のスロットが受けないのは、UV の合成が 1 描画に 1 組しか無いため。
Particle / Trail / UI / Decal のパスは `ApplyAlbedoSpriteUv` を通らないので同じく受けない。

流体のマスクだけは `ResolveSpriteReference` ではなく、同じキャッシュ
(`GetCachedTextureImportSettings` + `FindSprite`) からピクセル矩形を引く。
GPU のテクスチャを作らずに画像を自前で展開して縮めるので、必要なのは UV ではなく
整数の矩形で、`ResolveSpriteReference` が先に要求する «元画像の寸法» をまだ持っていない。

`.meta` は書き込み時刻でキャッシュする。解決は毎フレーム・毎ドローで走るので、
素直に読むと 1 スプライトにつき数千行の TOML を毎回パースすることになる
(SpriteRenderer が実際そうなっていた)。ID / 名前の索引もキャッシュ側で持つ。

## 4. ID の寿命

1. `id` は**発行一度きり**。リネーム・矩形移動・並び替えでは変えない
2. 再スライスは重なり面積で既存へ畳み込む (`Editor/Util/SpriteSlicer.hpp`)。
   Smart は ID・名前・pivot・Border を残して**矩形だけ**合わせ直し、Safe は重なった
   既存に一切触れない。どれも人が「この絵のための値」として詰めたもので、
   切り直しの意図は位置の修正でしかない
3. **Delete Existing は全 ID を再発行する**。実行前に「この .meta を参照している N 件が
   壊れます」と数えて確認させる
4. `.meta` を読むキャッシュは必ず書き込み時刻で無効化する。
   切り直した直後に古い ID を配るのはキャッシュの取りこぼしが原因
5. 矩形の生成 (グリッド / alpha 島) と畳み込みは Sprite Editor と AI バスで**同じ実装**を通す。
   写経を 2 つ持つと、片方だけが規則から外れて「AI で切ると参照が切れる」差になる

## 5. AI の触り口 (Editor バス)

| op | 返すもの |
|---|---|
| `sprite.list(path)` | `[{id, name, x, y, width, height, pivot, border, reference}]`。`reference` はそのまま `component.set` に貼れる完成形 |
| `sprite.thumbnail(path, sprite)` | **切り抜き済み** PNG (base64)。AI が目でコマを選べる唯一の手段 |
| `sprite.rename(path, sprite, name)` | 新しい名前。`_36` → `Key_W` |
| `sprite.slice(path, …)` | 生成数・引き継いだ ID 数・壊れる参照の数。`type=grid` / `automatic`、`mode=smart` / `safe` / `replace` |

`asset.thumbnail` はファイル丸ごとしか返さないので、シートの何番目がどの絵かは
AI から見えない。名前が `_0`〜`_271` の連番である以上、`sprite.list` +
`sprite.thumbnail` が無いと AI は割り当て先を選べない。

## 6. 人の触り口

- Inspector のフィールドは `keys.png / Key_W` + 切り抜きサムネ。壊れた参照は赤 + 理由
- Sprite Editor の Rename ボタンで一括リネーム (接頭辞 + 連番)。ID は変えないので参照は切れない
- Slice の Delete Existing は referrer 数付きの確認を挟む

## 7. 採らなかった案

| 案 | 内容 | 判定 |
|---|---|---|
| 1 Sprite = 1 ファイル (`Atlas/Key_W.sprite`) | 特別扱いが完全に消え、guid・rename 追従・D&D・サムネがタダで付く | キーボードシート 1 枚で `.sprite` + `.meta` が 544 ファイル。今の用途に合わない |
| Sprite ごとにフラットな guid | 参照が単一トークンになる | AssetDatabase が「ファイルを持たないエントリ」を索引する必要がある。しかも `DecodeGuidRef` はパス文字列を返す設計なので内部では結局 `path::sprite::id` に戻る |
