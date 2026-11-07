<!-- @file    engine-internal-spec.md -->
<!-- @brief   FBZZ Engine の内部境界、フレーム処理とデータ寿命の仕様。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-11-07 -->
# FBZZ Engine 内部仕様

対象は Windows 版 FBZZ Engine の作業ツリー（CMake 0.10.0、2026-11-07 確認）。現行の描画バックエンドは DX12。ゲームコードは公開 C# SDK を使う。World Partition / PCG と CPU 遮蔽カリングは作業ツリーで確認した開発中の経路で、配布済み SDK の機能表とは区別する。GreenWare は SDK 0.9.1 時代の別リポジトリで、ここで述べる現行 SDK の回帰対象に含めない。

この文書は実装を追うための共通仕様。詳細な型、保存形式、機能別の例外は各設計文書を参照する。負荷の削減方法、既定値、退避条件、計測の採否は [ランタイム最適化仕様](runtime-performance.md) にまとめる。

## モジュールと依存境界

| 境界 | 担当 | 受け渡すもの |
|---|---|---|
| Math | ベクトル、行列、姿勢、幾何判定 | 値型と数値演算。Scene や描画資源を知らない |
| Physics / Fluid | 物理ワールド・衝突・拘束／流体の数式 | Math を利用する兄弟モジュール。Fluid はファイル形式・Scene・RHI を知らない |
| Core | ログ、時刻、プロファイリング等の共通基盤 | Engine に依存せず利用できる基盤 |
| Graphics | 描画パイプライン、RenderGraph、描画スレッド、GPU 資源、DX12 実装 | 公開依存は Core / Math。Engine / Scene / Physics / Fluid に依存しない |
| Engine | Scene、アセット、入力の抽出、各システム、C# ホスト | Graphics に `RenderScene` と各パスの入力を渡し、RHI の操作は `IRenderer` を通す |
| Editor / 実行ホスト | 編集操作と Undo、Play、Standalone の起動とライフサイクル | ホストが Scene と ResourceManager の接続を決める。ゲームコードは C# SDK の公開 API を使う |

Engine 内部は Platform、Assets、Audio、AI、Scene、Rendering、Managed の OBJECT モジュールを Engine DLL に合成する。型の自己登録を維持し、許可されない直接・推移 include とリンクを configure で拒否する。Graphics のバックエンド実装ヘッダーは Engine や Editor の契約に持ち込まない。[モジュール境界](engine-module-boundaries.md) · [Graphics 分割](graphics-library.md) · [Fluid の範囲](fluid-library.md)

## フレーム処理

1. **シーン更新。** GameObject / Component を `ISystem` が処理する。`PreScript → Script → PrePhysics → Physics（固定刻み）→ PostPhysics → Navigation → LateScript → Cleanup → LateUpdate` の Phase と、RunMode・順序制約・ComponentAccess に従う。未宣言のアクセスは Unrestricted として扱う。同じ Phase でも競合しないと確認できる処理だけを並列に走らせる。
2. **姿勢と境界の確定。** C# の Transform setter は local を正本に親子の world を同期する。Engine 内部で Transform を直接変更した場合は、world を読む境界の前に `FlushWorldTransforms` で同期する。物理更新、描画入力、空間クエリが別々の未確定値を参照しないようにする。[Transform の契約](script-transform-contract.md)
3. **描画入力の抽出。** Engine が mesh、材質、姿勢、ライト、環境と各効果の入力を抽出する。共有できる幾何入力と、カメラ・光源・選択状態に依存する入力を分離する。CPU UI の配置・入力も描画の成功に依存させない。描画と UI はシステム Phase に含めない。[描画入力の境界](graphics-library.md) · [UI と実行ホスト](engine-module-boundaries.md)
4. **描画計画と提出。** Forward / Deferred は Graphics 内のパイプライン構成。材質とパスの契約に従って経路を選び、RenderGraph が読み書きと副作用から実行順と必要なパスを決める。提出後は Render Thread が命令記録を担当し、DX12 側が資源状態、コマンドキューと GPU 完了を管理する。入力はキュー投入後に書き換えない。[描画経路](pipeline-boundary.md) · [RenderGraph](render-graph.md) · [描画スレッド](render-thread.md)

`Update`、描画スレッドの記録完了、GPU の実行完了は別の境界。例えば描画命令を記録できても、その GPU 資源を直ちに破棄・上書きできるとは限らない。Resize・読戻し・Play/Stop・終了では、変更対象がどの境界まで使用されるかを確認して同期する。

