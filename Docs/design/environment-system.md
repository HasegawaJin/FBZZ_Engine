# 環境システム設計ドキュメント
## Sky / Cloud / Water / Fog / EnvironmentLighting を「空連動」で統合する

---

## 0. このドキュメントの位置づけ

ユーザー提案の `EnvironmentRenderer` ツリーをそのまま実装する前に、**既存資産との重複・抽象度・依存順序**を整理し、現実的な統合方針を決める。コードはまだ書かない（設計合意が目的）。

提案された理想ツリー（ゴール像）:

```
EnvironmentRenderer
 ├─ AtmosphereRenderer { Sky, AerialPerspective }
 ├─ CloudRenderer      { VolumetricClouds, CloudShadow }
 ├─ WaterRenderer      { Ocean, Lake, River, Foam, Caustics }
 ├─ FogRenderer
 ├─ SunMoonRenderer
 └─ EnvironmentLighting { SkyLight, ReflectionProbe, IBL }   ← IBL は空から生成
```

**結論の先出し:** 方向性は正しく、`IBL = 空から生成` が全体の**要石**。ただしツリーをそのまま実装すると既存実装を捨てることになり、また「描画パス」と「ライティングリソース」を混同している。**既存を活かす結線と、依存順序の再設計**として捉え直す。

---

## 1. 現状整理 — 何が既にあるか

| 提案ノード | 既存実装 | 状態 |
|---|---|---|
| Atmosphere / Sky | `Skydome.hlsl` + `Atmosphere.hlsli`（Rayleigh+Mie 単一散乱・太陽ディスク） | ✅ あり |
| Cloud | `Cloud.hlsli`（FBM 手続き型・平面レイヤー・セルフシャドウ近似） | ◯ 簡易版あり |
| Water (Ocean/Lake/River) | 単一 `WaterRenderPass` + `Water.hlsl`（Gerstner・SSR・屈折・泡・接岸） | ✅ あり（**既に統合済み**） |
| Foam | `Water.hlsl` 内（接岸泡＋ペイントマスク） | ✅ あり |
| Caustics | `UnderwaterCaustics` パス + `PostProcess/Water/Caustics.hlsl`（画面空間・水没時） | ◯ 水中のみあり |
| Fog | `Composite.hlsl` の指数フォグ（深度復元） | ✅ あり |
| SunMoon | 太陽ディスクは `Atmosphere.hlsli::SunDisk` のみ。月・昼夜遷移は無し | ◯ 太陽のみ |
| EnvironmentLighting / IBL | `IBLBrdfBake` パス・`DX11IblBaker`（irradiance/prefilter/BRDF LUT compute）・`EnvironmentLightComponent`・`ReflectionProbeComponent` | ◯ **オフライン経路のみ** |
| VolumetricClouds（レイマーチ） | 無し | ❌ 新規 |
| CloudShadow（投影） | 無し | ❌ 新規 |
| AerialPerspective | 無し（Composite フォグで代替中） | ❌ 新規 |

**重要な事実:**
- **Water は既に 1 パスに統合済み** → Ocean/Lake/River への分割は不要（後述）。
- **Caustics は既にある**（ただし「カメラ水没時の画面効果」。水面越しに水底へ落ちる**投影コースティクス**は別物で未実装）。
- **IBL の計算機構（compute）は完成している**が、入力は「HDRI ファイル→DDS をディスクに焼く**オフライン経路**」。`ReflectionProbeComponent.hpp` にも *「将来は動的キャプチャへ拡張予定。現時点は静的 .dds のみ」* と明記済み。
- つまり要石 `IBL = 空から生成` の実体は **「空キューブマップを実行時に生成し、既存 compute へ流す runtime 経路の新設」**。コンピュートシェーダは流用できる。

### 1-1. このエンジンの命名規則

