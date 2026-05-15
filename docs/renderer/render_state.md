# Renderer / RenderState

描画ステートの抽象。ラスタライザ・ブレンド・デプスステンシルの 3 種をプリセットで管理する。
`IRenderer` のメソッドで切り替え、`DX11Renderer` がステートオブジェクトを内部保持する。

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

} // namespace fbzz::renderer
```

---

## IRenderer への追加メソッド

```cpp
// IRenderer.hpp に追加
virtual void SetRasterizerMode(RasterizerMode mode) = 0;
virtual void SetBlendMode(BlendMode mode)           = 0;
virtual void SetDepthMode(DepthMode mode)           = 0;
```

フレーム先頭のデフォルト状態:
- `RasterizerMode::SOLID`
- `BlendMode::OPAQUE`
- `DepthMode::DEPTH_ON`

---

## DX11 実装方針

`DX11Renderer::Init()` 時に全プリセットのステートオブジェクトを事前生成しておく。
切り替えは `RSSetState` / `OMSetBlendState` / `OMSetDepthStencilState` の呼び出しのみ。

```cpp
// DX11Renderer 内部
Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_rsStates[2];    // SOLID, WIREFRAME
Microsoft::WRL::ComPtr<ID3D11BlendState>        m_blendStates[3]; // OPAQUE, ALPHA, ADDITIVE
Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_dsStates[3];    // ON, READ, OFF
```

---

## 使用例

```cpp
// 通常の不透明メッシュ描画 (BeginFrame 後のデフォルト状態)
renderer.Submit(call);

// デバッグライン描画 (深度テストなし・常に最前面)
renderer.SetDepthMode(DepthMode::DEPTH_OFF);
DebugDraw::Flush();
renderer.SetDepthMode(DepthMode::DEPTH_ON);  // 元に戻す

// 半透明オブジェクト描画
renderer.SetBlendMode(BlendMode::ALPHA_BLEND);
renderer.SetDepthMode(DepthMode::DEPTH_READ);
renderer.Submit(transparentCall);
renderer.SetBlendMode(BlendMode::OPAQUE);
renderer.SetDepthMode(DepthMode::DEPTH_ON);
```

---

## ファイル構成

```
engine/
└── include/engine/
    └── Renderer/
        └── RenderState.hpp   (enum 定義のみ。ヘッダオンリー)
```

---

## 参考ドキュメント

- [ID3D11RasterizerState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11rasterizerstate) — ポリゴン塗りつぶし・カリング設定
- [D3D11_RASTERIZER_DESC](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/ns-d3d11-d3d11_rasterizer_desc) — ラスタライザ設定構造体
- [ID3D11BlendState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11blendstate) — アルファブレンド設定
- [D3D11_BLEND_DESC](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/ns-d3d11-d3d11_blend_desc) — ブレンド設定構造体
- [ID3D11DepthStencilState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11depthstencilstate) — 深度テスト・書き込み設定
- [OMSetDepthStencilState](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetdepthstencilstate) — デプスステンシルステートのバインド
