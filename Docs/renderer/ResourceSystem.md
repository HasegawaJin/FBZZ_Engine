# ResourceSystem 設計書 — Handle ベースリソース管理

`fbzz::renderer` の `shared_ptr` 依存を排除し、ResourceManager による一元管理に移行する。

---

## 背景・目的

現状の `DrawCall` は `shared_ptr` を最大 25 個保持する。コピーのたびにアトミックな参照カウント操作が発生し、Submit 時には即座に `.get()` で生ポインタを取り出している。`shared_ptr` が守っているにもかかわらず生ポインタで使うという矛盾した構造を解消する。

| 指標 | Before (shared_ptr) | After (Handle) |
|------|---------------------|----------------|
| DrawCall サイズ | ≈ 400 byte | ≈ 100 byte |
| DrawCall コピーコスト | アトミック操作 ×25 | 整数コピーのみ |
| 所有権の所在 | 分散（複数の shared_ptr が共有） | ResourceManager に一元化 |
| use-after-free 検知 | コンパイル時のみ | 世代カウンタでランタイム検知 |

---

## 実装完了の定義

以下をすべて満たした時点で本タスク完了とする。

- [x] `ResourceHandle<Tag>` が世代付きで型安全に動作する
- [x] `ResourcePool<T>` が Insert / Get / Remove を正しく行い、古い Handle の Get が nullptr を返す
- [x] `ResourceManager` が Shader / Texture / Buffer / RenderTarget を一元管理する
- [x] `ShaderManager` の静的キャッシュを `ResourceManager` に統合し、`ShaderManager` を削除する
- [x] `DrawCall` / `ComputeCall` から `shared_ptr` が消え、Handle のみになる
- [x] `MeshRenderer` / `Material` / `Mesh` が保持する `shared_ptr<IXxx>` を Handle に置換する
- [x] `IRenderer::Create*()` を `private` の `CreateNative*()` に変更し、`ResourceManager` を `friend` にすることで外部からの直接呼び出しを禁止する
- [x] `RenderSystem` が `ResourceManager` を受け取り、Handle 経由でリソースを参照する
- [x] ビルドが通り、エディターでメッシュ・マテリアル・シャドウが正常に描画される
- [x] `Docs/renderer/Design.md` の `IRenderer` / `DrawCall` インターフェース記述を更新する

---

## 設計

### 1. ResourceHandle

```cpp
// Engine/include/Engine/Renderer/ResourceHandle.hpp

namespace fbzz::renderer {

template<typename Tag>
struct ResourceHandle {
    uint32_t id  : 20;  // スロットインデックス（最大 ~100 万件）
    uint32_t gen : 12;  // 世代カウンタ（削除後の再利用スロットを区別）

    bool IsValid() const { return id != 0; }
    bool operator==(const ResourceHandle&) const = default;

    static ResourceHandle Null() { return {0, 0}; }
};

// タグ型で各リソース種別を区別する
struct TextureTag       {};
struct ShaderTag        {};
struct BufferTag        {};
struct PipelineStateTag {};
struct RenderTargetTag  {};

using TextureHandle       = ResourceHandle<TextureTag>;
using ShaderHandle        = ResourceHandle<ShaderTag>;
using BufferHandle        = ResourceHandle<BufferTag>;
using PipelineStateHandle = ResourceHandle<PipelineStateTag>;
using RenderTargetHandle  = ResourceHandle<RenderTargetTag>;

} // namespace fbzz::renderer
```

**世代カウンタの動作例:**

```
1. Insert → スロット3 に配置、gen=1 → Handle{id=3, gen=1} を返す
2. Remove(Handle{3,1}) → スロット3 を解放、スロットの gen を 2 にインクリメント
3. Insert → スロット3 を再利用、gen=2 → Handle{id=3, gen=2} を返す
4. 古い Handle{id=3, gen=1} で Get() → スロットの gen(2) ≠ handle.gen(1) → nullptr
```

---

### 2. ResourcePool

```cpp
// Engine/include/Engine/Renderer/ResourcePool.hpp

namespace fbzz::renderer {

template<typename T, typename Tag>
class ResourcePool {
public:
    ResourceHandle<Tag> Insert(std::unique_ptr<T> resource);
    T*                  Get(ResourceHandle<Tag> handle);       // 無効なら nullptr
    void                Remove(ResourceHandle<Tag> handle);

private:
    struct Slot {
        std::unique_ptr<T> resource;
        uint32_t           gen      = 1;  // 0 は Null Handle 予約
        bool               occupied = false;
    };

    std::vector<Slot>     m_slots;
    std::vector<uint32_t> m_freeList;
};

} // namespace fbzz::renderer
```

**メモリレイアウト（スロット配列）:**

```
index: [0 reserved] [1]      [2]      [3]      [4]
                    Shader   Texture  <free>   Buffer
                    gen=1    gen=2    gen=3    gen=1
                                       ↑
                                  freeList に追加
```

スロット 0 は Null Handle 予約（id=0 は常に無効）。  
フリーリストにより削除済みスロットを O(1) で再利用する。

---

### 3. ResourceManager

