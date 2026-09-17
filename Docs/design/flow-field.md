# 流れの場 — 力ではなく媒質の速度を正本にする

`ForceField` は **力**を定義している。`ForceFieldSettings::strength` は「加速度の大きさ [m/s^2]」
(`Engine/Scene/Components/ForceField.hpp` 54 行) — だが、この値を読む側は 3 通りの別々の意味で使っている。

**場が定義すべきものは力ではなく「その点で空気 / 水がどう動いているか [m/s]」。** 力は消費者が自分の
結合係数で導く。単位を 1 つに決め、収集を 1 か所に集める。

---

## 1. 同じ `strength` が 3 つの単位を持っている

| 消費者 | 今の扱い | 実際の単位 |
|--------|---------|-----------|
| 粒子 (Wind / Attract / Vortex / Turbulence) | `velocity += dir * (strength * influence * dt)` (`ParticleForces.cpp` 157〜182 行) | 加速度 [m/s^2] |
| 粒子 (Drag) | `velocity *= 1 - strength*influence*dt` (同 184〜186 行) | 減衰係数 [1/s] |
| 粒子 (VectorField) | 場の値 [m/s] に `strength*dt` を掛けて足す (同 214 行) | [1/s] |
| 雲 | `windSpeed = cloud->windSpeed * ambient.strength` (`VolumetricCloudPass.cpp` 80 行) | **無次元の倍率** |
| 水面 | `windGain = windResponse * clamp(strength/10, 0, 2)` (`WaterSystem.cpp` 106〜110 行) | 無次元 (10 で正規化) |
| 剛体 | 何も読んでいない。物理には `physics::VolumeType`(`Gravity`/`Vortex`/`Buoyancy`…) という**別語彙の力**がある | — |
| 草 | Foliage はエンジン側に無い。`Assets/Shaders/Foliage/` は存在せず、配布ビルドの出力 (`GreenWare/Binaries/**/EngineAssets/Shaders/Foliage/`) にだけ残骸がある | — |

粒子の評価器 1 本の中ですら 3 つの単位が同居している。**「風を 2 倍にする」が消費者ごとに違う結果になる。**

原因は場が力を定義していることにある。そして — **加速度が欲しい消費者は 1 人もいない。**
全員が欲しいのは流速で、そこから自分の式で力へ落としている。正しい形は既にエンジンの中にある。

```cpp
// PhysicsSystem.cpp 159〜160 行 — WaterBuoyancyVolume::Apply
const math::Vector3 flow = { m_water.current.x, 0.0f, m_water.current.y };
body.ApplyForceNoWake((flow - body.GetVelocity()) * (m_water.waterDrag * mass * submersion));
```

水流は `[m/s]` で持ち、抵抗係数は**体の側**が持つ。風もこの式で書けるべきで、書けないのは
場が m/s を返さないからにすぎない。

---

## 2. 決定

**フィールドは媒質の速度 [m/s] を返す。力は消費者が自分の結合係数で導く。**

| 消費者 | 導出 | 結合係数の持ち主 |
|--------|------|-----------------|
| 粒子 | `v += (v_flow − v) · coupling · dt` (質量を持たないので係数は [1/s]) | エミッター |
| 剛体 | `F = k · m · (v_flow − v)` (`physics::FlowVolume`) | 体 (今の `waterDrag` と同じ位置) |
| 水面 | 表面の流速 = `.mat` の current + その点の `SampleFlow`。渦は深さの項として水面の形にも出る | `.mat` |
| 雲 | 雲の高度の環境流で移流。倍率ではなく m/s | 雲コンポーネント |
| 草 | 曲げ ∝ 流速 | マテリアル |
| 浮力 | この表に無い。体の形と密度の話 (`buoyancy.md`) | — |

粒子が「緩和」になると、`Drag` を別の型として持つ理由も消える (§3)。

---

## 3. 原始要素 — 型が enum に増え続けるのを止める

今の `ForceFieldType` は 7 種で、足すたびに `kForceFieldTypeCount` と codec と UI の clamp を
揃えて直す必要がある (`ForceField.hpp` 17〜29 行)。背骨が無いと増え続ける。
**ポテンシャル流の原始要素 + 乱流 + 焼いた場**で閉じる。

| 型 | 流れ | 今の型 |
|----|------|-------|
| Uniform | 一様流 | Wind |
| Source / Sink | 湧き出し・吸い込み | Repulse / Attract |
| Vortex | 軸まわりの周回 | Vortex |
| Curl | 発散ゼロの乱流 | Turbulence |
| Baked | 焼いた速度場 (`速度場 PNG`) | VectorField |

- **Drag は型から消える。** 抵抗は消費者の結合係数であって流れの種類ではない。粒子の空気抵抗は
  「流速 0 の Uniform への coupling」で書ける。今の `Drag` が `strength` を [1/s] として扱っているのは、
  型の皮をかぶった結合係数だった。
