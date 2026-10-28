<!-- @file    ai-infrastructure-audit.md -->
<!-- @brief   Audit, bounded AI contracts, measured evidence and marine migration ownership. -->
<!-- @author  Hasegawa Jin -->
<!-- @date    2026-10-11 -->
# AI基盤監査と海AI移行契約

2026-10-11。対象は既存FBZZ AI/BehaviorTree、C# SDK、海AIの再利用部分。既存ゲームSceneとconsumer scriptは統合担当が所有する。ここでは共通処理、専用テスト、新規authoring exampleを作る。commit/push/Editor起動は行わない。

## 監査結果と実装

|領域|既存の問題または有効な機能|今回の完成範囲|
|---|---|---|
|native BT構造|DFS格納に対して一部compositeがfirstChild+ordinalで兄弟を参照していた|subtreeEndを使って直接の子を走査。共有immutable treeと個体stateは継続|
|Running/abort|parallelやTimeLimitの残った枝、ガード閉鎖時のaction cleanupが不十分|全Runningノードの追跡、abortを1回、self/lower observer、終了済parallel枝の再実行防止、compile depth256上限|
|Blackboard|型付きstorageとwrite historyは既存。整数比較でfloatへ変換する経路があった|整数を整数で比較。初回/実変更のみrevisionを増加、Resetも増加。同値writeのhistoryは保持|
|native BT scheduler|tickRate、個体phase分散、debug node statusを既存利用|累積経過時間、BB revision/明示RequestEvaluation/target visible-id遷移でwake、pause/disable/reload/restartで旧actionをabort。Patrol actionがroute許可を所有し、Wait/pause中に別Systemが移動を再開しない|
|C# authoring|既存native RunScript actionはFailureで、custom condition/actionを追加できなかった|純managed companion BTを追加。型付きkey、共有定義+個体instance、reactive guard、任意action/target/strategy。native JSON RunScriptにcallbackを接続する機能は今回追加しない|
|海の近傍/群泳|consumerは既にlinked-grid、36候補/12近傍、decision LODを実装|native CrowdSimulationへ共通化。stamp付き3D grid、群れ/索敵、escape/pursue/arrive、加速/旋回制約、3D habitat、cached floor/ceiling、sphere回避、stuck recovery|
|interop|位置batchは既存だがrotationの個体呼出しが残る|crowd計算1回+world pose commit1回/region。更新後snapshotをmanagedでcue/audio/VFXが参照。全個体callbackは行わない|
|表現|Events、Animator、audio/VFXのC# APIは既存|mode/event bitsを返す。scriptが少数の必要な変更だけ既存APIへ通知|

native BTのobserverは評価時に働く。全関連キーのsubscription graphを実装したUE方式と同等とは主張しない。BBの全typed変更revisionは意図的にwakeするため、毎frameの座標をBBへ書かず連続移動入力をCrowdへ分離する。時刻条件は定期tickまたはRequestEvaluationを併用する。

## APIと所有境界

`Fbzz.AI.AiCrowd`はcaller-owned contiguous snapshotを保持する。speciesごとの`AiCrowdProfile`、個体の`Intent/Target/ExternalVelocity`で設定し、1回の`Step`で計算する。C#固有の条件はnativeへの関数callbackでなく、低頻度/イベント時のmanaged policyで入力へ反映する。少数の複雑な捕食者には`BehaviorTree`/`BehaviorTreeInstance`を使える。新規例は`AiCrowdExample.cs`と`ScriptBehaviorExample.cs`。

`AiPoseBatch.TryCommitWorldPoses`はposition+quaternionを同時に公開する。全入力とhierarchy mathをshadowで検証してからcommit。null/expiredはstatusでskip、別Scene/physics body/不正値/親子同一batchは原子的に失敗する。world rotationは速度から生成し、毎frameのrotation getterを避ける。モデル軸は+Z forward/+Y upに合わせ、offsetがあるassetは設定に持つ。

`AiEcology`は純managedの低頻度foodwebを所有し、Crowd snapshotをbulk入力する。Tierは役割表示、実際の摂餌適格性はtags/size/habitatで判定する。`AiDensityField`はenergy/m³の粗い3D場を所有し、粒子entityとは独立する。詳細は別のecology/density契約を参照。Scene spawn、floor/ceiling sample、animation/audio/VFX、Inspector設定はconsumerの責務。

