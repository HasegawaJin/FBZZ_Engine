# Font System — BMFont 互換 / SDF / UTF-8 日本語対応

`fbzz::renderer::FontAtlas` と `UISystem` のテキスト描画を、TextMeshPro 相当の
フォントアセット基盤へ刷新する。独自 `.fnt` を AngelCode BMFont 互換形式に置き換え、
可変グリフ矩形・カーニング・SDF・UTF-8 (日本語) を扱えるようにする。

前段として、テキストのジャギーを生んでいた `UIText.hlsl` のシェーダーバグは修正済み
(後述「済: ジャギーの修正」)。本ドキュメントはその先の基盤刷新を対象とする。

---

## 済: ジャギーの修正

刷新の前提として、まず既存パイプラインのバグを潰した。記録として残す。

`Assets/Shaders/UI/UIText.hlsl` は自身を "SDF font atlas text shader" と称し、
`smoothstep(0.35f, 0.65f, dist)` で距離場のしきい値処理を行っていた。
しかし `Tools/FontAtlasGen/gen_font_atlas.py` が出力するのは **SDF ではなくカバレッジ**
(Pillow の `draw.text(..., fill=255)` によるグレースケール描画) であり、
`UISystem.cpp` 側のコメントも `.r チャンネルを coverage として使い` と明記していた。
**シェーダーだけが素材の意味を取り違えていた。**

カバレッジの縁は生成時のアンチエイリアスで約 1 テクセル幅の傾斜を持つ。
そこへ `smoothstep(0.35, 0.65, …)` を掛けると傾斜が 30% 幅へ圧縮され、
アトラス 48px に対し `fontSize = 84` (Title.scene / Result.scene) の 1.75 倍拡大では
傾斜がサブピクセル未満に潰れてほぼ二値化 → 縁が階段状になっていた。

修正は、傾斜を潰すのではなく**画面 1 ピクセル幅へ正規化し直す**方式へ変更した。

```hlsl
float coverage = g_Texture.Sample(g_Sampler, input.uv).r;
float width    = max(fwidth(coverage), 1e-4f);
float alpha    = saturate((coverage - 0.5f) / width + 0.5f);
```

`fwidth(coverage)` は隣接ピクセル間のカバレッジ変化量なので、
`(coverage - 0.5) / fwidth(coverage)` は縁からの符号付き距離をピクセル単位で表す。
拡大時は `fwidth` が小さくなって縁が締まり、縮小時は大きくなって滑らかに減衰するため、
表示サイズによらず常に 1px 幅の AA が得られる。
SDF と同じ理屈を、素材を作り直さずカバレッジ場へ適用した形になる。

**残る限界**: 48px 固定ラスタである事実は変わらないため、
`fontSize = 84` では字形カーブがわずかに角張る。これは本ドキュメントの刷新で解消する。

なお素材側 (`.png` / `.meta`) は無関係だった。`.meta` には `compression = 'Auto'` (BC1/BC3) と
`srgb = true` が入っているが、`FontAtlas::Load` → `ResourceManager::LoadTexture` →
`DX11Texture::Init` の経路は `LoadFromWICFile` で PNG を直読みするだけで、
**`.meta` の圧縮 / sRGB / ミップ設定はこの経路では一切適用されていない**。
8bit グレースケール PNG が `R8_UNORM` になり、リニアなカバレッジがそのまま届いている。

---

## 現状の課題

| # | 課題 | 根拠 |
|---|------|------|
| 1 | グリフが均一セルに配置される | `gen_font_atlas.py` が `cell_w = ceil(maxAdvance) + 2`、`cell_h = line_height + 2` の固定セルで並べる。`i` も `W` も同じ 43x59 を占有し、Roboto アトラス 512x1024 のうち実インクは 1 割程度 |
| 2 | per-glyph メトリクスが無い | `FontGlyph` は `u0/v0/u1/v1/advance` のみ。`xoffset` / `yoffset` / `width` / `height` を持たないため、タイトにパックされたアトラスを描画できない |
| 3 | カーニングが無い | `.fnt` にカーニングペアの概念が無く、`UISystem` も `advance + letterSpacing` しか進めない |
| 4 | ASCII 固定で日本語が入らない | `FontAtlas::m_glyphs[128]` の生配列 + `GetGlyph(char)`。`CHARS` も `0x20`〜`0x7E` 決め打ち |
| 5 | 外部ツールと非互換 | 独自テキスト形式のため、BMFont / Hiero / msdf-atlas-gen の出力を持ち込めない |
| 6 | 距離場ではないので拡大に弱い | 課題 1〜5 と独立して、48px 固定ラスタでは任意サイズでシャープにならない |

