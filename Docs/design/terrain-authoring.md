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
