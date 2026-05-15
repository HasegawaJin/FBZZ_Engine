# Renderer / Light

ライティングデータの定義と管理。Step 2〜3 の範囲では Directional Light + Ambient Light のみ実装する。

---

## データ定義

```cpp
namespace fbzz::renderer {

// HLSL の cbuffer と 16 byte アライメントを合わせること
struct DirectionalLight {
    math::Vector3 direction = { 0.0f, -1.0f, 0.0f };  // ワールド空間・正規化済み
    float         intensity = 1.0f;
    math::Vector3 color     = { 1.0f, 1.0f, 1.0f };
    float         _pad      = 0.0f;
};

struct AmbientLight {
    math::Vector3 color = { 0.1f, 0.1f, 0.1f };
    float         _pad  = 0.0f;
};

// shader.md の b3 スロットに対応
struct LightConstants {
    DirectionalLight directional;
    AmbientLight     ambient;
};

} // namespace fbzz::renderer
```

---

## LightSystem

`Scene` が 1 つ所有する。`Scene::Render()` でシェーダーの `b3` スロットに転送する。

```cpp
namespace fbzz::renderer {

class LightSystem {
public:
    LightConstants BuildConstants() const;

    DirectionalLight m_directional;
    AmbientLight     m_ambient;
};

} // namespace fbzz::renderer
```

### Scene::Render() での使用

```cpp
void Scene::Render(IRenderer& renderer) {
    // 1. カメラ定数バッファ (b0)
    if (m_primaryCamera) {
        CameraConstants cb = BuildCameraConstants(*m_primaryCamera);
        m_cameraShader->SetConstantBuffer(0, &cb, sizeof(cb));
    }

    // 2. ライト定数バッファ (b3)
    LightConstants lc = m_lightSystem.BuildConstants();
    // 全シェーダーに broadcast するか、Submit 時に各シェーダーへ設定する

    // 3. DrawCall を RenderQueue に投入して描画
    m_renderQueue.Flush(renderer);
}
```

---

## HLSL 側の定義

```hlsl
// b3 スロット (shader.md 参照)
cbuffer LightConstants : register(b3) {
    float3 gDirLightDir;
    float  gDirLightIntensity;
    float3 gDirLightColor;
    float  _pad0;

    float3 gAmbientColor;
    float  _pad1;
};
```

---

## 将来の拡張

| ライトタイプ | 追加 Step |
|------------|---------|
| Directional Light | Step 2〜3 (現在) |
| Point Light | Step 5 以降 |
| Spot Light | Step 5 以降 |
| Shadow Map | Step 6 (DX12 移行時) |

Point Light 以降は `LightConstants` を配列に拡張する。上位レイヤーは `LightSystem` を通じて設定するため変更の影響を局所化できる。

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Renderer/
│       └── Light.hpp
└── src/
    └── Renderer/
        └── LightSystem.cpp
```