---

## 調査で判明した制約

実装方針を左右する事実がリポジトリ側にある。**これらが案の選択を決める。**

| # | 制約 | 影響 |
|---|------|------|
| A | **TTF ソースがリポジトリに無い** | `Assets/Fonts/` にあるのは `.png` / `.fnt` / `.meta` のみ。Roboto / Cinzel / BungeeOutline の TTF は含まれていない (`ThirdParty/ImGui/misc/fonts/` の 6 本はエディタ UI 用の別物)。**既存 3 フォントのアトラスをオフラインで再生成できない** |
| B | **Python が未インストール** | `python.exe` は `WindowsApps` の Store スタブで、`-V` が exit 49。Pillow も無い。**`gen_font_atlas.py` は現状そもそも実行できず、変更しても検証できない** |
| C | **stb_truetype / stb_rect_pack が未ベンダー** | `ThirdParty/Stb/` は `stb_image.h` のみ。`imstb_truetype.h` は ImGui 内部の私物であり、Engine 層から使うのは依存方向 (`Editor → Engine`) の逆転になる |
| D | **日本語グリフを持つフォントがリポジトリに無い** | 日本語対応には Windows システムフォント (`C:\Windows\Fonts\YuGothM.ttc` / `meiryo.ttc` 等) を実行時に参照するか、CJK フォントをリポジトリへ追加する必要がある |
| E | **更新可能テクスチャの API が無い** | `IRenderer` には `CreateNativeTextureFromData` しか無く、部分更新の口が無い。`DX11Texture::InitFromData` は `D3D11_USAGE_IMMUTABLE` / `MipLevels = 1` 固定。動的アトラスには新 API が要る |

制約 A と B により、**「オフライン Python ツールで SDF アトラスを焼き直す」路線は単独では成立しない**。
TTF の入手と Python 環境構築という、コード側で解決できない前提が 2 つ乗る。

---

## 設計方針

- **BMFont テキスト形式を正とする。** 独自形式をやめ、AngelCode BMFont の `.fnt` を読む。
  **WHY**: BMFont / Hiero / msdf-atlas-gen / TextMeshPro が共通で吐ける事実上の標準であり、
  「外部ツールで焼いてドロップするだけ」という TMP と同じワークフローに乗る。
  per-glyph 矩形・オフセット・カーニング・マルチページが仕様に最初から入っている。
- **旧 `.fnt` の読み込みは残す。** 先頭トークンが `info` / `common` なら BMFont、
  `line_height` なら旧形式として分岐する。
  **WHY**: 制約 A により既存 3 フォントは再生成できない。互換を切ると Title / Result /
  Load シーンの表示が全滅する。
- **グリフテーブルをコードポイント辞書にする。** `FontGlyph m_glyphs[128]` を
  `std::unordered_map<char32_t, FontGlyph>` へ置き換える。
  **WHY**: 日本語は疎で広いコードポイント空間に散るため、生配列では表現できない。
- **距離場かどうかを `.fnt` 側が宣言する。** `distance_field = none | sdf | msdf` を持たせ、
  シェーダーは分岐せず**同じ AA 式**を使う。
  **WHY**: 既に導入した `fwidth` 正規化はカバレッジにも SDF にもそのまま効く。
  MSDF のみ「3 チャンネルの中央値を取る」1 行が増えるだけで済む。
- **レイアウトとラスタライズを分離する。** `FontAtlas` は「コードポイント → グリフ矩形 + メトリクス」
  を返すことだけに責任を持ち、その裏が事前生成 PNG か実行時ラスタかを `UISystem` に見せない。
  **WHY**: 静的アトラスと動的アトラスを後から差し替えられるようにするため。

---

## 案の比較

