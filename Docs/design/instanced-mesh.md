<!-- @file    instanced-mesh.md -->
<!-- @brief   任意メッシュの共有個体管理・材質・Fiberと地形LOD・実測の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-10 -->
# Instanced Mesh の共通インスタンス

ユーザーの2026-10-10の指示により、既存Scene向けの自動batchに加え、大量配置専用のInstancedMeshComponentを導入する。木・草メッシュ・低木・花・岩は同じ管理を使う。FiberはShell/Fin/Bladeの形状評価・風・接触を維持し、同じ物体へ二重に配置しない。CPU/Indirectの別コンポーネントは作らない。

## 作者データと所有

InstanceSetAssetは既存.fzdataの内蔵DataAssetとして、mesh/material参照、partのLOD番号、最大4段階のscreen-height閾値、stable ID、local位置・Euler回転[rad]・scaleを持つ。全partは同じ個体配列を共有する。参照はguidを正本にし、primitiveも既存MeshResolverを使う。配列の長さ不一致・ID重複・非有限値・不正LOD・過剰個体数は診断して描画を拒否する。共有assetへの変更は全参照へ反映する。

InstancedMeshComponentはasset参照・有効状態・影・描画距離を持ち、Playでの個体変更はcomponent専用のcopy-on-write状態へ保存する。Edit Sceneの配置・asset・履歴をPlayで変更しない。削除したIDを新個体へ再利用しない。保存とUndoは既存Reflection/DataAsset経路を使う。作者向けC# APIは型付きasset設定とIDによる個体操作を提供し、Engine実装型を公開しない。

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
