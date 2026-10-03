<!--
/// @file    terrain-layers.md
/// @brief   地形の可変レイヤー・質感・穴・ブラシの契約。
/// @author  Hasegawa Jin
/// @date    2026-09-17
-->
# 地形 — 可変レイヤー・質感・穴・ブラシ

- 状態: 実装済み (2026-09-17)。**ビルドと実機描画の確認は未了**

bindless 化 ([bindless.md](bindless.md)) で「テクスチャはスロット 32 枠まで」という制限は外れた。
それでも地形は `RGBA8 スプラット = 4 層固定` のままだった。この文書はその 4 を外し、
外した先で効く質感 (高さブレンド・三方向投影・マクロ変化・PBR)、穴、ブラシ群の契約を決める。

実装の入口:

| 何 | どこ |
|---|---|
| データと不変条件 | `Engine/Scene/Components/TerrainComponent.hpp`、`Engine/Scene/TerrainSplat.hpp` |
| 描画 | `Systems/RenderPasses/Geometry/TerrainRenderPass.cpp`、`Assets/Shaders/Terrain/TerrainSurface.hlsli` |
| 保存 | `Scene/TerrainAssetSerializer.cpp` (.terrain TOML)、`Asset/FzTerrainSerializer.cpp` (FZTN) |
| 物理・経路 | `Physics/HeightFieldCollider`、`Systems/ColliderSync.cpp`、`Systems/NavMeshBakeSystem.cpp` |
| 草 | `FiberComponent` の `terrainLayer`、`FiberSurfaceSources.hpp` |
| ブラシ | `Editor/src/Tools/TerrainBrush.*` (人と AI バスが共有する唯一の実装) |

---

## 1. レイヤー — 頂点ごとに「上位 4 層の番号と重み」

### なぜ枚数を増やす方式にしないか

スプラットマップを 4 層ごとに 1 枚足す方式は、層数ぶん毎画素のフェッチと正規化が増える。
地形の大半の画素は 1〜2 層しか使っていないのに、N 層ぶんの重みを読むことになる。
**番号と重みを持つ方式なら、画素あたりの読み出しは層数に依存しない**。
番号は `uint8` なので理論上 255 層、層ごとのテクスチャは bindless 添字で引くので枠の上限もない。

### CPU の正本

```
layerMaterials : vector<string>            層の並び。番号 = 添字
splatIndices   : vector<uint8>  頂点 × 4    層番号
splatWeights   : vector<uint8>  頂点 × 4    重み [0, 255]
holeData       : vector<uint8>  セル × 1    1 = 穴。空 = 穴なし
```

頂点 `(x, z)` の添字は `(z * columns + x) * 4 + slot`、セル `(cx, cz)` は `cz * (columns - 1) + cx`。

**不変条件 (正準形)** — `TerrainSplat::Canonicalize` だけが作る:

1. 4 枠の重みの合計は **ちょうど 255**
2. 重みの降順に並ぶ。同じ重みなら番号の昇順
3. 同じ番号は 2 枠に現れない
4. 重み 0 の枠の番号は 0

正準形にするのは、Undo のスナップショット比較・保存差分・Fiber の内容署名が
「見た目が同じなら同じバイト列」を前提にするため。

### 塗り — 目標分布への線形補間

`TerrainSplat::BlendToward(slot, layer, t)` は、現在の分布 `w` を «その層 100%» の分布 `e_L` へ
`w' = lerp(w, e_L, t)` で寄せる。その後、

- 上位 4 層だけを残し (5 層目以下は捨てる)、合計 255 へ最大剰余法で量子化する
- `t > 0` なのに量子化で 1 段も進まない場合は、対象層へ 1/255 を強制的に移す
  (8 bit の最小単位より細かい `strength * dt` で長押ししても塗りが進まない、を防ぐ。旧実装と同じ理由)

層の削除 (`RemoveLayer`) は、その番号の枠を捨てて番号を詰め、残りを正規化する。
全重みが消えた頂点は層 0 = 255 に戻す。並べ替え (`MoveLayer`) は番号の置換だけで、重みは変えない。

