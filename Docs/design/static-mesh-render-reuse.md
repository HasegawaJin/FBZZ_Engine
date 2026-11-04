<!-- @file    static-mesh-render-reuse.md -->
<!-- @brief   静的MeshRendererの通知・現在値検証・フレーム間geometry再利用の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# 静的MeshRendererの描画データ再利用

2026-10-24追記: 形状の現在値検証は維持し、所属/所有順と一部の重複走査を別途最適化する選択経路を追加した。以下の保存領域再利用自体が公開layoutを変えない説明と、新しいScene/ComponentArray/FrameGeometryCacheのlayout変更は区別する。[新しい切替と契約](render-extraction-scan-optimization.md)。

`RenderSceneExtractor`は、通常の非Skinnedな`MeshRenderer`について、world行列・逆転置行列・world境界と遮蔽物適性をフレーム間で再利用する。公開する`RenderScene`は毎回独立した値を持つ。描画命令・材質・GPU資源実体を永続所有する仕組みではない。

## 変更の検出

`NotifyMeshRendererChanged(Scene&, EntityID)`はSceneの更新・抽出と同じスレッドで書き込み後に呼ぶ。C#のMeshサービスと`RuntimeMeshSystem`のMesh解決・手続きMesh差し替えから接続する。通知はSceneの描画入力版を進め、同じフレームのgeometry共有を失効させる。Sceneの再利用領域がなければ何も保持しない。通知用の個体索引と個体ごとのhash nodeは持たない。

行列・境界の再計算は通知回数ではなく、実際のpose・Mesh入力・寿命の相違から決める。したがってcastShadowsだけの変更や同じ値の再設定は、共有snapshotを更新しても行列を作り直さない。通知を落とすと同一フレームの共有snapshotに古い描画フラグが残るため、C#サービス境界からの回帰テストを持つ。

Transform、MeshRenderer、Meshの公開フィールドには直接書き込みがある。したがって通知の有無にかかわらず、geometry buildの候補走査中にworld位置・world回転・world scaleの10 floatと、Meshのhandle世代・件数・ローカル境界をbit単位で照合する。Editor、親子階層、物理、C# Transform、アニメーションが確定した現在のworld値を使う。描画前にlocalを同期する既存の`FlushWorldTransforms`契約を変えない。

この段階でも照合は全Renderer候補に対する **O(R)**。通知件数だけで処理を完結する **O(K)** にはなっていない。enabled、active、所属順は毎回の候補抽出から評価し、layer、shadow、LOD、sort位置は現在値を毎回出力する。通知なしで同一フレーム中にSceneを変更する場合、従来どおり`RenderFrameGeometryCache`を使わない。

## 寿命と所有権

スレッドごとに最大4 Sceneの私有領域を持つ。Sceneのアドレスと`GetRenderSceneGeneration()`、Entityの世代、Component寿命tokenを照合する。SceneのClear・move・同一アドレスの再利用とComponentの付け直しを同一の個体とみなさない。再利用値は`MeshRenderer`の格納添字で引くため、有効状態やScene内の表示順が変わっても、同じ入力のメッシュは再計算しない。メッシュの解除やスキン付きへの切り替えを観測した位置は失効させる。Componentの削除で末尾が移った位置は現在のEntityと寿命を検証し、格納範囲を外れた末尾は切り詰める。保持容量は各slotの過去最大件数、Sceneの退役slotは次の巡回で解放する。

初回のScene抽出は従来のdense geometry経路を使い、次のgeometry buildから再利用値を作る。Sceneを毎回作り直すcold処理では個体ごとの永続値と通知索引を作らない。連続する異なるフレームでworld姿勢が変わる個体もdense経路へ戻し、毎回変わる行列を永続領域へ二重転記しない。停止した次のbuildで再利用値を再構築する。同一フレームの再抽出は連続移動の判定に含めず、previous worldの規則を維持する。

64体以上で、既存の再利用値の75%以上が変わる観測が2 build続いた場合、次の16 geometry buildはScene単位でdense経路を使う。この間も現在の姿勢・所属・Meshを従来どおり抽出し、previous worldを進める。16 build後に必ず現在値を再検証し、静止したSceneは再利用へ復帰する。一回だけの全件teleportではこの待機へ入らない。`ColdGeometry`、`MovingGeometry`、`DenseGeometryFallback`のProfiler markerは経路の利用を示し、個体数を示すcounterではない。

