# Renderer System 設計書

`fbzz::renderer` — IRenderer 抽象インターフェースと、Step 5 で追加する Mesh / Material / LightSystem。

---

## 依存関係

```
fbzz::renderer
└── fbzz::math  (Vector3, Vector4, Matrix4, Quaternion)
```

---

## 実装済み (Step 0〜3)

### IRenderer

```cpp
class IRenderer {
public:
    virtual void BeginFrame() = 0;
    virtual void EndFrame()   = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    virtual std::shared_ptr<IBuffer>         CreateVertexBuffer(const void* data, size_t size, uint32_t stride) = 0;
    virtual std::shared_ptr<IBuffer>         CreateIndexBuffer(const void* data, size_t size) = 0;
    virtual std::shared_ptr<IConstantBuffer> CreateConstantBuffer(size_t size) = 0;
    virtual std::shared_ptr<IShader>         CreateShader(const std::string& path) = 0;
    virtual std::shared_ptr<ITexture>        CreateTexture(const std::string& path) = 0;
    virtual std::shared_ptr<IPipelineState>  CreatePipelineState(const PipelineStateDesc&) = 0;
    virtual std::shared_ptr<IRenderTarget>   CreateRenderTarget(uint32_t w, uint32_t h) = 0;

    virtual void Submit(const DrawCall& call) = 0;
    virtual void Resize(uint32_t w, uint32_t h) = 0;
    virtual void SetRenderTarget(std::shared_ptr<IRenderTarget>) = 0;
    virtual void SetSampler(uint32_t slot, SamplerMode mode) = 0;
};
```

### DrawCall

```cpp
struct DrawCall {
    std::shared_ptr<IBuffer>        vertexBuffer;
    std::shared_ptr<IBuffer>        indexBuffer;
    std::shared_ptr<IShader>        shader;
    std::shared_ptr<IPipelineState> pipelineState;

    std::array<std::shared_ptr<IConstantBuffer>, 4> constantBuffers = {};
    std::array<std::shared_ptr<ITexture>, 8>        textures        = {};

    uint32_t    indexCount  = 0;
    uint32_t    vertexCount = 0;
    uint32_t    startIndex  = 0;
    uint32_t    baseVertex  = 0;
    RenderLayer layer       = RenderLayer::OPAQUE;
};
```

### Camera

```cpp
class Camera {
public:
    math::Matrix4 GetViewMatrix()       const;
    math::Matrix4 GetProjectionMatrix() const;
    math::Matrix4 GetViewProjection()   const;
    void LookAt(const math::Vector3& target);

    math::Vector3    m_position = math::Vector3::ZERO;
    math::Quaternion m_rotation = math::Quaternion::Identity();
    float m_fovY   = 60.0f;
    float m_aspect = 16.0f / 9.0f;
    float m_zNear  = 0.1f;
    float m_zFar   = 1000.0f;
};
```

### DebugDraw

```cpp
class DebugDraw {
public:
    static void BeginFrame(IRenderer&, const math::Matrix4& viewProj);
    static void Line(IRenderer&, const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color = {1,1,1,1});
    static void Box(IRenderer&, const math::Vector3& center,
                    const math::Vector3& halfExtents, const math::Vector4& color);
    static void Sphere(IRenderer&, const math::Vector3& center, float radius,
                       const math::Vector4& color);
    static void Flush();
};
```

---

## Step 5 で追加

### Mesh

GPU メッシュ。`IBuffer` のラッパー。`AssetManager::Load<Mesh>()` が返す。

```cpp
struct Mesh {
    std::shared_ptr<IBuffer> vertexBuffer;
    std::shared_ptr<IBuffer> indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
};
```

### Material

シェーダー + テクスチャ + パラメータのセット。

```cpp
struct MaterialParams {
    math::Vector4 albedo    = { 1.0f, 1.0f, 1.0f, 1.0f };
    float         metallic  = 0.0f;
    float         roughness = 0.8f;
    float         _pad[2]   = {};
};

class Material {
public:
    std::shared_ptr<IShader>         shader;
    std::shared_ptr<ITexture>        albedoTexture;  // nullptr = 単色
    std::shared_ptr<IConstantBuffer> paramsBuffer;
    MaterialParams                   params;

    void Upload();  // params → GPU 定数バッファへ転送
};
```

### LightSystem

DirectionalLight のみ。`RenderSystem` が定数バッファ経由でシェーダーへ渡す。

```cpp
struct DirectionalLight {
    math::Vector3 direction = {  0.0f, -1.0f, 0.5f };
    float         _pad0     = 0.0f;
    math::Vector3 color     = {  1.0f,  1.0f, 1.0f };
    float         intensity = 1.0f;
};

class LightSystem {
public:
    void SetDirectional(const DirectionalLight& light);
    void Upload(IConstantBuffer& cb) const;
private:
    DirectionalLight m_directional;
};
```

---

## Step 6 以降

DX12 移行時に `DX12Renderer` と `RenderGraph` を新規ファイルとして追加する。  
`IRenderer` インターフェースは原則変更しない。既存 DX11 コードへの影響なし。
