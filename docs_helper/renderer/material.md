# Renderer / Material

マテリアルは描画に必要なシェーダー・テクスチャ・定数・パイプラインステートをまとめたデータ。
`DrawCall` に展開して `IRenderer::Submit()` に渡す。

---

## MaterialConstants

HLSL の b2 スロット (MaterialConstants) に対応する CPU 側構造体。

```cpp
namespace fbzz::renderer {

struct MaterialConstants {
    math::Vector4 albedo     = { 1.0f, 1.0f, 1.0f, 1.0f };
    float         metallic   = 0.0f;
    float         roughness  = 0.5f;
    float         useTexture = 0.0f;   // 0=無効, 1=有効 (bool を HLSL に渡すと型問題)
    float         _pad       = 0.0f;
};

} // namespace fbzz::renderer
```

---

## Material

```cpp
namespace fbzz::renderer {

class Material {
public:
    std::shared_ptr<IShader>        shader;
    std::shared_ptr<IPipelineState> pipelineState;
    std::shared_ptr<ITexture>       albedoTexture;   // nullptr = テクスチャなし

    MaterialConstants constants;

    // IRenderer から事前に生成しておく定数バッファ (b2 スロット)
    std::shared_ptr<IConstantBuffer> constantBuffer;

    // constants を constantBuffer に書き込む
    void UpdateConstantBuffer();
};

} // namespace fbzz::renderer
```

`Material::UpdateConstantBuffer()` は `constants` の内容を GPU に転送するだけ。
暗黙のステート変更は行わない。

---

## DrawCall

1 回の `IRenderer::Submit()` に渡すデータ。`MeshRenderer` が毎フレーム生成する。
**DrawCall は完全自己完結** — 描画に必要な全リソースを保持し、暗黙のグローバルステートに依存しない。

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

---

## 定数バッファスロット規則

| スロット | 用途 |
|----------|------|
| `b0` | CameraConstants (VP 行列, カメラ位置) |
| `b1` | ObjectConstants (ワールド行列) |
| `b2` | MaterialConstants (色, テクスチャフラグ) |
| `b3` | LightConstants (ライト情報) |

---

## MeshRenderer から DrawCall を生成するフロー

```cpp
// MeshRenderer::OnUpdate() 内
mat->UpdateConstantBuffer();   // MaterialConstants を GPU に転送

DrawCall call;
call.vertexBuffer        = m_vertexBuffer;
call.indexBuffer         = m_indexBuffer;
call.indexCount          = m_indexCount;
call.shader              = mat->shader;
call.pipelineState       = mat->pipelineState;
call.constantBuffers[0]  = m_cameraCB;              // b0: CameraConstants
call.constantBuffers[1]  = m_objectCB;              // b1: ObjectConstants
call.constantBuffers[2]  = mat->constantBuffer;     // b2: MaterialConstants
call.textures[0]         = mat->albedoTexture;      // t0: アルベドテクスチャ

renderer.Submit(call);
```

---

## 将来の拡張 (参考)

現在はシンプルな Phong マテリアルのみ。将来以下を追加できる。

| マテリアル | 説明 |
|-----------|------|
| `PhongMaterial` | 現在の実装 |
| `PBRMaterial` | DX12 移行後に対応 |
| `UnlitMaterial` | UI・デバッグ用 |

DX12 移行まで抽象化は不要。シンプルに `Material` 1 クラスで管理する。

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
