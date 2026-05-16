# Renderer / RenderQueue

描画順序の制御。`DrawCall::layer` で描画レイヤーを指定し、`IRenderer` が内部でソートして発行する。

---

## RenderLayer

```cpp
namespace fbzz::renderer {

enum class RenderLayer : uint32_t {
    OPAQUE      = 0,  // 不透明オブジェクト (ソートなし、前から後へ)
    TRANSPARENT = 1,  // 半透明オブジェクト (デプスソートあり、後から前へ)
    OVERLAY     = 2,  // デバッグ描画・UI  (デプステストなし、最後に描画)
};

} // namespace fbzz::renderer
```

---

## 描画順序

```
BeginFrame()
│
├─ [OPAQUE]       不透明オブジェクト
│   └─ Submit 順に描画 (ソートなし)
│
├─ [TRANSPARENT]  半透明オブジェクト
│   └─ カメラからの距離で降順ソートして描画 (後→前)
│
└─ [OVERLAY]      デバッグ描画・UI
    └─ Submit 順に描画 (DEPTH_OFF PSO を使用)
│
EndFrame() → Present
```

---

## DrawCall との関係

```cpp
// 不透明メッシュ
DrawCall opaqueCall;
opaqueCall.pipelineState = psoOpaque;       // DEPTH_ON
opaqueCall.layer         = RenderLayer::OPAQUE;

// 半透明メッシュ
DrawCall transparentCall;
transparentCall.pipelineState = psoTransparent;  // DEPTH_READ
transparentCall.layer         = RenderLayer::TRANSPARENT;

// デバッグライン (DebugDraw::Flush() が内部で生成)
DrawCall debugCall;
debugCall.pipelineState = psoDebug;         // DEPTH_OFF
debugCall.layer         = RenderLayer::OVERLAY;
```

---

## IRenderer 実装方針

`Submit()` は DrawCall をレイヤーごとのキューに追加し、`EndFrame()` 内でソート・発行する。

```
Submit(call)  →  m_queues[call.layer].push_back(call)

EndFrame()
├─ flush OPAQUE      (no sort)
├─ flush TRANSPARENT (sort back-to-front by camera distance)
├─ flush OVERLAY     (no sort)
└─ Present
```

Step 1〜3 の段階では OPAQUE のみ使用し、TRANSPARENT / OVERLAY のソートロジックはスタブで可。

---

## ファイル構成

```
engine/
└── include/engine/
    └── Renderer/
        └── RenderLayer.hpp   (enum のみ。ヘッダオンリー)
```
