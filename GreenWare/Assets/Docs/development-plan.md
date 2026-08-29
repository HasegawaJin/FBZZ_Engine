# MVP と開発計画

> 親: [企画書](企画書.md) ／ 関連: [未決事項](open-questions.md)

## 双剣への移行で、何が残り何が消えるか

**極性システムは 1 行も捨てない。**消えるのは「極を乗せる入口」だけである。

| 残るもの | 状態 |
|---|---|
| 引力（溜め → 加速 → 衝突の3段階） | ✅ そのまま |
| 反発（同極・即時インパルス） | ✅ そのまま |
| 質量差（Mite / Serpent / Roller） | ✅ そのまま |
| 集束（多対1）とコンボ | ✅ そのまま |
| 撃破コア（一時アンカー） | ✅ そのまま |
| プレイヤーの極性（引かれる / 弾かれる） | ✅ 入口が回避 → 斬撃へ変わるだけ |
| 中央ハザード / 実効半径20m | ✅ そのまま |
| Wave制・追加供給・ランク評価 | ✅ そのまま |
| ボス（極の切替・4攻撃・21クリップ） | ✅ そのまま |
| 反発リング / 撃破コアのシェーダー | ✅ そのまま |
| 演出基盤（ヒットストップ / 揺れ / 振動 / VFX） | ✅ そのまま |

| 消えるもの | なぜ |
|---|---|
| 極性レーザーの照射・なぞり・塗り時間・貫通 | 斬撃に途中の状態が無い |
| 照射バッテリー | 天井は「間合いを詰める時間」が果たす |
| タップの点付与 | 入力が「振る」1種類になった |
| クロスヘア | 狙う1点が無い |
| エイム9セル（AimYaw / AimPitch / Chest ひねり） | 腕で狙う姿勢が無い |
| 極性回避（Space + トリガー） | 斬撃に統合 |
| 銃モデル 2 種・抜刀 / 納刀・発砲リコイル | 双剣へ差し替え |

---

## 必要なモーション

**ここが最大の山である。**近接アクションの手応えは 9 割がここで決まる。
銃は「撃つ」1 モーションで成立していたが、剣は連撃が繋がって初めて気持ちよくなる。

### 作った（50本）

**モーションは全部入れ替えた。**一覧と実測フレームは [player-motions.md](player-motions.md)。
刀を持つ 31 本が `Katana_` 接頭辞、素手の 19 本が接頭辞なし。
下の「新規に要るもの」は当初の見積もりで、実物はこう着地した。

| 当初の見積もり | 実際に入ったもの |
|---|---|
| `Slash_R1/2/3` `Slash_L1/2/3`（段ごとに 6 本） | `Katana_Slash_R` / `Katana_Slash_L` の 2 本。段は再生速度と硬直で分ける |
| — | `Katana_Slash_Dual`（3段目 = フィニッシュ。両刀を交差させて斬る） |
| `Slash_Dash` | 専用クリップは作らず、`Katana_Slash_R/L` に踏み込み速度を重ねる |
| `Blades_Draw` / `Blades_Holster` | `Katana_Draw` / `Katana_Sheathe` |
| `Pose_GripBlades` | `Pose_GripKatana`（書き出し時の基準ポーズも兼ねる） |
| — | `Katana_Iai` / `Katana_JumpSlam` / `Katana_Parry` / `Katana_Victory` / `Katana_Cheer` / `Katana_Lose` |

> **WHY 段ごとに別クリップを作らなかったか:** 左右で軌道を分ける理由（どちらの極を
> 乗せたかが画面から読める）は 1・2 段目で成立している。段の違いは「速さと止まり方」で
> 出る方が強く、6 本を別々に作って全部を同じ質に保つより、2 本を詰めた方が
> **9 割を占める「モーションと当たり判定の噛み合い」に時間を回せる。**
>
> **WHY 3段で止めるか:** 4段以上にすると繋ぎ切ることが目的になり、盤面を見なくなる。
> 本作の思考は「次にどちらの剣で斬るか」であって「入力を続けられるか」ではない。

### 捨てた上体レイヤー

`Pose_AimUpper_*`（銃を構えて走る加算ポーズ）は使わなかった。
`Katana_` のロコモーション 9 本が最初から刀を持った姿勢で作られているので、
上体を加算で作り直す必要が消えた。9 セルのエイム（`Aim_*`）も同じ理由で外してある。