## データの正本と寿命

| データ | 正本と公開境界 | 無効化・失敗時の扱い |
|---|---|---|
| 編集中の Scene / Prefab / Asset | ディスク上の保存データと `.meta`。参照は `guid:`。Editor 操作を登録簿で定義し、メニュー・パレット・AI が同じ操作を使う | 既存データへ差分を適用する。保存形式と GPU/CPU の派生キャッシュを混同しない |
| Play の状態 | Edit とは別 Scene。C# static も Play 世代に分離する | Stop で元の Edit Scene・個体・履歴を保持。外部 I/O はプロセス共有なので同じ分離を前提にしない |
| Component / Entity の実行時参照 | Scene が所有。キャッシュは Scene の寿命と Entity / Component の有効性を確認する | Clear、削除、複製、Play 切替で再検証する。アドレスや再利用可能な ID だけで寿命を同定しない |
| 描画スナップショット | 提出時に必要な値・転送元を所有する。GPU ハンドルは非所有参照 | 次フレームで再利用する保存領域も、読者の利用が終わるまで上書きしない |
| GPU 資源と派生キャッシュ | 作成元 ResourceManager が資源を管理。寿命 ID と Reset 世代を区別する | 失効した世代のハンドルを別 Manager で解決・返却しない。受付済みの命令と GPU の退役を待つ境界を分ける |
| 非同期アセットとセル | 読み込み候補・世代・利用権はアセット管理が保持し、現行要求を確認して公開する。World Partition のランタイム常駐分は Streamer の所有 | 要求が無効になった候補を遅れて公開しない。セルを外すときはそのセルが所有する実体と利用権だけを解放する |

[C# と Play のトランザクション](csharp-settings-transactions.md) · [アセットストリーミング](asset-streaming.md) · [シェーダー再読み込みの寿命](shader-reload-lifetime.md) · [World Partition / PCG](world-partition-pcg.md)

## 主要経路の参照先

| 調べたい処理 | 実装の入口 | 詳細 |
|---|---|---|
| 更新順、アクセス競合、待ち | [`SystemScheduler.cpp`](../../Projects/Engine/src/Core/Scheduler/SystemScheduler.cpp)、[`TaskSystem.cpp`](../../Projects/Engine/src/Core/Concurrency/TaskSystem.cpp) | [大量個体処理](ecs-bulk-processing.md) |
| シーンから描画データを作る | [`RenderSceneExtractor.cpp`](../../Projects/Engine/src/Scene/Systems/RenderSceneExtractor.cpp) | [抽出作業領域](runtime-workset-optimization.md) · [静的メッシュの再利用](static-mesh-render-reuse.md) |
| 描画のキュー、描画可否 | [`RenderThread.cpp`](../../Projects/Graphics/src/Renderer/RenderThread.cpp)、[`ViewPipeline.cpp`](../../Projects/Graphics/src/Pipeline/ViewPipeline.cpp) | [描画スレッド](render-thread.md) · [ビュー別カリング](view-culling.md) |
| 衝突候補と形状クエリ | [`PhysicsSolver.cpp`](../../Projects/Physics/src/PhysicsSolver.cpp)、[`World.cpp`](../../Projects/Physics/src/World.cpp) | [差分 BroadPhase](broadphase-incremental.md) · [物理クエリ作業領域](runtime-workset-optimization.md#physics-query) |
| C# の呼び出しとデータ接続 | [`ManagedScriptAdapter.cpp`](../../Projects/Engine/src/Scene/Managed/ManagedScriptAdapter.cpp) | [C# ホスト](csharp-scripting.md) · [Component サービス](csharp-component-services.md) |
| アセットの読み込み・公開、ワールドの常駐 | [`AssetStreaming.cpp`](../../Projects/Engine/src/Asset/AssetStreaming.cpp)、[`WorldPartition.cpp`](../../Projects/Engine/src/Scene/WorldPartition.cpp) | [アセット](asset-streaming.md) · [World Partition](world-partition-pcg.md) |
| 計測・実行経路の確認 | [Profiler](profiler.md) · [PIX](pix-profiling.md) | [最適化の計測仕様](runtime-performance.md#比較計測の手順) · [ベンチマーク](../benchmarks/README.md) |

仕様を変えるときは、データの正本、書き込み主体、同期境界、失効条件、既定の代替経路を明記する。機能の回帰と速度の比較は別の証拠で残す。検証の入口は [AI 検証手順](ai-verification-loop.md) と [テスト規約](../conventions/test.md)。
