# Water System 設計 — 追加機能詳細

浮力・屈折・水没 PostProcess・フローマップ・ダイナミックリップル・コースティクスの設計。
各機能は独立して追加できる。依存関係は各節の冒頭に示す。

---

## 1. 浮力統合（VolumeComponent 活用）

**依存**: `VolumeComponent`, `ColliderComponent`, `PhysicsSystem`（既存）  
**WaterRenderSystem は不関与。** 物理システムが既存の仕組みで全て処理する。

### 設計方針

浮力は `physics::VolumeType::Buoyancy` として既に `PhysicsSystem` に実装済みである。
Water の GameObjet に以下 2 つのコンポーネントを追加するだけで機能する。

| コンポーネント | 役割 |
|---|---|
| `BoxColliderComponent`（trigger） | 水域の AABB として機能。ColliderVolume の形状判定に使われる |
| `VolumeComponent(type=Buoyancy)` | RigidBody が AABB 内に入ったとき浮力・抗力を適用 |

```
GameObject "Sea"
  ├─ Transform              (position.y = 水位)
  ├─ WaterComponent         (描画)
  ├─ BoxColliderComponent   (isTrigger=true, size = extentX × deepDepth × extentZ)
  └─ VolumeComponent        (type=Buoyancy, buoyancy=15.0, drag=2.0)
```

`ColliderVolume::Contains(position)` が BoxCollider の AABB 内判定を行い、
`ColliderVolume::Apply(body, dt)` が Y 軸上向き浮力と速度比例抗力を加算する。

### Gerstner 波考慮の精度向上（任意）

デフォルトの浮力は ColliderVolume の上面（`Transform.position.y`）を均一な水面として扱う。
Gerstner 波の波頭・波谷を考慮したい場合、`WaterComponent` に以下を追加する。

```cpp
// WaterComponent.hpp に追加
// Gerstner 波方程式の CPU 評価。浮力精度向上のため PhysicsSystem から呼び出せる。
// WHY: GPU 側の波変位と一致させるため、同じ方程式を CPU でも評価する。
float GetSurfaceHeightAt(float localX, float localZ, float time) const;
```

```cpp
// WaterComponent.cpp 実装スケッチ
float WaterComponent::GetSurfaceHeightAt(float localX, float localZ, float time) const
{
    if (!enableGerstnerWaves) return 0.0f;
    float y = 0.0f;
    for (const auto& w : waves) {
        if (w.amplitude < 0.0001f) continue;
        float k     = math::kTwoPi / w.wavelength;
        float omega = std::sqrt(9.8f * k);
        float phi   = k * (w.direction.x * localX + w.direction.z * localZ)
                    - omega * time;
        y += w.amplitude * std::sin(phi);
    }
    return y;
}
```

PhysicsSystem / Script から `WaterComponent::GetSurfaceHeightAt()` を呼び、
`VolumeComponent::buoyancy` をスケールして適用量を調整する設計が最もシンプル。
WHY: PhysicsSystem が ColliderVolume の外で水面高さを取得するだけで済み、
依存方向（engine → physics）を保てる。

---

## 2. スクリーンスペース屈折

**依存**: Water.hlsl・WaterCB の拡張のみ。RenderPassContext の `hdrRT` カラーを t6 にバインド。

### 原理

水面法線でスクリーン UV をオフセットし、その下のシーンカラーをサンプリングする。
これにより水底・水中オブジェクトが波に合わせて歪んで見える。

```
屈折 UV = screenUV + N.xz * refractionStrength * (1.0 - fresnel)
refractColor = sceneColor.Sample(screenUV + offset)
```

Fresnel 係数で重み付けすることで、見込み角が小さい（水面を真上から見る）ほど
屈折が強く見え、斜めから見ると反射に支配される物理挙動に近くなる。

### WaterCB 追加フィールド

```hlsl
// WaterCB (b1) に追加
float  g_refractionStrength; // 屈折強度 [0, 0.1] 推奨（大きすぎると破綻する）
float3 _refrPad;
```

### テクスチャスロット追加

