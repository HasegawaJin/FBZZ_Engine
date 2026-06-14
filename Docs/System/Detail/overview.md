# DetailSystem 設計 — 全体概要

## 目的

Terrain 上に草・岩・花・低木などの小オブジェクトを **GPU Instancing** で大量描画するシステム。
Unity の Terrain Detail System / Unreal の Landscape Grass Output に相当し、`TerrainComponent` の描画拡張として機能する。

草専用の高品質描画（プロシージャルブレード・風アニメーション）は `DetailLayerType::Grass` として同一フレームワーク内に収め、後から差し替え可能な設計にする。

---

## DetailSystem と GrassRenderer の関係

```
DetailSystem（本システム）
├── DetailLayerType::Mesh       岩・花・低木などを 3D メッシュ Instancing で描画
├── DetailLayerType::Billboard  カメラ向きクワッドで遠景を安く描画
└── DetailLayerType::Grass      プロシージャルな草ブレードを専用シェーダーで描画
                                ↑ いわゆる「GrassRenderer」はここだけを指す
```

**GrassRenderer は DetailSystem の 1 レイヤータイプ**。独立したシステムにはしない。

---

## 現在の実装状況

| 機能 | 状態 | 備考 |
|---|---|---|
| TerrainComponent / TerrainRenderSystem | 実装済み | Detail の基盤となる地形 |
| GPU Instancing | 未実装 | DrawInstanced パスがまだない |
| DetailRenderSystem | 未実装 | — |
| TerrainDetailComponent | 未実装 | — |
| Billboard 描画 | 未実装 | ParticleEmitter は内部実装あり、汎用 Billboard なし |
| 草シェーダー | 未実装 | — |
| 風システム連携 | 未実装 | — |

---

## アーキテクチャ

```
│ Editor Layer                                                        │
│  InspectorTerrain                                                   │
│    ・TerrainDetailComponent のレイヤーリストを編集                  │
│    ・[Bake] ボタンでチャンクインスタンスデータを再生成              │
│    ・各レイヤーの density map・メッシュ・スケールを調整             │
               ↓ Reflect / SceneSerializer
│ Scene Layer                                                         │
│  TerrainDetailComponent（TerrainComponent と同一 GameObject）       │
│    ・layers: std::vector<DetailLayer>（レイヤー定義・永続化対象）   │
│    ・chunks: DetailChunkCache（ランタイムのインスタンス配列、非保存）│
               ↓ DetailRenderSystem が Scene をイテレート
│ System Layer                                                        │
│  DetailRenderSystem                                                 │
│    1. カメラ周辺のチャンクを有効距離でフィルタ                      │
│    2. フラスタムカリングでチャンクを間引く                          │
│    3. チャンクごとにレイヤー別の DrawInstanced を Submit            │
│    4. GrassLayer は専用 PSO + Geometry/Wind シェーダーへ投げる      │
```

---

## データ構造

### DetailLayer（永続化対象）

```cpp
enum class DetailLayerType : uint8_t {
    Mesh,       // 岩・花・低木など 3D メッシュ
    Billboard,  // カメラ向きクワッド（遠景フォールバック）
    Grass,      // プロシージャル草ブレード
};

struct DetailLayer {
    DetailLayerType type         = DetailLayerType::Mesh;

    // アセット参照
    std::string     meshPath;         // Mesh / Grass 時に使用
    std::string     densityMapPath;   // グレースケール: 白=最大密度・黒=なし
    std::string     texturePath;      // Billboard / Grass 時のアルベド

    // 配置
    float           density          = 1.0f;   // 密度倍率 (0〜10)
    float           minScale         = 0.8f;   // スケールのばらつき下限
    float           maxScale         = 1.2f;
    float           alignToNormal    = 0.0f;   // 0=垂直固定、1=法線に沿う
    bool            randomYRotation  = true;

    // 描画距離
    float           drawDistance     = 50.0f;  // これ以上は非表示
    float           fadeStartDist    = 40.0f;  // フェードアウト開始距離

    // Grass 専用
    float           bladeHeight      = 0.4f;
    float           bladeWidth       = 0.05f;
    float           bladeSegments    = 3;      // ブレードの分割数（頂点数）
    float           windStrength     = 1.0f;
    float           windFrequency    = 1.0f;
};
```

### DetailInstance（ランタイム・GPU 転送用）

```cpp
// Mesh / Billboard レイヤー用
struct DetailInstance {
    float posX, posY, posZ;  // ワールド座標
    float rotY;              // Y 回転（ラジアン）
    float scale;             // 均等スケール
};                           // 20 bytes/インスタンス

// Grass レイヤー用（風フェーズを追加）
struct GrassInstance {
    float posX, posY, posZ;
    float rotY;
    float scale;
    float windPhase;         // ブレードごとの位相オフセット
};                           // 24 bytes/インスタンス
```

