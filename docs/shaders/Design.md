# FBZZ Engine Shader Library 設計書

HLSL シェーダーライブラリの構成・命名規則・レジスタ割り当て・実装順序。  
C++ 側の実装詳細は `docs/renderer/Design.md` を参照。

---

## ロードマップ上の位置づけ

| Step | 内容 | 状態 |
|------|------|------|
| 6a | HLSL ライブラリ構築 (DX11) | **現在地** |
| 6b | DX12Renderer 実装 + RenderGraph | 未着手 |
| 6c | DXR (レイトレーシング) | 未着手 |

---

## ディレクトリ構成

```
assets/shaders/
│
├── Common/                        基盤ユーティリティ（エントリポイントなし）
│   ├── Math.hlsli
│   ├── Types.hlsli
│   ├── Binding.hlsli
│   ├── Constants.hlsli
│   ├── Space.hlsli
│   ├── Color.hlsli
│   └── Random.hlsli
│
├── Rendering/                     ライティングアルゴリズム（エントリポイントなし）
│   ├── BRDF.hlsli                 Cook-Torrance PBR
│   ├── Lighting.hlsli             Lambert / Phong / Blinn-Phong / PBR 関数
│   ├── Atmosphere.hlsli           Rayleigh + Mie 大気散乱（Skydome 用）
│   ├── Shadow.hlsli               PCF シャドウサンプリング
│   ├── ToneMap.hlsli              ACES Filmic トーンマッピング
│   ├── Fog.hlsli
│   └── IBL.hlsli                  ← Step 6c（DX12）
│
├── Pipeline/                      レンダーパス単位（GBuffer / Deferred Lighting）
│   ├── GBuffer.hlsl               PBR マテリアルの G-Buffer 書き込み
│   ├── DeferredLighting.hlsl      G-Buffer を読んでライティング合成
│   └── ShadowMap.hlsl             シャドウデプス書き込み
│
├── Material/                      マテリアル種別ごとのフォワードシェーダー
│   ├── Unlit.hlsl
│   ├── Lit.hlsl                   Lambert 拡散のみ
│   ├── Phong.hlsl                 Phong 鏡面反射
│   ├── BlinnPhong.hlsl            Blinn-Phong（旧 Mesh.hlsl の後継）
│   ├── PBR.hlsl                   Cook-Torrance フォワード版（デバッグ用途）
│   └── Sky/                       2ファイルあるためサブフォルダ
│       ├── Skybox.hlsl            TextureCube サンプリング
│       └── Skydome.hlsl           手続き大気散乱
│
├── PostProcess/
│   ├── SSAO/
│   │   ├── SSAO.cs.hlsl
│   │   └── SSAOBlur.cs.hlsl
│   ├── Bloom/
│   │   ├── BloomDownsample.cs.hlsl
│   │   └── BloomUpsample.cs.hlsl
│   └── Composite.hlsl             ToneMap + Bloom + Fog 最終合成
│
├── RayTracing/                    ← Step 6c（DX12 + DXR）
│   ├── Shadow.lib.hlsl
│   └── Reflection.lib.hlsl
│
├── Platform/
│   ├── DX11.hlsli                 SM 5.0 回避策・型エイリアス
│   └── DX12.hlsli                 SM 6.x・bindless 拡張（Step 6b）
│
└── compiled/                      .cso / .dxil 出力先
```

---

## ファイル命名規則

| 種別 | 規則 | 例 |
|------|------|----|
| VS + PS 同居 | `PascalCase.hlsl` | `GBuffer.hlsl` |
| CS 単独 | `PascalCase.cs.hlsl` | `SSAO.cs.hlsl` |
| DXR ライブラリ | `PascalCase.lib.hlsl` | `Shadow.lib.hlsl` |
| インクルード専用 | `PascalCase.hlsli` | `BRDF.hlsli` |

エントリポイント名は全シェーダー共通で `VSMain` / `PSMain` / `CSMain`。

サブフォルダは **2ファイル以上** のまとまりがある場合のみ作る。1ファイルならフラットに置く。

