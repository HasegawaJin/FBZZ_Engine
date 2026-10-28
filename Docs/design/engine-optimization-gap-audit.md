<!-- @file    engine-optimization-gap-audit.md -->
<!-- @brief   Godot・UE・Unityの最適化基盤とFBZZの常用経路を照合し、次の実装順を決める。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-23 -->
# エンジン最適化基盤の差分監査

2026-10-23時点のソースと公式資料を照合した。対象はエンジン内部の作業量、同期、初回描画の停止、GPUメモリ。各製品の機能名を同一視せず、FBZZの常用経路に対応する仕組みがあるかを判定する。今回の監査ではC++の動作変更や新しい性能計測は行っていない。順位は構造と既存の契約から決めた実装候補であり、改善率の予測ではない。

## 既存の土台

FBZZには専用Render Thread、所有済み描画入力、GPUカリング・間接描画・Mesh Shaderの限定接続、自動instancing、bindless、同一フレーム内のRenderScene共有、型別Component chunk、並列System batch、非同期アセット転送・品質段・予算、差分BroadPhaseがある。これらを新規機能として重複実装しない。

確認先は[描画スレッド](render-thread.md)、[GPU形状描画](gpu-driven-geometry.md)、[大量個体処理](ecs-bulk-processing.md)、[アセットstreaming](asset-streaming.md)、[runtime作業領域](runtime-workset-optimization.md)。GPU経路には対象制約と通常描画へのfallbackがあり、全物体がGPU常駐描画になっているという意味ではない。

## 差分

