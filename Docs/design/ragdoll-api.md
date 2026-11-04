# Ragdollの汎用APIとゲーム側の責務

2026-09-13。RagdollSystemの計算方式は[active-ragdoll.md](active-ragdoll.md)を参照。

## 責務

Engineは骨の選択、物理・関節駆動、姿勢制約、ブレンド、衝撃入力、最終姿勢の同期を担当する。
キャラクターの種類、歩行周期、被弾・死亡の条件、反動の配分はゲーム側に置く。
Mech / Humanoidは選択可能な物理プロファイルであり、Player / Bossの判定には使わない。

## 独立した設定

| API | 変更する設定 |
|---|---|
| SetRoot / SetRoots | 対象部分木と深さ |
| SetExcludedBranches | 指定ボーンとその全子孫の除外 |
| SetProfile | 物理プロファイル |
| SetStandingGuard(enabled, distance, degrees) | 根位置・骨長の保持と目標からの変位・回転制限のみ |
| SetRootAnchor(scale, sag, tiltDegrees) | 自重比の支持倍率、たわみm、傾き度 |
| SetMuscle / SetRecovery | 関節駆動と衝撃後の回復 |
| SetGravity / SetContacts / SetGroundPlane | 重力、接触、補助平面 |
| SetSimulation(substeps, linearDrag, angularDrag, allowRest) | FixedUpdate の分割数1..64、並進/回転抵抗 [1/s]、支持された脱力個体の休止 |
| SetCollapse | ActiveからPassiveへの自動遷移の距離。0は自動遷移無効 |

StandingGuardはプロファイル・支持倍率・重力・接触・駆動モードを上書きしない。
BeginはPassive、BeginActiveはActiveを明示的に選ぶ。Guardを有効にしたPassiveも許可する。
その場合は筋力駆動なしで、姿勢制約だけが目標に対して働く。
回転制限はクォータニオンの最短角0〜180度。ゲーム向けの25度／30度上限は置かない。

## モデル別設定と部位別ドライブ（2026-11-03）

`physicsAssetPath` は GUID または `.ragdoll` への参照。空なら従来のプリセットを使う。
version 1 の TOML は `[ragdoll]` と `[[bodies]]`、`[[collision_exclusions]]` を持つ。
`definitions_only=true` では列挙した骨だけを剛体にする。指など剛体を持たない子孫も、最終親姿勢へ追従する。
骨に子がない場合も明示形状を生成できる。形状は capsule / sphere / box。
中心と関節アンカーはボーンローカルの m、回転は同空間の xyzw quaternion。
寸法は半径・カプセル中心線の半高・箱の半辺長。非一様スケールの形状寸法は最大絶対軸で保守的に拡大する。
mass=0 は密度から計算し、正値は kg。inertia=[0,0,0] は形状から計算し、正の3成分は剛体空間の対角慣性 kg·m²。
関節可動域は joint_rotation が定める基準からの degrees。
不正な設定の読み込みは使用中の定義を保つ。初回失敗は InvalidPhysicsAsset。内容署名が同一なら他アセットの更新で再構築しない。

C# の操作は FixedUpdate / Play 内で行う。

- `SetPhysicsAsset(reference)`: 次の更新で物理定義を読む。空文字はプリセットへ戻す。
- `SetBodyDrive(bone, settings)`: 正確な骨名の構築済み剛体を上書きする。見つからなければ false。設定は再構築にも引き継ぐ。
- `ClearBodyDrive(bone)`: 保存された部位設定へ戻す。上書きは Play のランタイムだけに保持し、アセットへ保存しない。
- `SetSurface(staticFriction, dynamicFriction, restitution)`: 静摩擦・動摩擦・反発を独立に設定する。相手との合成は PhysicsMaterial の規則に従う。

`RagdollDriveSettings` は Enabled / Space、LinearFrequency / AngularFrequency [Hz]、
LinearDampingRatio / AngularDampingRatio、MaxForce [N] / MaxTorque [N·m]、
TargetVelocityMultiplier を持つ。Space は Parent / World。Parent は相手の剛体へ反作用を返し、World は外部支持となる。
周波数または上限0は該当する駆動を停止する。UEの上限0=無制限とは異なる。
明示ドライブを停止しても旧サーボや根アンカーへ自動的に切り替えない。
全体の SetMuscle は出力に倍率、周波数に倍率の平方根を掛け、減衰比を保つ。Passive は全駆動を停止する。

## ボーン選択と保存

rootBoneName / extraRootBones / excludedRootBonesはInspectorで編集でき、シーン・Prefabへ保存される。
除外はノード名で解決し、部分文字列による「脚」などの意味判定はしない。
除外は追加の根より優先する。存在しない除外名はリグ構築時に警告する。
除外した子孫は物理を持たず、元の親ローカル姿勢を最終親姿勢へ伝播する。
プロファイル自身の除外規則は引き続き適用される。

