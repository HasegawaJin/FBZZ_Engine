# Engine / MeshRenderer

`fbzz::scene::MeshRenderer`。GameObject にアタッチする描画コンポーネント。
毎フレーム `DrawCall` を生成して `RenderQueue` に積む。

---

## クラス定義

```cpp
namespace fbzz::scene {

class MeshRenderer : public Component {
public:
    void OnAwake()  override;
    void OnUpdate(float dt) override;  // DrawCall を RenderQueue に Submit

    // メッシュ・マテリアル設定
    void SetMesh(std::shared_ptr<mesh::Mesh> mesh);
    void SetMaterial(std::shared_ptr<renderer::Material> material);

    std::shared_ptr<mesh::Mesh>          GetMesh()     const { return m_mesh; }
    std::shared_ptr<renderer::Material>  GetMaterial() const { return m_material; }

    renderer::RenderLayer m_layer = renderer::RenderLayer::OPAQUE;

private:
    std::shared_ptr<mesh::Mesh>          m_mesh;
    std::shared_ptr<renderer::Material>  m_material;
    std::shared_ptr<renderer::IBuffer>   m_vertexBuffer;
    std::shared_ptr<renderer::IBuffer>   m_indexBuffer;
    uint32_t                             m_indexCount = 0;
};

} // namespace fbzz::scene
```

---

## DrawCall 生成フロー

```cpp
// MeshRenderer::OnUpdate() 内
renderer::DrawCall call;
call.m_vertexBuffer = m_vertexBuffer;
call.m_indexBuffer  = m_indexBuffer;
call.m_indexCount   = m_indexCount;
call.m_material     = m_material;
call.m_worldMatrix  = GetOwner().GetTransform().GetWorldMatrix();
call.m_layer        = m_layer;

// Scene から渡された RenderQueue に積む
// (Scene::Render() が呼び出しを管理する)
```

`m_vertexBuffer` / `m_indexBuffer` は `SetMesh()` 時に `ResourceManager` 経由で生成する。

---

## 使用例

```cpp
auto& cube = m_scene->CreateObject("Cube");
auto& mr = cube.AddComponent<MeshRenderer>();
mr.SetMesh(resourceManager.LoadMesh("assets/cube.fbx"));
mr.SetMaterial(material);
mr.m_layer = renderer::RenderLayer::OPAQUE;
```

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Scene/
│       └── MeshRenderer.hpp
└── src/
    └── Scene/
        └── MeshRenderer.cpp
```

---

## 参考ドキュメント

- [DrawCall / Material](../renderer/material.md)
- [RenderQueue / RenderLayer](../renderer/render_queue.md)
- [ResourceManager](resource_manager.md)
