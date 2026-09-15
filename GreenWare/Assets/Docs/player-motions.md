# プレイヤーのモーション — 両手剣 (MiniBot C)

> 親: [企画書](企画書.md) ／ 関連: [二刀](blades.md) [操作・カメラ](camera-controls.md) [演出](presentation.md)
> 正本: `Assets/Models/Player/Clips.csv`（**22 本**）
> 制作データ: `Assets/_src/MiniBotC/MiniBotC_PlayerMotions.blend`
> リグ・書き出し: [minibot-c-humanoid](../../../Docs/design/minibot-c-humanoid.md) ・
> [minibot-c-fbzz-export](../../../Docs/design/minibot-c-fbzz-export.md)
> Controller の配線: `Assets/Animation/Player/README.md`

> **2026-09-14 改訂。** 以前の版は双剣（`Katana_` 接頭辞 50 本・抜刀/納刀・居合・登攀）を
> 書いていたが、**そのクリップは 1 本も存在しない**。
> 旧版は [archive/player-motions-dualblade.md](archive/player-motions-dualblade.md)。

---

## 何があるか（22 本）

秒数・ループ・`.anim` の参照は `Clips.csv` が正本。ここは**ゲーム側が何に使うか**だけを書く。

### 移動・空中

| クリップ | 尺 | ループ | 使う側 |
|---|---|---|---|
| `Idle` | 4.80s | ✅ | Locomotion の 1D BlendTree（0） |
| `SwordWalk` | 1.37s | ✅ | 同（2） |
| `Run_F` | 0.60s | ✅ | 同（6）。再生速度は実速に比例（`runClipSpeed` 6.0 m/s で 1.0 倍） |
| `RunStop` | 0.90s | | 走りからの急停止 |
| `Dodge` | 1.40s | | 回避。**`dodgeClipSeconds` と対**。`dodgeDuration`（0.36）との比が再生速度 |
| `JumpStart` / `FallLoop` / `Land` | 0.60 / 1.00 / 0.70s | 中のみ✅ | 跳躍 |

### 斬撃（6 連）

**1 段目の `Hit Time ÷ bladeStartup` が全段の再生速度**になる（`ComboPlaybackRate`）。

| 段 | クリップ | 尺 | Hit Time |
|---|---|---|---|
| 1 | `Slash01` | 1.70s | 0.80 |
| 2 | `UnderSlashandUpperSlash` | 1.80s | 0.667 |
| 3 | `UnderSlash` | 1.83s | 0.867 |
| 4 | `HighSpinAttack` | 1.87s | 1.133 |
| 5 | `SlideAttack` | 2.13s | 1.30 |
| 6（締め） | `JumpAttack` | 2.17s | 1.133（`bladeFinisherStartup` と対） |

溜め斬りは `HighSpinAttack`、溜めの保持は `Blocking`（ループを持つ唯一のクリップ）、
空中斬りと空中 2 段目はどちらも `JumpAttack`。

> **クリップが足りず、いくつかの役が同じ 1 本を共有している。**
> 締め・空中・空中 2 段目・溜め解放がすべて `JumpAttack`、4 段目と溜めが `HighSpinAttack`。
> 絵として区別が付かないので、**専用クリップを足すならここが先**。

### 弾き（Parry）

ガード 4 本を**押しっぱなしのガードではなく弾きの 3 相**として使う。
配分と速度の導出は `Assets/Animation/Player/README.md`。

| 相 | クリップ | 尺 |
|---|---|---|
| 構え（窓 0.22s） | `ToBlocking` | 0.50s |
| 弾けた（硬直 0.22s） | `GuardHit` | 0.50s |
| 外した（硬直 0.40s） | `BlockingToIdle` | 0.50s |
| とどめ（Execute） | `SlideAttack` | 2.13s |

### 被弾・終局

| クリップ | 尺 | 使う側 |
|---|---|---|
| `Hit` | 1.27s | `HitReaction` レイヤーの Slot |
| `Death` | 2.60s | `animator.Play("Death")` |
| `Victory` / `DefeatIdle` | 4.00s | ✅ Result シーン用（現状は未到達） |

---

## 無くなったもの

| 旧 | 今 |
|---|---|
| 双剣（右=赤 / 左=青） | **刀 1 本**（`Socket_Weapon_R` のみ） |
| 抜刀 `Katana_Draw` / 納刀 `Katana_Sheathe` | **無い。**刀は常に手にある |
| 居合 `Katana_Iai`（とどめ） | `SlideAttack` |
| 受け流し `Katana_Parry` | ガード 4 本（上記） |
| 登攀 `Climb` | **無い。**登攀そのものが 2026-09-10 に取り下げ |
| `FullBody` / 上半身マスクの使い分け | **マスクは 1 枚も使っていない**（全レイヤー全身） |

> **アニメーションイベント（`FBZZ_EVENT__WeaponAttach`）は使っていない。**
> 刀の持ち替えが無くなったため。ソケットは `Socket_Weapon_R` 固定。

---

## 差し替えるときに必ず一緒に直すもの

クリップを入れ替えると、**コード側の既定値は誰も直してくれない**。
2026-09-14 の時点で、シーンだけ新クリップへ移し替えられ、コード既定が
`Katana_*`（存在しないファイル）を指したまま半月以上放置されていた。

| 何を替えたら | どこを直す |
|---|---|
| 斬撃 6 本 | `BladeComponent` の `*ClipFile` / `*ClipName` / `*HitTime` |
| 1 段目 | ＋ `BladeTuning.fzdata` の `bladeStartup`（**全段の再生速度**） |
| 回避 | `PlayerControllerComponent::dodgeClipSeconds` と `PlayerTuning.fzdata` の `dodgeDuration` は**対** |
| 走り | `runClipSeconds` / `runClipSpeed`（足音の間隔がここから出る） |
| ガード 4 本 | `PlayerParryComponent` の `parry` / `guardHit` / `release` の 3 組 |
| どれでも | `Player.animcontroller` に**同名ステートが要る**。無いと `PlaySlot` が無音で畳まれる |

**シーンの値がコードの既定を上書きする。**片方だけ直すと、既存シーンは動くのに
新しいシーンへ Player を置いた瞬間に無音で壊れる。
