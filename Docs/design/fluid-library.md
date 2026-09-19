# 流体ライブラリ — 数式・結合・実行の 3 分割

流体のコードは 13,000 行あって、全部 `Engine/Asset/` にいる。最初の客が `.fluid` の**ベイク**だったから
「アセットの道具」として置かれた — それだけの理由で。ソルバー本体 (CPU の気体・液体・演算子評価) は
Math と CurlNoise しか include していないのに、Engine の中にいる限り誰でも `IRenderer` を足せる。

これを **`Projects/Fluid/`（`FBZZFluid`、`fbzz::fluid`）へ出し、Physics の隣に置く。**
「流体エンジンはどこにあるか」に 1 行で答えられる形にする：**数式は Fluid、結合は Physics、実行は Engine。**

---

## 3 分割

| 関心 | 中身 | 依存してよいもの | 置き場 |
|------|------|------------------|--------|
| **数式** | 移流・圧力投影・PBF 拘束・演算子 (発生源・力・障害物) の評価・焼いた速度場の標本化。将来の格子・高さ場・乱流合成 | Math だけ | `Projects/Fluid/` |
| **結合** | 浮力の法則・流れによる抵抗・コライダーの沈み方 | Math だけ (表面と流速は callback で受ける) | `Projects/Physics/` |
| **実行** | GPU ステップ (HLSL)・ベイク・ファイル形式・Reflect・プリセット・アセットの読み込み・Scene への配線 | RHI・Asset・Scene | `Projects/Engine/` |

```
Editor / Sandbox / GameHub
        │
      Engine ─── ベイク・GPU ステップ・.fluid / 速度場 PNG の読み書き・Reflect・Scene の配線
        │
   ┌────┴────┐
Physics    Fluid          ← 兄弟。互いを知らない
   └────┬────┘
       Math               ← CurlNoise はここへ降ろす
```

**Physics と Fluid は互いを知らない。** `physics::FluidVolume`（浮力）は「この XZ の水面高さ」と
「この点の流速」を `std::function` で受けるので、それが解析場でも格子でも高さ場でも同じ物が動く。
結合を場の実装から独立させるために兄弟にする。

---

## Fluid に置かないもの

リンカーがこれを守る。1 つでも要るなら、それは Fluid の仕事ではない。

- `IRenderer` / `ComputeCall` / `ResourceManager` / `ResourceHandle` — 実行は Engine
- `AssetManager` / `AssetDatabase` / `TextureAsset` — 読み込みは Engine
- `IReflector` / TOML / `FileSystem` / `Logger` — **Fluid はファイル形式を知らない。** 失敗は `bool` で返す (Physics と同じ)
- `RigidBody` / `Collider` — 結合は Physics。Fluid は「体」を知らない
- `Scene` / `GameObject` / `Transform`

---

## 今あるものの行き先

| 分類 | ファイル (`Engine/Asset/`) | 行き先 |
|------|---------------------------|--------|
| そのまま動く | `FluidSolver.hpp` / `FluidGasSolver.cpp` / `FluidLiquidSolver.cpp` / `FluidOperatorEval` / `FluidStepping` / `FluidBakeSettings.hpp` / `FluidGpuStep` (CB のレイアウト記述。RHI 非依存) | `Fluid/` |
| 割ってから動く | `FluidRecipe` / `FluidSourceMask` / `VectorFieldAsset` | 設定型・標本化は `Fluid/`、読み書きは Engine に残す |
| Engine に残す | `FluidGpuSolver` / `FluidGpuLiquidSolver` / `FluidGpuLiquidPack` / `FluidBaker` / `FluidVolumeBake` / `VolumeFlipbook*` / `VectorFieldImporter` / `VelocityFieldAtlas` | `Engine/Asset/` のまま |

`VolumeFlipbookAnalytic` は Math + CurlNoise しか見ないが、フリップブックの**発生源の見本** (煙玉・火球) であって
ソルバーではないので Engine に残す。Fluid の範囲は「解く」と「場を引く」に絞る。

### ほどく 3 か所

**1. `CurlNoise` を Math へ。** `Engine/Core/CurlNoise.hpp` → `Math/CurlNoise.hpp`、`fbzz::core` → `fbzz::math`。
流体の全ファイルと `ParticleForces` が使う純粋な数学で、これが Engine にいる限り何も下へ動かせない。
HLSL (`ParticleNoise.hlsli`) との一対一の契約はそのまま。

