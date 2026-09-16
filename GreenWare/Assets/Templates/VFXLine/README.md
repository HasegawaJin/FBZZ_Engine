# VFX Line テンプレート (雷・ビーム)

`VFXLineComponent` (雷・ビームの «線») の出発点になる `.vfx` を並べてある。
数値は `ApplyVFXLinePreset` (`Projects/Engine/src/Scene/VFXLineGeometry.cpp`) の
組み込みプリセット 5 種をそのまま写したもので、**見た目は保証済みの値**。

## 一覧

| ファイル | 対応プリセット | 使い所 |
|---|---|---|
| `Lightning_Bolt.vfx` | Lightning | 枝分かれのある雷を出しっぱなしにする。装置の放電、電気を帯びた武器 |
| `Lightning_Strike.vfx` | Lightning + 尺と閃光 | 一撃の落雷。0.25 秒で消え、`Point Light` の閃光が同時に走る |
| `Arc_Short.vfx` | ElectricArc | 電極の間を細かく走る放電。細く速い (`strikeRate = 24`)、枝は 1 本 |
| `Laser_Beam.vfx` | Laser | まっすぐな光線。芯が太く (`coreWidth = 0.35`)、末端が細らない |
| `Energy_Beam.vfx` | EnergyBeam | 太くうねるビーム。帯を輝点が流れる (`pulseSpeed = 6`) |
| `Tether_Link.vfx` | Tether | 垂れて揺れる引き寄せの線。`sag = 0.4`、輝点は始点へ向かって流れる |

`Lightning_Strike.vfx` だけはプリセットから **尺のみ** 変えてある
(`duration = 0.25` / `loop = false` / `fadeOut = 0.1`)。色・太さ・枝・打ち直し回数は
Lightning プリセットと同値。閃光は `LightComponent` + `VFXElement` + `VFXLightEnvelope` で、
ルートの `VFXComponent` の時刻に従う。

## 使い方

1. **中身を見る / 調整する** — Asset Browser でダブルクリックすると Prefab 編集モードで開く。
   `Line` の `VFX Line` を Inspector でいじり、別名で保存して自分の `.vfx` にする。
2. **端点を繋ぐ** — `Line` の `VFX Line` の端点欄で決める。
   - `fromEntity` 空 → 自分 (`Line` の GameObject) の位置から出る。`fromOffset` で手や胸へずらす。
   - `toEntity` を挿す → その実体へ伸びる。空なら `toPoint` (**自分から見たローカル座標**) へ伸びる。
   - `disableWhenEndpointMissing` が true なら、指した実体が消えたとき線を畳む。
     置いただけで見せたい場合は `toEntity` を空のままにする (座標指定は «消えた» にならない)。
3. **シーンへ置く** — Asset Browser から `.vfx` をシーンへドラッグしても**置けない**
   (配置経路は `.prefab` だけを受け付ける。下の「注意」参照)。現状の手は次の 2 つ:
   - Hierarchy の Create メニュー > **VFX** > `Lightning` / `Electric Arc` / `Laser` /
     `Energy Beam` / `Tether` — 同じプリセット値の GameObject をその場で作る
     (`Projects/Editor/src/Util/ObjectPresets.cpp`)。
   - スクリプトから `SpawnVFX(scene, "Assets/Templates/VFXLine/Lightning_Strike.vfx", pos, rot)`
     で 1 発鳴らす (`Engine/Scene/Components/VFXComponent.hpp`)。
4. **材質** — 既定は `Assets/Materials/Effects/VFXLine.mat` (加算 HDR の芯 + グロー)。
   色・明るさ・時刻は `VFXLineSystem` が毎フレーム per-instance の `paramOverrides` へ書くので、
   同じ `.mat` を何本共有しても互いを汚さない。差し替えるときは `VFXLine.hlsl` が読む
   `albedo` / `coreColor` / `intensity` / `phase` / `coreWidth` / `breakup` /
   `pulseSpeed` / `pulseDensity` を持つ材質にすること。

## `.fluid` テンプレートとの違い

`Assets/Templates/Fluid/*.fluid` は **煙・液体を焼くためのレシピ** で、Fluid Editor の
「New Fluid from Template」ギャラリーから選び、焼いて初めて絵になる (成果物は
フリップブックの `.mat` と `.vfx`)。

こちらの `Assets/Templates/VFXLine/*.vfx` は **焼かない**。線の形は毎フレーム
CPU で組み直され、帯へ広げるのは VS (`VFXLine.hlsl`) なので、置いた瞬間から動く。
専用のギャラリー UI も無い (Fluid Editor のギャラリーは `Assets/Templates/Fluid` +
`.fluid` 決め打ち)。

## 注意 / 未検証

- **未検証** — エディターを起動して実際の見え方は確認していない (このリポジトリでは
  ターミナルからビルドしないため)。TOML としてパースできること、キー名が
  `VFXLineComponent::Reflect` / `VFXComponent::Reflect` と一致することまでは確認済み。
- `.vfx` をシーンへ置く経路が無い。Asset Browser のダブルクリック・Hierarchy へのドロップ・
  ビューポートへのドロップ・AI の `prefab.instantiate` は、いずれも拡張子 `.prefab` で
  弾いている。`.vfx` のダブルクリックは Prefab 編集モードで開く動作になる。
- ルートの `VFXComponent` が尺を自動算出するとき (`duration = 0`)、`VFXLineComponent` は
  勘定に入らない (`VFXSystem::ObjectEndTime` が見ているのは `VFXElement` /
  `ParticleEmitter` / `TrailComponent` / `DecalComponent` だけ)。線だけの `.vfx` で
  `autoDestroy` を使うなら、ルートの `duration` を明示すること。
  出しっぱなしの 5 本は `autoDestroy = false` にしてある。
- プールから再利用した 2 回目以降、線の時刻が巻き戻らない (`VFXSystem` の
  `RewindPlayback` に `VFXLineComponent` の項が無い)。`Lightning_Strike.vfx` を
  `SpawnVFX` で連射する場合は、ここが効いて 2 発目が出ない可能性がある — **未検証**。