- **重力も型ではない。** 媒質の運動ではなく加速度で、正本は `PhysicsSettings::gravity`
  (`Engine/ProjectSettings.hpp` 24 行、既定 `(0, -9.81, 0)`)。
  `ParticleEmitter::MakeDefaultLocalForces` が `direction = (0,-1,0)` / `strength = 5` の **Wind を
  重力として置いている**現状 (`ParticleEmitter.hpp` 150〜163 行) は、エミッターのローカル加速度として
  場の外へ出す。粒子の重力が 5 で物理が 9.81 なのも、場に相乗りしているせいで気付けなかった。

---

## 4. 構成

```
Engine/Scene/Fields/
  FlowField          コンポーネント。原始要素のリスト (今の ForceField::forces と同じ形)
  FlowFieldSystem    Phase::PrePhysics / RunMode::Always / After<TransformPrePhysics>
                     Reads<Transform, FlowField>
                     全 FlowField を集めて Scene のフレームキャッシュへ書く。フレームに 1 回
  SampleFlow(p)      キャッシュから 1 点の流速を返す純関数。
                     HLSL に同型の関数を置く (ParticleGpuSim.cs.hlsl の ApplyForceFields と
                     式を一致させる契約は今と同じ)
Engine/Scene/Environment/
  SceneEnvironment   環境流 (ambient)。GameObject 探索をやめてシーン設定にする
Physics/
  FlowVolume         流れによる抵抗。流速は callback で受ける (Fluid も FlowField も知らない)
```

消費者 — 粒子 (CPU / GPU) / 剛体 / 水面 / 雲 / 将来の草 — が同じキャッシュを読む。

> `TransformPrePhysics` は `RunMode::SimOnly` (`TransformSystem.hpp` 36〜41 行)。編集中は
> `TransformEditorPreview` が `PreScript` で回すので、`FlowFieldSystem` を `Always` で置いても
> ワールド姿勢は確定している。順序ヒントは両方が走るときだけ効く。

**置き場の前例**: `WaterComponent` の `waves` / `current` / `cellSize` は「保存しない・WaterSystem が
毎フレーム書く・描画も浮力も水中判定もスクリプトも読む」という形で既にある
(`WaterComponent.hpp` 72〜85 行)。同じパターンをシーン全体のスコープへ広げるだけで、新しい概念は要らない。

---

## 5. 収集を 1 か所にする理由 = 今そこにあるバグ

`GatherForceFields` は 3 か所で走る。

| 呼び出し | マスク |
|---------|-------|
| `UpdateParticleCpuSimulation` (`ParticlePass.cpp` 2092 行、`ParticleSimulationSystem` の `LateUpdate` から) | `~0u` |
| `ScrubParticleEmitterForEditor` (同 2132 行) | `~0u` |
| 描画パス (同 2264 行、`GatherForceFields(ctx)` → `ctx.cullingMask`) | **カメラのカリングマスク** |

`cullingMask` は「このカメラが**描く**レイヤー」(`ShouldRenderGameObject` = `activeInHierarchy() &&
Layer::Contains(mask, go.layer)`)。ForceField は描かれない。ここで絞ると 3 つ起きる。

1. **同じエミッターが CPU と GPU で違う力を受ける。** `simulationMode` を切り替えただけで軌道が変わる
2. **SceneView と GameView が力バッファを奪い合う。** `AcquireGpuForceBuffer` は
   `emitter.runtime.gpuForceBuffer` へ書く (`ParticlePass.cpp` 704〜717 行) — エミッター単位なので、
   2 つのビューが別々のマスクで別々の力を書き合う
3. **Scene View でレイヤーを非表示にすると粒子の動きが変わる。** 見え方の操作が挙動を変える

さらに `WaterSystem.cpp` と `VolumetricCloudPass.cpp` が `FindAmbientWind` のためだけに
`RenderPasses/Geometry/ParticleForces.hpp` (描画パスの private ヘッダー) を include している。
**System が描画パスの内部ヘッダーを読んでいる** = 依存の逆立ち。

→ **`cullingMask` 引数は落とす。** 場はカメラに属さない。

---

## 6. 環境流を明示にする

`FindAmbientWind` (`ParticleForces.cpp` 106〜134 行) は「最初に見つかった radius ≤ 0 の Wind」で
`break` する。**GameObject の並び順で勝者が決まる。** 2 つ置くと沈黙で片方が負ける。
`if (result.active) break;` (131 行) は GameObject 単位なので、風を見つけた GameObject より後ろに
置かれた Turbulence は拾われない — 同じ GameObject の中に並んでいるかどうかで結果が変わる。

