# Renderer / RenderTarget

オフスクリーンレンダリング用の抽象インターフェースと、ウィンドウリサイズへの対応。

---

## IRenderTarget

```cpp
namespace fbzz::renderer {

class IRenderTarget {
public:
    virtual ~IRenderTarget() = default;

    virtual uint32_t GetWidth()  const = 0;
    virtual uint32_t GetHeight() const = 0;

    // カラーバッファをシェーダーのテクスチャとして渡す用
    virtual std::shared_ptr<ITexture> GetColorTexture() const = 0;
};

} // namespace fbzz::renderer
```

---

## IRenderer への追加メソッド

```cpp
// IRenderer.hpp に追加

// ウィンドウリサイズ時に呼ぶ。バックバッファを再生成する
virtual void Resize(uint32_t width, uint32_t height) = 0;

// オフスクリーン RT を生成する (Step 3 以降)
virtual std::shared_ptr<IRenderTarget> CreateRenderTarget(uint32_t width, uint32_t height) = 0;

// nullptr を渡すとバックバッファに戻す
virtual void SetRenderTarget(std::shared_ptr<IRenderTarget> rt) = 0;
```

---

## Resize の内部処理 (DX11)

```
DX11Renderer::Resize(w, h)
│
├─ m_renderTargetView.Reset()        既存 RTV を解放
├─ m_depthStencilView.Reset()        既存 DSV を解放
├─ m_depthStencilBuffer.Reset()      既存深度バッファを解放
├─ m_swapChain->ResizeBuffers(...)   スワップチェーンのバッファをリサイズ
├─ バックバッファから RTV を再生成
├─ 新サイズで深度バッファ・DSV を再生成
└─ ビューポートを新サイズで更新
```

RTV を解放してから `ResizeBuffers()` を呼ぶ順序が重要。逆にすると失敗する。

---

## Window リサイズとの連携

`Application::Init()` 内で `ResizeCallback` を登録する。

```cpp
m_window->SetResizeCallback([this](uint32_t w, uint32_t h) {
    m_renderer->Resize(w, h);
});
```

---

## 使用例 (Step 3 以降)

```cpp
// シャドウマップをオフスクリーン RT に描画
auto shadowMap = m_renderer->CreateRenderTarget(1024, 1024);

m_renderer->SetRenderTarget(shadowMap);
m_renderer->Clear(math::Vector4::WHITE);
// シャドウパス描画 ...

// バックバッファに戻してメインパス
m_renderer->SetRenderTarget(nullptr);
m_renderer->Clear(math::Vector4::BLACK);
// メインシーン描画 + shadowMap をテクスチャとしてバインド
```

---

## ファイル構成

```
engine/
└── include/engine/
    └── Renderer/
        └── IRenderTarget.hpp
```

---

## 参考ドキュメント

- [IDXGISwapChain::ResizeBuffers](https://learn.microsoft.com/ja-jp/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers) — スワップチェーンのリサイズ (解放順序に注意)
- [ID3D11Texture2D](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nn-d3d11-id3d11texture2d) — レンダーターゲット用テクスチャ
- [CreateRenderTargetView](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nf-d3d11-id3d11device-createrendertargetview) — RTV の生成
- [OMSetRenderTargets](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargets) — RT のバインド
- [D3D11_VIEWPORT](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/ns-d3d11-d3d11_viewport) — ビューポート設定構造体
