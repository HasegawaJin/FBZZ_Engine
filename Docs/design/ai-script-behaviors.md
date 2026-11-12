<!-- @file    ai-script-behaviors.md -->
<!-- @brief   Script behavior authoring, typed agent state, interruption and measured evaluation costs. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->
# C# で拡張する行動判断

Scene のネイティブ `BehaviorTreeComponent` は `farTickDistance` と `farTickRate` で距離による評価間隔を設定できる。すべての有効なカメラが指定距離より遠いときに間隔を延ばす。設定距離が 0、カメラがない、または近距離のカメラがある場合は `tickRate` を使う。Blackboard・知覚の変化と明示的な評価要求は、どの距離でも次の更新で処理する。

`Fbzz.AI` の `BehaviorTree` は共有する定義、`BehaviorTreeInstance` は個体ごとの実行状態を持つ。C# 作者は Engine の C++ を変更せず、条件・行動・優先順位・待機・ターゲット・移動戦略を変更できる。群れの近傍検索、連続 steering、3D habitat の安全制約、Transform のバッチ確定などの大量処理は native の共通サービスへ委ねる。

この API は純 managed の companion である。既存の `BehaviorTreeAsset` / `BehaviorTreeEvaluator` のネイティブ実行を置換せず、既存 asset の読み込みや C# delegate の native BT ノード登録を提供しない。C# の判断から native swarm の意図・ターゲット・パラメータを変更する接続点として使える。BT 自身の Tick は native 呼び出しを行わない。

## 小さい構成例

```csharp
BlackboardSchema.Builder layout = new();
BlackboardKey<float> fear = layout.Add<float>("Fear");
BlackboardKey<Vector3> target = layout.Add<Vector3>("Target");
BlackboardKey<int> mode = layout.Add<int>("Mode");

BehaviorTree tree = new(layout.Build(), Bt.ReactiveSelector(
    Bt.Guard(
        (BtContext context) => context.Blackboard.Get(fear) > 0.6f,
        Bt.Action((BtContext context) =>
        {
            context.Blackboard.Set(mode, 2, requestEvaluation: false);
            return BtStatus.RUNNING;
        }, name: "Flee")),
    Bt.Action((BtContext context) =>
    {
        context.Blackboard.Set(mode, 1, requestEvaluation: false);
        return BtStatus.RUNNING;
    }, name: "Cruise")));

BehaviorTreeInstance agent = tree.CreateInstance(evaluationIntervalSeconds: 0.1);
agent.Blackboard.Set(target, chosenTarget, requestEvaluation: false);
agent.Blackboard.Set(fear, sensedFear);
agent.Tick(Time.DeltaSeconds);
```

上の `mode` を native 側の移動意図へ反映し、steering は前回の意図を保って毎 movement step 継続する。位置・速度を全部 blackboard に詰めて毎フレーム変更通知する必要はない。サンプル `Samples/ScriptBehaviorExample.cs` は個体の `ISwimMovement` を typed key に入れ、独自の fear 閾値、alarm、任意の 3D target を使う。移動戦略の `Step` は Tick の評価間隔とは独立して呼び出される。1,000 体の標準経路には native のバッチ移動を使い、このサンプルの個別 strategy 呼び出しは特殊個体やゲーム独自の移動を説明する拡張経路である。

## ノードと状態の契約

| 部品 | 契約 |
|---|---|
| `Sequence` | 左から順に実行。SUCCESS の子を同じ実行中は再評価せず、RUNNING の子から再開する。子の FAILURE で終了 |
| `Selector` | FAILURE の子を飛ばす。RUNNING の枝を記憶して再開し、SUCCESS で終了 |
| `ReactiveSelector` | 評価時に左側の高優先度枝を再確認する。新しい Action/Wait の開始前に旧枝を abort する |
| `Condition` | bool を SUCCESS/FAILURE に変換する葉。Sequence で通過済みの条件は再確認しない |
| `Guard` | RUNNING の子も含めて評価時に条件を再確認し、false なら子を abort して FAILURE にする |
| `Action` | 同期 delegate が RUNNING/SUCCESS/FAILURE を返す。IDLE は不正。個体の状態は Context の blackboard に置く |
| `Wait` | 実際に枝へ入った時点からの simulation 時間で待つ。スキップした Tick の時間も進む。枝の再開始で待機時間をリセット |

ReactiveSelector の高優先度枝には、効果を出す Action の前に Guard または Condition を置く。高優先度 Action の開始自体が旧枝を中断するので、その Action が即座に FAILURE を返した場合も、旧枝は状態をリセットして再開始する。実行済みの効果を調べずに巻き戻す保証はしない。並列ノードはなく、同時に RUNNING となる Action/Wait は 1 つである。

