<!-- @file    instanced-mesh.md -->
<!-- @brief   任意メッシュの共有個体管理・材質・Fiberと地形LOD・実測の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-10 -->
# Instanced Mesh の共通インスタンス

ユーザーの2026-10-10の指示により、既存Scene向けの自動batchに加え、大量配置専用のInstancedMeshComponentを導入する。木・草メッシュ・低木・花・岩は同じ管理を使う。FiberはShell/Fin/Bladeの形状評価・風・接触を維持し、同じ物体へ二重に配置しない。CPU/Indirectの別コンポーネントは作らない。

## 作者データと所有

配置結果は専用 `.meshinstances`、Terrain Paint の再利用素材は `.terrainpaint` を使う。旧 InstanceSetAsset の `.fzdata` は読み込み互換を維持する。内部の InstanceSetAsset 値は mesh/material参照、partのLOD番号、最大4段階のscreen-height閾値、stable ID、local位置・Euler回転[rad]・scaleを持つ。全partは同じ個体配列を共有する。参照はguidを正本にし、primitiveも既存MeshResolverを使う。配列の長さ不一致・ID重複・非有限値・不正LOD・過剰個体数は診断して描画を拒否する。共有assetへの変更は全参照へ反映する。専用形式と作成手順は [地形 Paint](terrain-scatter.md) に従う。

InstancedMeshComponentはasset参照・有効状態・影・描画距離を持ち、Playでの個体変更はcomponent専用のcopy-on-write状態へ保存する。Edit Sceneの配置・asset・履歴をPlayで変更しない。削除したIDを新個体へ再利用しない。Component の保存キーは既存Reflectionと互換を保ち、専用配置は型付き AssetManager から解決する。作者向けC# APIは型付きasset設定とIDによる個体操作を提供し、Engine実装型を公開しない。

## 描画

Engineは解決済みmesh/materialと不変の個体snapshotをGraphicsへ渡す。GraphicsはSceneを参照しない。GPUの行列・保守的world boundsはowner/resetと内容revisionで管理し、静止個体を毎frame作り直さない。色・影・選択・速度で個体IDを共有し、camera/light/viewごとに可視個体とLOD指示を別に生成する。木の幹・葉がdepth順で交互になっても、part・材質・LODごとのdrawは個体数に比例して分裂させない。

標準の不透明/alpha-test材質は同じGBuffer評価を使い、Forwardの標準PBRも対応する。未知のcustom shaderと非対応deviceは診断付きCPU経路とする。透明物の合成順を間接圧縮で変更しない。全成功条件は資源世代・容量・shader・input範囲の検査を通し、失敗時は同じsnapshotとLODを使うfallbackを行う。

GPUは個体の可視判定とscreen-heightによるLODを行い、LOD別visible IDを生成する。描画は常駐行列をvisible IDで参照する。描画件数はGPU引数から読み、CPUへ可視数を読み戻さない。小さな束では通常instancingを選び、実測に基づいて経路を決める。初回upload、編集時、steady状態を別に測る。

DX12のGPU行は208byte（現在world・normal・前world・bounds）、viewは208byte、間接引数は24byte、visible IDは4byte。常駐GPU行の参照予算は256MiBで、直前frame以前の未使用行を退役し、収まらない描画は同じデータのCPU経路へ戻す。通常設定では32,768個体未満をCPU選別instancingにし、Indirect/Mesh診断設定では64個体以上のGPU経路を明示的に比較する。Cameraの球/奥行き距離、layer距離、bounds余白、極小object条件をCPU/GPUで揃える。

現段階ではRaster専用。RaySceneBuilderは配置をTLASへ入れられない場合のcoverage不足を診断し、草木を黙って省いたレイ結果を採用しない。

## 葉とFiber

葉の影は本体と同じalpha cutoff・albedo alpha・UV tiling/offsetを使い、両面材質は影も両面にする。通常・instanced・skinned影で同じ契約を使う。alpha-testを理由にMeshletへ入れない既存fallbackも、正しい葉の形を影へ書く。

