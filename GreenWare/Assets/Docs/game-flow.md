# ゲームの流れ / ステージ構成

> 親: [企画書](企画書.md) ／ 関連: [弾いて崩す](break-parry.md) [アリーナ](arena.md) [ボス1](boss.md) [ボス2](boss-serpent.md)
> 実装: `Scripts/Game/GameFlowComponent.hpp` `CombatManagerComponent.hpp` `ResultPresenterComponent.hpp`
> 台帳: `Scripts/UI/StageCatalog.hpp` ・ 進行: `Scripts/UI/StageProgressState.hpp`

> **2026-09-08 に全面改訂。** Wave 制・雑魚の追加供給・通路の導入 3 ビートは
> **すべて廃止**した（雑魚をモデルごと撤去したため）。以下が現行の流れ。

---

## ロード画面の経路（2026-09-15）

`Load.scene` を経由するのは `StageSelect → Stage_01〜03` と `Stage_01〜03 → Result` のみ。
Title・Options・一覧へ戻る・リザルトからのリトライと次ステージは、ロード画面を挟まずワイプで直接切り替える。

ロード画面では `maou_bgm_cyber45_Load.mp3` を 0.4 秒でフェードインしてループ再生する。
遷移先の GameFlow / ResultPresenter が BGM を切り替えるため、ロード画面の破棄時には停止しない。

## 1 本道の遷移

```text
Title ──> StageSelect ──> Stage_NN ──> Result ──┬─> 次のステージ
  ▲            ▲                                 ├─> リトライ（同じステージ）
  │            └─────────────────────────────────┘
  └── Options / Exit                             └─> 一覧へ戻る
```

| シーン | 役割 |
|---|---|
| `Title` | START / OPTIONS / EXIT |
| `Options` | 設定（[settings.md](settings.md)） |
| `StageSelect` | ステージの一覧。解放済みの行だけ押せる。右パネルはクリア後の記録 |
| `Stage_01` / `Stage_02` | 実戦。ボス 1 体との 1 対 1 |
| `Result` | 戦績とランク。次 / リトライ / 一覧の 3 択 |

