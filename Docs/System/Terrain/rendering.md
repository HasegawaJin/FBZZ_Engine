# Terrain System 設計 — 描画パイプライン

---

## データ構造

### TerrainLayer

テクスチャ 1 レイヤーの定義。最大 4 つまで `TerrainComponent.layers` に持てる。

```cpp
struct TerrainLayer {
    // AssetManager でロードするパス（空文字 = 未設定）
    std::string diffusePath;     // "assets/terrain/grass_d.png"
    std::string normalPath;      // "" = フラット法線（1,0,1）でパディング

    float tilingX       = 8.0f;  // UV タイリング係数（X 方向）
    float tilingZ       = 8.0f;  // UV タイリング係数（Z 方向）
    float normalStrength = 1.0f; // 法線マップの強度スケール
};
```

---

### TerrainComponent

```cpp
struct TerrainComponent {
    // --- ハイトマップ (CPU) ---
    // row-major: index = z * columns + x
    // 値域 [0, 1] → ワールド高さ = value * maxHeight
    std::vector<float>   heightData;
    uint32_t             columns   = 129;   // X 方向の頂点数（2^n + 1 推奨）
    uint32_t             rows      = 129;   // Z 方向の頂点数（2^n + 1 推奨）
    float                cellSize  = 1.0f;  // 1 マスのワールド単位幅
    float                maxHeight = 30.0f; // heightData=1 のときのワールド高さ

    // --- スプラットマップ (CPU, RGBA8 unorm) ---
    // index = (z * columns + x) * 4 + channel (0=R 1=G 2=B 3=A)
    // R=layer0, G=layer1, B=layer2, A=layer3, 各チャンネルは [0, 255]
    // 4 チャンネルの合計が 255 になるよう正規化する（シェーダー側で除算）
    std::vector<uint8_t> splatData;

    // --- テクスチャレイヤー (最大 4) ---
    std::vector<TerrainLayer> layers; // .size() <= 4

    // --- チャンク設定 ---
    // 地形を chunkSize × chunkSize マスのブロックに分割して描画
    // チャンクあたりの頂点数 = (chunkSize + 1)^2
    uint32_t chunkSize = 32;

    bool enabled      = true;
    bool heightDirty  = false; // true → TerrainRenderSystem がメッシュを再構築
    bool splatDirty   = false; // true → TerrainRenderSystem がテクスチャを再アップロード

    // Reflection (Inspector / Serializer 対応)
    const char* GetTypeName() const { return "Terrain"; }
    void Reflect(IReflector& r) {
        r.Field("enabled",    enabled);
        r.Field("columns",    columns);
        r.Field("rows",       rows);
        r.Field("cellSize",   cellSize);
        r.Field("maxHeight",  maxHeight);
        r.Field("chunkSize",  chunkSize);
        r.Field("layers",     layers);
        // heightData / splatData はバイナリシリアライズで別途扱う
    }

    // ワールド座標から高さをバイリニア補間で取得
    // (x, z) はテレインローカル座標 (Transform 適用前)
    float GetHeightAt(float localX, float localZ) const;

    // ワールド座標から補間法線を取得
    fbzz::math::Vector3 GetNormalAt(float localX, float localZ) const;
};
```

#### 頂点数・チャンク数の計算

```
chunkCountX = ceil((columns - 1) / chunkSize)
chunkCountZ = ceil((rows    - 1) / chunkSize)
totalChunks = chunkCountX * chunkCountZ

// 境界チャンクは chunkSize より小さい場合がある → 実装で clamp する
```

`2^n + 1` を推奨する理由: チャンク境界が整合し、将来的な LOD 二分割が容易になる。

---

## 頂点フォーマット

```cpp
// TerrainRenderSystem.cpp 内部で定義（外部公開不要）
struct TerrainVertex {
    fbzz::math::Vector3 position; // ローカル座標
    fbzz::math::Vector3 normal;   // 有限差分で計算した面法線
    fbzz::math::Vector2 uv;       // 地形全体にまたがる UV [0,1]
};
```

GPU 側の頂点レイアウトは `ResourceManager::CreateBuffer` に渡す `BufferDesc` で記述する。

---

## 法線計算アルゴリズム

ハイトマップの有限差分から法線を求める。境界頂点はクランプサンプリングで対応する。

