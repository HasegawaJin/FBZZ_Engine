<!-- @file    ai-ecology.md -->
<!-- @brief   Bounded ecology policy, food density and caller-owned crowd/scene integration. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->
# 生態系の判断と群泳・資源・シーンの接続

生態系は食性・エネルギー・捕獲・死骸・出生予約を扱い、連続移動は `AiCrowd`、plankton の供給量は `AiDensityField`、Scene の object と演出は所有する Script が扱う。`AiEcologyTier` は役割の説明であり、数値の大小で食べられる相手や勝敗を決めない。捕食可能性は diet/tag の一致、食べられるサイズ範囲、生息域の重なり、現在状態と距離・cooldown で判断する。

この API はゲーム向けの架空モデルで、実在する海洋生態や自然な個体数を保証しない。既存 Marine/Ocean script の実 Scene 移植は統合担当が行う。共通 API を使う例は `Projects/Managed/Authoring/AiEcologyExample.cs` に置く。

## 架空の 5 種と食性

habitat bit はゲーム側の分類として `OPEN_WATER=1`、`REEF=2`、`BOTTOM=4` とする。単位は body/prey size が m、energy がゲーム側で統一する資源単位。これらの数値はプリセット例で、同じ食性でも複数の body size と移動 profile を用意できる。

| StableId / 架空種 | Tier | Tags | Diet | BodySize / prey size | Habitat |
|---|---|---|---|---|---|
| 100 / 灯藻資源 | PRODUCER | PLANKTON, PLANT | NONE | 0.02 / 対象なし | OPEN_WATER, REEF |
| 110 / 青灯濾過魚 | PRIMARY_CONSUMER | GRAZER | PLANKTON | 0.30 / 0.005–0.08 | OPEN_WATER, REEF |
| 120 / 追泳魚 | SMALL_PREDATOR | PREDATOR | GRAZER | 0.90 / 0.10–0.65 | OPEN_WATER, REEF |
| 130 / 巡泳捕食魚 | APEX_PREDATOR | PREDATOR | GRAZER, PREDATOR | 4.00 / 0.15–1.50 | OPEN_WATER |
| 140 / 底生掃除魚 | SCAVENGER | GRAZER | CARRION | 0.45 / 0.02–2.00 | REEF, BOTTOM |

大型濾過魚は `CreatePlanktonVariant(150, 8)`、鎖状群体は `CreatePlanktonVariant(160, 12)` で追加し、どちらも `Tier=PRIMARY_CONSUMER, Diet=PLANKTON` を維持する。大きいから APEX に変えず、GRAZER/PREDATOR を diet に追加して肉食にすることもない。必要なら habitat、mouth radius、food cost、moving speed、population cap を別 profile にする。鎖状の描画・節の形状・Particle entity 数を ecology の食性へ混ぜない。

sample の `CreateMovementProfile` は body size の半分を保守的な body radius とし、近傍半径・separation・arrival を明示する。3D 遊泳は `CreateSwimming`、BOTTOM を持ち OPEN_WATER を持たない habitat の例は `CreateFloorFollowing`、床から中心までの offset は body radius + 0.05 m。Tier では移動形態を決めない。asset の +Z forward/+Y up と実寸を caller が確認し、描画 scale を AI の hidden scale にしない。細長い魚・鎖状群体に合わせる最終 profile は art の bounds から調整する。

Producer slot の局所 resource と密度場は別供給経路。プリセットの Producer は drain/reproduction とも 0、ResourceUnits=16 の有限供給源である。密度場の plankton は多数の GameObject や AI agent で表現しない。density の実消費量から得られる energy だけを `ApplyNutrition` へ渡し、同じ資源を slot resource と密度場の両方から二重加算しない。ゲーム側が補充するなら供給経路と予算を明示する。

## 栄養地点と卵床

`AiEcologySite` は最大 64 個の小さな gameplay データで、`ConfigureSites` で設定する。見た目の skeleton/egg model や Particle の状態は資源の正本にしない。