Meshポインターは現在のComponentとの同一性比較だけに使い、過去のポインターを逆参照しない。共有Meshのhandle・boundsがその場で変わっても、現在値の照合で検出する。保存する行列・境界には材質やnative資源への所有権を含めない。ResourceManagerの所有者・reset・内容版の検証とRay入力準備は既存経路で実行する。

geometryの走査はcallbackを呼ばない。再利用領域への参照は走査内だけで使い、材質ロード等が再入する区間で保持しない。通知による版は材質callbackの前に確定し、callback中の通知を古いgeometryの有効版として公開しない。

`previousWorld`は再利用しない。各抽出で従来の`AdvanceWorld`を呼び、同一フレームの複数view、連続フレーム、欠落フレーム、巻き戻しの規則を維持する。SkinnedMeshRenderer、パレット、GPU変形の成功ticketは従来どおり現在フレームから抽出する。材質・選択・visibilityはviewごとに解決する。

## 公開snapshotの保存領域

`ExtractRenderScene`は、最後の所有snapshotと共有姿勢が退役した後の空vector容量だけを次の抽出へ貸す。公開値の再利用や、使用中のsnapshotへの上書きは行わない。`RenderScene`と共有姿勢vectorはそれぞれ所有leaseの内部に置き、aliasing `shared_ptr`で従来の公開型と所有期間を保つ。同じSceneの次フレーム、別Scene、材質callbackの再入、同一フレームの複数viewでも、退役前の保存領域は貸し出さない。

スレッドごとのorigin poolはitem用を最大2本・各16MiB、object用を最大2本・各8MiBまで保持し、空き容量の上限は合計48MiB。上限を超える配列や空きslotがない配列は従来どおり解放する。全所有者が保持しているsnapshot自体の必要容量は、この空き容量上限とは別に存在する。退役が別スレッドでもorigin poolへmutexで返し、poolの寿命はleaseが保持する。返却前にitemをclearして変形ticket等の所有権を解放するため、空きpoolはScene・Component・GPU leaseを保持しない。

値返しの`ExtractRenderSceneGeometry`は従来どおり呼び手がvectorを所有する。同一フレーム共有用のcompact item配列は現在もフレームごとに確保する。変更は公開layout/APIに加えず、CPU保存領域の確保とページの初回使用を減らす。[変更あり1万体の調査と同条件比較](../benchmarks/2026-10-23-moving-mesh-tail/Report.md)。

## 検証と計測

`StaticMeshRenderReuseTests.cpp`は入力差分だけの再計算、直接書き込み、previous world、Entity/Component/Scene寿命、親子変更、最大Scene数を超えた後の再構築を公開snapshotと既存Profilerで検証する。`RenderSceneExtractorTests.cpp`はC#サービスからの通知による同一フレーム共有geometryの失効と、無変更の行列の再利用を検証する。`StaticMeshRenderMotionTests.cpp`は初回・連続移動・Scene単位のfallbackと停止後の復帰を検証する。

`FBZZStaticMeshReuseBench`は無変更・1%変更・全件変更・1%生成削除・cold build・初回2抽出を比較する。通常ケースは変更を適用した後の抽出時間、C#向けnative Meshサービスを使うケースは変更・通知から抽出までの時間を計測し、結果のbit単位hashを保存する。nativeサービス境界の計測はCoreCLRの実行時間を含まない。実描画は既存の`RegularMeshRenderPerformanceTest`でフレーム全体・pass・GPU計測と読戻し結果を比較する。CPU Profilerを停止した時間計測と、Profilerを有効にした内訳確認を区別する。結果・条件・制約は計測報告へ記載する。

[2026-10-23の性能比較](../benchmarks/2026-10-23-static-mesh-reuse/Report.md)に生標本と前後の実行物監査を保存した。静的ケースの改善だけでなく、初期化・全件変更・実描画の1%変更の後退も記録している。

## 参考

- [Godot RendererSceneCull](https://github.com/godotengine/godot/blob/master/servers/rendering/renderer_scene_cull.h)：instance依存の変更通知と更新対象の管理を参考にした。FBZZの公開フィールドを通知だけで追跡できるとは仮定していない。
- [Unreal Engine Mesh Drawing Pipeline](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)：static入力の保持と動的入力の分離、cacheが参照する資源寿命の考え方を参考にした。今回の実装はMeshDrawCommand cacheではない。
- [C++ shared_ptrのaliasing constructor](https://eel.is/c++draft/util.smartptr.shared.const)：内部のsnapshotを参照しながら、保存領域を持つleaseと所有期間を共有する。
