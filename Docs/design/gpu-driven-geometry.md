<!-- @file    gpu-driven-geometry.md -->
<!-- @brief   ビュー所有のGPU可視判定・LOD・間接描画とMesh Shaderの契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-10 -->

# GPU駆動の形状描画

実装順は、水面の同一条件のLOD選択を共有し、GPU可視判定・LOD選択・描画指示の共通基盤を追加し、最後にDX12のMesh Shaderを接続する。Scene/Gameの解像度、TAA、水中の光、既存の描画品質を維持する。SDKへの反映は別の操作とし、この実装では行わない。

## 高密度な静的形状のmeshlet接続

水面の試作では三角形ごとの重複頂点とCPU/RHI同期が増え、通常描画より遅かった。現在のMesh Shader接続は8,192 triangle以上の静的メッシュのGBufferと深度影を対象とし、meshlet内の頂点共有とGPUによる細かい可視判定を使う。小さなcubeや多数の低密度instanceは従来のinstancingを維持する。Skinned、GPU変形、custom surface、透明物は対象にしない。影のalpha cutoutは既存Shader契約を拡張せず通常描画へ戻す。

入力のtriangle順を保って最大64 unique vertex・126 triangleへ分割し、元VBのglobal vertex indexとmeshlet内のlocal triangle indexを別に持つ。Mesh stageは各unique vertexを一度だけ評価する。重複・degenerate triangleは削除しない。既存GBufferVS/PS、ShadowMapVS/PSを共有し、材質、alpha clip、normal、LOD dither、depth biasの契約を維持する。

共通blobは先頭に48 byteのdescriptorをmeshlet数だけ置く。各descriptorはboundsMin/boundsMaxのfloat4二本と、vertexOffsetBytes/primitiveOffsetBytes/vertexCount/primitiveCountのuint四つを持つ。offsetはblob先頭からのbyte、頂点参照はuint、triangleはlocal uint3。raw VBとblobはVS/MS bindless slot1/2を使う。

private b13は112 byteで、world frustum planes六本とmeshletCount/vertexStride/positionOffset/cullingEnabledを持つ。描画と同じcamera/lightのVPからplaneを求める。TAAは同じjittered VPを使い、camera/light切替を混同しない。local AABBをworldのabs linear basisで包み直し、丸めの安全域を付けて見えないmeshletを頂点評価前に除外する。negative/nonuniform scale・shear・両面材質を守るため初期のcone/backface cullは使わない。

分割と転送は初回またはVB/IB内容の更新時だけ行う。cacheはResourceManager所有・有界で、owner/reset、handle世代、内容版を検査する。failure、範囲外、無効なCPU data、非対応deviceは元のindexed描画へ戻る。versionを調べるために毎drawでGPU完了を待たない。性能比較は高密度形状の一部だけが見える条件と全体が見える条件を両方測り、結果を記録して適用条件を判断する。

cacheは全descriptorから求めたlocal AABBもCPUへ保持する。Mesh Shaderモードでも、全体が見える場合、軽微なクリップ、カリング無効、無効な視錐台は通常描画へ戻す。各world frustum planeへの投影半径をsupport、中心のsigned distanceをdistanceとし、`(support - distance) / (2 * support)` がどれか一面で0.25以上のときだけMesh stageを使う。これはAABB厚みのクリップ量の推定で、実際のtriangleの可視割合や任意GPUの高速化を保証しない。比較fixtureのforceMeshだけは全可視もMSへ送り、性能差を隠さない。初回分割・転送の費用はsteady測定へ含めず、assetのoffline生成や事前準備は未実装である。

### 不透明な静的メッシュのクラスタLOD

CPU形状を公開する静的メッシュのうち、8,192三角形以上でモーフを持たないサブメッシュは、インポート中に三角形の重心を空間分割して葉を作る。近接する子の形状をまとめ、元頂点を使うエッジ縮約で親を作る。サブメッシュ間は分けたまま、グループの境界・同一位置の属性分割・非多様体の頂点を固定する。境界の頂点と辺が各段で一致するので、隣のクラスタが別の段を選んでも継ぎ目を保てる。簡略化できない枝は細かい形状のまま残す。