```cpp
// Engine/include/Engine/Renderer/ResourceManager.hpp

namespace fbzz::renderer {

class ResourceManager {
public:
    explicit ResourceManager(IRenderer& renderer);

    // --- ロード（キャッシュ済みなら同じ Handle を返す） ---
    ShaderHandle  LoadShader (std::string_view path);
    TextureHandle LoadTexture(std::string_view path);

    // --- 動的生成 ---
    BufferHandle        CreateVertexBuffer  (const void* data, size_t bytes, uint32_t stride);
    BufferHandle        CreateIndexBuffer   (const void* data, uint32_t count);
    BufferHandle        CreateConstantBuffer(size_t sizeBytes);
    PipelineStateHandle CreatePipelineState (const PipelineStateDesc& desc);
    RenderTargetHandle  CreateRenderTarget  (uint32_t w, uint32_t h, uint32_t colorCount = 1);
    TextureHandle       CreateComputeTexture(uint32_t w, uint32_t h);

    // --- 生ポインタ貸し出し（フレーム内のみ有効と見なす） ---
    IShader*       Get(ShaderHandle h);
    ITexture*      Get(TextureHandle h);
    IBuffer*       Get(BufferHandle h);
    IPipelineState* Get(PipelineStateHandle h);
    IRenderTarget*  Get(RenderTargetHandle h);

    // RT のカラー/深度テクスチャを Handle で取得
    TextureHandle GetColorTexture(RenderTargetHandle rt, uint32_t index = 0);
    TextureHandle GetDepthTexture(RenderTargetHandle rt);

    // --- 解放 ---
    void Release(ShaderHandle h);
    void Release(TextureHandle h);
    void Release(BufferHandle h);
    void Release(PipelineStateHandle h);
    void Release(RenderTargetHandle h);

private:
    IRenderer& m_renderer;

    ResourcePool<IShader,       ShaderTag>        m_shaders;
    ResourcePool<ITexture,      TextureTag>        m_textures;
    ResourcePool<IBuffer,       BufferTag>         m_buffers;
    ResourcePool<IPipelineState, PipelineStateTag> m_pipelineStates;
    ResourcePool<IRenderTarget,  RenderTargetTag>  m_renderTargets;

    std::unordered_map<std::string, ShaderHandle>  m_shaderCache;
    std::unordered_map<std::string, TextureHandle> m_textureCache;
};

} // namespace fbzz::renderer
```

**RenderTarget のカラー/深度 Handle:**  
`DX11RenderTarget` が内部で保持する `shared_ptr<ITexture>` は、`ResourceManager::CreateRenderTarget()` 時に TexturePool に登録し、`TextureHandle` として管理する。RT 削除時に対応する Texture Handle も同時に無効化する。

---

### 4. DrawCall / ComputeCall の変化

**Before:**

```cpp
struct DrawCall {
    std::shared_ptr<IBuffer>                        vertexBuffer;
    std::shared_ptr<IBuffer>                        indexBuffer;
    std::shared_ptr<IShader>                        shader;
    std::shared_ptr<IPipelineState>                 pipelineState;
    std::array<std::shared_ptr<IConstantBuffer>, 8> constantBuffers = {};
    std::array<std::shared_ptr<ITexture>, 16>       textures        = {};
    // sizeof ≈ 400 byte
};
```

**After:**

```cpp
struct DrawCall {
    BufferHandle        vertexBuffer;
    BufferHandle        indexBuffer;
    ShaderHandle        shader;
    PipelineStateHandle pipelineState;
    std::array<BufferHandle,  8>  constantBuffers = {};
    std::array<TextureHandle, 16> textures        = {};
    // sizeof ≈ 100 byte（整数配列のみ）

    uint32_t      indexCount  = 0;
    uint32_t      vertexCount = 0;
    uint32_t      startIndex  = 0;
    uint32_t      baseVertex  = 0;
    RenderLayer   layer       = RenderLayer::OPAQUE;
    PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
};

struct ComputeCall {
    ShaderHandle                        shader;
    std::array<BufferHandle,  8>        constantBuffers = {};
    std::array<TextureHandle, 16>       srvInputs       = {};
    std::array<TextureHandle, 2>        uavOutputs      = {};
    uint32_t dispatchX = 1, dispatchY = 1, dispatchZ = 1;
};
```

---

### 5. IRenderer と Submit のシグネチャ変更

`IRenderer::Create*()` は Handle を返すように変更する。

```cpp
// 変更前
virtual std::shared_ptr<IBuffer> CreateVertexBuffer(...) = 0;

// 変更後
virtual BufferHandle CreateVertexBuffer(...) = 0;
```

`Submit` / `Dispatch` は `ResourceManager` を受け取る。

```cpp
// 変更前
virtual void Submit(const DrawCall& call) = 0;

// 変更後
virtual void Submit(const DrawCall& call, ResourceManager& rm) = 0;
virtual void Dispatch(const ComputeCall& call, ResourceManager& rm) = 0;
```

`DX11Renderer::Submit` 内部:

