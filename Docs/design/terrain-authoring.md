<!-- @file    terrain-authoring.md -->
<!-- @brief   既存 TerrainTool で山・草地・岩肌・雪と配置を確定して保存する制作契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-04 -->
# 地形と自然物の制作

2026-11-04、既存ツールを拡張して編集結果を確定・保存する方式を採用。常時の自動再配置や独立 AssetEditor は導入しない。操作の正本は Editor の OperatorRegistry とし、画面と AI は同じ操作を呼ぶ。

1. 対象地形を明示して 16bit HeightMap を取り込む。寸法と高さの変更も同じ Undo にまとめる。
2. 草地・岩肌・雪の三つの材質を設定し、標高と傾斜から保存スプラットを確定する。
3. Fiber は草地レイヤーを参照する。木と岩は既存 InstanceSet / InstancedMesh へ配置を確定する。
4. Sculpt・Paint・Ramp・Hole で形と分布を調整し、必要な範囲の制作を繰り返す。
5. Scene と地形・配置アセットを通常の保存経路で保存する。

## 地形の塗り分け

`terrain.biome.bake` は既存の異なる三層を指定し、高さ場全体のスプラットを置換する。高さと穴・追加メッシュ・材質ファイルは変更しない。標高は world Y のメートル、傾斜は world 上方向からの度数。平滑法線は既存 Terrain の有限差分を逆転置で変換する。

岩は `rockSlope` を中心とする `slopeFade` 幅、雪は `snowHeight` を中心とする `snowFade` 幅で smoothstep 遷移する。幅ゼロは段階的な判定。岩の重みを R、高度による雪の重みを S とすると、草=(1−R)(1−S)、岩=R、雪=(1−R)S。急斜面には雪が張り付かず岩が露出する。既存 Canonicalize で合計255の正準スプラットへ量子化する。

描画時の材質 autoBlend は別の効果なので、確定分布と草を一致させる制作ではオフにする。同梱の Alpine 材質はオフ。確定後の手塗りは保持され、再度 Bake を押したときだけ全体を上書きする。任意の PNG マスクは `terrain.layer.import_mask` で対象層へ混ぜる。白は対象層、黒は現在の分布を保持、invert で反転する。

### 地形の形状マップと密度

Terrain Tool の Shape & Density Maps は、選択地形のローカル XZ 全域を解析する。正の world scale と軸平行な Terrain を対象とし、隣接タイルを含む場合は共通の world XZ 格子で集水を計算する。対象外の欠損は障壁として扱う。穴は黒、PNG 上端は -Z 側、右端は +X 側。Height は解析域の標高範囲、Slope は 0〜90° を 0〜1 に写す。Cavity と Roughness は指定半径で対向する点の平均と中央の差を取り、平均と RMS からそれぞれ凹凸の向きと大きさを求める。Cavity の中間灰は平面、白側は凹み。Flow は集水面積を対数表示し、指定面積で白に達する。地表のない画素は全チャンネルで黒とする。

Density は 5 チャンネルの各係数の絶対値で、白側または黒側の値を 1 からどの程度乗算するかを指定する。0 は条件なし、正は白側、負は黒側を優先する。草地、森林、岩場のプリセットから調整できる。`terrain.maps.preview` は 48×48 の縮小画像を行優先の Gray8 16 進文字列で返す。`terrain.maps.export` は選んだマップをプロジェクトの `Assets/` へ線形 16bit PNG として書き出す。パスの省略時は固有名を割り当てる。`terrain.maps.bake` は選んだマップを指定した既存レイヤーへ混ぜ、1 Undo で確定する。黒と穴に接する頂点は塗りを維持する。

出力の縦横は各軸の地形頂点数と指定解像度の小さい方とし、最大 513×513 標本。Flow の共通格子は各軸最大 256 区間。既定では共有辺でつながる最大 16 タイルを使い、`includeNeighbors` で単一タイルへ切り替えられる。`terrainTiles` は解析する GUID 一覧を明示し、選択地形も含める。解析は明示操作時の CPU 処理。材質・Fiber・PCG は保存レイヤーを参照できる。Trees / Rocks Placement の `densityMap` は出力 PNG を独立した採用率として乗算し、地表の塗りと配置密度を別々に管理できる。

