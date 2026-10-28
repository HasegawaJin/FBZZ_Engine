<!-- @file    csharp-benchmarks.md -->
<!-- @brief   C# の大量個体計測における基準シーン・測定区間・結果の契約。 -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-07 -->

# C# の大量個体計測

保存した基準シーンを系統ごとに用意し、Play 中の C# が指定個数の一時 GameObject を作る。ディスク上の Scene は正本として保持し、ケースの実行では書き換えない。最初の ScriptBench は同じ平坦配置・同じ変位を個体別 Script、管理 Script の個別操作、Objects.TranslateWorld、一括位置読取と設定の四方式で比較する。

Runner は準備、待機、ウォームアップ、計測、位置と件数の検証、完了の順で進む。生成、参照とバッファの確保、結果の集計と file 保存は定常移動の測定区間に入れない。各ケースは新しい Play で始め、Stop の復元完了を待つ。Play の再開始は JIT や外部・native asset cache の初期化を保証しないため、cold-start の測定は別プロセスで行う。

移動は指定 simulation step に基づく共通の正弦変位を使い、比較ごとに実フレーム時間の差で軌道が変わらないようにする。描画の有無、カメラ、VSync、解像度、Profiler、Inspector/Gizmo の状態を結果とともに固定する。定常実時間性能は実時間の Playtest で測り、固定 timestep の Time.DeltaSeconds/FPS を実測時間として扱わない。

Runner の frameIntervalMs は Stopwatch による Update 開始から次の Update 開始までの実時間で、描画・待機を含む。updateWindowMs は先行する Runner の Update から LateUpdate までの実時間で、間の Engine phase も含む。workloadMs は任意で有効にする操作本体の計測で、callback dispatch を含まない。大量個体では計測用 timestamp 自体の費用があるため、通常比較では詳細本体計測を無効にする。

managed allocation は同じ Update 境界間の GC.GetAllocatedBytesForCurrentThread の差、GC 世代別回数はプロセス全体の差を記録する。native/GPU の確保量をこれらの値から推定しない。全フレームの CSV と JSON、nearest-rank の中央値・p95・p99・最大を Library/Benchmarks に保存し、集計と保存に伴う allocation は測定から除外する。

Engine のフレーム CPU 時間は既存 Performance Profiler の単調時計から取得する。Benchmark capture は有界な UI snapshot 履歴と別に、指定区間の scalar frame 情報だけを事前確保した領域へ連続収集する。実際の application frame serial と capture session を保存し、欠落・重複・session 切替・途中停止・scope sample 不完全を明示する。render/GPU の異なる frame を現在の CPU frame に暗黙結合しない。

Native の Script 実行対象は位相ごとに snapshot し、entry と個体の観測 ID を所有する連続 byte バッファへコピーする。offset と byte 長で完全照合し、callback の追加・削除・置換を跨いで元の対象を再解決する。バッファ容量は動的に増やし、ID 長と Script 個数に意味上の上限を設けない。実行順が既に整っているときは並べ替えを省き、必要な場合は安定ソートで同順序の entry の前後を保つ。

Playtest は準備、開始、完了と検証の表明、結果取得を担う。Runner と native capture の開始境界はそれぞれ記録し、異なる区間を同一の測定値として合算しない。case 間の主たる比較は同じ種類の指標で行う。

## Physics の実シミュレーション

PhysicsBench は `Assets/Scenes/PhysicsBench.scene` と `PhysicsBenchmarkRunner` の `FBZZCTestProject.Benchmarks.PhysicsBenchmarkComponent` を使う。C# が実際の Rigidbody と Collider を配置し、物理の積分・接触・解決・Transform 書戻しを測る。各個体へ Script を付けず、管理 Script が初回 FixedUpdate で物性と速度を設定する。大量の C# collision callback はこの基準へ混ぜない。

通常の Scene の Play は `renderObjects=true` とし、形状に合わせた MeshRenderer を剛体・床・CCD 標的の子に作る。`visualMaterial` は Inspector で共有 .mat の GUID 参照を指定し、各 child の Material へ割り当てる。球は青、箱は橙、capsule は緑、cylinder は赤、床は灰、CCD 標的は黄の runtime override を使う。剛体 owner は unit scale、描画 child の scale は Collider の寸法に合わせる。性能比較の Playtest は `renderObjects=false` を明示して描画負荷を除外する。

| mode | 初期ケースの要求個数 | 対象 |
|---|---:|---|
| `FREE_MOTION` | 64 / 256 / 1024 / 5000 | 離れた形状を同じ速度で動かし、積分と Collider / BroadPhase の費用を見る |
| `GROUND_CONTACT` | 256 / 1024 | 相互に離れた球と床の接触・摩擦・接触分類を見る |
| `BOX_STACK` | 256 | 箱の接触列と island / PGS を見る |
| `MIXED_SHAPES` | 256 | 球・箱・capsule・cylinder の形状別判定の差を見る |
| `SLEEPING` | 5000 | 明示的に sleep した個体と Scene 同期の残る費用を見る |
| `STATIC_COLLIDERS` | 5000 | 静的形状の同期・BroadPhase と動的個体がない場合の費用を見る |
| `CCD_PROJECTILES` | 64 | 高速球と static Rigidbody を持つ球の CCD を見る。対応範囲は `sphere_sphere` |

