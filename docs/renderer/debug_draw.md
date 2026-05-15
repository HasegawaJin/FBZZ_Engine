# Renderer / Debug Draw

物理コライダー・ボーン・グリッドなどをワイヤーフレームで可視化するデバッグ描画機能。

---

## 設計方針

`IRenderer` にデバッグ用の `DrawLine` / `DrawSphere` / `DrawAABB` を仮想メソッドとして持たせない。
代わりに `DebugDraw` が自身の GPU リソース (頂点バッファ・シェーダー・PSO・定数バッファ) を
`IRenderer::CreateXxx()` で確保し、`Flush()` 時に `IRenderer::Submit()` を呼ぶ。

この設計の利点:
- IRenderer の実装が DrawCall の自己完結原則を全メソッドで守れる
- DX12 移行時も DebugDraw 側の変更は不要

---

## DebugDraw

```cpp
namespace fbzz::renderer {

class DebugDraw {
public:
    // アプリ起動時に一度だけ呼ぶ。GPU リソースを確保する
    static void Init(IRenderer* renderer);

    static void Line  (const math::Vector3& from,   const math::Vector3& to,
                       const math::Vector4& color = math::Vector4::WHITE);
    static void Sphere(const math::Vector3& center, float radius,
                       const math::Vector4& color = math::Vector4::GREEN);
    static void AABB  (const math::Vector3& min,    const math::Vector3& max,
                       const math::Vector4& color = math::Vector4::RED);
    static void Cross (const math::Vector3& pos, float size = 0.1f,
                       const math::Vector4& color = math::Vector4::WHITE);
    static void Grid  (float cellSize = 1.0f, int halfCount = 10);

    // フレーム末尾に呼ぶ。蓄積した頂点を GPU に転送して Submit() する
    static void Flush();

private:
    static IRenderer*                      s_renderer;
    static std::shared_ptr<IBuffer>        s_vertexBuffer;   // DYNAMIC、毎フレーム Update()
    static std::shared_ptr<IShader>        s_shader;
    static std::shared_ptr<IPipelineState> s_pso;
    static std::shared_ptr<IConstantBuffer> s_cameraCB;

    struct LineVertex {
        math::Vector3 position;
        math::Vector4 color;
    };
    static std::vector<LineVertex> s_vertices;  // CPU 側蓄積バッファ
};

} // namespace fbzz::renderer
```

---

## Flush() の処理フロー

```
DebugDraw::Flush()
│
├─ s_vertices が空なら return
├─ s_vertexBuffer->Update(s_vertices.data(), ...)   CPU→GPU 転送
├─ DrawCall を組み立て
│   ├─ vertexBuffer    = s_vertexBuffer
│   ├─ indexBuffer     = nullptr  (非インデックス描画)
│   ├─ shader          = s_shader
│   ├─ pipelineState   = s_pso   (WIREFRAME / OPAQUE / DEPTH_OFF)
│   ├─ constantBuffers[0] = s_cameraCB
│   ├─ vertexCount     = s_vertices.size()
│   └─ layer           = RenderLayer::OVERLAY
├─ s_renderer->Submit(call)
└─ s_vertices.clear()
```

---

## 使用例

```cpp
// 毎フレーム: コライダーのデバッグ表示
DebugDraw::AABB(aabb.min, aabb.max, math::Vector4::RED);
DebugDraw::Sphere(body->GetPosition(), 0.5f, math::Vector4::GREEN);

// 物体の速度ベクトル
DebugDraw::Line(pos, pos + vel * 0.1f, math::Vector4::BLUE);

// フレーム末尾 (Application::Run ループ内)
DebugDraw::Flush();
```

---

## HLSL (DebugLine.hlsl)

```hlsl
cbuffer CameraConstants : register(b0) {
    float4x4 viewProjection;
    float3   cameraPos;
    float    _pad;
};

struct VSInput {
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct PSInput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color    = input.color;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return input.color;
}
```

トポロジは `D3D11_PRIMITIVE_TOPOLOGY_LINELIST`。
スフィア・AABB は CPU 側で複数ラインに分解して `s_vertices` に積む。

---

## ファイル構成

```
engine/
├── include/engine/
│   └── Renderer/
│       └── DebugDraw.hpp
├── src/
│   └── Renderer/
│       └── DebugDraw.cpp
assets/
└── shaders/
    └── DebugLine.hlsl
```

---

## 参考ドキュメント

- [D3D11_PRIMITIVE_TOPOLOGY](https://learn.microsoft.com/ja-jp/windows/win32/api/d3dcommon/ne-d3dcommon-d3d_primitive_topology) — LINELIST / TRIANGLELIST 等のトポロジ定数
- [IASetPrimitiveTopology](https://learn.microsoft.com/ja-jp/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-iasetprimitivetopology) — トポロジのバインド
