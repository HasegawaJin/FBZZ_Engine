<!-- @file    terrain-scatter.md -->
<!-- @brief   地形の保存済み形状と層を使う草木・岩の確定配置。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-04 -->
# 地形から木・岩を確定配置する

2026-11-04 のユーザー承認により、既存 TerrainTool を拡張する確定配置を採用する。2026-11-05 の承認で、メッシュ草・木・石を `.terrainpaint` 素材として共通のミニ Browser から選ぶ。配置結果は `.meshinstances` へ保存する。既存 Fiber 草は別の Paint モードに残す。実行時の自動再配置は行わない。

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

配置はメモリー上で全件生成し、既存 `InstanceSetAsset::Replace` の検証を通す。一括ベイクは atomic write で `Assets/Terrain/Scatter/<guid>.meshinstances` を保存し、.meta 発行・読み戻し成功後だけ target の参照を切り替える。元の template、共有参照、元配置ファイルは変更しない。モデル・材質の参照と LOD は template を継承する。

一回の操作は一回の Undo。Scene 所有者、Component 寿命、現在の参照・設定・配置 snapshot、未保存破棄世代を検査する。他の操作が配置先を変更していたら履歴の位置もデータも動かさない。Undo は参照または配置内容を戻し、追加した Component は除去する。生成した配置アセットは Undo で削除せず、Redo で再利用する。初回ファイルは操作時に保存し、以後の塗り足し・消去は同じ専用ファイルの draft として通常の保存に従う。未保存破棄は最後に保存した内容を再読込し、旧履歴を無効化する。自身の保存に伴う同内容の監視イベントは snapshot を交換しない。書込み失敗後の Scene は未変更で、.meta または読み戻しの失敗時は既に書いたアセットのパスを返す。

再配置は全置換であり、手動編集を混ぜた target には実行前に注意表示を出す。新しい配置アセットに属する ID は格子番号から決まり、元 asset の個体 ID を移植しない。生成後の地形編集で木・岩は自動移動しない。同じ設定で再実行して確定する。

## Paint の範囲編集

Terrain Tool と Map Editing Mode の Paint は Ground Materials・Fiber Grass Layer・Objects・HeightMap Stamp を切り替える。Objects はメッシュ草・木・石の共通入口。Fiber Grass Layer は対象地形の Fiber が参照する保存層を既存の地形ブラシで塗り、消去時は指定した別の層へ塗り戻す。未設定の地形では Paint から既存の草設定操作を呼べる。

`terrain.scatter` に `stroke` を渡すと全置換を範囲編集へ切り替える。 `stroke` は 1〜512 個の world XYZ 点、`brushRadius` は地形の接平面上で scale 適用後の距離 [m]、`erase` は消去。点の円の和を使う。追加は既存と同じ候補格子・条件を使い、既存配置が入る格子には重ねない。消去は円内の配置だけに適用し、標高・傾斜・密度フィルターを使わない。既存配置の ID と姿勢を保ち、追加時に ID が衝突したら未使用の ID を割り当てる。配置先がすでにアセットを参照している場合、その mesh・材質・LOD を引き継ぐ。

ストロークはドラッグ中に範囲を表示し、離したときに 1 件の Undo に積む。変更が無い場合は保存・履歴を作らない。追加候補の探索はブラシに重なる格子に絞る。自分専用の `.meshinstances` は塗り足しでも同じファイルを使い、共有されているファイルや旧 `.fzdata` は新しい専用ファイルへ複製してから編集する。

## Paint Library と素材の作成

Paint → Objects の Paint Library はプロジェクト内の `.terrainpaint` だけを検索・分類する。最高詳細 LOD の全部品を実際のメッシュ・材質で描いたサムネイルを表示し、選択素材を大きな回転プレビューで確認できる。クリックで一素材、Ctrl+クリックまたはチェックで最大16素材を混ぜる。GPU プレビューのキャッシュは最大48件、更新は1フレーム2件まで。

1. **+ Create Paint Asset** を開く。Asset Browser の Create → Terrain Paint Asset も同じフォームを開く。
2. Name、Category、Save Path を設定し、各 Part の Mesh と Material を選ぶ。部品は同じ足元原点で作成する。幹・葉のような別材質は Add Mesh Part で追加する。
3. 必要な場合だけ LOD を追加し、各部品の LOD 番号と降順の Screen Height を設定する。全 LOD に少なくとも一部品が必要。
4. Placement Defaults で間隔 [world m]、密度、大きさ、Yaw、傾斜、法線合わせ、下り向きと埋め込み深度を設定し、**Create Paint Asset** で保存する。
5. サムネイルを選び地形へドラッグする。**Edit Paint Asset** または Inspector で設定を編集し、**Save Paint Asset** で確定する。保存は Undo/Redo に対応し、別操作による変更を検出したら Reload を促す。