Fiberを付けた地形全体をLOD0へ固定しない。閲覧cameraで実際に草が描かれる近傍patchに必要なLOD0だけを維持し、遠方は地形LODへ戻す。色と全light shadowは同じ閲覧cameraの地形LODを使う。根元の近傍判定にはFiberのworld bounds・lodFarを含む保守的条件を使う。Fiberの地形内容検査はsurfaceRevisionと層/穴変更で無効化し、変更のない巨大height配列を毎frame読み直す必要をなくす。

地形の全LODはpatch境界の頂点間隔を1cellで共有し、内部だけ粗くする。粗い境界blockは既存grid頂点を中心とするtriangle listへ分割する。穴を含むblockはLOD0にし、その隣の粗いblockの共通edgeも1cellへ分割する。CPUのLOD IBとGPUのpacked IBは同じ生成関数を使い、異なるLODの隣接patchと穴の周辺でT字接続による隙間を作らない。

## 検証と測定

stable ID・invalid入力・保存/復元・Play分離・view/ResourceManager分離・revision更新をCPUで検証する。実DX12でopaque/葉のalpha-test/両面影、複数partとLOD、静止/変更、CPU/Indirectの画素一致、草の遠景で地形LODが下がることを確認する。

大量配置の検証sceneは10,000以上の木とDetailを持ち、近景・遠景・部分可視・全可視・移動を同じcameraで比較する。ユーザーの承認によりFBZZ標準ProfilerでCPU/GPU時間・draw数・upload量を測り、同一解像度・TAA・影・warmup/frame数で比較する。画像とscenario結果と時刻標本を保存し、別作業のGPU負荷がある場合はその限界を記録する。SDKの反映と起動は共有SDK契約に従う。