### 捨てるもの（16本）

`Aim_CC` `Aim_CD` `Aim_CU` `Aim_Idle` `Aim_LC` `Aim_LD` `Aim_LU` `Aim_RC` `Aim_RD` `Aim_RU`
（9セル + Idle）/ `Fire_Pistol_L` `Fire_Pistol_R` `Fire_Pistol_Dual` `Fire_Dry` /
`Draw_Pistols` `Holster_Pistols` `Pose_GripPistols` `Pose_FireZero`

> **消す前に確かめること:** `Pose_FireZero` は加算レイヤーの基準ポーズで、
> `Add_Fire` を消してから外すこと。順番を逆にすると、
> 基準の無い加算が適用されて上体が壊れる。

### モデル

| 何 | 要点 |
|---|---|
| 双剣（左右） | 短めの直剣。全長1.2m前後。刃は無彩色、**峰の極性ラインだけ発光** |
| 手のソケット | `SOCKET_Muzzle` は不要。`SOCKET_Hand_L` / `_R` を使う |

`WPN_Pistol_L` / `WPN_Pistol_R` は削除せず残す（差し戻せるようにしておく）。

---

## 実装の状態

**`Main.scene` を Play すれば、二刀を持って走り、斬り、引かれて飛ぶところまで通る。**

| 何 | どこ | 状態 |
|---|---|---|
| 斬撃（発生・判定・連撃・踏み込み） | `PolarityBladeComponent` | ✅ |
| 極の付与 / 中和 | `PolarityTargetComponent::Apply` を斬撃から呼ぶ | ✅ |
| **振っている間だけ自分も帯びる** | `PlayerPolarityComponent::Charge` | ✅ |
| 斬撃の小ダメージ | `CombatManagerComponent::DamageEnemyDirect` | ✅ |
| ノックバック（最終段だけ大きい） | `PolarityBodyComponent::ApplyRepulse(fromField=false)` | ✅ |
| ヒットストップ・揺れ・振動 | 斬撃は弱、衝突は強で分離 | ✅ |
| 斬撃モーション | `Attack` 加算レイヤーへ Slot で差し込む | ✅ |
| 二刀のロコモーション・被弾・抜刀 | `Player.animcontroller`（Base Layer） | ✅ |
| 双剣モデル | `WPN_Sword_R/L` を `SOCKET_Katana_R/L` へ追従 | ✅ |
| 斬撃の音 | 命中は `kImpactLight` を間借り。**空振りは無音** | **未録音** |

### 斬り抜けと判定を «合わせない» ようにした

判定が出る時刻は `bladeStartup`（最終段は `bladeFinisherStartup`）ただ 1 つが決める。
斬撃クリップは **`Hit Time / bladeStartup` を再生速度として毎回導出**して流すので、
振り抜くコマは必ず判定の瞬間に来る。発生を縮めればモーションも同じだけ速くなり、
2 つの数字がずれる余地がない。`Hit Time` はクリップ固有の事実
（`Katana_Slash_R/L` = f12、`Katana_Slash_Dual` = f14）だけを持つ。

### 今すぐ確かめられること

1. 敵を右クリック（＋）で斬る → 相手が赤くなる。自分も赤くなる
2. そのまま左クリック（−）で別の敵を斬る → 自分が青になり、**さっきの赤へ引かれる**
3. 同じ剣を続けて振る → **弾かれて離れる**
4. 赤と青を1体ずつ作る → 引き合って激突し、撃破コアが残る
5. 3 回続けて振る → 3段目が `Katana_Slash_Dual` になり、硬直と押し出しが増える

**2 と 3 が気持ちよくなければ、モーションを増やしても直らない。**
そのときに触るのは `bladeChargeSeconds` / `playerAttractSpeed` / `bladeStartup` の3つ。

## 作業順（ここから先）

| # | 項目 | 何が確かめられるか |
|---|---|---|
| 1 | 空振りの音 `SE_BLD_Swing_*` | 近接で最初に足りなくなる音。命中音 `SE_BLD_Hit_*` も同時に |
| 2 | 斬撃トレイル（`SOCKET_Trail_Base` / `_Tip`） | 刃がどこを通ったかが 1 コマでも読めるか |
| 3 | 導入の `Katana_Iai` とリザルトの `Katana_Victory` / `Katana_Lose` | 始まりと終わりの締まり |
| 4 | `bladeStartup` / `bladeFinisherStartup` の詰め | 連撃の «繋がる» 感触。モーション速度も一緒に動く |