### 流域の詳細化と谷筋の成形

Flow の詳細範囲は world XZ の中心と一辺 [m] で指定する。広域格子に外接するセル境界へ揃え、内部を地形の標本間隔と予算に合わせて最大 512 区間へ細分する。広域グラフから境界を越える外部集水面積と流出口を引き継ぐ。詳細範囲の周囲では広域値へ滑らかにつなぐ。穴と欠損は経路に含めない。詳細範囲が無効の場合は従来の広域 Flow を使う。

`terrain.flow.carve.preview` / `terrain.flow.carve` は同じ詳細グラフから、最低集水面積を超える辺を選び、既存の川床断面で谷と支流を成形する。集水面積で川幅と深さを決め、斜度で露岩と砂利の岸材質を選ぶ。緩い岸では指定量まで堆積させられる。プレビューは最大掘削深、最大堆積高、変更頂点数と保護頂点数を返す。確定は対象タイルの変更を 1 Undo にまとめる。タイル外周、穴、保護レイヤーのある頂点は維持する。最大掘削深と辺・頂点の処理予算を超える場合は確定しない。

### 材質ルールと植生の群落

`terrain.maps.bake_materials` は選択済みの複数レイヤーを、Height・Slope・Cavity・Flow・Roughness の重みとルールの強さで一度に正規化し、既存の正準スプラットへ保存する。道や手塗りの場所は既存レイヤーの合計重みまたは線形 PNG の保護マスクで保護できる。穴の縁を保護する。変更を 1 Undo にまとめる。

Density にはワールド XZ に固定したシード付き群落ノイズと、水際・道からの距離帯を追加できる。滑らかな帯の端で密度を落とす。高木、低木、シダのプリセットは同じ群落スケールとシードを使い、同じ林と空き地に対して異なる遷移帯を作る。水際と道は既存の地形配置用輪郭から取得する。道を指定すると、そのレイヤーの内部の距離は 0 とする。結果は従来どおり PNG・保存スプラット・散布密度に使える。

### タイルをまたぐ流路の確定

`terrain.flow.network.preview` は共通の流路グラフから、複数 Terrain の川床変更と各区間の Spline River を計画する。共有辺で同じ位置の頂点は一つのワールド高さに揃える。接する格子が合わない辺、段差がある辺、または重複するタイルの修復は先に行う。外周・穴・保護層を保ち、共有頂点の一方に保護があれば両方を保護する。支流は同じ高さと幅の下流断面へ接続する。曲がりで水面の帯が折り返す区間は、接続部を短く重ねた複数の帯へ分割する。選択タイルの材質レイヤーはアセット参照で各タイルに対応付ける。

`terrain.flow.network.apply` にはプレビューの `expected` を渡す。Scene 世代・対象と寿命・地形入力・設定を照合してから確定する。地形と水面を一回の Undo にまとめ、Redo は同じ水面 GUID を復元する。Scene View では掘削、堆積、保護位置を色分けする。表示用点群は間引くが、確定には全頂点の計画を使う。

`buildWaterfalls` は、有効な Spline Lake の流出口から同じ排水経路を下る急落を抽出する。湖の水面下に 0.5 m 以上の実地形の窪みがあり、水際から集水条件を満たす辺で流出できることを確認する。流出口の水面は湖水位に揃える。候補の滝口は湖より低く、既存地形と計画水面の両方で `waterfallMinDrop` と `waterfallMinGrade` を満たす必要がある。上下流の Spline River から接続断面を取り、掘削後の地形に水筋が埋まらない場合に Waterfall コンポーネントを生成する。棄却した候補の川は連続した水面として扱う。プレビューでは滝を紫、接続断面を淡い紫で表示し、湖の形や水位が変わったら再プレビューを求める。川床・水面・滝を同じ Undo で確定する。設定は Terrain Recipe に保存できる。[Waterfall authoring](waterfall-authoring.md) を参照。

