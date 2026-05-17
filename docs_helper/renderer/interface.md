# Renderer / Interface

レンダラーの抽象インターフェース群。上位レイヤーはこれらのみを参照し、DX11/DX12 の具体実装に直接依存しない。

---

## クラス階層

```
IRenderer               フレーム制御・リソース生成・描画コマンド発行
├── DX11Renderer
└── DX12Renderer

IBuffer                 頂点・インデックスバッファ
├── DX11Buffer
└── DX12Buffer

IConstantBuffer         定数バッファ (DX12 では GPU-visible ヒープ上)
├── DX11ConstantBuffer
└── DX12ConstantBuffer

IShader                 VS/PS シェーダーペア + 入力レイアウト定義
├── DX11Shader
└── DX12Shader

ITexture                2D テクスチャ
├── DX11Texture
└── DX12Texture

IPipelineState          ラスタライザ・ブレンド・デプス設定の束
├── DX11PipelineState   (3 つの State Object に分解して適用)
└── DX12PipelineState   (PSO として事前ベイク)

IRenderTarget           オフスクリーン描画ターゲット
├── DX11RenderTarget
└── DX12RenderTarget
```

---

## なぜ IPipelineState を導入するか

DX11 はラスタライザ・ブレンド・デプスステートを個別に `Set` するが、
DX12 はこれらをシェーダーと合わせて **Pipeline State Object (PSO)** として事前コンパイルする。

個別 `SetRasterizerMode()` / `SetBlendMode()` / `SetDepthMode()` を IRenderer に持たせると、
DX12 実装が「DrawCall ごとに PSO を検索 or 生成する」という回避コードを強いられる。

`IPipelineState` をリソースとして扱うことで:
- DX11: `CreatePipelineState()` 内で 3 つの State Object を生成、Draw 時に個別 `Set`
- DX12: `CreatePipelineState()` 内で PSO をコンパイル、Draw 時に `SetPipelineState` 1 回

---

## なぜ IConstantBuffer を独立させるか

DX11 は `UpdateSubresource` で CPU → GPU にコピーするだけだが、
DX12 は定数バッファを **GPU-visible なデスクリプタヒープ** 上に配置し、
ルートシグネチャ経由でバインドする。

`IShader::SetConstantBuffer(slot, rawPtr, size)` のような生ポインタ渡しでは
DX12 の「GPU リソースとして事前確保しておく」モデルと相性が悪い。

`IConstantBuffer` をリソースとして独立させることで:
- DX11: `Map/Unmap` or `UpdateSubresource`
- DX12: Upload ヒープへの書き込み + デスクリプタヒープへの登録

---

## DrawCall (完全自己完結)

```cpp
namespace fbzz::renderer {

struct DrawCall {
    std::shared_ptr<IBuffer>        vertexBuffer;
    std::shared_ptr<IBuffer>        indexBuffer;      // nullptr = 非インデックス描画
    std::shared_ptr<IShader>        shader;
    std::shared_ptr<IPipelineState> pipelineState;

    // スロット 0〜3 の定数バッファ
    std::array<std::shared_ptr<IConstantBuffer>, 4> constantBuffers = {};
    // スロット 0〜7 のテクスチャ
    std::array<std::shared_ptr<ITexture>, 8>        textures        = {};

    uint32_t indexCount  = 0;
    uint32_t vertexCount = 0;
    uint32_t startIndex  = 0;
    uint32_t baseVertex  = 0;

    RenderLayer layer = RenderLayer::OPAQUE;  // 描画順制御 (render_queue.md 参照)
};

} // namespace fbzz::renderer
```

DrawCall はすべての描画情報を自己完結させる。暗黙のグローバルステートに依存しない。
これにより DX12 のコマンドリスト並列記録にも将来対応できる。

---

## PipelineStateDesc

```cpp
namespace fbzz::renderer {

struct PipelineStateDesc {
    RasterizerMode rasterizer = RasterizerMode::SOLID;
    BlendMode      blend      = BlendMode::OPAQUE;
    DepthMode      depth      = DepthMode::DEPTH_ON;
};

} // namespace fbzz::renderer
```