### 旧データ (4 チャンネル RGBA) からの移行

`splatData[v*4+c]` を「番号 c、重み splatData」として読み、正準形にするだけ。**見た目は変わらない。**

---

## 2. GPU 契約

### 定数とバッファ

`TerrainObjectCB` (b1, 176 バイト)。正本は `RenderPassContext.hpp`、HLSL は `TerrainSurface.hlsli`。

```
float4x4 worldMatrix
float4x4 wvpMatrix
float4   weather           x=wetness y=darkening z=puddleAmount
float4   terrainParams     x=ローカル幅 X [m]  y=ローカル奥行 Z [m]  z=heightBlendDepth  w=自動ブレンドを持つ層があるか (0/1)
uint     layerBufferIndex  層配列 StructuredBuffer の bindless 添字
uint     layerCount
uint     splatColumns
uint     splatRows
```

層配列 `TerrainLayerGpu` (StructuredBuffer, 1 要素 96 バイト):

```
uint  diffuseIndex, normalIndex, aoRoughnessIndex, heightIndex   bindless 添字 (INVALID にしない)
float tilingX, tilingZ, normalStrength, roughness
float ambientOcclusion, hasAoRoughness, hasHeight, heightBlend
float autoMinHeight, autoMaxHeight, autoHeightFade, autoBlendEnabled
float autoMinSlope, autoMaxSlope, autoSlopeFade, autoBlendStrength
float triplanar, triplanarSharpness, macroScale, macroStrength
```

- テクスチャの無い層は、パスが持つ 1x1 の代替 (白 / 平らな法線 / 黒 / 中間灰) の添字を詰める。
  シェーダーは添字の有効判定をしない
- `.mat` の無い層、層 0 枚の地形は «白い既定層» 1 枚として送る (`layerCount >= 1`)
- バッファは CPU 側の直前のバイト列と比べ、違うときだけ `Update` する。層数が変わったら作り直す

### ピクセルテクスチャ枠

| 枠 | 中身 |
|---|---|
| t0 | 番号マップ RGBA8 UNORM (`columns × rows`)。値 × 255 を丸めて番号 |
| t1 | 重みマップ RGBA8 UNORM |
| t13 | 影 |

番号はフィルタできないので **Load で 4 近傍を読み、双線形の重みを CPU と同じ規則で掛けて合算する**。
頂点 `i` は `uv = i / (columns - 1)` にあるので、テクセル座標は `uv * (columns - 1, rows - 1)`。

---

## 3. 画素ごとの合成順

Forward (`Terrain.hlsl`) と GBuffer (`TerrainGBuffer.hlsl`) は `TerrainSurface.hlsli` の
`EvaluateTerrainSurface` を共有する。以前は 2 本のシェーダーに同じ式を手で写していた。

1. **候補の収集** — 4 近傍 × 4 枠 = 最大 16 組を番号で合算し、最大 8 候補にまとめる
2. **自動ブレンド** — `terrainParams.w = 1` のときだけ全層を走査し、高さ帯 × 傾斜帯のマスクで
   候補を加える。混ぜ方は旧実装と同じ (`lerp(塗り, 自動, 最大強度)`)
3. **上位 4 候補** を選んで正規化
4. 候補ごとに **albedo と高さ** を引く (三方向投影の層は 3 面)
5. **高さブレンド** — 高さの高い層が境界を押し広げる
6. 最終重みが残った候補だけ **法線・AO/Roughness** を引く
7. 天候 (`Wetness.hlsli`) → Forward は PBR 直接光 + 環境光、GBuffer は書き出し

### 高さブレンド

Mishkinis の式を N 層へ広げたもの。`depth = heightBlendDepth`:

```
h_i = w_i + height_i * heightBlend_i
ma  = max(h_i) - depth
b_i = max(h_i - ma, 0)
w'  = lerp(w, b / Σb, max(heightBlend_i))
```

`heightBlend = 0` の層だけなら `w' = w` で旧来の線形ブレンドと一致する。
高さテクスチャ (`.mat` の `height`) が無い層は **albedo の輝度** を高さの代わりにする。
既存の地形素材 (Ground / Grass / Sand / Rock) は高さマップを持たないため。

