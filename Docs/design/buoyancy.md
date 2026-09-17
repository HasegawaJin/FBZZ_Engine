# 浮力 — 法則は Physics、表面は水面、形はコライダー

水面に物を浮かべるのに要るものは 3 つある。**表面がどこにあるか**、**体がどんな形か**、
**水がどれだけ強く押すか**。今は 1 つ目と 3 つ目を `WaterComponent` が両方持ち、2 つ目を
捨てている。**表面を持つのは正しく、法則を持つのが間違い。** 形は Physics が既に知っている。

実装は `Engine/Scene/Components/WaterComponent.hpp`、
`Engine/Scene/Systems/PhysicsSystem.cpp` の `WaterBuoyancyVolume`、
`Physics/Volume.hpp` / `Physics/ColliderVolume.hpp`。

---

## 1. 今の形と、何が変で何が変でないか

| 今 Water 側にあるもの | 判定 | 理由 |
|---|---|---|
| `GetSurfaceHeightAt(x, z, t)` (`WaterComponent.hpp:256`) | **正しい** | 波を知っているのは水面だけ。Gerstner の逆写像も `WaveMeshFade` もここにしか無い |
| `physics::Volume` を継承して `World::SyncVolume` へ渡す (`PhysicsSystem.cpp:519-533`) | **正しい** | 依存方向を守ったまま抽象で渡している。Physics は `WaterComponent` を知らない |
| `buoyancy = 15.0f [m/s²]` (`WaterComponent.hpp:53`) | **変** | 「この水がどれだけ強く押すか」は物理法則であって水面の性質ではない。水を差し替えると法則が変わることになる |
| `WaterBuoyancyVolume` が `PhysicsSystem.cpp:96-190` の無名 namespace にいる | **変** | 法則の実装が Engine の cpp に閉じ込められ、**Physics 側に浮力が 1 行も無い**。テストも Engine を起こさないと書けない |
| `VolumeType::Buoyancy` がもう 1 本ある (`ColliderVolume.cpp:78-81`) | **明確に間違い** | 法則が 2 つあり、片方を `if (go.GetComponent<WaterComponent>()) return;` で黙らせている (`PhysicsSystem.cpp:300-303`) |

`VolumeType::Buoyancy` を使う `VolumeComponent` は **`GreenWare/Assets` に 1 件も無い**
(`type = 'Buoyancy'` で全 `.scene` / `.prefab` を検索済み)。Inspector は今でも
「この Volume は無視される」と黄色で知らせている (`InspectorTerrainWater.cpp:276-286`) —
**警告を出さないといけない時点で、境界が間違っている。**

---

## 2. 今の法則の限界 — 船が浮かない理由

`MakeBodyRadii` (`PhysicsSystem.cpp:82-91`) が、剛体に属する非トリガーコライダーの
`ComputeVolume()` の合計 (`PhysicsSystem.cpp:281`) を**等価球の半径** `r = ∛(3V/4π)`
へ直す (0.05〜50 m でクランプ)。`WaterBuoyancyVolume::Apply` はその球で押す。

```
arm      = 0.6 r                                    体の回転で回した水平 4 点
sub      = clamp01((surfaceY − (probe.y − r)) / 2r)  点ごとの沈み率
lift     = UP · buoyancy · mass · sub · 0.25
torque   = Σ offset × lift
drag     = (flow − v) · waterDrag · mass · Σ(sub·0.25)
angular  = −ω · waterDrag · (0.4 m r²) · Σ(sub·0.25)
```

**ここで船体の長さが失われる。** 等価球はどの向きから見ても同じ形なので、長い船が波に
乗ってもピッチしない。腕は `±X` と `±Z` の水平 4 点に固定 (`PhysicsSystem.cpp:134-140`)
なので、幅と長さの違いも縦方向の分布も出ない。**質量 × 加速度で押している**ため、
同じ体積の木と鉄がまったく同じ速さで浮き上がる — 実世界で最初に見分けがつくところが出ない。

一方、**この 2 つは正しいので残す**。

- `ApplyForceNoWake` / `ApplyTorqueNoWake` (`PhysicsSystem.cpp:152-163`) — 浮力と抵抗は毎
  substep 掛かる環境力で、`WakeUp` すると水面範囲で止まった body が永久に Sleep できない
- `buoyancyDepth` (`WaterComponent.hpp:57-59`) — 水面は厚みの無い板なので、上限が無いと
  **水面の真下にある洞窟の中まで浮力が届く**

---