| 種別 | 規則 | 例 |
|---|---|---|
| システム（処理の所有者） | `XxxSystem` | `RenderSystem` / `AnimatorSystem` |
| 設定コンポーネント（描画を伴う） | `XxxRenderer` | `SkyRenderer` / `MeshRenderer` |
| 設定コンポーネント（その他） | `XxxComponent` | `WaterComponent` / `ReflectionProbeComponent` |
| RenderPass クラス（`AddPass<T>()`） | `XxxRenderPass` | `WaterRenderPass` / `TerrainRenderPass` |
| raw パス名（`AddRawPass("...")`） | PascalCase 文字列 | `"Sky"` / `"IBLBrdfBake"` / `"UnderwaterCaustics"` |
| パスのリソース名 | PascalCase 文字列 | `"HDR"` / `"ShadowMap"` / `"GBuffer"` / `"Bloom"` |
| 定数バッファ | `XxxCB` / `XxxConstants` | `AtmosphereCB` / `WaterCB` / `LightConstants` |
| IBL ベイカー | `IXxx` / `DX11Xxx` | `IIblBaker` / `DX11IblBaker` |
| シェーダー配置 | カテゴリ別ディレクトリ | `Material/Sky/` / `Rendering/*.hlsli` / `PostProcess/Water/` |

> ⚠️ 既存の `AtmosphericScatteringComponent` は名前に反して**実体はフォグ設定**（`SkyRenderer` が散乱パラメータを持つ）。歴史的経緯なので**改名はしない**（広範な参照があり churn が大きい）。本ドキュメントでは下表の対応で扱う。

### 1-2. 提案ツリー → エンジン採用名の対応表

| 提案ノード | エンジンでの呼称 | 種別 | 既存/新規 |
|---|---|---|---|
| `EnvironmentRenderer`（頂点） | `EnvironmentResources`（状態オブジェクト）+ raw パス群 | レンダー層ヘルパー | 🆕 新規 |
| `AtmosphereRenderer / Sky`（設定） | `SkyRenderer` | Component | ✅ 既存 |
| `AtmosphereRenderer / Sky`（描画） | `"Sky"` raw パス（`SkyPass.cpp`） | RawPass | ✅ 既存（改修） |
| 空をキューブマップへ焼く（①） | `"SkyCapture"` raw パス | RawPass | 🆕 新規 |
| `EnvironmentLighting / IBL` 畳み込み（②） | `"SkyLightBake"` raw パス | RawPass | 🆕 新規 |
| `EnvironmentLighting / SkyLight` | `EnvironmentLightComponent` | Component | ✅ 既存 |
| `EnvironmentLighting / ReflectionProbe` | `ReflectionProbeComponent` | Component | ✅ 既存 |
| `EnvironmentLighting / IBL`（compute） | `IIblBaker` / `DX11IblBaker` | Renderer | ✅ 既存（runtime 経路追加） |
| `WaterRenderer`（設定） | `WaterComponent` | Component | ✅ 既存 |
| `WaterRenderer`（描画） | `WaterRenderPass` | RenderPass | ✅ 既存 |
| `WaterRenderer / Ocean·Lake·River` | `WaterComponent` のプリセット値 | （データ） | ✅ 既存で表現 |
| `WaterRenderer / Foam` | `Water.hlsl` 内蔵 | （機能） | ✅ 既存 |
| `WaterRenderer / Caustics`（投影） | `"WaterCaustics"` raw パス | RawPass | 🆕 新規（水中版 `"UnderwaterCaustics"` は既存） |
| `FogRenderer / AerialPerspective` | `AtmosphericScatteringComponent` + Composite フォグ | Component | ✅ 既存（ソース追加） |
| `SunMoonRenderer` | `SunMoonRenderer` + `"SunMoon"` パス | Component | ✅ 既存 |
| `CloudRenderer`（設定） | `CloudComponent` | Component | 🆕 新規 |
| `CloudRenderer / VolumetricClouds` | `"VolumetricCloud"` raw パス | RawPass | 🆕 新規 |
| `CloudRenderer / CloudShadow` | `"CloudShadow"` raw パス + `"CloudShadowMap"` リソース | RawPass | 🆕 新規 |

**新設リソース名（パスの入出力宣言用）:** `"SkyEnvCube"` / `"SkyIrradiance"` / `"SkyPrefilter"` / `"CloudShadowMap"`

---

## 2. 提案ツリーの評価（採用 / 修正 / 却下）