### 地形制作レシピ

`terrain.recipe.save` と `terrain.recipe.load` は `Assets/` の version 1 `.terrainrecipe.toml` に、Flow、Material Rules、Density の引数を保存する。対象ノード・対象タイル・プレビュートークン・出力先・ワールド上の詳細中心は呼び出し時に指定する。共通のサンプリング設定を複数の節に置くときは同じ値を使う。対象を指定して保存・読込すると材質の参照からレイヤー番号を対応付ける。レシピの読み込みではシーンを変更しない。Editor では選択地形で開いてプレビューでき、AI は返された設定を各操作の引数に使える。

### Scene Viewでのマップ確認

**Shape & Density Maps → Scene overlay** で生成マップまたは保存済みTerrain Layerを選ぶと、対象地形に最大48×48個の標本を重ねる。青は0、黄は1。点にカーソルを合わせると値とワールド座標を確認できる。穴の標本は省く。生成マップは **Generate Preview** の結果を再利用し、対象地形の形状・層・TransformまたはUndo履歴が変わると更新を求める。保存層は塗りや地形形状の世代が変わったときに再標本化する。

**Trees / Rocks Placement → Preview density mask** は指定した配置用PNGを、配置の確定処理と同じ線形サンプラーで読み、同じオーバーレイに表示する。画像はプレビューボタンを押した時点の内容を使う。

## 取り込みと履歴

`terrain.heightmap.import` は node/path/unipolar、任意の columns/rows/cellSize/maxHeight を受ける。寸法のゼロ指定は既存を維持する。変更候補へ画像を読み、全て成功したときだけ適用する。失敗で元地形と履歴を変更しない。画像は色ではなく線形の値として読み、16bit を維持する。

Undo/Redo は Scene 世代と TerrainComponent 寿命を確認し、別 Scene や削除・再作成した Component へ古い編集を適用しない。保存は通常の Scene/terrain 保存と分離する。

## 高さ画像の部分貼り付け

2026-11-05、ユーザー指定の「HeightMap画像を地形の一部へ貼り付け」を Paint の HeightMap Stamp として追加する。対象を選択し、画像・矩形サイズ・回転・高さ・合成方式・強度・縁のぼかしを設定し、Scene View の対象上をクリックして一回確定する。長押しでは重ねない。外枠とぼかし開始位置のプレビューは地表に沿って表示する。

UI と AI は `terrain.heightmap.stamp` を共用する。center は world 座標、width/depth と heightScale/heightOffset は地形ローカルの m、rotation はローカル +Y 軸の度数。画像上端は -Z、右端は +X。画像は既存の線形16bit対応ローダーで貼り付けサイズの格子密度（各軸2〜4097標本）へ再標本化し、回転後の各頂点で双線形補間する。

画像値 p は unipolar で [0,1]、bipolar で [-1,1]、目標値 V = heightOffset + heightScale × p。add は現在高 H + V、replace は V、max は max(H,V)、min は min(H,V) を合成先とする。UI の Anchor to Clicked Height は add 以外でクリック地点のローカル高さを heightOffset に足す。画像範囲内のUV端距離 d = min(u,1-u,v,1-v)、f = clamp(d/edgeFade,0,1)、重み = strength × f²(3−2f)。edgeFade=0 は重み strength のみ。結果を ±maxHeight へ収める。

矩形の外側・他の地形・材質分布・穴・追加メッシュは保持する。編集後の高さは既存 heightData に確定して通常の .terrain 保存対象にし、新しい保存形式や持続的な編集レイヤーは導入しない。不正画像・設定・変換では地形と履歴を変更せず、変化のない適用も履歴へ積まない。Undo/Redo は既存の競合・寿命検証と実行時世代の維持に従い、形状とコライダーを失効させる。