`AiCrowdQueries.CopyNearestMoving`は保存snapshotだけから少数の動く個体を選ぶ。native呼出しやheap確保はなく、.25秒に2個のcueを取る用途に使える。`SchoolingJewelsAiProfiles.cs`は納品manifestの4種の実速度、離隔、alignment/cohesionと床上高さ帯を設定し、`ApplyHabitat`でcached terrain envelopeへ変換する。asset GUID・baked clip・Sceneへの接続は統合担当が行う。

## 実装上限と性能の意味

crowd最大4096個体、64profile、64threat、256sphere obstacle。既定候補36/近傍12、判断予算256個体、近10Hz/中4Hz/遠1Hz。round-robinで飢餓を避け、判断遅延をDecisionAgeで公開する。dangerは判断予算/遠距離LODを待たず連続steeringへ入る。habitat安全は毎更新で適用する。姿勢LODと判断LODを分離する。最大movement dtは0.1秒で過大積分を抑え、判断/恐怖の時計は実deltaを保持する。

agent grid構築O(N)、判断O(decisions×候補上限)は維持する。2026-10-15の脅威/球障害物broadphaseは、各Stepで64/256件までのAABB BVHを構築し、通常の連続移動を空間候補へ絞る。8件以下・広すぎるquery・64nodeの探索上限では元の線形scanへ戻す。投影後は移動したnextで再照会し、元index順の後続だけを評価する。再照会4回後は残りを元順序で走査し、巨大球・密集・長い投影連鎖でも仕事量を固定上限内に保つ。詳細は [Crowd snapshot bridge](ai-crowd-bridge.md#threat-and-obstacle-broadphase)。

脅威の等strength tie/途中memory、球の正確な半径判定、毎更新の安全補正を変えない。region分割は引き続き入力件数と関心領域を制御する。raycast/pathfinding、mesh障害物、動的physics body群への対応は範囲外。壁や侵入時の安全補正は加速/旋回制約より優先する。違反数/decision数/近傍候補数を既存statsで測り、BVHの内部件数をpacket ABIへ追加しない。

### 独立CPUの証拠

|対象|結果|測定範囲|
|---|---|---|
|native BT+Blackboard|14+22件PASS|実sourceと実TestKitのassert helper。fixtureのEngine初期化のみ隔離stub|
|native Crowd+packet境界|25件PASS（kernel17、transfer8）|実kernelと実転送source。実delta時計と個体floor/ceiling内のwander目標を含む。Scene/GPUを含まない|
|Crowd1000体×600更新|p50 0.1442ms、p95 0.1590ms、max 0.2173ms、steady heap0回|MSVC /O2 /MD、60Hz、120warmup、sphere/threatなし。平均候補4014.08、判断142.91/frame|
|Crowd1000体、障害物64・脅威3|p50 0.3953ms、p95 0.7352ms、max 5.0003ms、steady heap0回|上記に続く600更新。平均候補3748.76、判断143.26/frame。共用hostの一測定で負荷を隔離していない|
|Crowd C# facade+Host marshaller|44checks PASS、1000×300stepでdispatch300、managed0B|実facadeをCdecl mockへ接続。native計算やGPU時間ではない|
|C# BT|21件PASS、1000×600tickで両経路0B|周期判断11.94ms合計、毎tickイベント30.747ms合計。移動/描画を含まない|
|Pose C# facade+Host marshaller|8groups PASS、1000×100commitでdispatch100、managed0B|Sceneの原子性は別native Scene tests8件を用意|
|snapshot cue query|4groups・37checks PASS、1000×300queryで0B/native0回|距離順・安定同順位・無効/停止除外・出力tail保持。2個ずつ計600cue|
|Ecology|14件PASS、持続1000体128更新のp95 0.3812ms、warm0B|bulk位置更新+生態Step+snapshot保存。予算・有限資源・出生・復元・密集候補巡回・実栄養量以下の丸めを含む|
|Density|10groups・1717checks PASS、16384cells×100反復51.24ms、warm0B|Step/query/consume/save/restore、保存則と実量消費。粒子やGPUを含まない|
|Ecology authoring sample|10件PASS、1000×600更新でdispatch600・0B|実API/math source+mock host。出生/施設/濾過摂餌/少数BT/+Z姿勢を検証。実native呼出し時間を含まない|
|production C# build|ScriptApi/ManagedHostと全authoring例がwarning0/error0|実project/sourceを独立コピーへ集め、staged sourceと同じop340 contractでRelease build。SDK配布は未実施|
|Pose transfer/BT System/Patrol/Runtime|実sourceの独立C++構文compile成功。Scene tests8+3も構文compile成功|共有Runtime/Scene testsのリンクと実行は共通build枠待ち。構文成功を実Scene PASSと扱わない|

999,000全対全候補との数学的比較はアルゴリズム説明であり、現consumerの性能baselineではない。現consumerはすでに候補数を制限している。測定結果をGPU/frame改善率として使わない。

過去の同じ通常workload測定はp95 0.1475msと0.3914ms、最大0.1693msと7.8526msだった。host負荷で変動するため最新の単測定を保証値とせず、統合時に同条件で再測定する。証拠はworkspaceの`ScratchTests/CrowdTransferTests.json`と同名log、`BtTests.json`、`FullManagedBuild.log`、`AuthoringCheck.log`に保存している。

## consumer移行と比較

読取baselineは`Scratch/Aquarium/OceanReefV2Consumer`の1024魚。Marineは845行/非空817行、Benthicは284行/非空273行。旧単独Marineは802行/非空776行。生成/設定/animation/terrain/特殊生態を含むため、API helperの短さと直接比較して削減率を主張しない。

1. 1024体baseline、seed、population、habitat、profile、observerのログを保持する。Sceneをcamera-relativeに生成/teleportしない。
2. Marine/Benthicの設定をprofileとagentに変換。中層遊泳と床追従を別profileにする。regionごとに一つのAiCrowdを保持する。
3. `BuildGrid/GridCoordinates/Cell/Steer/Limit/Clamp`と近傍群泳/危険伝播をCrowdへ移す。独自goal/生態判断はscript/BTに残し入力を変更する。
4. floor/ceilingはTerrain担当APIで空間/時間cacheしbulk入力。岩はregionごとのsphere配列。毎魚毎frameの64岩×3projectionを再現して新APIへ持ち込まない。
5. Crowd.Step後のsnapshotからrotation/poseLODを生成し、AiPoseBatchへ一回commit。変更mode/実速度だけ既存Animator APIへ、音/VFXは.25秒等のcadenceでsnapshotを少数queryする。
6. ecologyイベントを必要なpolicy変更へ変換。死亡/出生はgenerationhandleとScene objectの対応を更新する。卵や死骸の見た目の個数を生態の正にしない。

移行後に同一条件で記録する指標はscriptの総行/非空行と削除method一覧、native dispatch/frame、CPU p50/p95/maxとGC、候補数/判断遅延、floor/ceiling侵入と障害物constraint、速度/旋回の不連続、逃走遅延、population/摂餌/出生予算、アニメとcue状態遷移。現段階で実Scene移行後の行数・GPU・見た目比較は未測定で、統合担当が実施する。

## 共通hookの反映

`AiSharedHooks.patch`はRuntime/Contract/Tests CMakeの3ファイルだけ。操作番号340（220は既存CAPTURE_RELOAD）、index0計算/index1姿勢。scope/generation/main-thread/callback policyの後、Play限定で分岐し、汎用target解決の前に処理する。既存operation番号/packet layout/base bridge interfaceを変更しないadditive拡張。SDK/Host/nativeは同じcontractとnew sourceを同時に配布する。schema hashやSDK反映順は親の共通ABI判断へ委ねる。

既存Engine CMakeのAI/*.cppとScene/Managed/*.cpp globを再configureし、Tests CMakeへ5テストsourceを追加する。競合中の共有Runtimeを古いsnapshotで上書きせず、親がcontext patchをまとめて適用する。BT担当の7ファイルは本番へ反映済み、その他はStageでcoherent unitとして引き渡す。既存BT/Patrol専用ファイルの変更もcontext patchを確認して適用する。

## 公式資料から採用した考え方

- [Epic Behavior Tree overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview): observer abortと変化で判断する考え方を採用。nativeの完全なsubscription/service frameworkは必要な段階へ延期。
- [Epic Mass Gameplay](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine): データbatch、simulation LODと表現LODの分離、StateTree設定と連続movement処理の責務分離を採用。MassEntity全体やUE依存は導入しない。
- [Epic AI Perception](https://dev.epicgames.com/documentation/en-us/unreal-engine/ai-perception-in-unreal-engine): 知覚結果と判断を分け、刺激の記憶と変化通知を使う設計を採用。海の必要な3D近傍/危険に絞る。
- [Reynolds Boids](https://www.red3d.com/cwr/boids/): cohesion/alignment/separationと空間探索を採用。fish個体全対全を避ける。

公式資料は2026-10-11に確認。設計の参考でありUE実装sourceを移植していない。