### 三方向投影 (triplanar)

`.mat` の `triplanar = 1` の層だけ。投影は **地形ローカル座標**。
重みは `pow(|Ng|, triplanarSharpness)` を正規化し、側面の重みが 1% 未満なら上面 1 回に落とす
(平地で 3 倍のフェッチを払わない)。側面のタイリングは上面と同じ «1 m あたりの繰り返し数»
(`tiling / ローカル幅`) に揃え、崖で模様の大きさが変わらないようにする。
法線は Whiteout ブレンド。

### マクロ変化 (タイルの繰り返しを隠す)

```
far   = diffuse(uv * tiling * macroScale)            大きく引き伸ばした同じテクスチャ
mean  = diffuse の最小ミップ                          テクスチャ全体の平均色
ratio = clamp(輝度(far) / 輝度(mean), 0.25, 2.0)
albedo *= lerp(1, ratio, macroStrength)
```

平均で割るのは、暗いテクスチャほど全体が暗くなる «ただの乗算» にしないため。
`macroStrength = 0` (既定) なら 1 回もフェッチしない。

### Forward の照明を PBR に揃える

Forward は Blinn-Phong、GBuffer は DeferredLighting の Cook-Torrance で、
同じ地形が経路によって別の艶に見えていた。Forward も `Lighting_PBR_Direct` (metallic = 0) を使う。
GBuffer 側の法線増幅 (×3) と粗さの下限 (0.6) は Deferred 固有の見え方の補正なので GBuffer だけに残す。

### 新しい .mat キー

| キー | 既定 | 意味 |
|---|---|---|
| `height` (テクスチャ) | なし | 高さブレンド用。R が高さ |
| `heightBlend` | 0 | 高さブレンドの効き [0, 1] |
| `triplanar` | 0 | 1 で三方向投影 |
| `triplanarSharpness` | 4 | 投影面の切り替わりの鋭さ |
| `macroScale` | 0.1 | マクロ変化の引き伸ばし率 |
| `macroStrength` | 0 | マクロ変化の強さ [0, 1] |

---

## 4. 穴

セル単位。**三角形を作らない**ことで表現し、シェーダーの clip は使わない。

| 消費者 | 扱い |
|---|---|
| 描画 (本体・影・選択輪郭) | インデックスから省く。LOD の粗いブロックに穴が 1 つでもあれば、そのブロックは LOD0 の三角形で埋める (穴の形を粗くしない) |
| 物理 (`HeightFieldCollider`) | 三角形を BVH へ入れない。`Triangle::index` は穴でも進める (`RefitTransform` が index から格子を逆算するため) |
| NavMesh | 穴セルの標本は «面なし» |
| エディターのレイキャスト | 穴で交差しても貫通して次の交差を探す |
| Fiber | 穴セルから葉を生やさない |
| `GetHeightAt` | 変えない (穴でも補間高さを返す)。穴かは `IsHoleAtLocal` で問う |

clip にしないのは、影・GBuffer・velocity・選択の全パスへ同じ判定を配る必要が無くなるため。
地形の形は CPU が既に三角形として持っているので、そこで省けば全経路に一度で効く。

---

## 5. Fiber を層で制御する

`FiberComponent`:

- `terrainLayer` (既定 -1 = 全面)。0 以上ならその層の重みで草を生やす
- `terrainLayerThreshold` (既定 0.25)。セルの 4 隅の重みの最大値がこれ未満のセルはパッチから省く

Blade は頂点カラーの A に層の重みを入れ、`BuildFiberBlades(..., densityFromVertexAlpha=true)` が
棄却法で根元を間引く。**A を読むのはこの引数が true のときだけ**で、
通常のメッシュでは乱数の消費順も含めて従来と一致する。
内容署名には高さに加えて番号・重み・穴・層設定を含める。

---

## 6. ブラシ

`TerrainSculptOp` は末尾に追加する (保存済みの int 値を変えない)。