| Site 例 | 種類 | 有限予算 | 対象と位置 |
|---|---|---|---|
| 海底の架空 skeleton | NUTRIENT | Resources=Capacity=120、RecoveryRate=0 | FoodTags=CARRION、SpeciesTagsMask=GRAZER/PREDATOR、REEF/BOTTOM、anchor と radius=4 m |
| 架空卵床 | SPAWNING | SpawnCapacity=16、live+pending の合計で予約を制限 | GRAZER/PREDATOR、指定 habitat、anchor と radius=3 m |

栄養地点の `Resources` は実際に減った量だけ energy にする。明示した RecoveryRate が正なら bounded な回復を使えるが、既定は 0 とし、死亡 slot の死骸や plankton density とは別の reservoir にする。sites も候補 grid へ入り、agent と合わせた candidate 64 予算に含まれる。

卵床の `OccupiedPending` は SPAWN_REQUESTED の予約、`OccupiedLive` は Commit 後の所属個体数を表す。Cancel は pending を戻し、Commit は pending を live に替える。kernel は site の radius 内かを検証し、caller はその範囲でさらに habitat/camera/pool を確認する。卵の描画数は Sites/event の観測値から決め、visual egg を増やしただけで population や出生枠を増やさない。SPAWNING site を設定した場合、対応する tag/habitat または空き枠がない種の予約は出ず、無関係な位置へ fallback しない。SPAWNING site がない場合は caller habitat sampling 経路を使える。

site の actor eligibility には `SpeciesTagsMask` の一致も必要で、NONE は全種許可ではない。sample の `CreateSiteExamples(skeletonAnchor, eggBedAnchor)` を初期設定時に `ConfigureSites` へ渡す。所属個体/予約、または未消費の site event が存在する間に index/StableId を別施設へ差し替える変更は拒否される。

## 固定予算

| 処理 | 上限/頻度 | 意味 |
|---|---:|---|
| ecology 判断 | 2 Hz / 0.5 s | hunger、索敵、捕獲、出生予約をまとめて進める |
| candidate | 64 / actor / decision | サイズ・diet・habitat を調べる候補の上限。全対全や必ず最近傍を保証しない |
| capture | 32 / decision | 成功・失敗・栄養 site の摂食を含む試行枠。上限で未処理が残ったら次の判断へ回す |
| spawn | 8 / decision | birth 候補の予約枠。Scene に自動 spawn する件数ではない |
| slots | 最大 4096 | ALIVE/CORPSE/DORMANT/PENDING_SPAWN を含む所有 slot の容量 |

サンプルの capacity は caller が必要な池/海域の件数で指定し、最大値を毎回満杯に確保する必須設定にしない。`AiCrowd` の移動 decision LOD は near 10 Hz / mid 4 Hz / far 1 Hz を基準とし、ecology 2 Hz と独立に保持する。連続 steering と terrain/habitat 制約は各 movement step 継続する。ecology の候補上限と `AiCrowdSettings.CandidateLimit` は責務が異なるので同じ値へ強制しない。

ecology の HuntRadius は CellSize 以下であることを検証し、既定 CellSize は 8 m。MaxDecisions=1,000、MaxPendingSpawns=min(16, capacity) も固定上限として持つ。sample の総 PopulationCap は指定 capacity、種別 cap は caller が設定する。1,024 個体の海域へ導入する場合、種別 cap、空き slot、corpse/pending の滞在枠を明示して既存個体数を受け入れられる構成にする。

## データの流れ

