# Water System 設計 — 描画パイプライン

---

## データ構造

### GerstnerWave

水面の複雑な波形を Gerstner 波の重ね合わせで表現するためのパラメータ 1 波分。
最大 4 波を `WaterComponent.waves` に格納し、頂点シェーダーで合算する。

```
Gerstner 波方程式（deep-water 近似）:
  P(x,z,t) に対して変位 d を加算する

  wave number:      k = 2π / λ
  角周波数:         ω = √(g·k)  （g = 9.8 m/s²、深水波の分散関係）
  位相:             φ = k · dot(D̂, P_xz) − ω · t

  変位:
    Δx = Q·A·D̂.x · cos(φ)
    Δy =   A      · sin(φ)
    Δz = Q·A·D̂.z · cos(φ)

  Q（steepness）= 0 → 正弦波（単純な上下動）
  Q（steepness）= 1 → 最大 Gerstner（波頭が鋭く崩れる直前）
```

```cpp
namespace fbzz::scene {

// 波 1 本分のパラメータ。振幅 0 で事実上オフ扱い。
struct GerstnerWave {
    fbzz::math::Vector2 direction  = {1.0f, 0.0f}; // 進行方向（正規化不要、シェーダー側で normalize）
    float               amplitude  = 0.3f;  // 波高 A [m]
    float               wavelength = 10.0f; // 波長 λ [m]
    float               steepness  = 0.5f;  // 急峻度 Q [0, 1]
                                            // 0 = 正弦波、1 = Gerstner 最大（これ以上は頂点が交差）
};

} // namespace fbzz::scene
```

---

### WaterComponent

