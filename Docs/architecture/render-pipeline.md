# Render Pipeline

FBZZ Engine の描画は **RenderGraph ベースの Deferred + Forward ハイブリッド** で構成される。
パスの実行順と一時リソースのライフタイムは `RenderGraph` が静的解析し、
物理 RenderTarget をフレームをまたいで再利用 (alias) することでメモリを節約する。

---

## 全体フロー

```
RenderSystem::OnRender()
  │
  ├─ [Shadow Pass]          ← 全不透明ジオメトリを光源 VP でレンダリング
  │    ShadowMap RT (Depth only)
  │
  ├─ [GBuffer Pass]         ← Deferred 対象メッシュを MRT に書き込む
  │    GBuffer RT
  │      Albedo+Alpha  (RGBA8)
  │      Normal        (RGBA16F)
  │      PBR params    (RGBA8)  ← Roughness / Metallic / AO / Emissive
  │      Depth
  │
  ├─ [Deferred Lighting]    ← GBuffer を読んで HDR RT に Lit 結果を合成
  │    HDR RT ← GBuffer reads + ShadowMap read
  │
  ├─ [Forward Opaque]       ← Deferred 非対応 or 特殊マテリアル
  │    HDR RT (write)
  │    対象: Terrain / Foliage / Detail / Skinned PBR など
  │
  ├─ [Sky Pass]             ← Skybox / Skydome
  │
  ├─ [Water / Caustics]     ← 反射・屈折・コースティクスを HDR RT に合成
  │
  ├─ [Forward Transparent]  ← アルファブレンド (奥から手前ソート)
  │    Particle / Trail / MeshTrail / Decal
  │
  ├─ [Post Process]
  │    SSAO → Bloom (Downsample CS → Upsample CS)
  │         → FXAA → Composite (ToneMap + Color Grading)
  │         → Custom Post Process
  │
  ├─ [Selection Outline]    ← エディター専用: SelectionMask → Outline
  │
  └─ [UI]                   ← Canvas / Image / Text (LDR 最終 RT へ)
```

---

## RenderGraph — パス依存解析とリソース管理

`RenderGraph` (`Engine/include/Engine/Renderer/RenderGraph.hpp`) が
読み書き宣言からパス間の依存 DAG を構築し、未使用パスを自動カリングする。

```cpp
// パス登録例 (RenderPipeline 経由)
pipeline.AddRawPass(
    "DeferredLighting",
    { "GBuffer", "ShadowDepth" },   // reads
    { "HDR" },                       // writes
    [&ctx]{ ExecuteDeferredLightingPass(ctx); }
);
```

### トランジェント RT のエイリアシング

```
フレーム内ライフタイム解析
  GBuffer  : pass 1 → pass 3   ┐
  ShadowMap: pass 0 → pass 2   ┘ aliasGroup=0 → 物理 RT を 1 枚共有

RebuildTransientPool() でグループ → 物理ハンドルをマップ
GetTransientRT(name) でパスコールバックがハンドルを取得
```

トポロジ変化 (パスの有効/無効切替) があったフレームのみ `Plan()` を再実行し、
変化がないフレームは前フレームの実行順を再注入してコストを節約する。

---

## GBuffer レイアウト

| チャンネル | RT | 内容 |
|-----------|-----|-----|
| Albedo.rgb + Alpha.a | GBuffer RT[0] | ベースカラー + アルファ |
| Normal.xyz | GBuffer RT[1] | ワールド法線 (RGBA16F) |
| Roughness / Metallic / AO / Emissive | GBuffer RT[2] | PBR パラメーター |
| Depth | Depth Stencil | ハードウェア深度 |

Deferred Lighting パスはこれら 4 枚を読み、ポイント / スポット / ディレクショナルライトを
1 フルスクリーンパスで合成して HDR RT に書き込む。

---

## シェーダー種別

### Surface (Static Mesh)

| シェーダー | パス | 特徴 |
|-----------|-----|------|
| `PBR` | Deferred | GGX BRDF, Roughness/Metallic ワークフロー |
| `Lit` | Forward | ランバート + Blinn-Phong 簡易モデル |
| `Toon` | Forward | 段階ランプ + アウトライン法線押し出し |
| `Subsurface` | Forward | SSS 近似 (透過散乱項) |
| `Anisotropic` | Forward | 異方性ハイライト (Kajiya-Kay) |
| `Dissolve` | Forward | ノイズテクスチャによるアルファカットアウト溶解 |
| `RimLight` | Forward | 輪郭フレネル発光 |