1. authored position と cached terrain floor/ceiling を caller が用意し、`TryAddAuthoredAgent` で ecology と crowd の slot を同じ index に対応させる。未使用 slot と Producer は crowd 更新を無効にする。
2. `Update` は保持中の移動指示で `AiCrowd.Step` を 1 回呼ぶ。失敗した frame は ecology/density の時計を進めない。長い frame は crowd の MaximumDelta（既定 0.1 秒）へ揃えて clamp し、catch-up loop を作らない。
3. 成功した新しい snapshot を `AiCrowd.CopyPositions` で再利用 `Vector3[]` へまとめてコピーし、`AiEcology.UpdatePositions` へ渡す。後者には容量と同じ長さ、未使用 slot も含めて有限な位置を渡す。これらは managed data 操作で、個体ごとの native position getter を呼ばない。
4. `AiEcology.Step` が true を返す判断時点だけ movement を `AiCrowdAgent.Intent/Target/TargetVelocity` へ変換する。mode/target が変わった slot を `REQUEST_DECISION` で起こす。動いている追跡対象の位置と速度は snapshot から毎 frame 更新するが、それだけで全個体の decision LOD を解除しない。
5. 2 Hz の density/摂食と少数 BT は同じ crowd の指示を更新する。新しい指示は次の movement frame に使う。大型濾過/鎖状群体は十分な密度の feeding goal を探し、mouth sphere で消費できた資源だけ energy に変換する。枯れた場に居続けず、別の goal へ移る。
6. `CopyPoseInputs` が用意する world position/rotation と caller の再利用 GameObject/status 配列を、`AiPoseBatch.TryCommitWorldPoses` でまとめて Scene へ反映する。速度が十分あるときだけ +Z-forward rotation を更新し、停止中は保持する。scale と演出は caller が所有する。

Scene の可視性から既存個体を camera-relative に teleport しない。位置更新と新しい個体を作る処理を分け、通常の移動で habitat に留まる。floor/ceiling sampling は caller の terrain API から渡し、移動中も cached envelope を必要な頻度で更新する。追跡に navmesh や physics contact を追加する場合も caller の別責務とし、球状 local avoidance だけで通路到達を保証しない。

## plankton 密度と濾過食

`AiDensityField(minimum, maximum, cellsX, cellsY, cellsZ, capacity, initialDensity, seed)` の値は energy/m³。固定 grid は最大 32×16×32=16,384 cell、1 query は最大 1,024 cell。sample は小さい field を 2 Hz で `Step(dt, growthPerSecond, recoveryPerSecond, flowVelocity)` し、growth/recovery は 0 を既定として有限資源を保つ。Step の dt は 0–0.5 秒。field は particle entity や Scene object を持たない。

大型濾過/鎖状群体の feeding handler は `TryAttachFilterFeeder(handle, localMouthOffset, mouthRadius, ...)` で最大 8 個体に明示する。`FindFeedingGoal(position, radius, maxCells:64, out goal, minimumDensity)` で十分な密度の reservoir へ巡回する。これは budget 内で閾値を超える近い cell を見つける API で、全 field の最高密度点を保証しない。goal は cell center なので、query radius の外側に半 cell 対角まで出る場合がある。sample は habitat と cached floor/ceiling を確認して crowd の ARRIVE target に使い、安全な候補がないときは WANDER に戻す。

mouth は body position と cached world rotation から `position + rotation * localMouthOffset` で決める。`TryConsumeSphere(mouth, mouthRadius, requestedEnergy, out actualEnergy, maxCells:64)` は sphere と交わる粗 cell の reservoir を使い、実際の減少量を double で返す。粒の数や mouth 内に見える VFX 数から energy を作らない。必要 energy を事前に現在の不足量で制限し、`ApplyNutrition(handle, actualEnergy)` に float 化して渡すときは実消費量より増えない丸めを使う。kernel も energy の加算結果を下向きに丸め、微小な nutrition を足す際の float 丸めで energy が増えすぎることを防ぐ。0 消費なら回復せず、近傍が枯れたら別の feeding goal へ移る。

VFX は `ReadDensity(position)` の値を density 表示・発光・particle rate に使うだけ。設定を変える gameplay が必要なら `TrySetDensity` を明示的に使い、Renderer の LOD/culling で density 自体を増減させない。`TotalEnergy` は O(CellCount) の診断積分で、毎個体・毎 frame の処理へ入れない。

## 出生と object pool