### 案A: 静的 BMFont + オフライン SDF 生成

`gen_font_atlas.py` を BMFont 出力 + タイトパッキング + SDF 生成 (高解像度ラスタ →
ユークリッド距離変換 → ダウンサンプル) に作り替え、アトラスを焼き直す。

- ✅ 実行時コストゼロ。エンジン側は「読むだけ」で済み、追加の third-party 不要
- ✅ ポートフォリオとして「アセットパイプラインを自作した」と説明しやすい
- ❌ **制約 A**: 既存 3 フォントの TTF が無いので焼き直せない (ユーザーが再入手する必要)
- ❌ **制約 B**: Python 環境が無いので実装しても動作確認できない
- ❌ **制約 D**: 日本語は JIS 第 1 水準だけで約 3,000 字。48px SDF + パディングで
  2048x2048 が 3〜4 ページ必要になり、リポジトリに数 MB の PNG が乗る

### 案B: 実行時動的アトラス (TMP の Dynamic 相当)

`stb_truetype.h` + `stb_rect_pack.h` をベンダーし、`.ttf` / `.ttc` を直接参照して
使われたグリフだけを実行時にラスタライズ → SDF 化 → アトラスへ追記する。

- ✅ **制約 A / D が消える**。TTF をリポジトリに入れなくても、Windows システムフォントを
  パス指定するだけで日本語が出る
- ✅ 任意のコードポイントに自動対応。プレイヤー名・動的テキストも扱える
- ✅ **制約 B が消える**。Python 非依存になり、C++ 側だけで完結・検証できる
- ❌ 新しい third-party 依存 (`stb_truetype` / `stb_rect_pack`) が増える。
  AGENTS.md の OK リスト (`stb_image` は既に許可) への追記が要る
- ❌ **制約 E**: 更新可能テクスチャの API を `IRenderer` に足す必要がある (DX11 / DX12 両方)
- ❌ 初出グリフのラスタライズでヒッチが出る (対策: 起動時の事前ウォームアップ)

### 案C: ハイブリッド (推奨)

BMFont 互換パーサを共通の土台として先に入れ、その上で
**静的アトラス (既存資産・外部ツール出力) と動的アトラス (実行時ラスタ) を
`FontAtlas` の裏で切り替える。** TextMeshPro の Static / Dynamic 二本立てと同じ構造。

- ✅ 既存 3 フォントは旧形式のまま動き続ける (制約 A を回避)
- ✅ 日本語は動的アトラスで解決 (制約 D を回避)
- ✅ 外部ツール (msdf-atlas-gen 等) の出力も後から差せる
- ✅ フェーズ分割できるので、途中で止めても壊れない
- ❌ 実装量は最大。ただし各フェーズが独立して価値を持つ

**推奨は案C。** 制約 A / B / D を回避できる唯一の構成であり、
「TextMeshPro のように」という要件に構造まで含めて一致する。

---

## BMFont `.fnt` テキスト形式 (読み取り対象)

```
info face="Roboto" size=48 bold=0 italic=0 charset="" unicode=1 stretchH=100 smooth=1 aa=1 padding=4,4,4,4 spacing=1,1
common lineHeight=57 base=45 scaleW=512 scaleH=1024 pages=1 packed=0 alphaChnl=0 redChnl=4 greenChnl=4 blueChnl=4
page id=0 file="Roboto_0.png"
chars count=95
char id=65 x=112 y=60 width=33 height=35 xoffset=-1 yoffset=10 xadvance=31 page=0 chnl=15
kernings count=2
kerning first=65 second=86 amount=-2
```

FBZZ 拡張として、距離場の種別を宣言する 1 行を追加する
(BMFont の未知トークンは無視される仕様なので、外部ツールの出力とも共存できる)。

```
fbzz distanceField=sdf spread=4
```

`distanceField` が無い `.fnt` は `none` (カバレッジ) として扱う。

---

## レイヤー構成

```
UISystem (レイアウト)
   ↓  GetGlyph(char32_t) / GetKerning(prev, next) / GetLineHeight()
FontAtlas (ファサード)
   ├─ StaticFontSource   ... BMFont .fnt + PNG を読む / 旧 .fnt 互換も担当
   └─ DynamicFontSource  ... stb_truetype でラスタ → SDF 化 → stb_rect_pack で配置
                             → ITexture::UpdateRegion() で GPU へ追記
   ↓
ResourceManager / IRenderer
```