```cpp
namespace fbzz::scene {

struct WaterComponent {

    // ---- ジオメトリ ----
    // 水面は XZ 平面の均等グリッド。CPU 側は平坦（高さ 0）、
    // Gerstner 波の変位は頂点シェーダー側で加算する。
    uint32_t resolutionX = 64;    // X 方向のグリッド分割数（頂点数 = resolutionX + 1）
    uint32_t resolutionZ = 64;    // Z 方向のグリッド分割数
    float    extentX     = 100.0f; // X 方向の幅 [m]
    float    extentZ     = 100.0f; // Z 方向の幅 [m]
    // WHY: Transform.position.y を水位基準として使うため、
    //      WaterComponent 自体は Y=0 のローカル平面を出力する。

    // ---- 水の色 ----
    fbzz::math::Vector3 shallowColor = {0.20f, 0.60f, 0.70f}; // 浅瀬の水色
    fbzz::math::Vector3 deepColor    = {0.00f, 0.10f, 0.30f}; // 深部の水色
    float shallowDepth = 0.5f; // この深さ以下を「浅瀬」と判定する閾値 [m]
    float deepDepth    = 5.0f; // この深さ以上を「深部」として deepColor を使う [m]
    float opacity      = 0.85f; // 基本不透明度 [0, 1]（深度でさらに変調する）

    // ---- Fresnel 反射 ----
    // 視線と水面法線の角度が大きいほど強く反射する（水面を斜めから見ると鏡に近い）。
    // Schlick 近似: F = fresnelBias + (1 - fresnelBias) * (1 - dot(N, V))^fresnelPower
    float reflectivity  = 0.5f;  // 最大反射率へのスケール
    float fresnelBias   = 0.02f; // 真上（垂直入射）での最小反射率（ガラスは約 0.04）
    float fresnelPower  = 5.0f;  // べき乗数（大きいほど角度依存が急峻）

    // ---- 法線マップ（波アニメーション） ----
    // 2 枚の法線マップを異なる方向にスクロールさせてブレンドし、
    // 単調に見える繰り返しを低減する。
    std::string           normalMap1Path;
    std::string           normalMap2Path;
    float                 normalMap1Tiling = 4.0f;
    float                 normalMap2Tiling = 6.0f;
    float                 normalStrength   = 1.0f;
    fbzz::math::Vector2   normalMap1Scroll = { 0.02f,  0.01f}; // UV/秒
    fbzz::math::Vector2   normalMap2Scroll = {-0.01f,  0.02f};

    // ---- Gerstner 波（頂点変位） ----
    // waves[i].amplitude == 0 の波は事実上スキップされる（シェーダー側で加算が 0）。
    std::array<GerstnerWave, 4> waves;
    bool enableGerstnerWaves = true;

    // ---- 岸辺泡 ----
    // TerrainComponent::GetHeightAt() で地形高さを取得し、
    // 水面高さとの差が foamThreshold 以下の領域を泡として表示する。
    float       foamThreshold = 0.3f;  // この高さ差（水面 - 地形）以下で泡が最大 [m]
    float       foamFade      = 0.5f;  // threshold 前後のフェード幅 [m]
    float       foamStrength  = 1.0f;  // 泡の強度スケール [0, 1]
    std::string foamTexPath;           // 泡テクスチャパス（白・高コントラスト推奨）
    float       foamTiling    = 8.0f;  // 泡テクスチャのタイリング係数

    // ---- 環境反射 ----
    std::string envCubemapPath; // 反射用キューブマップ。空文字 = SkyRenderer のキューブマップを流用（未実装時は黒）

    // ---- 状態フラグ ----
    bool enabled   = true;
    bool meshDirty = true; // true → WaterRenderSystem がグリッドメッシュを再構築
    bool foamDirty = true; // true → 岸辺泡マスクを再生成（TerrainComponent 参照）
    bool texDirty  = true; // true → 法線マップ・泡テクスチャを再ロード

    const char* GetTypeName() const { return "Water"; }
    void Reflect(IReflector& r) {
        r.Field("enabled",            enabled);
        r.Field("resolutionX",        resolutionX);
        r.Field("resolutionZ",        resolutionZ);
        r.Field("extentX",            extentX);
        r.Field("extentZ",            extentZ);
        r.Field("shallowColor",       shallowColor);
        r.Field("deepColor",          deepColor);
        r.Field("shallowDepth",       shallowDepth);
        r.Field("deepDepth",          deepDepth);
        r.Field("opacity",            opacity);
        r.Field("reflectivity",       reflectivity);
        r.Field("fresnelBias",        fresnelBias);
        r.Field("fresnelPower",       fresnelPower);
        r.Field("normalMap1Path",     normalMap1Path);
        r.Field("normalMap2Path",     normalMap2Path);
        r.Field("normalMap1Tiling",   normalMap1Tiling);
        r.Field("normalMap2Tiling",   normalMap2Tiling);
        r.Field("normalStrength",     normalStrength);
        r.Field("normalMap1Scroll",   normalMap1Scroll);
        r.Field("normalMap2Scroll",   normalMap2Scroll);
        r.Field("enableGerstnerWaves",enableGerstnerWaves);
        r.Field("waves",              waves);
        r.Field("foamThreshold",      foamThreshold);
        r.Field("foamFade",           foamFade);
        r.Field("foamStrength",       foamStrength);
        r.Field("foamTexPath",        foamTexPath);
        r.Field("foamTiling",         foamTiling);
        r.Field("envCubemapPath",     envCubemapPath);
    }
};

} // namespace fbzz::scene
```

---

## 頂点フォーマット

```cpp
// WaterRenderSystem.cpp 内部定義（外部公開不要）
struct WaterVertex {
    fbzz::math::Vector3 position; // XZ 平面グリッド、Y=0（波変位はシェーダー側）
    fbzz::math::Vector2 uv;       // [0, 1] × [0, 1]（水面全体にまたがる UV）
};
// stride = 20 bytes
```

法線・接線は頂点バッファに持たない。Gerstner 波の TBN は頂点シェーダーが解析的に導出する。

---

## WaterRenderSystem

### シグネチャ

