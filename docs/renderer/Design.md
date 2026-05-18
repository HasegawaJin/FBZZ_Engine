# Renderer System 設計書

`fbzz::renderer` — IRenderer 抽象インターフェースと、Step 5 で追加する Mesh / Material / LightSystem。

---

## 依存関係

```
fbzz::renderer
└── fbzz::math  (Vector3, Vector4, Matrix4, Quaternion)
```

---

## 実装済み (Step 0〜4)

### IRenderer

```cpp
class IRenderer {
public:
    virtual void BeginFrame() = 0;
    virtual void EndFrame()   = 0;
    virtual void Clear(const math::Vector4& color) = 0;

    virtual std::shared_ptr<IBuffer>         CreateVertexBuffer(const void* data, size_t size, uint32_t stride) = 0;
    virtual std::shared_ptr<IBuffer>         CreateIndexBuffer(const void* data, uint32_t count) = 0;
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

## Step 5 で追加 (feature/mesh-renderer)

### 頂点フォーマット

全メッシュ共通。`Mesh.hlsl` / `Unlit.hlsl` の `VSInput` と一致させる。

```cpp
struct Vertex {
    math::Vector3 position;
    math::Vector3 normal;
    math::Vector2 uv;
};
```

`Vertex` は `Mesh.hpp` に定義する。

---

### Mesh

GPU メッシュ。`IBuffer` のラッパー。`AssetManager::Load<Mesh>()` または `PrimitiveMesh` が返す。

```cpp
// Mesh.hpp
struct Mesh {
    std::shared_ptr<IBuffer> vertexBuffer;
    std::shared_ptr<IBuffer> indexBuffer;
    uint32_t vertexCount = 0;
    uint32_t indexCount  = 0;
};
```

`Mesh` は plain struct。生成責務は `PrimitiveMesh` (手続き) または `MeshImporter` (ファイル) が持つ。

---

### PrimitiveMesh

手続き生成メッシュのファクトリ。GPU バッファ生成に `IRenderer&` を使うため renderer モジュールに置く。

```cpp
// PrimitiveMesh.hpp
class PrimitiveMesh {
public:
    static std::shared_ptr<Mesh> Cube  (IRenderer& renderer);
    static std::shared_ptr<Mesh> Sphere(IRenderer& renderer, int segments = 16);
    static std::shared_ptr<Mesh> Plane (IRenderer& renderer);
};
```

#### Cube の頂点定義

6 面 × 4 頂点 = 24 頂点、6 面 × 2 三角形 × 3 = 36 インデックス。  
面ごとに法線が異なるため頂点を共有しない。

---

### Material

シェーダー + テクスチャ + パラメータのセット。

```cpp
// Material.hpp

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
    std::shared_ptr<IConstantBuffer> paramsBuffer;   // Init() で生成
    MaterialParams                   params;

    void Init(IRenderer& renderer);   // paramsBuffer を生成する
    void Upload();                    // params → GPU 定数バッファへ転送
};
```

`Init()` を分けているのは `shared_ptr<Material>` を生成してから renderer を注入するケースに対応するため。

---

### LightSystem

DirectionalLight のみ。`RenderSystem` が定数バッファ経由でシェーダーへ渡す。

```cpp
// LightSystem.hpp

struct DirectionalLight {
    math::Vector3 direction = {  0.0f, -1.0f,  0.5f };
    float         _pad0     = 0.0f;
    math::Vector3 color     = {  1.0f,  1.0f,  1.0f };
    float         intensity = 1.0f;
};