`UISystem` は `FontAtlas` の 3 メソッドしか呼ばないため、どちらのソースかを意識しない。

---

## 追加 / 変更ファイル

| ファイル | 変更 |
|---------|------|
| `Engine/include/Engine/Renderer/FontAtlas.hpp` | `FontGlyph` に `width/height/xoffset/yoffset/page` 追加。`m_glyphs` を `unordered_map<char32_t, FontGlyph>` へ。カーニング表・ページ配列・`FontSourceMode` を追加 |
| `Engine/src/Renderer/FontAtlas.cpp` | BMFont パーサ実装。旧形式との自動判別 |
| `Engine/include/Engine/Renderer/FontSource.hpp` *(新規)* | `StaticFontSource` / `DynamicFontSource` の抽象 |
| `Engine/src/Renderer/DynamicFontSource.cpp` *(新規)* | stb_truetype ラスタ + EDT による SDF 化 + rect pack |
| `Engine/include/Engine/Util/Utf8.hpp` *(新規)* | UTF-8 → `char32_t` デコード (エンジン共通で使えるように Util へ) |
| `Engine/src/Scene/Systems/UISystem.cpp` | `SubmitTextWithAtlas` / `ComputeTextLogicalSize` を可変矩形 + カーニング + UTF-8 走査へ |
| `Engine/include/Engine/Renderer/IRenderer.hpp` | `CreateNativeDynamicTexture(w, h, format)` を追加 |
| `Engine/include/Engine/Renderer/ITexture.hpp` | `virtual bool UpdateRegion(x, y, w, h, const uint8_t*)` を追加 |
| `Engine/src/Renderer/Platform/DX11/DX11Texture.cpp` | `USAGE_DEFAULT` + `UpdateSubresource` による部分更新 |
| `Engine/src/Renderer/Platform/DX12/…` | 同等の実装 (アップロードヒープ経由) |
| `Engine/include/Engine/Scene/Components/UIText.hpp` | `fontPath` に `.ttf` / `.ttc` を許可。`outlineWidth` / `outlineColor` を追加 (SDF なら安価) |
| `Assets/Shaders/UI/UIText.hlsl` | SDF / MSDF 分岐と、距離場ベースのアウトライン |
| `ThirdParty/Stb/stb_truetype.h`, `stb_rect_pack.h` *(新規)* | ベンダー |
| `AGENTS.md` | 外部ライブラリ OK リストに `stb_truetype` / `stb_rect_pack` を追記 |
| `Tools/FontAtlasGen/gen_font_atlas.py` | BMFont 形式 + タイトパッキング + SDF 出力へ (案A 相当。Python 環境が復活したら) |

シェーダーは 4 コピー (`Assets/` / `GreenWare/Assets/` / GameHub テンプレート 2 種) を同期し、
`compile_shaders.ps1` を root と GreenWare の両方で回すこと。

---

## フェーズ分割

各フェーズは単独で完結し、途中で止めても既存の表示は壊れない。

| Phase | 状態 | 内容 | 得られるもの |
|-------|------|------|-------------|
| **1** | **完了** | BMFont パーサ + 可変グリフ矩形 + カーニング + UTF-8 デコード。旧 `.fnt` は互換パスで維持 | 外部ツールの出力が挿せるようになる。既存フォントは現状維持 |
| **2** | **完了** | `ITexture::UpdateRegion` と `CreateNativeDynamicTexture` を DX11 / DX12 に追加 | 動的アトラスの土台。他機能 (VFX 等) でも使える汎用 API |
| **3** | **完了** | `DynamicFontSource`: stb_truetype ラスタ + stb_rect_pack | `.ttf` 直指定が通る。**日本語が出る** |
| **4** | **完了** | `stbtt_GetCodepointSDF` で距離場として焼く | 任意サイズで完全にシャープ |
| **5** | 任意 | `gen_font_atlas.py` を BMFont + SDF 出力へ。静的焼き込みで実行時コストを消す | 配布ビルドの初期ヒッチ解消 |