ノードは元形状のローカルAABB、親番号、縮約の各段で累積した最大位置変位を持つ。GBufferのDX12 `mesh`モードで、TAAのジッターを除いた射影と描画解像度から誤差をピクセル換算する。近面を横切る候補は細かく描く。GPUは各候補から親を調べ、誤差が1ピクセルに収まる最上位のノードだけを描画する。非一様スケール・shearはローカルからclipへの行列に含める。通常の`cpu`モードを同じアセットの比較に使える。 [Epic Naniteの階層クラスタの概要](https://dev.epicgames.com/documentation/unreal-engine/nanite-in-unreal-engine)

標準PBRの不透明GBufferを対象とし、葉風・alpha cutout・LOD dither・深度bias・内容更新済みVB/IBはクラスタLODの対象外とする。影とForwardは既存の経路を使う。blobはメッシュに付随するCPUデータとして保持し、DX12で使用するときにだけ既存の有界キャッシュへアップロードする。初期段階は全メッシュレット候補をDispatchMeshし、採用しない候補は頂点評価前に出力ゼロへ落とす。クラスタ遮蔽・候補の圧縮・ページのストリーミングは、岩場での画質・GPU時間・インポート時間の比較後に判断する。

## 所有と入力

Graphicsの共通処理はScene、物理、水の数式を参照しない。候補は保守的なworld AABB、同一VB/IBと材質のbatch、LOD別index範囲、投影サイズの基準を持つ。波と変形を含むAABBは呼び手が供給する。GPUのLOD指標は投影サイズによる格子密度で、任意形状の幾何誤差を保証しない。

CPUの水面選択cacheはRenderPassContextのビュー・フレームに閉じる。surface、変位上限、全LOD設定、morph、LOD camera、viewport、frustumと不可視葉を保持する条件が一致する場合だけ再利用する。媒体atlas用cameraとmain cameraを同一視しない。遠景meshの生成条件は描画パスごとに維持する。

GPU候補、判定結果、引数はビュー・batchごとに借り直す動的バッファを使う。CPUへ結果を読み戻して描画数を決めない。透明物の順序を入れ替えない。不可視候補は描画数0の指示とし、候補順の引数を保持する。容量・有限値・LOD範囲・世代・資源の準備が不正なら、呼び手は既存のCPU描画へ戻す。

## RHI

DrawCallは通常の直接描画を既定とする。StructuredBufferの引数を指定すると、同じ束縛でDrawIndexedの指示をExecuteIndirectする。旧20 byteの指示は維持し、共通GPU選択は先頭に候補のinstance offsetを付けた24 byteを使う。b15の1 DWORD root constantで候補番号を渡し、VSのSV_InstanceIDへ加算する。StartInstanceLocationがSV_InstanceIDへ含まれると仮定しない。通常描画ではoffsetを0へ戻す。Mesh経路はMS/PSのshaderとthread group数を明示し、間接指示はDispatchMeshの12 byteを使う。範囲、alignment、count、shader種別とtopologyを描画前に検証する。

Mesh ShaderはDX12のfeature queryで対応が確認された場合だけ使う。非対応backendはcapabilityをfalseとして既存描画を保持する。共通インターフェースにDX型を公開しない。ThreadedRendererは要求を値として保持し、引数・raw VB/IB・StructuredBufferのGPU寿命とcommand順序を維持する。CSのUAV出力からSRV・INDIRECT_ARGUMENTへの状態遷移をbackendが担当する。

## 水面への接続

既存の有限四分木、2:1の隣接LOD、stitch IB、辺と角の帯域、morph、遠景との分割を正本にする。独立したGPUの投影LODで葉を変更して継ぎ目を壊さない。初期の水面adapterでは、この整合済み候補の可視判定と描画指示をGPUへ渡す。

水面Mesh Shaderの比較用Shaderは同じunit-grid VB、stitch IB、192 byteのパッチ情報を読み、共通Waterの頂点評価とPSを使う。三角形の区画ごとに出力数・総出力byte上限を守る。選択マスク・界面・媒体atlasの位置と被覆が一致することをGPUテストで確認する。下記の実測で通常描画より遅かったため、本番WaterRenderPassのMesh dispatchは取り外した。MESH_SHADERを選んでも水面は通常のindexed/instancingを使い、高密度な静的メッシュと影だけをMesh stageへ送る。

GPU可視判定へ渡す境界は選択済みパッチのaabbMin/aabbMaxを正本にし、モーフの移動余白や遠景stripの拡張をUV矩形から作り直して失わない。既存の可視数・カリング数・三角数は同じ保守的なCPU境界による診断上の推定を維持する。実際のGPU指示はCSが決め、統計のために読み戻さない。間接描画とMesh dispatchを旧GPU Instancing件数へ含めない。

Editorの`render.geometry_mode`操作で`cpu`（通常描画）、`indirect`（水面・地形・大量静的instanceのGPU間接描画）、`mesh`（高密度静的メッシュ・影のMesh Shaderと地形GPU描画）を比較できる。既定はcpu、Play中はruntimeの一時設定とする。`Water geometry path`ログは間接水面の準備成功を確認する用途に残る。GPU profilerは単一passの契約なので、その内部へBeginPass/EndPassを入れ子にしない。対象がカスタムshaderや旧格子のためfallbackしただけのFPSを、そのGPU経路の高速化と解釈しない。

## ラスタライズのトポロジー比較

Editorの`render.raster_geometry_mode`操作で`list`（既定）、`strip`、`indexed_fiber`を切り替える。`strip`は、草のBladeと対応GrassCardのリボン、直接描画する穴なしLOD0地形、固定格子と縫合済みパッチの水面に適用する。地形の穴・粗いLOD・Meshletと、水面のGPU間接描画・スプラインはListを使う。`indexed_fiber`は草の各リボンで頂点を共有するListに切り替える。

地形と水面では既存の面割りと巻き順から描画用インデックスを作る。R32の`0xFFFFFFFF`をStrip間の区切りに使う。草はリボンごとにインスタンス化し、各段の左右の頂点を一度ずつ評価する。計測では同じカメラ・解像度・パスのGPUタイムスタンプを比較する。

2026-11-08にRTX 4070・DX12・Releaseで同じ条件を3回計測した。各回のGPUパス中央値のうち中央の値を示す。草は250,036束の色と影の合計、地形は32×32セルの128パッチ、水面は128×128画素の色パスを計測した。

| 対象 | List (ms) | Indexed Fiber List (ms) | Strip (ms) |
|---|---:|---:|---:|
| Fiber Blade | 1.846 | 2.375 | 1.406 |
| GrassCard | 1.834 | 2.383 | 1.489 |
| Terrain GBuffer | 0.036 | — | 0.036 |
| Terrain影 | 0.036 | — | 0.036 |
| 固定格子の水面 | 0.029 | — | 0.028 |
| 適応LODの水面 | 0.093 | — | 0.109 |

草のStripは1束72→28頂点で、GPU時間がBladeで約24%、GrassCardで約19%短くなった。地形と固定格子の水面では6,144→2,143インデックスになり、GPU時間は同程度だった。適応LODの水面では318,912→177,118インデックスになり、GPU時間は約16%長くなった。 [計測1](../../build/agent/raster-topology-gpu-20261108.log) · [計測2](../../build/agent/raster-topology-gpu-20261108-r2.log) · [計測3](../../build/agent/raster-topology-gpu-20261108-r3.log)

上の適応LODは旧Strip生成の結果。現行は縫合済みの2×2セルの扇をListと同じ面順で走査し、最大3面ずつStripにする。同じ208パッチ・36ドローの描画区間で、`VSInvocations` は元のListと現行Stripともに89,944回、旧Stripでは144,898回だった。インデックスは318,912→225,892、背景コピーを除くGPU中央値は90.112→89.088 µsだった。 [DX12カウンタと再計測](../../build/agent/adaptive-water-reuse-summary-20261108.csv)

現行Stripは隣接グラフを作らず、同じ解像度の適応格子は同一シーン内の水面で共有する。16×16セルの全16縫合パターンのCPU生成時間は1.885→0.054 ms、64×64セルでは28.856→0.682 msだった。バッファのアップロードはこの計測区間に含めていない。 [CPU生成比較](../../build/agent/adaptive-water-reuse-cpu-20261108.log)

## 地形と草木への接続

Terrainの`indirect`経路は共通GPU選択を使う。地形ごとに全チャンク・全LODのIBを連結し、候補順の24-byte間接指示から各パッチを描く。LODの最大格子幅は投影16pxで選ぶ。透視・正射影を区別し、閲覧カメラの位置・投影・解像度を影でも共有する。ライト視錐台は可視判定だけに使う。Fiberを描く範囲の地形はLOD0を維持する。Frustum Cullingを無効にしたときは本体・影ともCPU経路へ戻る。

シーンのTerrainComponentで`farLodDistance`を正の距離（world m）にすると、既存の1・2・4セル間隔に8セル間隔を加える。0または省略時は従来の3段階を使う。最遠LODの候補に入るのは、パッチ中心までの距離が設定値と従来のLOD2開始距離の両方に達し、三角形数が減る場合だけ。GPU間接描画は既存の投影サイズ判定も適用する。色と影は同じ閲覧カメラを基準にする。

8セル間隔でも穴と全パッチ境界の標本を残す。物理・地形照会・地形アセットの高さやスプラットは変更しない。CPUの地形遮蔽証明は最大格子幅に列の端を揃え、境界を証明できない地形では描画用の三角形へ戻る。

GPUのVSは`SV_VertexID`とb15のパッチ番号から座標・UVを作り、高さ標本から法線と接線を中央差分で作る。ForwardとGBufferは既存TerrainSurface、影は同じTerrainGeometryの評価を使う。高さ標本は隣接地形の平均済みの一標本の外周を含む。スカルプト・穴・隣接の再生成でGPU資源を無効化する。穴の形とindex生成は既存CPU契約を正本にし、CPU頂点・LODバッファもfallbackと選択輪郭用に保持する。GPUで四分木全体を生成・均衡化する実装ではない。

### Terrainのメッシュレットと法線コーン

DX12の`mesh`では、地形のForward、GBuffer、深度影にメッシュレットを使える。既存のパッチVBとカメラで選択したLODのIBを使い、穴と境界を含む三角形の順序を保つ。キャッシュはVB/IBの世代と内容版に従う。頂点評価、画素シェーダー、ラスタライザーは対応するインデックス描画と共有する。未対応の描画条件や準備失敗時は既存の描画経路へ戻る。`indirect`は引き続き投影サイズでパッチLODを選ぶ。

各コーンは三角形の幾何法線を包む軸と半角、および全三角形の裏側に置いた頂点を地形ローカル空間で持つ。透視投影は視点からコーン頂点への向き、平行投影は視線方向を使う。Mesh stageは実際に描くカメラまたはライトの視錐台と、全三角形が裏向きになる条件を検査する。境界には安全域を持たせ、反転や不安定な変換、コーンを作れない形状、裏面カリングを使わないラスタライザーではコーン判定を省く。 [meshoptimizer のクラスタ境界](https://github.com/zeux/meshoptimizer/blob/master/src/meshletutils.cpp) · [DirectX Meshlet Cull サンプル](https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/MeshletCull/MeshletAS.hlsl)

Terrainの48 byte記述子はAABBの未使用2枠に、量子化した軸と安全側に丸めた角度しきい値、および中心から頂点までの距離を格納する。キャッシュ生成時に復元した頂点を全三角形の面と照合する。Mesh stageはAABBと同時にコーンを読み、向きが裏側の場合だけ安全域付きの二乗比較を行う。

草は既存FiberのBlade配置・地形層・風・接触・距離LODを使い、木は通常のModel/MeshRendererとLODGroupを使う。別のVegetationコンポーネントや保存形式は追加しない。`gpuInstancing`を有効にした`indirect`で、64個以上の連続する同一のGBuffer/深度影の束はGPUでworld sphereを判定し、128-byte instance行列を詰め直し、20-byte引数のinstanceCountをatomicに生成する。LOD遷移・無効bounds・小さい束・非対応・準備失敗ではCPU描画を使う。透明物の束やVelocity/ObjectMaskにはこの順序を入れ替える処理を使わない。

instanceの距離・画面サイズ・遮蔽・LODGroupの選択は既存CPU契約を維持する。GBufferの対象物体はCPUで視錐台外を除外せずGPUの候補へ渡し、小さい束や準備失敗では同じ境界をCPUで判定してfallbackする。GPU化したのは束内の視錐台可視判定・圧縮・描画数で、全シーンのLODがGPUへ移ったとは扱わない。Scene/Game・光源面・frameごとに別のバッファを借り、GPU結果をCPUへ読み戻さない。GPU間接instanceを旧CPU-instancing件数へ加えない。三角数の既存統計は候補数の推定で、GPU可視数の実測ではない。

## 採用と検証

2026-10-10の地形・instance追加後は、関連CPU回帰71件と実DX12回帰15件が合格した。後者は新規4件と既存GPU選択3件・meshlet8件で、地形2パッチと全3LODの位置・法線・接線・UV、130 instanceの圧縮と不可視・frame/view分離、入力不正・小さい束のCPU fallbackの画素一致を扱う。GPU結果は`Scratch/GpuTerrainVegetation/GpuTests.json`、CPU結果は`build/agent/test-20261010-132038-11668.log`。最終compile checkは4単位でerror/warningとも0（`build/agent/check-20261010-132133-37120.log`）。性能比較用の既存disabled testは実行していない。今回の追加をFPS改善の実測とは扱わず、SDKへは反映していない。

GPU駆動とMesh経路を切り替えて同じcamera・frame条件で比較する。フレームごとのCPU/GPUの時間と完成したEditor全体の実時間を記録し、速くなると測定できた経路だけ既定候補にする。技術の追加だけを120FPS達成として扱わない。

CPUテストはcache条件差・ビュー寿命・無効入力・候補範囲、GPUテストは可視判定・LOD・間接指示の実出力・Meshと既存描画の一致を扱う。ReleaseのAgentBuildを一度に一本だけ使う。

参照: [Microsoft Mesh Shader仕様](https://microsoft.github.io/DirectX-Specs/d3d/MeshShader.html)、[GPU cullingとLODの公式サンプル](https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-mesh-shader-samples-win32/)、[MeshShaderTier対応判定](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options7)。

## 2026-10-10の接続と実測

水面LODの共有、GPU可視判定・最大4LODの選択・24 byteの順序付き間接指示、DX12 MS/PS経路を接続した。水面はCPUで均衡化した1LODの候補をGPUへ渡し、独立したGPU LOD選択は汎用fixtureで検証する。以下は水面Mesh dispatchを取り外す前の比較である。この水面比較時点では有限四分木全体のGPU生成・2:1均衡化とTerrainの本番adapterは未実装で、一般Modelの高密度静的adapterは冒頭の契約で追加した。

RTX 4070・DX12・ソースRelease、NightLightsLabの1,000灯・8標本、1904×893、TAAと水中光を有効、Scene/Gameを毎フレーム描くhidden batchで比較した。各方式は64フレームのwarmup後に120フレームの完成したEditor実時間を計測する。単一の最終比較であり、GPU時間の値は5回のsnapshotの中央値である。

| 方式 | Editor中央値 | p95 | p99 | 最大 | 中央値換算FPS | WaterForward CPU | WaterForward GPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| 通常描画 | 9.521ms | 11.119ms | 11.807ms | 13.469ms | 105.04 | 0.078ms | 0.552ms |
| GPU間接描画 | 14.256ms | 15.109ms | 15.637ms | 15.737ms | 70.14 | 1.408ms | 2.126ms |
| Mesh Shader | 10.234ms | 11.573ms | 12.144ms | 12.328ms | 97.71 | 0.554ms | 0.681ms |

GPU間接経路は33候補、Mesh経路は4×33 groupを準備したログがあり、各実行のrenderer errorは0で正常終了した。フォールバックだけを計測した比較ではない。通常描画が最速のため、geometryDrawModeの既定はCPUを維持する。120FPSには未達である。GPU間接経路の同期するTryDispatchはRHIとの並行性を減らし、見えない候補も転送するため、この水面ではCPU/GPU両方の負荷が増える。Mesh経路の初期実装は三角形ごとに頂点を複製し、共有格子の頂点再利用を失う。今後のGPU validation/preparationの非同期化とmeshlet内の頂点共有を調べる根拠であり、Mesh Shader一般の性能を結論付ける数字ではない。

同じ共有ツリーで別のEditor/RHI最適化も進行したため、以前の100〜102FPSとの変化をLOD共有だけの独立効果と解釈しない。比較ログは`Scratch/NightGeometryCpuFinal/report.json`、`Scratch/NightGeometryIndirectFinal/report.json`、`Scratch/NightGeometryMeshFinal/report.json`と、各ディレクトリの`console.txt`。GPU profilerの入れ子を試した途中の比較は全passの記録が無効になったため、GPU時間の採用比較から除外した。

CPU回帰37件、実DX12回帰146件が合格した。共通GPU fixtureでは候補順・不可視候補・投影LOD・複数のinstance offset・別ResourceManagerと失敗時の未変更を、Water fixtureではstitchとmorph、2つの波時刻、透視/正射影、direct/indirect Meshを既存indexed出力と比較する。EditorLauncher・GraphicsStandalone・GraphicsAutoのReleaseビルド、C++のcompile checkはエラー・警告0。最終テストのログと結果は`build/agent/test-20261010-112249-40940.log`と`build/agent/water-reference-gpu.json`。SDKには反映していない。

## 高密度静的meshletの最終比較

RTX 4070・DX12・ソースReleaseで、256/512 cellsの平面（131,072/524,288 triangle）を一つのstatic meshとして比較した。対象は16×16のdepth targetへのGBuffer shaderとShadow shaderで、pixel負荷を小さくしてgeometryの費用を測る。12 frame warmup後の48 frameを測定し、各条件45〜48件の有効なGPU timestampを得た。値はclear・geometry Submit・target restoreのGPU中央値であり、完成したEditorのFPSではない。初回cache生成、Present、query resolveはGPU測定外。CPU Submitは実際のdraw adapterとcache hitを含む。

以下は524,288 triangleの最終測定。全可視や可視面積1/2もforceMeshでMSへ送り、遅くなる条件を隠していない。本番はforceMeshを使わず、前述のクリップ量条件でindexedへ戻す。

| 見える面積 | GBuffer indexed | GBuffer MS | Shadow indexed | Shadow MS |
|---|---:|---:|---:|---:|
| 全体 | 2.152ms | 4.108ms | 2.241ms | 2.882ms |
| 1/2 | 2.179ms | 2.323ms | 2.439ms | 1.899ms |
| 1/4 | 2.385ms | 1.438ms | 2.225ms | 1.042ms |
| 1/16 | 2.342ms | 0.446ms | 2.461ms | 0.390ms |
| 1/64 | 2.152ms | 0.164ms | 2.214ms | 0.125ms |

1/4は約40〜53%、1/16以下は約81〜94%のGPU時間減少だった。1/2ではGBufferとShadowの傾向が揃わず、全可視は遅い。初回比較でも大きな部分クリップの利点を確認したが、全可視GBufferは初回2.462ms・最終4.108msと変動した。同時に動くEditorや他の作業のGPU負荷・clockを制御しておらず、厳密な単独環境の性能保証には使わない。最終40 workloadは`Scratch/MeshletGeometryBenchmarkFinal.json`、初回24 workloadは`Scratch/MeshletGeometryBenchmark.json`にtimestamp標本とCPU時間も保存する。

新規GPU回帰8件は、本番adapterのbindings/counts/画素、全可視・軽微なクリップのindexed fallback、大きな部分クリップのMS選択、全GBuffer attachmentとdepth、負・非一様scale・shear、無効/無効化frustum、VB/IB更新、資源解放・Resetを確認した。既存GPU146件と合わせて154件が合格し、CPU回帰55件も合格した。比較用disabled benchmarkは通常の回帰件数へ含めない。最終Releaseビルドは`build/agent/build-20261010-131550-32760.log`、コンパイル検査は`build/agent/check-20261010-131353-12624.log`と`build/agent/check-20261010-131538-43608.log`、CPU結果は`build/agent/test-20261010-132121-9296.log`、GPU結果は`build/agent/meshlet-regression.json`と`build/agent/water-reference-gpu.json`。

利用はEditorの`Geometry Draw Mode`操作で`mesh`を選ぶ。この比較の対象は高密度のstatic GBufferとdirectional/spot/point shadowで、水面、小さいmesh、全可視、custom、skinned/変形、透明、非対応GPUは通常描画を使う。Terrain adapterの条件は上記の地形節に記載する。offline meshlet asset生成は未実装。

## Standaloneの設定

ProjectSettingsの`[render].geometryMode`とRenderPipelineAssetの`geometryMode`は`cpu`、`indirect`、`mesh`を保存する。未指定は`cpu`。実行時の解決でもこの値を維持し、Editorの一時操作だけに依存しない。

## 変形済み群れの間接描画

同一mesh/material/bindingsの連続した変形済み群れは既存144-byte行を入力として使う。64行ずつの安定prefix scanで可視個体を順序を変えず圧縮し、GPUがinstanceCountを生成する。入力上限4096、32行未満・無効bounds・非対応shader/backendは従来描画へ戻す。透明・Velocity・LOD fade・prepared skinningは対象外。最初の接続は既存CPUの個体可視判定を維持する。

Computeとindirect drawは値を所有する一つのRHI命令へまとめ、producer側のTryDispatch待機を省く。native validation失敗時は元の入力を直接描画する。Managerは通常Submitと同じframe寿命、Scene/Componentは借用しない。nativeのハンドル検証、状態遷移、fence寿命を省かない。実行をRender::GpuIndirect::NativeDraw、縮退をNativeFallback scopeで観測する。

geometryMode = "instances"は通常の水面・地形LODを維持し、instanceの間接描画だけを有効にする。既存のindirectは従来どおり水面・地形も切り替える。両者は別条件で比較する。

静的instanceのGPU判定も同じ所有命令にまとめる。producer側で個体ごとに静的VBの内容版を取得せず、間接drawのindex範囲はnative実行直前に検証する。既存の同期Prepare APIは変更せず、SubmitStaticだけがqueue受理を返す。native validationが成立しない資源はGPUへ渡さない。

永続 `InstanceSet` は同じ色ビューの選別要求をまとめて記録し、1 回の待機で要求ごとの成否を受け取る。描画と材質定数は元のセット・パーツ順で提出し、記録に失敗したセットは CPU 選別で描く。Hi-Z の深度集約は段ごとに定数を保持して一括記録し、失敗した段に依存する処理を止める。水面は定数更新・選別・間接描画を元の透明描画順でキューへ積み、選別を記録できなければ同じ水面グループを直接描く。

影パスはすべての有効なライトビューについて、地形と `InstanceSet` の選別をアトラス描画前に記録する。太陽光のカスケードはセット内の配置と共通 LOD を一度評価し、カスケードごとの錐台と出力領域へ振り分ける。Spot と Point の錐台・引数も独立に保持する。記録結果を受け取った後に各タイルのビューポートとライト定数を束縛し、元の描画順を保つ。無効化された選別や失敗した要求はタイル単位で既存の CPU 経路へ戻す。