---

## 実装の地図（極性システム側・すでに入っている）

| 何 | どこ |
|---|---|
| 同極反発 | `PolarityFieldComponent::ApplyRepulsion` → `PolarityBodyComponent::ApplyRepulse` |
| 距離の分離 | `PolarityTuning` の `repulsionRadius`(6m) / `attractionRadius`(12m) |
| プレイヤーの極 | `PlayerPolarityComponent` → `PlayerControllerComponent::RequestPolarityDrive` |
| 中和の罰 | `PolarityTargetComponent::m_paintLock` |
| 撃破コア | `KillCoreComponent` ＋ `CombatManagerComponent` の枠プール |
| Roller の転がり | `EnemyRollerComponent::Phase::Rolled` |
| 実効半径 / 床ハザード | `ArenaBoundsComponent` / `ArenaHazardComponent` |
| Wave 進行 | `WaveDirectorComponent` ＋ `EnemySupplyComponent::SetSupplyActive` |
| ランク評価 | `GameResultState::Rank()` / `ResultPresenterComponent` |
| 反発リング / 撃破コアの絵 | `DecalPolarityRing.hlsl` / `PolarityCore.hlsl` |

**`PlayerPolarityComponent` は捨てない。**入口を「回避＋トリガー」から
「斬撃」へ差し替えるだけで、引かれ方・弾かれ方・体当たりはそのまま使える。

---

## 押しと引きの«持ち場»は分けたまま

反発はヒットストップにもカメラにも触らない（`PolarityFieldComponent::PushAway`）。
毎秒使う手なので、画面が止まると上手いプレイヤーほど見づらくなる。

**斬撃のヒットストップもここへ加わる。** 3 段階になるので配分を守ること。

| 出来事 | ヒットストップ |
|---|---|
| 反発（押し） | なし（0〜2F） |
| 斬撃 | 弱（2〜3F） |
| 衝突（引力の激突） | 強（速度に比例） |

---

## 物差し（設計が効いているかを数字で確かめる）

`CombatManagerComponent` に計測を足せば全部取れる。1周プレイして記録する。

| 指標 | 目標 | 何を判定するか |
|---|---|---|
| 1分あたりの斬撃回数 | 30以上 | サイクルが1〜2秒に収まっているか |
| **左右を交互に振った割合** | **50%以上** | 二刀に理由があるか。片方に偏るなら引力/反発が効いていない |
| **斬撃で自分が動いた回数 / 斬撃回数** | 40%以上 | «斬るたびに自分が動く» が成立しているか |
| 1回の集束での巻き込み体数 | 3体以上が半数 | 撃破コアとアンカーが足りているか |
| 反発で倒した数 / 全撃破数 | 25〜40% | 押しと引きが両方使われているか |
| 被弾回数 | — | 近接にして緊張が上がったか（旧設計より増えているはず） |

**左右の偏りが最重要の指標になる。**片方の剣ばかり振っているなら、
「交互に振ると引かれ続けて飛び移れる」が体験として成立していない。

---

## 残っているアセット作業

| 項目 | 何が要るか |
|---|---|
| 斬撃の音 | `SE_BLD_Slash_*` / `SE_BLD_Hit_*`。空振りと命中で分ける |
| 反発の音 | `SE_POL_Repulse_*.wav`。今は `kImpactLight` を間借り |
| `HUD_Wave` テキスト | `WAVE 2 / 4` を出す UIText。無くても進行は動く |
| 導入の通路3ビート | 配置のみ。`SPAWN_E_*` マーカーのリネームを含む |

見た目は手続きシェーダーで入っている（テクスチャ不要）。

| 何 | シェーダー | 駆動 |
|---|---|---|
| 反発半径の環 / 弾けた放射 | `Material/Decal/Material/DecalPolarityRing.hlsl` | `PolarityRingComponent` |
| 撃破コア | `Material/Effects/PolarityCore.hlsl` | `KillCoreComponent` |