```hlsl
// Water.hlsl
Texture2D g_sceneColor : register(t6); // シーン HDR カラーバッファ（屈折サンプリング用）
```

WaterRenderSystem が DrawCall を発行する前に、
`RenderPassContext::handles.hdrRT` のカラーテクスチャを `textures[6]` にバインドする。

### ピクセルシェーダー追記

```hlsl
// PSMain 内、深度フェード計算後に追加
float2 refrOffset  = blendedNormal.xz * g_refractionStrength * (1.0f - fresnel);
float2 refrUV      = saturate(screenUV + refrOffset);
float3 refractColor = g_sceneColor.Sample(g_samplerClamp, refrUV).rgb;

// 屈折色を water 色に混ぜる（深部ほど屈折が水色で上書きされる）
waterColor = lerp(refractColor, waterColor, saturate(waterDepth / g_shallowDepth));
```

### エラーハンドリング

| 状況 | 対応 |
|---|---|
| hdrRT が未設定 | t6 に 1×1 黒テクスチャをバインド（屈折なし、見た目上の問題なし） |
| refractionStrength = 0 | offset が (0,0) となり通常の sceneUV をサンプリング（コスト無駄だが破綻なし） |

---

## 3. 水没カメラ PostProcess

**依存**: `PostProcCB`（拡張）・`Composite.hlsl`（追記）・`WaterRenderSystem`（水面高さを PostProcCB に書き込む）

### 設計方針

`PostProcCB` の末尾パディング（`_pad[1]`）と空き領域を活用し、
水没エフェクト専用フィールドを追加する。  
既存の `customParameters[4]` は CustomPostProcessPass 専用とし、混在させない。

### PostProcCB 拡張（RenderPassContext.hpp + Constants.hlsli）

```cpp
// RenderPassContext.hpp — PostProcCB に追加
struct PostProcCB {
    // ... （既存フィールド）...
    float _pad[1];              // 既存パディング → 削除してフィールドを追加

    // ── 水没エフェクト ──────────────────────────────
    float  underwaterStrength;  // 水没エフェクト強度 [0, 1]（0 = 非水中）
    float  underwaterDepth;     // カメラが水面から何 m 潜っているか [m]
    float2 _underwaterPad;
    float3 underwaterColor;     // 水中フォグ色（shallowColor / deepColor のブレンド）
    float  underwaterFogDensity; // 水中フォグ密度（通常フォグより強い）
};
```

```hlsl
// Constants.hlsli — PostProcConstants cbuffer に追加
cbuffer PostProcConstants : register(CB_POSTPROC) {
    // ... 既存 ...
    float  _ppPad;              // 削除
    float  underwaterStrength;
    float  underwaterDepth;
    float2 _underwaterPad;
    float3 underwaterColor;
    float  underwaterFogDensity;
};
```

### WaterRenderSystem が PostProcCB を更新

WaterRenderSystem の末尾（水面 DrawCall 発行後）に、
カメラ位置と水面高さを比較して PostProcCB を更新する。

```cpp
// WaterRenderSystem.cpp 末尾のスケッチ
void UpdateUnderwaterCB(
    Scene&              scene,
    const Camera&       camera,
    PostProcCB&         ppCB)
{
    ppCB.underwaterStrength = 0.0f; // まずリセット

    for (auto [water, tf] : scene.View<WaterComponent, Transform>()) {
        if (!water.enabled) continue;
        float waterWorldY = tf.position.y;
        float cameraY     = camera.GetPosition().y;
        if (cameraY >= waterWorldY) continue; // 水上 → スキップ

        float depth = waterWorldY - cameraY;
        ppCB.underwaterStrength  = math::Saturate(depth / 2.0f); // 2m で完全水没
        ppCB.underwaterDepth     = depth;

        // shallowColor / deepColor を深度でブレンド
        float t = math::Saturate(depth / water.deepDepth);
        math::Vector3 col = math::Lerp(water.shallowColor, water.deepColor, t);
        ppCB.underwaterColor[0]  = col.x;
        ppCB.underwaterColor[1]  = col.y;
        ppCB.underwaterColor[2]  = col.z;
        ppCB.underwaterFogDensity = math::Lerp(0.05f, 0.4f, t);
        break; // 複数水域あれば最初にヒットした水面を使う
    }
}
```

