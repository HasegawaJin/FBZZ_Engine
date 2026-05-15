# Renderer / RenderState

描画ステートの抽象。ラスタライザ・ブレンド・デプスステンシルの 3 種をプリセットで管理する。
`PipelineStateDesc` にまとめ、`IRenderer::CreatePipelineState()` で `IPipelineState` リソースとして生成する。

---

## ステート定義

```cpp
namespace fbzz::renderer {

enum class RasterizerMode {
    SOLID,       // 通常の塗りつぶし描画 (デフォルト)
    WIREFRAME,   // ワイヤーフレーム
};

enum class BlendMode {
    OPAQUE,      // 不透明 (デフォルト)
    ALPHA_BLEND, // アルファブレンド (半透明)
    ADDITIVE,    // 加算合成 (パーティクル・エフェクト)
};

enum class DepthMode {
    DEPTH_ON,    // 深度テスト・書き込みあり (デフォルト)
    DEPTH_READ,  // 深度テストあり・書き込みなし (半透明オブジェクト)
    DEPTH_OFF,   // 深度テスト・書き込みなし (デバッグ描画, UI)
};

struct PipelineStateDesc {
    RasterizerMode rasterizer = RasterizerMode::SOLID;
    BlendMode      blend      = BlendMode::OPAQUE;
    DepthMode      depth      = DepthMode::DEPTH_ON;
};

} // namespace fbzz::renderer
```

---

## なぜ IPipelineState を使うか

DX11 はラスタライザ・ブレンド・デプスステートを `Set` 系メソッドで個別に切り替えるが、
DX12 ではこれらをシェーダーとまとめて **Pipeline State Object (PSO)** として事前コンパイルする。

`IRenderer::SetRasterizerMode()` 等の個別 Set API を持つと DX12 で PSO の動的生成が必要になり
パフォーマンスと設計の両面で問題になる。`IPipelineState` をリソースとして扱うことで
DX11/DX12 双方が自然に実装できる。

---

## DX11 実装方針

`DX11Renderer::CreatePipelineState()` 内で 3 種のステートオブジェクトを事前生成し、
`DX11PipelineState` にまとめて保持する。`Submit()` 時に個別に `Set` する。

```cpp
// DX11PipelineState 内部
Microsoft::WRL::ComPtr<ID3D11RasterizerState>  rsState;
Microsoft::WRL::ComPtr<ID3D11BlendState>        blendState;
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> dsState;
```

---

## DX12 実装方針

`DX12Renderer::CreatePipelineState()` 内でシェーダーバイトコードと合わせて
`ID3D12PipelineState` をコンパイルする。`Submit()` 時は `SetPipelineState` 1 回のみ。

---

## 使用例

```cpp
// 初期化時にプリセットを生成
auto psoOpaque = renderer.CreatePipelineState({
    RasterizerMode::SOLID,
    BlendMode::OPAQUE,
    DepthMode::DEPTH_ON
});

auto psoTransparent = renderer.CreatePipelineState({
    RasterizerMode::SOLID,
    BlendMode::ALPHA_BLEND,
    DepthMode::DEPTH_READ
});

auto psoDebug = renderer.CreatePipelineState({
    RasterizerMode::WIREFRAME,
    BlendMode::OPAQUE,
    DepthMode::DEPTH_OFF
});

// 描画時は DrawCall に渡すだけ
DrawCall opaqueCall;
opaqueCall.pipelineState = psoOpaque;

DrawCall transparentCall;
transparentCall.pipelineState = psoTransparent;
```

---

## ファイル構成

```
engine/
└── include/engine/
    └── Renderer/
        └── PipelineStateDesc.hpp   (enum + struct 定義のみ。ヘッダオンリー)
```

---

## 参考ドキュメント

- [ID3D11RasterizerState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11rasterizerstate) — ポリゴン塗りつぶし・カリング設定
- [ID3D11BlendState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11blendstate) — アルファブレンド設定
- [ID3D11DepthStencilState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11depthstencilstate) — 深度テスト・書き込み設定
- [ID3D12PipelineState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d12/nn-d3d12-id3d12pipelinestate) — DX12 PSO インターフェース
- [D3D12_GRAPHICS_PIPELINE_STATE_DESC](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d12/ns-d3d12-d3d12_graphics_pipeline_state_desc) — DX12 PSO 設定構造体
