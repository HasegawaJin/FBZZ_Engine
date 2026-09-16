# Player Controller

書式そのものの規則は 1 つ上の `Assets/Animation/README.md`。ここは Player の中身だけ。

**マスクはどのレイヤーも使っていない**（`baseLayerMaskPath` と 2 レイヤーの `maskPath` はすべて空 = 全身）。
承認済みの両手剣モーションが腰・脚・武器の連動を含むので、**全動作を全身で再生する**。
全ステートの IK 重みは 0。

`Player_Base.mask`（`Root` 配下）を全身マスクとして指していたが、**あれは全身ではない**。
骨ルートより上の `RootNode` / `PlayerRig` / メッシュ 2 ノードが重み 0 に落ち、
重み 0 のノードはバインドへ固定される。`PlayerRig` は Blender の軸変換を持ち
全クリップがトラックを持つので、固定すると毎フレーム引き戻される。
`M_UpperBody`（44 骨）と `M_Arms`（39 骨）と併せて将来の部分合成用に残してある。

クリップの秒数・ループ・参照の正本は `Assets/Models/Player/Clips.csv`（22 本）。
Controller は原本 FBX ではなく、インポート後の `.anim` を安定した導出 GUID で指す。
接続前に `Models/Player` 配下の FBX を FBZZ Editor でインポートすること。

## パラメーター

| 名前 | 型 | 書く側 |
|---|---|---|
| `Speed` / `VerticalSpeed` | Float | `PlayerControllerComponent`（m/s、上向き正） |
| `IsGrounded` | Bool | `PlayerControllerComponent` |
| `IsBlocking` | Bool | 未接続（2026-09-14 にガードを撤去。下記） |
| `IsDead` | Bool | `PlayerComponent`（`!IsAlive()` を毎フレーム） |
| `Jump` / `Dodge` | Trigger | `PlayerControllerComponent` |
| `Stop` | Trigger | 走り停止（未接続） |
| `Slash01` / `HighSpinAttack` / `JumpAttack` / `SlideAttack` / `UnderSlash` / `UnderSlashandUpperSlash` | Trigger | 未接続（下記） |
| `GuardHit` | Trigger | 未接続。弾きの成功は `Attack` レイヤーの Slot で鳴らす（下記） |
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

レイヤーは 3 枚とも空のステートマシン + Slot 専用、マスク無し（全身）、`mode = 0`（Override）:

| レイヤー | weight | 重みの駆動元 |
|---|---|---|
| `Attack` | 0.0 | `BladeComponent::DriveSlashLayerWeight`（Slot の重みから毎フレーム） |
| `Attack_B` | 0.0 | 同上。**段ごとに A / B を交互**に使い、繋ぎをクロスフェードする |
| `HitReaction` | 1.0 | 固定。被せ量は Slot のフェードが決める |

> **`Attack_B` はコンボの繋ぎ専用（2026-09-14）。** Slot はレイヤーに 1 本しか無く、
> `PlaySlot` は差し替え時にクリップを `time = 0` で即入れ替えて weight を保持するため、
> 1 枚だと段と段が**ハードカット**になる（`slashFadeIn` は「ロコモーションから攻撃へ入る」
> ときにしか効かない）。2 枚を交互に張れば、前の段が `slashFadeOut` で抜けながら
> 次の段が `slashFadeIn` で乗る。`DriveSlashLayerWeight` は 3 枚とも
> 自分の Slot の重みで駆動する ── 乗っている 1 枚だけを駆動すると、
> 前の段のレイヤーが最後の weight で止まって二重に被さる。

## ガード 4 本は弾き（Parry）のクリップ（2026-09-14）

`ToBlocking` / `Blocking` / `GuardHit` / `BlockingToIdle` は**押しっぱなしのガードではなく、
弾きの 3 相**として使う。`PlayerParryComponent` が `Attack` レイヤーの Slot へ差す:

| 相 | クリップ | 速度 |
|---|---|---|
| 構え（窓 0.22 秒） | `ToBlocking` | Guard Snap で**終端まで駆け抜けて留める** |
| 押しっぱなし（ガード） | `Blocking` **ループ** | 等速。差し替えないので姿勢が動かない |
| 受け止めた（弾き / ガード） | `GuardHit` | 0.50 / 0.22 = **2.3** |
| 離した・外した | `BlockingToIdle` | 0.50 / 0.40 = **1.25** |

**継ぎ目は 4 本とも「構えの姿勢」で揃う。**`Guard Pose At` をクリップ終端に置いてあるのは
このため ── `ToBlocking` の最終コマ＝`Blocking` の頭＝`GuardHit` の頭＝`BlockingToIdle` の頭。
一致した継ぎ目はハードカットでも見えないので、**1 枚のレイヤーで差し替えてよい**。

> **2 枚（`Attack` / `Attack_B`）でクロスフェードしてはいけない。**弾きの 4 本は
> 1 続きの動作の途中経過なので、混ぜると腕が 2 つの時点の平均を通って「カクッとズレる」。
> 2 枚が要るのは段どうしが別の動きである**連撃だけ**。

`Blocking` は溜め斬りの保持（`BladeComponent` の Charge Hold）とも共有している。
溜め中は構えられないので衝突しない。

## 未接続のもの

- `Stop` / `Hit` / 攻撃 6 本 / `Victory` / `Defeat` / `Reset` の Trigger は誰も送っていない。
- `IsBlocking` / `GuardHit` も同様。ガード 3 種のステートは Slot の置き場として残す
  （上記のとおり、そこに載っていないクリップは `PlaySlot` しても無音で畳まれる）。
- 旧データの二刀・登攀の処理は、この Controller では使用しない。
- `Victory` / `DefeatIdle` は `IsDead` では守られない。`Result` シーンに Player が
  置かれていないため現状は到達しない。