| 操作 | 式 | 参考 |
|---|---|---|
| Noise | ローカル XZ の fBm (値ノイズ 4 オクターブ) を `strength * w * dt` で加算。`noiseScale` [m]、`seed` | |
| Thermal Erosion | 8 近傍との高低差が安息角 `talus` を超えた分の半分を低い側へ流す | Musgrave et al. 1989 |
| Hydraulic Erosion | 決定的な乱数で水滴を落とし、勾配を下りながら容量に応じて削る・置く | Beyer 2015 |
| Terrace | `k = floor(h/step)`、`f = frac(h/step)`、`target = (k + f^(1 + 8·sharpness)) · step` へ寄せる | |
| Ramp | 始点と終点を結ぶ線分から `radius` 以内を、線分上の高さ `lerp(h0, h1, t)` へ寄せる。**1 ストローク 1 回**で dt を掛けない | |
| Hole | セル中心がブラシ内なら穴を立てる / 消す | |

- 侵食の乱数は `seed` とストローク内の呼び出し回数から決める。AI バスと人の操作で同じ結果になる
- Ramp は始点をクリックで固定し、離した位置を終点にする (AI は `terrain.ramp` で両端を渡す)

### ハイトマップの入出力

- 入力: 既存の `LoadHeightMapFromFile` (PNG / TGA / DDS)
- 出力: `SaveHeightMapToFile` — 16 bit グレースケール PNG。`unipolar` の意味は入力と対称

---

## 7. 保存形式

### .terrain (TOML) — `format_version = 2`

```toml
[terrain]
columns = 129
rows = 129
cellSize = 1.0
maxHeight = 30.0
chunkSize = 32
heightBlendDepth = 0.2
layerMaterials = ["guid:...", ...]
heightData = [...]
splatIndices = [...]   # 頂点 × 4
splatWeights = [...]   # 頂点 × 4
holes = [...]          # 穴セルの添字 (昇順)。穴が無ければ空
```

- `format_version = 1` (または `splatData` を持つ) ファイルは §1 の移行で読む。次の保存で v2 になる
- 穴は疎な添字列で持つ。大半の地形は穴を持たないので、セル数ぶんの 0 を書かない

### FZTN (バイナリ) — `FZTERRAIN_VERSION = 2`

```
FzTerrainHeader (48 バイト。_pad を holeCount / heightBlendDepth に使う)
layerCount × 256 バイト  レイヤーマテリアルパス
columns × rows × float   heightData
columns × rows × 4       splatIndices
columns × rows × 4       splatWeights
holeCount × uint32       穴セルの添字
```

v1 (層数 × 頂点の密な重み) は読み込み時に上位 4 層へ畳む。

---

## 8. AI バス

| コマンド | 変更 |
|---|---|
| `terrain.inspect` | `layers` を層数ぶん返す。`stats.layerCoverage` も層数ぶん。`holeCount` を追加 |
| `terrain.sample` | `layerWeights` を `[{layer, weight}]` (上位 4 層) で返す。`hole` を追加 |
| `terrain.sculpt` | `op` に `noise` / `thermalErosion` / `hydraulicErosion` / `terrace` を追加。`noiseScale` / `seed` / `terraceStep` / `terraceSharpness` / `talus` |
| `terrain.paint` | `layer` は `0〜layerCount-1` |
| `terrain.setLayerMaterial` | `layer == layerCount` で末尾に追加 |
| `terrain.ramp` (新規) | `start` / `end` (ワールド) / `radius` / `strength` / `falloff` |
| `terrain.hole` (新規) | `position` / `radius` / `erase` |

---

## 9. 方向光の斜面自己影

本体と影は同じカメラ距離から同じチャンク LOD を選ぶ。その上で、Forward と Deferred の
不透明受光面には `ComputeShadowSurface` の receiver plane PCF を使う。
固定の 5 mm bias を法線マップの N·L で増幅するだけでは、メートル単位の CSM texel と
PCF 近傍が示す斜面の深度差を覆えず、遮蔽物が無い地形にも縞・まだらな自己影が出る。