### DetailChunkCache（ランタイム、非保存）

```cpp
struct DetailChunk {
    int chunkX, chunkZ;

    // レイヤー数 × インスタンス配列
    // チャンクのベイク時またはロード時に生成、以降は変化しない
    std::vector<std::vector<DetailInstance>> instancesPerLayer;

    // GPU インスタンスバッファ（レイヤーごとに 1 本）
    std::vector<ResourceHandle<IBuffer>>     instanceBuffers;

    bool isDirty = true;  // bake が必要なら true
};
```

---

## チャンク設計

Terrain を **16 m × 16 m** のチャンクに分割して管理する。

新規レイヤーの密度マップは全要素 `0` で初期化する。
密度マップが未作成または無効な場合も密度 `0` として扱い、初回 Bake では配置しない。

```
Terrain (256m x 256m) → 16x16 = 256 チャンク
各チャンク: 16m x 16m に含まれる density map ピクセルを読んでインスタンス生成
```

**チャンクのライフサイクル:**

1. **Bake（Editor 操作 or ロード時）**
   - density map をサンプリング、Poisson disk または格子+ランダムオフセットで配置点を生成
   - 各配置点で TerrainHeightMap から高さを取得して posY を確定
   - `DetailChunk::instancesPerLayer` に格納
   - GPU バッファに Upload

2. **毎フレーム**
   - カメラ中心から `drawDistance` 以内のチャンクを有効化
   - 有効チャンクに Frustum Culling を適用（AABB vs Camera Frustum）
   - 生き残ったチャンクのレイヤー別 DrawInstanced を Submit

---

## レンダリングパイプライン内での位置づけ

```
  1. ShadowPass
  2. GBuffer / Forward 不透明描画（Opaque Mesh）
  3. DetailPass（このシステム）
       3a. Mesh / Billboard レイヤー → 不透明 + Alpha Test で描画
       3b. Grass レイヤー            → 両面描画・専用 PSO
  4. Sky / Water / Trail / Particle などの半透明描画
  5. PostProcessSystem
```

草・岩は **Alpha Test（不透明パス）** で描画し、透明ソートを避ける。
Billboard の透過フチは Alpha-to-Coverage で対処する。

---

## シェーダー設計

### Mesh / Billboard 共通 (`Detail.hlsl`)

```hlsl
// インスタンスバッファから InstanceID で posXYZ / rotY / scale を取得
// → World 行列を実行時生成（行列バッファを持たず軽量に）

struct DetailInstance { float3 pos; float rotY; float scale; };
StructuredBuffer<DetailInstance> g_Instances : register(t0);

DetailVSOut VS(DetailVSIn v, uint id : SV_InstanceID)
{
    DetailInstance inst = g_Instances[id];
    float s = sin(inst.rotY), c = cos(inst.rotY);
    float3x3 rot = { c,0,s,  0,1,0,  -s,0,c };
    float3 world = mul(rot, v.pos * inst.scale) + inst.pos;
    ...
}
```

Billboard は VS 内でカメラ右/上ベクトルを使ってクワッドを展開（Spherical または Cylindrical）。

### Grass レイヤー (`DetailGrass.hlsl`)

```hlsl
// GrassInstance から ブレードを構成
// Bezier カーブで根元→先端を曲げ、風アニメーションを加える
// 両面ライティング: dot(N, L) のかわりに abs(dot(N, L)) で裏面も明るく

float windOffset = sin(time * windFreq + inst.windPhase) * windStrength;
// 上ほど揺れが大きくなるよう、頂点の高さで補間
float3 blade = BezierBlade(basePos, bladeHeight, bendAmount + windOffset);
```

**PSO 差異（GrassLayerPSO）:**
| 設定 | 値 |
|---|---|
| CullMode | NONE（両面描画） |
| BlendMode | Alpha Test（閾値 0.3） |
| DepthWrite | ON |

---

## 配置アルゴリズム（Bake 時）

```
for each chunk:
  for each layer:
    sampleStep = 1 / (density * maxDensityPerMeter2)
    for x in chunk: step sampleStep
      for z in chunk: step sampleStep
        d = densityMap.sample(x, z)
        if rand() > d: skip
        y = heightMap.sample(x, z)
        rotY   = randomYRotation ? rand() * 2π : 0
        scale  = lerp(minScale, maxScale, rand())
        phase  = rand() * 2π   // Grass のみ
        append DetailInstance(x, y, z, rotY, scale, [phase])
```

Poisson disk を使うとより均一な分布になるが、格子+ランダムオフセットで視覚的に十分。
まず後者を実装し、必要に応じて Poisson disk に置き換える。

---

## 風システム

