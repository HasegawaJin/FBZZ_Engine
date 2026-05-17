# DirectX 11 レンダーパイプライン

## 全体像

CPUがDrawCallを発行すると、GPUは以下のステージを順番に処理して最終的に画面へピクセルを書き出す。

```
CPU (Application)
│
│  IASetVertexBuffers / IASetIndexBuffer
│  VSSetShader / PSSetShader
│  DrawIndexed / Draw
▼
┌─────────────────────────────────────────────────────┐
│              GPU パイプライン                        │
│                                                     │
│  IA → VS → [HS → DS → GS] → RS → PS → OM          │
│                                                     │
│  [] = オプション (Step 1 では使わない)               │
└─────────────────────────────────────────────────────┘
         │
         ▼
    RenderTarget (テクスチャ or バックバッファ)
         │
         ▼
    Present → 画面
```

---

## 各ステージ詳細

### IA — Input Assembler（入力アセンブラ）

頂点バッファとインデックスバッファを読み込み、プリミティブ（三角形）を組み立てる。

```cpp
// 設定する情報
context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
context->IASetInputLayout(inputLayout);    // 頂点フォーマット定義
context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
```

**Input Layout**: 頂点バッファの各フィールドが何を意味するか GPUに教える。

```cpp
// FBZZ Engine の頂点フォーマット
D3D11_INPUT_ELEMENT_DESC layout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};
// stride = 32 bytes
```

---

### VS — Vertex Shader（頂点シェーダー）

頂点ごとに実行される。主な仕事は座標変換。

```hlsl
// 典型的な Vertex Shader
cbuffer CameraBuffer : register(b0)  // Constant Buffer スロット 0
{
    float4x4 View;
    float4x4 Projection;
};

cbuffer ObjectBuffer : register(b1)  // スロット 1
{
    float4x4 World;
};

struct VSInput
{
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VSOutput
{
    float4 Position : SV_POSITION;  // クリップ空間座標（必須）
    float3 Normal   : NORMAL;
    float2 TexCoord : TEXCOORD;
    float3 WorldPos : WORLD_POS;
};

VSOutput main(VSInput IN)
{
    VSOutput OUT;
    float4 worldPos = mul(float4(IN.Position, 1.0), World);
    OUT.WorldPos    = worldPos.xyz;
    OUT.Position    = mul(mul(worldPos, View), Projection);
    OUT.Normal      = mul(IN.Normal, (float3x3)World);
    OUT.TexCoord    = IN.TexCoord;
    return OUT;
}
```

**座標空間の流れ**:
```
ローカル空間
    × World行列
    ↓
ワールド空間
    × View行列
    ↓
ビュー空間（カメラから見た座標）
    × Projection行列
    ↓
クリップ空間（-1〜1 に正規化される）
    ↓ (GPU が自動で実施)
NDC / スクリーン空間
```

---

### RS — Rasterizer Stage（ラスタライザ）

三角形をフラグメント（ピクセル候補）に変換する。補間・クリッピング・カリングもここ。

```cpp
D3D11_RASTERIZER_DESC rsDesc = {};
rsDesc.FillMode              = D3D11_FILL_SOLID;       // WIREFRAME も可
rsDesc.CullMode              = D3D11_CULL_BACK;        // 裏面を描かない
rsDesc.FrontCounterClockwise = FALSE;                  // 時計回りが表
rsDesc.DepthClipEnable       = TRUE;
```

- **背面カリング (Backface Culling)**: カメラに背を向けた面は描画しない → パフォーマンス向上
- **頂点間の値は線形補間される** (Normal, TexCoord など)
- **ビューポート変換**: クリップ空間 → スクリーンピクセル座標

```cpp
D3D11_VIEWPORT vp = {};
vp.Width    = (float)width;
vp.Height   = (float)height;
vp.MinDepth = 0.0f;
vp.MaxDepth = 1.0f;
context->RSSetViewports(1, &vp);
```

---

### PS — Pixel Shader（ピクセルシェーダー）

各ピクセルの最終色を決める。ライティング計算やテクスチャサンプリングはここ。

```hlsl
Texture2D    DiffuseMap : register(t0);   // テクスチャスロット 0
SamplerState Sampler    : register(s0);   // サンプラースロット 0

cbuffer LightBuffer : register(b0)
{
    float3 LightDir;
    float  Padding;
    float3 LightColor;
    float  Padding2;
};

float4 main(VSOutput IN) : SV_TARGET
{
    float3 normal    = normalize(IN.Normal);
    float  diffuse   = max(dot(normal, -LightDir), 0.0);
    float4 texColor  = DiffuseMap.Sample(Sampler, IN.TexCoord);
    return float4(texColor.rgb * LightColor * diffuse, texColor.a);
}
```

**レジスター番号の対応**:

| HLSL 側         | C++ SetShaderResources 側        |
|----------------|----------------------------------|
| `register(b0)` | `VSSetConstantBuffers(0, 1, &cb)` |
| `register(t0)` | `PSSetShaderResources(0, 1, &srv)` |
| `register(s0)` | `PSSetSamplers(0, 1, &sampler)` |

---

### OM — Output Merger（出力マージャー）

ピクセルシェーダーの結果を最終的にレンダーターゲットに書き込む。深度テストとブレンドを担う。

```cpp
// 深度テスト設定
D3D11_DEPTH_STENCIL_DESC dsDesc = {};
dsDesc.DepthEnable    = TRUE;
dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
dsDesc.DepthFunc      = D3D11_COMPARISON_LESS;   // 近い方が勝つ

// ブレンド設定（アルファブレンド）
D3D11_BLEND_DESC blendDesc = {};
blendDesc.RenderTarget[0].BlendEnable           = TRUE;
blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
```

**深度バッファ**: 各ピクセルの深度値(0.0〜1.0)を保持。新しいピクセルが既存より奠ければ書き込まない。

```cpp
// フレーム開始時にクリア
context->ClearRenderTargetView(rtv, clearColor);
context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
```

---

## Constant Buffer（定数バッファ）の仕組み

CPUからGPUへデータを渡す唯一の手段（頂点バッファ以外）。

```
CPU メモリ (struct)    →    GPU メモリ (cbuffer)
┌──────────────┐           ┌──────────────┐
│ View         │  Map /    │ View         │
│ Projection   │ memcpy /  │ Projection   │  → VS / PS が参照
│ ...          │  Unmap    │ ...          │
└──────────────┘           └──────────────┘
```

**16バイトアライメント必須**: cbuffer のメンバーは 16バイト境界に配置される。

```cpp
// NG: float3 (12bytes) + float (4bytes) でも問題ないが
//     float3 (12bytes) + float4 (16bytes) は float3 が次の境界に押し出される
struct Bad  { float3 a; float4 b; };  // b は 16 バイト境界 → a が詰め物される

// OK: 常に float4 か float4x4 で揃える
struct Good { float3 a; float pad; float4 b; };
```

```cpp
// DX11 での更新
D3D11_MAPPED_SUBRESOURCE mapped;
context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
memcpy(mapped.pData, &data, sizeof(data));
context->Unmap(cb, 0);
```

---

## Sampler（サンプラー）

テクスチャ座標(UV)からピクセル色を取り出す方法を定義する。

| フィルタリング | 意味 |
|--------------|------|
| `POINT`      | 最近傍ピクセルをそのまま使う（ドット絵風）|
| `LINEAR`     | 隣接ピクセルを線形補間（滑らか）|
| `ANISOTROPIC`| 斜め方向も補正（高品質・重い）|

| アドレスモード | UV が 0〜1 を超えたとき |
|--------------|----------------------|
| `WRAP`       | タイリング（繰り返し）|
| `CLAMP`      | 端のピクセルを引き伸ばす |
| `MIRROR`     | 折り返して繰り返す |

---

## フレーム 1 枚の処理フロー（FBZZ Engine）

```
Application::Run() ループ
│
├── BeginFrame()
│   └── OMSetRenderTargets(rtv, dsv)   ← 書き込み先セット
│
├── Clear(color)
│   ├── ClearRenderTargetView          ← 背景色で塗り潰し
│   └── ClearDepthStencilView          ← 深度バッファリセット
│
├── [将来] Submit(DrawCall)
│   ├── PSO を Apply()
│   │   ├── RSSetState
│   │   ├── OMSetBlendState
│   │   └── OMSetDepthStencilState
│   ├── Shader::Bind()
│   │   ├── VSSetShader / PSSetShader
│   │   └── IASetInputLayout
│   ├── VSSetConstantBuffers / PSSetConstantBuffers
│   ├── PSSetShaderResources (テクスチャ)
│   ├── IASetVertexBuffers / IASetIndexBuffer
│   └── DrawIndexed / Draw
│
└── EndFrame()
    └── SwapChain::Present(1, 0)       ← バックバッファを画面に表示
```

---

## よくある罠

| 症状 | 原因 |
|------|------|
| 真っ黒 / 何も描画されない | InputLayout が Shader の入力と一致していない |
| ちらつく / Z-fighting | 深度バッファをクリアし忘れ、または深度フォーマット不足 |
| テクスチャが白い | SRV をバインドし忘れ、または `register(t0)` がずれている |
| 定数バッファの値が化ける | 16バイトアライメント違反、または `Map` 後に `Unmap` 忘れ |
| 裏面が見えない | CullMode が BACK なのに頂点の巻き順が逆 |
| Present が遅い | `SyncInterval=1` は VSync 待ち。`0` にすると垂直同期なし |