Action の `onExit(context, reason)` は、開始した行動につき終端・abort・reset・dispose・fault のいずれかで 1 回呼ぶ。RUNNING を返すたびには呼ばない。reason は `SUCCEEDED` / `FAILED` / `ABORTED` / `RESET` / `DISPOSED` / `FAULTED`。古い intent を解除してから新しい行動へ移るため、旧行動の終了処理が新しい intent を消す順序にはならない。外部 Jobs などを開始した行動は onExit で必要な解除を行う。

定義は authoring の配列をコピーし、内部状態を持たない。1 つのノードを複数箇所で再利用しても、各出現箇所に別のカーソル・タイマーを割り当てる。定義に保持した delegate が個体の可変変数を capture すると、その capture は共有される。共有する delegate は immutable な設定/key のみ capture し、可変個体値は `BtContext.Blackboard` から取得する。最大 128 階層、4,096 ノード出現箇所として再帰と容量を制限する。

`BlackboardSchema.Builder.Add<T>` は型付き key を返し、`Build` で schema を固定する。key はその schema の blackboard のみで使え、別 schema と default key は例外で拒否する。型はコンパイル時に固定され、名前は診断・authoring のためだけに使う。instance 作成時に `BlackboardSlot<T>` を一度確保し、scalar の Get/Set は typed field へアクセスする。毎 Tick の dictionary 検索や scalar 格納時の boxing は行わない。変更の検出は `EqualityComparer<T>.Default` を使い、primitive/enum/SDK Vector3 は無確保で比較する。独自 struct は `IEquatable<T>` を実装して比較時の boxing を避ける。参照型の default 値は同じ参照を共有するため、個体固有の mutable object は CreateInstance 後に明示設定する。

## 評価頻度と危険への反応

`Tick(deltaSeconds)` を個体 update から 1 回呼ぶ。初回、指定間隔を過ぎたとき、変更通知、明示的な `RequestEvaluation()` のいずれかで実際に評価する。その他の Tick は時計を進めて前回 status を返し、ユーザー delegate を呼ばない。interval=0 は毎 Tick 評価する明示設定であり、既定値は 0.1 秒。距離 LOD は `EvaluationIntervalSeconds` を変更することで設定する。

`Blackboard.Set(key, value)` は値が変わった場合に次の Tick を評価対象にする。同じ値を再設定しても起こさない。`Set(key, value, requestEvaluation:false)` は位置等の頻繁なデータ更新に使い、指定評価間隔を保つ。参照型の内部値だけを変更した場合は Set が参照の変化を検出できないので、必要なら `RequestEvaluation()` を呼ぶ。callback 中の key 変更や RequestEvaluation は再帰実行せず、次の Tick へ保持する。

危険イベントの遅延は通常の decision interval ではなく「次に所有者が Tick を呼ぶまで」。far LOD でも owner の Tick を毎 update 呼び、早期判定だけを省略する構成なら、低頻度判断の途中に alarm が来ても次 update で逃走へ切り替えられる。所有者自体が update を間引けば、その間のイベント反応も遅れる。連続 steering / habitat / collision safety の頻度を decision LOD と一緒に落とさない。

`BtContext.DeltaSeconds` は前回の評価からの実際の経過時間、`TimeSeconds` は Reset 後の Tick clock。渡す時間は有限・非負の simulation 秒とし、時間 pause や time scale は呼び出し側で決める。指定間隔は最小間隔で、実際の評価は Tick の時点に量子化される。大量の時間が経過しても過去の評価をまとめて catch-up 実行しない。

この companion は独自の scheduler、複数スレッド、時間 budget を持たない。条件・行動は同期完了し、そのコストは作者側で bounded にする。大量経路は native scheduler と batch 処理の budget を使う。1000 体へ高コストな script condition を毎 Tick 登録することを既定にしない。

## 寿命と例外

state と blackboard は single-thread 使用。pure managed の属性は、delegate の内部から任意スレッドで Engine API を使う許可を意味しない。Engine を呼ぶ場合は元の Script callback/Phase の契約に従う。

既存の `ScriptCallbackSafety.RequireSynchronous` を使い、async state machine を持つ delegate と multicast 内の async delegate を登録時に拒否する。Task を同期関数で包む、async の呼び出しを内部に隠すなどのコードは自動検出できない。callback は await、fire-and-forget、スレッド切り替えを行わない。