world position の画面微分を材質合成・discard・カスケード選択より前に求め、選択した
方向光の正射影でアトラス UV と深度の微分へ写す。UV の 2×2 Jacobian を逆にして
`g = d(depth)/d(atlas UV)` を求め、各 tap の比較値を
`depth - bias - dot(abs(g), atlasTexelSize) + dot(g, sampleUV - centerUV)` とする。
最後の項はクランプ後の実際の tap 位置を使う。線形比較が tap 内の 4 texel に同じ基準を
渡す残差だけ、最大 1 texel の深度幅で覆う。PCF カーネル全体ぶんの大きな固定 bias は足さない。
境界の次カスケードも同じ world 微分から別の勾配を求めて混ぜる。

逆行列は UV 微分長の積に対する相対 determinant が `1e-4` 以下なら使わない。
非有限値や、1 texel の補正が通常 Z の全深度範囲の 1% を超える面も既存 bias へ戻す。
Deferred では隣接画素の前方距離が中央の 5% を超えて跳ぶと微分を無効にし、輪郭の
別オブジェクトを受光平面へ取り込まない。Forward の fallback 法線は材質法線ではなく
地形の幾何法線を渡す。実際の補正は現在の LOD 三角形の位置微分から求めるため、
元の高さ格子から平滑化した法線とも独立する。

この経路は位置の微分がある不透明表面限定。水面・体積光・Compute Shader は従来の
`ComputeShadow` / 個別の可視性評価を維持する。追加の定数バッファ・GBuffer チャンネルは作らない。

[Microsoft — Cascaded Shadow Maps, per-texel depth bias](https://learn.microsoft.com/windows/win32/dxtecharts/cascaded-shadow-maps#calculating-a-per-texel-depth-bias-with-ddx-and-ddy-for-large-pcfs)
が示す近傍受光面の平面近似と、カスケード分岐より前に微分する契約に従う。

`DeferredEmissionTest.SlopedReceiverAvoidsPcfSelfShadowAndPreservesBlockerShadow` は実シェーダーで
16² の影と 64² の HDR を描き、PCF 半径 2/3 の急斜面に自己影が出ず、別の遮蔽物の影が残ることを確かめる。
旧方式へ切り替えると自己影の比較が失敗するため、補正の回帰を検出できる。
`GreenWare/Tests/Playtests/TerrainShadowAcne.playtest.json` は ShowcaseCoast の 3 視点で影の強度 1/0 を撮影し、
移動・回転後の影も確認する。最後に光と表示設定を戻し、シェーダー診断 0 件を表明する。

2026-10-03 の同じ斜面 ROI では、影の有無による RGB 平均絶対差が 12.84 → 0.16 (8 bit) へ減少した。
4 以上暗くなる画素は 99.65% → 0%。丘上の実際の投影影は残り、移動・回転後も連続している。

---

## 参考資料

- A. Mishkinis, *Advanced Terrain Texture Splatting* (Game Developer, 2013) — https://www.gamedeveloper.com/programming/advanced-terrain-texture-splatting
- B. Golus, *Normal Mapping for a Triplanar Shader* (2017) — https://bgolus.medium.com/normal-mapping-for-a-triplanar-shader-10bf39dca05a
- F. K. Musgrave, C. E. Kolb, R. S. Mace, *The Synthesis and Rendering of Eroded Fractal Terrains* (SIGGRAPH 1989) — https://history.siggraph.org/learning/the-synthesis-and-rendering-of-eroded-fractal-terrains-by-musgrave-kolb-and-mace/
- H. T. Beyer, *Implementation of a method for hydraulic erosion* (TUM, 2015) — https://www.firespark.de/resources/downloads/implementation%20of%20a%20methode%20for%20hydraulic%20erosion.pdf
- Unity Manual, *Terrain Holes* — https://docs.unity3d.com/Manual/terrain-PaintHoles.html
- Microsoft, *Resource binding in HLSL — ResourceDescriptorHeap* — https://learn.microsoft.com/windows/win32/direct3d12/resource-binding-in-hlsl

参照 URL は該当する C++ / HLSL の `@see` にも置く。