### Composite.hlsl 追記

```hlsl
// PSMain の色調整前に挿入
if (underwaterStrength > 0.0f)
{
    // 水中フォグ（深度指数減衰）
    float ndcZ     = texDepth.Sample(sampDefault, uv).r;
    float linDepth = nearZ * farZ / (farZ - ndcZ * (farZ - nearZ));
    float fogFactor = 1.0f - exp(-underwaterFogDensity * linDepth);
    ldr = lerp(ldr, underwaterColor, fogFactor * underwaterStrength);

    // 水中 UV ゆらぎ（波紋の屈折効果）
    float wave = sin(uv.x * 20.0f + time * 2.0f) * 0.003f
               * underwaterStrength;
    ldr = lerp(ldr, texHDR.Sample(sampDefault, uv + wave).rgb, underwaterStrength * 0.3f);

    // 彩度低下（水は色を吸収する）
    float gray   = dot(ldr, float3(0.299f, 0.587f, 0.114f));
    float satMul = lerp(1.0f, 0.5f, underwaterStrength);
    ldr = lerp(float3(gray, gray, gray), ldr, satMul);
}
```

### SSAO の自動無効化

水中では SSAO は不自然なので、WaterRenderSystem が更新した `underwaterStrength > 0` のとき
`ExecuteSSAOPass` 内の ssaoIntensity を 0 に落とす。
これは `PostProcCB.ssaoIntensity` を 0 で上書きすることで実現できる。

---

## 4. フローマップ（河川・流れ）

**依存**: Water.hlsl・WaterCB・WaterComponent の拡張のみ。

### 設計方針

フローマップは R8G8 テクスチャで各ピクセルの流れ方向（XZ）を格納する。
Valve が Portal で考案した「2 フェーズ位相ずらし」で継ぎ目のないスクロールを実現する。

WHY 2 フェーズ: 単純スクロールでは UV が 0→1 でリセットされたとき継ぎ目が見える。
2 つの位相（0.5 ずらし）をブレンドすることで常に片方が自然な状態になる。

```
phase0 = frac(time * flowSpeed)        // 0 → 1 を繰り返す
phase1 = frac(time * flowSpeed + 0.5)  // phase0 から 0.5 ずれる
blend  = |2 * phase0 - 1|              // 0→1→0 の三角波（2 サンプルのクロスフェード）
```

### WaterComponent 追加フィールド

```cpp
struct WaterComponent {
    // ... 既存フィールド ...

    // ---- フローマップ（河川・流れ） ----
    bool        enableFlowMap = false;
    std::string flowMapPath;     // R=flow X, G=flow Z, [0,1] → [-1,1] に変換
    float       flowSpeed    = 0.3f;  // フロースクロール速度 [UV/s]
    float       flowTiling   = 1.0f;  // フローマップ UV タイリング（法線と独立）
};
```

### WaterCB 追加フィールド

```hlsl
float  g_flowSpeed;
float  g_flowTiling;
uint   g_enableFlowMap; // 0 = 無効、1 = 有効（フロー / 通常を分岐）
float  _flowPad;
```

### テクスチャスロット追加

```hlsl
Texture2D g_flowMap : register(t7); // R=flowX, G=flowZ (RG8 UNORM)
```

### ピクセルシェーダー変更

