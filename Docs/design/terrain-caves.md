# Terrain の洞窟・張り出し

既存高さ場を維持し、穴で入口の地面を取り除き、Terrain が追加三角形を所有する。追加面は同じ Transform、保存、描画、衝突、地表照会の経路を使う。床・壁・天井を独立した飾りオブジェクトで重ねない。

## データと作成契約

TOML .terrain は terrain_asset.format_version = 3。terrain テーブルの既存 columns、rows、cellSize、maxHeight、heightData、layerMaterials、splatIndices、splatWeights を保持する。

| フィールド | 内容 |
|---|---|
| holes | 穴セルの疎な整数添字。cz * (columns - 1) + cx。セルの2三角形を全LOD・衝突・照会で除外する |
| surfacePositions | Terrain local の [x,y,z,...] 浮動小数点列。単位m。heightDataと異なりmaxHeightを掛けない |
| surfaceIndices | uint32の三角形リスト、0基点。surfacePositions内の頂点番号 |
| surfaceMaterial | 追加面に使う .mat 参照。空は既存Terrain層を使用。指定時は1層triplanar投影 |

両geometry配列が空なら旧高さ場。片方だけ、非有限座標、範囲外添字、退化三角形を拒否する。TOMLとFZTNの失敗は既存出力を変更しない。FZTN書き出しはv3、v1/v2を引き続き読み込み、追加面は空に戻す。v3は既存48byte headerの最後の予約8byteを頂点数・添字数に使い、穴列の後に256byte material path、float3頂点列、uint32添字列を追加する。

三角形のcross(b-a,c-a)は空間側へ向く。床は上、天井は下、壁は洞窟内側。法線はTRS逆転置でworldへ変換する。洞窟の外皮、入口の切断側面、天井の厚みと床の継ぎ目は生成側が作る。自己交差、manifold、地面との継ぎ目の一致は自動修復しない。TerrainColliderを同じObjectに付ける。surfacePositions/Indicesを直接編集した場合はRequestSurfaceRebuild()を呼ぶ。SetSurfaceGeometry()は検証とrevision更新をまとめて行う。

## 床・天井照会

native TerrainSurfaceQuery.hpp:

- TrySampleTerrainWorld(terrain, transform, worldX, worldZ, out): 穴を考慮した高さ場と追加面の最上交点。
- TrySampleTerrainFloorWorld(terrain, transform, referenceWorld, maxDistance, out): 参照Y以下で上向きの面のうち最も近い床。
- TrySampleTerrainCeilingWorld(terrain, transform, referenceWorld, maxDistance, out): 参照Y以上で下向きの面のうち最も近い天井。

位置・距離はworld m、距離は有限かつ0以上。面上の距離0も成功。垂直壁は床・天井に含まれない。false時にnative outは未変更。Physics初期化は不要。旧GetHeightAt/GetHeightLocalは高さ場のclamp付きauthoring scalarを維持する。GetHeightWorld/TrySampleWorldは参照Yを持たない最上面照会。どちらも洞窟内の床への押し戻しに使わない。

ScriptSceneProxy::TryGetTerrainFloorAt/CeilingAt は有効Objectの全Terrainから選ぶ。ScriptTerrainProxy::TryGetFloorWorld/CeilingWorld は自身のみ。

C#公開API (source接続済み、SDK更新待ち):

- ScriptScene.TryGetTerrainFloorAt(Vector3 referenceWorld, float maxDistance, out Vector3 position, out Vector3 normal)
- ScriptScene.TryGetTerrainCeilingAt(Vector3 referenceWorld, float maxDistance, out Vector3 position, out Vector3 normal)
- Terrain.TrySampleFloorWorld(Vector3 referenceWorld, float maxDistance, out TerrainSurfaceSample sample)
- Terrain.TrySampleCeilingWorld(Vector3 referenceWorld, float maxDistance, out TerrainSurfaceSample sample)