class LightSystem {
public:
    void SetDirectional(const DirectionalLight& light);
    const DirectionalLight& GetDirectional() const;
    void Upload(IConstantBuffer& cb) const;
private:
    DirectionalLight m_directional;
};
```

---

### Mesh.hlsl — 定数バッファレイアウト

`Unlit.hlsl` の b0/b1/b2 を踏襲し、b3 にライトを追加する。

```hlsl
cbuffer CameraConstants  : register(b0) { float4x4 viewProjection; float3 cameraPos; float _pad; };
cbuffer ObjectConstants  : register(b1) { float4x4 world; };
cbuffer MaterialConstants: register(b2) { float4 albedo; float metallic; float roughness; float2 _pad2; };
cbuffer LightConstants   : register(b3) { float3 lightDir; float _pad3; float3 lightColor; float lightIntensity; };
```

#### ライティングモデル

Lambert 拡散 + Blinn-Phong 鏡面反射の簡易実装。

```hlsl
// 拡散
float NdotL = max(dot(N, -lightDir), 0.0);
float3 diffuse = albedo.rgb * lightColor * lightIntensity * NdotL;

// 鏡面 (Blinn-Phong)
float3 H        = normalize(-lightDir + viewDir);
float  NdotH    = max(dot(N, H), 0.0);
float  shininess = lerp(4.0, 128.0, 1.0 - roughness);
float3 specular  = lightColor * lightIntensity * pow(NdotH, shininess) * (1.0 - roughness);

float3 ambient = albedo.rgb * 0.08;
return float4(ambient + diffuse + specular, albedo.a);
```

---

### RenderSystem

`MeshRenderer` を持つ全 GameObject の DrawCall を発行する。

```cpp
// RenderSystem.hpp
void RenderSystem(Scene& scene,
                  renderer::IRenderer& renderer,
                  const renderer::Camera& camera,
                  const renderer::LightSystem& lights);
```

#### 内部フロー

```
1. PerFrameCB を生成・Upload  (viewProj, cameraPos)
2. LightCB を生成・Upload
3. scene.View<Transform, MeshRenderer>() を走査
   ├─ !mr.enabled または mr.mesh / mr.material が nullptr → スキップ
   ├─ PerObjectCB を Upload (tf.GetWorldMatrix())
   ├─ mr.material->Upload()
   └─ DrawCall を構築して renderer.Submit()
```

PerFrameCB と LightCB は **フレームに 1 回だけ生成**する。  
PerObjectCB はオブジェクトごとに内容を上書きして再利用する。

---

### SceneManager::Update シグネチャ変更

`RenderSystem` は Camera と LightSystem が必要なため、ゲームループから直接呼ぶ。  
`SceneManager::Update` は物理・Transform・Destroy のみ担当する。

**変更前:**
```cpp
void Update(float dt, renderer::IRenderer& renderer, physics::World& world);
```

**変更後:**
```cpp
void Update(float dt, physics::World& world);
```

**ゲームループでの呼び出しイメージ:**
```cpp
sm.Update(dt, physWorld);
renderer.BeginFrame();
renderer.Clear({ 0.05f, 0.08f, 0.15f, 1.0f });
RenderSystem(*sm.GetActive(), renderer, camera, lights);
renderer.EndFrame();
```

---

### 変更ファイル一覧

| 種別 | ファイル |
|------|---------|
| 新規 | `engine/include/engine/Renderer/Mesh.hpp` |
| 新規 | `engine/include/engine/Renderer/PrimitiveMesh.hpp` |
| 新規 | `engine/src/Renderer/PrimitiveMesh.cpp` |
| 新規 | `engine/include/engine/Renderer/Material.hpp` |
| 新規 | `engine/src/Renderer/Material.cpp` |
| 新規 | `engine/include/engine/Renderer/LightSystem.hpp` |
| 新規 | `engine/src/Renderer/LightSystem.cpp` |
| 新規 | `assets/shaders/Mesh.hlsl` |
| 修正 | `engine/src/Scene/Systems/RenderSystem.cpp` (stub → 実装) |
| 修正 | `engine/include/engine/Scene/SceneManager.hpp` (Update シグネチャ) |
| 修正 | `engine/src/Scene/SceneManager.cpp` (Update 実装 + RenderSystem 除去) |
| 修正 | `sandbox/src/main.cpp` (Cube メッシュ + LightSystem + RenderSystem 呼び出し) |

---

## Step 6 以降

DX12 移行時に `DX12Renderer` と `RenderGraph` を新規ファイルとして追加する。  
`IRenderer` インターフェースは原則変更しない。既存 DX11 コードへの影響なし。
