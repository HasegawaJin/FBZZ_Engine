<!-- @file    terrain-scatter.md -->
<!-- @brief   地形の保存済み形状と層を使う草木・岩の確定配置。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-04 -->
# 地形から木・岩を確定配置する

2026-11-04 のユーザー承認により、既存 TerrainTool を拡張する確定配置を採用する。新しい Vegetation 型や実行時の自動再配置は作らない。草は Fiber の保存済み地形層、木・岩は既存 InstanceSetAsset と InstancedMeshComponent を使う。

`terrain.scatter` は、`terrain` の高さ・穴・保存済み層と、`source` の InstanceSetAsset に保存された mesh/material/LOD を使い、`target` の配置を置き換える。`source` が空なら target の現在の InstanceSet を使う。target は既存 GameObject で、InstancedMesh がなければ追加する。木・岩を別 target に分ければ条件を独立して調整できる。

## 条件と座標

| 引数 | 既定 | 契約 |
|---|---:|---|
| spacing | 6 | 地形ローカル X/Z 軸の scale 適用後の候補格子幅 [m]。0.25〜10000 |
| density | 1 | 候補の採用率 [0,1]。指定層の重みも乗算 |
| seed | 1 | 0 以上の再現用整数 |
| minHeight / maxHeight | -100000 / 100000 | world 標高 [m]、両端を含む |
| minSlope / maxSlope | 0 / 40 | world 上向きからの傾斜 [度]、0〜90 |
| layer | -1 | 保存済み層番号。-1 は層制限なし |
| layerThreshold | 0.25 | 指定層の最小重み [0,1] |
| minScale / maxScale | 0.8 / 1.2 | モデル原寸へ掛ける world scale、0.01〜100 |
| minYaw / maxYaw | 0 / 360 | 上向き軸の回転 [度]、-360〜360 |
| alignToNormal | false | true は地形の world 法線へモデルの +Y を揃える |
| alignDownslope | false | モデルの +Y を world 法線、+Z を最急降下方向へ揃える。ほぼ平坦な場所では通常の yaw を使う |
| downslopeJitter | 0 | 下り方向を基準に、seed で決める左右の振れ幅 [度]。0〜180 |
| embedDepth | 0 | 採用した地表点から法線の逆向きへ移す距離 [world m]。0〜100 |
| maxInstances | 100000 | 1〜1000000。超過時は全体を拒否 |

地形 X/Z の格子ごとに一点を置き、seed と格子 ID から位置・採用率・回転・scale を決める。点は格子幅の 15〜85% に揺らす。spacing は最小個体間距離ではない。格子・seed が同じなら density や高度条件の変更で残る個体の位置・回転・scale は変わらない。

高さは描画・物理と同じ二三角形の `TerrainComponent::GetHeightAt`、法線は既存の補間法線を逆転置で world へ変換する。穴には配置しない。洞窟・追加 surface の三角形はこの操作の対象外。層の重みは保存 splat の双線形補間で、材質 shader の自動ブレンド結果は読まない。草木を同じ領域へ生やす場合は、先に条件を保存層へベイクする。

地形は回転、非一様・負 scale に対応する。散布の直前に Transform を同期し、同じフレームの編集も反映する。配置先は正の一様 worldScale のみ受け付ける。非一様 scale の親の下で world 法線へ回転すると shear を必要とし、InstancePlacement の TRS で保存できないためである。モデルの足元を原点にしておく。

斜面の石は Terrain Authoring の Orientation を Downslope にする。重力方向を地表の接平面へ投影して +Z を決め、法線を軸に Downslope Jitter の範囲で回す。Embed Depth は標高・傾斜・層の採用判定後に適用するので、埋め込みの調整で候補 ID や採否が変わらない。

100 万候補、指定個体数、地形の 4097×4097 上限を編集前に検査する。NaN、非可逆 Transform、上下限逆転、壊れた地形配列は拒否する。上限超過で先頭部分だけを配置する動作は持たない。

## 保存と履歴

配置はメモリー上の独立 InstanceSetAsset に全件生成し、既存 `Replace` の検証を通す。既存 reflector と atomic write で `Assets/Terrain/Scatter/<guid>.fzdata` を保存し、.meta 発行・読み戻し成功後だけ target の参照を切り替える。元の template、共有参照、元配置ファイルは変更しない。モデル・材質の参照と LOD は template を継承する。

一回の操作は一回の Undo。Scene 所有者、Component 寿命、現在の参照・設定・配置 snapshot、未保存破棄世代を検査する。他の操作が配置先を変更していたら履歴の位置もデータも動かさない。Undo は参照を戻し、追加した Component は除去する。生成した配置アセットは Undo で削除せず、Redo で再利用する。配置アセットは操作時に保存済みで、Scene の参照変更は通常の Scene 保存に従う。書込み失敗後の Scene は未変更で、.meta または読み戻しの失敗時は既に書いたアセットのパスを返す。

再配置は全置換であり、手動編集を混ぜた target には実行前に注意表示を出す。新しい配置アセットに属する ID は格子番号から決まり、元 asset の個体 ID を移植しない。生成後の地形編集で木・岩は自動移動しない。同じ設定で再実行して確定する。

## 素材と開始手順

付属の `Assets/Terrain/Alpine/AlpinePine.fzdata`、`AlpineBoulder.fzdata`、`AlpineRock.fzdata` は個体ゼロの配置テンプレート。地形と別の空 GameObject を用意して target に指定し、source にテンプレートを指定する。Pine は幹と葉を共通の足元原点へ作った二 part を同じ配置に描く。

回帰では seed の再現、密度変更時の候補 ID、穴・高度・傾斜・保存層、回転・負/非一様地形 scale に対する実 world query と配置の一致、非有限値・容量超過で出力不変、template/兄弟参照の不変、保存・再読込、一括 Undo/Redo と競合拒否を検証する。