```cpp
// WaterRenderSystem.hpp
namespace fbzz::scene {

void WaterRenderSystem(
    Scene&                          scene,
    renderer::IRenderer&            renderer,
    renderer::ResourceManager&      resources,
    const renderer::Camera&         camera,
    renderer::RenderTargetHandle    outputRT,
    float                           elapsedTime,      // Gerstner 波位相 g_time に渡す
    const renderer::RenderSettings* settings = nullptr
);

} // namespace fbzz::scene
```

### システム内部状態

シングルスレッド制約のもと `static` ローカルでキャッシュを保持する。

```cpp
// WaterRenderSystem.cpp 内部

struct WaterMesh {
    ResourceHandle<BufferTag> vertexBuffer;
    ResourceHandle<BufferTag> indexBuffer;
    uint32_t                  indexCount;
};

struct WaterTextures {
    ResourceHandle<TextureTag> normalMap1;  // 波法線マップ 1
    ResourceHandle<TextureTag> normalMap2;  // 波法線マップ 2
    ResourceHandle<TextureTag> foamTex;     // 泡テクスチャ
    ResourceHandle<TextureTag> foamMask;    // 岸辺泡マスク（CPU 生成）
    ResourceHandle<TextureTag> envCube;     // 環境キューブマップ
};

// EntityID → WaterMesh
static std::unordered_map<uint32_t, WaterMesh>     s_meshCache;
// EntityID → WaterTextures
static std::unordered_map<uint32_t, WaterTextures> s_texCache;
```

### 処理フロー

```
1. SceneView<WaterComponent, Transform> でシーンを走査
2. WaterComponent.enabled == false → スキップ
3. meshDirty == true
       → BuildWaterMesh() でグリッド頂点・インデックスを生成（CPU → GPU）
       → meshDirty = false
4. foamDirty == true
       → BuildFoamMask() で泡マスクテクスチャを生成（次節で詳述）
       → foamDirty = false
5. texDirty == true
       → 法線マップ・泡テクスチャ・キューブマップを ResourceManager::LoadTexture() でロード
       → texDirty = false
6. カメラフラスタムで水面 AABB をカリング（省略可能だが大きな水域では効果あり）
7. WaterCB を更新して DrawCall 発行（ALPHA_BLEND, DEPTH_READ）
```

### グリッドメッシュ生成

```cpp
// CPU 側は平坦な XZ グリッド。波変位は GPU（頂点シェーダー）で行う。
void BuildWaterMesh(
    const WaterComponent&       water,
    std::vector<WaterVertex>&   outVerts,
    std::vector<uint32_t>&      outIndices)
{
    uint32_t nx = water.resolutionX;
    uint32_t nz = water.resolutionZ;
    float dx = water.extentX / nx;
    float dz = water.extentZ / nz;
    // 原点を中心に配置（左前 = (-extentX/2, 0, -extentZ/2)）
    float ox = -water.extentX * 0.5f;
    float oz = -water.extentZ * 0.5f;

    for (uint32_t iz = 0; iz <= nz; ++iz) {
        for (uint32_t ix = 0; ix <= nx; ++ix) {
            WaterVertex v;
            v.position = {ox + ix * dx, 0.0f, oz + iz * dz};
            v.uv       = {float(ix) / nx, float(iz) / nz};
            outVerts.push_back(v);
        }
    }

    uint32_t w = nx + 1;
    for (uint32_t iz = 0; iz < nz; ++iz) {
        for (uint32_t ix = 0; ix < nx; ++ix) {
            uint32_t i00 = iz * w + ix;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = i00 + w;
            uint32_t i11 = i01 + 1;
            outIndices.insert(outIndices.end(), {i00, i01, i10, i10, i01, i11});
        }
    }
}
```

---

## 岸辺泡マスク生成（TerrainSystem 統合）

`foamDirty == true` のとき WaterRenderSystem が CPU 上で泡マスクテクスチャを生成する。
泡マスクの解像度は水面グリッドと同じ `(resolutionX+1) × (resolutionZ+1)` を使う。