Import Existing Definition は旧 `.fzdata`、`.meshinstances`、別 `.terrainpaint` のモデル・材質・LOD だけをコピーする。配置個体は素材へ取り込まない。空のシーンでも素材を先に作れる。

専用素材を含むパレットは、素材ごとの間隔・姿勢・傾斜設定と、ブラシ全体の Density Multiplier・標高・Seed を使う。Weight の正規化比を各素材の密度へ掛け、GUID 順に独立した決定的格子から候補を追加する。既存配置の占有格子には重ねない。間隔が異なるため、Share は個体数の保証ではない。旧素材だけのパレットは従来の共通格子・配合を維持する。

素材の設定変更は次回以降の配置条件に使う。既存結果のモデル・材質・LOD は配置ファイルの snapshot を維持する。素材を変更しただけで、既存の木の形や配置を作り直さない。

## 専用形式の契約

両形式は UTF-8 TOML、`version = 1` の固定スキーマとし、DataAssetFactory へ新しい `.fzdata` 型を登録しない。アセット参照は `guid:` を正本とし `.meta` を随伴する。旧 InstanceSetAsset の `.fzdata` は引き続き読める。

| 拡張子 | 用途 | 主なフィールド |
|---|---|---|
| `.terrainpaint` | 再利用する素材定義 | `format = "fbzz.terrainpaint"`、`name`、`category` (`grass/tree/rock/other`)、`mesh.parts`、`mesh.lodHeights`、`placement` |
| `.meshinstances` | 保存済み配置結果 | `format = "fbzz.meshinstances"`、`terrainPaintRef`、`mesh.parts`、`mesh.lodHeights`、`placements` |

`mesh.parts` は `mesh/material/lod` の配列、`mesh.lodHeights` は最大4段階の閾値。`placement` は上表の spacing、density、min/maxScale、min/maxYaw、min/maxSlope、alignToNormal、alignDownslope、downslopeJitter、embedDepth を持つ。素材に個体配列は含めない。

`placements` は同じ長さの `ids`、`positions`、`rotations`、`scales`。位置・回転・scale は各要素が XYZ の配列で、座標は配置 Component のローカル、回転は Euler [rad]、ID は正の安定 ID。モデル・材質の見た目は配置結果へ保存し、terrainPaintRef は来歴として持つ。未知バージョン・壊れた配列・非有限値は部分適用せず拒否する。通常の配布パイプラインは両形式と `.meta` を Assets の一部としてコピーする。

## Asset Browser の配置パレット

Asset Browser の **Filters → Instance Set** は `.terrainpaint`、`.meshinstances` と旧配置 `.fzdata` を拾う。**All Assets** でフォルダをまたいで表示する。タイルはモデルの最高詳細 LOD の部品を同じ視点から描く。Ctrl+クリックで最大 16 素材を選び、**Paint on Terrain** で既存 Paint に渡す。

**Placement Palette** は素材名を色分けし、相対的な **Weight** と正規化した **Share** を表示する。旧素材だけの場合は間隔・密度をストローク全体に適用し、候補ごとに配合比で素材を一つ割り当てる。専用素材は素材ごとの規則に従う。重み 0 は追加から外れる。Ctrl+ドラッグで追加と消去を切り替える。消去は選択素材で描いた配置に適用する。

**Live Preview** はブラシを当てた範囲の候補点を同じ素材色で表示し、最後に当てた位置はパレットの調整中も参照できる。配置確定と同じ計画から、標高・傾斜・穴・占有格子・配合を反映する。表示は最大 384 マーカーで、全候補数をパレットに表示する。プレビューはシーン編集や保存を行わない。

`terrain.paint_instances` は地形と素材 GUID の組ごとに配置用 InstancedMesh を作る。既存の配置を占有格子として扱い、追加分だけを各素材へ振り分ける。任意の `weights` は `sources` と同順の 0〜100 の数値配列で、省略時は等配合になる。出力は `Assets/Terrain/Scatter/Painted/` 以下へ保存する。全変更の準備が完了した時点でシーンへ反映し、一筆を一件の履歴にまとめる。

## 素材と開始手順

付属の `Assets/Terrain/Alpine/AlpinePine.fzdata`、`AlpineBoulder.fzdata`、`AlpineRock.fzdata` は個体ゼロの配置テンプレート。Asset Browser のパレットに渡せる。一括ベイクでは地形と別の空 GameObject を target に指定し、source にテンプレートを指定する。Pine は幹と葉を共通の足元原点へ作った二 part を同じ配置に描く。

回帰では seed の再現、密度変更時の候補 ID、穴・高度・傾斜・保存層、回転・負/非一様地形 scale に対する実 world query と配置の一致、非有限値・容量超過で出力不変、template/兄弟参照の不変、保存・再読込、一括 Undo/Redo と競合拒否を検証する。