参照: [UE ISM/HISM](https://dev.epicgames.com/documentation/unreal-engine/instanced-static-mesh-component-in-unreal-engine)、[Unity Detail](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/TerrainData.SetDetailLayer.html)、[D3D12 indirect drawing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/indirect-drawing)。

## 汎用名と互換性 (2026-10-11)

`InstancedMeshComponent`、`InstanceSetAsset`、C# の `InstancedMeshes`、native kind `INSTANCED_MESH`、登録・保存型キー `InstancedMeshComponent` は既に用途を限定しない名前なので維持する。木・岩・家具・任意の小物で同じ契約を使う。既存 Scene / Prefab / `.fzdata` の型や GUID を移行する必要はない。

GPU 回帰の `InstancedVegetationTest` / `TerrainVegetationShaderTest` は `InstancedMeshTest` / `TerrainInstanceShaderTest` に統一し、ファイルと CMake 登録も揃える。benchmark は `FBZZ_INSTANCE_BENCH_COUNT` / `FBZZ_INSTANCE_BENCH_REPORT` を正本にし、旧 `FBZZ_VEGETATION_BENCH_*` も未指定時の互換入力として受け付ける。出力は `Scratch/InstancedMeshBenchmark.json`、schema は `fbzz-instanced-mesh-benchmark/1`。


## Loaded GUID resolution (2026-10-18)

Render extraction resolves loaded instance-set GUIDs through bounded canonical-key aliases. Each hit checks the current AssetDatabase revision and reacquires the current DataAssetRegistry entry; no asset pointer is retained. Index moves/removal/project retirement and ClearCache reopen normal resolution. Reload, undo, replacement and snapshots remain authoritative in the registry. Failed loads and non-GUID references retain the original filesystem, path-priority and retry behavior. Raw disk presence is checked on cold/reindexed lookup rather than polled for every render frame of an already-loaded definition. Absolute-path normalization separately retains only pure path spelling, with locale folding left live and relative inputs excluded.

Dedicated `.meshinstances` / `.terrainpaint` GUIDs use a bounded cache of generational AssetManager handles. Every hit reacquires the current value through `Get`; unload and AssetDatabase revision changes reopen normal resolution. The dispatcher chooses the format from the indexed path without adding filesystem checks to the legacy warm path. Snapshot revisions remain unique across imported object replacements, and same-content native reloads retain the current snapshot for editing history.

## 配置全体の保守的カリング (2026-11-06)

### 植生の128mセル予算

`InstancedMeshComponent` の `vegetationCellBudget` は、配置先をワールド原点基準の128mのXZセルへ分ける。設定は配置セットのowner単位で、`vegetationDrawDetailPerCell`、`vegetationNearDetailPerCell`、`vegetationShadowDetailPerCell` はそれぞれ通常描画の詳細候補、LOD0候補、影の詳細候補に割り当てる行数。各値の0は上限なし。生成時の配置数はPCGまたはTerrain散布の予算で管理する。

描画側は安定IDからセル内の順位を作り、通常描画と影の超過は利用可能なLOD2以降、近景の超過はLOD1以降へ切り替える。遠景用のLOD3を使うかは投影サイズで決める。全行をそれぞれの可視判定に通し、単一LODの素材にはそのLODを使う。配置の版やownerの変換が変わったときにセルの行と境界を更新する。境界には幹・樹冠・風変形を含む全LODの形状を収め、視点やライトから確実に外れたセルを個体選別の前に外す。

太陽の手前2カスケードは、枝や樹冠の影を保つため投影サイズに応じたLODを維持する。セルの粗い可視判定は全カスケードで使い、影の詳細予算は残りのカスケードと局所ライトへ適用する。

植生予算が有効な配置では、3枚目以降の方向ライトのカスケードで、風の変形範囲を含む個体境界の直径をシャドウマップのテクセル数に換算する。2テクセル未満の影を省き、8以下はLOD3、40以下はLOD2、80以下はLOD1を下限にする。利用可能なLODの範囲に収め、選んだLODに属する幹と葉を同じ個体IDで描く。CPU、個別GPU選別、複数カスケードのGPU選別で同じ判定を使う。通常描画のLODと葉のアルファ判定は既存経路を使う。[Unreal Engine の遠景シャドウカリングと LOD](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine#improving-non-nanite-performance)

8kmマップの最適化では、常駐行の作成時に、有効な各個体のworld球を含むAABBをSourceへ保持する。ビューとライトごとに、個体単位のCPU走査・GPU選別用バッファの貸出より前に、同じ余白・球距離または前方距離と視錐台で配置全体を判定する。範囲外と確定したSourceだけを省き、極小物判定とLODは既存の個体単位の処理へ残す。

部分編集では変更した球を既存AABBへ加え、削除・移動前の範囲は残す。全体の再構築時だけ縮めるため、範囲の再走査を毎フレームや一個体の編集へ追加しない。非有限の境界は粗い判定を使わず従来の個体処理へ戻す。全個体が無効なら描画しない。内容・owner変換・モデル境界・ResourceManager世代の既存照合に従い、カメラ移動だけで常駐行を再転送しない。

CPU 遮蔽を使う 512 個体以上の集合は、常駐球を包む二分木を source ごとに作る。節全体が距離・視錐台・遮蔽で除外できる場合は有効個体数をまとめて数え、残る最大 32 個体の葉だけを通常選別する。個体・owner・モデル境界の変更で木を破棄する。可視結果を保存せず、CPU の描画順は選別後に元の slot 順へ戻す。

GPU選別用バッファの確保後にUAV等の検証で拒否した場合は、ハンドルをResourceManagerへ返す。ディスクリプター不足でプールへ入らなかったバッファを保持し続けない。

## カリング統計の完全性

カメラの通常色パスだけが有効個体を一度数える。Source 全体の視錐台・距離除外は有効個体数を一括加算し、CPU の個体選別は最初の除外理由を一件加算する。Shadow・Selection・Velocity・Capture と Forward の前段 GBuffer はカメラ統計を増やさない。

GPU 個体選別の候補は `gpuVisibilityUnknownObjects` へ分ける。追加の CPU 全走査や GPU 読戻しは行わず、未確定候補を可視として数えない。Analysis は `Known visible` と未確定候補数を表示し、API は `knownVisibleObjects` を可視数の下限として返す。`visibleObjectsComplete` が false のフレームは `visibleObjects` を null とし、真のフレームだけ距離・極小を含む全除外理由を引いた値を返す。

## 地形・森林の参照解決 (2026-11-06)

Scene読込は保存GUIDを標準の`Assets/...`参照へ戻す。配置定義と地形材質は、GUIDに加えて、現在のプロジェクト索引・実パスと一致した読込成功済みの標準参照も世代付きhandleで再利用する。各表は最大4,096件とし、AssetDatabaseの索引版またはAssetManagerの内容世代が変われば捨てる。非標準表記・未登録・共有側への相対fallback・失敗した読込は従来のパス優先順位と再試行を維持する。索引付きの読込済み資産のファイル変更は既存の監視・移動・削除通知から反映し、定常描画ではディスクの有無を再確認しない。

各取得で現在のhandleをGetし直すため、Unload・プロジェクト切替後の実体や過去のポインターを使わない。地形材質のauthoring snapshotとテクスチャのbindless添字は各抽出で現在値を読み、地形のdirtyが立たないInspector編集・ホットリロードも反映する。地形資源は既存のScene・ResourceManager・Reset世代の所有を保つ。配置数・高さ・splat・LOD・風と影の時刻は変更しない。

索引通知を提供しないホストや同一バイナリーの比較では、起動時に`FBZZ_LOADED_REFERENCE_MODE=poll`を指定すると、この参照再利用を省いて通常のロード解決へ戻る。実行中には切り替えない。

Profilerの明示的なInstancesフィルターは定義解決・snapshot取得・owner状態を、Terrainsフィルターはsetup・層材質・patchを分けて集計する。通常描画では内部フェーズの時計を読まない。検証は現在のGPU層パラメーター、保持snapshot、編集・reload・unload・移動・Reset・同じ標準パスを持つ別プロジェクトで行う。

## 遠景樹木のアセットLOD (2026-11-06)

ユーザーの遠景ビルボード化と負荷削減の指示により、島のSpruce/Birchへ既存4段階目のLODを使う。LOD0〜2のモデルと材質を保ち、最後に切替前のLOD2の実形状・透過色・接空間法線を焼いた交差ビルボードを足す。外形寸法はLOD0を基準にし、切替で樹冠が急に密になることを避ける。3方向の垂直板と上面の計8三角形を標準PBR/alpha-testの単一partとして描き、通常のinstancing・影・GBuffer・選択を共有する。配置ID・位置・姿勢・本数は変えない。遠景の材質は風を0にし、近景の葉は既存の材質で揺らす。

Directional Light の Auto Bake モードでは、カスケード影に葉の風変形前の形状を使う。ライトやキャスターの移動でリアルタイム描画に切り替わる間も、葉の影は同じ形状を使う。見た目の葉と Spot・Point Light の影は時刻に応じて揺れる。Directional Light の Realtime モードでは、手前2枚のカスケードに葉の揺れを反映する。

切替は距離の固定値ではなく既存の保守的boundsと画面内の大きさで選ぶ。近い2段の閾値を保ち、Spruceは0.055、Birchは0.04未満で最終LODへ移す。Paint素材にも同じ定義を保存して、新規配置へ継承する。焼いたアトラスは色と法線で同じ配置を使い、透過縁へ色を延長してmipの黒縁を抑える。遠景から近景へ移動した画像と同一視点の実DX12時間・三角形数を比較する。

色・法線アトラスで遠景の樹木の光応答を保つ方式の参考: [Unity BillboardAsset](https://docs.unity.com/en-us/engine/7000.0/script-reference/unityengine/billboardasset)。この島は固定した交差板を使い、カメラ方向で画像を選ぶ方式は追加しない。

Deferredの後段Forwardでは、全partがGBuffer対応ならSourceの行列upload・CPU/GPU選別・LOD指示の準備を行わない。描画対象は既にGBufferへ提出しており、後段のpartループも全て省くためである。混在するcustom shader、通常のForwardと反射captureは既存経路で描く。初回Forward呼出しのupload/drawが0で、その後のGBufferが正しく初期化して描けることを実DX12で検証する。
