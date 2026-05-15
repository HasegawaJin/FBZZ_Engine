# Renderer / Material

マテリアルは描画に必要なシェーダー・テクスチャ・定数をまとめたデータ。`DrawCall` と合わせて使う。

---

## Material

```cpp
namespace fbzz::renderer {

struct MaterialConstants {
    math::Vector4 albedo      = { 1.0f, 1.0f, 1.0f, 1.0f };
    float      metallic    = 0.0f;
    float      roughness   = 0.5f;
    float      useTexture  = 0.0f;   // 0=無効, 1=有効 (HLSL に bool を渡すと型問題)
    float      _pad        = 0.0f;
};

class Material {
public:
    std::shared_ptr<IShader>  m_shader;
    std::shared_ptr<ITexture> m_albedoTexture;
    MaterialConstants         m_constants;

    // シェーダーに定数・テクスチャをバインドする
    void Bind() const;
};

} // namespace fbzz::renderer
```

---

## DrawCall

1 回の `IRenderer::Submit()` に渡すデータ。`MeshRenderer` が毎フレーム生成する。

```cpp
namespace fbzz::renderer {

struct DrawCall {
    std::shared_ptr<IBuffer>   m_vertexBuffer;
    std::shared_ptr<IBuffer>   m_indexBuffer;
    uint32_t                   m_indexCount = 0;
    std::shared_ptr<Material>  m_material;
    math::Matrix4                 m_worldMatrix;
};

} // namespace fbzz::renderer
```

---

## MeshRenderer から DrawCall を生成するフロー

```cpp
// MeshRenderer::OnUpdate() 内
DrawCall call;
call.m_vertexBuffer = m_vertexBuffer;
call.m_indexBuffer  = m_indexBuffer;
call.m_indexCount   = m_indexCount;
call.m_material     = m_material;
call.m_worldMatrix  = m_owner->GetTransform().GetWorldMatrix();
renderer.Submit(call);
```

---

## DX11Renderer::Submit の処理

```cpp
void DX11Renderer::Submit(const DrawCall& call) {
    // 頂点・インデックスバッファをバインド
    // オブジェクト定数バッファ (ワールド行列) を更新
    // マテリアルのシェーダー・テクスチャをバインド
    // DrawIndexed
}
```

---

## 将来の拡張 (参考)

現在はシンプルな Phong マテリアルのみ。将来以下を追加できる。

| マテリアル | 説明 |
|-----------|------|
| `PhongMaterial` | 現在の実装 |
| `PBRMaterial` | DX12 移行後に対応 |
| `UnlitMaterial` | UI・デバッグ用 |

ただし DX12 移行まで抽象化は不要。シンプルに `Material` 1 クラスで管理する。

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Renderer/
│       ├── Material.hpp
│       └── DrawCall.hpp
└── src/
    └── Renderer/
        └── Material.cpp
```
