# GreenWare ゲーム内エフェクト

`Assets/VFX/Textures/` の素材を、企画書 12 章の各場面へ差し込んだ `.vfx` 一式。
`Assets/VFX/Templates/` はエンジン付属の汎用テンプレートで、こちらは本作専用。

素材は無彩色で焼いてあるので（`Textures/README.md`）、赤 / 青はすべて
**`Blade Color` パラメーター**から乗る。グラフを色ごとに 2 本へ割ってはいけない。

## どこで鳴るか

| `.vfx` | 鳴らす場所 | 企画書 |
|---|---|---|
| `FX_IMP_Explosion.vfx` | `VfxManagerComponent::Blast`（衝撃・爆発の 1 件ごと） | 12.6 |
| `FX_BEAM_Scorch.vfx` | `BossBeamComponent` の焼け跡ごと（間引きあり） | 12.1 |
| `FX_PLR_RunDust.vfx` | `PlayerControllerComponent::TickFootsteps` の足音と同じ刻み | — |
| `FX_BOSS_ShockDust.vfx` | ボスの重量が床へ掛かった点 (`PlayGroundDust`) | — |
| `FX_PLR_Parry.vfx` | `PlayerParryComponent::OnParried` の弾き | break-parry |
| `FX_PLR_ParryJust.vfx` | 同上、窓の頭で受けた Just 弾き (`PlayParry(..., just)`) | break-parry «Just 窓» |
| `FX_BLD_SlashHit.vfx` | `BladeComponent::ResolveHit` の刀の当たり (`PlaySlashHit`) | presentation «斬撃» |
| `FX_BOSS_Topple.vfx` | `BossAiComponent::Topple` / `SerpentAiComponent::Topple` (`PlayTopple`) | break-parry «転倒» |
| `FX_BOSS_Execute.vfx` | `BossRigComponent::ExecutePart` / `SerpentAiComponent::Execute` (`PlayExecute`) | break-parry «とどめ» |

呼び出しは全部 `VfxManagerComponent`（シーンの `Manager/VfxManager`）を通る。
枠（GameObject）は `.vfx` の種類ごとにリングで持ち回すので、鳴らす側は
GameObject を作らないこと。

**延長（`Extended`）では何も鳴らさない。** 付与と同じ絵を返すと、なぞりの途中で
「また 1 体増えた」と読み違えて、起爆したときのコンボ数が合わなくなる。

## 画面演出をここに入れないこと

ヒットストップ・カメラ揺れ・振動・画面フラッシュは `ImpactFeedbackManagerComponent` が
1 つの出来事に対する配分を持っている。同じものを `.vfx` の
`ScreenEffect` / `CameraShake` / `TimeScale` ノードでも足すと、集束で 5 体潰れたときに
2 系統が別々の強さで重なって画面が壊れる。

そのため本作の `.vfx` には**この 3 種のノードを 1 つも置いていない。**
テンプレート（`Templates/Impact.vfx` など）から塊を取り込むときは、
必ずこの 3 種を落としてから使うこと。

## 色の制約（12.2）

| 層 | 色 |
|---|---|
| 閃光 / 爆風 / 電弧 / リング | 刀の色（`Blade Color`） |
| 火花 / 煙 / 破片 | **無彩色と熱色のまま**。刀の色を乗せない |

火花と煙まで赤青にすると、どちらの刀が出たかを色で読めなくなる。
赤青は刀の左右の表示専用に空けておく。

## 差し替えパラメーター

| グラフ | パラメーター | 用途 |
|---|---|---|
| 共通 | `Blade Color` | 右 = 赤 / 左 = 青。`BladeColor()` の値をそのまま渡す |
| Launch | `Wake Velocity` | 置き去りの尾。撃ち出し速度に比例させる |
| Impact | `Spark Power` / `Blast Light` | 衝突速度（12.6「衝突速度に比例」） |
| Impact | `Ground Mark` | 柱・壁への叩きつけだけ焦げ → ひび（7.5） |
| Beam Scorch | `Ember Rate` / `Sear Size` | 火の粉の量と、光る範囲（デカールの Size に合わせる） |
| Run Dust | `Dust Color` | 床の色。ここだけは極性色ではなくステージの色を入れる |
| Run Dust | `Puff Size` / `Kick Speed` | 走行速度に比例。`VfxManager` の Run Dust が min/max を持つ |
| Parry / ParryJust | `Spark Power` / `Flash Light` / `Ring Size` | 弾いた手の重さ。Just は 1.35 倍 (`PlayParry` の boost) |
| Slash Hit | `Tint` / `Arc Size` / `Spark Power` | `Tint` は振った刀の極性色。`Arc Size` は 3 段目だけ太く |
| Topple | `Scale` / `Crack Size` / `Dust Color` | 1.0 でコア (6m 級)、蛇は 0.7。土煙は床の色 (Run Dust と同じ) |
| Execute | `Cut Size` / `Spark Power` / `Cut Light` | 切断面の大きさ。脚 1.0 / 節 0.7 |

## 弾き・当たり・転倒・とどめの読み分け (2026-09-07)

| 出来事 | 輪 | 閃光 | 残るもの | 色 |
|---|---|---|---|---|
| 刀の当たり (`SlashHit`) | **無し** | 小 | 無し (0.35 秒) | 弧と光は極性色、火花は熱色 |
| 弾き (`Parry`) | 硬い輪 1 本 | 中 | 無し (0.4 秒) | 白〜氷色 |
| Just 弾き (`ParryJust`) | 硬い輪 2 重 + 放射の光条 | 大 | 無し (0.45 秒) | 白〜氷色 |
| 転倒 (`Topple`) | 床を走る輪 (14m) | **無し** (弱い光のみ) | ひび 6 秒・低い土煙 | 無彩色 |
| とどめ (`Execute`) | 無し | 大 | 赤熱した縫い目 0.9 秒・火の粉 | 白〜熱色 |

輪は «弾いた» の印、閃光は «壊れた / 斬った» の印、残るものは «盤面が変わった» の印。
1 つの出来事に 3 つ全部を乗せないこと。

## 調整するときの順番

1. **短くする方を先に試す。** 命中感は総量ではなく立ち上がりの速さで決まる。
   量を増やしても鈍いままなら、寿命が長すぎる。
2. **明るさより彩度。** 12.2 の注記どおり、発光を上げると白飛びして赤と青が消える。
   明るくしてよいのは面積の狭い芯（`FX_GlowCore` / `FX_SplashImpact`）だけ。
3. **加算の枚数を数える。** 白く抜けるのは 1 粒が明るいからではなく、重なり枚数が多いから。
   `maxParticles` と `bursts.count` を先に疑う。