→ `SceneEnvironment` のシーン設定にする。**重力が `ProjectSettings` にあるのと同じ位置づけ。**
局所の `FlowField` は必ず半径を持つ GameObject とし、「半径 0 なら環境風」という暗黙の規約をやめる。

- `ScriptWindProxy` (`Engine/Scene/ScriptProxy/ScriptWindProxy.hpp`) は残す。スクリプトから見た
  `SetDirection` / `SetStrength` / `SetTurbulence` の語彙はそのままで、触る先を `SceneEnvironment` にする
- Create メニューの `env.windZone` "Wind Zone" (`Projects/Editor/src/Util/ObjectPresets.cpp` 882 行) は
  **"Ambient Wind" へ改名**。`WindZoneComponent` という型はもう無く、`MakeWindZone` は
  `ForceField` + `MakeAmbientWindForces()` を置いているだけ (同 280〜287 行) なので、
  プリセットの向き先をシーン設定へ差し替える

---

## 7. 認めた限界

書いておかないと、後で「バグ」として報告される。解くのは `fluid-library.md` の「この先ここへ足すもの」。

| 限界 | 当面の逃げ道 |
|------|-------------|
| 遮蔽を知らない (壁の裏でも吹く) | 静的ジオメトリから遮蔽を焼く / 屋内に減衰フィールドを手で置く |
| 保存則が無い (発散ゼロなのは Curl だけ) | 見た目には出ない |
| 双方向結合が無い (動く物が流れを乱さない) | 骨に `FlowField` を付けて偽装する |
| 骨付き `FlowField` は 1 フレーム古い (`PrePhysics` で集めるが、スキン姿勢は `IKSystem` が後で確定する) | 見えない |
| `FlowVolume` は水中の体にも風を届ける (Physics は水面を知らない)。水中の体は `FluidVolume` の抵抗も受けるので二重になる | 当面は `flowCoupling` を opt-in にして、浮く物にだけ付ける |
| 浮いた体は `FluidVolume` の水流と `FlowVolume` の風の両方を受ける。`flowCoupling` が既定 0 なので、立てた体だけが二重になる | 水に浮かべる体では `flowCoupling` を 0 のままにする |

---

## 8. 何が変わるか

- **`ForceField` → `FlowField` に改名。** 単位が変わるので名前も変える。同じ名前のまま
  m/s^2 → m/s にすると、既存のスクリプトとシーンが黙って別の意味になる
- `strength` の意味が加速度 → 流速 [m/s]。`ApplyForceFields` (C++ / HLSL 両方) が
  「足す」から「緩和する」に変わる。**式の二重化の契約は今のまま** (`ParticleForces.hpp` 96 行の
  「式は `ParticleGpuSim.cs.hlsl` と一致させること」)
- `FindAmbientWind` 削除、`SceneEnvironment` 新設。`WaterSystem` / `VolumetricCloudPass` の
  `ParticleForces.hpp` への include が消える
- `physics::VolumeType::Buoyancy` と `WaterComponent::buoyancy` の廃止は **`buoyancy.md` の話**
- 既存シーンは `SceneSerializer` が旧キー (`ForceField` / `strength` [m/s^2]) を新しい意味へ写す
  (概算流速)。GreenWare の 6 シーン (`Load` / `Title` / `Stage_01`〜`03` / `WaterTest`) が対象。
  **GreenWare は完成品だが、この設計では既存シーンの絵が変わることを認める** — 単位の取り違えを
  そのまま写す変換は存在しない

---

## 9. 順序

各段でビルドが通り、絵の変化がその段の主張どおりであること。

1. **この文書と `buoyancy.md` で境界を固定する**
2. **`FlowFieldSystem` + Scene のフレームキャッシュ + `SampleFlow`。** 収集の一本化だけ。
   `cullingMask` を落とす。**この段で絵は変わらない** (変わったらそれが §5 のバグ)
3. **粒子の評価を緩和形へ。** C++ と HLSL を同時に直す。片方だけだと `simulationMode` で軌道が割れる
4. **`SceneEnvironment` に環境流を移し、`FindAmbientWind` を撤去**
5. **雲と水面を m/s へ。** 倍率と `/10` の正規化が消える
6. **`physics::FlowVolume`。** 剛体が初めて流れを受ける

---

## 決めないこと

- 格子 (実行時流体) の中身 → `fluid-library.md`
- 浮力・沈み方・`VolumeType::Buoyancy` の行き先 → `buoyancy.md`
- 草 (Foliage) の復活。流れを受ける消費者として席だけ空けておく。表面繊維 (Fiber) は 2026-09-17 から局所の場を受ける消費者になった ([fiber-rendering.md](fiber-rendering.md) の「局所 FlowField」)。GPU 評価は `Rendering/FlowField.hlsli` を粒子と共有する