Phase 4 は当初「EDT を自前実装してラスタ後段に挿す」想定だったが、
`stb_truetype` に `stbtt_GetCodepointSDF()` があったためラスタライズ関数の差し替えだけで済んだ。
またシェーダーの変更も不要だった (次節)。

### Phase 1 の実装結果

追加・変更したもの:

- `Engine/include/Engine/Util/Utf8.hpp` *(新規)* — UTF-8 ⇄ `char32_t`。
  冗長符号化・サロゲート・途切れた系列を U+FFFD へ落とし、**どの失敗経路でも必ず 1 バイト以上進める**
  ことでレイアウトループの無限ループを構造的に防ぐ。
- `Engine/include/Engine/Renderer/FontAtlas.hpp` / `src/Renderer/FontAtlas.cpp` — BMFont パーサへ刷新。
  `FontGlyph` に `width` / `height` / `xOffset` / `yOffset` / `page` を追加し、
  グリフ表を `unordered_map<char32_t, FontGlyph>` へ。カーニング表は
  前後 2 文字を 64bit へ詰めたキーで引く。旧形式は先頭トークンで自動判別し、
  「均一セルの BMFont」(`width = cell_w`, `height = lineHeight`, オフセット 0) に正規化して読む。
- `Engine/src/Scene/Systems/UISystem.cpp` — レイアウトを UTF-8 コードポイント単位へ。
  行幅計算を `ComputeLineWidths()` に一本化し、整列計算と描画で送り幅の規則がズレないようにした。
  グリフはページごとに頂点をまとめ、ページ単位でドローコールを分ける (マルチページ対応)。

検証 (`cl /std:c++20 /W4 /utf-8`):

- `FontAtlas.cpp` / `UISystem.cpp` とも**警告ゼロ**で構文チェック通過。
- 実装ファイルを直接取り込む単体ハーネスで **78 チェック / 0 失敗**。
  実ファイルの Roboto 旧 `.fnt` が旧挙動と 1:1 で一致すること (UV・advance・
  `fallbackAdvance == cell_w * 0.5`)、合成 BMFont の UV 正規化・負の `xoffset`・
  マルチページ・カーニング・`fbzz distanceField=sdf` 行、異常系 3 種、
  UTF-8 の日本語 / サロゲート外 4 バイト / 不正入力の終端保証を確認した。

**未検証**: エディタ / ゲーム実行時の実際の描画。VS でのビルドと目視確認が必要。

### Phase 2 の実装結果 — 動的テクスチャ API

| ファイル | 変更 |
|---------|------|
| `Engine/include/Engine/Renderer/ITexture.hpp` | `DynamicTextureFormat` (R8 / RGBA8) と `UpdateRegion(x, y, w, h, pixels, srcRowPitch)` を追加。既定実装は `false` を返す |
| `Engine/include/Engine/Renderer/IRenderer.hpp` | `CreateNativeDynamicTexture()` を追加 (既定 `nullptr` = 未対応バックエンド) |
| `Engine/include/Engine/Renderer/ResourceManager.hpp` / `.cpp` | 公開窓口 `CreateDynamicTexture()` |
| `DX11Texture` | `USAGE_DEFAULT` + `UpdateSubresource` + `D3D11_BOX`。`DYNAMIC` + `Map(WRITE_DISCARD)` は全面書き直し向けで部分更新に向かないため不採用 |
| `DX12Texture` | 矩形ぶんだけのアップロードバッファ + `CopyTextureRegion`。`PIXEL_SHADER_RESOURCE` ⇄ `COPY_DEST` を遷移し、完了後に `DX12StateTracker` へ現在状態を通知 |

`srcRowPitch` を引数に持たせているのが要点で、CPU 側がアトラス全面のバッファを保持したまま
その部分矩形だけを転送できる。これにより「ダーティ矩形をまとめて 1 回転送」が可能になる。
どちらのバックエンドも生成時にゼロクリアする (未初期化のまま SRV を張ると、
まだ焼いていない領域のゴミが「字の周りの謎の模様」として見えるため)。

### Phase 3 / 4 の実装結果 — 動的 SDF アトラス