参考は Cygames の [Cyllista Game Engine 公式講演](https://speakerdeck.com/cygames/dainamitukunabian-geng-woke-neng-nisurucyllista-game-enginefalseopunwarudoxiang-kepurosiziyarubei-jing-zhi-zuo-turutomiao-hua-ji-neng) のスライド21（画像ブラシ）と26（矩形のレイヤー化）。本機能は画像の部分貼り付けを FBZZ の確定保存方式へ合わせたもので、同講演の非破壊レイヤースタック全体を再現するものではない。

## 境界

木・岩は [配置の契約](terrain-scatter.md) に従う。独自 Vegetation Component やゲーム作者向け C++ スクリプトは作らない。メッシュ形状を押す Stamp、曲線で編集する道、非破壊の積層地形は別機能であり、既存の球状 Stamp / 直線 Ramp と区別する。

## 地形と岸の成形

### 集水域と流出口

`terrain.drainage.inspect` は指定した Terrain と検索矩形を最大 256×256 区間でサンプリングし、外周から Priority-Flood で排水可能な高さを求める。実際の地形は変更しない。各点から下流へ単一の辺を張り、急降下を優先し、平坦では先に排水が確定した点へつなぐ。逆順に面積を足して集水量を近似する。`minArea` m² 以上の支流・本流の辺と下流 ID を上限付きで返す。`seed` の流出口・最低のあふれ口も返す。穴・欠損を未知の障壁として扱う。各辺の座標、排水高、元の地表高、集水面積を返す。参考は [Barnes et al., Priority-Flood](https://doi.org/10.1016/j.cageo.2013.04.024)。

`control_points.fit_river_drainage` は同じ入力で、seed から検索範囲端へ向かう流下列を既存の Inline/Live 水域へ 1 Undo で設定する。`minArea` に達した点から始め、集水面積に応じて幅と深さを増やす。Priority-Flood の排水高 (`waterHeight`) は流向と湖の流出口の解析に使う。川の水面高 (`riverSurfaceHeight`) は実地形 + 0.05 m を上限とし、上流から下流へ同じか低くなるように伝播する。窪地の出口で地形が水面より高い場合は川床の掘削が必要。Flow Network は掘削上限を超える場合に確定を拒否する。検索矩形外の流域を推測せず、河口の接続は検索範囲を調整するか既存 Water Zone から指定する。1 水域の最大 256 制御点は既存契約に従う。

川の制御点は地形を掘らずに設定する。川底と水面が重ならないよう、川の水面は排水高に 0.05 m を足す。照会ではこの高さを `riverSurfaceHeight` として返す。

`terrain.drainage.inspect`、`control_points.fit_river_drainage`、`control_points.fit_lake_shore` に `terrainTiles` を渡すと、最大 16 枚の Terrain GUID を同じ格子で解析する。配列を渡したときは単体指定より優先する。複数の地表が重なる位置は最上面を使う。全タイルで地表が欠けた位置は未知のまま扱う。256×256 区間の制限は解析全体に適用する。解析結果から設定する川は 1 回につき 1 本。

### 川床と川岸の成形

`terrain.river.carve.preview` と `terrain.river.carve` は、既存の Spline River の水面高・幅・深さを使い、指定した Terrain 1 枚に川床を刻む。中央は制御点の水深まで下げ、岸へ向かうほど浅くする。川の半幅が地形セルの対角線の半分より狭い場合は、差の分だけ掘削を広げる。細い川が頂点列の間を通る場合も、周囲の頂点を下げて地形三角形を川床へ追従させる。水面の外側は `bankWidth` [m] と `bankRise` [m] で掘削の肩を決め、範囲の縁を現在の地形へつなぐ。 `endFade` [m] で両端の掘削を弱める。 `maxCut` [m] を超える切り下げがあれば確定を拒否する。プレビューは変更頂点数、最大切り下げ、保護した頂点数を返す。

川の制御点、地形タイルの外周、穴に接する頂点は維持する。既に目標より低い地形は持ち上げない。掘削は絶対高さを目標にするため、同じ設定で再実行しても掘り下がらない。確定は 1 Undo。タイル境界をまたぐ場合は外周が残るため、共有頂点を両側へ適用する処理が別途必要になる。

`bankLayer` を指定すると、浅い岸から水面のすぐ上までを既存の Terrain レイヤーへ焼く。初期値 -1 では材質を変更しない。植物側は同じ材質を PCG の `includeMaterial` または `excludeMaterial` に指定できる。`protectedLayer` を設定すると、その重みが 0.35 以上の道などの頂点は掘削と塗り分けの両方から除外する。

### 尾根と谷の成形

`terrain.relief.sculpt` はワールド XZ の `start` から `end` に沿って高さ場へ起伏を加える。`height` はワールド m で、正は尾根、負は谷。`leftWidth` と `rightWidth` は進行方向から見た両側の裾幅、`bend` は中央での左向きの曲がり [m]。`peakAlong` は起点から最も高く変化する位置 [0.1,0.9]。両端と裾は smoothstep で高さ変化ゼロへ接続する。主尾根へ幅や向きの異なる支尾根・谷を重ねられる。

直立した Terrain の高さ場だけを、端から 1 頂点内側の範囲で編集する。高さは既存の許容範囲へ収め、穴・塗り・追加メッシュを保持する。1 操作 1 Undo。区間が短すぎる場合や単一操作の候補予算を超えた場合は編集しない。地形タイルの端の頂点は共有辺を保つため変更しない。

`terrain.relief.massif` は `start` / `end` の主尾根に、左右の支尾根とその間の谷を重ねる。`leftWidth` と `rightWidth` で斜面の広がりを別々に指定し、`branchesPerSide` と `seed` で枝の向き・長さ・高さを変える。谷筋には既存の Hydraulic Erosion を `erosion` の強さで適用する。タイルの外周と穴に接する頂点は保護する。結果を 1 Undo で確定する。

### 池の輪郭

`control_points.fit_lake_shore` は有効な Terrain 1 枚の地表を、池の基準水位（Water の高さ、または既存 Spline Lake の基準平面）で切る。`seed` は水がたまる場所の world XZ。省略時は Water の原点。`width` / `depth` [world m] の矩形を `spacing` 以下の格子で照会し、seed に接続した水没域の輪郭を求める。`tolerance` [m] で輪郭を簡略化し、直線区間の Spline Lake 制御点へ変換する。半径や点順で丸く成形せず、地形の入り江や張り出しを残す。

検索範囲の外へ流出する水没域、輪郭に隣接する地形欠損、簡略化で交差した輪郭は診断して拒否する。中島は水面と水中判定が共有するメッシュから除外する。両軸最大 256 区間、輪郭ごとに最大 256 制御点、中島は最大 16 個かつ合計 512 点。既存の Inline/Live の有限矩形水域または湖を対象にし、妥当性を検証してから 1 Undo で確定する。 `useSpillLevel = true` で窪地の流出口から `freeboard`（既定 0.1 m）を引いた高さを水位にする。

### Island.scene の制作案

`Northern Forest Lake` の水位は 139.144485 m。湖底が丸いため、輪郭の取得に先立って東岸に岬、南岸に入り江を成形する。対象地形は `Spruce Island 8km NW Terrain`（`07a3cad4-9641-55e0-8806-a0e7e3c251a3`）。

| `terrain.relief.sculpt` | start XYZ | end XYZ | height | leftWidth | rightWidth | bend | peakAlong |
|---|---|---|---:|---:|---:|---:|---:|
| 東岸の岬 | `[-790,0,2850]` | `[-1050,0,2900]` | 55 | 44 | 68 | 20 | 0.4 |
| 南岸の入り江 | `[-1220,0,2920]` | `[-1260,0,3190]` | -45 | 50 | 28 | -12 | 0.52 |

次に `control_points.fit_lake_shore` へ `node = 61597f94-3cd4-54e0-b7b4-ce092d13f3b2`、上記 Terrain GUID、`seed = [-1100,0,2848]`、`width = 896`、`depth = 768`、`spacing = 4`、`tolerance = 3` を渡す。保存済み地形データを使った試算では、41 点の閉じた輪郭が得られ、中島と自己交差はなかった。配置後は既存の草木と道が水に入っていないか Scene View で確認する。

## 起伏の制作時検査

### タイル境界の高さ

`terrain.seam.validate` は `node` と `neighbor` の Terrain が共有する辺の高さを比較する Query。Terrain Inspector の `Terrain Seam Rules` からも呼べる。正のスケールを持つ軸平行な高さ場を対象にする。隣接を判定する XZ 誤差は 0.01 world m。重なる辺の両端と、両方の高さ場の格子頂点位置を線形補間で評価し、解像度が違っても片方だけの頂点にできた段差を検出する。`heightTolerance` はワールド m、既定 0.05。最大段差、標本数、違反座標を返す。穴・追加メッシュ・法線は高さの比較から外す。参考は [Epic Landscape Technical Guide, Landscape Components](https://dev.epicgames.com/documentation/unreal-engine/landscape-technical-guide-in-unreal-engine) の共有辺頂点。

`maxCrossSlopeDifference` を指定すると、高さが揃っていても境界を横切る斜面に急な折れがある場所を検出する。単位は度、0（既定）で無効。両側の格子点で分けた各区間の中点で、共有辺に接する三角形の勾配を同じワールド軸方向に測る。正負を含む傾斜角の差を比較するため、同じ傾きのまま継がる坂は合格し、左右で逆向きになる尾根は角度差が出る。結果は `slopeSampleCount`、`maxCrossSlopeDifferenceDegrees` と `TERRAIN_SEAM_SLOPE` の座標を返す。

2026-11-10 の `Island.scene` の保存済み 4 タイルでの試算では、共有辺の高さは一致していたが、SW–SE の `(384.0, 43.19, -718.0)` では傾斜が約 +51.23° から −42.10° に変わった。角度差は約 93.33°。高さ場から算出した値で、描画時の見え方は Scene View で判断する。

`terrain.seam.blend` は共有辺の高さが 0.05 m 以内で一致し、辺全体の格子が揃う 2 枚を対象にする。`neighbor` の境界三角形の傾きへ `node` の最初の内側頂点を寄せ、`blendRows`（既定 6、1〜64）行まで smoothstep で補正を弱める。`strength`（既定 1、0〜1）と `maxCorrection`（既定 8 world m、0〜1000）で補正量を制限する。対象外のタイル・他の共有辺に影響しないよう、選んだタイルの外周頂点は変更しない。穴、塗り、追加メッシュを保持し、変更を 1 Undo で確定する。Terrain Inspector からは既定値で実行できる。

同じ保存データを使い、SW を SE に寄せる既定値の試算では、最大の角度差が約 93.33° から 5.06° に下がり、12° を超える境界区間は 89 から 0 になった。地形ファイルに対する編集・保存は試算には含まない。

### 急斜面と孤立した起伏

`terrain.relief.validate` は選択中または `node` で指定した Terrain の高さ場を読み取り、Terrain Inspector と AI に検査結果を返す。傾けた地形と特異なスケールはエラー。直立した地形の yaw と非一様スケールを考慮し、差分をワールド m で測る。

| 検査 | 判定 |
|---|---|
| 急斜面 | 1 セルを描画と同じ対角線で分け、2 三角形のうち大きい斜度が `maxSlopeDegrees`（既定 60°）を超える。 |
| 孤立した突起・くぼみ | 上下左右の 4 頂点すべてから `minIsolatedRelief` m（既定 2 m）以上突出・陥没する内部頂点を返す。 |

しきい値の 0 は各検査を無効にする。穴セルと穴に接する内部頂点を対象外にする。洞窟の追加メッシュとタイル間の継ぎ目は別の形状契約に属する。検査は高さ場を 1 回走査し、全件数と先頭最大 64 件の種別・ワールド座標・斜度または高低差を返す。意図した崖や谷はしきい値を調整し、修正は既存 Sculpt から確定する。川は [川の自然ルール](river-nature-rules.md) で照合する。
