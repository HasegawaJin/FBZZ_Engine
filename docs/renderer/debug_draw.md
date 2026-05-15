# Renderer / Debug Draw

物理コライダー・ボーン・グリッドなどをワイヤーフレームで可視化するデバッグ描画機能。

---

## 概要

`IRenderer` のデバッグ描画メソッドを使う。プロダクションビルドでは `#ifdef FBZZ_DEBUG` で除去する。

```cpp
// IRenderer のデバッグ描画インターフェース
virtual void DrawLine(const math::Vector3& from, const math::Vector3& to,
                      const math::Vector4& color) = 0;
virtual void DrawSphere(const math::Vector3& center, float radius,
                         const math::Vector4& color) = 0;
virtual void DrawAABB(const math::Vector3& min, const math::Vector3& max,
                      const math::Vector4& color) = 0;
```

---

## DebugDraw ヘルパー

`IRenderer` を毎回渡すのが煩雑なため、フレーム中にコマンドをキューに積み、
フレーム末尾でまとめて描画するヘルパーを用意する。

```cpp
namespace fbzz::renderer {

class DebugDraw {
public:
    static void Init(IRenderer* renderer);

    static void Line(const math::Vector3& from, const math::Vector3& to,
                     const math::Vector4& color = math::Vector4::WHITE);
    static void Sphere(const math::Vector3& center, float radius,
                       const math::Vector4& color = math::Vector4::GREEN);
    static void AABB(const math::Vector3& min, const math::Vector3& max,
                     const math::Vector4& color = math::Vector4::RED);
    static void Cross(const math::Vector3& pos, float size = 0.1f,
                      const math::Vector4& color = math::Vector4::WHITE);
    static void Grid(float cellSize = 1.0f, int halfCount = 10);

    // フレーム末尾に呼ぶ。キューをフラッシュして描画
    static void Flush();

private:
    static IRenderer* s_renderer;
};

} // namespace fbzz::renderer
```

---

## 使用例

```cpp
// コライダーのデバッグ表示
DebugDraw::AABB(aabb.min, aabb.max, math::Vector4::RED);
DebugDraw::Sphere(body->GetPosition(), 0.5f, math::Vector4::GREEN);

// 物体の速度ベクトル
Vector3 vel = body->GetVelocity();
DebugDraw::Line(pos, pos + vel * 0.1f, math::Vector4::BLUE);

// フレーム末尾
DebugDraw::Flush();
```

---

## DX11 実装方針

ラインは `DebugLine.hlsl` に専用シェーダーを用意し、
`D3D11_PRIMITIVE_TOPOLOGY_LINELIST` で描画する。
スフィア・AABB は複数のラインに分解して描画する。

```hlsl
// assets/shaders/DebugLine.hlsl
cbuffer DebugConstants : register(b0) {
    float4x4 viewProjection;
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

- [D3D11_PRIMITIVE_TOPOLOGY](https://learn.microsoft.com/en-us/windows/win32/api/d3dcommon/ne-d3dcommon-d3d_primitive_topology) — LINELIST / TRIANGLELIST 等のトポロジ定数
- [IASetPrimitiveTopology](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-iasetprimitivetopology) — トポロジのバインド