| ノード | 判定 | 理由 |
|---|---|---|
| `EnvironmentLighting` を `EnvironmentRenderer` の子に置く | **修正** | SkyLight/ReflectionProbe/IBL は「全 Lit シェーダーが**消費するリソース**」で、描画パスとは寿命も実行タイミングも別。レンダラーの兄弟ではなく**リソース提供システム**に分離する。 |
| `WaterRenderer { Ocean, Lake, River }` の3分割 | **却下** | 3者は「Gerstner 振幅・流速・フローマップ違い」の**データ駆動プリセット**で足りる。既に単一パスで動いている。1 シェーダー + マテリアルプリセットに保つ。 |
| `Water { Foam, Caustics }` をサブノード化 | **採用（概念のみ）** | Foam/Caustics は Water の**サブ効果**。クラス階層化はせず、Water マテリアルのフィーチャトグルとして扱う。 |
| `AerialPerspective` と `FogRenderer` を別立て | **修正（統合）** | エアリアルパースペクティブ＝大気散乱由来の距離フォグ。既存 Composite フォグと二重実装になる。**フォグは1系統**にし「大気から係数を引く」オプションにする。 |
| `CloudRenderer { VolumetricClouds }` | **採用（後回し）** | レイマーチ雲は大物。現状の FBM 平面雲を活かしつつ、Phase D で段階導入。 |
| `CloudShadow` | **採用（Phase C）** | 雲テクスチャ/密度を地表へ投影。中規模。 |
| `SunMoonRenderer` | **採用（Phase B）** | 太陽ディスクは既存。月＋昼夜遷移を足して `EnvironmentLightComponent` と連動させる。 |
| `AtmosphereRenderer { Sky }` | **採用（再構成）** | 「大気の**計算**（LUT/キューブマップ）」と「空ドームの**描画**」を分離するのが肝（§4）。 |

---

## 3. アーキテクチャ原則

### 3-1. 「描画パス」と「環境リソース提供者」を分ける

```
描画パス (RenderPass)                環境リソース提供者 (EnvironmentResources)
────────────────────                ──────────────────────────────────────
Sky ドーム塗り                       空キューブマップ
Water / Cloud / Fog 描画             irradiance / prefilter / BRDF LUT (IBL)
                                     SkyLight 方向光・色（昼夜で変化）
↑ HDR バッファへ書く                 ↑ 全 Lit シェーダーが SRV として読む
↑ 毎フレーム実行                     ↑ 太陽が動いた時だけ再生成（キャッシュ）
```

`EnvironmentLighting` を描画パスの兄弟にすると、この「生成タイミングの違い」が表現できない。**リソースは別システムが所有**し、パスはそれを参照するだけにする。

### 3-2. Water はデータ駆動の1系統を維持

Ocean/Lake/River は `WaterComponent` のパラメータ（Gerstner 波数・振幅・流速・フローマップ有無）で表現する。マテリアルプリセット（`.mat` / コンポーネント既定値）として配布し、シェーダーとパスは1本に保つ。

### 3-3. フォグは1系統に統合

`AtmosphericScatteringComponent` に `fogSource` を追加（C++ は camelCase フィールド）:

```
fogSource = Exponential   → 従来の指数フォグ（既存）
fogSource = Atmosphere    → 大気散乱の透過率・インスキャッタから係数を引く（新規・エアリアル）
```

`Composite.hlsl` のフォグ適用箇所を分岐させ、係数の出どころだけ差し替える。フォグの**適用ロジックは共通**。

---

## 4. 依存順序（最大の論点）

### 4-1. 現状の順序（空が不透明描画の後）

```
IBLBrdfBake → Shadow → Opaque/GBuffer → Terrain/Detail/Foliage
            → Sky → (GTAO/SSAO) → DeferredLighting → … → Water → … → Composite
```

`Sky` が**不透明ライティングより後**にある。これは「空連動 IBL」と両立しない（不透明物を照らす時点で空がまだ確定していない）。

### 4-2. 提案する順序（大気の「計算」を前倒し）

```
① "SkyCapture"     ← 太陽が動いた時だけ。空を低解像度キューブマップ "SkyEnvCube" に焼く
② "SkyLightBake"   ← ①が変わった時だけ。既存 compute で "SkyIrradiance" + "SkyPrefilter" 生成
③ "Shadow"
④ "ForwardOpaque" / "DeferredGBuffer" + "DeferredLighting"   ← ②の IBL を参照（空連動）
⑤ TerrainRenderPass / DetailRenderPass / FoliageRenderPass   ← ②の IBL を参照
⑥ "Sky"（ドーム塗り）            ← ①の "SkyEnvCube" をそのまま表示しても良い
⑦ WaterRenderPass / 半透明        ← ②の IBL + コピー済み sceneColor を参照
⑧ "VolumetricCloud" → ⑨ "Composite"（フォグ/エアリアル統合, §3-3）
```