`SPAWN_REQUESTED` は空き slot の予約であり出生完了ではない。caller は species の habitat、terrain floor/ceiling、body radius、壁/障害物、camera の可視性を使って bounded sampling を行う。利用できる object pool/Prefab と安全な位置が用意できた場合だけ `CommitSpawn(handle, position)` を呼び、その位置を crowd/object に対応づける。位置が見つからない、pool が不足、可視領域にしか置けない場合は `CancelSpawn(handle)` を呼ぶ。

sample の `AcceptSampledSpawn` には sampler の habitatSafe/cameraVisible と poolObjectReserved を渡す。不可視かつ安全な候補、予約済み pool object、kernel の site radius が揃った場合だけ Commit し、それ以外は Cancel して有限予算を戻す。pool object の返却と実 GameObject の表示は caller が行う。

既存個体や既存 Scene object の再配置を出生処理の代わりにしない。Commit 前の PENDING_SPAWN は population cap と slot 予算を消費する。handle は Slot+Generation を照合して予約/個体を識別し、キャンセル済み、捕獲済み、再利用済み handle を別の個体へ適用しない。死亡時も generation が変わり、古い ALIVE handle と新しい CORPSE handle は別になる。kernel は Camera、Prefab、GameObject を参照しない。

## 少数の BT と演出

標準の群れは ecology movement と native crowd の意図を使い、1,000 個体へ毎 frame C# BT callback を登録しない。`TryAttachSpecialPredator` は最大 8 個体だけに共有 definition と個体別 `BehaviorTreeInstance` を付ける。`SetPredatorPerception` へ caller が計算した custom danger と任意 waypoint を渡す。BT は 0.5 秒間隔で評価し、bool key の変更で次 Tick の評価を要求する。ReactiveSelector + Guard が FLEE/任意 target/通常 ecology を切り替え、override 終了時は通常指示へ戻す。移動は `AiCrowd` の acceleration/turn/habitat 制約の経路に残す。

`AiEcology.Step(dt)` の true は新しい判断、false は未到達または不正 dt を表す。false のとき kernel の Events/Movements は前回値を保持する。sample は `EcologyEvaluatedThisUpdate` と `NewEvents`（未到達時は空 span）を公開するため、caller は successful Update の直後に NewEvents を一度だけ読む。Commit/Cancel の BORN/SPAWN_CANCELLED は次の true Step へ queue される。crowd の `AiCrowdAgent.Events` は毎 successful crowd Step の新しい snapshot から読む。少数の近い/見える対象に、次の既存 facade を接続する。

| 合図 | 接続例 | 抑制する処理 |
|---|---|---|
| MODE_CHANGED → FLEE/PURSUE | `Animator.SetBool` / `SetFloat` / `SetTrigger` | 値が同じ frame の繰り返し設定、全個体の native getter |
| HUNT_STARTED / ESCAPED / CONSUMED | `Audio.PlayAtPoint` または近傍に選んだ `AudioSource.PlayOneShot` | 全 candidate の音、見えていない全群れの one-shot |
| CONSUMED / RESOURCE / BORN | authored `ParticleSystem.Burst` / `Play` | density の点ごとに GameObject を spawn する処理 |

Audio/VFX の clip・asset・gain・rate・距離・channel cap は scene caller のゲーム設定で持つ。density の表示は `ReadDensity` を読むだけで、VFX が `ConsumeSphere` を呼ばない。採餌による depletion と描画の culling/LOD を分ける。

## 保存と復元

停止した owner update の同じ時点で、次の状態を保存する。save の配列確保/I/O は通常 Tick の無確保契約と分ける。position だけを戻して energy、seed、cooldown、generation を初期化すると同じ続きにはならない。