Tick 中の例外は active 行動を `FAULTED` で解除し、カーソル・待機状態をクリアしてから元の例外を再 throw する。解除自身の例外は元の例外の `Data["Fbzz.AI.CleanupFailure"]` が書き込み可能ならそこに保持する。独自 Exception が Data の取得/書き込みを拒否しても元の例外を優先する。既存の `ManagedScriptRuntime.TryInvoke` が enclosing Script を faulted にし、所有者の Jobs 等を解除する境界へ到達する。例外を successful status に変換しない。終端/解除 callback の呼び出し前に active を外すので、解除が例外を投げても同じ行動を二重解除しない。faulted instance の Tick は拒否し、明示 Reset 後に再利用できる。

`Reset()` は行動を解除し、時計・カーソル・待機状態を初期化するが blackboard 値を保持する。`Dispose()` は `DISPOSED` で行動を解除し、再呼び出し可能な終端操作。Tick/Reset/Dispose の callback からの再入は拒否する。所有する Script は OnDisable で必要な Reset、OnDestroy で Dispose を呼ぶ。Blackboard 内の任意の参照は Engine の所有者登録・自動 remap・保存対象にはならず、GameObject 等の参照は利用時に生存を確認する。hot reload 時は AfterReload 等で helper/tree/delegate を作り直し、必要な単純値を明示移行する。game assembly の delegate を process 永続 static に残さない。

## Epic 公式資料からの採用と見送り

2026-10-11 に公式 [Behavior Tree Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview) と [Decorator Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-behavior-tree-node-reference-decorators) を確認した。

| 原理 | FBZZ での適用 |
|---|---|
| 値の変更で必要な判断を進める | typed decision key の変更通知 + 次 Tick の評価要求を採用 |
| 条件を枝に付け、実行中の処理を中断する | Guard の self abort と ReactiveSelector の高優先度切り替えを採用 |
| 持続的処理と判断を分ける | script/native の保持した movement intent と、低頻度 BT の分離を採用 |

UE の完全な passive observer graph、全 Observer Aborts mode、visual editor、Services/Simple Parallel、UE の UObject/AIController/asset 構造は導入しない。この companion は指定間隔でも条件を確認するので、UE と同じ完全 event-driven runtime と扱わない。必要な危険反応と script の拡張性を小さい契約で確かめる実装であり、UE のソースをコピーしていない。

## 独立 CPU 検証

`Scratch/Tests/ScriptBt/ScriptBtTests.csproj` は package-free の console harness。新 API と sample、実際の SDK の `ApiContractAttribute` / `SynchronousCallbackAttribute` / `Vector3` / `Mathf` 宣言だけをコンパイルし、共有 SDK/native build と Editor は起動しない。別環境では `-p:EngineRoot=<checkout>` を渡す。offline `NuGet.Config` が外部 source を使わない。

```powershell
dotnet run --project Scratch/Tests/ScriptBt/ScriptBtTests.csproj --configuration Release -p:EngineRoot=C:/path/to/FBZZ_Engine
```

テストは 2 個体の state 分離、ノード再利用、authoring 配列のコピー、Sequence memory、Guard self abort、高優先度の起動前 cleanup、failed preemption の再開始、quiet data/event による頻度差、callback 内 event の保持、Wait の経過時間、terminal/reset/dispose、async 拒否、fault と cleanup fault、再入、無効時間、木の上限、custom flee/target/movement strategy を検証する。

1,000 instance × 600 frame の steady Tick を Release で計測し、ウォームアップ後の `GC.GetAllocatedBytesForCurrentThread()` 差分が 0 であることを assertion にする。続けて 600 frame 毎回 RequestEvaluation を起こす経路でも 0 を assertion にする。初期化、例外、ユーザー delegate 内の確保は hot-path 無確保保証の範囲外。elapsed time は CPU 上の managed 判断だけであり、native swarm/terrain/Transform、GPU、実海シーンの自然さや全フレーム時間の証拠ではない。行数減少・native 全体性能は統合担当の移行比較と併せて確認する。

2026-10-11、.NET SDK 10.0.300 / Release、独立 harness の結果は `RESULT PASS tests=21`。同じ 600,000 Tick に対し、0.1 秒間隔の判断は 100,000 action callback、毎 Tick の明示 event 経路は 600,000 action callback となった。

| 計測対象 | 結果 |
|---|---:|
| 1,000 個体 × 600 frame、0.1 秒判断 | 11.940 ms 合計 / 100 評価・個体 |
| 同じ個体数・frame 数、毎 Tick の明示評価要求 | 30.747 ms 合計 / 600 評価・個体 |
| それぞれの steady managed allocation | 0 B |

この単純な action は typed int key を Get/Set するだけで、ゲーム固有 callback の仕事を含めない。時間は当該 CPU/実行時の参考値で、速度の固定保証や native 海 AI の移行前後比較ではない。検証の受入条件は評価回数、危険 event の次 Tick 反応、状態分離、解除順序と無確保である。