- `Engine/include/Engine/Renderer/DynamicFontSource.hpp` / `src/Renderer/DynamicFontSource.cpp` *(新規)*
  — pimpl で stb の型をヘッダーから隠している (`FontAtlas.hpp` は `UISystem` からも
  include されるため、20 万行のシングルヘッダを持ち込まない)。
- `src/Renderer/StbFontImpl.cpp` *(新規)* — `STB_*_IMPLEMENTATION` を展開する唯一の翻訳単位。
- `FontAtlas::Load()` が拡張子で分岐し、`.ttf` / `.ttc` / `.otf` なら動的モードへ。
- `FontAtlas::PrepareText()` を追加し、`UISystem` がレイアウト前に呼ぶ。
  **WHY**: `GetGlyph()` 内で遅延生成すると 1 文字ごとに GPU 転送が走る。
  「先に全部焼く → あとは読むだけ」に分けることで転送をフレーム 1 回に抑え、
  `GetGlyph()` を `const` のまま保てる。
- アトラスは 2048×2048 の R8。埋まったら**ページを増やして継続する**
  (`FontGlyph::page` と `UISystem` のページ別バッチが Phase 1 で入っているのでそのまま乗る)。

**シェーダーは変更していない。** Phase 1 のジャギー修正で入れた
`saturate((v - 0.5) / fwidth(v) + 0.5)` は、そのまま距離場の 1px AA 式になっている。
SDF の `onedge_value = 128` が UNORM 化で 0.502 になり 0.5 判定と一致するため、
カバレッジと SDF を分岐なしで同じ式が扱える。

#### 字の大きさの正規化 (実装中に見つかった問題)

`UISystem` は `scale = fontSize / lineHeight` でグリフを拡大する。
当初 `lineHeight` をフォントの縦メトリクスから取っていたが、実測すると
**Roboto = 1.19em / Yu Gothic = 1.10em / Noto Sans JP = 1.45em** とばらつきが極端で、
同じ `fontSize` を指定しても和文フォントだけ 2 割小さく描画されてしまった。

対処として、動的モードの `lineHeight` は**フォントのメトリクスではなく `em × 1.2` の固定比**にした
(1.2 は組版で一般的な値で、CSS の `line-height: normal` 相当)。
ベースライン位置 (`base`) だけは実フォントの ascent を使う。これを固定比にすると
字が行の中で上下にずれて見えるため。

結果、静的 Roboto と動的フォントで 1em の描画サイズが 70.7px 対 70.0px (比 0.99) に揃った。

#### 検証

`cl /std:c++20 /W4 /utf-8` で全 8 ファイルが**自ファイル診断ゼロ**。
実装ファイルを直接取り込むハーネスで **34 チェック / 0 失敗**:

- 遅延生成: 焼く前はページ 0 かつ GPU 未接触 → `PrepareText` で 1 ページ生成
- 転送回数: **ページあたり 1 回**、同じ文字列の再呼び出しでは 0 回、新規文字のときだけ +1
- SDF 値域: グリフ内部の最大値 191 (> onedge 128)、padding 領域の角 0
- メトリクス: 全角 '日' の送りが半角 'H' より広い、スペースは図形なし
- フォントに図形が無いコードポイント (U+E000) も登録され、毎フレーム再試行しない

加えて、`UISystem` と同じレイアウト式 + `UIText.hlsl` と同じ AA 式で CPU 合成した
PNG を出力して目視確認した (84px / 28px とも日本語・欧文ともシャープ、ジャギーなし)。

---

## 決定事項

1. **third-party 追加**: **`stb_truetype` / `stb_rect_pack` をベンダーする**(決定・実施済み)。
   `ThirdParty/Stb/` に stb_truetype v1.26 / stb_rect_pack v1.01 (いずれも public domain) を配置し、
   AGENTS.md の OK リストへ追記した。
   **WHY**: 「数学・物理は自作」は保つが、TrueType の字形解釈は別ドメインであり、
   自作すると glyf / loca / cmap / hmtx パースというフォント刷新とは別スケールの作業になる。
   `stb_image` を既に許可している方針と整合する。