### アルゴリズム

```
1. SceneView<TerrainComponent, Transform> でシーン内の TerrainComponent を探す
   （複数ある場合はエンティティ ID が最小のものを優先。将来は最近傍で選択）

2. 泡マスクテクスチャを (resolutionX+1)×(resolutionZ+1) の R8 フォーマットで用意する

3. グリッド各頂点 (ix, iz) について:
   a. 水面ワールド座標を計算
        worldX = waterTransform.position.x + (−extentX/2 + ix * dx)
        worldZ = waterTransform.position.z + (−extentZ/2 + iz * dz)
        waterY = waterTransform.position.y

   b. TerrainComponent のローカル座標に変換
        localX = worldX - terrainTransform.position.x
        localZ = worldZ - terrainTransform.position.z

   c. 地形高さを取得
        terrainY = terrain.GetHeightAt(localX, localZ)
                   + terrainTransform.position.y

   d. 水深差 = waterY − terrainY
      （正 = 水面が地形より高い = 水の中、負 = 陸地）

   e. 泡強度 = smoothstep(foamThreshold + foamFade,
                          foamThreshold - foamFade,
                          heightDiff)
      WHY: smoothstep の引数を逆順にすることで、
           heightDiff が小さいほど（地形が水面に近いほど）強度が 1 に近くなる。

   f. マスクテクスチャの R チャンネルに foamWeight * 255 を書き込む

4. ResourceManager::CreateTexture() / Update() でテクスチャを GPU にアップロード
```

```cpp
// smoothstep ヘルパー（CPU 側）
static float SmoothStep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 泡強度の計算（heightDiff = waterLevel - terrainHeight）
static float ComputeFoamWeight(float heightDiff, float threshold, float fade) {
    // heightDiff が threshold より小さいほど泡が強い
    return SmoothStep(threshold + fade, threshold - fade, heightDiff);
}
```

---

## シェーダー設計

ファイル: `Assets/Shaders/Water/Water.hlsl`

### 定数バッファ

既存スロットと互換性を保つ。b0（Camera）と b3（Light）は Terrain.hlsl と同じ共通バッファを使う。

```hlsl
// b1: 水面ごとに更新（WaterRenderSystem が毎フレーム Upload）
cbuffer WaterCB : register(b1)
{
    float4x4 g_worldMatrix;     // Transform から生成した World 行列
    float4x4 g_wvpMatrix;       // World × View × Projection 行列

    // 水の色
    float3 g_shallowColor;  float _pad0;
    float3 g_deepColor;     float _pad1;
    float  g_shallowDepth;
    float  g_deepDepth;
    float  g_opacity;
    float  _pad2;

    // Fresnel
    float  g_reflectivity;
    float  g_fresnelBias;
    float  g_fresnelPower;
    float  _pad3;

    // 法線マップスクロール
    float2 g_normalMap1Scroll;  // UV/秒 → g_time と積算
    float2 g_normalMap2Scroll;
    float  g_normalMap1Tiling;
    float  g_normalMap2Tiling;
    float  g_normalStrength;
    float  g_time;              // エンジンからの累積時間 [s]

    // 泡
    float  g_foamThreshold;
    float  g_foamFade;
    float  g_foamStrength;
    float  g_foamTiling;

    // Gerstner 波（4 波分）
    // waveDir[i].xy = 進行方向, .z = steepness Q, .w = unused
    float4 g_waveDir[4];
    // waveParams[i].x = amplitude A [m]
    //              .y = wavelength λ [m]
    //              .z = angular frequency ω = sqrt(9.8 * 2π/λ)
    //              .w = wave number k = 2π/λ
    float4 g_waveParams[4];
};
```

### テクスチャスロット

TerrainRenderSystem（t0〜t12）とは異なるパスで発行するため、スロット番号の衝突はない。