```cpp
void DX11Renderer::Submit(const DrawCall& dc, ResourceManager& rm) {
    if (auto* s = rm.Get(dc.shader))
        static_cast<DX11Shader*>(s)->Bind(m_context.Get());

    for (uint32_t i = 0; i < 8; ++i) {
        auto* cb = rm.Get(dc.constantBuffers[i]);
        if (!cb) continue;
        ID3D11Buffer* buf = static_cast<DX11ConstantBuffer*>(cb)->GetBuffer();
        m_context->VSSetConstantBuffers(i, 1, &buf);
        m_context->PSSetConstantBuffers(i, 1, &buf);
    }
    // texture / vertex buffer / draw も同様
}
```

---

### 6. RenderTarget → 次パスへの受け渡し

`IRenderTarget::GetColorTexture()` / `GetDepthTexture()` は廃止し、`ResourceManager` 経由に統一する。

```cpp
// Before（RT から直接 shared_ptr を取得）
dc.textures[8] = shadowMapRT->GetDepthTexture();

// After（ResourceManager から Handle を取得）
dc.textures[8] = rm.GetDepthTexture(shadowMapRT);
```

---

## 移行手順

### Step 1 — ResourceHandle / ResourcePool の実装

- `ResourceHandle.hpp` 新規作成
- `ResourcePool.hpp` / `ResourcePool.cpp` 新規作成
- 単体テスト: Insert → Get → Remove → 古い Handle で Get が nullptr

### Step 2 — ResourceManager の実装と ShaderManager 統合

- `ResourceManager.hpp` / `ResourceManager.cpp` 新規作成
- `ShaderManager` の静的キャッシュを `ResourceManager::m_shaderCache` に移植
- `ShaderManager` クラスを削除

### Step 3 — DrawCall / ComputeCall を Handle に置換

- `DrawCall.hpp` / `ComputeCall` の `shared_ptr` を Handle に変更
- `IRenderer::Submit` / `Dispatch` に `ResourceManager&` を追加
- `DX11Renderer::Submit` / `Dispatch` を Handle → `.get()` → static_cast に書き換え

### Step 4 — IRenderer::Create*() の戻り値を Handle に変更

- `IRenderer` の Create 系を Handle 返しに変更
- `DX11Renderer` の実装を対応させる
- `SetRenderTarget` のシグネチャも `RenderTargetHandle` に変更

### Step 5 — Mesh / Material / MeshRenderer を Handle に置換

- `Mesh::vertexBuffer` / `indexBuffer` を `BufferHandle` に変更
- `Material::shader` / テクスチャ / `paramsBuffer` を各 Handle に変更
- `MeshRenderer::mesh` / `material` は `shared_ptr<Mesh>` / `shared_ptr<Material>` のままでよい（Mesh・Material 自体の管理は別途 AssetManager に委ねる）

### Step 6 — RenderSystem の更新

- `RenderSystem` のシグネチャに `ResourceManager&` を追加
- static リソース（`shadowMapRT`, `frameCB` 等）を `ResourceManager` で生成・管理
- `IRenderTarget::GetColorTexture()` / `GetDepthTexture()` 呼び出しを `rm.GetColorTexture()` / `rm.GetDepthTexture()` に置換

---

## 変更ファイル一覧

| 種別 | ファイル |
|------|---------|
| 新規 | `Engine/include/Engine/Renderer/ResourceHandle.hpp` |
| 新規 | `Engine/include/Engine/Renderer/ResourcePool.hpp` |
| 新規 | `Engine/include/Engine/Renderer/ResourcePool.cpp` |
| 新規 | `Engine/include/Engine/Renderer/ResourceManager.hpp` |
| 新規 | `Engine/src/Renderer/ResourceManager.cpp` |
| 削除 | `Engine/include/Engine/Renderer/ShaderManager.hpp` |
| 削除 | `Engine/src/Renderer/ShaderManager.cpp` |
| 修正 | `Engine/include/Engine/Renderer/DrawCall.hpp` (Handle に置換) |
| 修正 | `Engine/include/Engine/Renderer/IRenderer.hpp` (Create 系・Submit シグネチャ変更) |
| 修正 | `Engine/include/Engine/Renderer/IRenderTarget.hpp` (GetColorTexture 廃止) |
| 修正 | `Engine/include/Engine/Renderer/Mesh.hpp` (BufferHandle に変更) |
| 修正 | `Engine/include/Engine/Renderer/Material.hpp` (各 Handle に変更) |
| 修正 | `Engine/src/Renderer/Platform/DX11/DX11Renderer.cpp` (Submit 実装更新) |
| 修正 | `Engine/src/Renderer/Platform/DX11/DX11RenderTarget.cpp` (Texture Handle 登録) |
| 修正 | `Engine/src/Scene/Systems/RenderSystem.cpp` (ResourceManager 受け取り) |
| 修正 | `Engine/include/Engine/Scene/Systems/RenderSystem.hpp` (シグネチャ拡張) |
| 修正 | `Docs/renderer/Design.md` (IRenderer インターフェース記述を更新) |

---

## Step 7 以降

Handle の整数部分を不透明な ID に昇格させることで、GPU リソースの streaming・ホットリロード・非同期ロードへの対応が可能になる。現時点では設計上の余地として残す。