2. **日本語フォント**: **OFL フォントを同梱する**(決定・実施済み)。
   `Assets/Fonts/MPLUS1p/MPLUS1p-Regular.ttf` (1.7MB) を OFL ライセンス本文とともに配置した。
   **WHY (同梱)**: Windows システムフォント参照は環境依存で、配布ビルドで
   「環境によっては出ない」不具合を作る。ポートフォリオとして確実性を優先する。

   **WHY (Noto Sans JP ではなく M PLUS 1p)**: 当初 Noto Sans JP を予定していたが、
   実際に焼いて確認したところ 2 つの問題があった。

   - Google Fonts が配布する `NotoSansJP[wght].ttf` は**可変フォント**で、
     `glyf` に入っている既定マスターが最小ウェイト側にある。`stb_truetype` は
     `gvar` のデルタを適用しないため、Regular ではなく Thin 相当の細い字形で焼かれる。
   - 静的版の `NotoSansJP-Regular.otf` は **CID-keyed CFF** で、
     `stb_truetype` の CFF 対応では字形が壊れる (実際に "Hello" が "H .!!." になった)。

   M PLUS 1p Regular は静的な `glyf` ベース TTF なので `stb_truetype` が正しく解釈でき、
   サイズも 1.7MB と Noto の 9.6MB より小さい。ライセンスも同じ SIL OFL 1.1。
   将来 Noto を使いたい場合は、可変フォントを Regular で静的インスタンス化してから
   同梱する必要がある (fontTools 等のツールが要る)。

3. **`.ttf` / `.ttc` / `.otf` を `.meta` (GUID) 対象へ追加**(実施済み)。
   `AssetDatabase::ShouldHaveMeta()` に追加した。
   **WHY**: `UIText.fontPath` がフォント原本をパスで直接参照するため、
   `.fnt` や `.physmat` と同じくリネーム・移動で参照が切れないよう GUID が要る。

## 使い方

`UIText.fontPath` に拡張子つきでフォント原本を指定すると動的モードになる。

```
fontPath = 'Assets/Fonts/MPLUS1p/MPLUS1p-Regular.ttf'
```

拡張子なし (従来どおり) なら静的モードで `.fnt` + PNG を読む。
既定フォント (`fontPath` が空のとき) は `ProjectSettings::defaultFontPath` のままなので、
既存シーンの挙動は変わらない。日本語を出したいテキストだけ `.ttf` を指すか、
プロジェクト全体で切り替えるなら `defaultFontPath` を差し替える。

---

## 残る未決事項

1. **既存 3 フォントの扱い** — Roboto / Cinzel / BungeeOutline は現在、旧形式の互換パスで
   動いている。TTF を再入手して BMFont + SDF へ焼き直すか、このまま維持するか。
   焼き直さない限り 48px ラスタの限界 (大きい `fontSize` でのカーブの角張り) は残る。
   なお `.ttf` を入手できれば動的モードへ差し替えるだけで SDF 化できる。
2. **MSDF の要否** — 単一チャンネル SDF で足りるか、角の保持のため MSDF まで行くか。
   MSDF は自前実装だとエッジカラーリングが重い。msdf-atlas-gen の出力を読む前提にするなら
   Phase 1 の BMFont パーサだけで対応でき、追加実装は不要。
3. **カーニング (動的モード)** — `stb_truetype` が読むのは旧来の `kern` テーブルのみで、
   GPOS ベースのカーニングしか持たない現代的なフォントでは 0 が返る。
   欧文の詰めにこだわるなら、静的 BMFont アトラス (外部ツール生成) を使う方が確実。
4. **初出グリフのヒッチ** — 大量の新規文字が同時に現れると、そのフレームで
   ラスタライズ + 転送が走る。起動時によく使う文字を先に焼く
   ウォームアップ API (`FontAtlas::PrepareText` を起動時に呼ぶだけ) で回避できるが、
   実測してから判断する。
5. **アトラスの解放** — 現状、一度焼いたグリフは解放されない。
   長時間プレイで多様な文字が出続けるとページが増え続ける。
   実用上は常用漢字を焼き切っても数ページで頭打ちになる見込みだが、
   問題が出るならフレーム跨ぎの LRU 破棄が要る。