| 優先 | 仕組み・参考先 | FBZZの状態 | 減らせる作業と実装条件 |
|---|---|---|---|
| 1 | 変更通知で更新対象を集める。[Godotのdependency notificationとinstance update list](https://github.com/godotengine/godot/blob/master/servers/rendering/renderer_scene_cull.h) | Renderer所属の索引や局所的な版管理はある。描画抽出を全経路の変更通知だけで更新する仕組みは未整備 | 毎フレームの再抽出を減らす。Editor、C#、物理、アニメーション、直接フィールド書き込みの失効を取りこぼさないことが前提 |
| 2 | 永続する描画データ・描画命令の再利用。[UEのCached Mesh Draw Commands / GPU Scene](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-drawing-pipeline-in-unreal-engine)、[Unity URPのSRP Batcher](https://docs.unity3d.com/6000.0/Documentation/Manual/SRPBatcher.html) | meshlet等の個別cacheと同一フレームのgeometry共有はある。一般Meshのフレーム跨ぎの永続描画台帳・命令cacheは常用経路にない | 変わらないmesh・材質・束縛を作り直さない。PSO cacheやinstancingとは別の層。view固有情報と資源寿命を分ける |
| 3 | 依存関係・優先度・負荷分散を扱うジョブ基盤。[UE Tasks](https://dev.epicgames.com/documentation/unreal-engine/tasks-systems-in-unreal-engine?lang=en-US)、[Unity Jobsのwork stealing](https://docs.unity3d.com/cn/2022.3/Manual/JobSystemOverview.html)、[Godotの優先度付きgroup task](https://docs.godotengine.org/en/4.4/classes/class_workerthreadpool.html) | TaskSystemはmutex付き共有FIFO。SystemSchedulerは競合しないSystemをbatch化し、batchごとにfutureを待つ。依存付きready task、work stealing、TaskSystemの優先度は未実装 | 背景処理とフレーム処理の競合やbatch間の待機を減らす。既存のPhase・ComponentAccess・Scene書込契約を維持する |
| 4 | 初回使用前のpipeline準備。[Godotのpipeline precompilation](https://docs.godotengine.org/en/stable/tutorials/performance/pipeline_compilations.html)、[UE PSO Precaching](https://dev.epicgames.com/documentation/unreal-engine/pso-precaching-for-unreal-engine)、[Unity ShaderVariantCollection](https://docs.unity3d.com/cn/6000.0/ScriptReference/ShaderVariantCollection.html) | PSOのメモリ内cacheはある。Graphics cache missではその場でCreateGraphicsPipelineStateを呼ぶ。事前収集・非同期precache・永続PSO cacheの経路は確認できない | 初登場の材質・描画設定で生じる停止を減らす。シェーダーbytecodeの事前コンパイルとdriverのpipeline生成を区別する。Godotの該当方式はForward+ / Mobileが対象 |
| 条件付き | グラフによるGPUメモリ共有・バリア・並列記録。[UE RDG](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine) | RenderGraphの依存解決・pass culling・計画cache・寿命解析はある。実heap aliasing、グラフによるbarrier列生成、並列command-list記録には接続していない | 同時生存しない資源のVRAMを共有できる場合に限る。既存の束縛時StateTrackerを二重化しない |

Godot・UE・Unityが表のすべてを同じ構成で実装している、という比較ではない。例えばGodotのpriority group task、Unityのwork stealing、UEのtask dependencyは異なる仕組みであり、それぞれFBZZの不足を埋める参考になる。

## ソースで確認した境界

- [RenderSceneExtractor.cpp](../../Projects/Engine/src/Scene/Systems/RenderSceneExtractor.cpp)：`ExtractRenderScene`のcache hit条件に`Time::frameCount`があり、同一フレームの共有である。miss時は`BuildRenderSceneGeometry`がRenderer候補を走査し直す。
- [DeferredPasses.cpp](../../Projects/Graphics/src/Passes/Geometry/DeferredPasses.cpp)：GBuffer等のqueueを作り、材質順にsortし、DrawCallを組み立てる。材質CBの共有とinstancingは存在するため、その効果と永続命令cacheを分ける。
- [TaskSystem.cpp](../../Projects/Engine/src/Core/Concurrency/TaskSystem.cpp)：一つの`m_queue`からmutex下で取り出す。`SystemScheduler.cpp`はbatch末尾で`future::get`する。Phaseを跨いだ任意のScene並列更新は許可されていない。
- [DX12PsoCache.cpp](../../Projects/Graphics/src/Renderer/Platform/DX12/DX12PsoCache.cpp)：`GetOrCreate`のcache missからnative PSO作成へ進み、その結果をメモリ内mapへ保存する。
- [RenderGraph設計](render-graph.md)：物理aliasプールは以前の計測で共有先がなく撤去した。現在のDX12資源生成はCommittedResourceが中心で、寿命解析結果をPlacedResource / aliasing barrierへ接続する実装はない。

## 次の範囲

第一候補は、静的MeshRendererの変更追跡とフレームを跨ぐ描画データ再利用。GameObject・C# SDK・Scene保存形式を維持し、Engine内部の抽出処理とGraphicsへ渡す不変入力に範囲を限定する。全面的なarchetype移行は[大量個体処理の既存方針](ecs-bulk-processing.md)に含まれない。

1. Transform、親子関係、active/enabled、mesh、材質、描画設定の書き込み経路を列挙する。Scene寿命世代と内容変更の版を混同しない。追跡できない経路は既存の現在値検証へ戻す。
2. Entity世代・Component寿命・資源owner/resetを伴う永続データを持つ。個別版の通知を導入しただけで全走査が不要になったとは判断しない。全件の比較が残る段階はO(R)、通知だけで閉じられる段階をO(K)として分けて計測する。
3. 公開済みsnapshotは不変にする。Scene順・slot順・透明順、previous world、view固有の材質・camera・layer・selectionを保持する。Skinned、未知callback、Ray入力等は初期段階では従来経路を使う。
4. 静的多数、1%変更、全件変更、生成・削除・reparent・Undo・shader reload・ResourceManager reset・Play切替を検証する。無変更の速さだけで採否を決めない。
5. `GeometryBuild`、材質準備、命令構築、確保量、GPU upload量とStandalone全体のP50/P95/P99を前後比較する。前回のGPU計測同期削減の簡易Clearベンチを、Scene抽出改善やゲーム全体FPSの証拠に流用しない。

ジョブ基盤はこの差分更新とは独立した第二候補。既存FIFOのqueue waitとmain waitを実シーンで確認し、必要ならフレーム用の優先度・group処理から段階的に拡張する。work stealingという方式名だけを理由に全面置換しない。

VRAM aliasingは保留。[現在の設計](render-graph.md#トランジェントリソースのエイリアスプール)に従い、寿命が重ならない資源の実数と削減可能byteが測れてから再検討する。

## 2026-10-23の実装結果

監査後、静的MeshRendererの通知とworld・逆転置・境界のフレーム間再利用を実装した。[契約](static-mesh-render-reuse.md)と[性能比較](../benchmarks/2026-10-23-static-mesh-reuse/Report.md)を参照。全候補の入力照合はO(R)で残り、通知だけで閉じる差分抽出、材質・draw commandの永続cacheは未実装。静的ケースで短縮した一方、初期化・全件変更・実描画の1%変更には後退が残るため、表の優先1・2を全面的に解消したとは扱わない。