---

## マテリアル × パイプライン対応

| マテリアル | ライティングモデル | パイプライン | 備考 |
|---|---|---|---|
| Unlit | なし | Forward | 旧 Unlit.hlsl の後継 |
| Lit | Lambert 拡散のみ | Forward | |
| Phong | Phong 鏡面反射 | Forward | |
| BlinnPhong | Blinn-Phong | Forward | 旧 Mesh.hlsl の後継 |
| PBR | Cook-Torrance | **Deferred** | GBuffer 書き込みが主。フォワード版は Material/PBR/ |
| Skybox | キューブマップ | Forward | 深度トリック: VS で z = w |
| Skydome | 手続き大気散乱 | Forward | Atmosphere.hlsli を使用 |

フォワードマテリアルはすべて同一 cbuffer レイアウト（b0–b3）を使用するため、  
C++ 側から差し替えるだけで切り替え可能。

---

## レジスタ割り当て（`Common/Binding.hlsli`）

### cbuffer

| レジスタ | 名前 | 更新頻度 | 使用ステージ |
|---------|------|---------|------------|
| b0 | CameraConstants | per-frame | VS, PS |
| b1 | ObjectConstants | per-draw | VS |
| b2 | MaterialConstants | per-material | PS |
| b3 | LightConstants | per-frame | PS |
| b4 | ShadowConstants | per-frame | PS |
| b5 | PostProcConstants | per-pass | CS, PS |
| b6 | AtmosphereConstants | per-frame | PS（Skydome のみ） |

### Texture（読み取り専用）

| レジスタ | 名前 | バインドタイミング |
|---------|------|----------------|
| t0 | albedoTex | per-draw（マテリアル） |
| t1 | normalTex | per-draw |
| t2 | metallicRoughnessTex | per-draw |
| t3 | emissiveTex | per-draw |
| t4 | aoTex | per-draw |
| t5 | gbuffer0Tex（albedo + roughness） | per-pass（DeferredLighting） |
| t6 | gbuffer1Tex（normal + metallic） | per-pass |
| t7 | depthTex | per-pass |
| t8 | shadowMapTex | per-pass |
| t9 | ssaoTex | per-pass |
| t10 | bloomTex | per-pass（Composite） |
| t11 | envCubeTex | per-pass（Skybox / IBL） |
| t12 | envEquirectTex | per-pass（Skydome 等緯度テクスチャ） |

### UAV（コンピュートシェーダー出力）

| レジスタ | 用途 |
|---------|------|
| u0 | outputTex |
| u1 | outputTex2 |

---

## cbuffer 定義（`Common/Constants.hlsli`）

```hlsl
#include "Binding.hlsli"

cbuffer CameraConstants : register(b0) {
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 invViewProjection;  // worldPos 復元用
    float3   cameraPos;
    float    nearZ;
    float    farZ;
    float3   _pad0;
};

cbuffer ObjectConstants : register(b1) {
    float4x4 world;
    float4x4 worldInvTranspose;  // 非一様スケール対応
};

cbuffer MaterialConstants : register(b2) {
    float4 albedo;
    float  metallic;
    float  roughness;
    float  emissiveScale;
    uint   textureMask;          // bit0=albedo bit1=normal bit2=metalRough bit3=emissive
};

cbuffer LightConstants : register(b3) {
    float3 lightDir;
    float  _pad1;
    float3 lightColor;
    float  lightIntensity;
};

cbuffer ShadowConstants : register(b4) {
    float4x4 lightViewProjection;
    float2   shadowMapTexelSize;
    float    shadowBias;
    float    _pad2;
};

cbuffer PostProcConstants : register(b5) {
    float2 texelSize;
    float2 screenSize;
    float  exposure;
    float  time;
    float2 _pad3;
};

cbuffer AtmosphereConstants : register(b6) {
    float3 rayleighScattering;   // 波長ごとの散乱係数（RGB）
    float  mieScattering;
    float  planetRadius;         // 地球半径 (km)
    float  atmosphereRadius;     // 大気圏上端 (km)
    float  sunIntensity;
    float  mieG;                 // Mie 位相関数の非対称パラメータ
};
```