```hlsl
Texture2D   g_normalMap1 : register(t0); // 波法線マップ 1
Texture2D   g_normalMap2 : register(t1); // 波法線マップ 2
Texture2D   g_foamTex    : register(t2); // 泡テクスチャ（白地に高コントラスト模様）
Texture2D   g_foamMask   : register(t3); // 岸辺泡マスク（CPU 生成 R8）
TextureCube g_envCube    : register(t4); // 環境キューブマップ（反射）
Texture2D   g_sceneDepth : register(t5); // シーン深度バッファ（深度フェード用）

SamplerState g_sampler      : register(s0); // WRAP, Anisotropic x4
SamplerState g_samplerClamp : register(s1); // CLAMP, Linear（スクリーン座標サンプリング用）
SamplerState g_samplerCube  : register(s2); // WRAP, Linear（キューブマップ用）
```

### 頂点シェーダー入出力

```hlsl
struct WaterVSInput {
    float3 position : POSITION;
    float2 uv       : TEXCOORD0;
};

struct WaterPSInput {
    float4 svPos    : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 normal   : TEXCOORD2;  // Gerstner 波から導出したワールド法線
    float3 tangent  : TEXCOORD3;  // 同じく接線ベクトル
    float3 binormal : TEXCOORD4;  // 同じく従法線ベクトル
    float4 screenPos: TEXCOORD5;  // スクリーン座標（深度サンプリング用）
};
```

### 頂点シェーダー — Gerstner 波変位

```hlsl
// 1 波分の変位と TBN 更新量を計算する
float3 GerstnerWaveDisplace(
    float4 dir,     // g_waveDir[i]:    xy=direction, z=steepness Q
    float4 params,  // g_waveParams[i]: x=A, y=λ, z=ω, w=k
    float3 pos,     // XZ 平面上の現在位置
    float  t,       // 累積時間
    inout float3 tangent,
    inout float3 binormal)
{
    float2 D    = normalize(dir.xy);
    float  Q    = dir.z;
    float  A    = params.x;
    float  k    = params.w;
    float  omega = params.z;
    float  phi  = k * dot(D, pos.xz) - omega * t;

    // TBN への加算分（∂P/∂x, ∂P/∂z の偏微分から導出）
    tangent.x  -= Q * D.x * D.x * k * A * sin(phi);
    tangent.y  += D.x * k * A * cos(phi);
    tangent.z  -= Q * D.x * D.y * k * A * sin(phi);
    binormal.x -= Q * D.x * D.y * k * A * sin(phi);
    binormal.y += D.y * k * A * cos(phi);
    binormal.z -= Q * D.y * D.y * k * A * sin(phi);

    return float3(
        Q * A * D.x * cos(phi),
        A * sin(phi),
        Q * A * D.y * cos(phi)
    );
}

WaterPSInput VSMain(WaterVSInput input)
{
    float3 worldPos = mul(float4(input.position, 1.0f), g_worldMatrix).xyz;
    float3 tangent  = float3(1, 0, 0);
    float3 binormal = float3(0, 0, 1);
    float3 disp     = float3(0, 0, 0);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        if (g_waveParams[i].x > 0.0001f) // amplitude == 0 はスキップ
            disp += GerstnerWaveDisplace(g_waveDir[i], g_waveParams[i], worldPos, g_time, tangent, binormal);
    }

    worldPos += disp;
    tangent   = normalize(tangent);
    binormal  = normalize(binormal);
    float3 normal = normalize(cross(tangent, binormal));
    // WHY: Gerstner 波の TBN は解析微分で求まるため、
    //      CPU 頂点バッファに法線を持つ必要がない。

    WaterPSInput o;
    o.svPos     = mul(float4(worldPos, 1.0f), g_wvpMatrix);  // 既に world 済みなので View×Proj のみ
    // ※ 実装上は g_wvpMatrix = View * Projection としてシステム側から渡す
    o.worldPos  = worldPos;
    o.uv        = input.uv;
    o.normal    = normal;
    o.tangent   = tangent;
    o.binormal  = binormal;
    o.screenPos = o.svPos; // Perspective-correct UV はピクセルシェーダーで除算
    return o;
}
```

