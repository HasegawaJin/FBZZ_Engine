<!-- @file    README.md -->
<!-- @brief   山・草・木・岩・雪を組み立てる Alpine オリジナル素材の仕様と使い方。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-04 -->
# Alpine 地形スターター

山脈、谷、峠の **16bit HeightMap PNG 3 種**、草・森林・岩・雪の分布マスク、地形用材質、針葉樹・大小の岩の散布テンプレートをまとめた制作素材です。独自の数値生成・メッシュ制作による素材で、第三者の地形やゲームデータは含みません。

`Alpine_Preview.jpg` は高さと分布から作った説明図、`Alpine_PropsPreview.jpg` は簡易モデルの形状確認図です。どちらも Engine で撮影した画像ではありません。木と岩は形状や配置をすぐ試すためのローポリ素材、地形材質は 256² の簡易的な反復テクスチャです。

## 最初の地形

**Tools > Terrain Tool** または Map Editing Mode を開き、共通の **Terrain Authoring > Target Terrain** で対象地形を選びます。**1. HeightMap Import / Export > Alpine Starter (replace terrain)** が出発点です。パスが空なら `AlpineRange_Height16.png` を使います。対象の高さ・寸法・レイヤーを置き換え、穴は重なる格子範囲で保持します。作成結果はブラシで編集でき、操作履歴から戻せます。

既存の HeightMap 入力欄を使って個別に読み込む場合は、先に次を設定します。PNG 読み込みだけでは地形の頂点数・高さ倍率・幅は変更されません。

| 設定 | 値 |
|---|---|
| Columns / Rows | 513 / 513 |
| Cell Size | 4 m |
| Max Height | 640 m |
| 高さモード | Unipolar [0 → maxH] |
| Transform | 原点・回転なし・Scale 1 を出発点にする |
| 地形の幅と奥行き | 2048 m × 2048 m |

| 高さ画像 | 特徴 | 高度の範囲 | 生成 seed |
|---|---|---:|---:|
| `AlpineRange_Height16.png` | 二本の尾根と連なる山頂、谷と裾野 | 約 29–590 m | 20261104 |
| `GlacialValley_Height16.png` | 山脈に挟まれた広い谷底 | 約 30–605 m | 20261105 |
| `HighlandPass_Height16.png` | 尾根の間をつなぐ鞍部と峠 | 約 24–565 m | 20261106 |

画像の左から右が地形ローカル +X、上から下が +Z。黒が Y=0、白が Y=640 m です。元 PNG はリニアな 16bit グレースケールで、各画像に約 49,000–54,000 段階の高さがあります。8bit カラー保存への変換は段差の原因になります。読み込みは PNG 本体を使い、色のプレビュー画像を HeightMap 欄に入れないでください。

## 草・岩・雪の地面

スターターの地形層は **0=Grass、1=Rock、2=Snow**。材質は `Grass.mat`、`Rock.mat`、`Snow.mat` で、すべて同じフォルダー内の Color / Normal / Height PNG を参照します。自動ブレンド表示は無効で、ベイク済みの層重みを描画するため、草や配置条件と同じ地面を編集できます。

**2. Grass / Rock / Snow > Bake Terrain Layers** で高さと傾斜から層へ塗り分け、そのあと Paint / Smooth / Erosion / Ramp で局所的に調整します。Snow Height は world Y [m]、Rock Slope は傾斜 [度] です。既に手塗りした重みへ全面ベイクを繰り返すと上書きされるので、細部調整の前に大まかな形と雪の高さを決めます。

各 HeightMap と同名の `*_GrassMask.png` / `*_ForestMask.png` / `*_RockMask.png` / `*_SnowMask.png` は、白が強く黒が弱い **8bit リニア分布画像**です。同セクションの **Layer Mask PNG > Import Layer Mask** (`terrain.layer.import_mask`) で対象の層へ取り込めます。白が対象層へ混ざり、黒では既存の塗りが残ります。単なる高さ帯だけでなく、斜面と緩やかな斑を含めています。元 HeightMap と同じ解像度・配置で使います。

| マスク | 分布の目安 |
|---|---|
| Grass | 低い場所。280–385 m で減り、24–43° の斜面で減る |
| Forest | 低地と裾野。240–345 m で減り、28–43° の斜面で減る。斑で疎密を変える |
| Rock | 28–48° の斜面で増え、300–490 m の高地にも出る。積雪面で減る |
| Snow | 約 370–500 m の高地。38–64° の急斜面で減る。境界は斑で少し揺らぐ |

`*_LayersRGBA.png` は参照用の 4 チャンネル分布です。**R=地面、G=草、B=岩、A=雪**で、Forest は含みません。これは `.terrain` の `splatIndices/splatWeights` のバイト列とは異なり、RGBA のまま既存スプラットへ直接置き換える形式ではありません。通常の塗り分けには個別マスクを使います。Forest は草の層へ重ねず、森林用に別の層を追加するか木の散布条件の参考にします。

