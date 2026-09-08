# 実装状況と残り作業

> 親: [企画書](企画書.md) ／ 関連: [未決事項](open-questions.md)

> **2026-09-08 に全面改訂。** 以前の版は「二丁拳銃 → 双剣」の移行計画で、
> その後 2 回コアが変わっている（[捨てたもの](企画書.md#捨てたもの)）。
> 移行の記録が要るときは git 履歴を見ること。

---

## 入っているもの

### 戦闘

| 何 | どこ | 状態 |
|---|---|---|
| **弾き**（窓 0.22 秒 / Just 0.08 秒 / 外し硬直 0.55 秒） | `Player/PlayerParryComponent.hpp` | ✅ |
| **崩しゲージ**（弾き +34〜60 / 斬撃 +3.5〜6 / 減衰） | `Combat/BossBreakComponent.hpp` | ✅ |
| **転倒**（5 秒）と**とどめ**（Q または攻撃 / 3.4m 以内） | `PlayerParryComponent::TryExecute` → `IBoss::Execute` | ✅ |
| **連続弾き**（+15% × 最大 4） | `BossBreakComponent::AddParry` | ✅ |
| **土壇場**（残り HP 1 で崩し ×1.25 / Flux 窓 ×1.5） | `PlayerComponent::DriveLastStand` | ✅ |
| **ジャスト回避 → Flux**（2.5 秒。次の一振りが満溜め全周） | `PlayerComponent::OnPerfectDodge` → `BladeComponent::GrantFlux` | ✅ |
| **一撃の契約**（`PlayerHitKind` / `PlayerHitResult`） | `Combat/PlayerHit.hpp` | ✅ |
| 斬撃 **5 連撃**（段ごとに別クリップ・1 段目が全段の再生速度を決める） | `Player/BladeComponent.hpp` | ✅ |
| 溜め斬り（0.75 秒で満溜め・全周・射程 1.8 倍） | 同上 | ✅ |
| 回避（先行入力 0.15 秒 / 攻撃をキャンセル / 無入力は前へ） | `Player/PlayerControllerComponent.hpp` | ✅ |
| ロックオン（索敵 14m・切り替え） | `BladeComponent` ＋ `AimMarkerComponent` | ✅ |
| 斬撃チェイン（`CHAIN` 表示・転倒中だけ +8%/hit） | `CombatManagerComponent::RegisterBladeChain` | ✅ |

### ボス

| 何 | どこ | 状態 |
|---|---|---|
| **ボス1 ポラリティ・コア**（4 足 / 21 クリップ / 脚 4 本） | `Combat/BossAiComponent.hpp` ほか | ✅ |
| 脚の破壊・欠損・2 本で砲台化・もげた脚の落下 | `BossRigComponent` / `BossLegDebrisComponent` | ✅ |
| **ボス2 ポラリティ・サーペント**（28 → 6 節 / 床の開口） | `Combat/Serpent*.hpp`（7 本） | ✅ |
| ボスカメラ（登場 / 転倒 / 撃破 / とどめ） | `Camera/BossCameraDirectorComponent.hpp` | ✅ |
| 撃破演出（部位の飛散 + 粒になって立ち昇る） | `BossDeathVfxComponent` / `SerpentDeathVfxComponent` | ✅ |
| 崩しゲージの HUD（白へ寄る / 転倒中は琥珀で明滅 / `n / total`） | `BossHealthBarComponent` | ✅ |

> **ラグドールは撤去済み。**転倒は `Boss_Crash` クリップだけ、被弾のひるみは
> 脚の SpringBone ＋ `BossCollapsePostureComponent::Stagger` の傾け。
> 再導入するなら `BossAiComponent::BeginCrash` と `BossRigComponent::Flinch` が差し込み口。

### モーション

**50 本。全部入れ替え済み。**一覧と実測フレームは [player-motions.md](player-motions.md)。
刀を持つ 31 本が `Katana_` 接頭辞、素手の 19 本が接頭辞なし。

判定が出る時刻は `bladeStartup`（最終段は `bladeFinisherStartup`）ただ 1 つが決め、
クリップは `Hit Time / bladeStartup` を再生速度として毎回導出して流す。
**振り抜くコマは必ず判定の瞬間に来る**ので、2 つの数字がずれる余地がない。

### 画面と音

| 何 | どこ | 状態 |
|---|---|---|
| 止め（`HitstopFreeze.hlsl`）/ スロー（`SlowMotion.hlsl`）/ 扉（`ScreenWipe.hlsl`） | [presentation.md](presentation.md#止めスロー扉2026-09-07) | ✅ |
| 危機・被弾・溜めの縁（`GameVignette.hlsl`。方向・鼓動・縁取り） | `ScreenEffectManagerComponent` | ✅ |
| メニューの動きの語彙（滑り込み / 帯 / 解読 / 走査 / 題字の溶け込み） | `Scripts/UI/UiMotion.hpp` ほか | ✅ |
| 斬撃の音（空振り 6 種 / 命中 / 締め / 溜め） | `Utils/SeLibrary.hpp` の `kBladeSwing*` | ✅ |
| 斬撃トレイル | `Vfx/BladeTrailComponent.hpp` | ✅ |
| 弾き / とどめの VFX | `FX_PLR_Parry.vfx` / `FX_PLR_ParryJust.vfx` / `FX_BOSS_Execute.vfx` | ✅ |
| SE の voice プール（Create/DestroyVoice でゲームスレッドが止まる件） | エンジン側 | ✅ |

### 画面遷移とデータ

| 何 | どこ | 状態 |
|---|---|---|
| Title / StageSelect / Stage / Result / Options | `Assets/Scenes/*.scene` | ✅ |
| ステージ台帳（7 枠 / 実体 2 本） | `UI/StageCatalog.hpp` | ✅ |
| 評価とランク（3 軸 / 1 表から 2 画面が引く） | `Game/GameResultState.hpp` | ✅ |
| 設定の宣言簿（`settings::Declare*` 1 行で増える） | `Game/GameSettingsRegistry.hpp` ・ [settings.md](settings.md) | ✅ |
| 設定の保存（`%LOCALAPPDATA%/GreenWare/settings.toml`） | `Game/GameSettingsComponent.hpp` | ✅ |

---

## 2026-09-08 に片付けたもの

| 何 | どう直したか |
|---|---|
| **進行が保存されていなかった** | `StageProgressState` に `Load` / `Save` / `ResolvePath` を足し、`save` ストアの `%LOCALAPPDATA%/GreenWare/progress.toml` へ落とすようにした。読むのは `EnsureInit(save)` で 1 度、書くのは `Commit(index, save)`（[game-flow.md](game-flow.md#保存2026-09-08)） |
| **`maxHealth` が 50** | 既定の **5** へ戻した。被ダメ軸（0/1/3）と土壇場（残り 1）が設計どおり効く |
| **2 面で崩しの数値が違った** | Stage_01 を Stage_02 側へ揃えた（弾き 40 / 斬撃 3.5 / 減衰 2.5 秒・−8）。`BladeComponent` のコメントが宣言している「斬るだけなら約 30 発」と一致する |
| **OPTIONS が銃の説明のままだった** | 行の key を `lgun` / `rgun` / `fire` → `lblade` / `rblade` / `parry` へ改名。文言・解説欄・アイコンを刀と弾きへ差し替え |
| **弾きが OPTIONS に無かった** | `fire` 行（割り当て不可の飾り）を **`Parry` 行**にした。Q / LB が**キーコンフィグで差し替えられる**ようになった |
| **ジャンプが OPTIONS に無かった** | `dodge` と `pause` の間に **`jump` 行**を追加（8 行目）。Space / A が差し替えられるようになった。行は 44px 刻みのまま `pause` を 730 → 774 へ送り、解説欄も 1 行ぶん（806 → 850）下げてある |
| **回避のアイコンが実際の割り当てと違った** | Space / A を出していたが実体は Shift / B。アイコンと `KeyIcons::Prompt()` を直した |
| **タイトルのキャッチ** | 「異なる極は引き合い、ぶつかって壊れる」→「斬撃では崩れない。崩すのは弾きだけ」 |
| **選択画面の右パネル** | 見出し `HP` → **`PARTS`**、値を「脚 4 本」「28 → 6 節」へ。構成と解法も弾きの語へ |
| **Inspector に極性時代の文言が残っていた** | 「Mite の HP 100 に対して 4 回」「その側の銃は撃てない」「帯電中の雑魚を外向きへ」「無敵時間は持たない」を現状へ。ツールチップの旧章番号（8 章 / 11 章）は `Docs/*.md` の参照へ置き換え |

> ⚠ **どれも実機未確認**（ビルドは VSCode / Visual Studio から）。
> シーンを 3 つ（`Options` / `Stage_01` / `Title`）と `PlayerTuning.fzdata` を触っている。

---

## 残り作業

### 1. ステージ 3（ボス 3「ポラリティ・ルーム」）

**設計と AI の骨は入っている** — [boss-loom.md](boss-loom.md) と
`Combat/LoomAiComponent.hpp` / `LoomBossComponent.hpp`。
状態機械・`Parryable` の配線・崩しの受け口・とどめ で腕が 1 本もげるところまで通る。

残っているのは**絵とシーン**。

| # | 何 | 何が確かめられる |
|---|---|---|
| 1 | **箱で 4 連を振る**（`Stage_01` に立方体を置いて `LoomAiComponent` を付ける） | 拍 0.62 秒が弾きの窓 0.22 秒に対して読めるか。**ここが駄目なら全部やり直し** |
| 2 | 締めの「拍を飛ばす」溜め | 連打で流しているとそこで外すか |
| 3 | 梭（跳ぶ手）と幕（走る手） | 3 択が成立しているか |
| 4 | 腕をもぐ → 連撃が縮んでテンポが上がる | 進行が形で読めるか |
| 5 | モデル / リグ / アニメ / VFX | ここで初めて絵を当てる |
| 6 | `Stage_03.scene` と `StageCatalog` の 3 行目 | 台帳は**最後**。`scene` を埋めた瞬間に押せてしまう |

**1〜4 は専用シーン無しで確かめられる。**

台帳へ足すときは、

- `StageCatalog::kStages` に 1 行（`scene` は**シーンが出来てから**埋める）
- `StageProgressState::kCount` は台帳から引くので触らない
- `StageSelect.scene` の Row は **7 行とも既にある**（複製は不要）
- ボスに **`IBoss::Bind`**（`LoomBossComponent::OnStart` に書いてある）
- ボスに `EnemyHealthComponent` と `BossBreakComponent`（無いと終わらない / 崩れない）
- `progress.toml` は列ごとの配列なので、**行が増えても古い保存を読める**（短い方に合わせる）

---

## 触るべき数字

手触りが悪いときに最初に触る場所。**`Assets/Data/` の 2 ファイルが正本。**

| 症状 | 触る値 | ファイル |
|---|---|---|
| 弾きが「押したのに出ない」 | `windowSeconds`（0.22） | `PlayerParryComponent`（Inspector） |
| 連打で弾けてしまう | `windowSeconds` を 0.16 へ / `recoverySeconds`（0.55） | 同上 |
| 斬るだけで崩しが満ちる | `slashGain` / `chargedSlashGain` | シーンの `BossBreakComponent` |
| 連撃全体が速い / 遅い | `bladeStartup`（0.17） — **5 段まとめて動く** | `BladeTuning.fzdata` |
| 締めの重さ | `bladeFinisherStartup`（0.38） / `bladeRecovery`（0.26） | 同上 |
| 回避が走りに埋もれる | `dodgeSpeed`（20）÷ `moveSpeed`（10）の**比**で決まる | `PlayerTuning.fzdata` |
| 回避が瞬間移動に見える | `dodgeDuration`（0.36）と `Dodge_Roll` の `speed`（1.575）は**対** | 同上 ＋ `Player.animcontroller` |

---

## 物差し

設計が効いているかを数字で確かめる。`CombatManagerComponent` に計測を足せば取れる。

| 指標 | 目標 | 何を判定するか |
|---|---|---|
| **1 周あたりの弾き回数** | 15 以上 | 弾きが主要な供給源になっているか。少ないなら斬りで溜まりすぎている |
| **Just 弾きの割合** | 20〜40% | 窓 0.08 秒が読み切りとして成立しているか。60% を超えるなら広すぎる |
| **弾きで得た崩し / 全崩し** | **60% 以上** | 「弾かなければ崩しは溜まらない」が成立しているか。**最重要** |
| 転倒 1 回あたりの経過秒 | 25〜40 秒 | 長いと退屈、短いと弾く緊張が出ない |
| 1 分あたりの斬撃回数 | 30 以上 | 隙に踏み込めているか |
| 被弾回数 | — | HP を 5 へ戻したあとに測り直すこと |

**「弾きで得た崩し / 全崩し」が最重要の指標になる。**
斬るだけで倒せているなら、コンセプトが体験として成立していない。
