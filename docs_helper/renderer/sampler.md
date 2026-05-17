# Renderer / Sampler

テクスチャサンプリング方式をプリセットで管理する。
`RenderState` と同様に、`DX11Renderer::Init()` 時に全プリセットを事前生成する。

---

## SamplerMode

```cpp
namespace fbzz::renderer {

enum class SamplerMode {
    WRAP_LINEAR,    // 繰り返しテクスチャ・線形補間 (デフォルト)
    WRAP_POINT,     // 繰り返しテクスチャ・最近傍補間 (ピクセルアート)
    CLAMP_LINEAR,   // 端でクランプ・線形補間 (UI, スカイボックス, レンダーターゲット参照)
};

} // namespace fbzz::renderer
```

---

## IRenderer への追加メソッド

```cpp
// IRenderer.hpp に追加
// slot: HLSL の SamplerState レジスタ番号 (s0, s1 ...)
virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;
```

---

## DX11 実装

`DX11Renderer` が `ID3D11SamplerState` を 3 種類事前生成して保持する。

```cpp
// DX11Renderer 内部
Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplers[3];  // WRAP_LINEAR, WRAP_POINT, CLAMP_LINEAR
```

HLSL 側の対応:

```hlsl
// SamplerState の宣言 (Phong.hlsl 等)
SamplerState gSamplerWrap  : register(s0);
SamplerState gSamplerClamp : register(s1);

float4 PSMain(PSInput input) : SV_TARGET {
    float4 albedo = gTexture.Sample(gSamplerWrap, input.uv);
    return albedo;
}
```

---

## 使用例

```cpp
// フレーム開始時にサンプラーをセット (通常は BeginFrame 後に一度だけ)
renderer.SetSampler(0, SamplerMode::WRAP_LINEAR);   // s0: 通常テクスチャ
renderer.SetSampler(1, SamplerMode::CLAMP_LINEAR);  // s1: RT 参照・UI

// テクスチャ自体は DrawCall に渡す (shader->SetTexture は使わない)
DrawCall call;
call.textures[0] = mat->albedoTexture;   // t0 にバインド
renderer.Submit(call);
```

---

## ファイル構成

```
engine/
└── include/engine/
    └── Renderer/
        └── Sampler.hpp    (SamplerMode enum のみ。ヘッダオンリー)
```

---

## 参考ドキュメント

- [D3D11_SAMPLER_DESC](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/ns-d3d11-d3d11_sampler_desc) — サンプラー設定構造体 (Filter / AddressU/V/W)
- [ID3D11SamplerState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11samplerstate) — サンプラーステートインターフェース
- [PSSetSamplers](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-pssetsamplers) — ピクセルシェーダーへのサンプラーバインド