---

## 頂点フォーマット（`Common/Types.hlsli`）

```hlsl
// 全メッシュ共通（C++ 側 Mesh.hpp の Vertex と一致させること）
struct VSInput {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float3 tangent  : TANGENT;   // 法線マップ対応（Step 6a で追加）
    float2 uv       : TEXCOORD;
};

// G-Buffer MRT 出力
struct GBufferOut {
    float4 albedoRoughness : SV_Target0;  // RGBA8_UNORM
    float4 normalMetallic  : SV_Target1;  // RGBA16_FLOAT
};

// Lighting パスが GBuffer から復元したデータ（中間構造体）
struct GBufferData {
    float3 albedo;
    float  roughness;
    float3 worldNormal;
    float  metallic;
    float3 worldPos;
    float  ao;
};
```

---

## GBuffer レイアウト

| RenderTarget | フォーマット | 内容 |
|---|---|---|
| RT0 (gbuffer0) | RGBA8_UNORM | albedo (RGB) + roughness (A) |
| RT1 (gbuffer1) | RGBA16_FLOAT | world normal (RGB) + metallic (A) |
| Depth | D32_FLOAT | 深度 |

---

## レンダーパスフロー（Phase 1 完成形）

```
Pass 1  ShadowMap.hlsl               → shadow depth tex
Pass 2  GBuffer.hlsl                 → gbuffer0, gbuffer1, depth
Pass 3  SSAO.cs.hlsl                 → ssao tex（生）
        SSAOBlur.cs.hlsl             → ssao tex（平滑化）
Pass 4  DeferredLighting.hlsl        → HDR color tex
Pass 5  [Forward] Unlit / Lit /
        Phong / BlinnPhong / Sky     → HDR color tex に合成
Pass 6  BloomDownsample.cs.hlsl  ┐
        BloomUpsample.cs.hlsl    ┘   → bloom tex
Pass 7  Composite.hlsl               → swapchain（LDR）
```

---

## 既存シェーダーとの移行対応

| 既存ファイル | 移行先 | 備考 |
|-------------|--------|------|
| `Mesh.hlsl` | `Material/BlinnPhong.hlsl` | 内容はほぼそのまま移植 |
| `Unlit.hlsl` | `Material/Unlit.hlsl` | 内容はほぼそのまま移植 |
| `Debug.hlsl` | 移動なし | DebugDraw 専用・変更不要 |

`Mesh.hpp` の `Vertex` 構造体に `tangent` フィールドを追加する（法線マップ対応）。  
既存の `.bat` コンパイルスクリプトも新ファイルに合わせて更新する。

---

## Step 6a 実装順（DX11）

| 順 | 対象 | 依存 |
|----|-----|------|
| 1 | `Common/` 全ファイル | なし |
| 2 | `Rendering/BRDF.hlsli` | Math |
| 3 | `Rendering/Lighting.hlsli` | BRDF |
| 4 | `Material/Unlit.hlsl` | Constants, Types |
| 5 | `Material/Lit.hlsl` | Lighting |
| 6 | `Material/Phong.hlsl` | Lighting |
| 7 | `Material/BlinnPhong.hlsl` | Lighting |
| 8 | `Pipeline/ShadowMap.hlsl` | Constants |
| 9 | `Rendering/Shadow.hlsli` | なし |
| 10 | `Pipeline/GBuffer.hlsl` | Constants, Types, Space |
| 11 | `Pipeline/DeferredLighting.hlsl` | Lighting, Shadow |
| 12 | `Material/PBR.hlsl`（フォワード版） | BRDF, Lighting, Shadow |
| 13 | `PostProcess/SSAO/SSAO.cs.hlsl` + `SSAOBlur.cs.hlsl` | Random, Space |
| 14 | `PostProcess/Bloom/` | Color |
| 15 | `Rendering/ToneMap.hlsli` | Color |
| 16 | `PostProcess/Composite.hlsl` | ToneMap, Fog |
| 17 | `Material/Sky/Skybox.hlsl` | Constants, Types |
| 18 | `Rendering/Atmosphere.hlsli` + `Material/Sky/Skydome.hlsl` | Atmosphere |