| 保存対象 | 実 API / 内容 |
|---|---|
| ecology | 再利用 `new AiEcologySnapshot(capacity, speciesCount)` へ `CaptureSnapshot`。Version=1、species、slot/generation/seed、timers、grid/decision cursor、pending、site の有限資源・live/pending、event/queue を含む |
| density | caller の再利用 `double[CellCount]` へ `TrySave(cells, out AiDensitySnapshot header)`。layout/capacity/time/seed/version と energy cells |
| crowd | Snapshot の agent、各 GetProfile、Settings、caller が渡した threat/obstacle 配列。floor/ceiling、velocity/intent、seed、decision/threat timer と DecisionCursor も保存 |
| scene/policy | species StableId とゲーム設定版、slot/generation と object pool の対応、アニメ位相、sample の cached rotation/密度経過時間/feeding 設定と少数 BT の perception |

`System.Text.Json` で ecology snapshot を書く場合は `JsonSerializerOptions { IncludeFields = true }` を使う。公開 field と内部配列を省略すると snapshot が欠ける。Capacity/SpeciesCount と JsonConstructor は復号を支え、RestoreSnapshot は shape/version/有限値/handle/site/予約数をまとめて検証する。density の `TryRestore(in header, cells)` は一致する topology/capacity を検証してから更新する。

復元は I/O 完了後、未公開の新しい ecology/density/crowd と scene binding 候補へ行い、全検証後に owner が組を入れ替える。各 API の復元は別々なので、生きた組へ順次適用して途中で失敗すると全体は atomic にならない。保存 species 自体も復元されるため、インストール済みゲーム設定の StableId/設定版との互換性は caller が別途判断する。Scene object の寿命 ID を古い値のまま信用せず現在の GameObject を再接続し、PENDING_SPAWN の予約は同じ handle のまま再承認または Cancel する。勝手に可視領域へ配置しない。

この authoring sample は複合 save manager や BT cursor/timer の snapshot を実装しない。少数 BT の再構築は perception を復元して Reset する明示的なロード方針であり、その BT まで tick 単位で同じ続きを保証しない。厳密な replay が必要なゲームは追加の BT state 保存または固定入力ログを設計する。ecology/density の決定的復元契約と、視覚/ゲーム独自状態の復元方針を混同しない。

## UE 公式資料との関係と検証

2026-10-11 に公式 [MassGameplay Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine) の data-oriented な処理、simulation/representation LOD の分離、signal による判断の起動を確認した。FBZZ では bulk snapshot、別頻度の ecology/crowd/pose、少数 event の読み取りに適用する。UE の Actor spawning、MassEntity/StateTree の型や全 subsystem は導入しない。BT の根拠と詳細な契約は [script behavior 設計](ai-script-behaviors.md) を参照する。

kernel の独立テストは deterministic replay、候補/捕獲/出生予算、tags/size/habitat、stale handle、energy/resource、reserve/commit/cancel、JSON snapshot 往復を担当側で検証する。サンプルは `Scratch/Tests/AiEcologyExample/AiEcologyExampleTests.csproj` が既存 production の Vector3/Quaternion/Mathf/ApiContract と staged BT/crowd/ecology/density source を直接リンクする。代替型を作るのは live callback/native crowd bridge だけである。

独立 Release harness は 10 tests PASS、warning/error なし。実型で diet と大型 plankton variant、未使用 slot、native 失敗時の時計、visible/site 外出生 Cancel と queue、finite egg capacity、有限 site/density nutrition、2 個体の共有 BT/private state、+Z-forward batch pose、ecology/density の候補復元と不正 version/layout 拒否を確認した。1,000 体×600 frame の managed + mock host 更新で steady allocation 0 B、crowd bridge dispatch 600 回（1 frame 1 回）を確認した。実行時間は `Scratch/Tests/AiEcologyExample/validation-release.txt` に記録する。これは接続測定であり、統計的な速度改善値ではない。バッチ接続と allocation の証拠で、native steering の時間、実 native 呼び出しの cost、pose commit、terrain query、Audio/VFX、GPU、実 Scene の自然さは測定していない。実 marine 移行の行数・重複・挙動・GPU/native 性能比較は統合担当の別検証で確定する。

再実行は、この独立 project だけを `dotnet run --project Scratch/Tests/AiEcologyExample/AiEcologyExampleTests.csproj -c Release` で起動する。共通 engine build、SDK 配布、Editor 起動は含めない。