```
// サンプリング関数（境界外は境界値をクランプ）
h(x, z) = heightData[clamp(z, 0, rows-1) * columns + clamp(x, 0, columns-1)] * maxHeight

// 中心差分
dh/dx = (h(x+1, z) - h(x-1, z)) / (2 * cellSize)
dh/dz = (h(x, z+1) - h(x, z-1)) / (2 * cellSize)

// 法線（Y 上向き座標系）
normal = normalize( Vector3(-dh/dx, 1, -dh/dz) )
```

実装ではワールド高さに変換してから差分を取る（`cellSize` が単位）。

---

## TerrainRenderSystem

### シグネチャ

```cpp
// TerrainRenderSystem.hpp
namespace fbzz::scene {

void TerrainRenderSystem(
    Scene&            scene,
    IRenderer&        renderer,
    ResourceManager&  resources,
    const renderer::Camera& camera,
    const renderer::RenderSettings* settings = nullptr
);

} // namespace fbzz::scene
```

### システム内部状態

`TerrainRenderSystem` は複数フレームにわたってチャンクバッファを保持するため、
静的または外部から渡すキャッシュ構造が必要。
シングルスレッド制約（[conventions/threading.md](../../conventions/threading.md)）のもとでは `static` ローカルが最も単純。

```cpp
// TerrainRenderSystem.cpp 内部
struct TerrainChunk {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    uint32_t                  indexCount;
    fbzz::math::Vector3       aabbMin;
    fbzz::math::Vector3       aabbMax;
};

struct TerrainChunkKey {
    EntityID entityId;
    uint32_t chunkX;
    uint32_t chunkZ;
    bool operator==(const TerrainChunkKey&) const = default;
};
// std::unordered_map<TerrainChunkKey, TerrainChunk> でキャッシュ管理
```

### 処理フロー

```
1. SceneView<TerrainComponent, Transform> でシーンを走査
2. TerrainComponent.enabled == false → スキップ
3. heightDirty == true
       → 全チャンクのメッシュを再構築（CPU で頂点・インデックス生成 → GPU バッファ更新）
       → heightDirty = false
4. splatDirty == true
       → スプラットマップテクスチャを再アップロード
       → splatDirty = false
5. カメラフラスタムで各チャンクの AABB をカリング
6. 生き残ったチャンクを DrawCall に積んで Submit
```

### チャンクメッシュ生成

```cpp
// チャンク (cx, cz) の頂点・インデックスを生成するスケッチ
void BuildChunk(const TerrainComponent& terrain,
                uint32_t cx, uint32_t cz,
                std::vector<TerrainVertex>& outVerts,
                std::vector<uint32_t>&      outIndices)
{
    // チャンクが担当する頂点範囲
    uint32_t x0 = cx * terrain.chunkSize;
    uint32_t z0 = cz * terrain.chunkSize;
    uint32_t x1 = std::min(x0 + terrain.chunkSize, terrain.columns - 1);
    uint32_t z1 = std::min(z0 + terrain.chunkSize, terrain.rows    - 1);

    for (uint32_t z = z0; z <= z1; ++z) {
        for (uint32_t x = x0; x <= x1; ++x) {
            float h = terrain.heightData[z * terrain.columns + x] * terrain.maxHeight;
            TerrainVertex v;
            v.position = { x * terrain.cellSize, h, z * terrain.cellSize };
            v.normal   = ComputeNormal(terrain, x, z);
            v.uv       = { float(x) / (terrain.columns - 1),
                           float(z) / (terrain.rows    - 1) };
            outVerts.push_back(v);
        }
    }

    // クアッド → 2 三角形（時計回り）
    uint32_t w = x1 - x0 + 1;
    for (uint32_t z = 0; z < (z1 - z0); ++z) {
        for (uint32_t x = 0; x < (x1 - x0); ++x) {
            uint32_t i00 = z * w + x;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + w;
            uint32_t i11 = i01 + 1;
            outIndices.insert(outIndices.end(), { i00, i01, i10, i10, i01, i11 });
        }
    }
}
```

---

## シェーダー設計

ファイル: `Assets/Shaders/Terrain.hlsl`

### 定数バッファ

```hlsl
// PerObject (地形ごとに更新)
cbuffer TerrainCB : register(b1) {
    float4x4 g_worldMatrix;
    float4x4 g_wvpMatrix;
    float4   g_layerTiling[4]; // xy = tilingX/Z per layer
    float4   g_layerNormalStrength; // x=layer0, y=1, z=2, w=3
};
```

