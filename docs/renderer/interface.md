# Renderer / Interface

レンダラーの抽象インターフェース群。上位レイヤーはこれらのみを参照し、DX11/DX12 の具体実装に直接依存しない。

---

## クラス階層

```
IRenderer               描画フレームの制御・リソース生成・描画命令
└── DX11Renderer

IBuffer                 頂点・インデックスバッファ
└── DX11Buffer

IShader                 頂点・ピクセルシェーダーのペア
└── DX11Shader

ITexture                2D テクスチャ
└── DX11Texture
```

---

## IRenderer

```cpp
namespace fbzz::renderer {

class IRenderer {
public:
    virtual ~IRenderer() = default;

    // フレーム制御
    virtual void BeginFrame() = 0;
    virtual void EndFrame()   = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    // リソース生成
    // stride: 頂点 1 つのバイト数 (例: sizeof(Vertex))
    virtual std::shared_ptr<IBuffer>  CreateVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) = 0;
    virtual std::shared_ptr<IBuffer>  CreateIndexBuffer(const void* data, uint32_t count) = 0;
    virtual std::shared_ptr<IShader>  CreateShader(const std::string& path) = 0;
    virtual std::shared_ptr<ITexture> CreateTexture(const std::string& path) = 0;

    // 描画
    virtual void Submit(const DrawCall& call) = 0;

    // ウィンドウリサイズ (Window::ResizeCallback から呼ぶ)
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    // 描画ステート切り替え (詳細: render_state.md)
    virtual void SetRasterizerMode(RasterizerMode mode) = 0;
    virtual void SetBlendMode(BlendMode mode)           = 0;
    virtual void SetDepthMode(DepthMode mode)           = 0;

    // オフスクリーン RT (詳細: render_target.md)
    virtual std::shared_ptr<IRenderTarget> CreateRenderTarget(uint32_t width, uint32_t height) = 0;
    virtual void SetRenderTarget(std::shared_ptr<IRenderTarget> rt) = 0;   // nullptr = バックバッファ

    // サンプラー (詳細: sampler.md)
    virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;

    // デバッグ描画 (物理コライダーの可視化など)
    virtual void DrawLine(const math::Vector3& from, const math::Vector3& to,
                          const math::Vector4& color) = 0;
    virtual void DrawSphere(const math::Vector3& center, float radius,
                             const math::Vector4& color) = 0;
    virtual void DrawAABB(const math::Vector3& min, const math::Vector3& max,
                          const math::Vector4& color) = 0;
};

} // namespace fbzz::renderer
```

---

## IBuffer

```cpp
namespace fbzz::renderer {

class IBuffer {
public:
    virtual ~IBuffer() = default;

    // CPU 側からデータを再書き込み (動的バッファ用)
    virtual void Update(const void* data, size_t sizeBytes) = 0;

    virtual size_t   GetSize()   const = 0;
    // 頂点バッファのみ有効。インデックスバッファでは 0 を返す。
    // CreateVertexBuffer に渡した stride がそのまま返る。
    virtual uint32_t GetStride() const = 0;
};

} // namespace fbzz::renderer
```

---

## IShader

```cpp
namespace fbzz::renderer {

class IShader {
public:
    virtual ~IShader() = default;

    // 定数バッファへの書き込み
    virtual void SetConstantBuffer(uint32_t slot, const void* data, size_t sizeBytes) = 0;

    // テクスチャスロットへのバインド
    virtual void SetTexture(uint32_t slot, std::shared_ptr<ITexture> texture) = 0;
};

} // namespace fbzz::renderer
```

---

## ITexture

```cpp
namespace fbzz::renderer {

class ITexture {
public:
    virtual ~ITexture() = default;

    virtual uint32_t GetWidth()  const = 0;
    virtual uint32_t GetHeight() const = 0;
};

} // namespace fbzz::renderer
```

---

## 設計方針

- 上位レイヤー (`Scene`, `MeshRenderer` 等) は `IRenderer&` を受け取り描画する。`DX11Renderer*` にキャストしない。
- リソースは `std::shared_ptr<IBuffer>` 等で所有する。`IRenderer` が解放の責任を持つ。
- `Application` が `IRenderer` の実装を生成し、`Scene` / `Component` に渡す。

---

## ファイル構成

```
engine/
└── include/engine/
    └── Renderer/
        ├── IRenderer.hpp
        ├── IBuffer.hpp
        ├── IShader.hpp
        └── ITexture.hpp
```