### ピクセルシェーダー — 水面シェーディング

```hlsl
float4 PSMain(WaterPSInput input) : SV_Target
{
    // ── 1. 法線マップ（デュアルスクロール） ──
    float2 uv1 = input.uv * g_normalMap1Tiling + g_normalMap1Scroll * g_time;
    float2 uv2 = input.uv * g_normalMap2Tiling + g_normalMap2Scroll * g_time;
    float3 n1 = g_normalMap1.Sample(g_sampler, uv1).rgb * 2.0f - 1.0f;
    float3 n2 = g_normalMap2.Sample(g_sampler, uv2).rgb * 2.0f - 1.0f;
    // Reoriented Normal Mapping（2 法線の非線形ブレンド）
    // WHY: 単純な加算平均より、接線空間での向きが保存される
    float3 blendedNormal = normalize(float3(n1.xy + n2.xy, n1.z * n2.z));
    blendedNormal = normalize(lerp(float3(0, 0, 1), blendedNormal, g_normalStrength));

    // 接線空間 → ワールド空間
    float3x3 TBN = float3x3(input.tangent, input.binormal, input.normal);
    float3 N = normalize(mul(blendedNormal, TBN));

    // ── 2. Fresnel 反射 ──
    float3 V     = normalize(g_cameraPos - input.worldPos);
    float  NdotV = saturate(dot(N, V));
    float  fresnel = g_fresnelBias + (1.0f - g_fresnelBias)
                   * pow(1.0f - NdotV, g_fresnelPower);
    fresnel *= g_reflectivity;

    // ── 3. 環境キューブマップ反射 ──
    float3 R            = reflect(-V, N);
    float3 reflectColor = g_envCube.Sample(g_samplerCube, R).rgb;

    // ── 4. 深度ベース水色グラデーション ──
    // スクリーン座標を [0,1] テクスチャ UV に変換してシーン深度をサンプリング
    float2 screenUV = input.screenPos.xy / input.screenPos.w * float2(0.5f, -0.5f) + 0.5f;
    float rawSceneDepth = g_sceneDepth.Sample(g_samplerClamp, screenUV).r;

    // NDC 深度 → 線形深度（カメラ空間 Z）に変換
    // WHY: 非線形な NDC 深度では浅瀬の判定精度が低下するため線形化する
    float linearSceneDepth = (g_near * g_far)
                           / (g_far - rawSceneDepth * (g_far - g_near));
    float linearSurfDepth  = input.svPos.w; // パース除算後の w = カメラ空間 Z
    float waterDepth = max(0.0f, linearSceneDepth - linearSurfDepth);

    float depthFactor = saturate(waterDepth / g_deepDepth);
    float3 waterColor = lerp(g_shallowColor, g_deepColor, depthFactor);

    // ── 5. 岸辺泡 ──
    float foamMaskVal = g_foamMask.Sample(g_samplerClamp, input.uv).r;
    float foamTexVal  = g_foamTex.Sample(g_sampler, input.uv * g_foamTiling).r;
    float foam        = foamMaskVal * foamTexVal * g_foamStrength;

    // ── 6. 波面ハイライト（Blinn-Phong 鏡面反射） ──
    float3 L       = normalize(-g_lightDir);
    float3 H       = normalize(L + V);
    float specular = pow(saturate(dot(N, H)), 128.0f) * g_lightIntensity;

    // ── 7. 最終合成 ──
    // Fresnel でリフラクション色と反射色をブレンド
    float3 color = lerp(waterColor, reflectColor, fresnel);
    // 鏡面ハイライト加算
    color += float3(1.0f, 1.0f, 1.0f) * specular * 0.4f;
    // 泡を白としてアルファブレンド
    color = lerp(color, float3(1.0f, 1.0f, 1.0f), saturate(foam));

    // 透明度: 深部ほど不透明、浅瀬ほど透ける
    float alpha = g_opacity * lerp(0.4f, 1.0f, depthFactor);

    return float4(color, alpha);
}
```