```hlsl
// PSMain 内、法線マップサンプリング部分を置き換え

float3 waveNormal;

if (g_enableFlowMap)
{
    // フローベクトルを取得（[0,1] → [-1,1]）
    float2 flow = g_flowMap.Sample(g_samplerClamp, input.uv * g_flowTiling).rg
                * 2.0f - 1.0f;

    // 2 フェーズ位相ずらし
    float phase0 = frac(g_time * g_flowSpeed);
    float phase1 = frac(g_time * g_flowSpeed + 0.5f);
    float blend  = abs(2.0f * phase0 - 1.0f);

    float2 uv0 = input.uv * g_normalMap1Tiling + flow * phase0;
    float2 uv1 = input.uv * g_normalMap1Tiling + flow * phase1;
    float3 n0  = g_normalMap1.Sample(g_sampler, uv0).rgb * 2.0f - 1.0f;
    float3 n1  = g_normalMap1.Sample(g_sampler, uv1).rgb * 2.0f - 1.0f;
    waveNormal = normalize(lerp(n0, n1, blend));
}
else
{
    // 既存のデュアルスクロール法線
    float2 uv1 = input.uv * g_normalMap1Tiling + g_normalMap1Scroll * g_time;
    float2 uv2 = input.uv * g_normalMap2Tiling + g_normalMap2Scroll * g_time;
    float3 n1  = g_normalMap1.Sample(g_sampler, uv1).rgb * 2.0f - 1.0f;
    float3 n2  = g_normalMap2.Sample(g_sampler, uv2).rgb * 2.0f - 1.0f;
    waveNormal = normalize(float3(n1.xy + n2.xy, n1.z * n2.z));
}

waveNormal = normalize(lerp(float3(0, 0, 1), waveNormal, g_normalStrength));
```

---

## 5. ダイナミックリップル（波紋）

**依存**: WaterRenderSystem 内部（新 `WaterRippleSystem` 構造）・Water.hlsl（t8 追加）。

### 設計方針

RigidBody・パーティクル・スクリプトなどが水面に触れたとき、波紋（法線オフセット）を
CPU テクスチャに書き込む。各波紋は半径方向に広がりながら減衰して消滅する。

GPU 側は法線オフセットテクスチャとして受け取り、既存の法線ブレンドに加算するだけ。
WHY CPU 実装: 波紋数は同時数十程度で済み、GPU コンピュートシェーダーを追加する
コストより CPU 方式のシンプルさが勝る。将来的に数百波紋が必要になったら GPU 化する。

### 内部データ構造

```cpp
// WaterRenderSystem.cpp 内部
struct WaterRipple {
    fbzz::math::Vector2 positionUV;  // 波紋中心（水面 UV 座標 [0,1]）
    float               amplitude;   // 現在振幅 [0, 1]（減衰で減少）
    float               radius;      // 現在外径（UV 単位）
    float               speed;       // 半径拡張速度 [UV/s]
    float               decayRate;   // 振幅の指数減衰率 [1/s]（大きいほど早く消える）
    float               waveWidth;   // 波の幅（リング幅、UV 単位）
};

// エンティティごとの波紋リスト + 波紋テクスチャ
struct WaterRippleState {
    std::vector<WaterRipple>            ripples;
    std::vector<uint8_t>                rippleTex;  // RG8: XZ 法線オフセット
    uint32_t                            texWidth;
    uint32_t                            texHeight;
    ResourceHandle<TextureTag>          gpuTex;
    bool                                texDirty = false;
};
static std::unordered_map<uint32_t, WaterRippleState> s_rippleStates;
```

### 公開 API

```cpp
// WaterRenderSystem.hpp に追加
namespace fbzz::scene {

// 波紋を追加する。水面ローカル UV [0,1] で座標を指定する。
// amplitude: 初期振幅 [0, 1]（1 が最大変形）
// speed: 波紋の広がる速度 [UV/s]（0.3 前後推奨）
// decayRate: 振幅の指数減衰 [1/s]（2.0 で約 0.5 秒で消える）
void AddWaterRipple(
    EntityID            waterEntity,
    math::Vector2       positionUV,
    float               amplitude = 0.5f,
    float               speed     = 0.25f,
    float               decayRate = 2.0f);

} // namespace fbzz::scene
```

使用例（スクリプト側）:

```cpp
// 自由落下した RigidBody が水面の高さを通過したとき:
if (transform->position.y <= waterLevel && prevY > waterLevel) {
    math::Vector2 uvPos = WorldToWaterUV(transform->position, water, waterTf);
    scene::AddWaterRipple(waterEntityId, uvPos, 0.8f, 0.3f, 2.5f);
}
```