---

## DX12 移行方針（Step 6b / 6c）

### コンパイルパスの変化

| 項目 | DX11 (Step 6a) | DX12 (Step 6b+) |
|------|----------------|-----------------|
| コンパイラ | `fxc.exe` | `dxc.exe` |
| バイトコード形式 | DXBC | DXIL |
| シェーダーモデル | SM 5.0 | SM 6.5+ |
| 出力拡張子 | `.cso` | `.dxil` |
| コンパイルスクリプト | `compile_shaders.bat` (fxc) | 別途 `compile_shaders_dx12.bat` (dxc) を追加 |

コンパイル対象のソース `.hlsl` / `.hlsli` は **共通**。  
`Platform/DX11.hlsli` または `Platform/DX12.hlsli` を先頭で include することで  
プラットフォーム差異を吸収する。

### cbuffer バインディングの変化

DX11 では `register(bN)` がシェーダーバインディングを直接決定する。  
DX12 では Root Signature がシェーダーの `register(bN)` を GPU ディスクリプタヒープへ  
マッピングするため、レジスタ番号の意味は変わらない。  
→ **`Common/Binding.hlsli` の変更は不要**。Root Signature 側を `Binding.hlsli` の番号に合わせる。

### `Platform/DX11.hlsli` の役割

SM 5.0 で使えない機能の無効化マクロを定義する。

```hlsl
// Platform/DX11.hlsli
#define PLATFORM_DX11 1

// SM5.0 では WaveIntrinsics 不可 → 使用箇所をガードするマクロ
#define WAVE_INTRINSICS_SUPPORTED 0
```

### `Platform/DX12.hlsli` の役割

SM 6.x で有効になる機能のエイリアスと有効化マクロを定義する。

```hlsl
// Platform/DX12.hlsli
#define PLATFORM_DX12 1
#define WAVE_INTRINSICS_SUPPORTED 1  // SM 6.0+

// SM 6.5+ : Inline Raytracing（RayTracing/ フォルダのシェーダーで使用）
#define RAY_QUERY_SUPPORTED 1

// SM 6.6+ : Bindless（将来の拡張用）
// #define BINDLESS_SUPPORTED 1
```

各 `.hlsli` でプラットフォーム依存の分岐が必要な場合は以下のように書く。

```hlsl
#if WAVE_INTRINSICS_SUPPORTED
    uint laneMin = WaveActiveMin(value);
#else
    uint laneMin = value;  // DX11 フォールバック
#endif
```

### RayTracing/ の有効化（Step 6c）

`RayTracing/` フォルダのシェーダーは DX12 + DXR 専用。  
`dxc.exe` で `-T lib_6_5` ターゲットを指定してコンパイルする。  
DX11 のビルドパスでは一切コンパイルしない。

```
Shadow.lib.hlsl    → dxc -T lib_6_5 -Fo Shadow.lib.dxil
Reflection.lib.hlsl → dxc -T lib_6_5 -Fo Reflection.lib.dxil
```

### 移行時に変更が必要なファイル一覧

| ファイル | 変更内容 |
|---------|---------|
| `compile_shaders.bat` | DX11 ターゲット用として維持 |
| `compile_shaders_dx12.bat` | 新規追加。dxc で全シェーダーを SM 6.5 再コンパイル |
| 各 `.hlsl` 先頭 | `#include "Platform/DX11.hlsli"` → `#include "Platform/DX12.hlsli"` に切り替え |
| C++ `ShaderManager` | `.cso` ロードパスに加え `.dxil` ロードパスを追加 |

**ソースの `.hlsl` / `.hlsli` 自体は原則変更しない。**  
Platform include の切り替えと Root Signature の実装だけで移行できる設計にする。