---

## DrawCall 発行仕様

WaterRenderSystem が生成する DrawCall のパラメータ。

| フィールド | 値 |
|---|---|
| `shader` | `Water/Water.vs.cso` + `Water/Water.ps.cso` |
| `topology` | `TRIANGLE_LIST` |
| `blendMode` | `ALPHA_BLEND`（半透明・Terrain の後に描画） |
| `depthMode` | `DEPTH_READ`（深度書き込みなし。水面の後ろにある地形を正しく見せるため） |
| `rasterizerMode` | `SOLID_NOCULL`（水面の裏側も表示したい場合）または `SOLID`（片面） |
| `constantBuffers[0]` | Camera CB（b0 共通） |
| `constantBuffers[1]` | WaterCB（b1 水面専用） |
| `constantBuffers[3]` | Light CB（b3 共通） |
| `textures[0..5]` | normalMap1, normalMap2, foamTex, foamMask, envCube, sceneDepth |
| `layer` | `RenderLayer::Transparent`（Terrain より後） |

---

## WaterCB の事前計算（システム側）

シェーダーは `ω` と `k` を使うが、ユーザーが指定するのは `amplitude` と `wavelength` だけ。
WaterRenderSystem が CPU 側で変換してから CB に書き込む。

```cpp
// WaterRenderSystem.cpp 内 WaterCB 更新処理（スケッチ）
for (int i = 0; i < 4; ++i) {
    const GerstnerWave& w = water.waves[i];
    float k     = (2.0f * math::kPi) / w.wavelength;
    float omega = std::sqrt(9.8f * k); // 深水波の分散関係
    cb.waveDir[i]    = {w.direction.x, w.direction.z, w.steepness, 0.0f};
    cb.waveParams[i] = {w.amplitude, w.wavelength, omega, k};
}
```

---

## エラーハンドリング方針

[conventions/error_handling.md](../../conventions/error_handling.md) に準拠する。

| 状況 | 対応 |
|---|---|
| `normalMap1Path` / `normalMap2Path` が空 | フラット法線テクスチャ（128,128,255）で代替 |
| `foamTexPath` が空 | 1×1 白テクスチャで代替（泡は uniformly 表示される） |
| `envCubemapPath` が空 | 1×1 黒キューブマップで代替（反射なし） |
| シーンに TerrainComponent がない状態で foamDirty == true | 泡マスクを 0 で初期化してスキップ（assert 不要）、foamDirty = false |
| `resolution` が 0 | `assert`（ユーザー設定ミスはバグ扱い） |
| `GerstnerWave::steepness` > 1 | `assert`（steepness > 1 は波頂点の交差を引き起こす） |
| GPU バッファ生成失敗 | `assert`（GPU 初期化失敗は回復不能） |

---

## 将来拡張メモ

詳細設計は [features.md](features.md) を参照。

| 項目 | 概要 | Phase |
|---|---|---|
| スクリーンスペース屈折 | Water.hlsl + WaterCB 拡張のみ | 5 |
| 水没カメラ PostProcess | PostProcCB 拡張・Composite.hlsl 追記 | 6 |
| フローマップ | 河川・流れ方向 UV スクロール（Valve 方式） | 7 |
| ダイナミックリップル | CPU テクスチャ + `AddWaterRipple` API | 8 |
| コースティクス | 独立 PostProcess パス（HDR 乗算合成） | 9 |
| チャンク分割 | 大規模水域のフラスタムカリング | 10 |
| プラナー反射 | 水面を鏡として地形・オブジェクトを再描画（描画コスト 2 倍） | 未定 |
| FFT 波 | GPU コンピュートによる海洋スペクトル波 | 未定 |