### CPU テクスチャ更新アルゴリズム

```
毎フレーム WaterRenderSystem が波紋リストを更新:

1. rippleTex を 0 で初期化
2. 各 WaterRipple について:
     radius   += speed * dt
     amplitude *= exp(-decayRate * dt)
     if amplitude < 0.002 → 削除

   テクスチャの各ピクセル (u, v) について:
     dist = length(uv - positionUV)
     ring = dist - radius                    // 波頭からの距離
     ringFactor = exp(-(ring*ring) / (waveWidth*waveWidth))
                  * sign を考慮したサイン波
     法線 XZ オフセット = normalize(uv - positionUV) * ringFactor * amplitude

3. RG チャンネルに 0.5 + 0.5 * normalOffset を書き込む（[0,1] にパック）
4. GPU テクスチャを Update()（texDirty = true のとき）
```

WHY 全ピクセル更新: 波紋数が少ない間はキャッシュに収まり高速。
波紋が増えた場合はタイルごとに更新するよう最適化する。

### テクスチャスロット追加

```hlsl
Texture2D g_rippleTex : register(t8); // RG8: 波紋法線オフセット
```

### ピクセルシェーダー追記

```hlsl
// waveNormal の計算後、TBN 変換前に追加
float3 rippleNormal = g_rippleTex.Sample(g_samplerClamp, input.uv).rga * 2.0f - 1.0f;
rippleNormal.z = sqrt(saturate(1.0f - rippleNormal.x*rippleNormal.x
                                    - rippleNormal.y*rippleNormal.y));
waveNormal = normalize(waveNormal + rippleNormal * 0.5f);
```

---

## 6. コースティクス

**依存**: 独立した `ExecuteUnderwaterCausticsPass`（PostProcessPasses.hpp に追加）。
TerrainRenderSystem への変更は不要。HDR バッファへの乗算合成で実現する。

### 原理

水底・水中オブジェクト表面に投影される光の屈折模様（コースティクス）を、
スクリーン空間でポストプロセスとして合成する。

```
1. 深度バッファからワールド座標を復元（invViewProjection × NDC 座標）
2. ワールド座標の Y が水面 Y 以下 → 水中の表面と判定
3. コースティクステクスチャを水面 XZ 投影 UV で参照（アニメーション付き）
4. 水深（waterY - surfaceY）に応じてコースティクス強度を減衰
5. 結果を HDR バッファに乗算合成
```

WHY スクリーンスペース: Terrain や Mesh に対して個別にテクスチャスロットを追加せずに済む。
デメリットはカメラが動いたとき表面移動にアーティファクトが出やすい点。

### 定数バッファ（専用 UnderwaterCB）

DrawCall ではなく FullScreen Triangle パスなので、
既存の `PostProcConstants(b5)` の `customParameters` を流用する。

```hlsl
// Composite.hlsl か専用 Caustics.hlsl 内
// customParameters 使用割り当て:
//   [0] = caustics intensity scale
//   [1] = caustics tiling (XZ 投影 UV のスケール)
//   [2] = water surface Y（ワールド座標）
//   [3] = caustics time offset (= g_time * causticsSpeed)
```

### テクスチャ

```hlsl
// 既存の t5 以降の空きスロットを使用。Caustics パスは独立した Draw で完結するため
// Water.hlsl のスロットとは共有しない。
Texture2D g_causticsTex : register(t0); // コースティクステクスチャ（Voronoi 系）
Texture2D g_hdrColor    : register(t1); // HDR カラーバッファ（乗算対象）
Texture2D g_depth       : register(t2); // シーン深度バッファ
```

### CausticsPass シェーダー概要（Caustics.hlsl）