**2. `FluidRecipe` を割る。** ヘッダーの struct 群 (`FluidKind` / 形 / `FluidSource` / `FluidForce` / `FluidCollider` /
`FluidGasSettings` / `FluidLiquidSettings` / `FluidRenderSettings` / `FluidOutputSettings` / `FluidRecipe`) と
`ResolveGasResolution` は Math しか見ないので Fluid へ。`.cpp` の TOML 読み書き・`ReflectFluidRecipe`・
プリセット (`FluidPreset` / `MakeFluidPreset` / `FluidPresetName`) は `IReflector` と `FileSystem` に依存するので
Engine に `FluidRecipeCodec` として残す。**設定型とファイル形式を分ける**、それだけ。

**3. `SourceMask` と `VectorField` の向きを逆にする。** 今は Fluid 側の演算子評価が「画像を読むマスク」を include し、
マスクが `TextureAsset` から読む。Fluid には素のデータ (`SourceMask` = 256² の float 配列 / `VectorFieldAsset` = 格子 +
`SampleLocal` + 量子化 + `NormalizeVectorField` + `BakeVectorField`) だけを置き、Engine が Texture や `速度場 PNG` /
`.fga` からそれを埋める。式が Fluid、入口が Engine。

---

## 名前

- ライブラリ `FBZZFluid`、`EXPORT_NAME "Fluid"`、include の根は `Fluid/`、namespace `fbzz::fluid`
- CMake は `Projects/Physics/CMakeLists.txt` を写す (SHARED / `WINDOWS_EXPORT_ALL_SYMBOLS` / `PUBLIC FBZZMath`)。
  TOML は要らない (ファイル形式は Engine)
- **型名とファイル名の `Fluid` 接頭辞は移動の段では触らない** (`fluid::FluidGasSolver` / `Fluid/FluidGasSolver.hpp`)。
  `physics::World` に倣って落とすのは、移動がビルドを通ってからの別の一手。2 つの機械的な変更を 1 度に混ぜない

---

## 段取り

各段の終わりでビルドが通ること。1 段 1 コミット。

1. **`CurlNoise` → Math。** 単独で通る。全 include 元の namespace を直す
2. **`Projects/Fluid/` の骨組み。** CMake、`add_subdirectory` (Physics の後・Engine の前)、`fbzz_engine_module` のリンク先、
   `Projects/Tests/Fluid/` の空の suite。何も入っていなくてもリンクが通る状態
3. **「そのまま動く」を移す。** ディレクトリと namespace だけ。Editor / Tests を含む全 include 元を grep で洗う
4. **3 つを割る。** `FluidRecipe` → 型は Fluid / codec は Engine。`SourceMask` と `VectorField` はデータを Fluid / 読み込みを Engine
5. **周辺。** SDK (`FBZZSDK.cmake` の install・ABI 定義の foreach・`ValidateFBZZSDK.cmake`)、Launcher / Sandbox の DLL コピー、
   カバレッジ (`CMakeLists.txt` の計装対象・`Tools/Coverage/RunCoverageLLVM.ps1`)、`Doxyfile` の INPUT、AGENTS.md の依存方向の行

移すテストは `FluidSolver` / `FluidDeterminism` / `FluidOperatorEval` / `FluidSegmentShape` / `FluidAmountEnvelope` /
`FluidLiquid3D` / `FluidGpuStep` (CB の詰め方だけを見ている)。`FluidRecipeBake` (TOML) / `FluidGpuLiquid` (GPU) /
`VolumeFlipbook*` / `VectorFieldTests` のうちファイルを読む部分は Engine に残る。

---

## この先ここへ足すもの

置き場を決めておく。今は作らない。

| 段 | 中身 | 何が消えるか |
|----|------|-------------|
| 格子 `Grid3` + 実行時ステップ | ベイク用の気体ソルバーを世界の媒質として回す。静的コライダーを障害物に | 遮蔽・保存則・双方向結合 |
| 高さ場 `HeightField2D` | 水面の波動方程式。風応力で立ち、体の通過で押される | 航跡・押し退け |
| カスケード `Cascade` | 近くを細かく遠くを粗く。CSM と同型 | 「粗い格子は細部を持たない」 |
| 乱流合成 `WaveletTurbulence` | 最小セル以下を、格子の速度で移流されエネルギーの釣り合った統計で埋める | 静的ノイズの「凪でも揺れる」 |
| 局所 3D 窓 | 高さ場が折り畳む所だけ PBF を開く | 砕波・水柱 |

どの段も**前の段を壊さず上に乗る**。どこで止めても整合した状態になる — それがこの分割の実利で、
階段の全段を今登る必要はない。

---

## 決めないこと

- `strength` の単位 (加速度 → 流速) や `WaterComponent::buoyancy` の廃止は **`flow-field.md` / `buoyancy.md` の話**。
  この文書は置き場だけを決める。式は 1 行も変えない
- 型名の接頭辞を落とす時期