## 衝撃入力

- Push / PushAt: ワールド並進速度の加算[m/s]。角速度を暗黙に加算しない。
- PushAngular / PushAngularAt: ワールド角速度の加算[rad/s]。並進速度を変更しない。
- Atは指定原点からの距離で減衰し、radius <= 0は全剛体へ一律。
- 入力の意味はGuardや駆動モードで変えない。方向・配分・演出上限は呼び出し側が決める。

## 部位操作・支持観測・独立把持（2026-11-03）

[制御の詳細](ragdoll-controls.md)を参照。スクリプトが入力・足運び・姿勢遷移を所有する。

| C# API | 用途 |
|---|---|
| GetBodyName / GetBodyState | 物理部位の列挙、重心姿勢・質量・速度・支持状態 |
| GetBodyContact | 部位の接触点・法線・相手速度・自己接触の区別 |
| TryGetBodyDrive | 保存設定またはランタイム上書き設定の取得 |
| SetBodyTarget / ClearBodyTarget(s) | Parent/World空間のCOM目標と速度、位置/回転の独立した混合率 |
| ApplyBodyImpulse / ApplyBodyImpulseAt / ApplyBodyAngularImpulse | 部位へのワールド力積 [N·s / N·m·s] |
| ApplyBodyForce / ApplyBodyForceAt / ApplyBodyTorque | 正の秒数だけ加える力・トルク [N / N·m] |
| GetBalanceState | 全体重心、支持凸包、重心余裕、CapturePoint近似と有効フラグ |
| RagdollBalanceUtility.ComputeAcceleration | 支持面に沿う有限PD加速度の純managed計算 |
| GrabWorld / GrabBody | 骨ごとの固定点/現行RigidBodyへの把持。位置のみ/回転保持を選択 |
| GetGrabState / ReleaseGrab / ReleaseAllGrabs | 左右独立の状態取得と解除、破断理由の取得 |

変更はFixedUpdate / Play。観測は直前に解いた状態で、Rigの構築前にはValid=falseを返す。
EditでもsimulateInEditorで構築したRigがあれば、その状態を観測できる。
手動目標は有効なSetBodyDriveとActiveを必要とし、standingGuardの最終姿勢制限も受ける。
力・力積はPassiveでも作用する。Push系の速度加算とは質量・慣性への依存が異なる。
継続力を繰り返し呼ぶと加算する。毎FixedUpdateで使う場合は、その命令間隔をdurationにする。
手動目標・継続力・把持はRig再構築で消える。保存されたモデル別ドライブは変えない。

つかみの対象点と到達距離は呼出側が選ぶ。到達距離は部位COMからの距離で、
初回要求の失敗は現在の把持を保持する。対象RigidBodyは同じSceneの現行Component参照に限る。
対象の破棄はTargetLost。ForceLimit / DistanceLimit / Releasedを区別し、自動で再把持しない。
動的な把持対象と把持元Rigの相互衝突は除外し、最後の解除後に全形状が2cm以上離れてから戻す。
静的な支持面・床・他物体との接触は継続する。
Passiveでは把持を維持し、End完了・無効化・非active階層・再構築ではすべて解除する。
対象を無効化/非activeにするゲーム処理では、破棄と異なり実体が残るので明示的にReleaseGrabする。

バランス観測は自己接触・把持点を接地に数えない。支持余裕は正が凸包内、負が凸包外。
CapturePointは一定重心高さの近似で、無支持・無重力・高さ不足ではCaptureValid=false。
歩行生成・自立保証・起き上がりはこのAPIに含めない。

## GreenWareへの旧移行記録 (SDK 0.9.1)

`Assets/Scripts/Utils/RagdollPresentation.hpp`が立位設定セットと演出用の並進／回転配分を持つ。
Playerの周期的な揺れと入力タイミングはPlayerComponentに残す。
Boss01の除外名Yaw_FR / Yaw_FL / Yaw_BR / Yaw_BLはBossRigComponentとStage_01.sceneが指定する。

旧preserveLegAnimationとSetLegAnimationPreservedは削除し、該当シーン・スクリプトを移行した。
旧SetStandingGuard(distance, degrees, human)は独立した設定APIへ置き換えた。
当時追加したユーティリティは Engine の C++ API であり、ゲーム作者向け Script API ではない。

## 検証

回帰テストに設定の非干渉、制約の解除、Passive要求の保持、任意名の枝の除外、
追加根より除外を優先すること、リストの保存・復元、並進・回転入力の独立性を追加。
C++ビルド・テスト実行・GreenWareでの見た目確認は未実施。
EngineとScriptsのAPIが変わるため、全体ビルドとEditorの再起動が必要。