（既存の `"Sky"` パスは現状 ④⑤ の後にあるが、本設計では「大気の計算①②」を前倒しし、`"Sky"` は可視ドーム描画⑥としてのみ残す。）

**性能の肝:** ①② は「太陽方向・大気パラメータが変化したフレームだけ」実行しキャッシュする。毎フレーム IBL を焼くと重い。昼夜遷移中は数フレームに1回の更新＋補間で十分。

**注意:** ⑥ の空ドーム描画は現状を流用できるが、①で既にキューブマップ化しているなら「ドームにキューブマップを貼る」方式へ寄せると、可視の空と IBL の見た目が完全一致する（破綻しない）。

---

## 5. モジュール構成（既存の流儀に合わせる）

### 5-1. スケジューラ System は作らない（重要な決定）

**`EnvironmentSystem` のようなスケジューラ `XxxSystem` は新設しない。**

理由は、この engine の `XxxSystem` が「スケジューラが毎フレーム呼ぶ**ステートレス関数**」だから（[RenderSystem.hpp:41](Projects/Engine/include/Engine/Scene/Systems/RenderSystem.hpp#L41) は自由関数で、毎フレーム `RenderPipeline` を組み直す）。環境システムが要求するのは正反対の2つで、System 抽象とかみ合わない:

1. **フレームをまたぐ GPU リソースの保持** — 空キューブマップ・irradiance・prefilter・dirty 状態。ステートレス関数は持てず、結局 `static`/`ResourceManager` に逃がすことになり System を作る意味が消える。
2. **レンダーパイプライン内での厳密な順序** — 空連動 IBL は `"ShadowMap"`/`"HDR"` と依存解決された順序で動く必要がある。別 System 関数に切り出すと `RenderPipeline` の依存グラフ（リソース宣言で順序が決まる仕組み）の外に出てしまう。

→ 既に `"Sky"` / `"IBLBrdfBake"` が **RenderSystem パイプライン内の raw パス**として存在する。環境系もそこに並べ、永続状態は **`OcclusionCuller`（`RenderPasses/` の System でないヘルパー）のような状態オブジェクト** に持たせる。

### 5-2. 構成

この engine は深い継承ではなく **パス合成（`AddPass<T>` / `AddRawPass(...)`）** でパイプラインを組む。提案ツリーもそれに乗せ、名前は §1-1/§1-2 の慣習に揃える:

```
EnvironmentResources（🆕 状態オブジェクト・System ではない / OcclusionCuller 相当）
  └─ 保持: "SkyEnvCube" / "SkyIrradiance" / "SkyPrefilter" ハンドル + skyLightDir·Color + dirty フラグ
     （フレームをまたいで生存。RenderPassContext 経由で各パスが読み書き）

RenderSystem パイプライン内の raw パス（既存 "Sky"/"IBLBrdfBake" の隣に追加）:
  ├─ "SkyCapture"      （①）  ← dirty 時のみ実行（SkyRenderer の散乱で空を描画）
  ├─ "SkyLightBake"    （②）  ← dirty 時のみ実行（DX11IblBaker の compute を runtime 化）
  ├─ "Sky"             （⑥, 既存 SkyPass.cpp を改修＝可視ドームのみ）
  ├─ "VolumetricCloud" （⑧, 既存 Cloud.hlsli を昇格）
  └─ フォグ/エアリアルは "Composite" 内で係数差し替え（§3-3）

WaterRenderPass（既存 XxxRenderPass・据え置き）  ← EnvironmentResources の IBL を参照するよう結線
SkyRenderer 拡張（Phase B）                       ← 太陽/月の位置・色を EnvironmentResources に供給
```

**設定コンポーネントの所在（既存を流用）:**

| 役割 | コンポーネント |
|---|---|
| 大気・太陽・（将来）月の設定 | `SkyRenderer`（既存・拡張） |
| グローバル IBL（=SkyLight）設定 | `EnvironmentLightComponent`（既存・`source` トグル追加） |
| 局所反射 | `ReflectionProbeComponent`（既存） |
| フォグ設定 | `AtmosphericScatteringComponent`（既存・`fogSource` 追加） |
| 水面設定 | `WaterComponent`（既存） |
| 雲設定 | `CloudComponent`（🆕 新規） |

- `EnvironmentResources` は **状態の入れ物**であって処理の所有者ではない。dirty 判定とパス登録は RenderSystem 側で行う。
- `EnvironmentRenderer` を頂点とする**継承ツリーにはしない**（この engine に `XxxRenderer` という System 抽象は存在しない）。
- 依存方向は既存通り `Engine` 内に閉じる（Editor/GameHub から実装に触れない）。

> 例外: 描画と無関係に毎フレーム回る環境シミュ（例: 昼夜の時刻を進める）が必要になっても、`SkyRenderer` のフィールド更新程度で済み、専用 System を立てるほどではない。

---

## 6. フェーズ別ロードマップ

### Phase A — 空 → 動的 IBL（要石・高価値 / 中工数）

| 対象 | 変更内容 | 規模 |
|---|---|---|
| `EnvironmentResources`（🆕 状態オブジェクト） | 空キューブマップ・IBL テクスチャ・dirty フラグの保持（System ではない / §5-1） | S |
| RenderSystem 側の dirty 判定・パス登録 | `SkyRenderer` 変化検知で `"SkyCapture"`/`"SkyLightBake"` を条件登録 | M |
| `"SkyCapture"` raw パス（🆕） | 空を低解像度キューブマップ `"SkyEnvCube"` へ描画（既存 `Atmosphere.hlsli` 流用） | M |
| `"SkyLightBake"` raw パス（🆕） | `"SkyEnvCube"` → `"SkyIrradiance"`/`"SkyPrefilter"`（`DX11IblBaker` の compute を runtime 経路化） | M |
| `IIblBaker` / `DX11IblBaker` | ファイル出力（`Bake()`）に加え **GPU テクスチャ直書き**の API（例: `BakeToTextures()`）を追加 | M |
| `EnvironmentLightComponent` | `source = StaticDDS / DynamicSky` トグル追加（既定は後方互換で StaticDDS） | S |
| `RenderSystem` 結線 | パス順序を §4-2 に変更（Sky 計算を不透明前へ） | M |

**Phase A 完了で得られるもの:** 地形・水・メッシュが**実際の空で照らされ**、先日の Sky/Water の HDR 化（リニア出力・事前トーンマップ除去）が初めて活きる。

### Phase B — 大気フォグ統合 + Sun/Moon（中）

| 対象 | 変更内容 | 規模 |
|---|---|---|
| `Composite.hlsl` フォグ | `fogSource = atmosphere` 分岐追加（§3-3） | S |
| `SunMoonRenderer` + `"SunMoon"` パス | 太陽・月ディスクを Sky から分離して加算描画。昼夜のライト駆動は SkyRenderer の既存時刻設定を使う | M |
| `EnvironmentLightComponent` | 時刻 → 太陽方向/色のカーブ | S |

### Phase C — 雲・水のポリッシュ（表層効果）

| 対象 | 変更内容 | 規模 |
|---|---|---|
| `Cloud.hlsli` | ライティング改善（既に着手済み: シルバーライニング）・密度プロファイル調整 | S |
| `"CloudShadow"` raw パス（🆕） | 雲密度を地表へ投影し `"CloudShadowMap"` を生成、シャドウに合成 | M |
| `"WaterCaustics"` raw パス（🆕） | 水面越しに水底へ落ちる投影コースティクス（既存 `"UnderwaterCaustics"` とは別） | M |

### Phase D — 大物（後回し）

| 対象 | 変更内容 | 規模 |
|---|---|---|
| `"VolumetricCloud"` raw パス + `CloudComponent` | レイマーチ雲（FBM 平面雲からの置換） | L |
| `WaterComponent` River プリセット | フローマップ駆動の河川シミュ | L |

---

## 7. リスク評価

| リスク | 内容 | 対策 |
|---|---|---|
| IBL 毎フレーム再生成で重い | 動的キャプチャ＋畳み込みは高コスト | dirty フラグでキャッシュ。昼夜遷移は数フレームに1回更新＋補間 |
| 描画順序変更の副作用 | Sky 計算を不透明前へ動かすと既存パスの依存が崩れる | `AddRawPass` の入出力依存（"HDR" 等）を明示し、ビルダーに順序を解決させる |
| 見た目の回帰 | 空連動で全シーンのライティングが変わる | `EnvironmentLightComponent.source` で `StaticDDS` にフォールバック可能にし、既存シーンを壊さない |
| IblBaker の二重 API | ファイル出力版とテクスチャ直書き版が並立 | 内部 compute を共通化し、出力先（DDS / GPU テクスチャ）だけ差し替える |
| スコープ肥大 | ツリー全体を一気に作ると終わらない | Phase A 単独で価値が出る設計にし、B 以降は独立して着手可能にする |

---

## 8. 現実性の判断 / 非目標

**着手すべき:** Phase A（空 → 動的 IBL）。compute は既存、`ReflectionProbe` も拡張を予定済みで、**コードベースが既にこの方向を想定している**。単独で「全オブジェクトが空で照らされる」という明確な価値が出る。

**やらないこと（過剰設計の回避）:**
- Water の Ocean/Lake/River 3分割 → `WaterComponent` のデータ駆動プリセットで代替（§2）。
- `AerialPerspective` を独立フォグ系統として新設 → 既存 `AtmosphericScatteringComponent` + Composite フォグに係数差し替えで統合（§3-3）。
- `EnvironmentSystem` 等の新スケジューラ `XxxSystem` → System はステートレス関数で GPU リソースを持てず、パイプライン順序の外に出る。`EnvironmentResources`（状態オブジェクト）+ RenderSystem 内 raw パスで表現（§5-1）。
- `EnvironmentRenderer` 等の新 System 抽象 → System はステートレス関数で GPU リソースを持てず、パイプライン順序の外に出る。描画系は `SunMoonRenderer` のような既存 Component + RenderPass で表現する。
- `"VolumetricCloud"` を最初に作る → 大物。FBM 平面雲を活かし Phase D へ。

---

## 9. 推奨実装順序

```
1. EnvironmentResources（状態オブジェクト・System ではない）を作り、空キューブマップ/IBL ハンドルと dirty フラグを保持
2. "SkyCapture" raw パス: 既存 Atmosphere.hlsli で空を "SkyEnvCube" へ描画（RenderSystem パイプラインに登録）
3. DX11IblBaker に GPU テクスチャ直書き API（BakeToTextures 等）を追加（既存 compute 流用）
4. "SkyLightBake" raw パス: "SkyEnvCube" から "SkyIrradiance"/"SkyPrefilter" を実行時生成
5. RenderSystem のパス順序を §4-2 に変更（Sky 計算を不透明ライティング前へ）
6. EnvironmentLightComponent に source = StaticDDS/DynamicSky トグル追加
   → ここまでで Phase A 完了（空連動 IBL）
7. Composite フォグへ fogSource = atmosphere を追加 + SkyRenderer に Sun/Moon（Phase B）
8. "CloudShadow" / "WaterCaustics"（Phase C）
9. "VolumetricCloud" / River プリセット（Phase D）
```

---

## 10. 参考：既存パイプライン該当箇所

- 空パス: `Projects/Engine/src/Scene/Systems/RenderPasses/Geometry/SkyPass.cpp`
- IBL ベイク: `Projects/Engine/src/Renderer/Platform/DX11/DX11IblBaker.cpp` / `Projects/Engine/include/Engine/Renderer/IIblBaker.hpp`
- 環境ライト/プローブ: `EnvironmentLightComponent.hpp` / `ReflectionProbeComponent.hpp`
- パイプライン定義: `Projects/Engine/src/Scene/Systems/RenderSystem.cpp`（`IBLBrdfBake` / `Sky` / `WaterRenderPass` / `UnderwaterCaustics` / `Composite`）
- フォグ・トーンマップ: `Assets/Shaders/PostProcess/Color/Composite.hlsl`
- 大気・雲: `Assets/Shaders/Rendering/Atmosphere.hlsli` / `Assets/Shaders/Rendering/Cloud.hlsli`
</content>
</invoke>