遷移はすべて `Utils/SceneTransition.hpp` の `transition::Begin` を通す
（ワイプ。[presentation.md](presentation.md#止めスロー扉2026-09-07)）。

---

## ステージ台帳

**シーン名・表示名・ボスの構成の正本は `Scripts/UI/StageCatalog.hpp` の 1 表。**

| # | シーン | 名前 | ボス | 決着条件 |
|---|---|---|---|---|
| 1 | `Stage_01` | チュートリアル | スパイダー（4 足歩行） | **相棒の案内で斬撃・回避・弾きを覚え、脚 4 本を破壊** |
| 2 | `Stage_02` | SERPENT PIT | サーペント（蛇型・2 体） | **28 → 6 節まで折る** |
| 3 | `Stage_03` | ISLE | サンフラワー | **翼へのとどめ・落下した翼の破壊・投擲の弾き返しで撃破を狙う** |
| 4〜7 | — | ???? | — | **枠だけ。まだ作っていない** |

> **WHY 1 つの表に畳むか:** ステージの情報は 3 か所が別々に要る — 選択画面の行（名前）、
> 右パネル（ボスの構成）、`PLAY / RETRY` で読み込むシーン名。別々に持つと、
> ステージを 1 本足すたびに 3 か所を直すことになり、片方だけ直した状態が
> 「名前は出ているのに押すと 1 面が始まる」という形で出る。
>
> **`scene` が空文字の枠は «まだ作っていない» の意味。**選択画面は解放しても押させない
> （`StageExists`）。これが無いと、クリアした次の行が「押すと読み込みに失敗して固まる」になる。

右パネルの見出しは `ボス / 部位 / 戦い方 / 変化 / 攻略`。
サーペントは 3 段階、チュートリアルは習得に合わせた解禁なので、2 フェーズ固定の見出しにしない。
部位欄は破壊対象を示す。BOSS 03 には投擲を弾き返して本体へダメージを与える経路もある。
攻略情報は**初回クリア後**に表示する。解放済みで未クリアなら「未クリア」、実体のない枠は「準備中」。
ステージ名と和文の説明には全文字版の解書体を指定し、サブセットフォントの収録文字に依存させない。

---

## ステージ内の進行

**Wave は無い。**シーンを読み込んだ瞬間にボスが居て、倒すか倒されるかで終わる。

| 出来事 | 実装 |
|---|---|
| 開始 | `GameFlowComponent::OnStart`。計測開始（`debugElapsed`） |
| 目的の 1 行 | `HUD_Objective` に `PARRY > BREAK > EXECUTE`。**転倒中だけ** `IT'S DOWN!  EXECUTE` |
| 勝ち | `IBoss` が全部の部位を失った → `AREA CLEAR` → `bossEndDelay`（5 秒）後に Result |
| 負け | プレイヤーの HP が 0 → 移動を停止 → `SYSTEM DOWN` → `endDelay`（0.8 秒）後に Result |

> 2026-09-13: 上半身だけが揺れる不自然さを避けるため、Playerのラグドール演出を撤去。
> 開始時に既存のRagdollを無効化し、被弾・移動・撃破から物理反応を発火しない。

> **WHY 崩れている間ずっと拘束を言い直すか:** `RequestSuspend` は 1 フレームぶんの
> 要求。1 度きりにすると次のフレームからコントローラーが重力と入力を当て直し、
> **倒れている体の下で足が走る。**

⚠ **ボスは `IBoss::Bind` で名乗る。**新しいボスを足したら必ず書くこと。
横断インターフェースは型で引けない（`scene.GetScript<IBoss>()` は必ず空を返す）ので、
忘れると **「倒してもステージが終わらない」**という形でしか症状が出ない。

> **WHY 勝ちの猶予を 5 秒も取るか:** 撃破演出（部位の飛散・カメラの寄り・粒になって
> 立ち昇る）が終わる前にワイプが降りると、決着がリザルト画面の文字だけになる。
> `bossEndDelay` は演出の尺と一緒に伸ばすこと（片方だけ触ると一瞬で消える）。

---

## ポーズ（Stage_01〜03）

`Pause`（Esc / パッドの MENU）で開く。実装は `Scripts/UI/PauseMenuComponent.hpp`、
シーン側は `Pause_Canvas` 一式（3 つのステージに同じ構成が入っている）。

| 行 | 何が起きるか |
|---|---|
| `RESUME` | 閉じる。Esc / MENU をもう一度押すのと同じ |
| `OPTIONS` | **同じシーンの中で**設定を開く（[settings.md](settings.md)）。Esc で 1 段戻る |
| `EXIT` | 設定を保存し、ワイプで `Title` へ |

```text
Pause_Canvas
  Pause_Root ──┬─ Pause_Mark / Pause_Rule / Pause_Title / Pause_Hint  見出し（共通）
               ├─ Pause_Menu     RESUME / OPTIONS / EXIT
               ├─ Pause_Options  Options 画面と同じ名前の行 + OptionsScreenComponent
               └─ Pause_Cursor   GameCursorComponent
```

- **時間は `TimeManagerComponent::SetPaused`** で止める。ヒットストップ（Override）と
  スロー（下の層）はそのまま生きていて、閉じれば元の層へ戻る。
- **プレイヤーには毎フレーム `RequestSuspend(true)`** を言い直す。timeScale が 0 でも
  `OnUpdate` は回っていて、斬撃の入力は押した瞬間に段が進むため、言わないと
  「閉じた瞬間に溜めていた入力が出る」。
- **視点は `TpsCameraComponent` が `IsPaused()` を見て止める。** マウスの移動量は
  時間に掛かっていないので、止めるだけではカーソルを動かした分だけ背後で振り向く。
- **カーソルは `Pause_Cursor` の有効・無効で出し入れする。** `GameCursorComponent` は
  `OnEnable` で `Confined`（優先度 UI）を積み、`OnDisable` で下ろすので、閉じれば
  視点操作の `Locked`（優先度 Camera）へ自動で戻る。
- **扉が動いている間は開かない**（`transition::Active()`）。リザルトへ落ちる途中で
  開くと、止めたまま次のシーンへ渡ることになる。

> **WHY OPTIONS でシーンを移らないか:** 音量を直すだけで盤面を降ろすと、戻ったときに
> 敵の位置も体力も作り直しになる。Options 画面は行を `scene.Find` で引くだけなので、
> 同じ名前の行をポーズの枠へ置けば、あちらのスクリプトをそのまま内側で動かせる。
> ポーズの中では `standalone = false` にして、BGM の掛け替えと扉の描画、Esc の
> 受け取りだけを黙らせてある。
>
> ⚠ **エディタの Play では Esc がカーソル解放にも使われる。** ポーズの確認は
> パッドの MENU か、配布ビルドで行うのが確実。

---

## 評価とランク

3 項目を各 0〜3 点で採点し、合計 9 点でランクを決める（採点 version 2）。

| ステージ | クリアタイム：3 / 2 / 1 点 | 攻防：各条件で 1 点 |
|---|---|---|
| 01 | 4:00 / 5:30 / 7:00 | 最大6連撃・2弾き・1拍締め |
| 02（2体） | 6:00 / 8:00 / 10:00 | 最大8連撃・6弾き・2拍締め |
| 03 | 4:00 / 5:30 / 7:00 | 最大10連撃・4弾き・2拍締め |

被ダメージは 0 / 1 / 3 以下で 3 / 2 / 1 点。S=8〜9、A=6〜7、B=4〜5、C=0〜3。
攻防は CHAIN の最高値・成功した弾き・拍を揃えて当てた締めを評価する。空振り入力では加点しない。
同じ技術だけを繰り返しても攻防の満点にはならない。

採点と表示は `GameResultState::Profile` / `Axis` を共有する。
時計は交戦中の非スケール秒を使い、ポーズ・強制時間上書き中を除外する。
ステージ番号は各シーンの `GameFlowComponent.stageNumber` が正本。
旧記録は解放・クリア状態を保持し、次のクリアから新基準の記録へ移行する。
詳細: [残り火と採点設計](../../../Docs/design/boss03-ground-fire-and-scoring.md)。
数値はプレイ調整前の初期基準。

**HP は 5**（`PlayerTuning.fzdata` の `maxHealth`）。敵の攻撃は 1〜3 なので、
1 発が全体の 2〜6 割にあたる。

> 2026-09-08 まで `maxHealth` が **50**（調整用の値）のまま残っていて、
> 被ダメ軸はほぼ常に満点、土壇場（残り 1）は実質発動しなかった。5 へ戻してある。

### 連撃

衝突と同じ `CHAIN` 表示に乗る（`ChainDisplayComponent`）。猶予 2.2 秒の内に次を当てれば続く。
**ボスが倒れている間だけ**、連撃が伸びるほど斬撃が重くなる（1 撃ごと +8%、上限 1.6 倍）。

ジャスト回避は採点に乗せないが回数を数える（`GameResultState::perfectDodges`）。

> 旧軸（1 回の集束での巻き込み数 / 反発による押し込み撃破）は雑魚を畳んだ時点で
> 一度も起きない出来事になり、どの周も 3 点満点で C 止まりだった。

### ランクを出す条件

**勝った挑戦を採点する**（`RankAvailable()`）。
リトライ前の失敗は次の挑戦へ持ち越さない。

---

## リトライと解放

| 出来事 | 何が起きるか |
|---|---|
| リザルトで RETRY | `StageProgressState::cursor` のステージを読み直す。`noRetry = false` |
| リザルトで NEXT | **cursor を次の行へ持ち替えてから**読み込む（忘れると 1 つ前の行へ記録が入る） |
| クリア | `StageProgressState::Commit(cursor)` — `cleared` / `bestScore` / `bestSeconds` / `bestChain` / `leastDamage` を更新し、**次の行を解放**する（実体があるときだけ） |

### 保存（2026-09-08）

解放と自己ベストは **`save` ストア**へ落ちる。

| 何 | どこ |
|---|---|
| 保存先 | `%LOCALAPPDATA%/GreenWare/progress.toml`（設定と同じ親フォルダ・`Per User` を切ると exe の隣） |
| 中身 | `[stages]` に `cleared` / `unlocked` / `bestScore` / `bestSeconds` / `bestChain` / `leastDamage` / `bestTechnique` / `scoreVersion` の 8 本の配列 |
| 読む | `StageProgressState::EnsureInit(save)` — **起動して最初に触った画面で 1 度だけ** |
| 書く | `StageProgressState::Commit(index, save)` — リザルトが勝ちで閉じるとき |

> **WHY 読むのを 1 度きりにするか:** シーンを移るたびに読み直すと、まだ書いていない
> 「今回の記録」がディスクの古い値で上書きされる。読むのは起動直後の 1 回、
> 書くのはクリアした瞬間だけ、と向きを固定する。
>
> **WHY 実行ファイルの隣に置かないか:** エディタと配布ビルドは別 exe なので、
> 相対パスだと「エディタで解放した面が製品版では閉じている」になる。
> 設定が同じ理由で per-user へ寄せてあるので、進行も同じ親フォルダへ置く。
>
> **WHY `config` ではなく `save` か:** `config` は遊び方の好みで、セーブ枠を
> 切り替えても付いて回るもの。解放と自己ベストは周回の結果なので枠と一緒に動く。

---

## 想定プレイ時間

| 単位 | 時間 |
|---|---|
| 1 ステージ | 2〜4 分（ランク S の基準が 3 分以内） |
| 2 ステージ通し | 5〜10 分 |

**背景を 1 枚も足さずに «難しくなった» を成立させる**のが狙いだった。
Wave の密度でやっていたことを、今は**ボスの手数と部位の残りで**やっている
（脚が 2 本落ちるとコアは砲台へ移行し、サーペントは節が減るほど速くなる）。