## 3. 既に持っているもの

| 要るもの | 現状 |
|---|---|
| 体の密度 | ✅ `PhysicsMaterial::density` (`PhysicsMaterial.hpp:48`) + `MassMode::FromDensity` (`RigidBodyComponent.hpp:22-25`)。質量 = Σ `ComputeVolume()` × `density` (`PhysicsSystem.cpp:261`) |
| コライダー体積 | ✅ `Collider::ComputeVolume()` (`Collider.hpp:39`)。Sphere / AABB / OBB / Capsule / Cylinder が厳密解を override、残りは AABB 体積で近似 (`Collider.cpp:27`) |
| 水面の高さ | ✅ `GetSurfaceHeightAt` / `ScriptWaterProxy::GetSurfaceHeightWorld` |
| 流速 | ✅ `WaterComponent::current` (`.mat` の `flowDirection × currentSpeed`) |
| **形に沿った沈み具合** | ❌ 等価球で潰している |

**足りないのは 1 つだけ。** 密度も体積も、質量を出すために既に毎フレーム計算している。

---

## 4. 目標の法則

```
F = ρ_fluid · g · V_submerged      作用点は沈んだ部分の重心 (center of buoyancy)
```

密度モードの体なら `mass = ρ_body · V` なので、**浮くか沈むかは ρ_fluid と ρ_body の比だけで
決まる**。木は浮き、鉄は沈む。`buoyancy [m/s²]` というノブは要らなくなる。

「コライダーのどの位置がどれだけ沈んでいるか」の答えは、**形ごとのサンプル点**で出す
(契約名は仮に `ISubmergible` / `SampleSubmersion`)。

| 形 | 標本 |
|---|---|
| Sphere | 球冠の解析解。サンプル不要 |
| Box / AABB / OBB | 8 頂点 + 中心 |
| Capsule / Cylinder | 軸に沿った数点 |
| ConvexHull | 頂点 |
| TriangleMesh / HeightField | hull へ落とすか、既定どおり AABB |

各点が `ρ_fluid · g · (V/N) · 沈み率` を**その点に**掛ける。トルクは外積から自然に出るので、
**船体の長さがピッチに、幅がロールに効く。** 抵抗も点ごとに `k · (m/N) · (v_flow − v_point)`
で掛ける — 点ごとに流速が違えば、それがそのまま回転になる。角速度の減衰は今と同じ考えで残す。

調整は `.physmat` の密度で行う。それでも演出上「もっと浮かせたい」が要るなら、
`RigidBodyComponent` に `buoyancyScale = 1` を置く。**倍率は体側に置く** —
水側へ戻すと「この水は法則が違う」が復活して、今と同じ場所へ戻る。

> `RigidBody` に `ApplyForceAtPoint` はあるが (`RigidBody.hpp:30`) NoWake 版が無い。
> 今と同じく `ApplyForceNoWake` + `ApplyTorqueNoWake` を自分で組むか、NoWake 版を足す。

---

## 5. 所有

| 層 | 置くもの |
|---|---|
| **Physics** | `physics::FluidVolume` — 法則 (`ρ·g·V_sub`)・サンプル点の集約・抵抗。入力は `float(x, z)` の表面高さ callback、`Vector3(p)` の流速 callback、`FluidParams { density, drag, depthLimit }`。各 `Collider` に `SampleSubmersion(surfaceFn, out samples[])` |
| **Engine** | `WaterComponent` — 表面だけ (波・`ρ_water`・`current`・`buoyancyDepth`)。`PhysicsSystem` — 配線だけ (Water を `FluidVolume` として毎フレーム申告する) |

```
        Engine ─── WaterComponent / PhysicsSystem (配線)
          │
   ┌──────┴──────┐
Physics        Fluid        ← 兄弟。互いを知らない
   └──────┬──────┘
         Math
```

Physics は表面と流速を `std::function` で受けるので、**Water も将来の格子・高さ場も知らない**
(`fluid-library.md`)。溶岩でも泥でも同じ法則が動く。`Volume` の `Contains(position)` は
今と同じく派生が答える — 矩形 × 深さ上限の判定は `WaterComponent` 側の寸法が要るので、
callback で渡すか `FluidParams` に箱を持たせる。

**`FlowField` には入れない。** 浮力は体の体積と形に依存し、場は体を知らない。流速を配るのは
`flow-field.md` の担当で、浮力は「法則」。同じ場に同居させると、今の `WaterComponent` と
同じ混ざり方をもう一度やることになる。

