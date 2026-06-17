# Foliage System

## 目的

`FoliageSystem` は Terrain 上へ樹木・大型岩などを配置し、FBX の複数 SubMesh を
それぞれ別の `.mat` で GPU Instancing 描画する。

草・花など単一マテリアルで大量配置する要素は `DetailSystem`、幹と葉のように
複数マテリアルを必要とする大型要素は `FoliageSystem` が担当する。

## コンポーネント

`FoliageComponent` は `TerrainComponent` と同じ GameObject に追加する。
各 `FoliageSpecies` は次の永続データを持つ。

- FBX モデルパス
- `Model::meshes` のインデックスに対応する Material パス配列
- 100 平方メートルあたりの密度
- Scale 範囲、Y 軸ランダム回転、乱数 Seed
- 描画距離
- 配置方式 (`Procedural` / `Stamp`)
- Stamp 方式で配置した Terrain ローカル位置・Y 回転・Scale

配置結果と GPU バッファは再生成可能なランタイムキャッシュであり、Scene へ保存しない。

## 配置

Terrain のローカル XZ 平面を密度から求めた間隔で走査し、Seed 固定のジッターを加える。
高さは `TerrainComponent::GetHeightAt()` から取得し、Terrain の World Matrix で
ワールド座標へ変換する。

Terrain Transform または Species の配置パラメータが変化した場合だけ再 Bake する。

## Foliage Tool

`FoliageTool` は大型植生を個体単位で調整するためのクリック式ツールである。

- 左クリック: 選択 Species を1個配置
- `Shift + 左クリック`: 消去半径内の同 Species を削除
- 配置時の Scale と Y 回転: Species の範囲・ランダム回転設定を使用
- 配置と削除: Undo / Redo 対応

最初の配置時に対象 Species は `Stamp` 方式へ切り替わる。
Stamp は Terrain ローカル空間へ保存するため、Terrain の移動・回転・Scale に追従する。

## 描画

Species ごとに 1 個の StructuredBuffer を作り、各 SubMesh を同じ InstanceBuffer で描画する。
葉の Material は `double_sided` と `alphaCutoff` を利用できる。

現段階では Terrain 全体が 1 バッチのため、描画距離判定は Terrain の外接円を使う粗い判定である。
個体単位の LOD、Billboard、Collider、チャンク単位カリングは後続実装とする。