C# bridgeは対象4ファイルを明示したユーザー許可後にsourceへ適用済み。native41件通過とは別に、追加のC# packet/native bindingテストはCPU枠調整後にbuild・実行し、SDK catalog/DLLを更新してからconsumerを接続する。C#契約はmiss/非有限値でfalse、out default。無効scopeは既存API同様の例外。

## 衝突と描画

追加面なしは既存HeightFieldCollider。追加面ありは穴を除いた縫合済み高さ場＋追加面のTriangleMeshCollider/BVH。床・壁・天井へexact rayが当たり、開いた入口には当たらない。通常SphereColliderなどの動的bodyのnarrowphaseが同じ三角形を使う。Transformを直接動かすcamera/fish/sharkは自動で止まらないため、ゲーム側が床・天井marginと移動方向のPhysics.Raycastを使う。PhysicsQueryOptions.TriggerMode = QueryTriggerMode.IGNORE。Physics同期後のrayを使い、実移動長に余裕を加える。

現行Physics.SphereCast/OverlapSphereのmesh経路は外接AABB近似で、洞窟空間にもfalse hitする。camera用の厳密なradius sweepとしては使えない。rayのmargin/ringは近似であり、完全な球sweepは別途Physics側実装が必要。

追加面は最大256三角形のpatchへ分け、全XYZのlocal AABBを持つ。CPU頂点経路でcamera/shadow/selectionに同じpatchを渡し、GPU高さ場decoderを使わない。3 LOD全てで同じ面を保ち穴・天井の脱落を防ぐ。追加面に草を生やさない。既存高さ場LODの穴も保持する。

## 水との境界と制限

水深/岸泡atlasは従来どおり各XZの最上面に対する水面の深さ。追加面をbounds/signatureへ含め、穴内では床を拾う。洞窟内の参照Yに応じた床探索とは用途が異なる。水中判定にはWaterの水位/範囲を使い、最上地形Yを空気/水境界に流用しない。この追加geometryは水を排除する密閉solidや水中空気ポケットを作らない。

床照会は追加面数に線形、傾いた高さ場の床/天井照会は全cell走査。upright高さ場の通常照会は既存の定数時間経路。小さな洞窟patchを分散配置する設計を想定。既存NavMeshBakeSystemは高さ場の単層経路を維持し、洞窟の複数階navmeshは未対応。底生AIは参照Yのfloor queryを使い、NavMeshへ洞窟経路が生成されたと仮定しない。

## 再現fixtureと観測点

Scratch/TerrainCaveFixture20261011/Assets/TerrainCaveFixture のterrain/material/diffuse PNGをconsumerの同じAssets下へコピーし、Cave.terrainをTerrainへ割り当てる。PNGは再現用1texel色見本。各ファイルの .meta とGUID参照は統合先の通常importで生成する。fixtureは12m四方、高さ場y=0、入口x=2..10/z=0..4、床y=-6、天井y=-2、back wall z=10。単位scaleのidentity Transformで以下を観測する。

| 参照点/操作 | 期待 |
|---|---|
| (6,-4,7), floor/ceiling距離10 | 床(6,-6,7), normal up／天井(6,-2,7), normal down |
| (6,-5.35,7), floor距離1 | 床-6。+0.65m camera marginを保てる |
| (6,-2.4,7), ceiling距離1 | 天井-2。-0.4m camera marginを保てる |
| XZ=(6,2), top | 入口ramp y=-3。高さ場y=0は穴 |
| ray (6,-4,3)→+Z、距離5 | miss。入口と内部は連続して通行可能 |
| ray (6,-4,7)→+X、距離10 | right wall x=10、距離4 |
| ray (6,-4,7)→±Y、距離10 | floor y=-6／ceiling y=-2 |
| ray (6,1,2)→-Y、距離2 | miss。入口に見えない高さ場床がない |

TerrainSplatTestsには上記照会、sphereの入口通過と壁/床/天井の停止、TOML/FZTN/inline Scene往復、FZTN v1/v2互換と無効データのtransactionテストがある。GPUの実画像と最終consumer/SDK接続は統合担当の利用枠で行う。