> `flow-field.md` §2 は剛体の流れ抵抗を `physics::FlowVolume` (`F = k·m·(v_flow − v)`) に置く。
> **`FluidVolume` の抵抗はその同じ結合を、沈み率で重み付けして点ごとに掛けたもの。**
> 係数の持ち主も同じ (体側)。どちらが先に入っても、後から来た側が結合式を共有すればよい。

---

## 6. 移行

**`buoyancy = 15` を密度比へ。** 完全に沈んだとき、今は `lift/mass = buoyancy`、
目標は `lift/mass = (ρ_w/ρ_b)·g`。等置して `ρ_w/ρ_b = 15/9.81 ≈ 1.53`。
`SceneSerializer` で旧キー `buoyancy` を水の `density` へ写す (体の密度が 1 のときに
同じ挙動になる値)。旧キーは読むだけで書かない。

**`VolumeType::Buoyancy` は削除する。** 使用アセットはゼロだが、コードの参照は 7 か所ある
(`ColliderVolume.hpp:14` / `ColliderVolume.cpp:78-81` / `SceneSerializer.cpp:384,395` /
`PhysicsSystem.cpp:295,300-303` / `VolumeComponent.hpp:27,41,50` /
`InspectorPhysics.cpp:474,485` / `InspectorTerrainWater.cpp:276-286` /
`ScriptVolumeProxy::SetBuoyancy`) と、テストが 2 件 (`ColliderVolumeTests.cpp:188,301`)。
列挙値を消すと `type` の int 保存 (`VolumeComponent.hpp:41` の上限 5) もずれるので、
**Serializer は文字列 ↔ 列挙を持っている** (`SceneSerializer.cpp:384-395`) ことを利用して
旧文字列を読み捨てる経路だけ残す。

**既存シーンの絵は変わる。** 密度が正しく効くようになるので、今「なんとなく浮いていた」物は
浮き方が変わる。GreenWare は完成品だが、この設計ではそれを理由に法則を歪めない。

---

## 7. テスト (`Tests/Physics/Auto/`、Engine 無しで回る)

解析解と突き合わせる。法則が Physics にあることの実利がここに出る。

| 見るもの | 期待 |
|---|---|
| 球が半分沈んだとき | 揚力 = `ρ_w · g · (2/3)πr³` |
| 箱を傾けて沈める | トルクの符号が水平へ戻す向き |
| 中立密度 (`ρ_b = ρ_w`) | 水平に浮いて止まる (沈みも浮き上がりもしない) |
| 流速のある水 | 速度が `v_flow` へ収束する |
| `depthLimit` の外 | 力ゼロ |

float は `EXPECT_VEC3_NEAR` 等 (`TestKit/Approx.hpp:83`)。`TEST_F` + fixture で、
乱数・時刻・sleep は持ち込まない (`Docs/conventions/test.md`)。表面 callback は
`[](float, float){ return 0.0f; }` のような固定平面で足りる — **波を巻き込まずに法則だけ試せる**
のが、`std::function` で受ける形にした理由そのもの。

---

## 8. 順序

1. **この文書で境界を固定する。** コードは 1 行も動かさない
2. **`WaterBuoyancyVolume` を `physics::FluidVolume` として Physics へ移す。**
   **式は今のまま** (等価球・4 点・`buoyancy·mass`)。表面と流速を callback に切り替えるだけ。
   同じコミットで `VolumeType::Buoyancy` を削除する。**絵が変わらない一手**
3. **密度ベースの法則 + 各 Collider の `SampleSubmersion`。** ここで初めて絵が変わる
4. **`WaterComponent::buoyancy` を廃止**し、`density` を持たせて Serializer に移行を書く
5. **Physics のテスト。** 3 と 4 の間でも書けるが、解析解と比べられるのは 3 の後

各段の終わりでビルドが通ること。1 段 1 コミット。

---

## 9. 決めないこと

- 水面そのものの形 (Gerstner / 高さ場) → `water-waves.md` / `fluid-library.md`
- 流れの場と `current` の単位 → `flow-field.md`
- 3D の水体積 (潜る・水柱・押し退け) → `fluid-library.md` の「局所 3D 窓」
- `density` の単位を実寸 (kg/m³) へ揃えるか。既存の唯一の `.physmat` は `density = 25000`
  で、`PhysicsMaterial.hpp:48` の「kg/m³ 相当」は既に守られていない。**比だけで決まる法則に
  すると単位の食い違いが目に見える**ので、揃えるならこの設計の後
