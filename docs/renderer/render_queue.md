# Renderer / RenderQueue

DrawCall の描画順を管理するキュー。不透明→半透明の正しい順序で描画する。
`Scene::Render()` が DrawCall を投入し、フレーム末尾に一括 Flush する。

---

## RenderLayer

DrawCall に描画レイヤーを付与する。

```cpp
namespace fbzz::renderer {

enum class RenderLayer : uint32_t {
    OPAQUE      = 0,   // 不透明オブジェクト (前→後でソート、Early-Z 効率化)
    TRANSPARENT = 1,   // 半透明オブジェクト (後→前でソート、正しいブレンド)
    OVERLAY     = 2,   // UI・デバッグ描画 (ソートなし、常に最前面)
};

} // namespace fbzz::renderer
```

### DrawCall に RenderLayer を追加 (material.md の変更)

```cpp
struct DrawCall {
    std::shared_ptr<IBuffer>  m_vertexBuffer;
    std::shared_ptr<IBuffer>  m_indexBuffer;
    uint32_t                  m_indexCount = 0;
    std::shared_ptr<Material> m_material;
    math::Matrix4             m_worldMatrix;
    RenderLayer               m_layer = RenderLayer::OPAQUE;   // ← 追加
};
```

---

## RenderQueue

```cpp
namespace fbzz::renderer {

class RenderQueue {
public:
    // MeshRenderer::OnUpdate() から呼ぶ
    void Submit(const DrawCall& call);

    // Scene::Render() の末尾で呼ぶ
    void Flush(IRenderer& renderer, const math::Vector3& cameraPos);

    // フレーム先頭でクリアする
    void Clear();

private:
    std::vector<DrawCall> m_opaqueQueue;
    std::vector<DrawCall> m_transparentQueue;
    std::vector<DrawCall> m_overlayQueue;

    static bool SortOpaque(const DrawCall& a, const DrawCall& b,
                           const math::Vector3& camPos);
    static bool SortTransparent(const DrawCall& a, const DrawCall& b,
                                const math::Vector3& camPos);
};

} // namespace fbzz::renderer
```

---

## Flush の処理順

```
RenderQueue::Flush(renderer, cameraPos)
│
├─ OPAQUE キュー: カメラに近い順でソート (front-to-back)
│     renderer.SetBlendMode(OPAQUE)
│     renderer.SetDepthMode(DEPTH_ON)
│     各 DrawCall を renderer.Submit()
│
├─ TRANSPARENT キュー: カメラから遠い順でソート (back-to-front)
│     renderer.SetBlendMode(ALPHA_BLEND)
│     renderer.SetDepthMode(DEPTH_READ)
│     各 DrawCall を renderer.Submit()
│     renderer.SetBlendMode(OPAQUE)  ← 戻す
│     renderer.SetDepthMode(DEPTH_ON)
│
└─ OVERLAY キュー: ソートなし
      renderer.SetDepthMode(DEPTH_OFF)
      各 DrawCall を renderer.Submit()
      renderer.SetDepthMode(DEPTH_ON)
```

---

## Scene との統合

`Scene` が `RenderQueue` を所有する。`MeshRenderer` は `Scene` の `RenderQueue` に Submit する。

```cpp
// Scene.hpp に追加
renderer::RenderQueue& GetRenderQueue() { return m_renderQueue; }

private:
renderer::RenderQueue m_renderQueue;
```

```cpp
// Scene::Render() の更新
void Scene::Render(IRenderer& renderer) {
    m_renderQueue.Clear();

    // 全 GameObject の MeshRenderer が m_renderQueue.Submit() を呼ぶ
    for (auto& obj : m_objects) {
        obj->Render(renderer);   // MeshRenderer::OnUpdate 相当
    }

    // ソートして一括描画
    auto* cam = GetPrimaryCamera();
    math::Vector3 camPos = cam ? cam->GetCamera().m_position : math::Vector3::ZERO;
    m_renderQueue.Flush(renderer, camPos);
}
```

---

## ソート基準

| レイヤー | ソートキー | 理由 |
|---------|-----------|------|
| OPAQUE | カメラとの距離 (近→遠) | Early-Z でピクセルシェーダーをスキップ |
| TRANSPARENT | カメラとの距離 (遠→近) | 正しいアルファブレンドのため |
| OVERLAY | なし | UI は重ね順を DrawCall の投入順で制御 |

距離は `DrawCall::m_worldMatrix` の平行移動成分とカメラ位置の差分で計算する。

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Renderer/
│       └── RenderQueue.hpp
└── src/
    └── Renderer/
        └── RenderQueue.cpp
```