## 草を生やす

**Enable Grass on Layer** (`terrain.grass.setup`) で Terrain と同じ GameObject に Fiber を用意します。既定の `GrassCards.mat` は、透過 PNG `GrassCards_Color.png` の草束を幅 50 cm の交差ビルボードに貼り、根元を留めて風で揺らします。密度は 4 束/ローカル m²、表示距離は 80 m、草の層は 0 です。既存の地形に適用する場合も同じボタンを押します。

草の丈、色、揺れの大きさと速さは材質、密度・幅・距離は Terrain Tool で調整できます。1 本ずつの葉を使うときは `GrassBlade.mat` と幅 0.035 m を指定します。

草のデータは、各カメラの表示距離に入る地形パッチから生成します。移動先では同じ seed から同じ草を再現し、地形の一部分を編集するとその周辺を更新します。距離制御を無効にすると地形全体が対象になります。制作時の上限は 1 パッチ 32,768 本、1 ビューの候補範囲で 500,000 本です。複数ビューと直近 2 フレームのデータを保持するため、合計使用量は各ビューの範囲と移動量でも変わります。

## 木と岩を置く

| 散布テンプレート | 内容 |
|---|---|
| `AlpinePine.fzdata` | 約 8.9 m の針葉樹。幹と葉の 2 パーツ、計 1,152 三角形 |
| `AlpineBoulder.fzdata` | 高さ約 2.4 m の大岩、224 三角形 |
| `AlpineRock.fzdata` | 高さ約 1.25 m の小岩、224 三角形 |

**3. Trees / Rocks Placement** の Placement Object に散布先 GameObject を指定し、Model template に上表の `.fzdata` を指定して **Bake Placements** を使います。木・大岩・小岩ごとに別の GameObject にすると、密度と再配置を独立して扱えます。テンプレートは配置数 0 の原本です。散布結果は別の配置アセットへ保存し、元テンプレートは空のまま残します。木は低地・緩斜面・草の層、岩は岩の層を出発点に、seed、密度、縮尺、回転を調整します。雪の上や道に不要な物体が残ったときは、その分布条件を直して再配置します。

散布テンプレートは、すぐ読み込める **FZMH v2 `.mesh`、m 単位、Y 上、足元原点** を参照します。生成済みメッシュは Engine の規則に合わせてフォルダー内のパスで参照します。幹と葉は共通の原点を持つ別メッシュです。LOD は 0 のみ、衝突形状は含みません。

編集用 OBJ 原本も同梱しています。OBJ は **Y 上・足元原点・cm 単位**。FBZZ の DCC インポートの既定倍率 0.01 により上表の m 寸法になります。他の DCC では cm として取り込んでください。原本を編集した後は Editor の通常モデルインポートを行い、新しいモデルへ配置テンプレートの参照を差し替えます。一体の `AlpinePine.obj` と `AlpineProps.mtl` も含みます。

## 丘陵の森林を作る

`ForestHills_Height16.png` は森林用の緩やかな丘です。513 × 513 頂点、格子間隔 1.5 m、Max Height 40 m、Unipolar で取り込むと、768 m 四方、高さ約 4〜33 m の地形になります。

Alpine Starter で材質を用意し、この画像を指定の寸法で取り込み直してから塗り分けます。小道を出すには `ForestHills_TrailMask.png` を岩の層へ取り込みます。草は層 0 に密度 5 束 / m²、表示距離 85 m。木は `AlpinePine.fzdata` を使い、層 0 に間隔 8 m、採用率 0.87、縮尺 1.6〜2.35 で配置します。元の針葉樹はこの設定で高さ約 14〜21 m になります。

近くの木にも揺れを付ける場合は、`ForestPineSpecimen.fzdata` を参照する Instanced Mesh と Sequence Player を同じオブジェクトに付けます。地面に原点を合わせ、`ForestPineSway.sequence` と **Play On Awake** を設定すると Play 中に根元を支点に揺れます。シーンの環境風は草に使い、木の揺れはこのループで調整します。

## 保存と検証

HeightMap PNG が元の数値素材です。編集した地形は Scene に保存できます。地形を独立した素材にするときは Inspector の地形アセット保存を使います。木と岩の配置は結果の `.fzdata`、参照する材質は `.mat` に保存します。PNG の再読み込みは現在の彫刻結果を置き換えます。

このフォルダーの `.meta` は参照 GUID を保持します。別プロジェクトへ移す場合はフォルダーごとコピーし、HeightMap、PNG、OBJ、材質、テンプレートと `.meta` を一緒に移します。草の材質は共通 Engine シェーダーを使います。

PNG の bit depth・解像度・画素の往復一致、材質とテンプレートの参照、OBJ の非退化三角形と足元原点を確認済みです。DX12 のエディターで Alpine Range を取り込み、既定の草、木と岩の配置、シーンの保存・再読み込みを確認しました。数値の再現情報は `Manifest.json` にあります。