### テクスチャスロット

```hlsl
Texture2D g_splatmap       : register(t0); // RGBA スプラットマップ
Texture2D g_diffuse[4]     : register(t1); // t1〜t4: 各レイヤー拡散光
Texture2D g_normal[4]      : register(t5); // t5〜t8: 各レイヤー法線マップ

SamplerState g_sampler     : register(s0); // Wrap, Anisotropic x4
SamplerState g_samplerClamp: register(s1); // Clamp (スプラットマップ用)
```

### ピクセルシェーダーのブレンドロジック

```hlsl
float4 main(VSOutput input) : SV_Target {
    // スプラットマップからレイヤーウェイトを取得
    float4 splat = g_splatmap.Sample(g_samplerClamp, input.uv);
    // 合計を 1 に正規化（ペイントツールが保証するが念のため）
    float wsum = splat.r + splat.g + splat.b + splat.a;
    if (wsum > 0.001f) splat /= wsum;

    float3 albedo = float3(0, 0, 0);
    float3 normal = float3(0, 0, 0);

    [unroll]
    for (int i = 0; i < 4; ++i) {
        float2 tiledUV = input.uv * g_layerTiling[i].xy;
        float3 d = g_diffuse[i].Sample(g_sampler, tiledUV).rgb;
        float3 n = g_normal[i].Sample(g_sampler, tiledUV).rgb;
        albedo += d * splat[i];
        normal += n * splat[i];
    }

    // 法線マップを [0,1] → [-1,1] に変換し、頂点法線で変換（TBN）
    // ...（Phong / PBR ライティング計算）
    return float4(albedo, 1.0f);
}
```

未設定レイヤー（layers.size() < 4）には白テクスチャ + フラット法線をバインドする。
シェーダー側でレイヤー数を動的分岐せず、常に 4 レイヤー固定でブレンドする。
WHY: `[unroll]` + テクスチャフェッチの GPU パイプライン効率が固定ループの方が良い。

---

## GetHeightAt / GetNormalAt 実装スケッチ

```cpp
float TerrainComponent::GetHeightAt(float localX, float localZ) const {
    // グリッド座標に変換
    float gx = localX / cellSize;
    float gz = localZ / cellSize;

    // 整数部・小数部
    int   x0  = std::clamp(int(gx),     0, int(columns) - 2);
    int   z0  = std::clamp(int(gz),     0, int(rows)    - 2);
    float fx  = gx - x0;
    float fz  = gz - z0;

    // バイリニア補間
    auto h = [&](int x, int z) {
        return heightData[z * columns + x] * maxHeight;
    };
    float h00 = h(x0,   z0);
    float h10 = h(x0+1, z0);
    float h01 = h(x0,   z0+1);
    float h11 = h(x0+1, z0+1);
    return h00*(1-fx)*(1-fz) + h10*fx*(1-fz)
         + h01*(1-fx)*fz     + h11*fx*fz;
}
```

`GetNormalAt` は同様にバイリニア補間した法線を返す（法線を格納した別配列を持つか、その場で有限差分で計算する）。

---

## LOD（将来拡張 / Phase 3 以降）

Phase 3 では距離に応じてチャンクの解像度を落とす。

| LOD レベル | 距離 | インデックスステップ |
|---|---|---|
| 0 | 〜 50m | 1（フル解像度） |
| 1 | 50〜150m | 2（1/4 ポリゴン） |
| 2 | 150m〜  | 4（1/16 ポリゴン） |

各 LOD のインデックスバッファを事前生成し、距離に応じて切り替える。
チャンク境界の T 字接合（T-junction）は境界頂点の縫合（stitching）で解決する。

---

## エラーハンドリング方針

[conventions/error_handling.md](../../conventions/error_handling.md) に準拠する。

| 状況 | 対応 |
|---|---|
| `heightData` のサイズが `columns * rows` と不一致 | `assert`（設定ミスはバグ扱い） |
| `layers.size() > 4` | `assert` |
| チャンクバッファ生成失敗（ResourceManager） | `assert`（GPU 初期化失敗は回復不能） |
| `GetHeightAt` で範囲外座標 | クランプして継続 |
| スプラットマップが空 (`splatData.empty()`) | 全ウェイトを layer0=255, 他=0 として扱う |