要求個数はケースの入力であり、床や CCD の標的を含む実際の個体・body 数とは分けて記録する。CCD の box wall / mesh 対応や kinematic body は現在の SDK 契約に含まれない。静的個体の移動を kinematic solver の検証と呼ばない。

設定変更を適用して `runRequested=false` で Play へ入り、初回 FixedUpdate の準備完了で `PhysicsBenchmarkReady` を出す。native Performance Profiler を有効にし、60 frame の warmup と 120 frame の capture を開始してから `runRequested=true` を適用する。Runner の warmup 60 / measure 120 と native capture の区間はそれぞれ保持する。準備・集計・検証・保存を定常測定の値へ混ぜない。

主系列の Playtest は `lockstep=1/60` を使う。固定 dt から実時間 FPS を算出せず、既存 Profiler の実 CPU / wall time を使う。native の各 captured frame に完了した `World::Step` が 1 回あることを、120 valid frame、除外 0、合計 step 120、zero-step / multi-step frame 0 で確認する。プロジェクトの fixed timestep がこの条件と合わなければケースは失敗し、条件を緩めず設定と実際の step 数を調べる。

`PhysicsBench-Realtime-FREE_MOTION-256` は `lockstep=0` の別系列とし、zero-step / multi-step frame と実 step 数をそのまま記録する。1 frame あたりと 1 World step あたりの費用を分ける。Runner の `totalFixedSteps` / `measuredFixedSteps` と秒数、sample ごとの `fixedSteps` / `fixedSeconds` を保存し、native と C# の異なる capture 境界を同一系列として暗黙に結合しない。

完了には 120 sample、位置・姿勢・速度の有限性とケースごとの実移動 / 接触 / sleep の検証を要求する。`PhysicsBenchmarkCompleted` が存在し、`PhysicsBenchmarkFailed` と Console error がないことも表明する。結果は `Library/Benchmarks/Physics/PHYSICS-<mode>-<count>-<UTC>-<id>/` の JSON と frame CSV に保存する。C# の `validated`、mode と要求数、実 body 数、sleep 数、fixed step 数を確認し、native の 120 行を欠落なく取得する。

native の Physics 集計は `PhysicsSystem`、`SyncRigidBodies`、`SyncColliders`、`SubmitSceneState`、`physics::World::Step`、`DispatchCollisionEvents`、`WriteBackTransforms` を分ける。scope 不完全・fault・abort の frame は原データへ残して集計から除外する。`World::Step` の呼出数は World 内の substep 数と同じではない。`msPerStep` は step のある frame の総時間を step 数で割った分布であり、複数 step がある frame から個別 step の percentile を推定しない。substep、形状、材質、sleep、CCD、描画と Profiler の条件を固定して比較する。

sleep の whole-World early-out を実行済みと仮定しない。現在の Collider 同期は、非所有形状の変更を見逃さないため既存 slot でも保守的に Scene 変更として扱う。静止・sleep ケースも同期と BroadPhase の実測値、実際の sleep 数から判断する。SIMD の候補は測定後に選び、独立した剛体の一括積分や同形状 pair 判定と、island 構築・配列再確保などアルゴリズム側の費用を分ける。連結 contact の PGS の順序依存を単純な並列計算へ置き換えない。

`World::Step` 全体の時間を積分だけの費用として扱わない。次の詳細計測では force / integration / Collider 更新、BroadPhase の proxy / BVH 構築 / pair 探索、NarrowPhase、island 構築 / PGS / 位置補正、CCD 探索を分ける。pair・contact・island・CCD 候補の件数と、反復中の確保や全配列走査も確認する。Physics から Engine/Core Profiler へ依存させず、時計と記録は Engine 側に置く。SIMD を比較するときも、データの詰替え・演算・書戻しの合計と物理状態の回帰を同条件で評価する。

一時実行入口 `Scratch/PhysicsBench/RunPhysicsPlaytests.ps1` は hidden SDK Editor を直列に起動する。通常は一巡、比較では `-Repeats 3` を使い、Realtime は別の `-Pattern` で実行する。既存ビルドの完了を待ち、SDK の managed build がソースと別 root の入口を使う場合だけ、ケース中は AgentBuild と同じソース root の mutex を予約する。同じ root の子ビルドを親が予約して互いに待つ状態を作らず、ケース終了・例外で必ず解放する。各回の Playtest report、ログ、入力と SDK binary・Physics ソースの SHA-256、新しく生成された該当 mode/count の C# JSON / CSV と native capture を新しい archive へコピーする。JSON は UTF-8 として読む。既存の Playtest report と C# result を上書きせず、同時生成などで新しい結果が一意に対応しない場合は失敗する。

測定完了後の物理検証で失敗した場合も、C# は `phase=FAILED` / `validated=false` と最初に失敗した個体の index・形状・理由・位置・速度、形状別の失敗数と Y 範囲を保存する。非有限値の診断は文字列または null に投影する。Playtest は完了判定より先に native capture を取得し、実行入口は失敗判定より先に原データを archive へ保持する。失敗した測定を性能改善の合格比較へ含めない。

参照する .NET の timer と allocation 契約は実装の @see にも記載する。

- https://learn.microsoft.com/dotnet/api/system.diagnostics.stopwatch.gettimestamp
- https://learn.microsoft.com/dotnet/api/system.gc.getallocatedbytesforcurrentthread