### Skinned Mesh

上記 Surface シェーダーのスキン対応版 (`Skinned` プレフィックス) が全種類存在する。
頂点シェーダーでボーン行列を StructuredBuffer から読み、GPU 上でスキニングを実行する。

### エフェクト

| シェーダー | 説明 |
|-----------|------|
| `Particle` | CPU パーティクル (ビルボード) |
| `ParticleGPU` | GPU パーティクル (CS シミュレーション + ビルボード描画) |
| `Trail` | リボントレイル (頂点ストリーム更新) |
| `MeshTrail` / `SkinnedMeshTrail` | メッシュ残像エフェクト |
| `Water` | FFT 波面 + 反射・屈折・コースティクス |
| `Terrain` | マルチレイヤーペイント + Normal Blending |

---

## ポストプロセス チェーン

```
HDR RT
  │
  ├─ SSAO (Compute)      → ssaoRaw → SSAOBlur → ssaoBlur
  │
  ├─ Bloom (Compute)
  │    BloomDownsample ×4 → bloomHalf
  │    BloomUpsample  ×4 → bloomFull
  │
  ├─ Composite (Pixel)
  │    ToneMap (ACES / Reinhard / Uncharted2)
  │    + Color Grading (Contrast / Saturation / HueShift / Temperature)
  │    + Vignette / Film Grain / Chromatic Aberration / Lens Distortion
  │    + Fog / Underwater / Sepia / Invert / Posterize
  │    + Procedural 32^3 Color LUT (RGBA8 Texture3D)
  │    + Bloom 合成 + SSAO 適用
  │
  ├─ FXAA               → LDR RT
  │
  └─ Custom Post Process (スクリプト拡張可能)
```

`PostProcCB` (定数バッファ) が全パラメーターを一括管理し、
`RenderSettings` を通じてスクリプト・エディターから実行時に変更できる。

Color LUTは外部DDSを読み込まず、`LUTColorGradingSettings`からCPUで32×32×32の
RGBA8 Texture3Dを生成する。x=R、y=G、z=Bの軸順とLDR sRGB入力を固定し、DX11では
Immutable SRVとして保持する。設定値が変化した場合だけ再生成する。

### パイプライン設定の互換性

| 区分 | パス / 設定 | 規則 |
|---|---|---|
| AA スロット | FXAA / TAA | 排他。競合時は安定した FXAA を優先する |
| AO スロット | SSAO / GTAO | 排他。競合時は安定した SSAO を優先する |
| Deferred 専用 | SSR / GTAO / Contact Shadows | Forward では実行されない |
| IBL 必須入力 | Irradiance / Prefiltered Cubemap | 両方指定されるまで IBL 寄与を 0 にする |
| UAV u3 | SSR / Contact Shadows | 逐次パスで時分割するため同時有効化可能 |

TOML 読み込み時と Inspector 表示時は `RenderSettings::NormalizeExclusivePipelineSlots()` を使い、
設定経路に関係なく同じ排他規則を適用する。

---

## 影システム

ディレクショナルライト専用のカスケードなし単一シャドウマップ実装。
NDC バイアスはシーン規模に依存しないよう `shadowBiasNDC = 0.005f / depthRange` として
ワールド空間で約 5mm 相当のバイアスを保つよう正規化する。

```
ShadowPass → shadowMapRT (depth)
           ↓
DeferredLighting / ForwardPasses が shadowDepthTex を読んでソフトシャドウを適用
```

---

## オクルージョンカリング

`OcclusionCuller` が CPU 側 Software Rasterizer として動作し、
フラスタムカリング通過後のオブジェクトに対して深度バッファ比較による隠面除去を行う。
`RenderPassContext` を通じて全パスが同一カラーデータを参照する。

```
statsTotalObjects / statsFrustumCulled / statsOcclusionCulled / statsDrawCalls
  ← RenderDebugOverlay でリアルタイム表示
```

---

## スクリプトからのパス追加

```cpp
// Script から UserRenderPassDesc を登録する例
UserRenderPassDesc desc;
desc.name = "MyWaterEffect";
desc.injectionPoint = UserRenderPassInjectionPoint::AfterOpaque;
desc.accesses = { { "HDR", RenderGraph::ResourceUsage::ReadWrite } };
desc.execute  = [](RenderPassContext& ctx) { /* カスタム描画 */ };
```

挿入点は `AfterOpaque / AfterTransparent / BeforePostProcess` の 3 種類。
`RenderGraph` の依存解析が自動で実行順を確定するため、パス間の手動ソートは不要。