DetailRenderSystem が毎フレームシェーダー定数に書き込む。

```cpp
struct DetailWindCB {
    float3 windDir;       // 正規化風向きベクトル (XZ 平面)
    float  windStrength;  // グローバル風速
    float  time;          // 累積時間（sin に渡す）
    float3 _pad;
};
```

フェーズ 1 では定数値、将来的に `VolumeComponent` や Weather 系と連携して動的に変化させる。

---

## LOD 戦略

| 距離 | Mesh レイヤー | Grass レイヤー |
|---|---|---|
| 〜20 m | フルメッシュ | プロシージャルブレード（フル分割） |
| 20〜40 m | LOD1 メッシュ | ブレード分割数を減らす |
| 40〜50 m | Billboard フォールバック | Billboard クワッドに切り替え |
| 50 m〜 | 非表示 | 非表示 |

フェーズ 1 では LOD 切り替えは実装せず、単純な `drawDistance` カットオフのみ。

---

## 永続化対象

`SceneSerializer` は `TerrainDetailComponent::layers` の各 `DetailLayer` フィールドを保存する。

| フィールド | 保存 |
|---|---|
| `type` | ✓ |
| `meshPath` / `densityMapPath` / `texturePath` | ✓ |
| `density` / `minScale` / `maxScale` | ✓ |
| `alignToNormal` / `randomYRotation` | ✓ |
| `drawDistance` / `fadeStartDist` | ✓ |
| `bladeHeight` / `bladeWidth` / `bladeSegments` | ✓（Grass のみ意味あり） |
| `windStrength` / `windFrequency` | ✓ |
| `chunks`（インスタンス配列）| **✗** ロード時に Bake で再生成する |
| `instanceBuffers`（GPU バッファ）| **✗** ランタイムリソース |

---

## 不足している機能・改善点

### 重要度：高（Phase 1 必須）

- GPU Instancing パスが未整備。`DrawInstanced` 呼び出し口と StructuredBuffer 更新 API を `IRenderer` に追加する
- density map サンプリングと Bake ロジック
- TerrainDetailComponent の `Reflect()` と Inspector 表示

### 重要度：中

- Grass レイヤー専用シェーダー（プロシージャルブレード・風）
- Alpha-to-Coverage による Billboard フチのアンチエイリアス
- チャンク Frustum Culling（AABB 判定）

### 重要度：低

- LOD 段階切り替え（距離別メッシュ / ブレード分割数）
- Poisson disk 配置アルゴリズム
- プレイヤー近傍での草インタラクション（踏み倒し）
- Cast shadow 対応（草の影は通常省略する）

---

## 実装フェーズ計画

| Phase | 内容 | 完了条件 |
|---|---|---|
| 1 | `IRenderer` に `DrawInstanced` + StructuredBuffer 更新 API を追加 | DrawInstanced が DX11 バックエンドで動作する |
| 2 | `TerrainDetailComponent` + `DetailLayer` 定義・Reflect・保存 / 読み込み | Inspector から レイヤー追加・編集できる |
| 3 | Bake ロジック（density map → インスタンス配列）+ Mesh/Billboard レイヤー描画 | 岩・花が Terrain 上に GPU Instancing で表示される |
| 4 | Grass レイヤー（プロシージャルブレード・風アニメーション） | 草が風に揺れて描画される |
| 5 | LOD / Frustum Culling / フェードアウト | 遠距離で Billboard 切り替えと非表示 |

---

## ファイル配置

```
Projects/Engine/
  include/Engine/
    Scene/
      Components/
        TerrainDetailComponent.hpp   ← DetailLayer 定義 + Reflect
      Systems/
        DetailRenderSystem.hpp       ← DetailChunk / DrawInstanced 呼び出し
  src/Scene/
    Components/
      TerrainDetailComponent.cpp     ← Bake ロジック
    Systems/
      DetailRenderSystem.cpp
    SceneSerializer.cpp              ← TerrainDetailComponent の保存 / 読み込み

Assets/Shaders/
  Detail/
    Detail.hlsl                      ← Mesh / Billboard 共通 VS + PS
    DetailGrass.hlsl                 ← Grass 専用 VS + PS（両面・風）

Projects/Editor/
  src/Panels/Inspector/
    InspectorTerrain.cpp             ← TerrainDetailComponent レイヤー編集 UI

Docs/System/Detail/
  overview.md                        ← このファイル
```

---

## 隣接ドキュメント

- [../Environment/overview.md](../Environment/overview.md) — Terrain / Water 環境システム全体
- [../Rendering/overview.md](../Rendering/overview.md) — RenderSystem / RenderPass の設計
- [../Particle/overview.md](../Particle/overview.md) — GPU Instancing パス実装時に参照する DrawCall 構造