DX12 では `IShader` も PSO に含まれるが、シェーダー差し替えの柔軟性を保つため
`IPipelineState` と `IShader` は別リソースとして保持し、`DrawCall` で組み合わせる。

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
    virtual std::shared_ptr<IBuffer>        CreateVertexBuffer(const void* data, size_t sizeBytes, uint32_t stride) = 0;
    virtual std::shared_ptr<IBuffer>        CreateIndexBuffer(const void* data, uint32_t count) = 0;
    virtual std::shared_ptr<IConstantBuffer> CreateConstantBuffer(size_t sizeBytes) = 0;
    virtual std::shared_ptr<IShader>        CreateShader(const std::string& path) = 0;
    virtual std::shared_ptr<ITexture>       CreateTexture(const std::string& path) = 0;
    virtual std::shared_ptr<IPipelineState> CreatePipelineState(const PipelineStateDesc& desc) = 0;

    // 描画 (DrawCall は完全自己完結)
    virtual void Submit(const DrawCall& call) = 0;

    // ウィンドウリサイズ (Window::ResizeCallback から呼ぶ)
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    // オフスクリーン RT (詳細: render_target.md)
    virtual std::shared_ptr<IRenderTarget> CreateRenderTarget(uint32_t width, uint32_t height) = 0;
    virtual void SetRenderTarget(std::shared_ptr<IRenderTarget> rt) = 0;  // nullptr = バックバッファ

    // サンプラー (詳細: sampler.md)
    virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;
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
    virtual uint32_t GetStride() const = 0;
};

} // namespace fbzz::renderer
```

---

## IConstantBuffer

```cpp
namespace fbzz::renderer {

class IConstantBuffer {
public:
    virtual ~IConstantBuffer() = default;

    // CPU からデータを書き込む
    // DX11: UpdateSubresource / Map+Unmap
    // DX12: Upload ヒープへの memcpy
    virtual void Update(const void* data, size_t sizeBytes) = 0;

    virtual size_t GetSize() const = 0;
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

    // シェーダーのメタ情報
    virtual const std::string& GetPath() const = 0;
};

} // namespace fbzz::renderer
```

定数バッファ・テクスチャのバインドは DrawCall が持つ。IShader は
シェーダーバイトコードと入力レイアウトのみを管理する。

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

## IPipelineState

```cpp
namespace fbzz::renderer {

class IPipelineState {
public:
    virtual ~IPipelineState() = default;

    virtual const PipelineStateDesc& GetDesc() const = 0;
};

} // namespace fbzz::renderer
```

---

## DX11 / DX12 実装の対応表

| インターフェース | DX11 実装 | DX12 実装 |
|----------------|-----------|-----------|
| `IBuffer` | `ID3D11Buffer` (頂点/インデックス) | `ID3D12Resource` (デフォルトヒープ) |
| `IConstantBuffer` | `ID3D11Buffer` + `UpdateSubresource` | `ID3D12Resource` (アップロードヒープ) + CBV デスクリプタ |
| `IShader` | VS + PS + `InputLayout` | DXBC/DXIL バイトコード (PSO に渡す) |
| `IPipelineState` | RS + BS + DS (個別 Set) | `ID3D12PipelineState` (事前ベイク) |
| `ITexture` | `ID3D11ShaderResourceView` | SRV デスクリプタ |
| `IRenderTarget` | `ID3D11RenderTargetView` | `ID3D12Resource` + RTV デスクリプタ |
| `BeginFrame` | — | コマンドアロケータ Reset |
| `EndFrame` | `Present` | `ExecuteCommandLists` + `Present` |

---

## 設計方針

- 上位レイヤー (`Scene`, `MeshRenderer` 等) は `IRenderer&` のみ参照する。具体実装にキャストしない。
- リソースは `std::shared_ptr<IBuffer>` 等で所有。`IRenderer` が解放の責任を持つ。
- `DrawCall` は完全自己完結。暗黙のグローバルステートに依存しない。
- `Application` が `IRenderer` の実装を生成し `Scene` / `Component` に渡す。

---

## ファイル構成

```
engine/
└── include/engine/
    └── Renderer/
        ├── IRenderer.hpp
        ├── IBuffer.hpp
        ├── IConstantBuffer.hpp
        ├── IShader.hpp
        ├── ITexture.hpp
        ├── IPipelineState.hpp
        ├── IRenderTarget.hpp
        ├── DrawCall.hpp
        └── PipelineStateDesc.hpp
```