```hlsl
float4 PSMain(FSTriVSOut p) : SV_Target0
{
    float  ndcZ       = g_depth.Sample(sampDefault, p.uv).r;
    float3 hdr        = g_hdrColor.Sample(sampDefault, p.uv).rgb;

    // 深度 = 1（スカイ）は除外
    if (ndcZ >= 0.9999f) return float4(hdr, 1.0f);

    // ワールド座標を復元
    float4 ndcPos  = float4(p.uv * 2.0f - 1.0f, ndcZ, 1.0f);
    ndcPos.y      *= -1.0f;
    float4 worldH  = mul(ndcPos, invViewProjection);
    float3 worldPos = worldH.xyz / worldH.w;

    float waterSurfY = customParameters[2];
    float depth      = waterSurfY - worldPos.y;
    if (depth < 0.0f) return float4(hdr, 1.0f); // 水面より上は除外

    // コースティクス UV（XZ 平面投影）
    float  tiling    = customParameters[1];
    float  timeOff   = customParameters[3];
    float2 causticsUV = worldPos.xz * tiling + float2(timeOff, timeOff * 0.7f);

    // 2 枚のコースティクスをずらしてブレンド（単純繰り返しを緩和）
    float caus0 = g_causticsTex.Sample(sampDefault, causticsUV).r;
    float caus1 = g_causticsTex.Sample(sampDefault, causticsUV * 1.3f + 0.5f).r;
    float caustics = (caus0 + caus1) * 0.5f;

    // 深度減衰（水底が深いほど薄い）
    float attenuation = exp(-depth * 0.5f);
    float intensity   = caustics * attenuation * customParameters[0];

    // HDR に乗算合成（1.0 を下回らないようにクランプ）
    hdr *= 1.0f + intensity;

    return float4(hdr, 1.0f);
}
```

### 実行順序

```
ExecuteCompositePass()   // トーンマップ後の LDR に適用すると明るさが合わない
    ↓
ExecuteCausticsPass()    // HDR バッファに乗算合成 → トーンマップで正しくクランプされる
    ↓
ExecuteFxaaPass()
```

WHY HDR に合成: LDR に適用すると白飛び部分でコースティクスが消える。
HDR 段階で乗算することでトーンマッパーが自然に処理する。

---

## 追加フィールドまとめ（WaterComponent）

既存設計からの追加分のみ記載する。

```cpp
struct WaterComponent {
    // （既存フィールド省略）

    // ---- スクリーンスペース屈折 ----
    float refractionStrength = 0.03f; // [0, 0.1] 推奨

    // ---- フローマップ ----
    bool        enableFlowMap = false;
    std::string flowMapPath;
    float       flowSpeed     = 0.3f;
    float       flowTiling    = 1.0f;

    // ---- コースティクス ----
    bool        enableCaustics   = true;
    float       causticsIntensity = 0.4f;
    float       causticsTiling    = 0.5f;
    float       causticsSpeed     = 0.15f;
    std::string causticsTexPath;          // 空文字 = Voronoi 手続き生成テクスチャで代替（未実装時）
};
```

---

## 実装フェーズ補足

各機能の推奨実装順と主な作業量の目安。

| Phase | 機能 | 主な変更ファイル | 作業量 |
|---|---|---|---|
| **5** | スクリーンスペース屈折 | Water.hlsl, WaterCB | 小 |
| **6** | 水没カメラ PostProcess | PostProcCB, Constants.hlsli, Composite.hlsl, WaterRenderSystem.cpp | 中 |
| **7** | フローマップ | Water.hlsl, WaterCB, WaterComponent | 小 |
| **8** | ダイナミックリップル | WaterRenderSystem.cpp (大幅), Water.hlsl | 中 |
| **9** | コースティクス | Caustics.hlsl (新), PostProcessPasses.hpp/cpp, CompositePass.cpp | 大 |
| **—** | 浮力（VolumeComponent） | 既存のまま。ユーザーが BoxCollider + VolumeComponent を追加するだけ | なし |

---

## 隣接ドキュメント

- [overview.md](overview.md) — 全体フェーズ計画
- [rendering.md](rendering.md) — 基本描画パイプライン・シェーダー基礎設計
- [../../conventions/error_handling.md](../../conventions/error_handling.md) — エラーハンドリング規則
