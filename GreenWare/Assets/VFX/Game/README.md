# GreenWare ゲーム内エフェクト

`Assets/VFX/Textures/` の素材を、企画書 12 章の各場面へ差し込んだ `.vfx` 一式。
`Assets/VFX/Templates/` はエンジン付属の汎用テンプレートで、こちらは本作専用。

素材は無彩色で焼いてあるので（`Textures/README.md`）、赤 / 青はすべて
**`Polarity Color` パラメーター**から乗る。グラフを極ごとに 2 本へ割ってはいけない。

## どこで鳴るか

| `.vfx` | 鳴らす場所 | 企画書 |
|---|---|---|
| `FX_POL_Charge.vfx` | `PolarityTargetComponent::Apply` → `Applied` | 12.1 / 12.3 |
| `FX_POL_Neutralize.vfx` | `PolarityTargetComponent::Apply` → `Neutralized` | 12.4 |
| `FX_ATR_Launch.vfx` | `PolarityBodyComponent::TickWindup` の撃ち出し | 7.3 ② / 17 |
| `FX_IMP_Explosion.vfx` | `PolarityFieldComponent::ResolveImpacts` の 1 件ごと | 12.6 |
| `FX_BEAM_Scorch.vfx` | `BeamScorchComponent::Place` の焼け跡ごと（間引きあり） | 6.2 / 12.1 |

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
| 閃光 / 爆風 / 電弧 / リング / 記号 | 極性色（`Polarity Color`） |
| 火花 / 煙 / 破片 | **無彩色と熱色のまま**。極性色を乗せない |
| 中和 | 黄（`kColorWarning`）。赤でも青でもない |

火花と煙まで赤青にすると、盤面で「誰が帯電しているか」を色で数えられなくなる。
盤面の赤青は極性の表示専用に空けておく。

## 差し替えパラメーター

| グラフ | パラメーター | 用途 |
|---|---|---|
| 共通 | `Polarity Color` | ＋ = 赤 / − = 青。`PolarityColor()` の値をそのまま渡す |
| Charge | `Symbol Material` | `FX_SymPlus` / `FX_SymMinus` の差し替え |
| Charge | `Symbol Offset` / `Halo Size` | 敵の背丈と太さ（`bodybounds::Of`）へ追従 |
| Neutralize | `Collapse Radius` | 体の外から吸い込ませる半径 |
| Launch | `Wake Velocity` | 置き去りの尾。撃ち出し速度に比例させる |
| Impact | `Spark Power` / `Blast Light` | 衝突速度（12.6「衝突速度に比例」） |
| Impact | `Ground Mark` | 柱・壁への叩きつけだけ焦げ → ひび（7.5） |
| Beam Scorch | `Ember Rate` / `Sear Size` | 火の粉の量と、光る範囲（デカールの Size に合わせる） |

## 調整するときの順番

1. **短くする方を先に試す。** 命中感は総量ではなく立ち上がりの速さで決まる。
   量を増やしても鈍いままなら、寿命が長すぎる。
2. **明るさより彩度。** 12.2 の注記どおり、発光を上げると白飛びして赤と青が消える。
   明るくしてよいのは面積の狭い芯（`FX_GlowCore` / `FX_SplashImpact`）だけ。
3. **加算の枚数を数える。** 白く抜けるのは 1 粒が明るいからではなく、重なり枚数が多いから。
   `maxParticles` と `bursts.count` を先に疑う。
