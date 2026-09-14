# Player Controller

書式そのものの規則は 1 つ上の `Assets/Animation/README.md`。ここは Player の中身だけ。

Base 全身マスク: `Player_Base`（54 骨）。`M_UpperBody`（44 骨）と `M_Arms`（39 骨）は将来の合成用で、
今はどのレイヤーからも参照していない。承認済みの両手剣モーションが腰・脚・武器の連動を含むので、
**全動作を全身で再生する**。全ステートの IK 重みは 0。

クリップの秒数・ループ・参照の正本は `Assets/Models/Player/Clips.csv`（22 本）。
Controller は原本 FBX ではなく、インポート後の `.anim` を安定した導出 GUID で指す。
接続前に `Models/Player` 配下の FBX を FBZZ Editor でインポートすること。

## パラメーター

| 名前 | 型 | 書く側 |
|---|---|---|
| `Speed` / `VerticalSpeed` | Float | `PlayerControllerComponent`（m/s、上向き正） |
| `IsGrounded` | Bool | `PlayerControllerComponent` |
| `IsBlocking` | Bool | `PlayerComponent`（ガード入力） |
| `IsDead` | Bool | `PlayerComponent`（`!IsAlive()` を毎フレーム） |
| `Jump` / `Dodge` | Trigger | `PlayerControllerComponent` |
| `Stop` | Trigger | 走り停止（未接続） |
| `Slash01` / `HighSpinAttack` / `JumpAttack` / `SlideAttack` / `UnderSlash` / `UnderSlashandUpperSlash` | Trigger | 未接続（下記） |
| `GuardHit` | Trigger | `PlayerComponent`（ガード中の被弾） |
| `Hit` | Trigger | 未接続（下記） |
| `Death` / `Victory` / `Defeat` | Trigger | 未接続。撃破は `animator.Play("Death")` |
| `Reset` | Trigger | 終局 3 種から Locomotion へ戻す |

## 構成

既定ステートは `Locomotion`（`Idle` 0 / `SwordWalk` 2 / `Run_F` 6 の 1D BlendTree、平滑化 0.08 秒）。

**State 側の遷移は「流れ」だけ** — Locomotion → RunStop / ToBlocking、
JumpStart → FallLoop → Land、一撃モーションの復帰（再生し切ってから接地状態で
Locomotion か FallLoop を選ぶ）、ガード 3 種の行き来、終局 3 種 → `Reset`。

**AnyState は「割り込み」だけ** — Death / Defeat / Victory / Hit / GuardHit / Dodge / Jump / 攻撃 6 本。
割り込み側には `IsDead == False` が付いていて、撃破後は Death から蹴り出されない
（終局 3 種は `Reset` まで保持する）。

崖からの落下は `IsGrounded == false` かつ `VerticalSpeed < -0.1`。踏み切りの瞬間は
上向き速度が正なのでこの条件に当たらず、AnyState の `Jump` が拾う。

## 攻撃と被弾のステートは Trigger では使っていない

攻撃 6 本と `Hit` は、実際には **`Attack` / `HitReaction` レイヤーの Slot** で再生される
（`BladeComponent` / `PlayerControllerComponent`）。全身 Override で、
速度は `SetSlotSpeed` が判定時刻に合わせて閉ループで詰める。

Base Layer に同名ステートを置いてあるのは、**そこに載っていないクリップは
`PlaySlot` しても無音で畳まれる**から。加えて Editor 上で 1 本ずつ確認する入口にもなる。
ステートを消すと Slot 側が黙るので、消すときは必ず両方を見る。

レイヤーは 2 枚とも空のステートマシン + Slot 専用、マスクは `Player_Base`（全身）、
`mode = 0`（Override）:

| レイヤー | weight | 重みの駆動元 |
|---|---|---|
| `Attack` | 0.0 | `BladeComponent::DriveSlashLayerWeight`（Slot の重みから毎フレーム） |
| `HitReaction` | 1.0 | 固定。被せ量は Slot のフェードが決める |

## 未接続のもの

- `Stop` / `Hit` / 攻撃 6 本 / `Victory` / `Defeat` / `Reset` の Trigger は誰も送っていない。
- 旧データの二刀・パリィ・登攀の処理は、この Controller では使用しない。
- `Victory` / `DefeatIdle` は `IsDead` では守られない。`Result` シーンに Player が
  置かれていないため現状は到達しない。
