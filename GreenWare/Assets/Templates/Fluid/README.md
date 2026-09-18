# `.fluid` テンプレート

Fluid Editor の **Open ▼ → New...**（New Fluid from Template ギャラリー）に、組み込みプリセット
（`FluidPreset`）と並んで «User templates» として出る。ここのファイルは編集しても自動保存されない
（`Assets/Templates` の下は «作り始めの原本» という約束）。使うときはギャラリーから複製して、
複製先を焼く。

各ファイルは «それらしい絵» であると同時に、**レシピの仕組みを 1 つずつ見せる見本**になっている。
真似したい仕組みから選ぶのが早い。

## 一覧

| ファイル | 見本にしている仕組み | 種類 / 焼き |
|---|---|---|
| `AshFall` | `weight` が `buoyancy` を上回る «沈む気体»・長い `detail_period` | gas / 3D loop |
| `BladeMist` | `capsule` の湧き口を `motion` で振り抜く | gas / 2D |
| `BlastWall` | 動くコライダー（`[collider.motion]`）に煙をえぐらせる | gas / 3D |
| `CoolantJet` | `capsule` のコライダー（パイプ）に液体を当てる | liquid / 3D |
| `DoubleBlast` | `start_time` で発生源と力を時間差に並べる | gas / 3D |
| `FloorDust` | `plane` コライダー + `gas.floor` で床を這わせる | gas / 2D |
| `FrostBreath` | 上らない気体（低 `buoyancy` + `wind`）と `cone` の湧き口 | gas / 2D |
| `GrateSteam` | コライダーを複数置いて流れを絞る（`box` ×2 + `cylinder`） | gas / 2D loop |
| `GustSweep` | 力のエンベロープ（`[[force.amount.key]]`） | gas / 2D |
| `HeatHaze` | `distortion` シェーディング（色でなく速度を焼く） | gas / 2D loop |
| `JetFlame` | 燃料 → 燃焼の炎と 3D の黒体放射 | gas / 3D loop |
| `LavaDrip` | 液体の `viscosity` / `cohesion` を上げた «とろみ» | liquid / 3D |
| `MagicAura` | `glow` シェーディングと `emission_ramp`・`attract` | gas / 3D loop |
| `QuickPuff` | 焼き値段のつまみ（16 コマ・128px・超解像なし） | gas / 2D |
| `ShockRing` | `ring` の湧き口 + `repulse`（distortion） | gas / 2D |
| `SigilBurn` | `texture` の湧き口（画像の形に湧かせる） | gas / 2D |
| `SmokeVortex` | 発生源の `motion` キーと `vortex` / `drag` | gas / 3D loop |
| `SplashOnRock` | `box` コライダーに水をぶつける | liquid / 3D |
| `TwoToneSmoke` | 発生源ごとの `color_key` と `albedo_ramp` | gas / 3D |
| `VentPulse` | 量のエンベロープ（`[[source.amount.key]]`）と `cylinder` の湧き口 | gas / 2D loop |
| `WaterCurtain` | 液体の見た目 4 点（softness / extinction / gloss / fresnel） | liquid / 3D |
| `WindField` | `output.vector_field` で `速度場 PNG` も焼く | gas / 3D loop |

## 選ぶときの目安

- **2D か 3D か** — 3D（Volume Flipbook Baker）は焼き時間が桁で違う。足元の土埃や一瞬の霧は 2D で足りる。
  厚みと自己影が要るもの（爆発・煙柱・液体）だけ 3D にする。
- **ループするか** — `output.loop = true` は末尾を先頭へクロスフェードする。常設の煙・蒸気・降灰はループ、
  当たりの演出は 1 回きり。
- **時刻の基準** — `start_time` / `duration` と `motion` / `amount` のキーの `time` は、**ウォームアップも
  含めた通算の秒数**。焼くのは `warmup` から `warmup + duration` までの窓なので、ループ物のエンベロープは
  その窓に合わせて置く（`VentPulse` を参照）。

## 注意 / 未検証

- **未検証** — 追加したテンプレート（`AshFall` / `BladeMist` / `CoolantJet` / `DoubleBlast` / `FrostBreath` /
  `GrateSteam` / `GustSweep` / `LavaDrip` / `QuickPuff` / `VentPulse`）は、TOML としてパースできることと
  キー名・enum が `FluidRecipe` の読み書きと一致することまでしか確認していない。焼いた絵は
  Fluid Editor で目視して詰めること。
- ギャラリーのサムネイルは先頭 48 件まで（`kFluidGalleryMaxThumbs`）。組み込みプリセット 19 件 +
  ここの 22 件 = 41 件。あと 7 件を超えると、超えたぶんは色板だけになる。
